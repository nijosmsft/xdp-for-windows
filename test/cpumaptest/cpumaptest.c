//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// CPUMAP control-plane tests.
//
// These compile src/xdp/cpumap.c unmodified into a user-mode executable (see
// precomp.h) and exercise the control plane directly. The cases were chosen from
// review findings on this increment: each of the first three is a defect that
// shipped in the first implementation and that a test would have caught.
//
//   ZeroSettingsInherit          zero ring depth / drain batch must INHERIT the
//                                map's established value, not be rejected
//   InvalidCpuIndex              CPU validation must run on EVERY update, not
//                                only when a target is created
//   DpcTargetingFailure          a failed KeSetTargetProcessorDpcEx must fail
//                                the update and unwind, not publish a target
//                                whose DPC points nowhere
//   SharedTargetAccounting       two keys naming one CPU: retire work accounting
//                                must balance, which it does not if it is
//                                incremented per value and decremented per target
//   SweepRearm                   a release arriving while the sweep runs must
//                                re-arm and be swept on the next pass
//   CoalescedSweep               two maps in one work-queue dispatch, with the
//                                worker required to cache Entry->Next
//   AllocationFailure            allocation failure must surface as a resource
//                                error and must leak nothing
//   CapAccounting                charges must be released exactly, on both the
//                                success and the failure path
//   QuiesceEmpty                 quiesce over live maps must terminate, touch no
//                                entry, and leave targets usable
//   HelperTargetRundown          helper-time target rundown acquire/release must
//                                be balanced on success and take no reference on
//                                acquire failure
//   CommitInvalidMetadata        stale or aliased frame metadata must not be
//                                treated as owning CPUMAP references
//
// Every case asserts the live allocation count returns to its starting value, so
// any unwind path that forgets a free fails here rather than in a driver.
//

#include "precomp.h"

static UINT32 XdpCpuMapTestFailures;
static UINT32 XdpCpuMapTestRun;
static const CHAR *XdpCpuMapTestCurrent = "<none>";

LONG XdpCpuMapTestLiveAllocations;
LONG XdpCpuMapTestFailAllocationsAfter = -1;
ULONG XdpCpuMapTestCurrentProcessorIndex;
BOOLEAN XdpCpuMapTestFailDpcTargeting;
DRIVER_OBJECT *XdpDriverObject;

typedef struct DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) _XDPCPUMAP_TEST_EPOCH_ALLOCATION {
    struct _XDPCPUMAP_TEST_EPOCH_ALLOCATION *Next;
    SIZE_T Size;
    ULONG Magic;
} XDPCPUMAP_TEST_EPOCH_ALLOCATION;

#define XDPCPUMAP_TEST_EPOCH_ALLOCATION_MAGIC 'eCdX'

C_ASSERT(sizeof(XDPCPUMAP_TEST_EPOCH_ALLOCATION) % MEMORY_ALLOCATION_ALIGNMENT == 0);

static XDPCPUMAP_TEST_EPOCH_ALLOCATION *XdpCpuMapTestDeferredEpochFrees;
static LONG XdpCpuMapTestDeferredEpochFreeCount;

VOID
XdpCpuMapTestAssert(
    _In_ BOOLEAN Condition,
    _In_z_ const CHAR *Expression,
    _In_z_ const CHAR *File,
    _In_ int Line
    )
{
    if (!Condition) {
        printf(
            "FAIL [%s] %s\n  at %s:%d\n",
            XdpCpuMapTestCurrent, Expression, File, Line);
        XdpCpuMapTestFailures++;
    }
}

VOID
XdpCpuMapTestResetAllocator(
    VOID
    )
{
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDeferredEpochFrees == NULL);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDeferredEpochFreeCount == 0);

    XdpCpuMapTestLiveAllocations = 0;
    XdpCpuMapTestFailAllocationsAfter = -1;
    XdpCpuMapTestCurrentProcessorIndex = 0;
    XdpCpuMapTestFailDpcTargeting = FALSE;
}

VOID *
XdpCpuMapTestAllocate(
    _In_ SIZE_T NumberOfBytes
    )
{
    VOID *Memory;

    if (XdpCpuMapTestFailAllocationsAfter == 0) {
        return NULL;
    }

    if (XdpCpuMapTestFailAllocationsAfter > 0) {
        XdpCpuMapTestFailAllocationsAfter--;
    }

    Memory = calloc(1, NumberOfBytes);
    if (Memory != NULL) {
        XdpCpuMapTestLiveAllocations++;
    }

    return Memory;
}

VOID
XdpCpuMapTestFree(
    _In_opt_ VOID *P
    )
{
    if (P == NULL) {
        return;
    }

    XdpCpuMapTestLiveAllocations--;
    free(P);
}

//
// Epoch stubs. These ENFORCE the contract from ebpf_extension.h: every epoch
// memory operation must occur inside an epoch-protected region. A provider
// running one outside a region is the defect this enforcement exists to catch,
// and it is otherwise undetectable in test because the operation still succeeds.
//

LONG XdpCpuMapTestEpochDepth;
ULONG XdpCpuMapTestRundownAcquireCalls;
ULONG XdpCpuMapTestRundownReleaseCalls;
ULONG XdpCpuMapTestExpectAssert;
ULONG XdpCpuMapTestAssertsObserved;
LONG XdpCpuMapTestEpochOperations;

static
void
XdpCpuMapTestEpochEnter(
    void *EpochState
    )
{
    //
    // The runtime fills the caller's state block. Scribbling a marker is enough
    // to catch a caller that passes an undersized or aliased block, and it
    // asserts the harness agrees with the SDK on the size.
    //
    C_ASSERT(sizeof(epoch_state_t) == 4 * sizeof(uint64_t));
    memset(EpochState, 0xEE, sizeof(epoch_state_t));

    XdpCpuMapTestEpochDepth++;
}

static
void
XdpCpuMapTestEpochExit(
    void *EpochState
    )
{
    UNREFERENCED_PARAMETER(EpochState);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochDepth > 0);
    if (XdpCpuMapTestEpochDepth > 0) {
        XdpCpuMapTestEpochDepth--;
    }
}

static
void *
XdpCpuMapTestEpochAllocate(
    size_t Size,
    uint32_t Tag
    )
{
    XDPCPUMAP_TEST_EPOCH_ALLOCATION *Allocation;

    UNREFERENCED_PARAMETER(Tag);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochDepth > 0);
    XdpCpuMapTestEpochOperations++;

    Allocation = XdpCpuMapTestAllocate(sizeof(*Allocation) + Size);
    if (Allocation == NULL) {
        return NULL;
    }

    Allocation->Next = NULL;
    Allocation->Size = Size;
    Allocation->Magic = XDPCPUMAP_TEST_EPOCH_ALLOCATION_MAGIC;

    return Allocation + 1;
}

static
void
XdpCpuMapTestEpochFree(
    void *Memory
    )
{
    XDPCPUMAP_TEST_EPOCH_ALLOCATION *Allocation;

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochDepth > 0);
    XdpCpuMapTestEpochOperations++;

    if (Memory == NULL) {
        return;
    }

    Allocation = ((XDPCPUMAP_TEST_EPOCH_ALLOCATION *)Memory) - 1;
    XDPCPUMAP_TEST_ASSERT(Allocation->Magic == XDPCPUMAP_TEST_EPOCH_ALLOCATION_MAGIC);
    Allocation->Next = XdpCpuMapTestDeferredEpochFrees;
    XdpCpuMapTestDeferredEpochFrees = Allocation;
    XdpCpuMapTestDeferredEpochFreeCount++;
}

static
VOID
XdpCpuMapTestDrainEpochFrees(
    VOID
    )
{
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochDepth == 0);

    while (XdpCpuMapTestDeferredEpochFrees != NULL) {
        XDPCPUMAP_TEST_EPOCH_ALLOCATION *Allocation = XdpCpuMapTestDeferredEpochFrees;

        XdpCpuMapTestDeferredEpochFrees = Allocation->Next;
        XDPCPUMAP_TEST_ASSERT(Allocation->Magic == XDPCPUMAP_TEST_EPOCH_ALLOCATION_MAGIC);
        Allocation->Magic = 0;
        XdpCpuMapTestDeferredEpochFreeCount--;
        XdpCpuMapTestFree(Allocation);
    }

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDeferredEpochFreeCount == 0);
}

ebpf_base_map_client_dispatch_table_t XdpCpuMapTestClientDispatch = {
    NULL,                           // header
    NULL,                           // find_element_function
    XdpCpuMapTestEpochEnter,
    XdpCpuMapTestEpochExit,
    XdpCpuMapTestEpochAllocate,
    NULL,                           // epoch_allocate_cache_aligned_with_tag
    XdpCpuMapTestEpochFree,
    NULL,                           // epoch_free_cache_aligned
};

static const VOID *XdpCpuMapTestFindMap;
static const VOID *XdpCpuMapTestFindKey;
static XDP_CPUMAP_PROVIDER_VALUE *XdpCpuMapTestFindValue;
static ebpf_result_t XdpCpuMapTestFindResult;
static UINT32 XdpCpuMapTestFindCallCount;

static
ebpf_result_t
XdpCpuMapTestFindElement(
    _In_ const VOID *Map,
    _In_ const VOID *Key,
    _Outptr_result_maybenull_ uint8_t **Value
    )
{
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochDepth > 0);

    XdpCpuMapTestFindCallCount++;
    XdpCpuMapTestFindMap = Map;
    XdpCpuMapTestFindKey = Key;

    if (XdpCpuMapTestFindResult != EBPF_SUCCESS) {
        *Value = NULL;
        return XdpCpuMapTestFindResult;
    }

    *Value = (uint8_t *)XdpCpuMapTestFindValue;
    return EBPF_SUCCESS;
}

static
VOID
XdpCpuMapTestResetFindElement(
    VOID
    )
{
    XdpCpuMapTestFindMap = NULL;
    XdpCpuMapTestFindKey = NULL;
    XdpCpuMapTestFindValue = NULL;
    XdpCpuMapTestFindResult = EBPF_SUCCESS;
    XdpCpuMapTestFindCallCount = 0;
    XdpCpuMapTestClientDispatch.find_element_function = XdpCpuMapTestFindElement;
}

VOID
XdpCpuMapTestEnterCallbackEpoch(
    _Out_ epoch_state_t *EpochState
    )
{
    XdpCpuMapTestEpochEnter(EpochState);
}

VOID
XdpCpuMapTestExitCallbackEpoch(
    _Inout_ epoch_state_t *EpochState
    )
{
    XdpCpuMapTestEpochExit(EpochState);
}

//
// Provider-callback wrappers, keyed on DISPATCH PATH rather than on callback
// category. Category is the wrong discriminator: not every provider callback is
// epoch-protected.
//
//   Protocol dispatch  -- ebpf_core.c enters an epoch around every protocol
//                         handler, so callbacks reached from an IOCTL-driven map
//                         operation inherit that region.
//
//   Epoch work item    -- object destruction is registered as an epoch work item
//                         (ebpf_object.c), and _ebpf_epoch_work_item_callback
//                         invokes it with NO ebpf_epoch_enter. It runs after the
//                         epoch has retired, which is why a provider may block
//                         there and equally why there is no region to inherit.
//
// A wrapper that supplies an epoch the runtime would not does more harm than no
// assertion at all: it licenses exactly the pattern the assertion exists to
// catch. So each wrapper below reproduces its real path, and the two callbacks
// that have BOTH paths get one wrapper each.
//

//
// preprocess_map_create: protocol dispatch only. Protected.
//
static
NTSTATUS
XdpCpuMapTestCreateMap(
    _In_ UINT32 MaxEntries,
    _Outptr_ XDP_CPUMAP **CpuMap
    )
{
    epoch_state_t EpochState;
    NTSTATUS Status;

    XdpCpuMapTestEnterCallbackEpoch(&EpochState);
    Status = XdpCpuMapCreate(&XdpCpuMapTestClientDispatch, MaxEntries, CpuMap);
    XdpCpuMapTestExitCallbackEpoch(&EpochState);

    return Status;
}

//
// preprocess_map_update_element: protocol dispatch only. Protected.
//
static
NTSTATUS
XdpCpuMapTestResolve(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ const XDP_CPUMAP_ENTRY_V1 *Entry,
    _Out_ XDP_CPUMAP_PROVIDER_VALUE *Value
    )
{
    epoch_state_t EpochState;
    NTSTATUS Status;

    XdpCpuMapTestEnterCallbackEpoch(&EpochState);
    Status = XdpCpuMapResolveTarget(CpuMap, Entry, Value);
    XdpCpuMapTestExitCallbackEpoch(&EpochState);

    return Status;
}

//
// postprocess_map_delete_element, protocol-dispatch path: an element delete or
// replace driven by a map operation. Protected.
//
static
VOID
XdpCpuMapTestRelease(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ const XDP_CPUMAP_PROVIDER_VALUE *Value
    )
{
    epoch_state_t EpochState;

    XdpCpuMapTestEnterCallbackEpoch(&EpochState);
    XdpCpuMapQueueValueRelease(CpuMap, Value);
    XdpCpuMapTestExitCallbackEpoch(&EpochState);
}

//
// postprocess_map_delete_element, TEARDOWN path: _clean_up_custom_hash_map walks
// the surviving elements while the map is being destroyed, and that runs from
// the epoch work item. NOT protected.
//
// Using this variant is what proves XdpCpuMapQueueValueRelease performs no epoch
// memory operation -- a property the design requires anyway, since the callback
// may arrive at DISPATCH_LEVEL beneath a base-map lock.
//
static
VOID
XdpCpuMapTestReleaseAtTeardown(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ const XDP_CPUMAP_PROVIDER_VALUE *Value
    )
{
    XdpCpuMapQueueValueRelease(CpuMap, Value);
}

static
ebpf_result_t
XdpCpuMapTestFindElementInEpoch(
    _In_ const VOID *Map,
    _In_ XDP_CPUMAP *CpuMap,
    _In_ const VOID *Key,
    _Outptr_result_maybenull_ XDP_CPUMAP_PROVIDER_VALUE **Value
    )
{
    epoch_state_t EpochState;
    ebpf_result_t Result;

    XdpCpuMapTestEnterCallbackEpoch(&EpochState);
    Result = XdpCpuMapFindElementFromBaseMap(Map, CpuMap, Key, Value);
    XdpCpuMapTestExitCallbackEpoch(&EpochState);

    return Result;
}

static
intptr_t
XdpCpuMapTestRedirectInEpoch(
    _In_ const VOID *Map,
    _In_ UINT64 Key,
    _In_ intptr_t FallbackAction,
    _In_ BOOLEAN IsProgTestRun,
    _In_ XDP_INTERFACE_MODE InterfaceMode,
    _Inout_ XDP_CPUMAP *CpuMap,
    _Inout_ XDP_CPUMAP_REDIRECT_CONTEXT *Redirect
    )
{
    epoch_state_t EpochState;
    intptr_t Result;

    XdpCpuMapTestEnterCallbackEpoch(&EpochState);
    Result =
        XdpCpuMapRedirectMap(
            Map, Key, FallbackAction, IsProgTestRun, InterfaceMode, CpuMap, Redirect);
    XdpCpuMapTestExitCallbackEpoch(&EpochState);

    return Result;
}

static
XDP_CPUMAP_HELPER_STATS
XdpCpuMapTestQueryHelperStats(
    _In_ const XDP_CPUMAP *CpuMap
    )
{
    XDP_CPUMAP_HELPER_STATS Stats;

    XdpCpuMapQueryHelperStats(CpuMap, &Stats);
    return Stats;
}

static
SIZE_T
XdpCpuMapTestGlobalNonPagedBytes(
    VOID
    )
{
    SIZE_T NonPagedBytes;

    XdpCpuMapQueryGlobalStats(NULL, &NonPagedBytes);
    return NonPagedBytes;
}

static
SIZE_T
XdpCpuMapTestTargetChargeBytes(
    _In_ UINT32 RingDepth
    )
{
    SIZE_T RingBytes =
        sizeof(XDP_CPUMAP_RING) + ((SIZE_T)RingDepth * sizeof(XDP_CPUMAP_ENTRY));

    return
        (RingBytes + sizeof(KDPC) + MEMORY_ALLOCATION_ALIGNMENT - 1) &
        ~((SIZE_T)MEMORY_ALLOCATION_ALIGNMENT - 1);
}

//
// postprocess_map_delete: reached from the epoch work item on the destruction
// path. NOT protected -- so XdpCpuMapDestroy must establish its own region for
// the final map free, and this wrapper must not hand it one.
//
static
VOID
XdpCpuMapTestDestroyMap(
    _Inout_ _Post_invalid_ XDP_CPUMAP *CpuMap
    )
{
    XdpCpuMapDestroy(CpuMap);
}

//
// Helpers.
//

static
XDP_CPUMAP_ENTRY_V1
XdpCpuMapTestEntry(
    _In_ UINT32 Cpu,
    _In_ UINT32 RingDepth,
    _In_ UINT32 DrainBatchSize
    )
{
    XDP_CPUMAP_ENTRY_V1 Entry;

    RtlZeroMemory(&Entry, sizeof(Entry));
    Entry.Size = XDP_CPUMAP_ENTRY_SIZE_V1;
    Entry.Version = XDP_CPUMAP_ENTRY_VERSION_1;
    Entry.TargetCpu = Cpu;
    Entry.RingDepth = RingDepth;
    Entry.DrainBatchSize = DrainBatchSize;

    return Entry;
}

//
// Drives the sweep to completion. Bounded so a sweep that fails to make progress
// fails the test instead of hanging.
//
static
UINT32
XdpCpuMapTestDrainSweeps(
    VOID
    )
{
    UINT32 Dispatches = 0;

    while (XdpCpuMapTestPendingWorkQueueEntries() > 0) {
        XdpCpuMapTestRunWorkQueue();

        if (++Dispatches > 64) {
            XDPCPUMAP_TEST_ASSERT(FALSE);
            break;
        }
    }

    return Dispatches;
}

#define XDPCPUMAP_TEST_BEGIN(name) \
    XdpCpuMapTestCurrent = (name); \
    XdpCpuMapTestRun++; \
    XdpCpuMapTestResetAllocator()

//
// Tests.
//

//
// Zero means "inherit the map's established setting". Section 5.3 defines it as
// the default-inheriting value and loaders write zero for every entry after the
// first, so rejecting zero against an established non-default breaks the
// documented contract. The original implementation converted zero to the DEFAULT
// before comparing, which rejected exactly that case.
//
static
VOID
XdpCpuMapTestZeroSettingsInherit(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value0;
    XDP_CPUMAP_PROVIDER_VALUE Value1;
    XDP_CPUMAP_PROVIDER_VALUE Value2;
    XDP_CPUMAP_ENTRY_V1 Entry;
    const UINT32 NonDefaultDepth = XDP_CPUMAP_RING_DEPTH_DEFAULT * 2;
    const UINT32 NonDefaultBatch = XDP_CPUMAP_DRAIN_BATCH_DEFAULT / 2;
    NTSTATUS Status;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("ZeroSettingsInherit");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    Status = XdpCpuMapTestCreateMap(16, &CpuMap);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));

    //
    // First entry establishes non-default map settings.
    //
    Entry = XdpCpuMapTestEntry(0, NonDefaultDepth, NonDefaultBatch);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));
    XDPCPUMAP_TEST_ASSERT(CpuMap->EffectiveRingDepth == NonDefaultDepth);
    XDPCPUMAP_TEST_ASSERT(CpuMap->EffectiveDrainBatchSize == NonDefaultBatch);

    //
    // A later entry passing zero must INHERIT, not be rejected against the
    // default.
    //
    Entry = XdpCpuMapTestEntry(1, 0, 0);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value1);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));
    XDPCPUMAP_TEST_ASSERT(Value1.Target != NULL);
    if (Value1.Target != NULL) {
        XDPCPUMAP_TEST_ASSERT(Value1.Target->Ring->Capacity == NonDefaultDepth);
    }

    //
    // A NONZERO disagreement is still an error.
    //
    Entry = XdpCpuMapTestEntry(2, NonDefaultDepth * 2, 0);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value2);
    XDPCPUMAP_TEST_ASSERT(Status == STATUS_INVALID_PARAMETER);

    Entry = XdpCpuMapTestEntry(2, 0, NonDefaultBatch + 1);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value2);
    XDPCPUMAP_TEST_ASSERT(Status == STATUS_INVALID_PARAMETER);

    //
    // A nonzero value that AGREES is accepted.
    //
    Entry = XdpCpuMapTestEntry(2, NonDefaultDepth, NonDefaultBatch);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value2);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));

    //
    // An out-of-range NONZERO value is rejected on its own merits.
    //
    Entry = XdpCpuMapTestEntry(3, 3, 0);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value2);
    XDPCPUMAP_TEST_ASSERT(Status == STATUS_INVALID_PARAMETER);

    XdpCpuMapTestRelease(CpuMap, &Value0);
    XdpCpuMapTestRelease(CpuMap, &Value1);
    Entry = XdpCpuMapTestEntry(2, NonDefaultDepth, NonDefaultBatch);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value2);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));
    XdpCpuMapTestRelease(CpuMap, &Value2);
    XdpCpuMapTestRelease(CpuMap, &Value2);

    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// CPU validation must run on EVERY update. The original implementation validated
// the index only on the path that CREATED a target, so an invalid index was
// accepted whenever some other key had already named a valid one -- and the
// second check, against the target table size, does not catch an index that is
// in range for the table but names no processor.
//
static
VOID
XdpCpuMapTestInvalidCpuIndex(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_PROVIDER_VALUE Shared;
    XDP_CPUMAP_PROVIDER_VALUE Reject;
    XDP_CPUMAP_ENTRY_V1 Entry;
    NTSTATUS Status;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("InvalidCpuIndex");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    Status = XdpCpuMapTestCreateMap(16, &CpuMap);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));

    //
    // Rejected on an empty map. This index is INSIDE the target table -- the
    // table is sized by the maximum processor count -- so the bounds check does
    // not catch it and only KeGetProcessorNumberFromIndex can.
    //
    Entry = XdpCpuMapTestEntry(XDP_CPUMAP_TEST_PROCESSOR_COUNT, 0, 0);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Reject);
    XDPCPUMAP_TEST_ASSERT(Status == STATUS_INVALID_PARAMETER);
    XDPCPUMAP_TEST_ASSERT(Reject.Target == NULL);

    //
    // An index beyond the table is rejected by the bounds check.
    //
    Entry = XdpCpuMapTestEntry(XDP_CPUMAP_TEST_MAX_PROCESSOR_COUNT, 0, 0);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Reject);
    XDPCPUMAP_TEST_ASSERT(Status == STATUS_INVALID_PARAMETER);

    //
    // Establish a valid target, then re-offer the nonexistent-processor index.
    // This is the case the original code let through: validation ran only when a
    // target was being created, so once ANY key named a valid CPU, an index in
    // the hot-add gap was accepted.
    //
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));

    Entry = XdpCpuMapTestEntry(XDP_CPUMAP_TEST_PROCESSOR_COUNT, 0, 0);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Reject);
    XDPCPUMAP_TEST_ASSERT(Status == STATUS_INVALID_PARAMETER);
    XDPCPUMAP_TEST_ASSERT(Reject.Target == NULL);
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 1);

    //
    // And re-offering the SAME valid index still resolves to the existing
    // target, so validation did not break the reuse path.
    //
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Shared);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));
    XDPCPUMAP_TEST_ASSERT(Shared.Target == Value.Target);
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 1);

    //
    // Malformed headers are rejected regardless of CPU.
    //
    // N.B. these resolve into a scratch value, not into one holding a live
    // reference: a failed resolve zeroes its out-value, so reusing a variable
    // that already holds a reference would silently discard it. That is a real
    // hazard for callers, and the zeroing is what makes it detectable.
    //
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    Entry.Version = XDP_CPUMAP_ENTRY_VERSION_1 + 1;
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestResolve(CpuMap, &Entry, &Reject) == STATUS_INVALID_PARAMETER);
    XDPCPUMAP_TEST_ASSERT(Reject.Target == NULL);

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    Entry.Flags = 1;
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestResolve(CpuMap, &Entry, &Reject) == STATUS_INVALID_PARAMETER);
    XDPCPUMAP_TEST_ASSERT(Reject.Target == NULL);

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    Entry.Size = XDP_CPUMAP_ENTRY_SIZE_V1 + 1;
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestResolve(CpuMap, &Entry, &Reject) == STATUS_INVALID_PARAMETER);

    //
    // Releasing a zeroed value must be a safe no-op, since that is exactly what
    // the runtime hands back for an operation that never committed.
    //
    XdpCpuMapTestRelease(CpuMap, &Reject);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RetireWorkCount == 0);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestRelease(CpuMap, &Shared);

    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// KeSetTargetProcessorDpcEx is _Must_inspect_result_. Ignoring it publishes a
// target whose DPC has no affinity, which in the driver means redirect silently
// runs on the wrong CPU -- a correctness failure that looks like a performance
// mystery. The update must fail and unwind completely.
//
static
VOID
XdpCpuMapTestDpcTargetingFailure(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    NTSTATUS Status;
    LONG Baseline;
    LONG AfterCreate;

    XDPCPUMAP_TEST_BEGIN("DpcTargetingFailure");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    Status = XdpCpuMapTestCreateMap(16, &CpuMap);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));
    AfterCreate = XdpCpuMapTestLiveAllocations;

    XdpCpuMapTestFailDpcTargeting = TRUE;

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value);
    XDPCPUMAP_TEST_ASSERT(!NT_SUCCESS(Status));
    XDPCPUMAP_TEST_ASSERT(Value.Target == NULL);

    //
    // Nothing published and the charged ring/DPC bytes were released. The
    // target shell was allocated before DPC targeting failed, but it is
    // epoch-freed and therefore remains queued until the explicit epoch drain.
    //
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->ChargedRingEntries == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->ChargedNonPagedBytes == CpuMap->HelperStatsBytes);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDeferredEpochFreeCount == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == AfterCreate + 1);
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == AfterCreate);

    //
    // The map is still usable once targeting works again.
    //
    XdpCpuMapTestFailDpcTargeting = FALSE;
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));
    XDPCPUMAP_TEST_ASSERT(Value.Target != NULL);
    if (Value.Target != NULL) {
        XDPCPUMAP_TEST_ASSERT(Value.Target->Dpc->Targeted);
        XDPCPUMAP_TEST_ASSERT(Value.Target->Dpc->Target.Number == 0);
    }

    XdpCpuMapTestRelease(CpuMap, &Value);

    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// Shared targets: two keys naming one CPU produce TWO value references but at
// most ONE retire. Retire work accounting that increments per released value and
// decrements per retired target therefore never returns to zero, which trips the
// destroy-time assertions. The accounting must balance against what each sweep
// pass actually consumed.
//
static
VOID
XdpCpuMapTestSharedTargetAccounting(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE ValueA;
    XDP_CPUMAP_PROVIDER_VALUE ValueB;
    XDP_CPUMAP_PROVIDER_VALUE ValueC;
    XDP_CPUMAP_ENTRY_V1 Entry;
    NTSTATUS Status;
    LONG Baseline;
    LONG AfterCreate;
    LONG EpochOpsBefore;

    XDPCPUMAP_TEST_BEGIN("SharedTargetAccounting");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    Status = XdpCpuMapTestCreateMap(16, &CpuMap);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));
    AfterCreate = XdpCpuMapTestLiveAllocations;

    //
    // Three distinct keys, all naming CPU 3. One target, three references.
    //
    Entry = XdpCpuMapTestEntry(3, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &ValueA)));
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &ValueB)));
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &ValueC)));

    XDPCPUMAP_TEST_ASSERT(ValueA.Target == ValueB.Target);
    XDPCPUMAP_TEST_ASSERT(ValueB.Target == ValueC.Target);
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 1);
    if (ValueA.Target != NULL) {
        XDPCPUMAP_TEST_ASSERT(ValueA.Target->ValueRefCount == 3);
    }

    //
    // Releasing two of three is a REPLACEMENT pattern: the target survives and
    // the sweep must still consume both pending releases.
    //
    XdpCpuMapTestRelease(CpuMap, &ValueA);
    XdpCpuMapTestRelease(CpuMap, &ValueB);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RetireWorkCount == 2);

    XdpCpuMapTestDrainSweeps();

    XDPCPUMAP_TEST_ASSERT(CpuMap->RetireWorkCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 1);
    if (ValueC.Target != NULL) {
        XDPCPUMAP_TEST_ASSERT(ValueC.Target->ValueRefCount == 1);
        XDPCPUMAP_TEST_ASSERT(ValueC.Target->Active);
    }

    //
    // Releasing the last one retires the target and must also return the
    // accounting to zero.
    //
    XdpCpuMapTestRelease(CpuMap, &ValueC);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RetireWorkCount == 1);

    EpochOpsBefore = XdpCpuMapTestEpochOperations;

    XdpCpuMapTestDrainSweeps();

    //
    // Retiring epoch-frees the target shell, and that free runs on XDP's work
    // queue -- outside any provider callback. It is therefore legal only if the
    // sweep established its own epoch region, which the stub enforces. Asserting
    // the operation was actually observed keeps this from passing vacuously if
    // the retire path is ever restructured to free some other way.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochOperations > EpochOpsBefore);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochDepth == 0);

    XDPCPUMAP_TEST_ASSERT(CpuMap->RetireWorkCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->ChargedRingEntries == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->ChargedNonPagedBytes == CpuMap->HelperStatsBytes);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDeferredEpochFreeCount == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == AfterCreate + 1);
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == AfterCreate);

    //
    // With every target gone, map settings are re-establishable.
    //
    Entry = XdpCpuMapTestEntry(4, XDP_CPUMAP_RING_DEPTH_MIN, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &ValueA)));
    XDPCPUMAP_TEST_ASSERT(CpuMap->EffectiveRingDepth == XDP_CPUMAP_RING_DEPTH_MIN);
    XdpCpuMapTestRelease(CpuMap, &ValueA);

    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// A release that arrives while the sweep is RUNNING must re-arm the sweep rather
// than be lost, because the running pass has already snapshotted the pending
// counts it will consume.
//
static
VOID
XdpCpuMapTestSweepRearm(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value0;
    XDP_CPUMAP_PROVIDER_VALUE Value1;
    XDP_CPUMAP_ENTRY_V1 Entry;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("SweepRearm");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value0)));
    Entry = XdpCpuMapTestEntry(1, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value1)));

    //
    // Arming twice before any dispatch must queue the map ONCE. The map owns a
    // single embedded sweep entry, so a second insertion would corrupt the list.
    //
    XdpCpuMapTestRelease(CpuMap, &Value0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPendingWorkQueueEntries() == 1);
    XdpCpuMapTestRelease(CpuMap, &Value1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPendingWorkQueueEntries() == 1);

    XDPCPUMAP_TEST_ASSERT(CpuMap->RetireWorkCount == 2);

    XdpCpuMapTestDrainSweeps();

    XDPCPUMAP_TEST_ASSERT(CpuMap->RetireWorkCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->SweepState == XdpCpuMapSweepIdle);

    //
    // A fresh release after the sweep went idle must arm it again.
    //
    Entry = XdpCpuMapTestEntry(2, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value0)));
    XdpCpuMapTestRelease(CpuMap, &Value0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPendingWorkQueueEntries() == 1);

    XdpCpuMapTestDrainSweeps();
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RetireWorkCount == 0);

    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// The work queue hands the routine a LIST, not one entry, and re-arming rewrites
// Entry->Next. A worker that processes only the head, or that reads Entry->Next
// after processing, loses maps or follows a rewritten link. Two maps armed before
// a single dispatch exercises both.
//
static
VOID
XdpCpuMapTestCoalescedSweep(
    VOID
    )
{
    XDP_CPUMAP *CpuMapA;
    XDP_CPUMAP *CpuMapB;
    XDP_CPUMAP_PROVIDER_VALUE ValueA;
    XDP_CPUMAP_PROVIDER_VALUE ValueB;
    XDP_CPUMAP_ENTRY_V1 Entry;
    UINT32 Dispatched;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("CoalescedSweep");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMapA)));
    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMapB)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMapA, &Entry, &ValueA)));
    Entry = XdpCpuMapTestEntry(1, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMapB, &Entry, &ValueB)));

    //
    // Arm both, then dispatch ONCE. Both must be swept by that single call.
    //
    XdpCpuMapTestRelease(CpuMapA, &ValueA);
    XdpCpuMapTestRelease(CpuMapB, &ValueB);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPendingWorkQueueEntries() == 2);

    Dispatched = XdpCpuMapTestRunWorkQueue();
    XDPCPUMAP_TEST_ASSERT(Dispatched == 2);

    XDPCPUMAP_TEST_ASSERT(CpuMapA->TargetCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMapB->TargetCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMapA->RetireWorkCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMapB->RetireWorkCount == 0);

    //
    // Re-arm both and dispatch again, to prove a swept map returns to a state
    // where its embedded entry can be re-queued.
    //
    Entry = XdpCpuMapTestEntry(2, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMapA, &Entry, &ValueA)));
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMapB, &Entry, &ValueB)));
    XdpCpuMapTestRelease(CpuMapA, &ValueA);
    XdpCpuMapTestRelease(CpuMapB, &ValueB);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPendingWorkQueueEntries() == 2);

    Dispatched = XdpCpuMapTestRunWorkQueue();
    XDPCPUMAP_TEST_ASSERT(Dispatched == 2);
    XDPCPUMAP_TEST_ASSERT(CpuMapA->TargetCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMapB->TargetCount == 0);

    XdpCpuMapTestDestroyMap(CpuMapA);
    XdpCpuMapTestDestroyMap(CpuMapB);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// Allocation failure must be reported as a RESOURCE condition and must unwind
// completely. Reporting it as bad input tells a loader to stop retrying valid
// input; leaking on the unwind is worse.
//
static
VOID
XdpCpuMapTestAllocationFailure(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    NTSTATUS Status;
    LONG Baseline;
    LONG AfterCreate;
    SIZE_T GlobalBaseline;
    SIZE_T GlobalAfterCreate;

    XDPCPUMAP_TEST_BEGIN("AllocationFailure");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    GlobalBaseline = XdpCpuMapTestGlobalNonPagedBytes();

    //
    // Fail each allocation in map creation: the map shell, the target table,
    // and the charged helper-stats shards. The third case is the one that
    // proves a failed stats allocation releases its reservation; a leaked charge
    // would leave GlobalNonPagedBytes above the starting value.
    //
    for (LONG Allow = 0; Allow < 3; Allow++) {
        CpuMap = (XDP_CPUMAP *)(ULONG_PTR)MAXULONG_PTR;
        XdpCpuMapTestFailAllocationsAfter = Allow;
        Status = XdpCpuMapTestCreateMap(16, &CpuMap);
        XDPCPUMAP_TEST_ASSERT(!NT_SUCCESS(Status));
        XDPCPUMAP_TEST_ASSERT(CpuMap == NULL);
        XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestGlobalNonPagedBytes() == GlobalBaseline);
    }

    XdpCpuMapTestFailAllocationsAfter = -1;
    Status = XdpCpuMapTestCreateMap(16, &CpuMap);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));
    AfterCreate = XdpCpuMapTestLiveAllocations;
    GlobalAfterCreate = XdpCpuMapTestGlobalNonPagedBytes();
    XDPCPUMAP_TEST_ASSERT(GlobalAfterCreate == GlobalBaseline + CpuMap->HelperStatsBytes);

    //
    // Fail each of the three target allocations in turn: ring, DPC, shell. Every
    // one must return a resource status, publish nothing, charge nothing and
    // leak nothing.
    //
    for (LONG Allow = 0; Allow < 3; Allow++) {
        XdpCpuMapTestFailAllocationsAfter = Allow;

        Entry = XdpCpuMapTestEntry(0, 0, 0);
        Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value);

        XDPCPUMAP_TEST_ASSERT(
            Status == STATUS_NO_MEMORY || Status == STATUS_INSUFFICIENT_RESOURCES);
        XDPCPUMAP_TEST_ASSERT(Value.Target == NULL);
        XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 0);
        XDPCPUMAP_TEST_ASSERT(CpuMap->ChargedRingEntries == 0);
        XDPCPUMAP_TEST_ASSERT(CpuMap->ChargedNonPagedBytes == CpuMap->HelperStatsBytes);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == AfterCreate);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestGlobalNonPagedBytes() == GlobalAfterCreate);
    }

    XdpCpuMapTestFailAllocationsAfter = -1;

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    Status = XdpCpuMapTestResolve(CpuMap, &Entry, &Value);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(Status));
    XdpCpuMapTestRelease(CpuMap, &Value);

    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// Cap charges are taken global-then-per-map and must be released in exact
// reverse on every exit. Charging is the one place where an imbalance is
// invisible until the machine has been up long enough to exhaust the cap.
//
static
VOID
XdpCpuMapTestCapAccounting(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Values[4];
    XDP_CPUMAP_ENTRY_V1 Entry;
    UINT32 ExpectedEntries;
    SIZE_T ExpectedTargetBytes;
    SIZE_T ExpectedCharge;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("CapAccounting");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    for (UINT32 Cpu = 0; Cpu < RTL_NUMBER_OF(Values); Cpu++) {
        Entry = XdpCpuMapTestEntry(Cpu, XDP_CPUMAP_RING_DEPTH_MIN, 0);
        XDPCPUMAP_TEST_ASSERT(
            NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Values[Cpu])));
    }

    ExpectedTargetBytes =
        XdpCpuMapTestTargetChargeBytes(XDP_CPUMAP_RING_DEPTH_MIN);
    ExpectedCharge =
        CpuMap->HelperStatsBytes +
        (RTL_NUMBER_OF(Values) * ExpectedTargetBytes);
    ExpectedEntries = XDP_CPUMAP_RING_DEPTH_MIN * RTL_NUMBER_OF(Values);
    XDPCPUMAP_TEST_ASSERT(CpuMap->ChargedRingEntries == ExpectedEntries);
    XDPCPUMAP_TEST_ASSERT(CpuMap->ChargedNonPagedBytes == ExpectedCharge);
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == RTL_NUMBER_OF(Values));
    for (UINT32 Cpu = 0; Cpu < RTL_NUMBER_OF(Values); Cpu++) {
        XDPCPUMAP_TEST_ASSERT(
            Values[Cpu].Target->ChargedNonPagedBytes == ExpectedTargetBytes);
    }

    //
    // Retire half, and the charge must fall by exactly half.
    //
    XdpCpuMapTestRelease(CpuMap, &Values[0]);
    XdpCpuMapTestRelease(CpuMap, &Values[1]);
    XdpCpuMapTestDrainSweeps();

    XDPCPUMAP_TEST_ASSERT(CpuMap->ChargedRingEntries == ExpectedEntries / 2);
    XDPCPUMAP_TEST_ASSERT(
        CpuMap->ChargedNonPagedBytes ==
        CpuMap->HelperStatsBytes + (ExpectedTargetBytes * 2));
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == RTL_NUMBER_OF(Values) / 2);

    XdpCpuMapTestRelease(CpuMap, &Values[2]);
    XdpCpuMapTestRelease(CpuMap, &Values[3]);
    XdpCpuMapTestDrainSweeps();

    XDPCPUMAP_TEST_ASSERT(CpuMap->ChargedRingEntries == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->ChargedNonPagedBytes == CpuMap->HelperStatsBytes);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RetireWorkCount == 0);

    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    //
    // Global charges are released too: a second module lifetime must start from
    // a clean slate, which it cannot if the global counters drifted.
    //
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, XDP_CPUMAP_RING_DEPTH_MAX, 0);
    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Values[0])));
    XdpCpuMapTestRelease(CpuMap, &Values[0]);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// Quiesce over live maps with empty rings. This does not measure the pause bound
// -- an empty ring scans zero slots, which is precisely the limitation this
// increment surfaced -- but it does prove quiesce terminates, holds its locks in
// the documented order, touches no live target state, and leaves the map usable.
//
static
VOID
XdpCpuMapTestQuiesceEmpty(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    const UINT32 Token = 0;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("QuiesceEmpty");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    //
    // Quiesce with no maps at all must be a no-op, not a fault.
    //
    XdpCpuMapQuiesceInterface(&Token);
    XdpCpuMapQuiesceRxQueue(&Token);

    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    XdpCpuMapQuiesceInterface(&Token);
    XdpCpuMapQuiesceRxQueue(&Token);

    //
    // The target is untouched: quiesce must not retire, deactivate, or disturb
    // reference counts.
    //
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 1);
    if (Value.Target != NULL) {
        XDPCPUMAP_TEST_ASSERT(Value.Target->Active);
        XDPCPUMAP_TEST_ASSERT(Value.Target->ValueRefCount == 1);
        XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
        XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Head == Value.Target->Ring->Tail);
    }

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();

    //
    // Quiesce after every target has retired must also terminate cleanly.
    //
    XdpCpuMapQuiesceInterface(&Token);

    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    //
    // And after the map is destroyed it must be gone from the registry.
    //
    XdpCpuMapQuiesceInterface(&Token);

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// Quiesce scan cost.
//
// This increment was scoped believing the pause bound was measurable with empty
// rings. It is not: the scan walks head to a SNAPSHOTTED TAIL, so an empty ring
// costs nothing and the per-entry term never appears.
//
// It is measurable here, though, and with NO production change. The harness
// compiles cpumap.c directly, so it can populate ring slots itself, and the
// per-entry comparison -- the unscoped global term, the one that scales with
// every live map rather than with the pausing queue -- runs for every scanned
// slot whether or not it matches. Populating with a NON-MATCHING owner token
// therefore exercises the full scan while leaving the tombstone-transfer path,
// which correctly asserts nothing can be enqueued in this increment, untouched.
//
// What this asserts is the STRUCTURAL property, which holds regardless of how
// user-mode timings map onto kernel ones: cost is proportional to OCCUPANCY, not
// to ring capacity. The wall-clock figures are printed as an indication only.
//
static
VOID
XdpCpuMapTestQuiesceScanCost(
    VOID
    )
{
#define XDPCPUMAP_TEST_SCAN_MAPS 4
#define XDPCPUMAP_TEST_SCAN_CPUS 8

    XDP_CPUMAP *CpuMaps[XDPCPUMAP_TEST_SCAN_MAPS];
    XDP_CPUMAP_PROVIDER_VALUE Values[XDPCPUMAP_TEST_SCAN_MAPS][XDPCPUMAP_TEST_SCAN_CPUS];
    XDP_CPUMAP_ENTRY_V1 Entry;
    NET_BUFFER_LIST *Sentinel = (NET_BUFFER_LIST *)(ULONG_PTR)0xF00DF00D;
    const UINT32 QuiescingToken = 0;
    const UINT32 OtherToken = 0;
    const UINT32 RingDepth = XDP_CPUMAP_RING_DEPTH_DEFAULT;
    LARGE_INTEGER Frequency;
    LARGE_INTEGER Start;
    LARGE_INTEGER End;
    double FullUs;
    double SparseUs;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("QuiesceScanCost");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCAN_MAPS; M++) {
        XDPCPUMAP_TEST_ASSERT(
            NT_SUCCESS(XdpCpuMapTestCreateMap(64, &CpuMaps[M])));

        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCAN_CPUS; C++) {
            Entry = XdpCpuMapTestEntry(C, RingDepth, 0);
            XDPCPUMAP_TEST_ASSERT(
                NT_SUCCESS(XdpCpuMapTestResolve(CpuMaps[M], &Entry, &Values[M][C])));
        }
    }

    //
    // Fill every ring. The owner tokens deliberately do NOT match the token
    // quiesce is given, so every slot is compared and none is transferred.
    //
    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCAN_MAPS; M++) {
        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCAN_CPUS; C++) {
            XDP_CPUMAP_RING *Ring = Values[M][C].Target->Ring;

            for (UINT32 I = 0; I < RingDepth; I++) {
                Ring->Entries[I].Nbl = Sentinel;
                Ring->Entries[I].RxQueueOwner = &OtherToken;
                Ring->Entries[I].GenericOwner = &OtherToken;
            }

            Ring->Head = 0;
            Ring->Tail = RingDepth;
        }
    }

    QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&Start);
    XdpCpuMapQuiesceInterface(&QuiescingToken);
    QueryPerformanceCounter(&End);

    FullUs =
        ((double)(End.QuadPart - Start.QuadPart) * 1000000.0) / (double)Frequency.QuadPart;

    //
    // Nothing matched, so nothing was tombstoned and the rings are untouched.
    //
    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCAN_MAPS; M++) {
        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCAN_CPUS; C++) {
            XDP_CPUMAP_RING *Ring = Values[M][C].Target->Ring;

            XDPCPUMAP_TEST_ASSERT(Ring->Entries[0].Nbl == Sentinel);
            XDPCPUMAP_TEST_ASSERT(Ring->Head == 0);
            XDPCPUMAP_TEST_ASSERT(Ring->Tail == RingDepth);
        }
    }

    //
    // Now the same rings at ONE EIGHTH occupancy, same capacity. If cost tracked
    // capacity the two would be indistinguishable.
    //
    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCAN_MAPS; M++) {
        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCAN_CPUS; C++) {
            Values[M][C].Target->Ring->Tail = RingDepth / 8;
        }
    }

    QueryPerformanceCounter(&Start);
    XdpCpuMapQuiesceInterface(&QuiescingToken);
    QueryPerformanceCounter(&End);

    SparseUs =
        ((double)(End.QuadPart - Start.QuadPart) * 1000000.0) / (double)Frequency.QuadPart;

    printf(
        "  [QuiesceScanCost] %u maps x %u targets x %u slots: full=%.1fus sparse(1/8)=%.1fus\n",
        (UINT32)XDPCPUMAP_TEST_SCAN_MAPS, (UINT32)XDPCPUMAP_TEST_SCAN_CPUS,
        RingDepth, FullUs, SparseUs);

    //
    // The structural claim: scan cost follows occupancy, not capacity. Asserted
    // loosely -- a factor of two against a factor of eight -- because this is a
    // user-mode process on a shared machine, and the point is the SHAPE of the
    // relationship, not a wall-clock budget.
    //
    XDPCPUMAP_TEST_ASSERT(SparseUs * 2.0 < FullUs || FullUs < 50.0);

    //
    // Drain the synthetic entries before teardown: they hold no real references,
    // but leaving Tail ahead of Head would misrepresent the rings to destroy.
    //
    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCAN_MAPS; M++) {
        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCAN_CPUS; C++) {
            XDP_CPUMAP_RING *Ring = Values[M][C].Target->Ring;

            RtlZeroMemory(Ring->Entries, (SIZE_T)RingDepth * sizeof(XDP_CPUMAP_ENTRY));
            Ring->Head = 0;
            Ring->Tail = 0;
        }
    }

    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCAN_MAPS; M++) {
        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCAN_CPUS; C++) {
            XdpCpuMapTestRelease(CpuMaps[M], &Values[M][C]);
        }
    }

    XdpCpuMapTestDrainSweeps();

    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCAN_MAPS; M++) {
        XDPCPUMAP_TEST_ASSERT(CpuMaps[M]->TargetCount == 0);
        XdpCpuMapTestDestroyMap(CpuMaps[M]);
    }

    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// The full destruction sequence, run exactly as the runtime runs it: entirely
// OUTSIDE an epoch.
//
// ebpf_custom_map_delete releases surviving elements via
// _clean_up_custom_hash_map and then calls postprocess_map_delete, and the whole
// chain is reached from the object's epoch work item. Neither step has a region
// to inherit, so every epoch memory operation either side of that boundary must
// be established by CPUMAP itself. This test is the one that fails if it is not.
//
static
VOID
XdpCpuMapTestTeardownWithoutEpoch(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Values[4];
    XDP_CPUMAP_ENTRY_V1 Entry;
    LONG Baseline;
    LONG EpochOpsBefore;

    XDPCPUMAP_TEST_BEGIN("TeardownWithoutEpoch");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    for (UINT32 Cpu = 0; Cpu < RTL_NUMBER_OF(Values); Cpu++) {
        Entry = XdpCpuMapTestEntry(Cpu, 0, 0);
        XDPCPUMAP_TEST_ASSERT(
            NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Values[Cpu])));
    }

    //
    // From here on, nothing supplies an epoch. Everything below models the
    // runtime's destruction chain.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochDepth == 0);

    for (UINT32 Cpu = 0; Cpu < RTL_NUMBER_OF(Values); Cpu++) {
        XdpCpuMapTestReleaseAtTeardown(CpuMap, &Values[Cpu]);
    }

    //
    // The release callback itself must perform no epoch operation: it can arrive
    // at DISPATCH_LEVEL beneath a base-map lock, where entering a region is not
    // an option. Any operation here would have asserted already.
    //
    EpochOpsBefore = XdpCpuMapTestEpochOperations;
    XDPCPUMAP_TEST_ASSERT(CpuMap->RetireWorkCount == (LONG)RTL_NUMBER_OF(Values));

    XdpCpuMapTestDrainSweeps();

    //
    // The sweep DID free target shells, inside a region it established itself.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochOperations > EpochOpsBefore);
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochDepth == 0);

    EpochOpsBefore = XdpCpuMapTestEpochOperations;

    XdpCpuMapTestDestroyMap(CpuMap);

    //
    // And destroy freed the map itself, likewise inside its own region.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochOperations > EpochOpsBefore);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochDepth == 0);

    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// Helper target rundown acquire/release symmetry, driven through the production
// helper body. A successful helper acquire is paired with exactly one release on
// the non-handoff path used by increment 4. A failed acquire takes nothing, so
// it has no release. The backing reference is deliberately separate and must
// remain held until after the target rundown is released, so map destroy cannot
// observe the data-path reference as gone while the target is still pinned.
//
static
VOID
XdpCpuMapTestHelperTargetRundown(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_REDIRECT_CONTEXT Redirect = {0};
    XDP_CPUMAP_HELPER_STATS Stats;
    XDP_CPUMAP_ENTRY_V1 Entry;
    const UCHAR MapObject = 0;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("HelperTargetRundown");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));
    XDPCPUMAP_TEST_ASSERT(Value.Target != NULL);

    XdpCpuMapTestResetFindElement();
    XdpCpuMapTestFindValue = &Value;

    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestRedirectInEpoch(
            &MapObject, 0, XDP_TX, FALSE, XDP_INTERFACE_MODE_GENERIC, CpuMap, &Redirect) ==
        XDP_REDIRECT);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == CpuMap);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == Value.Target);
    XDPCPUMAP_TEST_ASSERT(Redirect.TargetKey == 0);
    XDPCPUMAP_TEST_ASSERT(Redirect.TargetCpu == Entry.TargetCpu);

    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 1);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 2);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.Calls == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.Success == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.RedirectSlotUnconfigured == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.RedirectModeUnsupported == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.HelperTargetInactive == 0);

    XdpCpuMapClearRedirectContext(&Redirect);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == NULL);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == NULL);
    XDPCPUMAP_TEST_ASSERT(Redirect.TargetKey == 0);
    XDPCPUMAP_TEST_ASSERT(Redirect.TargetCpu == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    ExWaitForRundownProtectionRelease(&Value.Target->PacketRundown);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestRedirectInEpoch(
            &MapObject, 0, XDP_TX, FALSE, XDP_INTERFACE_MODE_GENERIC, CpuMap, &Redirect) ==
        XDP_TX);

    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == NULL);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == NULL);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.Calls == 2);
    XDPCPUMAP_TEST_ASSERT(Stats.Success == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.RedirectSlotUnconfigured == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.RedirectModeUnsupported == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.HelperTargetInactive == 1);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// CPUMAP map-context offset publication guard. The first slot of the fake eBPF
// map deliberately contains a valid-looking XDP header pointer. If the resolver
// dereferences before publication, this case returns success instead of
// EBPF_OPERATION_NOT_SUPPORTED. Publishing offset zero then proves zero is not
// being treated as the unpublished sentinel.
//
static
VOID
XdpCpuMapTestHelperContextOffsetGuard(
    VOID
    )
{
    typedef struct _XDPCPUMAP_TEST_FAKE_MAP {
        XDP_EBPF_MAP_HEADER *Context0;
        UCHAR Padding[8];
        XDP_EBPF_MAP_HEADER *Context16;
    } XDPCPUMAP_TEST_FAKE_MAP;

    XDP_EBPF_MAP_CONTEXT_OFFSET ContextOffset = {0};
    XDPCPUMAP_TEST_FAKE_MAP FakeMap = {0};
    XDP_CPUMAP CpuMapContext = {0};
    XDP_EBPF_MAP_HEADER *Header = &CpuMapContext.Header;
    XDP_EBPF_MAP_HEADER *ResolvedHeader = NULL;
    XDP_CPUMAP *ResolvedCpuMap = NULL;
    XDP_EBPF_MAP_TYPE ResolvedMapType = (XDP_EBPF_MAP_TYPE)0;

    XDPCPUMAP_TEST_BEGIN("HelperContextOffsetGuard");

    CpuMapContext.Header.Type = XdpEbpfMapTypeCpuMap;
    FakeMap.Context0 = Header;
    XDPCPUMAP_TEST_ASSERT(
        XdpEbpfMapContextResolve(&ContextOffset, &FakeMap, &ResolvedHeader) ==
        EBPF_OPERATION_NOT_SUPPORTED);
    XDPCPUMAP_TEST_ASSERT(ResolvedHeader == NULL);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapGetMapFromContextOffset(
            &ContextOffset, &FakeMap, &ResolvedMapType, &ResolvedCpuMap) ==
        EBPF_OPERATION_NOT_SUPPORTED);
    XDPCPUMAP_TEST_ASSERT(ResolvedCpuMap == NULL);

    XdpEbpfMapContextOffsetPublish(&ContextOffset, 0);
    XDPCPUMAP_TEST_ASSERT(
        XdpEbpfMapContextResolve(&ContextOffset, &FakeMap, &ResolvedHeader) == EBPF_SUCCESS);
    XDPCPUMAP_TEST_ASSERT(ResolvedHeader == Header);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapGetMapFromContextOffset(
            &ContextOffset, &FakeMap, &ResolvedMapType, &ResolvedCpuMap) == EBPF_SUCCESS);
    XDPCPUMAP_TEST_ASSERT(ResolvedMapType == XdpEbpfMapTypeCpuMap);
    XDPCPUMAP_TEST_ASSERT(ResolvedCpuMap == &CpuMapContext);

    ContextOffset = (XDP_EBPF_MAP_CONTEXT_OFFSET){0};
    FakeMap.Context0 = NULL;
    FakeMap.Context16 = Header;
    XdpEbpfMapContextOffsetPublish(
        &ContextOffset, FIELD_OFFSET(XDPCPUMAP_TEST_FAKE_MAP, Context16));
    XDPCPUMAP_TEST_ASSERT(
        XdpEbpfMapContextResolve(&ContextOffset, &FakeMap, &ResolvedHeader) == EBPF_SUCCESS);
    XDPCPUMAP_TEST_ASSERT(ResolvedHeader == Header);

    FakeMap.Context16 = NULL;
    XDPCPUMAP_TEST_ASSERT(
        XdpEbpfMapContextResolve(&ContextOffset, &FakeMap, &ResolvedHeader) ==
        EBPF_OPERATION_NOT_SUPPORTED);
    XDPCPUMAP_TEST_ASSERT(ResolvedHeader == NULL);
}

//
// CPUMAP helper lookup must go through the base-map find callback. It must not
// invent a private lookup path, and inactive maps must fail before the base-map
// callback is reached.
//
static
VOID
XdpCpuMapTestHelperFindElement(
    VOID
    )
{
    XDP_CPUMAP CpuMap = {0};
    XDP_CPUMAP_PROVIDER_VALUE Value = {0};
    XDP_CPUMAP_PROVIDER_VALUE *FoundValue = NULL;
    UINT64 Key = 7;
    const UCHAR MapObject = 0;

    XDPCPUMAP_TEST_BEGIN("HelperFindElement");

    XdpCpuMapTestResetFindElement();
    XdpCpuMapTestFindValue = &Value;
    CpuMap.Active = TRUE;
    CpuMap.ClientDispatch = &XdpCpuMapTestClientDispatch;

    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestFindElementInEpoch(&MapObject, &CpuMap, &Key, &FoundValue) ==
        EBPF_SUCCESS);
    XDPCPUMAP_TEST_ASSERT(FoundValue == &Value);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFindCallCount == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFindMap == &MapObject);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFindKey == &Key);

    XdpCpuMapTestFindResult = EBPF_INVALID_ARGUMENT;
    FoundValue = &Value;
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestFindElementInEpoch(&MapObject, &CpuMap, &Key, &FoundValue) ==
        EBPF_INVALID_ARGUMENT);
    XDPCPUMAP_TEST_ASSERT(FoundValue == NULL);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFindCallCount == 2);

    CpuMap.Active = FALSE;
    XdpCpuMapTestFindResult = EBPF_SUCCESS;
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestFindElementInEpoch(&MapObject, &CpuMap, &Key, &FoundValue) ==
        EBPF_INVALID_OBJECT);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFindCallCount == 2);
}

//
// Failure fallbacks must be produced by XdpCpuMapRedirectMap itself, not by a
// lower-level primitive plus manual stats bookkeeping. These cases protect the
// helper's null-provider-value guard and the "failed rundown acquired nothing"
// path: both return the program's fallback action and take no references.
//
static
VOID
XdpCpuMapTestHelperFailureFallbacks(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_REDIRECT_CONTEXT Redirect = {0};
    XDP_CPUMAP_HELPER_STATS Stats;
    XDP_CPUMAP_ENTRY_V1 Entry;
    const UCHAR MapObject = 0;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("HelperFailureFallbacks");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));
    XDPCPUMAP_TEST_ASSERT(Value.Target != NULL);

    //
    // Lookup miss: the base map callback returns no value. The helper must not
    // dereference it and must not acquire target or backing references.
    //
    XdpCpuMapTestResetFindElement();
    XdpCpuMapTestFindResult = EBPF_INVALID_ARGUMENT;
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestRedirectInEpoch(
            &MapObject, 7, XDP_PASS, FALSE, XDP_INTERFACE_MODE_GENERIC, CpuMap, &Redirect) ==
        XDP_PASS);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == NULL);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == NULL);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.Calls == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.Success == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.RedirectSlotUnconfigured == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.RedirectModeUnsupported == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.HelperTargetInactive == 0);

    //
    // Inactive map: XdpCpuMapFindElementFromBaseMap returns INVALID_OBJECT
    // before calling the base-map finder. It is a helper fallback reason, not a
    // target acquire, so it must still take zero references.
    //
    CpuMap->Active = FALSE;
    XdpCpuMapTestFindResult = EBPF_SUCCESS;
    XdpCpuMapTestFindValue = &Value;
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestRedirectInEpoch(
            &MapObject, 8, XDP_DROP, FALSE, XDP_INTERFACE_MODE_GENERIC, CpuMap, &Redirect) ==
        XDP_DROP);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == NULL);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == NULL);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFindCallCount == 1);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.Calls == 2);
    XDPCPUMAP_TEST_ASSERT(Stats.Success == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.RedirectSlotUnconfigured == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.RedirectModeUnsupported == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.HelperTargetInactive == 1);

    CpuMap->Active = TRUE;

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// A CPUMAP redirect in a non-generic interface mode takes the program fallback
// action. Since no target is selected in that path, it must not acquire either
// target rundown or a map backing reference.
//
static
VOID
XdpCpuMapTestHelperModeFallback(
    VOID
    )
{
    static const intptr_t FallbackActions[] = { XDP_PASS, XDP_DROP, XDP_TX };
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_REDIRECT_CONTEXT Redirect = {0};
    XDP_CPUMAP_HELPER_STATS Stats;
    XDP_CPUMAP_ENTRY_V1 Entry;
    const UCHAR MapObject = 0;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("HelperModeFallback");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    XdpCpuMapTestResetFindElement();
    XdpCpuMapTestFindValue = &Value;

    for (UINT32 I = 0; I < RTL_NUMBER_OF(FallbackActions); I++) {
        intptr_t Result;

        XdpCpuMapTestCurrentProcessorIndex = I;
        Result =
            XdpCpuMapTestRedirectInEpoch(
                &MapObject, I, FallbackActions[I], FALSE, XDP_INTERFACE_MODE_NATIVE, CpuMap,
                &Redirect);

        XDPCPUMAP_TEST_ASSERT(Result == FallbackActions[I]);
        XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == NULL);
        XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == NULL);
        XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
        XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
        XDPCPUMAP_TEST_ASSERT(CpuMap->HelperStats[I].Calls == 1);
        XDPCPUMAP_TEST_ASSERT(CpuMap->HelperStats[I].RedirectModeUnsupported == 1);
    }

    XdpCpuMapTestCurrentProcessorIndex = 0;
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFindCallCount == 0);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.Calls == RTL_NUMBER_OF(FallbackActions));
    XDPCPUMAP_TEST_ASSERT(Stats.RedirectModeUnsupported == RTL_NUMBER_OF(FallbackActions));
    XDPCPUMAP_TEST_ASSERT(Stats.Success == 0);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// Multiple helper calls in one eBPF invocation replace the prior redirect
// intent. The replacement path must release the first target rundown and backing
// reference before storing the second one.
//
static
VOID
XdpCpuMapTestHelperReplacementRelease(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value0;
    XDP_CPUMAP_PROVIDER_VALUE Value1;
    XDP_CPUMAP_REDIRECT_CONTEXT Redirect = {0};
    XDP_CPUMAP_HELPER_STATS Stats;
    XDP_CPUMAP_ENTRY_V1 Entry;
    const UCHAR MapObject = 0;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("HelperReplacementRelease");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value0)));
    Entry = XdpCpuMapTestEntry(1, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value1)));

    XdpCpuMapTestResetFindElement();
    XdpCpuMapTestFindValue = &Value0;
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestRedirectInEpoch(
            &MapObject, 0, XDP_DROP, FALSE, XDP_INTERFACE_MODE_GENERIC, CpuMap, &Redirect) ==
        XDP_REDIRECT);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == CpuMap);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == Value0.Target);
    XDPCPUMAP_TEST_ASSERT(Redirect.TargetKey == 0);
    XDPCPUMAP_TEST_ASSERT(Redirect.TargetCpu == Value0.Target->AbsoluteCpu);
    XDPCPUMAP_TEST_ASSERT(Value0.Target->PacketRundown.Count == 1);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 2);

    XdpCpuMapClearRedirectContext(&Redirect);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == NULL);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == NULL);
    XDPCPUMAP_TEST_ASSERT(Value0.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    XdpCpuMapTestFindValue = &Value1;
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestRedirectInEpoch(
            &MapObject, 1, XDP_DROP, FALSE, XDP_INTERFACE_MODE_GENERIC, CpuMap, &Redirect) ==
        XDP_REDIRECT);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == CpuMap);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == Value1.Target);
    XDPCPUMAP_TEST_ASSERT(Redirect.TargetKey == 1);
    XDPCPUMAP_TEST_ASSERT(Redirect.TargetCpu == Value1.Target->AbsoluteCpu);
    XDPCPUMAP_TEST_ASSERT(Value0.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(Value1.Target->PacketRundown.Count == 1);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 2);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFindCallCount == 2);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.Calls == 2);
    XDPCPUMAP_TEST_ASSERT(Stats.Success == 2);

    XdpCpuMapClearRedirectContext(&Redirect);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == NULL);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == NULL);
    XDPCPUMAP_TEST_ASSERT(Redirect.TargetKey == 0);
    XDPCPUMAP_TEST_ASSERT(Redirect.TargetCpu == 0);
    XDPCPUMAP_TEST_ASSERT(Value1.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    XdpCpuMapTestRelease(CpuMap, &Value0);
    XdpCpuMapTestRelease(CpuMap, &Value1);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

static
XDP_FRAME_CPUMAP_REDIRECT_V1
XdpCpuMapTestFrameRedirect(
    _Inout_ XDP_CPUMAP *CpuMap,
    _Inout_ XDP_CPUMAP_TARGET *Target,
    _In_ UINT32 TargetKey
    )
{
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect = {0};

    Redirect.Size = sizeof(Redirect);
    Redirect.Version = XDP_FRAME_CPUMAP_REDIRECT_VERSION_1;
    Redirect.Flags = XDP_FRAME_CPUMAP_REDIRECT_FLAG_OWNERSHIP_PENDING;
    Redirect.CpuMap = CpuMap;
    Redirect.Target = Target;
    Redirect.TargetKey = TargetKey;
    Redirect.TargetCpu = Target->AbsoluteCpu;
    return Redirect;
}

static
VOID
XdpCpuMapTestCommitInvalidMetadata(
    VOID
    )
{
    static const NET_BUFFER_LIST *ActionNbl = (NET_BUFFER_LIST *)(ULONG_PTR)1;
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("CommitInvalidMetadata");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    //
    // This models the smallest faithful form of the VM hang class: the caller
    // observed OWNERSHIP_PENDING in stale/aliased frame bytes, but the metadata
    // did not come from the CPUMAP helper and therefore owns no target rundown
    // or map backing reference. Validation must reject it without releasing
    // either object and without acquiring the queue NBL rundown.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapCommitGroupInit(&CommitGroup, &NblRundown);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    Redirect.Size = sizeof(Redirect) - 1;

    //
    // Invalid metadata is a broken invariant, so checked builds now assert on
    // it. The behaviour under test is the RETAIL path: reject safely, release
    // nothing, and leave the queue rundown untouched. Count the assertion
    // instead of failing on it so that path stays reachable in the harness.
    //
    XdpCpuMapTestExpectAssert = 1;
    XdpCpuMapTestAssertsObserved = 0;
    XDPCPUMAP_TEST_ASSERT(
        !XdpCpuMapCommitRedirect(&Redirect, ActionNbl, FALSE, &CommitGroup));
    XdpCpuMapTestExpectAssert = 0;
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestAssertsObserved > 0);
    XdpCpuMapCommitGroupFinish(&CommitGroup);
    XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);

    //
    // If this test is run against the defective branch for deletion-criterion
    // validation, keep the deliberately corrupted accounting from cascading
    // into unrelated cleanup failures.
    //
    Value.Target->PacketRundown.Count = 0;
    CpuMap->RefCount = 1;

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

static
VOID
XdpCpuMapTestCommitRejectPaths(
    VOID
    )
{
    static const NET_BUFFER_LIST *ActionNbl = (NET_BUFFER_LIST *)(ULONG_PTR)1;
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect;
    XDP_CPUMAP_HELPER_STATS Stats;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("CommitRejectPaths");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    //
    // Step 2 reject: the pause gate rejects before NblRundown is acquired, so
    // the reject path must release only the helper-held target rundown and
    // backing reference.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapCommitGroupInit(&CommitGroup, &NblRundown);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    XDPCPUMAP_TEST_ASSERT(
        !XdpCpuMapCommitRedirect(&Redirect, ActionNbl, TRUE, &CommitGroup));
    XdpCpuMapCommitGroupFinish(&CommitGroup);
    XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseRejected == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownRejected == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitBatchInsertFailed == 0);

    //
    // Step 3 reject: failed ExAcquireRundownProtectionEx does not take a
    // reference, so the shared reject path must again leave NblRundown alone.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapCommitGroupInit(&CommitGroup, &NblRundown);
    ExWaitForRundownProtectionRelease(&NblRundown);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    XDPCPUMAP_TEST_ASSERT(
        !XdpCpuMapCommitRedirect(&Redirect, ActionNbl, FALSE, &CommitGroup));
    XdpCpuMapCommitGroupFinish(&CommitGroup);
    XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseRejected == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownRejected == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitBatchInsertFailed == 0);

    //
    // Step 4 reject: increment 5 has no batch object, so insertion cannot
    // succeed. This path runs after NblRundown was acquired and must release it
    // exactly once at the shared step-5 reject site.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapCommitGroupInit(&CommitGroup, &NblRundown);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    XDPCPUMAP_TEST_ASSERT(
        !XdpCpuMapCommitRedirect(&Redirect, ActionNbl, FALSE, &CommitGroup));
    XdpCpuMapCommitGroupFinish(&CommitGroup);
    XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseRejected == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownRejected == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitBatchInsertFailed == 1);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();

    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == 0);
}

//
// The eBPF program supplies an unconstrained 64-bit key, but a CPUMAP key is
// UINT32 and the base-map lookup reads only that many bytes. A key with nonzero
// upper bits must therefore be rejected BEFORE the lookup, or it would alias
// onto a configured low-32 slot.
//
// This must be a defined fallback, never an assertion: program input is
// untrusted, and a checked xdp.sys with no kernel debugger attached fail-fasts
// the machine on a failed ASSERT.
//
static
VOID
XdpCpuMapTestHelperHighKeyRejected(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_REDIRECT_CONTEXT Redirect = {0};
    XDP_CPUMAP_ENTRY_V1 Entry;
    const UCHAR MapObject = 0;
    LONG Baseline;
    intptr_t Result;

    XDPCPUMAP_TEST_BEGIN("HelperHighKeyRejected");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    XdpCpuMapTestResetFindElement();
    XdpCpuMapTestFindValue = &Value;

    //
    // Upper bits set, low 32 bits naming slot 0 which IS configured. Without the
    // range check this aliases onto that slot and redirects.
    //
    Result =
        XdpCpuMapTestRedirectInEpoch(
            &MapObject, 0x0000000100000000ull, XDP_PASS, FALSE,
            XDP_INTERFACE_MODE_GENERIC, CpuMap, &Redirect);

    XDPCPUMAP_TEST_ASSERT(Result == XDP_PASS);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == NULL);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == NULL);

    //
    // Rejected before the lookup, so the find callback must not have run and no
    // reference may have been taken.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFindCallCount == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// Section 6.3 requires NblRundown acquisition once per flush group, not per
// packet. Reference totals alone cannot prove that — a per-packet implementation
// moves the same totals. This test counts CALLS into the rundown API, which is
// the thing that actually costs an interlocked operation on a shared line.

//
// Valid helper metadata arriving with a NULL ActionNbl. Checked builds must
// assert -- it is a broken invariant -- and retail must still release the target
// rundown and backing reference the metadata genuinely owns, rather than zeroing
// over them. Leaking a rundown reference here is what deadlocks queue teardown.
//
static
VOID
XdpCpuMapTestCommitNullActionNbl(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("CommitNullActionNbl");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapCommitGroupInit(&CommitGroup, &NblRundown);

    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);

    XdpCpuMapTestExpectAssert = 1;
    XdpCpuMapTestAssertsObserved = 0;
    XDPCPUMAP_TEST_ASSERT(
        !XdpCpuMapCommitRedirect(&Redirect, NULL, FALSE, &CommitGroup));
    XdpCpuMapTestExpectAssert = 0;

    //
    // Exactly one assertion: the ActionNbl invariant. The metadata itself was
    // valid, so none of the metadata assertions may have fired.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestAssertsObserved == 1);

    //
    // And the references it owned must have been released, not leaked.
    //
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);

    XdpCpuMapCommitGroupFinish(&CommitGroup);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// Section 6.3 requires NblRundown acquisition to be batched once per flush
// group. Every other commit test uses one packet per fresh group, which CANNOT
// distinguish a credit pool from per-packet acquisition: both end with the same
// reference totals and the same net-zero rundown. The only observable that
// separates them is how many times the code goes to the shared rundown, so this
// test asserts CALL COUNTS.
//
// Deletion criterion: replace XdpCpuMapCommitGroupTakeCredit's pooling with a
// plain per-packet ExAcquireRundownProtectionEx/ExReleaseRundownProtectionEx
// pair and phase 1 fails on its first loop iteration.
//
#define XDPCPUMAP_TEST_BATCH_COMMITS 8u

//
// Must stay under a chunk. A group that commits more than
// XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK entries legitimately acquires more than once,
// so exceeding it here would assert the wrong invariant.
//
C_ASSERT(XDPCPUMAP_TEST_BATCH_COMMITS < XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK);

static
VOID
XdpCpuMapTestCommitGroupBatching(
    VOID
    )
{
    static const NET_BUFFER_LIST *ActionNbl = (NET_BUFFER_LIST *)(ULONG_PTR)1;
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect;
    XDP_CPUMAP_HELPER_STATS Stats;
    LONG Baseline;
    UINT32 Index;

    XDPCPUMAP_TEST_BEGIN("CommitGroupBatching");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    //
    // Phase 1: many commits, one group, one acquire.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapCommitGroupInit(&CommitGroup, &NblRundown);
    XdpCpuMapTestRundownAcquireCalls = 0;
    XdpCpuMapTestRundownReleaseCalls = 0;

    for (Index = 0; Index < XDPCPUMAP_TEST_BATCH_COMMITS; Index++) {
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
        XdpCpuMapReferenceBacking(CpuMap);
        Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
        XDPCPUMAP_TEST_ASSERT(
            !XdpCpuMapCommitRedirect(&Redirect, ActionNbl, FALSE, &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);

        //
        // The assertion that carries the whole claim. Per-packet acquisition
        // would make this Index + 1.
        //
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestRundownAcquireCalls == 1);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestRundownReleaseCalls == 0);
    }

    //
    // The whole chunk is still held mid-group: increment 5 has no batch object,
    // so every commit returned its credit, and a returned credit is group-local
    // state that must not touch the shared rundown.
    //
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == (LONG)XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK);
    XDPCPUMAP_TEST_ASSERT(CommitGroup.Credits == XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    XdpCpuMapCommitGroupFinish(&CommitGroup);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestRundownAcquireCalls == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestRundownReleaseCalls == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitBatchInsertFailed == XDPCPUMAP_TEST_BATCH_COMMITS);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownRejected == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseRejected == 0);

    //
    // Phase 2: a group that commits nothing must not touch the rundown at all.
    // Acquiring in Init would charge every flush that carries no CPUMAP traffic,
    // which is the overwhelmingly common case.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestRundownAcquireCalls = 0;
    XdpCpuMapTestRundownReleaseCalls = 0;
    XdpCpuMapCommitGroupInit(&CommitGroup, &NblRundown);
    XdpCpuMapCommitGroupFinish(&CommitGroup);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestRundownAcquireCalls == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestRundownReleaseCalls == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);

    //
    // Phase 3: once the queue is running down the failure latches for the rest
    // of the group. Retrying per packet would reintroduce exactly the
    // per-packet interlocked cost the pool exists to remove, and it would do so
    // on the pause path, where every packet in the flush takes the same failing
    // trip to the same contended line.
    //
    ExInitializeRundownProtection(&NblRundown);
    ExWaitForRundownProtectionRelease(&NblRundown);
    XdpCpuMapCommitGroupInit(&CommitGroup, &NblRundown);
    XdpCpuMapTestRundownAcquireCalls = 0;
    XdpCpuMapTestRundownReleaseCalls = 0;

    for (Index = 0; Index < XDPCPUMAP_TEST_BATCH_COMMITS; Index++) {
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
        XdpCpuMapReferenceBacking(CpuMap);
        Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
        XDPCPUMAP_TEST_ASSERT(
            !XdpCpuMapCommitRedirect(&Redirect, ActionNbl, FALSE, &CommitGroup));

        //
        // Exactly one failed attempt for the whole group, not one per packet.
        //
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestRundownAcquireCalls == 1);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestRundownReleaseCalls == 0);

        //
        // A rejected commit still owes the references the helper took.
        //
        XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);
        XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
        XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    }

    //
    // Nothing was acquired, so Finish has nothing to release and must not make a
    // pointless call against a rundown that is already down.
    //
    XdpCpuMapCommitGroupFinish(&CommitGroup);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestRundownAcquireCalls == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestRundownReleaseCalls == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownRejected == XDPCPUMAP_TEST_BATCH_COMMITS);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitBatchInsertFailed == XDPCPUMAP_TEST_BATCH_COMMITS);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseRejected == 0);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

INT
__cdecl
main(
    int argc,
    char **argv
    )
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    XdpCpuMapTestZeroSettingsInherit();
    XdpCpuMapTestInvalidCpuIndex();
    XdpCpuMapTestDpcTargetingFailure();
    XdpCpuMapTestSharedTargetAccounting();
    XdpCpuMapTestSweepRearm();
    XdpCpuMapTestCoalescedSweep();
    XdpCpuMapTestAllocationFailure();
    XdpCpuMapTestCapAccounting();
    XdpCpuMapTestQuiesceEmpty();
    XdpCpuMapTestQuiesceScanCost();
    XdpCpuMapTestTeardownWithoutEpoch();
    XdpCpuMapTestHelperTargetRundown();
    XdpCpuMapTestHelperContextOffsetGuard();
    XdpCpuMapTestHelperFindElement();
    XdpCpuMapTestHelperHighKeyRejected();
    XdpCpuMapTestHelperFailureFallbacks();
    XdpCpuMapTestHelperModeFallback();
    XdpCpuMapTestHelperReplacementRelease();
    XdpCpuMapTestCommitInvalidMetadata();
    XdpCpuMapTestCommitRejectPaths();
    XdpCpuMapTestCommitNullActionNbl();
    XdpCpuMapTestCommitGroupBatching();

    XdpCpuMapTestCurrent = "<summary>";

    //
    // Every epoch region entered by a test or by the code under test must have
    // been exited. An imbalance here means a path entered a region and returned
    // without leaving it, which in production would pin reclamation forever.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochDepth == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochOperations > 0);

    if (XdpCpuMapTestFailures > 0) {
        printf("cpumaptest: %u test(s) run, %u FAILURES\n",
            XdpCpuMapTestRun, XdpCpuMapTestFailures);
        return 1;
    }

    printf("cpumaptest: %u test(s) run, all passed\n", XdpCpuMapTestRun);
    return 0;
}
