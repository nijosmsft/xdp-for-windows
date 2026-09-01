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
//   HelperDisallowedQueueNoLeak  a queue that does not permit CPUMAP redirect
//                                makes the helper decline BEFORE acquiring
//                                anything, so nothing is stranded
//   CommitGroupUnusedIsFree      a flush group that carries no CPUMAP packet
//                                costs zero rundown, ring, DPC and NDIS work
//   EnqueueInsertOrdering        section 7 step 5 must precede step 6: the DPC
//                                is queued while the group still holds every
//                                target rundown reference
//   EnqueueTargetInactive        the under-lock re-check is against the TARGET,
//                                not the selector key
//   EnqueueRingFull              a full ring drops the packet, hands the
//                                original back, and releases its references
//   DrainPartitionedIndication   chains partition by (FilterHandle, PortNumber,
//                                flags); both POCs merge them and must not be
//                                copied
//   DrainTombstoneSkip           quiesce tombstones in place; the drain advances
//                                past a tombstone and releases nothing from it
//   DrainYieldRequeueGate        DPC self-requeue is gated by the target
//                                rundown, so retire's KeRemoveQueueDpc cannot be
//                                defeated
//   RetireDrainReturns           a retiring target RETURNS queued packets rather
//                                than indicating them
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
// NDIS indication and return recording.
//

XDP_CPUMAP_TEST_INDICATION XdpCpuMapTestIndications[XDP_CPUMAP_TEST_MAX_INDICATIONS];
ULONG XdpCpuMapTestIndicationCount;
XDP_CPUMAP_TEST_INDICATION XdpCpuMapTestReturns[XDP_CPUMAP_TEST_MAX_INDICATIONS];
ULONG XdpCpuMapTestReturnCount;

VOID
XdpCpuMapTestResetNdis(
    VOID
    )
{
    RtlZeroMemory(XdpCpuMapTestIndications, sizeof(XdpCpuMapTestIndications));
    RtlZeroMemory(XdpCpuMapTestReturns, sizeof(XdpCpuMapTestReturns));
    XdpCpuMapTestIndicationCount = 0;
    XdpCpuMapTestReturnCount = 0;
}

VOID
XdpCpuMapTestRecordNdisCall(
    _Inout_ XDP_CPUMAP_TEST_INDICATION *Log,
    _Inout_ ULONG *LogCount,
    _In_ NDIS_HANDLE FilterHandle,
    _In_ NET_BUFFER_LIST *NblChain,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG NblCount,
    _In_ ULONG Flags
    )
{
    ULONG ActualCount = 0;

    //
    // NDIS is entitled to believe the caller's count. A drain that miscounts a
    // partition would corrupt the upper stack's accounting silently, so verify
    // it here rather than trusting it.
    //
    for (const NET_BUFFER_LIST *Nbl = NblChain; Nbl != NULL; Nbl = Nbl->Next) {
        ActualCount++;
    }
    XDPCPUMAP_TEST_ASSERT(ActualCount == NblCount);
    XDPCPUMAP_TEST_ASSERT(NblCount > 0);

    XDPCPUMAP_TEST_ASSERT(*LogCount < XDP_CPUMAP_TEST_MAX_INDICATIONS);
    if (*LogCount >= XDP_CPUMAP_TEST_MAX_INDICATIONS) {
        return;
    }

    Log[*LogCount].FilterHandle = FilterHandle;
    Log[*LogCount].PortNumber = PortNumber;
    Log[*LogCount].NblCount = NblCount;
    Log[*LogCount].Flags = Flags;
    Log[*LogCount].Head = NblChain;
    (*LogCount)++;
}

//
// DPC queue model.
//

KDPC *XdpCpuMapTestQueuedDpcs[XDP_CPUMAP_TEST_MAX_QUEUED_DPCS];
ULONG XdpCpuMapTestQueuedDpcCount;
ULONG XdpCpuMapTestDpcInsertCalls;
ULONG XdpCpuMapTestDpcRunCount;
LONG XdpCpuMapTestMinRundownAtDpcInsert;
BOOLEAN XdpCpuMapTestShouldYield;

VOID
XdpCpuMapTestResetDpcs(
    VOID
    )
{
    for (ULONG Index = 0; Index < XdpCpuMapTestQueuedDpcCount; Index++) {
        XdpCpuMapTestQueuedDpcs[Index]->Queued = FALSE;
    }

    RtlZeroMemory(XdpCpuMapTestQueuedDpcs, sizeof(XdpCpuMapTestQueuedDpcs));
    XdpCpuMapTestQueuedDpcCount = 0;
    XdpCpuMapTestDpcInsertCalls = 0;
    XdpCpuMapTestDpcRunCount = 0;
    XdpCpuMapTestMinRundownAtDpcInsert = MAXLONG;
    XdpCpuMapTestShouldYield = FALSE;
}

BOOLEAN
XdpCpuMapTestInsertQueueDpc(
    _Inout_ KDPC *Dpc
    )
{
    XdpCpuMapTestDpcInsertCalls++;

    //
    // Every CPUMAP DPC carries its target as the deferred context, so the
    // ordering invariant above is observable right here.
    //
    if (Dpc->Context != NULL) {
        const XDP_CPUMAP_TARGET *Target = (const XDP_CPUMAP_TARGET *)Dpc->Context;

        if (Target->PacketRundown.Count < XdpCpuMapTestMinRundownAtDpcInsert) {
            XdpCpuMapTestMinRundownAtDpcInsert = Target->PacketRundown.Count;
        }
    }

    //
    // The real KeInsertQueueDpc returns FALSE and does nothing when the DPC is
    // already queued. Collapsing repeat inserts is what makes "one DPC per
    // target per flush" the right producer behaviour rather than an
    // optimization.
    //
    if (Dpc->Queued) {
        return FALSE;
    }

    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestQueuedDpcCount < RTL_NUMBER_OF(XdpCpuMapTestQueuedDpcs));
    if (XdpCpuMapTestQueuedDpcCount >= RTL_NUMBER_OF(XdpCpuMapTestQueuedDpcs)) {
        return FALSE;
    }

    XdpCpuMapTestQueuedDpcs[XdpCpuMapTestQueuedDpcCount++] = Dpc;
    Dpc->Queued = TRUE;
    return TRUE;
}

BOOLEAN
XdpCpuMapTestRemoveQueueDpc(
    _Inout_ KDPC *Dpc
    )
{
    if (!Dpc->Queued) {
        return FALSE;
    }

    for (ULONG Index = 0; Index < XdpCpuMapTestQueuedDpcCount; Index++) {
        if (XdpCpuMapTestQueuedDpcs[Index] != Dpc) {
            continue;
        }

        XdpCpuMapTestQueuedDpcCount--;
        for (ULONG Shift = Index; Shift < XdpCpuMapTestQueuedDpcCount; Shift++) {
            XdpCpuMapTestQueuedDpcs[Shift] = XdpCpuMapTestQueuedDpcs[Shift + 1];
        }
        XdpCpuMapTestQueuedDpcs[XdpCpuMapTestQueuedDpcCount] = NULL;
        Dpc->Queued = FALSE;
        return TRUE;
    }

    XDPCPUMAP_TEST_ASSERT(FALSE);
    return FALSE;
}

VOID
XdpCpuMapTestRunQueuedDpcs(
    VOID
    )
{
    ULONG Iterations = 0;

    while (XdpCpuMapTestQueuedDpcCount > 0) {
        KDPC *Dpc = XdpCpuMapTestQueuedDpcs[0];

        XdpCpuMapTestQueuedDpcCount--;
        for (ULONG Shift = 0; Shift < XdpCpuMapTestQueuedDpcCount; Shift++) {
            XdpCpuMapTestQueuedDpcs[Shift] = XdpCpuMapTestQueuedDpcs[Shift + 1];
        }
        XdpCpuMapTestQueuedDpcs[XdpCpuMapTestQueuedDpcCount] = NULL;

        XDPCPUMAP_TEST_ASSERT(Dpc != NULL);
        if (Dpc == NULL) {
            break;
        }

        Dpc->Queued = FALSE;

        XdpCpuMapTestDpcRunCount++;
        Dpc->Routine(Dpc, Dpc->Context, NULL, NULL);

        //
        // A self-requeueing DPC is re-run, which is what the real
        // KeFlushQueuedDpcs ends up doing. Bounded so a drain that fails to make
        // progress fails the test instead of hanging.
        //
        if (++Iterations > 4096) {
            XDPCPUMAP_TEST_ASSERT(FALSE);
            break;
        }
    }
}

//
// Epoch stubs. These ENFORCE the contract from ebpf_extension.h: every epoch
// memory operation must occur inside an epoch-protected region. A provider
// running one outside a region is the defect this enforcement exists to catch,
// and it is otherwise undetectable in test because the operation still succeeds.
//

LONG XdpCpuMapTestEpochDepth;
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
XdpCpuMapTestRedirectInEpochEx(
    _In_ const VOID *Map,
    _In_ UINT64 Key,
    _In_ intptr_t FallbackAction,
    _In_ BOOLEAN IsProgTestRun,
    _In_ BOOLEAN CpuMapRedirectAllowed,
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
            Map, Key, FallbackAction, IsProgTestRun, CpuMapRedirectAllowed, InterfaceMode,
            CpuMap, Redirect);
    XdpCpuMapTestExitCallbackEpoch(&EpochState);

    return Result;
}

//
// The overwhelmingly common case: a receive queue that permits CPUMAP redirect.
//
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
    return
        XdpCpuMapTestRedirectInEpochEx(
            Map, Key, FallbackAction, IsProgTestRun, TRUE, InterfaceMode, CpuMap, Redirect);
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
    XdpCpuMapTestResetAllocator(); \
    XdpCpuMapTestResetNdis(); \
    XdpCpuMapTestResetDpcs()

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
    XDP_CPUMAP_QUIESCE_STATS StatsBefore;
    XDP_CPUMAP_QUIESCE_STATS StatsAfter;
    LONG64 FullScanned;
    LONG64 FullTargets;
    LONG64 FullTombstoned;
    LONG64 SparseScanned;
    LONG64 SparseTargets;
    LONG64 SparseTombstoned;
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
    XdpCpuMapQueryQuiesceStats(&StatsBefore);
    QueryPerformanceCounter(&Start);
    XdpCpuMapQuiesceInterface(&QuiescingToken);
    QueryPerformanceCounter(&End);
    XdpCpuMapQueryQuiesceStats(&StatsAfter);

    FullScanned = StatsAfter.EntriesScanned - StatsBefore.EntriesScanned;
    FullTargets = StatsAfter.TargetsVisited - StatsBefore.TargetsVisited;
    FullTombstoned = StatsAfter.Tombstoned - StatsBefore.Tombstoned;

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

    XdpCpuMapQueryQuiesceStats(&StatsBefore);
    QueryPerformanceCounter(&Start);
    XdpCpuMapQuiesceInterface(&QuiescingToken);
    QueryPerformanceCounter(&End);
    XdpCpuMapQueryQuiesceStats(&StatsAfter);

    SparseScanned = StatsAfter.EntriesScanned - StatsBefore.EntriesScanned;
    SparseTargets = StatsAfter.TargetsVisited - StatsBefore.TargetsVisited;
    SparseTombstoned = StatsAfter.Tombstoned - StatsBefore.Tombstoned;

    SparseUs =
        ((double)(End.QuadPart - Start.QuadPart) * 1000000.0) / (double)Frequency.QuadPart;

    printf(
        "  [QuiesceScanCost] %u maps x %u targets x %u slots: full=%.1fus sparse(1/8)=%.1fus\n",
        (UINT32)XDPCPUMAP_TEST_SCAN_MAPS, (UINT32)XDPCPUMAP_TEST_SCAN_CPUS,
        RingDepth, FullUs, SparseUs);

    //
    // The structural claim, asserted on DETERMINISTIC COUNTERS rather than on
    // wall-clock time: scan cost follows OCCUPANCY, not capacity.
    //
    // The previous form of this assertion -- "sparse is at least twice as fast,
    // OR the full pass took under 50us" -- had a threshold escape. Deleting the
    // scan entirely makes both passes take no time at all, which satisfies the
    // second disjunct, so the test passed against a quiesce that did nothing.
    // A deletion criterion has to fail on an assertion that names the invariant.
    //
    XDPCPUMAP_TEST_ASSERT(
        FullScanned ==
            (LONG64)XDPCPUMAP_TEST_SCAN_MAPS * XDPCPUMAP_TEST_SCAN_CPUS * RingDepth);
    XDPCPUMAP_TEST_ASSERT(
        SparseScanned ==
            (LONG64)XDPCPUMAP_TEST_SCAN_MAPS * XDPCPUMAP_TEST_SCAN_CPUS * (RingDepth / 8));
    XDPCPUMAP_TEST_ASSERT(SparseScanned * 8 == FullScanned);

    //
    // Every target was visited in both passes, so the difference is occupancy
    // and nothing else, and no slot matched so none was transferred.
    //
    XDPCPUMAP_TEST_ASSERT(
        FullTargets == (LONG64)XDPCPUMAP_TEST_SCAN_MAPS * XDPCPUMAP_TEST_SCAN_CPUS);
    XDPCPUMAP_TEST_ASSERT(SparseTargets == FullTargets);
    XDPCPUMAP_TEST_ASSERT(FullTombstoned == 0);
    XDPCPUMAP_TEST_ASSERT(SparseTombstoned == 0);

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

//
// Synthetic indication identity. CPUMAP only ever compares the queue tokens and
// hands the filter handle and port straight back to NDIS, so distinct addresses
// and distinct handles are a faithful model of two interfaces or two ports.
//
static const UCHAR XdpCpuMapTestRxQueueA = 0;
static const UCHAR XdpCpuMapTestRxQueueA2 = 0;
static const UCHAR XdpCpuMapTestRxQueueB = 0;
static const UCHAR XdpCpuMapTestGenericA = 0;
static const UCHAR XdpCpuMapTestGenericB = 0;

#define XDPCPUMAP_TEST_FILTER_A ((NDIS_HANDLE)(ULONG_PTR)0xF117A)
#define XDPCPUMAP_TEST_FILTER_B ((NDIS_HANDLE)(ULONG_PTR)0xF117B)

static
VOID
XdpCpuMapTestInitGroup(
    _Out_ XDP_CPUMAP_COMMIT_GROUP *Group,
    _In_ EX_RUNDOWN_REF *NblRundown
    )
{
    XdpCpuMapCommitGroupInit(
        Group, NblRundown, XDPCPUMAP_TEST_FILTER_A, 0, &XdpCpuMapTestRxQueueA,
        &XdpCpuMapTestGenericA, FALSE);
}

//
// Ends a group and returns how many originals the flush handed back as
// undeliverable. The caller owns those in production; here the count is the
// assertion.
//
static
UINT32
XdpCpuMapTestFinishGroup(
    _Inout_ XDP_CPUMAP_COMMIT_GROUP *Group
    )
{
    NET_BUFFER_LIST *Rejected;
    UINT32 Count = 0;

    XdpCpuMapCommitGroupFinish(Group, &Rejected);

    while (Rejected != NULL) {
        Rejected = NET_BUFFER_LIST_NEXT_NBL(Rejected);
        Count++;
    }

    return Count;
}

//
// Commits one packet through the full section 6.3 sequence, taking the two
// references the helper would have taken first. This is the real pattern; there
// is no reference-taking test helper and there must not be one, because the
// acquire sites are part of what is under test.
//
static
BOOLEAN
XdpCpuMapTestCommit(
    _Inout_ XDP_CPUMAP *CpuMap,
    _Inout_ XDP_CPUMAP_TARGET *Target,
    _Inout_ NET_BUFFER_LIST *Nbl,
    _Inout_ XDP_CPUMAP_COMMIT_GROUP *Group
    )
{
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect;

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Target, 0);

    return XdpCpuMapCommitRedirect(&Redirect, Nbl, FALSE, TRUE, Group);
}

static
VOID
XdpCpuMapTestCommitInvalidMetadata(
    VOID
    )
{
    NET_BUFFER_LIST ActionNbl = {0};
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
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);
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
        !XdpCpuMapCommitRedirect(&Redirect, &ActionNbl, FALSE, TRUE, &CommitGroup));
    XdpCpuMapTestExpectAssert = 0;
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestAssertsObserved > 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 0);

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
    NET_BUFFER_LIST ActionNbl = {0};
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
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    XDPCPUMAP_TEST_ASSERT(
        !XdpCpuMapCommitRedirect(&Redirect, &ActionNbl, TRUE, TRUE, &CommitGroup));
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireExCalls == 0);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseDrop == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownDrop == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyUnsupportedDrop == 0);

    //
    // Step 3 reject: failed ExAcquireRundownProtectionEx does not take a
    // reference, so the shared reject path must again leave NblRundown alone.
    //
    // N.B. the low-resource (!CanPend) outcome is NOT covered here. It is a
    // counted DROP rather than one of these transient rejections, and
    // CommitDeepCopyUnsupportedDrop owns it, asserting that the commit declines
    // ownership -- which a counter alone cannot show. The packet's terminal fate
    // is decided by the caller and is not observable in this harness.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);
    ExWaitForRundownProtectionRelease(&NblRundown);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    XDPCPUMAP_TEST_ASSERT(
        !XdpCpuMapCommitRedirect(&Redirect, &ActionNbl, FALSE, TRUE, &CommitGroup));
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseDrop == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownDrop == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyUnsupportedDrop == 0);

    //
    // TX-inspect is an ASSERTED-IMPOSSIBLE state, not a reject reason. The
    // generic LWF runs outbound sends through this same post-inspection path,
    // but a TX-inspect queue never has CpuMapRedirectEnabled set, so it never
    // registers the frame extension and no send NBL can reach the commit. The
    // behaviour under test is therefore: checked builds assert, and RETAIL still
    // fails closed -- releasing every reference the metadata owns rather than
    // committing a send NBL into a receive ring.
    //
    // Deletion criterion: drop "|| Group->TxInspect" from the commit's reject
    // condition, leaving the assertion alone. The send NBL is committed and
    // enqueued, and the four reference/enqueue assertions below fail while the
    // assertion count still passes -- which is the point: the assert alone does
    // not make retail safe. No other test constructs a TX-inspect group.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapCommitGroupInit(
        &CommitGroup, &NblRundown, XDPCPUMAP_TEST_FILTER_A, 0, &XdpCpuMapTestRxQueueA,
        &XdpCpuMapTestGenericA, TRUE);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);

    XdpCpuMapTestExpectAssert = 1;
    XdpCpuMapTestAssertsObserved = 0;
    XDPCPUMAP_TEST_ASSERT(
        !XdpCpuMapCommitRedirect(&Redirect, &ActionNbl, FALSE, TRUE, &CommitGroup));
    XdpCpuMapTestExpectAssert = 0;

    //
    // Exactly one assertion: the TX-inspect invariant. The metadata itself was
    // valid, so none of the metadata assertions may have fired.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestAssertsObserved == 1);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireExCalls == 0);

    //
    // And it is not counted: TX inspect is a misconfiguration, not one of the
    // three legitimate runtime reject reasons.
    //
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseDrop == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownDrop == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyUnsupportedDrop == 0);

    //
    // Nothing was ever enqueued, so no producer may have queued a DPC.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);

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
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);

    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);

    XdpCpuMapTestExpectAssert = 1;
    XdpCpuMapTestAssertsObserved = 0;
    XDPCPUMAP_TEST_ASSERT(
        !XdpCpuMapCommitRedirect(&Redirect, NULL, FALSE, TRUE, &CommitGroup));
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

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 0);

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
// which phase 3 covers separately.
//
C_ASSERT(XDPCPUMAP_TEST_BATCH_COMMITS < XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK);

//
// Enough commits to exhaust a credit chunk AND overflow the batch, so both the
// second acquisition and the flush-when-full path run.
//
#define XDPCPUMAP_TEST_OVERSIZE_COMMITS (XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK + 8u)

C_ASSERT(XDPCPUMAP_TEST_OVERSIZE_COMMITS > XDP_CPUMAP_BATCH_SIZE);
C_ASSERT(XDPCPUMAP_TEST_OVERSIZE_COMMITS <= 2 * XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK);

static
VOID
XdpCpuMapTestCommitGroupBatching(
    VOID
    )
{
    NET_BUFFER_LIST Nbls[XDPCPUMAP_TEST_OVERSIZE_COMMITS] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
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
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);

    for (Index = 0; Index < XDPCPUMAP_TEST_BATCH_COMMITS; Index++) {
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index], &CommitGroup));

        //
        // The assertion that carries the whole claim. Per-packet acquisition
        // would make this Index + 1.
        //
        XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireExCalls == 1);
        XDPCPUMAP_TEST_ASSERT(NblRundown.ReleaseExCalls == 0);
    }

    //
    // A committed packet KEEPS its credit -- the credit is exactly the reference
    // its ring slot goes on to own -- so the pool has shrunk by the number
    // committed while the rundown itself is untouched.
    //
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == (LONG)XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK);
    XDPCPUMAP_TEST_ASSERT(
        CommitGroup.Credits ==
            XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK - XDPCPUMAP_TEST_BATCH_COMMITS);
    XDPCPUMAP_TEST_ASSERT(CommitGroup.Count == XDPCPUMAP_TEST_BATCH_COMMITS);

    //
    // The batch, not the ring, holds them until the flush: the helper's target
    // references are still outstanding and the DPC has not been queued.
    //
    XDPCPUMAP_TEST_ASSERT(
        Value.Target->PacketRundown.Count == (LONG)XDPCPUMAP_TEST_BATCH_COMMITS);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1 + (LONG)XDPCPUMAP_TEST_BATCH_COMMITS);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 0);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireExCalls == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.ReleaseExCalls == 1);

    //
    // Only the unconsumed credits went back. The ring now owns one reference per
    // enqueued packet, and every helper-held target reference was released --
    // once, in one batched call, after the DPC was queued (section 7 step 6).
    //
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == (LONG)XDPCPUMAP_TEST_BATCH_COMMITS);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.ReleaseExCalls == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 1);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1 + (LONG)XDPCPUMAP_TEST_BATCH_COMMITS);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueCount == XDPCPUMAP_TEST_BATCH_COMMITS);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownDrop == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseDrop == 0);

    //
    // Drain: the DPC hands every packet back and the two references its slots
    // owned go with it.
    //
    XdpCpuMapTestRunQueuedDpcs();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 1);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestIndications[0].NblCount == XDPCPUMAP_TEST_BATCH_COMMITS);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    //
    // Phase 2: once the queue is running down the failure latches for the rest
    // of the group. Retrying per packet would reintroduce exactly the
    // per-packet interlocked cost the pool exists to remove, and it would do so
    // on the pause path, where every packet in the flush takes the same failing
    // trip to the same contended line.
    //
    ExInitializeRundownProtection(&NblRundown);
    ExWaitForRundownProtectionRelease(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);

    for (Index = 0; Index < XDPCPUMAP_TEST_BATCH_COMMITS; Index++) {
        XDPCPUMAP_TEST_ASSERT(
            !XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index], &CommitGroup));

        //
        // Exactly one failed attempt for the whole group, not one per packet.
        //
        XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireExCalls == 1);
        XDPCPUMAP_TEST_ASSERT(NblRundown.ReleaseExCalls == 0);

        //
        // A rejected commit still owes the references the helper took.
        //
        XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
        XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    }

    //
    // Nothing was acquired, so Finish has nothing to release and must not make a
    // pointless call against a rundown that is already down.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireExCalls == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.ReleaseExCalls == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownDrop == XDPCPUMAP_TEST_BATCH_COMMITS);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseDrop == 0);

    //
    // Phase 3: the case the increment 5 review flagged as untested. A group that
    // commits more than one chunk legitimately acquires again -- once per chunk,
    // never once per packet -- and it also overflows the batch, so the
    // flush-when-full path runs inside the commit rather than at Finish.
    //
    XdpCpuMapTestResetNdis();
    XdpCpuMapTestResetDpcs();
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);

    for (Index = 0; Index < XDPCPUMAP_TEST_OVERSIZE_COMMITS; Index++) {
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index], &CommitGroup));

        XDPCPUMAP_TEST_ASSERT(
            NblRundown.AcquireExCalls ==
                1 + Index / XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK);
    }

    XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireExCalls == 2);
    XDPCPUMAP_TEST_ASSERT(NblRundown.ReleaseExCalls == 0);

    //
    // The batch flushed itself when it filled, so the first XDP_CPUMAP_BATCH_SIZE
    // packets are already in the ring with their DPC queued.
    //
    XDPCPUMAP_TEST_ASSERT(
        CommitGroup.Count == XDPCPUMAP_TEST_OVERSIZE_COMMITS - XDP_CPUMAP_BATCH_SIZE);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 1);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == (LONG)XDPCPUMAP_TEST_OVERSIZE_COMMITS);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);

    XdpCpuMapTestRunQueuedDpcs();
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
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
// The TX-inspect exclusion, tested for the LEAK and the TEARDOWN rather than
// for a configuration flag.
//
// This is a reference-ownership property, not a configuration-gating one. The
// helper acquires a target rundown reference and a map backing reference and
// only then records intent; nothing downstream can undo that if the redirect
// was never committable in the first place. So the availability of CPUMAP
// redirect on this queue must be decided BEFORE either acquisition, and it must
// come from the same XdpProgramCanCpuMapRedirect result that decides whether the
// private frame extension is registered, so the two can never disagree.
//
// Asserting only that a TX-inspect queue has its flag clear would be worthless:
// it passes against a gate placed anywhere -- including too late, after the
// acquisitions, where the helper takes two references and hands them to a caller
// that has no extension to record them in. The frame is then dropped by
// XdpInvokeEbpf, which DOES release both references on its way out (the drop
// path breaks to Exit, whose EbpfXdpReleaseCpuMapIntent still sees the intent
// set), so that particular arrangement loses packets rather than leaking. What
// leaks is any arrangement where the helper's intent is never delivered to
// XdpInvokeEbpf's release site at all -- an LWF-only gate, or a caller that
// clears the intent without releasing. Both are prevented by the same rule:
// acquire nothing until admission is established.
//
// The admission value here is DERIVED FROM A REAL HOOK ID via
// XdpCpuMapIsHookSupported, the same predicate XdpProgramCanCpuMapRedirect uses
// to decide whether the frame extension is registered. A test that hand-supplies
// FALSE proves nothing about the queue the decision is actually made for.
//
// Deletion criterion: make XdpCpuMapIsHookSupported accept every direction. The
// TX hook then admits, the helper acquires both references, and the leak,
// teardown and zero-assert assertions all fire, isolated to this test.
//
static
VOID
XdpCpuMapTestHelperDisallowedQueueNoLeak(
    VOID
    )
{
    static const XDP_HOOK_ID TxInspectHook = {
        .Layer      = XDP_HOOK_L2,
        .Direction  = XDP_HOOK_TX,
        .SubLayer   = XDP_HOOK_INSPECT,
    };
    static const XDP_HOOK_ID RxInspectHook = {
        .Layer      = XDP_HOOK_L2,
        .Direction  = XDP_HOOK_RX,
        .SubLayer   = XDP_HOOK_INSPECT,
    };
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_REDIRECT_CONTEXT Redirect = {0};
    XDP_CPUMAP_ENTRY_V1 Entry;
    XDP_CPUMAP_HELPER_STATS Stats;
    const UCHAR MapObject = 0;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("HelperDisallowedQueueNoLeak");

    //
    // The predicate itself, against real hook ids. Everything below feeds its
    // result to the helper rather than a literal.
    //
    XDPCPUMAP_TEST_ASSERT(!XdpCpuMapIsHookSupported(&TxInspectHook));
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapIsHookSupported(&RxInspectHook));

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    //
    // A perfectly good generic-mode redirect to a live, configured target. The
    // ONLY thing wrong with it is the hook direction, which is why the lookup is
    // armed to succeed: a helper that declined for any other reason would prove
    // nothing.
    //
    XdpCpuMapTestResetFindElement();
    XdpCpuMapTestFindValue = &Value;

    //
    // Count assertions rather than suppressing them: the claim is that ZERO
    // fired, not that any that fired were expected.
    //
    XdpCpuMapTestExpectAssert = 1;
    XdpCpuMapTestAssertsObserved = 0;

    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestRedirectInEpochEx(
            &MapObject, 0, XDP_PASS, FALSE, XdpCpuMapIsHookSupported(&TxInspectHook),
            XDP_INTERFACE_MODE_GENERIC, CpuMap, &Redirect) ==
        XDP_PASS);

    //
    // The leak assertions. Nothing acquired means nothing stranded.
    //
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.AcquireCalls == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMap == NULL);
    XDPCPUMAP_TEST_ASSERT(Redirect.CpuMapTarget == NULL);

    //
    // Declined before the lookup, so the gate genuinely precedes both the lookup
    // and the acquisition and cannot be satisfied by a later failure.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFindCallCount == 0);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.Calls == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.Success == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.RedirectModeUnsupported == 1);

    //
    // The same map on a receive hook still works, so the gate is scoped to the
    // queue rather than poisoning the map -- and this leaves real references
    // outstanding, so the teardown below has something to prove.
    //
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestRedirectInEpochEx(
            &MapObject, 0, XDP_PASS, FALSE, XdpCpuMapIsHookSupported(&RxInspectHook),
            XDP_INTERFACE_MODE_GENERIC, CpuMap, &Redirect) ==
        XDP_REDIRECT);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 1);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 2);

    XdpCpuMapClearRedirectContext(&Redirect);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    //
    // Teardown must COMPLETE. Retire waits on the target rundown, and the wait
    // stub asserts the count already reached zero -- a stranded helper reference
    // fails here, which is the harness's model of the hang.
    //
    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);

    //
    // Nothing above may have asserted. An ASSERT on this path would be reachable
    // from an untrusted BPF program.
    //
    XdpCpuMapTestExpectAssert = 0;
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestAssertsObserved == 0);

    XdpCpuMapStop();
}

//
// Frame-slot reuse after a successful commit.
//
// The frame ring is reused constantly, and stale CPUMAP metadata in a reused
// slot is the mechanism that hung the test machine twice in increment 5. A
// successful commit transfers the NBL and all three references out of the
// metadata, so what it leaves behind must own nothing AND claim nothing: if it
// left OWNERSHIP_PENDING set, the caller's own gate would hand the same slot
// back to the commit on its next visit, where it would assert on the metadata
// invariants and release references it no longer holds.
//
// Deletion criterion: replace the RtlZeroMemory at the end of the commit's
// success path with the design's literal "Redirect->Flags |=
// OWNERSHIP_COMMITTED". PENDING survives, the second visit is admitted, and the
// stale-claim and assertion-count assertions here fail. No other test visits a
// frame twice.
//
static
VOID
XdpCpuMapTestCommitFrameReuse(
    VOID
    )
{
    NET_BUFFER_LIST FirstNbl = {0};
    NET_BUFFER_LIST SecondNbl = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_FRAME_CPUMAP_REDIRECT_V1 FrameSlot;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("CommitFrameReuse");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);

    //
    // First use of the slot: an ordinary successful commit.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    FrameSlot = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapCommitRedirect(&FrameSlot, &FirstNbl, FALSE, TRUE, &CommitGroup));

    //
    // The claim: the slot now owns nothing and claims nothing. A caller gating
    // on OWNERSHIP_PENDING must not offer it again, and no stale object pointer
    // may survive into the reused slot.
    //
    XDPCPUMAP_TEST_ASSERT(
        (FrameSlot.Flags & XDP_FRAME_CPUMAP_REDIRECT_FLAG_OWNERSHIP_PENDING) == 0);
    XDPCPUMAP_TEST_ASSERT(FrameSlot.CpuMap == NULL);
    XDPCPUMAP_TEST_ASSERT(FrameSlot.Target == NULL);
    XDPCPUMAP_TEST_ASSERT(FrameSlot.Size == 0);

    //
    // Now reuse the same slot for a frame the eBPF program did NOT redirect --
    // the ordinary case, since the batch path zeroes the extension before each
    // inspection. Nothing may assert, and the references the first packet
    // transferred must be untouched.
    //
    XdpCpuMapTestExpectAssert = 1;
    XdpCpuMapTestAssertsObserved = 0;

    RtlZeroMemory(&FrameSlot, sizeof(FrameSlot));
    XDPCPUMAP_TEST_ASSERT(
        (FrameSlot.Flags & XDP_FRAME_CPUMAP_REDIRECT_FLAG_OWNERSHIP_PENDING) == 0);

    //
    // And a second, genuinely redirected frame through the same slot must commit
    // normally rather than tripping over the first one's residue.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    FrameSlot = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapCommitRedirect(&FrameSlot, &SecondNbl, FALSE, TRUE, &CommitGroup));

    XdpCpuMapTestExpectAssert = 0;
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestAssertsObserved == 0);

    XDPCPUMAP_TEST_ASSERT(CommitGroup.Count == 2);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail - Value.Target->Ring->Head == 2);

    XdpCpuMapTestRunQueuedDpcs();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndications[0].NblCount == 2);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
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
// A low-resource indication makes the commit DECLINE OWNERSHIP, counted.
//
// Scope of this test, stated narrowly and deliberately. It proves what the
// harness can execute: that the commit leaves the original with the caller --
// not consumed into the batch, not handed back as a flush rejection -- and
// releases every reference the metadata held, having taken none of its own.
//
// It does NOT prove the terminal fate of the NBL. That is decided by the
// caller's RX action, which is DROP because XdpInvokeEbpf converted the
// program's XDP_REDIRECT before post-inspection ran, and neither program.c nor
// the LWF receive path is compiled into this harness. Issue #16 called this "a
// counted fallback" and an earlier version of this comment claimed the terminal
// drop; both overstated what is checked here. The end-to-end outcome is
// hardware coverage.
//
// What the narrowed claim still rules out is the failure that matters: a
// low-resource NBL being lent out across a DPC. If the commit took ownership,
// the NBL would be enqueued and later indicated from another CPU, which is
// exactly what NDIS forbids for a RESOURCES indication.
//
// Deletion criterion: move the !CanPend check after XdpCpuMapCommitGroupTakeCredit
// and let it fall into the batch insert instead of the reject path. The NBL is
// then consumed, enqueued and indicated, and the ownership, counter and
// indication assertions here fail.
//
static
VOID
XdpCpuMapTestCommitDeepCopyUnsupportedDrop(
    VOID
    )
{
    NET_BUFFER_LIST ActionNbl = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect;
    XDP_CPUMAP_HELPER_STATS Stats;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("CommitDeepCopyUnsupportedDrop");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);

    //
    // Chain the original to a sentinel so a consumer that took ownership would
    // be visible: the batch and the reject list both re-link Next.
    //
    NET_BUFFER_LIST_NEXT_NBL(&ActionNbl) = (NET_BUFFER_LIST *)(ULONG_PTR)0xD0D0D0D0;

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);

    XDPCPUMAP_TEST_ASSERT(
        !XdpCpuMapCommitRedirect(&Redirect, &ActionNbl, FALSE, FALSE, &CommitGroup));

    //
    // The scope of the claim. Ownership stayed with the caller: the NBL is not
    // in the batch and its chain is untouched, and the flush hands nothing back.
    // What the caller then does with it is not observable here.
    //
    XDPCPUMAP_TEST_ASSERT(CommitGroup.Count == 0);
    XDPCPUMAP_TEST_ASSERT(
        NET_BUFFER_LIST_NEXT_NBL(&ActionNbl) == (NET_BUFFER_LIST *)(ULONG_PTR)0xD0D0D0D0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

    //
    // Nothing was queued, nothing indicated, and the references the helper took
    // were all released.
    //
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail == Value.Target->Ring->Head);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireExCalls == 0);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyUnsupportedDrop == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseDrop == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownDrop == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueCount == 0);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// A flush group that carries no CPUMAP packet must be FREE.
//
// This is the cost model the receive path depends on. `XdpGenericReceivePostInspectNbs`
// skips group init and finish entirely when the receive queue has no CPUMAP
// redirect enabled -- every native program, every XSK-only path, and every
// TX-inspect queue -- and that gate is only worth having because the group
// underneath it is already free of anything more expensive than stores. If Init
// or Finish reached the shared rundown, queued a DPC, or made an NDIS call, a
// regression of that one-line gate would cost the normal receive and XSK paths
// an interlocked operation per indication rather than a handful of stores.
//
// N.B. cpumaptest compiles src/xdp/cpumap.c only, so the LWF gate itself is out
// of the harness's reach. What is asserted here is the property that bounds the
// damage if the gate is ever lost.
//
// Deletion criterion: move the ExAcquireRundownProtectionEx chunk acquisition
// from XdpCpuMapCommitGroupTakeCredit into XdpCpuMapCommitGroupInit -- the
// natural "simplification" that charges every flush. That fails 4 assertions
// here and 13 in CommitRejectPaths, which witnesses the same invariant from the
// other direction: a flush that commits nothing must never reach the shared
// rundown, whether it was never fed a packet or fed one and rejected it. The
// overlap is the point; neither test alone covers both shapes.
//
// CommitGroupBatching phase 1 still passes under that mutation, because a group
// that DOES commit packets makes exactly the same single acquisition either
// way. That is why this claim needs its own test.
//
static
VOID
XdpCpuMapTestCommitGroupUnusedIsFree(
    VOID
    )
{
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_HELPER_STATS Stats;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("CommitGroupUnusedIsFree");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    //
    // A live target exists, so a group that wanted to enqueue could. This one
    // never does.
    //
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);

    //
    // Init alone must not have reached the rundown.
    //
    XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireExCalls == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireCalls == 0);
    XDPCPUMAP_TEST_ASSERT(CommitGroup.Credits == 0);
    XDPCPUMAP_TEST_ASSERT(CommitGroup.Count == 0);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

    //
    // Nor Finish. Zero trips to the shared rundown, in either direction.
    //
    XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireExCalls == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.ReleaseExCalls == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireCalls == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.ReleaseCalls == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);

    //
    // And no ring, DPC, NDIS or target-rundown work either: an empty flush must
    // not touch the target it could have enqueued to.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail == Value.Target->Ring->Head);
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->MaxDepth == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.AcquireCalls == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.ReleaseExCalls == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueCount == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueTargetInactive == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.RingFullCount == 0);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// Section 7 steps 5 and 6, and the "once per target group" rule.
//
// Step 5 queues the DPC while the flush group STILL holds every helper-acquired
// target rundown reference; step 6 releases them afterwards, in one batched
// call. That order is what makes step 5 safe without the separate producer
// reference an earlier design revision introduced and could not account for: a
// held reference means the retire path's ExWaitForRundownProtectionRelease
// cannot have returned, so Target->Dpc cannot have been freed.
//
// Deletion criterion: move the ExReleaseRundownProtectionEx in the flush ahead
// of the KeInsertQueueDpc. XdpCpuMapTestMinRundownAtDpcInsert becomes 0 and this
// test fails; nothing else does, because the resulting steady state is
// identical.
//
static
VOID
XdpCpuMapTestEnqueueInsertOrdering(
    VOID
    )
{
    NET_BUFFER_LIST Nbls[5] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value0;
    XDP_CPUMAP_PROVIDER_VALUE Value1;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_HELPER_STATS Stats;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("EnqueueInsertOrdering");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value0)));
    Entry = XdpCpuMapTestEntry(1, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value1)));
    XDPCPUMAP_TEST_ASSERT(Value0.Target != Value1.Target);

    //
    // One group, two targets, deliberately interleaved so grouping cannot be an
    // accident of insertion order.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestCommit(CpuMap, Value0.Target, &Nbls[0], &CommitGroup));
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestCommit(CpuMap, Value1.Target, &Nbls[1], &CommitGroup));
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestCommit(CpuMap, Value0.Target, &Nbls[2], &CommitGroup));
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestCommit(CpuMap, Value1.Target, &Nbls[3], &CommitGroup));
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestCommit(CpuMap, Value0.Target, &Nbls[4], &CommitGroup));

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

    //
    // The claim.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestMinRundownAtDpcInsert >= 1);

    //
    // One insert per target, one batched rundown release per target, and no
    // per-packet trips to either.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 2);
    XDPCPUMAP_TEST_ASSERT(Value0.Target->PacketRundown.ReleaseExCalls == 1);
    XDPCPUMAP_TEST_ASSERT(Value1.Target->PacketRundown.ReleaseExCalls == 1);
    XDPCPUMAP_TEST_ASSERT(Value0.Target->PacketRundown.ReleaseCalls == 0);
    XDPCPUMAP_TEST_ASSERT(Value1.Target->PacketRundown.ReleaseCalls == 0);
    XDPCPUMAP_TEST_ASSERT(Value0.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(Value1.Target->PacketRundown.Count == 0);

    //
    // Entries went to the ring their target owns, not to whichever ring the
    // flush happened to be holding.
    //
    XDPCPUMAP_TEST_ASSERT(Value0.Target->Ring->Tail - Value0.Target->Ring->Head == 3);
    XDPCPUMAP_TEST_ASSERT(Value1.Target->Ring->Tail - Value1.Target->Ring->Head == 2);
    XDPCPUMAP_TEST_ASSERT(Value0.Target->Ring->MaxDepth == 3);
    XDPCPUMAP_TEST_ASSERT(Value1.Target->Ring->MaxDepth == 2);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueCount == 5);
    XDPCPUMAP_TEST_ASSERT(Stats.RingFullCount == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueTargetInactive == 0);

    XdpCpuMapTestRunQueuedDpcs();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 2);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    XdpCpuMapTestRelease(CpuMap, &Value0);
    XdpCpuMapTestRelease(CpuMap, &Value1);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// Section 7 step 2: the under-lock re-check, and specifically that it is against
// the TARGET rather than the selector key.
//
// The sweep publishes Target->Active = FALSE under ConfigLock and only then
// waits on the rundown, so a producer that acquired before the publish reaches
// the flush with a live reference to a retiring target. A key-scoped check
// cannot see that: an in-place replacement can repoint the key at a new target
// while the old one retires, so a live key says nothing about the ring the
// packet resolved to. POC A has no re-check at all.
//
// These packets are DROPPED, not fallen back: ownership was committed and
// ActionNbl cleared, so no RX action remains.
//
// Deletion criterion: drop "&& Target->Active" from the step 2 conjunction. The
// packet is enqueued instead of rejected and five assertions here fail; no other
// test notices, because every other test flushes against a live target.
//
static
VOID
XdpCpuMapTestEnqueueTargetInactive(
    VOID
    )
{
    NET_BUFFER_LIST Nbls[2] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_HELPER_STATS Stats;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("EnqueueTargetInactive");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    //
    // Commit while the target is live, then retire it underneath the batch. This
    // is exactly the window the design describes.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[0], &CommitGroup));
    Value.Target->Active = FALSE;

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 1);

    //
    // Rejected at the ring, so nothing was queued and no DPC was raised, and the
    // three references the packet carried were all released.
    //
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail == Value.Target->Ring->Head);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueTargetInactive == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueCount == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.RingFullCount == 0);

    Value.Target->Active = TRUE;

    //
    // The other half of the conjunction: a map on its way out rejects too, and
    // the helper's own acquire gate is not a substitute, because it ran before
    // the map went inactive.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[1], &CommitGroup));
    CpuMap->Active = FALSE;

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 1);
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail == Value.Target->Ring->Head);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcInsertCalls == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueTargetInactive == 2);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueCount == 0);

    CpuMap->Active = TRUE;

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// Ring full (section 12). The overflowing packet is dropped -- its original is
// handed back to the caller for return to the miniport -- and its backing and
// NblRundown references are released, while the packets already queued are
// untouched.
//
// Deletion criterion: delete the capacity test in the flush. The ring's Tail
// runs past Head + Capacity, the enqueue count exceeds the ring depth and the
// rejected count is zero; four assertions here fail and no other test does,
// because no other test fills a ring.
//
#define XDPCPUMAP_TEST_SMALL_RING XDP_CPUMAP_RING_DEPTH_MIN

static
VOID
XdpCpuMapTestEnqueueRingFull(
    VOID
    )
{
    NET_BUFFER_LIST Nbls[XDPCPUMAP_TEST_SMALL_RING + 1] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_HELPER_STATS Stats;
    LONG Baseline;
    UINT32 Index;

    XDPCPUMAP_TEST_BEGIN("EnqueueRingFull");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, XDPCPUMAP_TEST_SMALL_RING, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Capacity == XDPCPUMAP_TEST_SMALL_RING);

    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);

    for (Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index], &CommitGroup));
    }

    //
    // Exactly one packet more than the ring holds, so exactly one is handed back.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 1);
    XDPCPUMAP_TEST_ASSERT(
        Value.Target->Ring->Tail - Value.Target->Ring->Head == XDPCPUMAP_TEST_SMALL_RING);
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->MaxDepth == XDPCPUMAP_TEST_SMALL_RING);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueCount == XDPCPUMAP_TEST_SMALL_RING);
    XDPCPUMAP_TEST_ASSERT(Stats.RingFullCount == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueTargetInactive == 0);

    //
    // The dropped packet's references went with it; the queued ones are still
    // owed by the ring.
    //
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == (LONG)XDPCPUMAP_TEST_SMALL_RING);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1 + (LONG)XDPCPUMAP_TEST_SMALL_RING);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);

    XdpCpuMapTestRunQueuedDpcs();
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
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
// Partitioned indication (section 7, "DPC drain").
//
// One ring legitimately holds packets captured on different filters and
// different NDIS ports, because a map is not bound to an interface. The drain
// must indicate each partition with ITS OWN captured handle and port. Both POCs
// instead capture the handle and port of whichever entry they visited last and
// indicate the entire chain with those; that is latent in the POCs only because
// their maps were bound to one filter and their port was almost always zero.
//
// Deletion criterion: make XdpCpuMapChainSetTake always reuse Chains[0] --
// exactly the POC behaviour. One merged indication is produced instead of four
// and the assertions here fail; nothing else does, because every other test
// enqueues from one filter and one port.
//
// The fourth partition exists because the first three varied MORE THAN ONE
// THING: each distinct rundown also carried a distinct filter handle, so the
// rundown component of the partition key was not load-bearing and could be
// deleted with the test still passing -- while releasing references against the
// wrong queue's rundown. Queues 1 and 2 below share a filter handle AND a port
// and differ only in rundown, which is the ordinary case of two RSS queues on
// one interface.
//
static
VOID
XdpCpuMapTestDrainPartitionedIndication(
    VOID
    )
{
    NET_BUFFER_LIST Nbls[8] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF RundownA;
    EX_RUNDOWN_REF RundownB;
    EX_RUNDOWN_REF RundownA2;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_HELPER_STATS Stats;
    LONG Baseline;
    UINT32 Index;
    UINT32 Seen[4] = {0};
    ULONG ReleaseCallsA;
    ULONG ReleaseCallsB;
    ULONG ReleaseCallsA2;

    XDPCPUMAP_TEST_BEGIN("DrainPartitionedIndication");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    ExInitializeRundownProtection(&RundownA);
    ExInitializeRundownProtection(&RundownB);
    ExInitializeRundownProtection(&RundownA2);

    //
    // Four distinct indication identities, interleaved into one ring:
    //   0: filter A, port 0, queue A
    //   1: filter A, port 5, queue A   -- differs only in port
    //   2: filter B, port 0, queue B   -- differs in filter
    //   3: filter A, port 0, queue A2  -- differs ONLY in rundown
    // Each is its own flush group, which is what a real second queue, second
    // port or second interface looks like.
    //
    for (Index = 0; Index < 2; Index++) {
        XdpCpuMapCommitGroupInit(
            &CommitGroup, &RundownA, XDPCPUMAP_TEST_FILTER_A, 0, &XdpCpuMapTestRxQueueA,
            &XdpCpuMapTestGenericA, FALSE);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index * 4 + 0], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        XdpCpuMapCommitGroupInit(
            &CommitGroup, &RundownA, XDPCPUMAP_TEST_FILTER_A, 5, &XdpCpuMapTestRxQueueA,
            &XdpCpuMapTestGenericA, FALSE);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index * 4 + 1], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        XdpCpuMapCommitGroupInit(
            &CommitGroup, &RundownB, XDPCPUMAP_TEST_FILTER_B, 0, &XdpCpuMapTestRxQueueB,
            &XdpCpuMapTestGenericB, FALSE);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index * 4 + 2], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        //
        // Identical filter handle, identical port, identical flags. ONLY the
        // receive queue and its rundown differ. Merging this with partition 0
        // would release both queues' references against one rundown.
        //
        XdpCpuMapCommitGroupInit(
            &CommitGroup, &RundownA2, XDPCPUMAP_TEST_FILTER_A, 0, &XdpCpuMapTestRxQueueA2,
            &XdpCpuMapTestGenericA, FALSE);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index * 4 + 3], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    }

    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail - Value.Target->Ring->Head == 8);
    XDPCPUMAP_TEST_ASSERT(RundownA.Count == 4);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 2);
    XDPCPUMAP_TEST_ASSERT(RundownA2.Count == 2);

    //
    // Baseline the release call counts: the commit groups above have already
    // made their own, so only the drain's calls carry the claim below.
    //
    ReleaseCallsA = RundownA.ReleaseExCalls;
    ReleaseCallsB = RundownB.ReleaseExCalls;
    ReleaseCallsA2 = RundownA2.ReleaseExCalls;

    //
    // One drain pass: the default batch is 32, so all eight are visited together
    // and the partitioning has to happen within a single batch.
    //
    XdpCpuMapTestRunQueuedDpcs();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcRunCount == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 4);

    for (Index = 0; Index < XdpCpuMapTestIndicationCount; Index++) {
        const XDP_CPUMAP_TEST_INDICATION *Indication = &XdpCpuMapTestIndications[Index];

        XDPCPUMAP_TEST_ASSERT(Indication->NblCount == 2);
        XDPCPUMAP_TEST_ASSERT(
            (Indication->Flags & NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL) != 0);
        XDPCPUMAP_TEST_ASSERT((Indication->Flags & NDIS_RECEIVE_FLAGS_RESOURCES) == 0);

        if (Indication->FilterHandle == XDPCPUMAP_TEST_FILTER_A &&
            Indication->PortNumber == 5) {
            Seen[1]++;
        } else if (Indication->FilterHandle == XDPCPUMAP_TEST_FILTER_B &&
                   Indication->PortNumber == 0) {
            Seen[2]++;
        } else if (Indication->FilterHandle == XDPCPUMAP_TEST_FILTER_A &&
                   Indication->PortNumber == 0) {
            //
            // Partitions 0 and 3 are indistinguishable from the indication
            // arguments alone -- that is precisely the point. There must be TWO
            // of them, and the rundown accounting below says which is which.
            //
            Seen[0]++;
        } else {
            XDPCPUMAP_TEST_ASSERT(FALSE);
        }
    }

    XDPCPUMAP_TEST_ASSERT(Seen[0] == 2);
    XDPCPUMAP_TEST_ASSERT(Seen[1] == 1);
    XDPCPUMAP_TEST_ASSERT(Seen[2] == 1);

    //
    // The assertion the fourth partition exists for: each rundown was released
    // exactly against its own entries. Merging partitions 0 and 3 would release
    // four against RundownA and none against RundownA2 -- the totals would still
    // balance in aggregate, so only per-object accounting catches it.
    //
    XDPCPUMAP_TEST_ASSERT(RundownA.Count == 0);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 0);
    XDPCPUMAP_TEST_ASSERT(RundownA2.Count == 0);
    XDPCPUMAP_TEST_ASSERT(RundownA.ReleaseExCalls - ReleaseCallsA == 2);
    XDPCPUMAP_TEST_ASSERT(RundownB.ReleaseExCalls - ReleaseCallsB == 1);
    XDPCPUMAP_TEST_ASSERT(RundownA2.ReleaseExCalls - ReleaseCallsA2 == 1);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.DrainCount == 8);
    XDPCPUMAP_TEST_ASSERT(Stats.IndicateChainCount == 4);
    XDPCPUMAP_TEST_ASSERT(Stats.DrainTombstoneCount == 0);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// Tombstones (sections 7.1 and 8.4). Quiesce takes a matching entry's NBL AND
// every reference it owned out under the ring lock in one step, leaving a slot
// that owns NOTHING, and the drain advances past it releasing nothing. Entries
// belonging to other queues are untouched and still indicate normally.
//
// Deletion criterion (skip): in the drain, do not advance Head past a tombstone.
// The ring never drains and nine assertions here fail.
//
// Deletion criterion (ownership transfer): replace XdpCpuMapChainSetTake's
// RtlZeroMemory with a field-by-field copy that leaves BackingRef and
// NblRundown behind. The slot-content assertions below fail.
//
// N.B. an earlier form of the second criterion mutated the RtlZeroMemory to
// "Entry->Nbl = NULL" and the test still passed -- because Take copies the
// references into the chain BEFORE clearing, so a partial clear still transfers
// and still releases them. The mutation changed how the slot was cleared without
// removing the transfer under test, and only the direct slot-content assertions
// added below can catch it.
//
static
VOID
XdpCpuMapTestDrainTombstoneSkip(
    VOID
    )
{
    NET_BUFFER_LIST Nbls[4] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF RundownA;
    EX_RUNDOWN_REF RundownB;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_HELPER_STATS Stats;
    XDP_CPUMAP_RING *Ring;
    LONG Baseline;
    UINT32 Index;

    XDPCPUMAP_TEST_BEGIN("DrainTombstoneSkip");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    ExInitializeRundownProtection(&RundownA);
    ExInitializeRundownProtection(&RundownB);

    //
    // Two receive queues interleaved in one ring, so the scan cannot pass by
    // matching a contiguous run.
    //
    for (Index = 0; Index < 2; Index++) {
        XdpCpuMapCommitGroupInit(
            &CommitGroup, &RundownA, XDPCPUMAP_TEST_FILTER_A, 0, &XdpCpuMapTestRxQueueA,
            &XdpCpuMapTestGenericA, FALSE);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index * 2 + 0], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        XdpCpuMapCommitGroupInit(
            &CommitGroup, &RundownB, XDPCPUMAP_TEST_FILTER_B, 0, &XdpCpuMapTestRxQueueB,
            &XdpCpuMapTestGenericB, FALSE);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index * 2 + 1], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    }

    XDPCPUMAP_TEST_ASSERT(RundownA.Count == 2);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 2);

    //
    // Slots 0 and 2 belong to queue A and are the ones quiesce will tombstone.
    //
    Ring = Value.Target->Ring;
    XDPCPUMAP_TEST_ASSERT(Ring->Entries[0].RxQueueOwner == &XdpCpuMapTestRxQueueA);
    XDPCPUMAP_TEST_ASSERT(Ring->Entries[2].RxQueueOwner == &XdpCpuMapTestRxQueueA);

    //
    // Quiesce queue A. It tombstones A's entries in place and returns their NBLs
    // to the miniport without indicating them, then its single
    // KeFlushQueuedDpcs runs the drain, which must walk past the tombstones and
    // indicate only B's packets.
    //
    XdpCpuMapQuiesceRxQueue(&XdpCpuMapTestRxQueueA);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturns[0].FilterHandle == XDPCPUMAP_TEST_FILTER_A);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturns[0].NblCount == 2);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 1);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestIndications[0].FilterHandle == XDPCPUMAP_TEST_FILTER_B);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndications[0].NblCount == 2);

    //
    // Head advanced past everything, tombstones included, and every reference is
    // accounted for exactly once.
    //
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail == Value.Target->Ring->Head);
    XDPCPUMAP_TEST_ASSERT(RundownA.Count == 0);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    //
    // A tombstone owns NOTHING. Asserted on the slot itself, not inferred from
    // reference totals: the totals balance under any implementation that
    // transfers ownership out, including one that clears only Nbl and leaves
    // dangling BackingRef and NblRundown pointers behind in a slot the producer
    // will reuse. Ownership transfer and slot clearing are one step or they are
    // not atomic (sections 7.1 and 8.4).
    //
    for (Index = 0; Index < 4; Index += 2) {
        const XDP_CPUMAP_ENTRY *Slot = &Ring->Entries[Index];

        XDPCPUMAP_TEST_ASSERT(Slot->Nbl == NULL);
        XDPCPUMAP_TEST_ASSERT(Slot->BackingRef == NULL);
        XDPCPUMAP_TEST_ASSERT(Slot->NblRundown == NULL);
        XDPCPUMAP_TEST_ASSERT(Slot->FilterHandle == NULL);
        XDPCPUMAP_TEST_ASSERT(Slot->RxQueueOwner == NULL);
        XDPCPUMAP_TEST_ASSERT(Slot->GenericOwner == NULL);
        XDPCPUMAP_TEST_ASSERT(Slot->PortNumber == 0);
        XDPCPUMAP_TEST_ASSERT(!Slot->IsDeepCopy);
    }

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.DrainTombstoneCount == 2);
    XDPCPUMAP_TEST_ASSERT(Stats.DrainCount == 2);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// The rundown-gated self-requeue (section 7, section 8.1a row 2).
//
// POC A re-queues its drain DPC whenever work remains and the processor asks it
// to yield. Production must acquire Target->PacketRundown first and re-queue only
// if that succeeds, because otherwise a DPC can re-queue itself AFTER the retire
// path's KeRemoveQueueDpc has cancelled it, defeating the cancel and leaving a
// DPC pointing at freed memory.
//
// Deletion criterion: call KeInsertQueueDpc unconditionally on the yield path.
// Phase 2 then re-queues against a retiring target and runs a second time, so
// both the run count and the "work is still in the ring" assertions fail here
// and nowhere else.
//
#define XDPCPUMAP_TEST_YIELD_PACKETS (XDP_CPUMAP_DRAIN_BATCH_DEFAULT + 8u)

static
VOID
XdpCpuMapTestDrainYieldRequeueGate(
    VOID
    )
{
    NET_BUFFER_LIST Nbls[XDPCPUMAP_TEST_YIELD_PACKETS] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_HELPER_STATS Stats;
    LONG Baseline;
    UINT32 Index;

    XDPCPUMAP_TEST_BEGIN("DrainYieldRequeueGate");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);
    for (Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index], &CommitGroup));
    }
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(
        Value.Target->Ring->Tail - Value.Target->Ring->Head == RTL_NUMBER_OF(Nbls));

    //
    // Phase 1, gate open: more work than one batch and the processor asking to
    // yield, so the drain must stop after one batch and re-queue itself rather
    // than running the ring dry in one DPC.
    //
    XdpCpuMapTestShouldYield = TRUE;
    XdpCpuMapTestDpcRunCount = 0;
    XdpCpuMapTestRunQueuedDpcs();

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcRunCount == 2);
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail == Value.Target->Ring->Head);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 2);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestIndications[0].NblCount == XDP_CPUMAP_DRAIN_BATCH_DEFAULT);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndications[1].NblCount == 8);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.DpcRequeueCount == 1);

    //
    // Phase 2, gate closed. Refill, then run the target's rundown down exactly
    // as the retire path does before it cancels the DPC. The drain must take its
    // batch and then decline to re-queue.
    //
    XdpCpuMapTestResetNdis();
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);
    for (Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index], &CommitGroup));
    }
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

    ExWaitForRundownProtectionRelease(&Value.Target->PacketRundown);

    XdpCpuMapTestDpcRunCount = 0;
    XdpCpuMapTestRunQueuedDpcs();

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcRunCount == 1);
    XDPCPUMAP_TEST_ASSERT(
        Value.Target->Ring->Tail - Value.Target->Ring->Head == 8);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 1);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.DpcRequeueCount == 1);

    //
    // Retire drains the remainder synchronously, which is exactly the promise
    // the gate relies on.
    //
    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// Delivery on retire (section 8.3, section 8.1a row 9a "Released -- teardown").
//
// A target that retires with packets still queued RETURNS them to the miniport
// WITHOUT indicating them. Indicating would deliver packets on a CPU whose
// steering configuration has just been deleted, and the design chooses a counted
// drop instead.
//
// Deletion criterion: have XdpCpuMapDrainRing call XdpCpuMapChainSetIndicate
// instead of XdpCpuMapChainSetReturn. The two assertions on indication and
// return counts here fail; every other test either drains through the DPC or
// retires an empty ring, so none of them notices.
//
static
VOID
XdpCpuMapTestRetireDrainReturns(
    VOID
    )
{
    NET_BUFFER_LIST Nbls[3] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    KDPC *TargetDpc;
    LONG Baseline;
    UINT32 Index;

    XDPCPUMAP_TEST_BEGIN("RetireDrainReturns");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));
    TargetDpc = Value.Target->Dpc;

    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);
    for (Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index], &CommitGroup));
    }
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

    //
    // The DPC is queued and deliberately not run: retire has to cancel it and
    // then own the drain itself.
    //
    XDPCPUMAP_TEST_ASSERT(TargetDpc->Queued);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 3);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturns[0].FilterHandle == XDPCPUMAP_TEST_FILTER_A);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturns[0].NblCount == 3);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

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
    XdpCpuMapTestHelperDisallowedQueueNoLeak();
    XdpCpuMapTestHelperReplacementRelease();
    XdpCpuMapTestCommitInvalidMetadata();
    XdpCpuMapTestCommitRejectPaths();
    XdpCpuMapTestCommitNullActionNbl();
    XdpCpuMapTestCommitGroupBatching();
    XdpCpuMapTestCommitFrameReuse();
    XdpCpuMapTestCommitDeepCopyUnsupportedDrop();
    XdpCpuMapTestCommitGroupUnusedIsFree();
    XdpCpuMapTestEnqueueInsertOrdering();
    XdpCpuMapTestEnqueueTargetInactive();
    XdpCpuMapTestEnqueueRingFull();
    XdpCpuMapTestDrainPartitionedIndication();
    XdpCpuMapTestDrainTombstoneSkip();
    XdpCpuMapTestDrainYieldRequeueGate();
    XdpCpuMapTestRetireDrainReturns();

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
