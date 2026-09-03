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
//   RetireSuppressedTargetDpc    a target whose CPU never runs its DPC still
//                                retires: retire cancels the queued instance and
//                                drains the ring on the worker thread
//   QuiesceScoping               quiesce reaches every ring of every live map,
//                                collects only the pausing queue's entries, and
//                                releases its per-map reference
//   QuiesceInterfaceScope        interface pause matches by GENERIC, so every
//                                queue on that interface is collected and no
//                                queue on another one is
//   QuiesceTailSnapshot          one pass is bounded by the tail it snapshotted,
//                                not by a tail a producer keeps advancing
//   QuiescePassBudget            the pass loop terminates at the budget, counts
//                                the exhaustion, and leaves the rundown wait
//                                able to complete
//   QuiesceTombstoneBalance      the section 8.1a audit table, row by row, on a
//                                ring mixing a deep copy, originals and a peer
//   QuiesceDurationAttribution   ScanUs is the scan and FlushUs is the flush,
//                                proved in both directions -- their SUM being
//                                right is satisfied by a build that transposes
//                                them
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

//
// See stubs/ntos.h for what this is and, more importantly, for what may not use
// it. NULL except while one of the two quiesce concurrency tests is running.
//
BOOLEAN (*XdpCpuMapTestRingLockReleaseHook)(VOID);

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

//
// When set, the indication stub stamps upper-stack metadata into slots CPUMAP
// does not carry -- which real consumers do, and nothing requires them to clear.
// Off by default so the reuse path is testable; on, it makes the discard path
// testable. Both are real: a clean return is reused, a stamped one is freed.
//
BOOLEAN XdpCpuMapTestStampOnIndicate;

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

    Log[*LogCount].NblSnapshotCount = 0;
    Log[*LogCount].NblSnapshotTruncated = FALSE;
    for (NET_BUFFER_LIST *Nbl = NblChain; Nbl != NULL; Nbl = Nbl->Next) {
        if (Log[*LogCount].NblSnapshotCount == RTL_NUMBER_OF(Log[*LogCount].NblSnapshot)) {
            //
            // Recorded rather than silently dropped: a truncated snapshot would
            // make a duplicate look absent, which is the exact conclusion these
            // tests must never reach by accident.
            //
            Log[*LogCount].NblSnapshotTruncated = TRUE;
            break;
        }

        Log[*LogCount].NblSnapshot[Log[*LogCount].NblSnapshotCount++] = Nbl;

        //
        // Model the upper stack stamping its OWN metadata on an NBL it was
        // handed. Real consumers do this -- classification handles, cancel ids,
        // WFP context -- and it is what makes a recycled descriptor dirty.
        //
        // Without it the harness could not tell a build that zeroes the OOB
        // slots before copying from one that does not, because every descriptor
        // it ever saw was already clean. That gap let the cross-packet metadata
        // leak survive a full criterion sweep undetected.
        //
        if (XdpCpuMapTestStampOnIndicate) {
            Nbl->NetBufferListInfo[ClassificationHandleNetBufferListInfo] =
                (VOID *)(ULONG_PTR)0xC1A551F1;
            Nbl->NetBufferListInfo[NetBufferListCancelId] = (VOID *)(ULONG_PTR)0xCA9CE11D;
        }
    }

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
    XdpCpuMapTestResetDpcs(); \
    XdpCpuMapTestRingLockReleaseHook = NULL

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
    // The scan/flush split is measured from ONE clock reading pair, so the two
    // parts account for the whole: each is a floor division of the same
    // frequency, and a sum of two floors is the floor of the sum or one less.
    //
    // The SUM is all this proves. Which term is which is proved by
    // XdpCpuMapTestQuiesceDurationAttribution, because a build that transposed
    // them would satisfy every assertion here.
    //
    XDPCPUMAP_TEST_ASSERT(
        StatsAfter.LastScanDurationUs + StatsAfter.LastFlushDurationUs <=
            StatsAfter.LastDurationUs);
    XDPCPUMAP_TEST_ASSERT(
        StatsAfter.LastDurationUs <=
            StatsAfter.LastScanDurationUs + StatsAfter.LastFlushDurationUs + 1);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.MaxDurationUs >= StatsAfter.LastDurationUs);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.MaxScanDurationUs >= StatsAfter.LastScanDurationUs);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.MaxFlushDurationUs >= StatsAfter.LastFlushDurationUs);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.MaxPassesExhausted == StatsBefore.MaxPassesExhausted);

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
        &XdpCpuMapTestGenericA, FALSE, FALSE, NULL);
}

//
// A low-resource group: every entry becomes a deep copy taken from Pool.
//
static
VOID
XdpCpuMapTestInitDeepCopyGroup(
    _Out_ XDP_CPUMAP_COMMIT_GROUP *Group,
    _In_ EX_RUNDOWN_REF *NblRundown,
    _In_opt_ XDP_CPUMAP_DEEPCOPY_POOL *Pool
    )
{
    XdpCpuMapCommitGroupInit(
        Group, NblRundown, XDPCPUMAP_TEST_FILTER_A, 0, &XdpCpuMapTestRxQueueA,
        &XdpCpuMapTestGenericA, FALSE, TRUE, Pool);
}

//
// Ends a group and returns the originals the flush handed back as undeliverable,
// as a chain, so callers can check IDENTITY rather than count.
//
static
NET_BUFFER_LIST *
XdpCpuMapTestFinishGroupEx(
    _Inout_ XDP_CPUMAP_COMMIT_GROUP *Group
    )
{
    NET_BUFFER_LIST *Rejected;

    XdpCpuMapCommitGroupFinish(Group, &Rejected);

    return Rejected;
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
    NET_BUFFER_LIST *Rejected = XdpCpuMapTestFinishGroupEx(Group);
    UINT32 Count = 0;

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
        XdpCpuMapCommitRedirect(&Redirect, &ActionNbl, FALSE, TRUE, &CommitGroup) == XdpCpuMapCommitDeclined);
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
        XdpCpuMapCommitRedirect(&Redirect, &ActionNbl, TRUE, TRUE, &CommitGroup) == XdpCpuMapCommitDeclined);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.AcquireExCalls == 0);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseDrop == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownDrop == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyFailCount == 0);

    //
    // Step 3 reject: failed ExAcquireRundownProtectionEx does not take a
    // reference, so the shared reject path must again leave NblRundown alone.
    //
    // N.B. the low-resource (!CanPend) outcome is NOT a rejection at all since
    // increment 8. It commits, and the flush builds a deep copy; DeepCopySuccess
    // and DeepCopyFailurePaths own it.
    //
    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);
    ExWaitForRundownProtectionRelease(&NblRundown);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapCommitRedirect(&Redirect, &ActionNbl, FALSE, TRUE, &CommitGroup) == XdpCpuMapCommitDeclined);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    XDPCPUMAP_TEST_ASSERT(Redirect.Size == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitPauseDrop == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.CommitRundownDrop == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyFailCount == 0);

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
        &XdpCpuMapTestGenericA, TRUE, FALSE, NULL);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);

    XdpCpuMapTestExpectAssert = 1;
    XdpCpuMapTestAssertsObserved = 0;
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapCommitRedirect(&Redirect, &ActionNbl, FALSE, TRUE, &CommitGroup) == XdpCpuMapCommitDeclined);
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
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyFailCount == 0);

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
        XdpCpuMapCommitRedirect(&Redirect, NULL, FALSE, TRUE, &CommitGroup) == XdpCpuMapCommitDeclined);
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
// Per-NBL disposition across everything NDIS was told to do with it.
//
// Increment 6's tests counted indications and returns. That cannot see the
// failures increment 7 exists to rule out: an NBL indicated twice, an NBL both
// indicated and returned, or an NBL that quietly appears in a partition it does
// not belong to all produce the same totals as correct behaviour. Identity is
// the only observable that separates them, so every zero-copy assertion below is
// made per NBL rather than per count.
//
typedef struct _XDPCPUMAP_TEST_NBL_DISPOSITION {
    UINT32 Indicated;
    UINT32 Returned;
    UINT32 IndicationIndex;
    ULONG IndicationFlags;
} XDPCPUMAP_TEST_NBL_DISPOSITION;

static
XDPCPUMAP_TEST_NBL_DISPOSITION
XdpCpuMapTestNblDisposition(
    _In_ const NET_BUFFER_LIST *Nbl
    )
{
    XDPCPUMAP_TEST_NBL_DISPOSITION Disposition = {0};

    Disposition.IndicationIndex = MAXUINT32;

    for (ULONG Index = 0; Index < XdpCpuMapTestIndicationCount; Index++) {
        const XDP_CPUMAP_TEST_INDICATION *Indication = &XdpCpuMapTestIndications[Index];

        //
        // The snapshot, not Head->Next: see XDP_CPUMAP_TEST_INDICATION. A
        // truncated snapshot cannot support a negative conclusion, so refuse it
        // outright rather than under-reporting appearances.
        //
        XDPCPUMAP_TEST_ASSERT(!Indication->NblSnapshotTruncated);
        XDPCPUMAP_TEST_ASSERT(Indication->NblSnapshotCount == Indication->NblCount);

        for (ULONG Slot = 0; Slot < Indication->NblSnapshotCount; Slot++) {
            if (Indication->NblSnapshot[Slot] == Nbl) {
                Disposition.Indicated++;
                Disposition.IndicationIndex = Index;
                Disposition.IndicationFlags = Indication->Flags;
            }
        }
    }

    for (ULONG Index = 0; Index < XdpCpuMapTestReturnCount; Index++) {
        const XDP_CPUMAP_TEST_INDICATION *Return = &XdpCpuMapTestReturns[Index];

        XDPCPUMAP_TEST_ASSERT(!Return->NblSnapshotTruncated);
        XDPCPUMAP_TEST_ASSERT(Return->NblSnapshotCount == Return->NblCount);

        for (ULONG Slot = 0; Slot < Return->NblSnapshotCount; Slot++) {
            if (Return->NblSnapshot[Slot] == Nbl) {
                Disposition.Returned++;
            }
        }
    }

    return Disposition;
}

//
// Counts how many of the NBLs in a caller-supplied array appear in a chain, so a
// rejected-original list can be checked for identity rather than length.
//
static
UINT32
XdpCpuMapTestCountNblsInChain(
    _In_opt_ const NET_BUFFER_LIST *Chain,
    _In_ const NET_BUFFER_LIST *Nbl
    )
{
    UINT32 Count = 0;

    for (const NET_BUFFER_LIST *Cur = Chain; Cur != NULL; Cur = Cur->Next) {
        if (Cur == Nbl) {
            Count++;
        }
    }

    return Count;
}


//
// Deep-copy success, end to end, on NBL IDENTITY (design section 8.1a row 9b).
//
// Row 9b differs from row 9a in the one way that matters: the ORIGINAL never
// enters the ring. Commit leaves it with the caller, whose DROP action returns
// it to the miniport, and only a COPY travels to the target CPU. Round 5 of the
// design collapsed the two rows and would have had a reviewer expect a double
// return of the original.
//
// What is asserted here, and why counting cannot substitute:
//
//   - The ring slot holds a DIFFERENT NBL from the one committed, flagged as a
//     deep copy, carrying the pool it came from. A build that enqueued the
//     original would pass any count-based check.
//   - The copy carries the original's BYTES and its RSS hash and checksum OOB
//     slots. The harness copies real bytes through a real MDL walk, so a build
//     that allocated but did not copy fails here rather than silently
//     delivering garbage.
//   - The copy is indicated WITH NDIS_RECEIVE_FLAGS_RESOURCES. This is the first
//     increment in which that branch executes at all.
//   - The copy is RECYCLED before the DPC returns, and never returned to the
//     miniport. The page counter proves the recycle actually gave the pages
//     back rather than merely unlinking the NBL.
//   - The original is untouched by CPUMAP throughout: not indicated, not
//     returned, and still chained where the caller left it.
//
// Deletion criteria, each naming the production operation removed. Radii are
// from running all 40 cases in isolation, with each outcome typed as pass,
// assertion failure, crash or hang.
//
//   (a) In XdpCpuMapDeepCopyAllocate, return Original instead of Copy. The
//       operation removed is "the ring carries the copy, not the original": the
//       ORIGINAL is then enqueued, indicated from the target CPU, and finally
//       recycled into the pool by the drain -- which is a miniport-owned NBL
//       being pushed onto our own free list. Detected as a CRASH rather than an
//       assertion failure. That is the point: in production this is memory
//       corruption, not a miscount.
//
//       N.B. deleting "BatchEntry->Nbl = Copy;" in the flush pre-pass is the
//       more direct phrasing of the same deletion and was the original wording
//       here, but it does not compile: removing it costs /analyze the path
//       invariant that Target != NULL implies CpuMap != NULL, and C6387/C6011
//       fire at the Stats fetch. Mutating the allocator's return value removes
//       the same operation without perturbing that proof.
//   (b) In XdpCpuMapChainSetIndicate, skip XdpCpuMapChainRecycleDeepCopies. The
//       operation removed is "a deep copy goes back to its pool inside the DPC".
//       Counts are unchanged -- the copy is still indicated exactly once -- and
//       only the live-page and live-descriptor assertions can see the leak.
//   (c) In XdpCpuMapDeepCopyAllocate, delete the carry loop outright. The
//       operation removed is "carried metadata travels with the copy"; bytes
//       still arrive, so only the per-slot metadata assertions fail -- one per
//       carried slot.
//   (i) In XdpCpuMapCommitRedirect, return XdpCpuMapCommitOwnershipTaken on the
//       low-resource path. The operation removed is "the caller keeps the
//       original": recv.c clears ActionNbl, so the original never reaches
//       DropList and is never returned to the miniport, while the copy is
//       delivered as well.
//   (k) In XdpCpuMapDeepCopyAllocate, drop the Copy->SourceHandle assignment.
//       The operation removed is "an originated receive NBL names its
//       originating filter", which is what NDIS routes the return by.
//   (l) In XdpCpuMapCommitRedirect, replace the inline
//       "Enqueued = XdpCpuMapFlushBatch(Group);" with "Enqueued = 1;", so the
//       batch is NOT flushed before returning and defers to
//       XdpCpuMapCommitGroupFinish. The operation removed is "nothing is in
//       flight when the caller can release the EC lock" -- section 8.4's quiesce
//       guarantee. See the note at the assertion site: this criterion is the
//       ONLY proof of that property until issue #21 lands a functional test that
//       can construct the race.
//
//       N.B. forcing the count necessarily also removes the outcome
//       propagation, so this mutation is compound. Criterion (o) isolates the
//       outcome half; (l) is retained for the flush half because nothing else
//       covers it.
//   (m) In XdpCpuMapDeepCopyAllocate, collapse the whole descriptor-selection
//       while-loop -- including its free/decrement body -- to a plain pop, so a
//       dirty CACHED descriptor is reused instead of freed. The operation
//       removed is "a descriptor we cannot prove clean is discarded, not
//       repaired": the previous packet's metadata then survives into the next
//       copy through the cache. Disabling only the cleanliness guard does NOT
//       compile: it leaves the free branch unreachable and /WX rejects it.
//   (n) In XdpCpuMapDeepCopyAllocate, delete the rule-2 refusal loop outright.
//       The operation removed is "a source carrying a slot outside the carried
//       set is refused rather than copied": the redirect then proceeds and
//       delivers a packet whose un-carried metadata was silently dropped. This
//       is the only coverage of the carry/refuse policy -- see the note on case
//       3 of DeepCopyFailurePaths.
//   (o) In XdpCpuMapCommitRedirect, delete the "if (Enqueued == 0) return
//       XdpCpuMapCommitDeclined;" guard. The operation removed is "the commit
//       result reports the real enqueue outcome": every failure then reports a
//       successful deep-copy redirect, and recv.c suppresses the
//       DropProgramInspection record for a packet that was actually lost.
//   (p) In XdpCpuMapDeepCopyAllocate, delete the fresh-descriptor cleanliness
//       branch. The operation removed is "a freshly allocated descriptor is
//       checked too, and a dirty one is a counted ONE-SHOT failure rather than a
//       retry": allocator residue is then carried into the copy. The recycled
//       half of the same check is criterion (m); this is the fresh half, which
//       is unreachable without the harness injecting residue.
//   (q) In XdpCpuMapDeepCopyAllocate, add a DeepCopyDescriptorResidue increment
//       to the RECYCLED-dirty discard. Unlike every other criterion here this is
//       an addition, because the invariant it covers is a NEGATIVE one: a
//       discarded recycled descriptor costs an allocation, not a packet, so no
//       counter may move for it. Deleting something cannot break a claim that
//       nothing happens, so the mutation has to be the thing the claim forbids.
//       Without it, the two residue paths could be merged back onto one counter
//       and nothing would notice -- which is the same "one counter, two jobs"
//       defect the fresh/source split already had to correct once.//
// Measured radii live in the increment-8 report, NOT here: embedded numbers go
// stale every round and turned the audit into four separate passes. What the
// source carries is exactly one current, unambiguous OPERATION per criterion.
// Every criterion is re-run in
// isolation after any change to the copy path, because a criterion validated
// against superseded code is reported as evidence while proving nothing.
//
#define XDPCPUMAP_TEST_DEEPCOPY_PAYLOAD_LEN 137u

static
VOID
XdpCpuMapTestDeepCopySuccess(
    VOID
    )
{
    UCHAR Payload[XDPCPUMAP_TEST_DEEPCOPY_PAYLOAD_LEN];
    NET_BUFFER_LIST *Original;
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_DEEPCOPY_POOL Pool;
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect;
    XDP_CPUMAP_HELPER_STATS Stats;
    XDPCPUMAP_TEST_NBL_DISPOSITION Disposition;
    NET_BUFFER_LIST *Copy;
    LONG Baseline;
    ULONG Index;

    XDPCPUMAP_TEST_BEGIN("DeepCopySuccess");

    XdpCpuMapTestResetNdisPool();

    for (Index = 0; Index < sizeof(Payload); Index++) {
        Payload[Index] = (UCHAR)(Index * 7 + 3);
    }

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapDeepCopyPoolInitialize(&Pool, (NDIS_HANDLE)(ULONG_PTR)0xF117A)));
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblPoolLive == 1);

    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitDeepCopyGroup(&CommitGroup, &NblRundown, &Pool);

    Original = XdpCpuMapTestCreateSourceNbl(Payload, sizeof(Payload));

    //
    // Every copied slot gets a distinct non-zero value, so a copy that carried
    // only some of them is caught by identity rather than by a zero check an
    // uninitialised slot would also pass. The pointer-owned slots are already
    // populated with real allocations by XdpCpuMapTestCreateSourceNbl.
    //
    for (ULONG Slot = 0; Slot < MaxNetBufferListInfo; Slot++) {
        if (XdpCpuMapTestIsCarriedSlot(Slot)) {
            Original->NetBufferListInfo[Slot] = (VOID *)(ULONG_PTR)(0xAB0000 + Slot);
        }
    }

    //
    // A sentinel successor, so a build that consumed the original into a chain
    // is visible rather than merely suspected.
    //
    NET_BUFFER_LIST_NEXT_NBL(Original) = (NET_BUFFER_LIST *)(ULONG_PTR)0xD0D0D0D0;

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);

    //
    // FALSE, and that is the contract: on the low-resource path FALSE means
    // "the caller still owns this NBL", not "the commit was rejected". The batch
    // entry below proves ownership WAS committed.
    //
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapCommitRedirect(&Redirect, Original, FALSE, FALSE, &CommitGroup) ==
                XdpCpuMapCommitDeepCopied);

    //
    // Section 8.4: the batch is ALREADY FLUSHED when commit returns on this
    // path. A batch entry is invisible to the quiesce scan, and the caller goes
    // on to call XdpGenericReceiveLowResources, which releases and reacquires
    // the EC spinlock -- a pause landing in that window would scan, find
    // nothing, and complete, and a deferred flush would then enqueue behind it.
    //
    // Asserting Count == 0 AND a populated ring is the observable form of
    // "nothing is in flight when the lock can be dropped".
    //
    // THESE TWO LINES AND CRITERION (l) ARE THE ONLY PROOF OF THAT PROPERTY.
    //
    // The functional pause-race test does NOT cover it and says so:
    // XdpGenericReceiveLowResources releases the EC lock only when the pass list
    // is non-empty, so the window needs a [PASS, REDIRECT] chain in one receive
    // call, which needs a selectively-redirecting BPF program that does not
    // exist yet. That work is issue #21 in increment 9. Until it lands, deleting
    // or weakening these assertions removes the last thing standing between a
    // deferred flush and a pause that silently depends on a target DPC.
    //
    XDPCPUMAP_TEST_ASSERT(CommitGroup.Count == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail - Value.Target->Ring->Head == 1);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

    //
    // The ring holds a copy, not the original.
    //
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail - Value.Target->Ring->Head == 1);
    {
        const XDP_CPUMAP_ENTRY *Slot = &Value.Target->Ring->Entries[0];

        Copy = Slot->Nbl;
        XDPCPUMAP_TEST_ASSERT(Copy != NULL);
        XDPCPUMAP_TEST_ASSERT(Copy != Original);
        XDPCPUMAP_TEST_ASSERT(Slot->IsDeepCopy);
        XDPCPUMAP_TEST_ASSERT(Slot->DeepCopyPool == &Pool);
        XDPCPUMAP_TEST_ASSERT(Slot->NblRundown == &NblRundown);
    }

    //
    // Bytes and receive metadata match, checked against the ORIGINAL's values
    // rather than against constants the test also wrote into the copy.
    //
    // Asserted over XdpCpuMapTestReceiveInfoSlots -- the policy -- not over a
    // list restated here. An enumerated list is how VLAN, frame type and
    // filtering information went missing while a criterion still claimed
    // "preserved OOB travels with the copy".
    //
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestNblPayloadEquals(Copy, Payload, sizeof(Payload)));
    //
    // Carried slots match; nothing else is set.
    //
    // Asserted over the WHOLE array rather than over the carried list alone,
    // because the defect this guards is precisely that an uncarried slot keeps
    // whatever was in the descriptor -- allocator residue on a fresh one, the
    // previous packet's metadata on a recycled one.
    //
    for (ULONG Slot = 0; Slot < MaxNetBufferListInfo; Slot++) {
        if (XdpCpuMapTestIsCarriedSlot(Slot)) {
            XDPCPUMAP_TEST_ASSERT(
                Copy->NetBufferListInfo[Slot] == Original->NetBufferListInfo[Slot]);
        } else {
            XDPCPUMAP_TEST_ASSERT(Copy->NetBufferListInfo[Slot] == NULL);
        }
    }

    //
    // No NDIS-managed reference was taken. A build that went back to
    // NdisCopyReceiveNetBufferListInfo would acquire a WFP context reference it
    // has no way to release from a cached descriptor.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestWfpReferences == 0);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyMetadataUnsupported == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyDescriptorResidue == 0);

    //
    // An originated receive NBL must name its originating filter, or NDIS has
    // nowhere to route the return.
    //
    XDPCPUMAP_TEST_ASSERT(Copy->SourceHandle == (NDIS_HANDLE)(ULONG_PTR)0xF117A);

    //
    // One descriptor allocated and one page outstanding while the copy is queued.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblLive == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 1);
    XDPCPUMAP_TEST_ASSERT(Pool.CacheCount == 1);

    //
    // The original is exactly where the caller left it.
    //
    XDPCPUMAP_TEST_ASSERT(
        NET_BUFFER_LIST_NEXT_NBL(Original) == (NET_BUFFER_LIST *)(ULONG_PTR)0xD0D0D0D0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 0);

    XdpCpuMapTestRunQueuedDpcs();

    //
    // The copy was indicated once, WITH RESOURCES, and never returned to the
    // miniport. The original was neither indicated nor returned by CPUMAP.
    //
    Disposition = XdpCpuMapTestNblDisposition(Copy);
    XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 1);
    XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 0);
    XDPCPUMAP_TEST_ASSERT(
        (Disposition.IndicationFlags & NDIS_RECEIVE_FLAGS_RESOURCES) != 0);
    XDPCPUMAP_TEST_ASSERT(
        (Disposition.IndicationFlags & NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL) != 0);

    Disposition = XdpCpuMapTestNblDisposition(Original);
    XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);
    XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 0);

    //
    // Recycled before the DPC returned: the pages are back, the descriptor is
    // still cached rather than freed, and no new descriptor was allocated.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblLive == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblAllocTotal == 1);

    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyBuildCount == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyFailCount == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueCount == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.DrainCount == 1);

    XdpCpuMapTestDeleteSourceNbl(Original);
    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);

    XdpCpuMapDeepCopyPoolCleanup(&Pool);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblPoolLive == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblLive == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);
    XdpCpuMapStop();
}

//
// Every row 9b failure path, and the one that leaks if unhandled.
//
// Section 8.1a row 9b names three ways preparing a copy can fail and one way the
// ring can refuse a finished one. All four are POST-commit: the original is
// already on the caller's DropList and the batch entry already holds a backing
// reference, an NblRundown credit and a target rundown reference, so each has to
// release explicitly rather than fall through to a pre-commit path that no
// longer exists.
//
// The retreat-succeeded-then-failed case is called out in the design as the one
// that leaks if unhandled, because the descriptor has pages attached by then and
// caching it as-is would strand them. The harness models pages as real
// allocations precisely so that "leaked" is observable rather than argued.
//
// Deletion criteria:
//
//   (d) In XdpCpuMapDeepCopyAllocate, replace the XdpCpuMapDeepCopyRecycle on
//       the MDL-copy failure path with XdpCpuMapDeepCopyPushFree. The operation
//       removed is "a partially built copy has its data start advanced back
//       before it is cached": the descriptor still returns to the free list, so
//       descriptor accounting is unchanged and only the live-PAGE assertion
//       fails. This is the exact substitution the design warns about.
//
//   (e) In the flush pre-pass, drop the three release calls on the failure path.
//       The operation removed is "a failed entry releases the references commit
//       gave it"; the packet is still lost and still counted, so only the
//       reference-balance assertions fail.
//   (f) In the flush's rejection branch, chain a deep copy into
//       Group->RejectedNbls instead of the recycle list. The operation removed
//       is "a rejected copy goes back to its pool, not to the miniport": the
//       copy is handed to the caller for DropList, which would return a
//       pool-owned buffer to a miniport that never owned it.
//
//
static
VOID
XdpCpuMapTestDeepCopyFailurePaths(
    VOID
    )
{
    UCHAR Payload[XDPCPUMAP_TEST_DEEPCOPY_PAYLOAD_LEN];
    NET_BUFFER_LIST *Original;
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_DEEPCOPY_POOL Pool;
    XDP_CPUMAP_DEEPCOPY_POOL FreshPool;
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect;
    XDP_CPUMAP_HELPER_STATS Stats;
    LONG Baseline;
    UINT32 AllocsBefore;
    UINT32 NblLiveBefore;
    UINT32 Case;

    XDPCPUMAP_TEST_BEGIN("DeepCopyFailurePaths");

    XdpCpuMapTestResetNdisPool();
    for (UINT32 Index = 0; Index < sizeof(Payload); Index++) {
        Payload[Index] = (UCHAR)(Index ^ 0x5A);
    }

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapDeepCopyPoolInitialize(&Pool, (NDIS_HANDLE)(ULONG_PTR)0xF117A)));
    ExInitializeRundownProtection(&NblRundown);

    //
    // Case 0: no descriptor available.        Case 1: retreat fails.
    // Case 2: the copy fails AFTER the retreat succeeded -- the leaking case.
    //
    for (Case = 0; Case < 3; Case++) {
        XdpCpuMapTestFailNblAllocAfter = (Case == 0) ? 0 : -1;
        XdpCpuMapTestFailRetreatAfter = (Case == 1) ? 0 : -1;
        XdpCpuMapTestFailMdlCopyAfter = (Case == 2) ? 0 : -1;

        XdpCpuMapTestInitDeepCopyGroup(&CommitGroup, &NblRundown, &Pool);
        Original = XdpCpuMapTestCreateSourceNbl(Payload, sizeof(Payload));
        NET_BUFFER_LIST_NEXT_NBL(Original) = (NET_BUFFER_LIST *)(ULONG_PTR)0xD0D0D0D0;

        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
        XdpCpuMapReferenceBacking(CpuMap);
        Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);

        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapCommitRedirect(&Redirect, Original, FALSE, FALSE, &CommitGroup) ==
                XdpCpuMapCommitDeclined);

        //
        // Flushed inline, so the failure has already been taken and accounted
        // for by the time commit returns.
        //
        XDPCPUMAP_TEST_ASSERT(CommitGroup.Count == 0);

        //
        // Nothing is handed back to the caller: the original was never taken, so
        // there is no rejected chain to return.
        //
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        //
        // Lost, counted, and not queued.
        //
        XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail == Value.Target->Ring->Head);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 0);
        XDPCPUMAP_TEST_ASSERT(
            NET_BUFFER_LIST_NEXT_NBL(Original) == (NET_BUFFER_LIST *)(ULONG_PTR)0xD0D0D0D0);

        //
        // Exactly what row 9b says the entry releases: the backing reference,
        // the NblRundown credit and the target rundown reference.
        //
        XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
        XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
        XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);

        //
        // And no leak. Case 2 is the one that would strand a page: the retreat
        // succeeded, so the descriptor had pages attached when the copy failed.
        //
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);

        XdpCpuMapTestDeleteSourceNbl(Original);
    }

    XdpCpuMapTestFailNblAllocAfter = -1;
    XdpCpuMapTestFailRetreatAfter = -1;
    XdpCpuMapTestFailMdlCopyAfter = -1;

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyFailCount == 3);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyBuildCount == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueCount == 0);

    //
    // Case 3: the source carries metadata we cannot carry, so the redirect is
    // REFUSED rather than delivered damaged.
    //
    // This is the ONLY coverage of the refuse half of the carry/refuse policy.
    // If it is deleted or weakened, nothing detects a build that silently
    // delivers a packet holding a pointer into storage the miniport has already
    // reclaimed. The carry half has its own criterion -- deleting the carry loop
    // fails six assertions in DeepCopySuccess, one per carried slot.
    //
    // N.B. an earlier revision claimed the carry loop's criterion was VACUOUS,
    // on the grounds that the refusal guarantees every non-carried slot is NULL
    // before the carry runs, so hand-carrying and calling
    // NdisCopyReceiveNetBufferListInfo produce an identical copy. That
    // equivalence is real but proves nothing: a deletion criterion DELETES the
    // operation, it does not swap it for an equivalent one. Deleting the carry
    // loses all six values and is detected immediately.
    //
    // Refusal, not assertion: a conforming miniport supplying media-specific
    // information is valid input, and a checked build must not bugcheck on it.
    //
    XdpCpuMapTestSourceMetadataBlobs = TRUE;

    XdpCpuMapTestInitDeepCopyGroup(&CommitGroup, &NblRundown, &Pool);
    Original = XdpCpuMapTestCreateSourceNbl(Payload, sizeof(Payload));
    NET_BUFFER_LIST_NEXT_NBL(Original) = (NET_BUFFER_LIST *)(ULONG_PTR)0xD0D0D0D0;

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);

    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapCommitRedirect(&Redirect, Original, FALSE, FALSE, &CommitGroup) ==
            XdpCpuMapCommitDeclined);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

    //
    // Nothing queued, nothing indicated, the original untouched, and the loss
    // counted under its own reason rather than folded into allocation failure.
    //
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail == Value.Target->Ring->Head);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 0);
    XDPCPUMAP_TEST_ASSERT(
        NET_BUFFER_LIST_NEXT_NBL(Original) == (NET_BUFFER_LIST *)(ULONG_PTR)0xD0D0D0D0);

    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyMetadataUnsupported == 1);
    //
    // The SOURCE reason, not the descriptor one: the two imply different
    // responses, so a test that could not tell them apart would let either
    // fire for the other's reason.
    //
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyDescriptorResidue == 0);

    //
    // And the PARTITION claim: a reason counter never fires alone. Every
    // copy-preparation failure increments the aggregate exactly once, whatever
    // its reason, which is what makes the aggregate the only figure packet
    // accounting may sum. Pinning the reasons against each other does not check
    // that -- three earlier cases plus this one is four.
    //
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyFailCount == 4);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyBuildCount == 0);

    XdpCpuMapTestDeleteSourceNbl(Original);
    XdpCpuMapTestSourceMetadataBlobs = FALSE;

    //
    // Case 3: the ring refuses a finished copy. Row 9b "(ii)": the copy goes back
    // to its pool, and NOT into RejectedNbls, which the caller would append to
    // DropList and return to a miniport that never owned it.
    //
    // Deactivating the target is the same lever EnqueueTargetInactive uses: it
    // is what the sweep publishes under ConfigLock before it waits, so the
    // under-ring-lock re-check in step 2 refuses the entry.
    //
    Value.Target->Active = FALSE;

    XdpCpuMapTestInitDeepCopyGroup(&CommitGroup, &NblRundown, &Pool);
    Original = XdpCpuMapTestCreateSourceNbl(Payload, sizeof(Payload));

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);

    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapCommitRedirect(&Redirect, Original, FALSE, FALSE, &CommitGroup) ==
                XdpCpuMapCommitDeclined);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueTargetInactive == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyBuildCount == 1);

    XdpCpuMapTestDeleteSourceNbl(Original);

    //
    // Case 5: a FRESH descriptor arrives dirty.
    //
    // The uniform cleanliness check applies to freshly allocated descriptors as
    // well as recycled ones, because NDIS does NOT document that
    // NdisAllocateNetBufferAndNetBufferList zeroes NetBufferListInfo -- it
    // documents initialization only for specific fields such as Scratch. Every
    // other case here reaches only the RECYCLED half of that check. Without
    // residue injection the fresh half never executes at all, and an invariant
    // that never executes can only be argued about.
    //
    // A DEDICATED pool is required. A shared one holds cached descriptors, and a
    // clean cached descriptor short-circuits to Found before the allocator is
    // ever reached, so the fresh branch is unreachable on a warm pool. That is
    // why injecting residue into a shared-pool case appears to do nothing.
    //
    // The assertions are the whole rule, not just the outcome: exactly ONE
    // allocation and no retry -- retrying would spin at DISPATCH_LEVEL for as
    // long as the allocator kept returning residue -- the descriptor FREED
    // rather than repaired or cached, the loss counted under its own reason, and
    // no pages or references left behind.
    //
    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(
            XdpCpuMapDeepCopyPoolInitialize(&FreshPool, (NDIS_HANDLE)(ULONG_PTR)0xF117B)));

    //
    // Reactivate the target. The previous case left it inactive, and leaving it
    // that way would make this case decline via EnqueueTargetInactive whether or
    // not the fresh-dirty branch exists -- passing for the wrong reason. The
    // EnqueueTargetInactive assertion below pins that down: it must stay at 1,
    // proving the decline came from the DESCRIPTOR-RESIDUE check and not the
    // ring.
    //
    Value.Target->Active = TRUE;

    AllocsBefore = (UINT32)XdpCpuMapTestNblAllocTotal;
    NblLiveBefore = (UINT32)XdpCpuMapTestNblLive;

    XdpCpuMapTestDirtyFreshAlloc = TRUE;

    XdpCpuMapTestInitDeepCopyGroup(&CommitGroup, &NblRundown, &FreshPool);
    Original = XdpCpuMapTestCreateSourceNbl(Payload, sizeof(Payload));

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);

    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapCommitRedirect(&Redirect, Original, FALSE, FALSE, &CommitGroup) ==
            XdpCpuMapCommitDeclined);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

    XdpCpuMapTestDirtyFreshAlloc = FALSE;

    XDPCPUMAP_TEST_ASSERT((UINT32)XdpCpuMapTestNblAllocTotal == AllocsBefore + 1);
    XDPCPUMAP_TEST_ASSERT((UINT32)XdpCpuMapTestNblLive == NblLiveBefore);
    XDPCPUMAP_TEST_ASSERT(FreshPool.CacheCount == 0);

    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail == Value.Target->Ring->Head);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyDescriptorResidue == 1);
    //
    // And NOT the source reason: this decline came from allocator residue,
    // which says nothing about the carried set.
    //
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyMetadataUnsupported == 1);

    //
    // The partition again: this failure also lands on the aggregate, making
    // five copy-preparation failures so far.
    //
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyFailCount == 5);
    //
    // The decline came from the DESCRIPTOR-RESIDUE check, and not from the ring:
    // the target was reactivated above, so EnqueueTargetInactive must not have
    // moved and no copy was built. Without this pin the case would pass on the
    // previous case's inactive target -- which is exactly how its first version
    // passed for the wrong reason.
    //
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueTargetInactive == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyBuildCount == 1);

    XdpCpuMapTestDeleteSourceNbl(Original);
    XdpCpuMapDeepCopyPoolCleanup(&FreshPool);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);

    XdpCpuMapDeepCopyPoolCleanup(&Pool);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblLive == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);
    XdpCpuMapStop();
}

//
// Teardown: a deep copy goes back to its POOL, and the miniport hears nothing.
//
// Section 8.1a row 9b, "Released -- teardown". Quiesce tombstoning and the
// retire drain both reach XdpCpuMapChainSetReturn, whose name is now only half
// right: an original is returned to the miniport, a copy is recycled. Handing a
// copy to NdisFReturnNetBufferLists would give the miniport a buffer allocated
// from our own pool, which never belonged to it.
//
// Increment 8 left this only INCIDENTALLY exercised: the deep-copy tests drained
// through the DPC, and the teardown tests carried originals, so no assertion
// anywhere said "zero miniport returns" for a copy. A build that returned the
// copy to the miniport AND recycled it would have passed every one of them.
//
// Deletion criterion (j): in XdpCpuMapChainSetReturn, drop the IsDeepCopy branch
// so every partition goes to NdisFReturnNetBufferLists. The operation removed is
// "teardown recycles copies instead of returning them". Descriptor and page
// accounting still balance -- the recycle is gone, so nothing double-frees --
// and only the return-count and disposition assertions here can see it.
//
static
VOID
XdpCpuMapTestDeepCopyTeardownRecycle(
    VOID
    )
{
    UCHAR Payload[96];
    NET_BUFFER_LIST *Originals[3];
    NET_BUFFER_LIST *Copies[3];
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_DEEPCOPY_POOL Pool;
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect;
    LONG Baseline;
    UINT32 Index;

    XDPCPUMAP_TEST_BEGIN("DeepCopyTeardownRecycle");

    XdpCpuMapTestResetNdisPool();
    for (Index = 0; Index < sizeof(Payload); Index++) {
        Payload[Index] = (UCHAR)(Index + 11);
    }

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapDeepCopyPoolInitialize(&Pool, (NDIS_HANDLE)(ULONG_PTR)0xF117A)));
    ExInitializeRundownProtection(&NblRundown);

    //
    // Cancel the drain so teardown owns disposal rather than the DPC.
    //
    XdpCpuMapTestRemoveQueueDpc(Value.Target->Dpc);

    for (Index = 0; Index < RTL_NUMBER_OF(Originals); Index++) {
        XdpCpuMapTestInitDeepCopyGroup(&CommitGroup, &NblRundown, &Pool);
        Originals[Index] = XdpCpuMapTestCreateSourceNbl(Payload, sizeof(Payload));

        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
        XdpCpuMapReferenceBacking(CpuMap);
        Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapCommitRedirect(
                &Redirect, Originals[Index], FALSE, FALSE, &CommitGroup) ==
                    XdpCpuMapCommitDeepCopied);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        Copies[Index] = Value.Target->Ring->Entries[Index & Value.Target->Ring->Mask].Nbl;
        XDPCPUMAP_TEST_ASSERT(Copies[Index] != NULL);
        XDPCPUMAP_TEST_ASSERT(Copies[Index] != Originals[Index]);
    }

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == (LONG)RTL_NUMBER_OF(Copies));
    XDPCPUMAP_TEST_ASSERT(Pool.CacheCount == RTL_NUMBER_OF(Copies));

    //
    // Retire with the ring occupied. Section 8.3, "Delivery on retire".
    //
    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();

    //
    // The claim, and the one no previous test made: NOTHING went to the
    // miniport. Not counted-as-zero-returns-of-originals, but zero return CALLS
    // at all, because on this path there is no original to return -- each went
    // home through the caller's DropList at commit time.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);

    for (Index = 0; Index < RTL_NUMBER_OF(Copies); Index++) {
        XDPCPUMAP_TEST_NBL_DISPOSITION Disposition;

        Disposition = XdpCpuMapTestNblDisposition(Copies[Index]);
        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);
        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 0);

        Disposition = XdpCpuMapTestNblDisposition(Originals[Index]);
        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);
        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 0);
    }

    //
    // Recycled, not leaked and not freed: the pages are back, the descriptors
    // are still cached, and the pool never grew past what it built.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblLive == (LONG)RTL_NUMBER_OF(Copies));
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblAllocTotal == (LONG)RTL_NUMBER_OF(Copies));

    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    for (Index = 0; Index < RTL_NUMBER_OF(Originals); Index++) {
        XdpCpuMapTestDeleteSourceNbl(Originals[Index]);
    }

    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);

    XdpCpuMapDeepCopyPoolCleanup(&Pool);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblLive == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);
    XdpCpuMapStop();
}

//
// The cache: descriptors are reused, growth is lazy and bounded, and reaching
// the cap is a counted drop rather than an error.
// Deletion criteria:
//
//   (g) In XdpCpuMapDeepCopyAllocate, remove the
//       Pool->CacheCount < XDP_CPUMAP_DEEPCOPY_CACHE_MAX guard. The operation
//       removed is the cap itself: allocation continues past it, so the
//       exhaustion assertions fail and nothing bounds the pool.
//
//   (h) In XdpCpuMapDeepCopyAllocate, skip the InterlockedFlushSList refill of
//       the local list. The operation removed is "recycled descriptors are
//       reused": every copy then allocates a new descriptor, so the
//       allocation-total and reuse assertions fail while packet counts stay
//       identical.
//
static
VOID
XdpCpuMapTestDeepCopyCacheReuseAndCap(
    VOID
    )
{
    UCHAR Payload[64];
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_DEEPCOPY_POOL Pool;
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect;
    XDP_CPUMAP_HELPER_STATS Stats;
    NET_BUFFER_LIST *Original;
    ULONG64 FirstCopyTag = 0;
    LONG Baseline;
    UINT32 Index;

    XDPCPUMAP_TEST_BEGIN("DeepCopyCacheReuseAndCap");

    XdpCpuMapTestResetNdisPool();
    RtlZeroMemory(Payload, sizeof(Payload));

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapDeepCopyPoolInitialize(&Pool, (NDIS_HANDLE)(ULONG_PTR)0xF117A)));
    ExInitializeRundownProtection(&NblRundown);

    //
    // Eight sequential low-resource redirects, each drained before the next.
    // Every one after the first must reuse the descriptor the previous recycled,
    // so the pool grows to exactly one.
    //
    for (Index = 0; Index < 8; Index++) {
        XdpCpuMapTestInitDeepCopyGroup(&CommitGroup, &NblRundown, &Pool);
        Original = XdpCpuMapTestCreateSourceNbl(Payload, sizeof(Payload));

        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
        XdpCpuMapReferenceBacking(CpuMap);
        Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapCommitRedirect(&Redirect, Original, FALSE, FALSE, &CommitGroup) ==
                XdpCpuMapCommitDeepCopied);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        if (Index == 0) {
            FirstCopyTag = Value.Target->Ring->Entries[0].Nbl->TestTag;
        } else {
            //
            // Identity, not just a count: the SAME descriptor came back.
            //
            XDPCPUMAP_TEST_ASSERT(
                Value.Target->Ring->Entries[Index & Value.Target->Ring->Mask].Nbl->TestTag ==
                    FirstCopyTag);
        }

        //
        // And it came back CLEAN. The indication stub stamps upper-stack
        // metadata into the slots NDIS does not copy, so a descriptor reused
        // without zeroing carries the previous packet's values into this one --
        // a cross-packet data leak through the cache.
        //
        {
            const NET_BUFFER_LIST *Reused =
                Value.Target->Ring->Entries[Index & Value.Target->Ring->Mask].Nbl;

            XDPCPUMAP_TEST_ASSERT(
                Reused->NetBufferListInfo[ClassificationHandleNetBufferListInfo] == NULL);
            XDPCPUMAP_TEST_ASSERT(
                Reused->NetBufferListInfo[NetBufferListCancelId] == NULL);
        }

        XdpCpuMapTestRunQueuedDpcs();
        XdpCpuMapTestDeleteSourceNbl(Original);
    }

    XDPCPUMAP_TEST_ASSERT(Pool.CacheCount == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblAllocTotal == 1);

    //
    // Now the other half of the rule: a descriptor the upper stack STAMPED is
    // discarded rather than reused.
    //
    // Slots CPUMAP does not carry are not ours to clear -- the array holds
    // entries with mixed ownership and blanket-clearing one NDIS
    // reference-manages would bypass its handling. So a dirty descriptor is
    // freed and a fresh one allocated: the cache degrades to slower and correct
    // instead of fast and carrying the previous packet's metadata.
    //
    XdpCpuMapTestStampOnIndicate = TRUE;

    for (Index = 0; Index < 3; Index++) {
        UINT32 AllocsBefore = (UINT32)XdpCpuMapTestNblAllocTotal;

        XdpCpuMapTestInitDeepCopyGroup(&CommitGroup, &NblRundown, &Pool);
        Original = XdpCpuMapTestCreateSourceNbl(Payload, sizeof(Payload));

        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
        XdpCpuMapReferenceBacking(CpuMap);
        Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapCommitRedirect(&Redirect, Original, FALSE, FALSE, &CommitGroup) ==
                XdpCpuMapCommitDeepCopied);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        //
        // The copy handed to the ring is clean in every slot CPUMAP does not
        // carry, whatever the previous packet's consumer left behind.
        //
        {
            const NET_BUFFER_LIST *Fresh =
                Value.Target->Ring->Entries[
                    (Index + 8) & Value.Target->Ring->Mask].Nbl;

            for (ULONG Slot = 0; Slot < MaxNetBufferListInfo; Slot++) {
                if (!XdpCpuMapTestIsCarriedSlot(Slot)) {
                    XDPCPUMAP_TEST_ASSERT(Fresh->NetBufferListInfo[Slot] == NULL);
                }
            }
        }

        XdpCpuMapTestRunQueuedDpcs();
        XdpCpuMapTestDeleteSourceNbl(Original);

        //
        // After the first, every iteration must ALLOCATE: the descriptor it
        // would otherwise have reused was stamped and therefore discarded.
        //
        if (Index > 0) {
            XDPCPUMAP_TEST_ASSERT((UINT32)XdpCpuMapTestNblAllocTotal == AllocsBefore + 1);
        }
    }

    //
    // Discarded descriptors are FREED, not leaked: the cache never holds more
    // than the one live copy in flight.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);
    XdpCpuMapTestStampOnIndicate = FALSE;
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblLive == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    //
    // Eight reused copies plus three the stamped-discard phase built.
    //
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyBuildCount == 11);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyFailCount == 0);

    //
    // The stamped-discard phase freed three dirty RECYCLED descriptors, and no
    // counter moved for any of them. That is the design's position, not an
    // oversight: a discarded recycled descriptor costs an allocation, not a
    // packet, so it is not a loss and does not belong in DeepCopyFailCount.
    // DeepCopyDescriptorResidue is FRESH-only and must stay at zero here --
    // asserting it is what stops the two residue paths being merged back into
    // one counter, which is the defect issue #22 has to reason about.
    //
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyDescriptorResidue == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyMetadataUnsupported == 0);

    //
    // Now the cap. Drive the pool to XDP_CPUMAP_DEEPCOPY_CACHE_MAX outstanding
    // copies by never draining, then take one more: the next allocation must be
    // refused and counted rather than growing the pool.
    //
    // The ring is left at the map's established depth, which is
    // XDP_CPUMAP_RING_DEPTH_DEFAULT and already well above the cache cap, so
    // ring-full cannot absorb the packet first and make this a test of the wrong
    // limit. Asking for a different depth here would be rejected anyway: section
    // 5.3 fixes the setting on first write.
    //
    {
        XDP_CPUMAP_PROVIDER_VALUE BigValue;

        C_ASSERT(XDP_CPUMAP_RING_DEPTH_DEFAULT > XDP_CPUMAP_DEEPCOPY_CACHE_MAX);

        Entry = XdpCpuMapTestEntry(1, 0, 0);
        XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &BigValue)));
        XdpCpuMapTestRemoveQueueDpc(BigValue.Target->Dpc);

        for (Index = 0; Index < XDP_CPUMAP_DEEPCOPY_CACHE_MAX; Index++) {
            XdpCpuMapTestInitDeepCopyGroup(&CommitGroup, &NblRundown, &Pool);
            Original = XdpCpuMapTestCreateSourceNbl(Payload, sizeof(Payload));

            XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, BigValue.Target));
            XdpCpuMapReferenceBacking(CpuMap);
            Redirect = XdpCpuMapTestFrameRedirect(CpuMap, BigValue.Target, 0);
            XDPCPUMAP_TEST_ASSERT(
                XdpCpuMapCommitRedirect(&Redirect, Original, FALSE, FALSE, &CommitGroup) ==
                XdpCpuMapCommitDeepCopied);
            XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
            XdpCpuMapTestDeleteSourceNbl(Original);
        }

        XDPCPUMAP_TEST_ASSERT(Pool.CacheCount == XDP_CPUMAP_DEEPCOPY_CACHE_MAX);

        //
        // One past the cap.
        //
        XdpCpuMapTestInitDeepCopyGroup(&CommitGroup, &NblRundown, &Pool);
        Original = XdpCpuMapTestCreateSourceNbl(Payload, sizeof(Payload));

        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, BigValue.Target));
        XdpCpuMapReferenceBacking(CpuMap);
        Redirect = XdpCpuMapTestFrameRedirect(CpuMap, BigValue.Target, 0);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapCommitRedirect(&Redirect, Original, FALSE, FALSE, &CommitGroup) ==
                XdpCpuMapCommitDeclined);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        //
        // Refused, counted, non-fatal: the pool did not grow, the ring did not
        // take the packet, and the run continues.
        //
        XDPCPUMAP_TEST_ASSERT(Pool.CacheCount == XDP_CPUMAP_DEEPCOPY_CACHE_MAX);
        XDPCPUMAP_TEST_ASSERT(
            BigValue.Target->Ring->Tail - BigValue.Target->Ring->Head ==
                XDP_CPUMAP_DEEPCOPY_CACHE_MAX);

        Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
        XDPCPUMAP_TEST_ASSERT(Stats.DeepCopyFailCount == 1);
        //
        // Eight from the reuse phase, three from the stamped discard phase,
        // then a full cache's worth here.
        //
        XDPCPUMAP_TEST_ASSERT(
            Stats.DeepCopyBuildCount == 11 + XDP_CPUMAP_DEEPCOPY_CACHE_MAX);

        XdpCpuMapTestDeleteSourceNbl(Original);
        XdpCpuMapTestRelease(CpuMap, &BigValue);
        XdpCpuMapTestDrainSweeps();
    }

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);

    XdpCpuMapDeepCopyPoolCleanup(&Pool);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblLive == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);
    XdpCpuMapStop();
}


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
// The DPC is affinitized to the CPU the map entry names.
//
// This is the driver-side mechanism that the functional test
// GenericRxEbpfCpuMapRedirect observes end to end: the packet is indicated on
// the target CPU because the drain DPC runs there. That test needs hardware, so
// the mechanism is pinned here as well, where it can be mutated.
//
// It asserts across SEVERAL DISTINCT CPUs on purpose. The pre-existing check in
// DpcTargetingFailure asserts Target.Number == 0 for a target configured at CPU
// 0, which cannot distinguish "affinitized to the configured CPU" from
// "affinitized to CPU 0" or from a zero-initialized field -- it varies nothing.
//
// Deletion criterion: in XdpCpuMapCreateTarget, target the DPC at a fixed
// processor number rather than the one resolved for AbsoluteCpu. Affinity stops
// tracking configuration and the per-CPU assertions here fail.
//
static
VOID
XdpCpuMapTestTargetDpcAffinity(
    VOID
    )
{
    static const UINT32 Cpus[] = {1, 3, 5};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Values[RTL_NUMBER_OF(Cpus)];
    XDP_CPUMAP_ENTRY_V1 Entry;
    LONG Baseline;
    UINT32 Index;

    XDPCPUMAP_TEST_BEGIN("TargetDpcAffinity");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    for (Index = 0; Index < RTL_NUMBER_OF(Cpus); Index++) {
        C_ASSERT(RTL_NUMBER_OF(Cpus) <= XDP_CPUMAP_TEST_PROCESSOR_COUNT);

        Entry = XdpCpuMapTestEntry(Cpus[Index], 0, 0);
        XDPCPUMAP_TEST_ASSERT(
            NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Values[Index])));
        XDPCPUMAP_TEST_ASSERT(Values[Index].Target != NULL);
    }

    for (Index = 0; Index < RTL_NUMBER_OF(Cpus); Index++) {
        const XDP_CPUMAP_TARGET *Target = Values[Index].Target;

        //
        // The claim: the DPC's affinity is the CPU the entry named, for every
        // entry, not just for whichever CPU happens to be zero.
        //
        XDPCPUMAP_TEST_ASSERT(Target->AbsoluteCpu == Cpus[Index]);
        XDPCPUMAP_TEST_ASSERT(Target->Dpc->Targeted);
        XDPCPUMAP_TEST_ASSERT(Target->Dpc->Target.Group == 0);
        XDPCPUMAP_TEST_ASSERT(Target->Dpc->Target.Number == (UCHAR)Cpus[Index]);

        //
        // And the DPC carries its own target as the deferred context, which is
        // what makes the drain run against the right ring on that CPU.
        //
        XDPCPUMAP_TEST_ASSERT(Target->Dpc->Context == (VOID *)Target);
    }

    //
    // Distinct CPUs are distinct targets with distinct DPCs and rings. A single
    // shared DPC would satisfy the affinity assertions above for whichever CPU
    // was configured last.
    //
    XDPCPUMAP_TEST_ASSERT(Values[0].Target != Values[1].Target);
    XDPCPUMAP_TEST_ASSERT(Values[1].Target != Values[2].Target);
    XDPCPUMAP_TEST_ASSERT(Values[0].Target->Dpc != Values[1].Target->Dpc);
    XDPCPUMAP_TEST_ASSERT(Values[1].Target->Dpc != Values[2].Target->Dpc);
    XDPCPUMAP_TEST_ASSERT(Values[0].Target->Ring != Values[1].Target->Ring);
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == RTL_NUMBER_OF(Cpus));

    for (Index = 0; Index < RTL_NUMBER_OF(Cpus); Index++) {
        XdpCpuMapTestRelease(CpuMap, &Values[Index]);
    }

    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// Zero-copy ownership, end to end, asserted on NBL IDENTITY.
//
// Design section 8.1a row 9a: the original miniport NBL is taken at commit,
// placed in the ring slot, and released by NdisFIndicateReceiveNetBufferLists
// WITHOUT NDIS_RECEIVE_FLAGS_RESOURCES on the target CPU. Issue #17 proved the
// indication processor; it did not prove that the thing indicated is the same
// NBL, indicated once, and never returned. A count-based test passes while the
// NBL is duplicated across partitions or both indicated and returned.
//
// Two targets and two indication identities, interleaved, so the assertion also
// covers partitioning: every original must appear in exactly ONE indication, and
// in the one belonging to its own filter/port/queue.
//
// Deletion criteria, each naming the production operation removed. Radii below
// are from running all 37 cases in isolation, with each outcome typed as pass,
// assertion failure, crash or hang -- not from FAIL-line counts in a single
// whole-suite run, which cannot distinguish a crash from a pass.
//
//   (a) In the flush, write Slot->IsDeepCopy = TRUE instead of FALSE. The drain
//       derives the RESOURCES bit from that field, so the operation removed is
//       "indicate originals without RESOURCES" and the flag assertions fail.
//       Nothing else changes: the same NBLs are still indicated once each.
//       Radius: ZeroCopyOwnershipLifecycle 12, DrainPartitionedIndication 4,
//       ZeroCopyTeardownDisposal 2, DrainTombstoneSkip 1, DrainYieldRequeueGate
//       1, RetireDrainReturns 1; 31 pass, 0 crash, 0 hang.
//
//   (b) In XdpCpuMapChainSetTake, drop the
//       NET_BUFFER_LIST_NEXT_NBL(Entry->Nbl) = NULL that terminates the NBL
//       before appending it. The operation removed is "build a well-formed
//       chain from slot NBLs": the indicated chain then still carries whatever
//       Next the producer left in place, so the chain walked at indication time
//       does not match the count the drain computed.
//       Radius: ZeroCopyPostCommitRejection 167, ZeroCopyOwnershipLifecycle 8;
//       35 pass, 0 crash, 0 hang.
//
//   (c) In step 5 of the flush, acquire Target->PacketRundown before
//       KeInsertQueueDpc and never release it. The operation removed is "the
//       group borrows target rundown references and returns ALL of them at step
//       6, keeping none for the queued DPC" -- the producer reference an earlier
//       design revision proposed and could not say when to release. The mutated
//       code still enqueues, still drains and still indicates correctly, so only
//       the PacketRundown.Count assertions can see it. This one is NOT isolating
//       and is not meant to be: the same invariant is asserted by every test
//       that flushes, and it fails in eleven of them -- CommitGroupBatching 11,
//       EnqueueInsertOrdering 4, ZeroCopyOwnershipLifecycle 4,
//       DrainYieldRequeueGate 2, EnqueueRingFull 2,
//       ZeroCopyPostCommitRejection 2, ZeroCopyTeardownDisposal 2,
//       CommitFrameReuse 1, DrainPartitionedIndication 1, DrainTombstoneSkip 1,
//       RetireDrainReturns 1; 26 pass, 0 crash, 0 hang.
//
#define XDPCPUMAP_TEST_ZEROCOPY_NBLS 6u

static
VOID
XdpCpuMapTestZeroCopyOwnershipLifecycle(
    VOID
    )
{
    NET_BUFFER_LIST Nbls[XDPCPUMAP_TEST_ZEROCOPY_NBLS] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value0;
    XDP_CPUMAP_PROVIDER_VALUE Value1;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF RundownA;
    EX_RUNDOWN_REF RundownB;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_HELPER_STATS Stats;
    LONG Baseline;
    UINT32 Index;

    XDPCPUMAP_TEST_BEGIN("ZeroCopyOwnershipLifecycle");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value0)));
    Entry = XdpCpuMapTestEntry(1, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value1)));

    ExInitializeRundownProtection(&RundownA);
    ExInitializeRundownProtection(&RundownB);

    //
    // Link the originals into an arrival chain, as the miniport indicates them.
    // This is not decoration: a slot NBL carries whatever Next the producer left
    // in place, so the drain MUST terminate each one as it appends it to a
    // partition. With unchained NBLs every Next is already NULL and that
    // production step would be unobservable.
    //
    for (Index = 0; Index + 1 < RTL_NUMBER_OF(Nbls); Index++) {
        NET_BUFFER_LIST_NEXT_NBL(&Nbls[Index]) = &Nbls[Index + 1];
    }

    //
    // Even NBLs go to target 0 via queue A, odd to target 1 via queue B. Two
    // targets and two rundowns means two rings and, in the drain, two
    // partitions -- and because the arrival chain interleaves them, a drain that
    // failed to terminate would splice one partition into the other.
    //
    for (Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        BOOLEAN UseA = (Index % 2) == 0;

        XdpCpuMapCommitGroupInit(
            &CommitGroup,
            UseA ? &RundownA : &RundownB,
            UseA ? XDPCPUMAP_TEST_FILTER_A : XDPCPUMAP_TEST_FILTER_B,
            0,
            UseA ? (const VOID *)&XdpCpuMapTestRxQueueA : (const VOID *)&XdpCpuMapTestRxQueueB,
            UseA ? (const VOID *)&XdpCpuMapTestGenericA : (const VOID *)&XdpCpuMapTestGenericB,
            FALSE, FALSE, NULL);

        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(
                CpuMap, UseA ? Value0.Target : Value1.Target, &Nbls[Index], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    }

    //
    // The ring slots hold the ORIGINALS themselves, in arrival order, flagged as
    // originals rather than deep copies, each owning its backing reference and
    // its own queue's rundown.
    //
    for (Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        BOOLEAN UseA = (Index % 2) == 0;
        const XDP_CPUMAP_TARGET *Target = UseA ? Value0.Target : Value1.Target;
        const XDP_CPUMAP_RING *Ring = Target->Ring;
        const XDP_CPUMAP_ENTRY *Slot = &Ring->Entries[(Index / 2) & Ring->Mask];

        XDPCPUMAP_TEST_ASSERT(Slot->Nbl == &Nbls[Index]);
        XDPCPUMAP_TEST_ASSERT(!Slot->IsDeepCopy);
        XDPCPUMAP_TEST_ASSERT(Slot->BackingRef == CpuMap);
        XDPCPUMAP_TEST_ASSERT(Slot->NblRundown == (UseA ? &RundownA : &RundownB));
        XDPCPUMAP_TEST_ASSERT(
            Slot->FilterHandle == (UseA ? XDPCPUMAP_TEST_FILTER_A : XDPCPUMAP_TEST_FILTER_B));
    }

    //
    // The ring owns one reference per queued original; the flush group kept
    // none of the target rundown references it borrowed.
    //
    XDPCPUMAP_TEST_ASSERT(RundownA.Count == (LONG)(RTL_NUMBER_OF(Nbls) / 2));
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == (LONG)(RTL_NUMBER_OF(Nbls) / 2));
    XDPCPUMAP_TEST_ASSERT(Value0.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(Value1.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1 + (LONG)RTL_NUMBER_OF(Nbls));

    //
    // Nothing has been handed to NDIS yet: ownership is CPUMAP's, exclusively.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 0);

    XdpCpuMapTestRunQueuedDpcs();

    //
    // The claim, stated to the harness boundary. Each original is indicated
    // EXACTLY ONCE by CPUMAP, is the subject of no CPUMAP return call, and is
    // indicated without RESOURCES -- it is a lent miniport buffer, not a deep
    // copy, so it must be allowed to complete asynchronously.
    //
    // What this proves is CPUMAP's own disposition calls, checked against the
    // immutable pointer snapshots taken inside the NdisF* stubs at call time
    // (see XDP_CPUMAP_TEST_INDICATION). It does NOT cross into the stack: the
    // original's actual return to FnMp travels through
    // XdpGenericReturnNetBufferLists after the upper stack completes it, which
    // this harness does not link and no functional test can observe -- FnMp
    // exposes no RX return stream. That leg is established by the WPP/ETW trace
    // retained on issue #18, which parsed 40 of 40 exact returns. Treat that as
    // external evidence, not unit coverage, and do not restate it as a harness
    // result.
    //
    for (Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XDPCPUMAP_TEST_NBL_DISPOSITION Disposition =
            XdpCpuMapTestNblDisposition(&Nbls[Index]);

        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 1);
        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 0);
        XDPCPUMAP_TEST_ASSERT(
            (Disposition.IndicationFlags & NDIS_RECEIVE_FLAGS_RESOURCES) == 0);
        XDPCPUMAP_TEST_ASSERT(
            (Disposition.IndicationFlags & NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL) != 0);

        //
        // And in the partition belonging to its own queue, not merely in some
        // indication.
        //
        XDPCPUMAP_TEST_ASSERT(Disposition.IndicationIndex != MAXUINT32);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestIndications[Disposition.IndicationIndex].FilterHandle ==
                ((Index % 2) == 0 ? XDPCPUMAP_TEST_FILTER_A : XDPCPUMAP_TEST_FILTER_B));
    }

    //
    // Two targets, so two DPCs and two indications; no chain merged across them.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 2);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 0);

    //
    // Every reference the slots owned went with the packets.
    //
    XDPCPUMAP_TEST_ASSERT(RundownA.Count == 0);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueCount == RTL_NUMBER_OF(Nbls));
    XDPCPUMAP_TEST_ASSERT(Stats.DrainCount == RTL_NUMBER_OF(Nbls));
    XDPCPUMAP_TEST_ASSERT(Stats.RingFullCount == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueTargetInactive == 0);

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
// Post-commit rejection: the original is handed back exactly once and is never
// indicated.
//
// Ring-full and the under-lock Target->Active re-check both happen AFTER
// ownership was taken and ActionNbl cleared, so the packet cannot fall back to
// PASS or TX -- the caller must return it to the miniport (section 8.1a row 9a,
// "Released -- post-commit failure"). Increment 6 asserted the counters and the
// reference balance for these paths; what it could not show is that the NBL
// handed back is the one that was rejected and that it is not ALSO indicated.
//
// Deletion criterion: in the flush's rejection branch, remove
// NET_BUFFER_LIST_NEXT_NBL(BatchEntry->Nbl) = Group->RejectedNbls and keep the
// Group->RejectedNbls = BatchEntry->Nbl store. The operation removed is "link
// each rejected original into the chain handed back to the caller". The mutated
// code still designates a head and still relinquishes the entry, so a
// count-based check on the head pointer is unchanged; what it no longer does is
// splice the previous rejection behind the new one, so the second rejected NBL
// is unreachable from the returned chain -- it instead carries whatever Next the
// producer left, which is why this test builds a real arrival chain. Only the
// per-NBL presence check above can see that.
//
// N.B. an earlier draft of this criterion proposed removing
// BatchEntry->Nbl = NULL instead, on the theory that the batch would then still
// hold the original. That criterion is INVALID and was discarded rather than
// run: BatchEntry->Target is set to NULL immediately above the branch, so both
// the outer target-selection loop and the inner BatchEntry->Target != Target
// test already skip the entry for the remainder of the flush, and Group->Count
// is reset before the group is reused. Nothing reads BatchEntry->Nbl again, so
// the mutation removes no observable behaviour and the test would pass -- rule 3.
//
// Radius of the criterion actually run, all 37 cases in isolation:
//
static
VOID
XdpCpuMapTestZeroCopyPostCommitRejection(
    VOID
    )
{
    NET_BUFFER_LIST Nbls[XDP_CPUMAP_RING_DEPTH_MIN + 2] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF NblRundown;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_HELPER_STATS Stats;
    NET_BUFFER_LIST *Rejected;
    LONG Baseline;
    UINT32 Index;

    XDPCPUMAP_TEST_BEGIN("ZeroCopyPostCommitRejection");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, XDP_CPUMAP_RING_DEPTH_MIN, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    ExInitializeRundownProtection(&NblRundown);
    XdpCpuMapTestInitGroup(&CommitGroup, &NblRundown);

    //
    // An arrival chain, as the miniport indicates it, so that the rejected
    // originals carry a live Next when the flush hands them back. A chain built
    // by overwriting the head rather than linking would then silently drop all
    // but the last rejection.
    //
    for (Index = 0; Index + 1 < RTL_NUMBER_OF(Nbls); Index++) {
        NET_BUFFER_LIST_NEXT_NBL(&Nbls[Index]) = &Nbls[Index + 1];
    }

    //
    // Two more originals than the ring holds. The last two are rejected as ring
    // full, after ownership was already taken. TWO, not one: a single rejection
    // cannot distinguish a chain that is built from one that is overwritten.
    //
    for (Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index], &CommitGroup));
    }

    Rejected = XdpCpuMapTestFinishGroupEx(&CommitGroup);

    //
    // Exactly the last two, each present exactly once in the chain handed back.
    //
    for (Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        BOOLEAN Overflow = Index >= XDP_CPUMAP_RING_DEPTH_MIN;

        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCountNblsInChain(Rejected, &Nbls[Index]) == (Overflow ? 1u : 0u));
    }

    //
    // A rejected original is not in the ring, and a queued one is not in the
    // rejected chain: ownership went exactly one way for each.
    //
    XDPCPUMAP_TEST_ASSERT(
        Value.Target->Ring->Tail - Value.Target->Ring->Head == XDP_CPUMAP_RING_DEPTH_MIN);

    Stats = XdpCpuMapTestQueryHelperStats(CpuMap);
    XDPCPUMAP_TEST_ASSERT(Stats.RingFullCount == 2);
    XDPCPUMAP_TEST_ASSERT(Stats.EnqueueCount == XDP_CPUMAP_RING_DEPTH_MIN);

    XdpCpuMapTestRunQueuedDpcs();

    //
    // The claim: a rejected original is NEVER indicated. Its terminal fate is
    // the caller's DropList, which this harness does not execute -- but "not
    // indicated by CPUMAP" is exactly the half that would be a double delivery.
    //
    for (Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XDPCPUMAP_TEST_NBL_DISPOSITION Disposition =
            XdpCpuMapTestNblDisposition(&Nbls[Index]);
        BOOLEAN Overflow = Index >= XDP_CPUMAP_RING_DEPTH_MIN;

        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == (Overflow ? 0u : 1u));
        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 0);
    }

    //
    // The rejected originals' references were released with them, so only the
    // queued ones ever charged the rundown, and the drain has now cleared those.
    //
    XDPCPUMAP_TEST_ASSERT(NblRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// Teardown disposal: queued originals are RETURNED, never indicated, exactly
// once each.
//
// Quiesce tombstones the entries belonging to a pausing queue and the retire
// drain empties a retiring target's ring; both hand the original back to the
// miniport with NdisFReturnNetBufferLists rather than indicating it, because in
// neither case is there a live steering configuration to deliver it under
// (sections 8.3 and 8.4, row 9a "Released -- teardown").
//
// Both paths are exercised in one test on two targets, so the assertion covers
// the case where they interleave: one target quiesced, the other retired, with
// no original visible to both.
//
// Deletion criterion (direction): in XdpCpuMapChainSetReturn, indicate instead
// of returning. The operation removed is "teardown returns without indicating";
// the same NBLs are still disposed of exactly once, so only the
// identity-and-direction assertions can see it -- counting disposals cannot.
// Full radius, all 37 cases run in isolation: ZeroCopyTeardownDisposal fails 9,
// DrainTombstoneSkip 5, RetireDrainReturns 4; the other 34 pass.
//
// Deletion criterion (scope): drop the owner predicate from the quiesce scan --
// replace the Slot->RxQueueOwner/GenericOwner conjunction with the bare
// Slot->Nbl != NULL test. The operation removed is "quiesce consumes only the
// entries belonging to the queue that is pausing", which is what the mid-test
// assertion here on the OTHER target's originals covers.
//
// Full radius, all 37 cases run in isolation:
//   QuiesceScanCost              CRASH  access-violation at cpumap.c:798 in
//                                       XdpCpuMapChainSetTake -- the scan reaches
//                                       the poison NBLs that test deliberately
//                                       plants for entries it expects to be out
//                                       of scope, and dereferences them
//   DrainTombstoneSkip           FAIL   6 assertions
//   ZeroCopyTeardownDisposal     FAIL   3 assertions
//   the other 34                 pass
//
// N.B. this was first reported as "3 failures, ZeroCopyTeardownDisposal only".
// That number came from a run filtered to this one test and was presented as a
// whole-suite radius. A filtered run can only ever report the case it selected,
// so it is not evidence about any other. Whole-suite behaviour must come from a
// whole-suite run, and a crash must be reported as a crash -- counting FAIL
// lines alone would have scored QuiesceScanCost as passing.
//
static
VOID
XdpCpuMapTestZeroCopyTeardownDisposal(
    VOID
    )
{
    NET_BUFFER_LIST QuiesceNbls[2] = {0};
    NET_BUFFER_LIST RetireNbls[2] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value0;
    XDP_CPUMAP_PROVIDER_VALUE Value1;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF RundownA;
    EX_RUNDOWN_REF RundownB;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    LONG Baseline;
    UINT32 Index;

    XDPCPUMAP_TEST_BEGIN("ZeroCopyTeardownDisposal");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));

    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value0)));
    Entry = XdpCpuMapTestEntry(1, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value1)));

    ExInitializeRundownProtection(&RundownA);
    ExInitializeRundownProtection(&RundownB);

    //
    // Queue A's originals go to target 0 and will be quiesced. Queue B's go to
    // target 1 and are left for the retire drain.
    //
    for (Index = 0; Index < RTL_NUMBER_OF(QuiesceNbls); Index++) {
        XdpCpuMapCommitGroupInit(
            &CommitGroup, &RundownA, XDPCPUMAP_TEST_FILTER_A, 0, &XdpCpuMapTestRxQueueA,
            &XdpCpuMapTestGenericA, FALSE, FALSE, NULL);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value0.Target, &QuiesceNbls[Index], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    }

    for (Index = 0; Index < RTL_NUMBER_OF(RetireNbls); Index++) {
        XdpCpuMapCommitGroupInit(
            &CommitGroup, &RundownB, XDPCPUMAP_TEST_FILTER_B, 0, &XdpCpuMapTestRxQueueB,
            &XdpCpuMapTestGenericB, FALSE, FALSE, NULL);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value1.Target, &RetireNbls[Index], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    }

    //
    // Cancel both drains so quiesce and retire own the disposal rather than the
    // DPC. Without this the rings would simply drain and neither teardown path
    // would be reached.
    //
    XdpCpuMapTestRemoveQueueDpc(Value0.Target->Dpc);
    XdpCpuMapTestRemoveQueueDpc(Value1.Target->Dpc);

    //
    // Quiesce queue A. Its originals are returned; queue B's are untouched,
    // which is the scoping property tombstoning exists for.
    //
    XdpCpuMapQuiesceRxQueue(&XdpCpuMapTestRxQueueA);

    for (Index = 0; Index < RTL_NUMBER_OF(QuiesceNbls); Index++) {
        XDPCPUMAP_TEST_NBL_DISPOSITION Disposition =
            XdpCpuMapTestNblDisposition(&QuiesceNbls[Index]);

        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 1);
        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);
    }

    for (Index = 0; Index < RTL_NUMBER_OF(RetireNbls); Index++) {
        XDPCPUMAP_TEST_NBL_DISPOSITION Disposition =
            XdpCpuMapTestNblDisposition(&RetireNbls[Index]);

        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 0);
        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);
    }

    XDPCPUMAP_TEST_ASSERT(RundownA.Count == 0);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == (LONG)RTL_NUMBER_OF(RetireNbls));

    //
    // Retire target 1 with its ring still occupied. Section 8.3, "Delivery on
    // retire": returned, not indicated, and counted as a drop.
    //
    XdpCpuMapTestRelease(CpuMap, &Value1);
    XdpCpuMapTestDrainSweeps();

    for (Index = 0; Index < RTL_NUMBER_OF(RetireNbls); Index++) {
        XDPCPUMAP_TEST_NBL_DISPOSITION Disposition =
            XdpCpuMapTestNblDisposition(&RetireNbls[Index]);

        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 1);
        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);
    }

    //
    // Nothing was indicated at any point in this test: teardown never delivers.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    XdpCpuMapTestRelease(CpuMap, &Value0);
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
// rejected count is zero; ten assertions here fail, plus twelve in
// ZeroCopyPostCommitRejection, which fills the same ring to witness the same
// invariant from the ownership side. This test asserts the counters and the
// reference balance; that one asserts WHICH originals came back. No other test
// notices, because no other test fills a ring.
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
            &XdpCpuMapTestGenericA, FALSE, FALSE, NULL);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index * 4 + 0], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        XdpCpuMapCommitGroupInit(
            &CommitGroup, &RundownA, XDPCPUMAP_TEST_FILTER_A, 5, &XdpCpuMapTestRxQueueA,
            &XdpCpuMapTestGenericA, FALSE, FALSE, NULL);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index * 4 + 1], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        XdpCpuMapCommitGroupInit(
            &CommitGroup, &RundownB, XDPCPUMAP_TEST_FILTER_B, 0, &XdpCpuMapTestRxQueueB,
            &XdpCpuMapTestGenericB, FALSE, FALSE, NULL);
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
            &XdpCpuMapTestGenericA, FALSE, FALSE, NULL);
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
// Deletion criterion (skip): in the drain, do not advance Head past a tombstone
// -- delete the Ring->Head++ from the Entry->Nbl == NULL branch, leaving the
// Scanned++ so the batch still terminates. The operation removed is "the drain
// makes forward progress over a slot it must not consume".
//
// The observed result is a HANG, not an assertion failure: XdpCpuMapDrainDpc's
// do/while (MoreWork && !Yield) loop is inside ONE DPC invocation, so a Head
// that never advances keeps MoreWork TRUE and the routine never returns. The
// test runner's 4096-iteration re-queue bound is never reached, because the
// spin is below it. That is a faithful model, not a harness artifact: in
// production this is a DPC that never completes, which is DPC_WATCHDOG_VIOLATION
// rather than a miscount.
//
// N.B. this criterion was previously documented as "the ring never drains and
// nine assertions here fail". That was never observed and is wrong -- the suite
// does not get far enough to assert anything. Recorded here because a runner
// that only counted assertion failures would have called a hang "not detected".
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
            &XdpCpuMapTestGenericA, FALSE, FALSE, NULL);
        XDPCPUMAP_TEST_ASSERT(
            XdpCpuMapTestCommit(CpuMap, Value.Target, &Nbls[Index * 2 + 0], &CommitGroup));
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);

        XdpCpuMapCommitGroupInit(
            &CommitGroup, &RundownB, XDPCPUMAP_TEST_FILTER_B, 0, &XdpCpuMapTestRxQueueB,
            &XdpCpuMapTestGenericB, FALSE, FALSE, NULL);
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
// instead of XdpCpuMapChainSetReturn. Four assertions on indication and return
// counts here fail, plus five in ZeroCopyTeardownDisposal, which drives the same
// retire path and asserts per-NBL that the original was returned and never
// indicated. No other test notices: every other one either drains through the
// DPC or retires an empty ring.
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

//
// Commits one zero-copy packet on a named queue. Increment 9's quiesce cases all
// need several distinct (queue, generic, filter) identities in one ring, which
// XdpCpuMapTestInitGroup cannot express because it hardcodes queue A.
//
static
VOID
XdpCpuMapTestCommitOnQueue(
    _Inout_ XDP_CPUMAP *CpuMap,
    _Inout_ XDP_CPUMAP_TARGET *Target,
    _Inout_ NET_BUFFER_LIST *Nbl,
    _In_ EX_RUNDOWN_REF *NblRundown,
    _In_ NDIS_HANDLE FilterHandle,
    _In_ const VOID *RxQueueOwner,
    _In_ const VOID *GenericOwner
    )
{
    XDP_CPUMAP_COMMIT_GROUP Group;

    XdpCpuMapCommitGroupInit(
        &Group, NblRundown, FilterHandle, 0, RxQueueOwner, GenericOwner, FALSE, FALSE, NULL);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestCommit(CpuMap, Target, Nbl, &Group));
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&Group) == 0);
}

//
// The producer that runs WHILE a quiesce scan is in progress. Driven by the
// single ring-lock-release hook in stubs/ntos.h; see that file for why the hook
// exists and what may not use it.
//
typedef struct _XDPCPUMAP_TEST_INJECTOR {
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_TARGET *Target;
    EX_RUNDOWN_REF *NblRundown;
    NET_BUFFER_LIST *Nbls;
    UINT32 Capacity;
    UINT32 Injected;
    const VOID *RxQueueOwner;
    const VOID *GenericOwner;

    //
    // TRUE if the injector was still armed and still had budget when the ring
    // started draining -- that is, it stopped because the experiment ended, not
    // because it ran out. Both cases below assert it, because without it
    // "quiesce stopped" could mean "the producer stopped first", which is the
    // wrong reason for either result.
    //
    BOOLEAN StoppedByDrain;
} XDPCPUMAP_TEST_INJECTOR;

static XDPCPUMAP_TEST_INJECTOR XdpCpuMapTestInjector;

static
BOOLEAN
XdpCpuMapTestInjectProducer(
    VOID
    )
{
    XDPCPUMAP_TEST_INJECTOR *Injector = &XdpCpuMapTestInjector;

    //
    // Stop as soon as the ring starts draining. Quiesce tombstones in place and
    // never touches Head, so Head can only have moved because quiesce's final
    // KeFlushQueuedDpcs has begun running the drain -- and the drain's loop
    // exits on an empty ring, so a producer that kept injecting into it would
    // never let it finish.
    //
    if (Injector->Target->Ring->Head != 0) {
        Injector->StoppedByDrain = TRUE;
        return FALSE;
    }

    if (Injector->Injected == Injector->Capacity) {
        return FALSE;
    }

    XdpCpuMapTestCommitOnQueue(
        Injector->CpuMap, Injector->Target, &Injector->Nbls[Injector->Injected],
        Injector->NblRundown, XDPCPUMAP_TEST_FILTER_B, Injector->RxQueueOwner,
        Injector->GenericOwner);
    Injector->Injected++;

    return TRUE;
}

//
// Quiesce scoping across EVERY live map (section 14, "Quiesce scoping").
//
// Invariant: pausing one receive queue returns every entry that queue owns, in
// every ring of every live map, and ZERO entries owned by a peer queue; the peer
// keeps forwarding afterwards because nothing of the peer's was torn down.
//
// What this adds over DrainTombstoneSkip, which already interleaves two queues
// in ONE ring, is BREADTH. Quiesce has no map-to-interface association, so its
// correctness rests on walking the whole registry and each map's whole target
// table, taking and releasing a backing reference per map. Nothing asserted that
// before: QuiesceScanCost walks four maps but with no matching entry, so a
// quiesce that left every per-map reference held, or that stopped after the
// first map, would have passed it.
//
// Deletion criterion (a): in XdpCpuMapQuiesceScope, delete the
// XdpCpuMapReleaseBacking(CpuMap) that ends each map's iteration. The operation
// removed is "quiesce releases the reference it took to pin the map". Every NBL
// is still disposed of correctly and every rundown still balances, so no
// disposition assertion anywhere can see it -- it is caught only by reference
// and allocation accounting, which is broad rather than local.
//
// Full radius, all 47 cases run in isolation: 10 detected --
// QuiesceScoping fails 6, QuiesceScanCost 4, QuiesceInterfaceScope 3,
// QuiesceTombstoneBalance 3, DrainTombstoneSkip 2, QuiescePassBudget 2,
// QuiesceTailSnapshot 2, ZeroCopyTeardownDisposal 2,
// QuiesceDurationAttribution 2, QuiesceEmpty 1; the other 37 pass. This case
// fails the most, and on the per-map RefCount assertion -- the only one that
// names the leaked reference directly. Everywhere else it surfaces as destroy
// never completing its RefCountZero wait, which is the consequence rather than
// the cause.
//
// Deletion criterion (b): in the same function, delete the "Matched++" in the
// take branch. The operation removed is "a pass that matched entries requires
// another pass", which is section 8.4's termination argument: a pass matching
// nothing is the proof that no pre-publication producer remains. Every entry
// here is reachable in pass one, so every disposition assertion still passes;
// only the PassesTotal and EntriesScanned assertions can see it.
//
// Full radius, all 47 cases run in isolation: 3 detected -- QuiesceScoping
// fails 4, QuiesceInterfaceScope 2, QuiescePassBudget 2; the other 44 pass.
//
static
VOID
XdpCpuMapTestQuiesceScoping(
    VOID
    )
{
#define XDPCPUMAP_TEST_SCOPE_MAPS 2
#define XDPCPUMAP_TEST_SCOPE_CPUS 2
#define XDPCPUMAP_TEST_SCOPE_PER_RING 2

    XDP_CPUMAP *CpuMaps[XDPCPUMAP_TEST_SCOPE_MAPS];
    XDP_CPUMAP_PROVIDER_VALUE Values[XDPCPUMAP_TEST_SCOPE_MAPS][XDPCPUMAP_TEST_SCOPE_CPUS];
    NET_BUFFER_LIST NblsA[XDPCPUMAP_TEST_SCOPE_MAPS][XDPCPUMAP_TEST_SCOPE_CPUS]
                         [XDPCPUMAP_TEST_SCOPE_PER_RING] = {0};
    NET_BUFFER_LIST NblsB[XDPCPUMAP_TEST_SCOPE_MAPS][XDPCPUMAP_TEST_SCOPE_CPUS]
                         [XDPCPUMAP_TEST_SCOPE_PER_RING] = {0};
    NET_BUFFER_LIST NblsLate[XDPCPUMAP_TEST_SCOPE_MAPS][XDPCPUMAP_TEST_SCOPE_CPUS] = {0};
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF RundownA;
    EX_RUNDOWN_REF RundownB;
    XDP_CPUMAP_QUIESCE_STATS StatsBefore;
    XDP_CPUMAP_QUIESCE_STATS StatsAfter;
    const LONG64 Rings = XDPCPUMAP_TEST_SCOPE_MAPS * XDPCPUMAP_TEST_SCOPE_CPUS;
    const LONG64 PerQueue = Rings * XDPCPUMAP_TEST_SCOPE_PER_RING;
    const LONG64 Occupancy = PerQueue * 2;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("QuiesceScoping");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    ExInitializeRundownProtection(&RundownA);
    ExInitializeRundownProtection(&RundownB);

    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCOPE_MAPS; M++) {
        XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMaps[M])));

        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCOPE_CPUS; C++) {
            Entry = XdpCpuMapTestEntry(C, 0, 0);
            XDPCPUMAP_TEST_ASSERT(
                NT_SUCCESS(XdpCpuMapTestResolve(CpuMaps[M], &Entry, &Values[M][C])));
        }
    }

    //
    // Both queues into every ring of every map, interleaved, so no scan can pass
    // by matching a contiguous run.
    //
    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCOPE_MAPS; M++) {
        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCOPE_CPUS; C++) {
            for (UINT32 I = 0; I < XDPCPUMAP_TEST_SCOPE_PER_RING; I++) {
                XdpCpuMapTestCommitOnQueue(
                    CpuMaps[M], Values[M][C].Target, &NblsA[M][C][I], &RundownA,
                    XDPCPUMAP_TEST_FILTER_A, &XdpCpuMapTestRxQueueA, &XdpCpuMapTestGenericA);
                XdpCpuMapTestCommitOnQueue(
                    CpuMaps[M], Values[M][C].Target, &NblsB[M][C][I], &RundownB,
                    XDPCPUMAP_TEST_FILTER_B, &XdpCpuMapTestRxQueueB, &XdpCpuMapTestGenericB);
            }

            //
            // Cancel the drain so quiesce owns disposal rather than the DPC.
            //
            XdpCpuMapTestRemoveQueueDpc(Values[M][C].Target->Dpc);
        }
    }

    XDPCPUMAP_TEST_ASSERT(RundownA.Count == (LONG)PerQueue);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == (LONG)PerQueue);

    XdpCpuMapQueryQuiesceStats(&StatsBefore);
    XdpCpuMapQuiesceRxQueue(&XdpCpuMapTestRxQueueA);
    XdpCpuMapQueryQuiesceStats(&StatsAfter);

    //
    // The scan reached every map and every target, on both passes. TargetsVisited
    // counts only targets whose ring was actually pinned and walked, so a scan
    // that stopped after the first map or the first target reads low here.
    //
    XDPCPUMAP_TEST_ASSERT(StatsAfter.PassesTotal - StatsBefore.PassesTotal == 2);
    XDPCPUMAP_TEST_ASSERT(
        StatsAfter.MapsVisited - StatsBefore.MapsVisited == XDPCPUMAP_TEST_SCOPE_MAPS * 2);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.TargetsVisited - StatsBefore.TargetsVisited == Rings * 2);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.EntriesScanned - StatsBefore.EntriesScanned == Occupancy * 2);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.Tombstoned - StatsBefore.Tombstoned == PerQueue);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.MaxPassesExhausted == StatsBefore.MaxPassesExhausted);

    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCOPE_MAPS; M++) {
        //
        // The owner reference plus one per surviving peer entry. A quiesce that
        // kept its per-map pin would read one higher.
        //
        XDPCPUMAP_TEST_ASSERT(
            CpuMaps[M]->RefCount ==
                1 + XDPCPUMAP_TEST_SCOPE_CPUS * XDPCPUMAP_TEST_SCOPE_PER_RING);

        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCOPE_CPUS; C++) {
            const XDP_CPUMAP_RING *Ring = Values[M][C].Target->Ring;

            for (UINT32 I = 0; I < XDPCPUMAP_TEST_SCOPE_PER_RING; I++) {
                XDPCPUMAP_TEST_NBL_DISPOSITION Disposition;

                Disposition = XdpCpuMapTestNblDisposition(&NblsA[M][C][I]);
                XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 1);
                XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);

                Disposition = XdpCpuMapTestNblDisposition(&NblsB[M][C][I]);
                XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 0);
                XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);

                //
                // Slot contents, not just totals: the peer's entries must be
                // untouched IN PLACE, and Head/Tail unmoved, because quiesce
                // compacts nothing.
                //
                XDPCPUMAP_TEST_ASSERT(Ring->Entries[I * 2 + 0].Nbl == NULL);
                XDPCPUMAP_TEST_ASSERT(Ring->Entries[I * 2 + 0].BackingRef == NULL);
                XDPCPUMAP_TEST_ASSERT(Ring->Entries[I * 2 + 0].NblRundown == NULL);
                XDPCPUMAP_TEST_ASSERT(Ring->Entries[I * 2 + 1].Nbl == &NblsB[M][C][I]);
                XDPCPUMAP_TEST_ASSERT(Ring->Entries[I * 2 + 1].BackingRef == CpuMaps[M]);
                XDPCPUMAP_TEST_ASSERT(Ring->Entries[I * 2 + 1].NblRundown == &RundownB);
            }

            XDPCPUMAP_TEST_ASSERT(Ring->Head == 0);
            XDPCPUMAP_TEST_ASSERT(
                Ring->Tail == XDPCPUMAP_TEST_SCOPE_PER_RING * 2);
        }
    }

    XDPCPUMAP_TEST_ASSERT(RundownA.Count == 0);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == (LONG)PerQueue);

    //
    // The peer keeps forwarding. A fresh commit on queue B enqueues normally and
    // delivers, together with everything quiesce left in place.
    //
    // N.B. this is forwarding AFTER the quiesce returns, not concurrent with the
    // scan. A single-threaded harness cannot run a peer receive indication
    // during a quiesce pass, and the one hook that could is deliberately
    // reserved for the two cases that have no alternative. Concurrent peer
    // forwarding is the functional row's job.
    //
    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCOPE_MAPS; M++) {
        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCOPE_CPUS; C++) {
            XdpCpuMapTestCommitOnQueue(
                CpuMaps[M], Values[M][C].Target, &NblsLate[M][C], &RundownB,
                XDPCPUMAP_TEST_FILTER_B, &XdpCpuMapTestRxQueueB, &XdpCpuMapTestGenericB);
        }
    }

    XdpCpuMapTestRunQueuedDpcs();

    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCOPE_MAPS; M++) {
        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCOPE_CPUS; C++) {
            XDPCPUMAP_TEST_NBL_DISPOSITION Disposition;

            for (UINT32 I = 0; I < XDPCPUMAP_TEST_SCOPE_PER_RING; I++) {
                Disposition = XdpCpuMapTestNblDisposition(&NblsB[M][C][I]);
                XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 1);
                XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 0);

                //
                // And the paused queue's packets were not resurrected by the
                // drain that ran after it.
                //
                Disposition = XdpCpuMapTestNblDisposition(&NblsA[M][C][I]);
                XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);
                XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 1);
            }

            Disposition = XdpCpuMapTestNblDisposition(&NblsLate[M][C]);
            XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 1);
            XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 0);
        }
    }

    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 0);

    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCOPE_MAPS; M++) {
        XDPCPUMAP_TEST_ASSERT(CpuMaps[M]->RefCount == 1);

        for (UINT32 C = 0; C < XDPCPUMAP_TEST_SCOPE_CPUS; C++) {
            XdpCpuMapTestRelease(CpuMaps[M], &Values[M][C]);
        }
    }

    XdpCpuMapTestDrainSweeps();

    for (UINT32 M = 0; M < XDPCPUMAP_TEST_SCOPE_MAPS; M++) {
        XdpCpuMapTestDestroyMap(CpuMaps[M]);
    }

    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// Interface-scoped quiesce (section 8.4, "XdpCpuMapQuiesceInterface").
//
// Invariant: an interface pause matches by GENERIC, so every receive queue on
// the pausing interface is collected -- including queues the caller never named
// -- and no queue on another interface is.
//
// This predicate has never been exercised against a match. QuiesceEmpty runs it
// on empty rings, QuiesceScanCost runs it with tokens chosen NOT to match, and
// every tombstone test uses the queue-scoped entry point. A build whose
// interface scope matched nothing at all would have passed the whole suite,
// which would leave interface pause returning none of its own NBLs.
//
// Deletion criterion: in XdpCpuMapQuiesceScope's match predicate, delete the
// GenericOwner disjunct, leaving only the RxQueueOwner test. The operation
// removed is "interface scope matches by owning interface". It compiles --
// GenericOwner is still read by the entry assertion and the trace -- and it is
// invisible to every queue-scoped test.
//
// Full radius, all 47 cases run in isolation: 1 detected -- this case fails 14,
// beginning with the Tombstoned outcome; the other 46 pass. That is the point:
// before this case existed, the operation had no coverage at all.
//
static
VOID
XdpCpuMapTestQuiesceInterfaceScope(
    VOID
    )
{
    NET_BUFFER_LIST NblsA1[2] = {0};
    NET_BUFFER_LIST NblsA2[2] = {0};
    NET_BUFFER_LIST NblsB[2] = {0};
    NET_BUFFER_LIST LateNbl = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF RundownA1;
    EX_RUNDOWN_REF RundownA2;
    EX_RUNDOWN_REF RundownB;
    XDP_CPUMAP_QUIESCE_STATS StatsBefore;
    XDP_CPUMAP_QUIESCE_STATS StatsAfter;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("QuiesceInterfaceScope");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    ExInitializeRundownProtection(&RundownA1);
    ExInitializeRundownProtection(&RundownA2);
    ExInitializeRundownProtection(&RundownB);

    //
    // Two DISTINCT receive queues on interface A, plus one on interface B, all
    // interleaved in one ring.
    //
    for (UINT32 Index = 0; Index < 2; Index++) {
        XdpCpuMapTestCommitOnQueue(
            CpuMap, Value.Target, &NblsA1[Index], &RundownA1, XDPCPUMAP_TEST_FILTER_A,
            &XdpCpuMapTestRxQueueA, &XdpCpuMapTestGenericA);
        XdpCpuMapTestCommitOnQueue(
            CpuMap, Value.Target, &NblsA2[Index], &RundownA2, XDPCPUMAP_TEST_FILTER_A,
            &XdpCpuMapTestRxQueueA2, &XdpCpuMapTestGenericA);
        XdpCpuMapTestCommitOnQueue(
            CpuMap, Value.Target, &NblsB[Index], &RundownB, XDPCPUMAP_TEST_FILTER_B,
            &XdpCpuMapTestRxQueueB, &XdpCpuMapTestGenericB);
    }

    XdpCpuMapTestRemoveQueueDpc(Value.Target->Dpc);

    XdpCpuMapQueryQuiesceStats(&StatsBefore);
    XdpCpuMapQuiesceInterface(&XdpCpuMapTestGenericA);
    XdpCpuMapQueryQuiesceStats(&StatsAfter);

    XDPCPUMAP_TEST_ASSERT(StatsAfter.Tombstoned - StatsBefore.Tombstoned == 4);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.PassesTotal - StatsBefore.PassesTotal == 2);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.EntriesScanned - StatsBefore.EntriesScanned == 12);

    //
    // BOTH of interface A's queues were collected, and interface B's was not.
    //
    for (UINT32 Index = 0; Index < 2; Index++) {
        XDPCPUMAP_TEST_NBL_DISPOSITION Disposition;

        Disposition = XdpCpuMapTestNblDisposition(&NblsA1[Index]);
        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 1);
        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);

        Disposition = XdpCpuMapTestNblDisposition(&NblsA2[Index]);
        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 1);
        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);

        Disposition = XdpCpuMapTestNblDisposition(&NblsB[Index]);
        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 0);
        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);
    }

    XDPCPUMAP_TEST_ASSERT(RundownA1.Count == 0);
    XDPCPUMAP_TEST_ASSERT(RundownA2.Count == 0);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 2);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 3);

    //
    // The peer interface keeps forwarding: nothing of its was consumed, and a
    // fresh commit re-arms the drain, which walks over interface A's tombstones
    // and indicates every one of interface B's packets exactly once.
    //
    XdpCpuMapTestRunQueuedDpcs();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);

    XdpCpuMapTestCommitOnQueue(
        CpuMap, Value.Target, &LateNbl, &RundownB, XDPCPUMAP_TEST_FILTER_B,
        &XdpCpuMapTestRxQueueB, &XdpCpuMapTestGenericB);
    XdpCpuMapTestRunQueuedDpcs();

    for (UINT32 Index = 0; Index < 2; Index++) {
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(&NblsB[Index]).Indicated == 1);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(&NblsB[Index]).Returned == 0);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(&NblsA1[Index]).Indicated == 0);
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(&NblsA2[Index]).Indicated == 0);
    }
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(&LateNbl).Indicated == 1);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 0);
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
// Quiesce tail snapshot (section 14, "Quiesce tail snapshot").
//
// Invariant: one pass over one ring terminates after at most the occupancy
// present when the pass started. Ring capacity bounds occupancy but NOT
// cumulative scanning, so a scan that followed a live tail could be extended
// indefinitely by a peer producer -- which is section 8.4's first correction and
// the reason the pause bound is a bound at all.
//
// The producer runs from the ring-lock-release hook, because that release is
// exactly where quiesce lets producers in and it is the only place a
// single-threaded harness can model one. The entries it injects belong to a
// queue the scan is NOT scoped to, so nothing matches, the pass count stays at
// one, and the assertion is on scanning alone.
//
// Deletion criterion: in XdpCpuMapQuiesceScope, delete the TailSnapshot capture
// -- remove the local and its assignment and let the two loop conditions read
// Ring->Tail directly. The operation removed is "the pass is bounded by the tail
// as it was, not as it becomes". It compiles, and the injected entries are still
// disposed of correctly, so only the EntriesScanned assertion can see it.
//
// Full radius, all 47 cases run in isolation: 2 detected -- this case fails 4,
// beginning with the EntriesScanned outcome, and QuiescePassBudget fails 5
// because following a live tail changes that case's pass arithmetic too; the
// other 45 pass.
//
static
VOID
XdpCpuMapTestQuiesceTailSnapshot(
    VOID
    )
{
#define XDPCPUMAP_TEST_SNAPSHOT_DEPTH 64u
#define XDPCPUMAP_TEST_SNAPSHOT_BATCH 4u
#define XDPCPUMAP_TEST_SNAPSHOT_FILL 16u

    NET_BUFFER_LIST Nbls[XDPCPUMAP_TEST_SNAPSHOT_FILL] = {0};
    NET_BUFFER_LIST Injected[8] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF Rundown;
    XDP_CPUMAP_QUIESCE_STATS StatsBefore;
    XDP_CPUMAP_QUIESCE_STATS StatsAfter;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("QuiesceTailSnapshot");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry =
        XdpCpuMapTestEntry(
            0, XDPCPUMAP_TEST_SNAPSHOT_DEPTH, XDPCPUMAP_TEST_SNAPSHOT_BATCH);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));
    XDPCPUMAP_TEST_ASSERT(CpuMap->EffectiveDrainBatchSize == XDPCPUMAP_TEST_SNAPSHOT_BATCH);

    ExInitializeRundownProtection(&Rundown);

    for (UINT32 Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XdpCpuMapTestCommitOnQueue(
            CpuMap, Value.Target, &Nbls[Index], &Rundown, XDPCPUMAP_TEST_FILTER_B,
            &XdpCpuMapTestRxQueueB, &XdpCpuMapTestGenericB);
    }

    XdpCpuMapTestRemoveQueueDpc(Value.Target->Dpc);
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Head == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail == XDPCPUMAP_TEST_SNAPSHOT_FILL);

    RtlZeroMemory(&XdpCpuMapTestInjector, sizeof(XdpCpuMapTestInjector));
    XdpCpuMapTestInjector.CpuMap = CpuMap;
    XdpCpuMapTestInjector.Target = Value.Target;
    XdpCpuMapTestInjector.NblRundown = &Rundown;
    XdpCpuMapTestInjector.Nbls = Injected;
    XdpCpuMapTestInjector.Capacity = RTL_NUMBER_OF(Injected);
    XdpCpuMapTestInjector.RxQueueOwner = &XdpCpuMapTestRxQueueB;
    XdpCpuMapTestInjector.GenericOwner = &XdpCpuMapTestGenericB;
    XdpCpuMapTestRingLockReleaseHook = XdpCpuMapTestInjectProducer;

    XdpCpuMapQueryQuiesceStats(&StatsBefore);
    XdpCpuMapQuiesceRxQueue(&XdpCpuMapTestRxQueueA);
    XdpCpuMapQueryQuiesceStats(&StatsAfter);

    XdpCpuMapTestRingLockReleaseHook = NULL;

    //
    // THE OUTCOME: the pass scanned the occupancy it snapshotted, and not one
    // slot the producer added afterwards.
    //
    XDPCPUMAP_TEST_ASSERT(
        StatsAfter.EntriesScanned - StatsBefore.EntriesScanned ==
            XDPCPUMAP_TEST_SNAPSHOT_FILL);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.PassesTotal - StatsBefore.PassesTotal == 1);
    XDPCPUMAP_TEST_ASSERT(StatsAfter.Tombstoned == StatsBefore.Tombstoned);

    //
    // SUPPORTING, and load-bearing for mutation rule 4: the tail really did
    // advance while the scan was running. Without these the outcome assertion
    // would hold just as well against a scan no producer ever raced, and the
    // criterion above would be detected for the wrong reason -- or not at all.
    //
    // One injection per ring-lock release the scan performs, which is one for
    // the acquisition that snapshots Head and Tail, plus one per chunk. The
    // snapshot release lands AFTER the snapshot is taken, so its entry is
    // already outside the pass's bound.
    //
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestInjector.Injected ==
            1 + XDPCPUMAP_TEST_SNAPSHOT_FILL / XDPCPUMAP_TEST_SNAPSHOT_BATCH);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestInjector.Injected < XdpCpuMapTestInjector.Capacity);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestInjector.StoppedByDrain);
    XDPCPUMAP_TEST_ASSERT(
        Value.Target->Ring->Tail ==
            XDPCPUMAP_TEST_SNAPSHOT_FILL + XdpCpuMapTestInjector.Injected);

    //
    // Everything the producer added was still delivered by the drain quiesce
    // flushed, so the bound costs no packet.
    //
    XdpCpuMapTestRunQueuedDpcs();
    for (UINT32 Index = 0; Index < XdpCpuMapTestInjector.Injected; Index++) {
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(&Injected[Index]).Indicated == 1);
    }
    for (UINT32 Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(&Nbls[Index]).Indicated == 1);
    }
    XDPCPUMAP_TEST_ASSERT(Rundown.Count == 0);
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
// Quiesce pass budget (section 14, "Quiesce pass budget").
//
// Invariant: the pass loop terminates at XDP_CPUMAP_QUIESCE_MAX_PASSES even
// against a producer that never stops, counts the exhaustion, and leaves the
// pre-existing rundown wait able to complete.
//
// The producer here injects entries the scan DOES match, so every pass finds
// work and only the budget can stop it. Exhaustion is a defect signal, not a
// tuning one -- section 8.4's termination argument says a gated producer set
// cannot survive one clean pass -- so this test deliberately produces the
// pathological case rather than pretending it is unreachable.
//
// Deletion criterion (a): delete "&& Passes < XDP_CPUMAP_QUIESCE_MAX_PASSES"
// from the do/while condition. The operation removed is the budget itself. The
// loop then runs until the injector exhausts its own capacity, which is what the
// capacity is for: without it this criterion would be observed as a hang rather
// than as an assertion failure, and a hang says nothing about which assertion
// found it.
//
// Full radius, all 47 cases run in isolation: 1 detected -- this case fails 5,
// beginning with both outcome assertions, and terminates rather than hanging;
// the other 46 pass.
//
// Deletion criterion (b): delete the "Matched++" in the take branch. The
// operation removed is "a pass that matched requires another pass". The loop
// then exits after pass one with the injected entries still queued, so
// PassesTotal is 1 and nothing is counted as exhausted. Radius is recorded on
// XdpCpuMapTestQuiesceScoping, which fails the most assertions against it.
//
static
VOID
XdpCpuMapTestQuiescePassBudget(
    VOID
    )
{
#define XDPCPUMAP_TEST_BUDGET_DEPTH 128u
#define XDPCPUMAP_TEST_BUDGET_BATCH 4u
#define XDPCPUMAP_TEST_BUDGET_FILL 4u

    NET_BUFFER_LIST Nbls[XDPCPUMAP_TEST_BUDGET_FILL] = {0};
    NET_BUFFER_LIST Injected[64] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF Rundown;
    XDP_CPUMAP_QUIESCE_STATS StatsBefore;
    XDP_CPUMAP_QUIESCE_STATS StatsAfter;
    UINT32 Delivered = 0;
    UINT32 Dropped = 0;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("QuiescePassBudget");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, XDPCPUMAP_TEST_BUDGET_DEPTH, XDPCPUMAP_TEST_BUDGET_BATCH);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    ExInitializeRundownProtection(&Rundown);

    for (UINT32 Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XdpCpuMapTestCommitOnQueue(
            CpuMap, Value.Target, &Nbls[Index], &Rundown, XDPCPUMAP_TEST_FILTER_B,
            &XdpCpuMapTestRxQueueB, &XdpCpuMapTestGenericB);
    }

    XdpCpuMapTestRemoveQueueDpc(Value.Target->Dpc);

    RtlZeroMemory(&XdpCpuMapTestInjector, sizeof(XdpCpuMapTestInjector));
    XdpCpuMapTestInjector.CpuMap = CpuMap;
    XdpCpuMapTestInjector.Target = Value.Target;
    XdpCpuMapTestInjector.NblRundown = &Rundown;
    XdpCpuMapTestInjector.Nbls = Injected;
    XdpCpuMapTestInjector.Capacity = RTL_NUMBER_OF(Injected);
    XdpCpuMapTestInjector.RxQueueOwner = &XdpCpuMapTestRxQueueB;
    XdpCpuMapTestInjector.GenericOwner = &XdpCpuMapTestGenericB;
    XdpCpuMapTestRingLockReleaseHook = XdpCpuMapTestInjectProducer;

    XdpCpuMapQueryQuiesceStats(&StatsBefore);
    XdpCpuMapQuiesceRxQueue(&XdpCpuMapTestRxQueueB);
    XdpCpuMapQueryQuiesceStats(&StatsAfter);

    XdpCpuMapTestRingLockReleaseHook = NULL;

    //
    // THE OUTCOME.
    //
    XDPCPUMAP_TEST_ASSERT(
        StatsAfter.PassesTotal - StatsBefore.PassesTotal == XDP_CPUMAP_QUIESCE_MAX_PASSES);
    XDPCPUMAP_TEST_ASSERT(
        StatsAfter.MaxPassesExhausted - StatsBefore.MaxPassesExhausted == 1);

    //
    // SUPPORTING, for mutation rule 4: the loop stopped because the BUDGET ran
    // out, not because the producer did. StoppedByDrain is only set when the
    // injector still had budget at the moment the ring began draining, which is
    // after the loop has already returned.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestInjector.Injected > 0);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapTestInjector.Injected < XdpCpuMapTestInjector.Capacity);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestInjector.StoppedByDrain);

    //
    // The fallback still works. Section 8.4: on exhaustion quiesce returns and
    // the pre-existing ExWaitForRundownProtectionRelease waits for the DPCs. In
    // this harness that wait is exactly "the rundown reaches zero", and it does
    // -- every packet was either tombstoned and returned or drained and
    // indicated, each exactly once.
    //
    XDPCPUMAP_TEST_ASSERT(Rundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    for (UINT32 Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XDPCPUMAP_TEST_NBL_DISPOSITION Disposition =
            XdpCpuMapTestNblDisposition(&Nbls[Index]);

        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated + Disposition.Returned == 1);
        Delivered += Disposition.Indicated;
        Dropped += Disposition.Returned;
    }
    for (UINT32 Index = 0; Index < XdpCpuMapTestInjector.Injected; Index++) {
        XDPCPUMAP_TEST_NBL_DISPOSITION Disposition =
            XdpCpuMapTestNblDisposition(&Injected[Index]);

        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated + Disposition.Returned == 1);
        Delivered += Disposition.Indicated;
        Dropped += Disposition.Returned;
    }

    XDPCPUMAP_TEST_ASSERT(
        Delivered + Dropped == RTL_NUMBER_OF(Nbls) + XdpCpuMapTestInjector.Injected);

    //
    // And the exhaustion was real: entries survived the loop, which is the whole
    // condition the counter names.
    //
    XDPCPUMAP_TEST_ASSERT(Delivered > 0);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// Tombstone reference balance, against the section 8.1a audit table (section 14,
// "Tombstone reference balance").
//
// A slot owns exactly three things -- the NBL, one CPUMAP backing reference, one
// receive-queue NBL rundown reference -- and NO target rundown reference. This
// case walks that table row by row, on a ring holding a zero-copy entry, a DEEP
// COPY, and a peer's entry at once, because the mixed case is where the audit's
// two NBL rows (9a, 9b) diverge and nothing exercised them together.
//
// Row 1/3, "no ring entry holds a target rundown reference", is asserted with
// the ring OCCUPIED. That is the round-3 deadlock condition: if a slot held one,
// the retire wait could not complete without the target CPU running its DPC.
// This assertion has no deletion criterion, because the operation it guards does
// not exist -- XDP_CPUMAP_ENTRY has no rundown field. It is here to fail on the
// day one is added, which is exactly how round 3 was reached.
//
// Deletion criterion: in XdpCpuMapChainSetTake, delete
// "Candidate->IsDeepCopy == Entry->IsDeepCopy" from the chain-match conjunction.
// The operation removed is "a deep copy and an original never share a
// disposition chain". They then merge, and XdpCpuMapChainSetReturn applies ONE
// disposition to both -- either handing the miniport a buffer from our own pool
// or recycling the miniport's NBL into it. No other case mixes a deep copy and a
// zero-copy entry from the same queue in one ring, so no other case can see it.
//
// Full radius, all 47 cases run in isolation: 1 detected -- this case fails 5;
// the other 46 pass. The first failure is production's own checked-build
// ASSERT(Chain->DeepCopyPool == Entry->DeepCopyPool), which the harness reports
// as a failure; the recycle and page-accounting outcomes follow it, so the
// result does not depend on assertions being enabled.
//
static
VOID
XdpCpuMapTestQuiesceTombstoneBalance(
    VOID
    )
{
    UCHAR Payload[64];
    NET_BUFFER_LIST Zero[2] = {0};
    NET_BUFFER_LIST PeerNbl = {0};
    NET_BUFFER_LIST LateNbl = {0};
    NET_BUFFER_LIST *Original;
    NET_BUFFER_LIST *Copy;
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF RundownA;
    EX_RUNDOWN_REF RundownB;
    XDP_CPUMAP_COMMIT_GROUP CommitGroup;
    XDP_CPUMAP_DEEPCOPY_POOL Pool;
    XDP_FRAME_CPUMAP_REDIRECT_V1 Redirect;
    XDP_CPUMAP_RING *Ring;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("QuiesceTombstoneBalance");

    XdpCpuMapTestResetNdisPool();
    for (UINT32 Index = 0; Index < sizeof(Payload); Index++) {
        Payload[Index] = (UCHAR)(Index + 3);
    }

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    XDPCPUMAP_TEST_ASSERT(
        NT_SUCCESS(XdpCpuMapDeepCopyPoolInitialize(&Pool, XDPCPUMAP_TEST_FILTER_A)));
    ExInitializeRundownProtection(&RundownA);
    ExInitializeRundownProtection(&RundownB);

    Ring = Value.Target->Ring;

    //
    // Slot 0: queue A, zero copy. Slot 1: queue B, the peer. Slot 2: queue A,
    // DEEP copy. Slot 3: queue A, zero copy.
    //
    XdpCpuMapTestCommitOnQueue(
        CpuMap, Value.Target, &Zero[0], &RundownA, XDPCPUMAP_TEST_FILTER_A,
        &XdpCpuMapTestRxQueueA, &XdpCpuMapTestGenericA);
    XdpCpuMapTestCommitOnQueue(
        CpuMap, Value.Target, &PeerNbl, &RundownB, XDPCPUMAP_TEST_FILTER_B,
        &XdpCpuMapTestRxQueueB, &XdpCpuMapTestGenericB);

    XdpCpuMapTestInitDeepCopyGroup(&CommitGroup, &RundownA, &Pool);
    Original = XdpCpuMapTestCreateSourceNbl(Payload, sizeof(Payload));
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTryAcquireTargetReference(CpuMap, Value.Target));
    XdpCpuMapReferenceBacking(CpuMap);
    Redirect = XdpCpuMapTestFrameRedirect(CpuMap, Value.Target, 0);
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapCommitRedirect(&Redirect, Original, FALSE, FALSE, &CommitGroup) ==
            XdpCpuMapCommitDeepCopied);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestFinishGroup(&CommitGroup) == 0);
    Copy = Ring->Entries[2].Nbl;
    XDPCPUMAP_TEST_ASSERT(Copy != NULL && Copy != Original);
    XDPCPUMAP_TEST_ASSERT(Ring->Entries[2].IsDeepCopy);
    XDPCPUMAP_TEST_ASSERT(Ring->Entries[2].DeepCopyPool == &Pool);

    XdpCpuMapTestCommitOnQueue(
        CpuMap, Value.Target, &Zero[1], &RundownA, XDPCPUMAP_TEST_FILTER_A,
        &XdpCpuMapTestRxQueueA, &XdpCpuMapTestGenericA);

    XdpCpuMapTestRemoveQueueDpc(Value.Target->Dpc);

    //
    // Row 8: one NblRundown reference per RING ENTRY, which on the low-resource
    // path is one per deep copy, not per original.
    //
    XDPCPUMAP_TEST_ASSERT(RundownA.Count == 3);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 1);

    //
    // Row 7: one backing reference per ring entry, plus the owner reference.
    //
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 5);

    //
    // Rows 1 and 3, with the ring OCCUPIED: no slot holds a target rundown
    // reference, so the retire wait never depends on the target CPU draining.
    //
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);

    //
    // Row 9b: the original went home through the caller's DropList at commit,
    // and only the copy is in the ring.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 1);
    XDPCPUMAP_TEST_ASSERT(Pool.CacheCount == 1);

    XdpCpuMapQuiesceRxQueue(&XdpCpuMapTestRxQueueA);

    //
    // Row 9a: originals RETURNED, exactly once, never indicated.
    //
    for (UINT32 Index = 0; Index < RTL_NUMBER_OF(Zero); Index++) {
        XDPCPUMAP_TEST_NBL_DISPOSITION Disposition =
            XdpCpuMapTestNblDisposition(&Zero[Index]);

        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 1);
        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);
    }

    //
    // Row 9b: the copy is RECYCLED into its originating pool, and the miniport
    // hears nothing about it -- it never owned it. Its pages are back and its
    // descriptor is cached, so it was neither returned nor leaked.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(Copy).Returned == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(Copy).Indicated == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(Original).Returned == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(Original).Indicated == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);
    XDPCPUMAP_TEST_ASSERT(Pool.CacheCount == 1);

    //
    // Rows 7 and 8 again: every reference the tombstoned slots held was released
    // exactly once, and the peer's are untouched.
    //
    XDPCPUMAP_TEST_ASSERT(RundownA.Count == 0);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 1);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 2);

    //
    // Row 3 again: quiesce's own target pin is balanced.
    //
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);

    //
    // Row 10: a tombstone owns NOTHING, asserted on the slot rather than
    // inferred from totals; the peer's slot is intact and Head/Tail unmoved.
    //
    for (UINT32 Index = 0; Index < 4; Index++) {
        const XDP_CPUMAP_ENTRY *Slot = &Ring->Entries[Index];

        if (Index == 1) {
            XDPCPUMAP_TEST_ASSERT(Slot->Nbl == &PeerNbl);
            XDPCPUMAP_TEST_ASSERT(Slot->BackingRef == CpuMap);
            XDPCPUMAP_TEST_ASSERT(Slot->NblRundown == &RundownB);
            continue;
        }

        XDPCPUMAP_TEST_ASSERT(Slot->Nbl == NULL);
        XDPCPUMAP_TEST_ASSERT(Slot->BackingRef == NULL);
        XDPCPUMAP_TEST_ASSERT(Slot->NblRundown == NULL);
        XDPCPUMAP_TEST_ASSERT(Slot->DeepCopyPool == NULL);
        XDPCPUMAP_TEST_ASSERT(!Slot->IsDeepCopy);
    }

    XDPCPUMAP_TEST_ASSERT(Ring->Head == 0);
    XDPCPUMAP_TEST_ASSERT(Ring->Tail == 4);

    XdpCpuMapTestRunQueuedDpcs();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);

    //
    // The peer keeps forwarding. A fresh commit re-arms the drain, which walks
    // over the tombstones and indicates only the peer's packets.
    //
    XdpCpuMapTestCommitOnQueue(
        CpuMap, Value.Target, &LateNbl, &RundownB, XDPCPUMAP_TEST_FILTER_B,
        &XdpCpuMapTestRxQueueB, &XdpCpuMapTestGenericB);
    XdpCpuMapTestRunQueuedDpcs();

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(&PeerNbl).Indicated == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(&LateNbl).Indicated == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblDisposition(Copy).Indicated == 0);
    XDPCPUMAP_TEST_ASSERT(RundownB.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);

    XdpCpuMapTestDeleteSourceNbl(Original);

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);

    XdpCpuMapDeepCopyPoolCleanup(&Pool);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestNblLive == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestPageLive == 0);
    XdpCpuMapStop();
}

//
// Suppressed or unavailable target DPC (section 14, and the round-3 deadlock).
//
// Invariant: a target whose CPU never runs its DPC still retires. The rundown
// wait completes because no ring entry holds a target rundown reference,
// KeRemoveQueueDpc cancels the instance queued before the wait finished,
// KeFlushQueuedDpcs returns, and the RING IS DRAINED SYNCHRONOUSLY BY THE WORKER
// THREAD -- which is what makes the packets returned rather than indicated.
//
// RetireDrainReturns reaches the same drain, but it cancels the DPC ITSELF
// beforehand, so it says nothing about whether retire would have cancelled it.
// Here the DPC is left queued exactly as a real suppressed target's would be,
// and retire has to deal with it.
//
// Deletion criterion: delete "KeRemoveQueueDpc(Target->Dpc);" from
// XdpCpuMapRetireTarget. The operation removed is "retire cancels the queued
// instance before flushing". The following KeFlushQueuedDpcs then RUNS that
// instance, so the ring is drained by the DPC and the packets are INDICATED to a
// target that is being retired -- the delivery-on-retire contract inverted. Both
// the outcome assertions and the DpcRunCount supporting assertion see it.
//
// Full radius, all 47 cases run in isolation: 3 detected -- this case fails 11,
// beginning with DpcRunCount, RetireDrainReturns 4, DeepCopyTeardownRecycle 4;
// the other 44 pass. The two existing cases cancel the DPC themselves and so
// detect it only through the changed disposition; this one detects the changed
// EXECUTOR, which is what the section 14 row is actually about.
//
// Second deletion criterion: delete the
// InterlockedAdd64(&XdpCpuMapSweepStats.RetireDropCount, ...) in
// XdpCpuMapDrainRing. The operation removed is "packets dropped on retire are
// counted". Every disposition still happens exactly once, so only the
// RetireDropCount assertion below can see it -- which is the point of adding a
// read path for a counter that had none.
//
// Full radius, all 47 cases run in isolation: 1 detected -- this case fails 1,
// on that assertion; the other 46 pass.
//
static
VOID
XdpCpuMapTestRetireSuppressedTargetDpc(
    VOID
    )
{
    NET_BUFFER_LIST Nbls[3] = {0};
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_PROVIDER_VALUE Value;
    XDP_CPUMAP_ENTRY_V1 Entry;
    EX_RUNDOWN_REF Rundown;
    KDPC *TargetDpc;
    ULONG RunsBefore;
    LONG64 RetireDropsBefore;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("RetireSuppressedTargetDpc");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(16, &CpuMap)));
    Entry = XdpCpuMapTestEntry(0, 0, 0);
    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestResolve(CpuMap, &Entry, &Value)));

    ExInitializeRundownProtection(&Rundown);
    TargetDpc = Value.Target->Dpc;

    for (UINT32 Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XdpCpuMapTestCommitOnQueue(
            CpuMap, Value.Target, &Nbls[Index], &Rundown, XDPCPUMAP_TEST_FILTER_A,
            &XdpCpuMapTestRxQueueA, &XdpCpuMapTestGenericA);
    }

    //
    // The DPC is queued and the target CPU never runs it. This is the suppressed
    // or offlined target; nothing here cancels it, because that is retire's job.
    //
    XDPCPUMAP_TEST_ASSERT(TargetDpc->Queued);
    RunsBefore = XdpCpuMapTestDpcRunCount;

    //
    // The round-3 precondition, asserted with the ring OCCUPIED: the only
    // holders of the target rundown are producers, DPC self-requeue windows and
    // quiesce passes, none of which needs the target CPU to make progress. If a
    // ring entry held one, the wait below could never complete.
    //
    XDPCPUMAP_TEST_ASSERT(Value.Target->PacketRundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(Value.Target->Ring->Tail - Value.Target->Ring->Head == 3);

    RetireDropsBefore = XdpCpuMapQueryRetireDropCount();

    XdpCpuMapTestRelease(CpuMap, &Value);
    XdpCpuMapTestDrainSweeps();

    //
    // The DPC never ran: retire cancelled it, and the worker drained the ring on
    // its own thread.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestDpcRunCount == RunsBefore);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestIndicationCount == 0);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturnCount == 1);
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestReturns[0].NblCount == RTL_NUMBER_OF(Nbls));

    for (UINT32 Index = 0; Index < RTL_NUMBER_OF(Nbls); Index++) {
        XDPCPUMAP_TEST_NBL_DISPOSITION Disposition =
            XdpCpuMapTestNblDisposition(&Nbls[Index]);

        XDPCPUMAP_TEST_ASSERT(Disposition.Returned == 1);
        XDPCPUMAP_TEST_ASSERT(Disposition.Indicated == 0);
    }

    XDPCPUMAP_TEST_ASSERT(Rundown.Count == 0);
    XDPCPUMAP_TEST_ASSERT(CpuMap->RefCount == 1);
    XDPCPUMAP_TEST_ASSERT(CpuMap->TargetCount == 0);

    //
    // Counted as a retire drop, one per packet. This is the ONLY observable for
    // "these packets were disposed of by the retire path rather than by the
    // pause", which matters because quiesce cannot see a ring whose target the
    // sweep already unlinked -- the acquire it is documented to fail on cannot
    // fail, since the unlink is published under ConfigLock before the rundown is
    // run down. RetireDropCount had no read path at all before this increment.
    //
    XDPCPUMAP_TEST_ASSERT(
        XdpCpuMapQueryRetireDropCount() - RetireDropsBefore == RTL_NUMBER_OF(Nbls));

    XdpCpuMapTestDestroyMap(CpuMap);
    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

//
// A DPC that is NOT a CPUMAP DPC, burning a controlled interval when the flush
// runs it. Used only by XdpCpuMapTestQuiesceDurationAttribution.
//
// KeInsertQueueDpc's stub asserts the target rundown depth for CPUMAP DPCs via
// Dpc->Context; this one is queued with a NULL context precisely because it
// belongs to nothing in this driver, which is the point -- KeFlushQueuedDpcs
// waits for unrelated DPCs, and that is the term no CPUMAP cap can shrink.
//
static UINT32 XdpCpuMapTestBurnDpcMicroseconds;
static UINT32 XdpCpuMapTestBurnDpcRuns;

static
VOID
XdpCpuMapTestBurnDpcRoutine(
    _In_ KDPC *Dpc,
    _In_opt_ VOID *DeferredContext,
    _In_opt_ VOID *SystemArgument1,
    _In_opt_ VOID *SystemArgument2
    )
{
    LARGE_INTEGER Frequency;
    LARGE_INTEGER Start;
    LARGE_INTEGER Now;
    LONG64 Target;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&Start);
    Target = (Frequency.QuadPart * XdpCpuMapTestBurnDpcMicroseconds) / 1000000;

    //
    // Busy-wait to a wall-clock target rather than spinning a counter: the
    // duration has to be a controlled INPUT to the assertion, not whatever the
    // optimizer leaves of a loop.
    //
    do {
        QueryPerformanceCounter(&Now);
    } while (Now.QuadPart - Start.QuadPart < Target);

    XdpCpuMapTestBurnDpcRuns++;
}

//
// Scan/flush attribution (section 14, pause-latency calibration).
//
// Invariant: ScanUs is the ring scan and FlushUs is KeFlushQueuedDpcs -- not
// merely that the two sum to DurationUs.
//
// Attribution is the entire value of the split. Section 14's remediation --
// reduce XDP_CPUMAP_GLOBAL_MAX_RING_ENTRIES, then XDP_CPUMAP_MAX_LIVE_MAPS --
// acts on the scan alone, while KeFlushQueuedDpcs costs O(processor count) and
// sits outside every CPUMAP cap. A build that transposed the two would send
// someone to shrink a constant that cannot move the term that is actually large.
// The sum assertion on XdpCpuMapTestQuiesceScanCost cannot see that, and did
// not: transposing the two assignments passed the whole suite.
//
// Both directions are asserted, because one alone is satisfiable by a constant
// offset rather than by attribution.
//
//   (a) scan-heavy, flush-empty: rings full of entries the scope does not match,
//       and NO DPC queued, so the flush has nothing to run.
//   (b) scan-trivial, flush-heavy: no live maps at all, and one queued DPC that
//       is not a CPUMAP DPC and burns a controlled interval. That is a faithful
//       model rather than a convenience -- the production concern is precisely
//       that KeFlushQueuedDpcs waits on UNRELATED DPCs, which is why an idle
//       machine understates it.
//
// Deletion criterion (a): delete the
// InterlockedExchange64(&...LastScanDurationUs, ScanDurationUs). ScanDurationUs
// still feeds MaxScanDurationUs and the trace, so it compiles. Direction (a)'s
// ordering assertion then compares zero against a real flush figure and fails.
//
// Full radius, all 47 cases run in isolation: 2 detected -- this case fails 3,
// QuiesceScanCost 1 on its sum assertion; the other 45 pass.
//
// Deletion criterion (b): delete the corresponding LastFlushDurationUs exchange.
// Direction (b)'s ordering and controlled-floor assertions fail.
//
// Full radius, all 47 cases run in isolation: 1 detected -- this case fails 3;
// the other 46 pass. Nothing else in the suite reads that field for anything
// but its sum, which is exactly why this case had to exist.
//
// Transposition criterion (c): swap the two exchanges, so LastScanDurationUs
// receives FlushDurationUs and vice versa. This is NOT a deletion and is
// recorded as such -- it removes no operation. It is included because it is the
// specific defect the sum assertion admits, and both directions must fail it or
// the split is decoration.
//
// Full radius, all 47 cases run in isolation: 2 detected -- this case fails 3,
// which is BOTH directions (direction (b)'s ordering and controlled floor, and
// direction (a)'s ordering), and QuiesceScanCost fails 1 incidentally on its
// Max-versus-Last assertion; the other 45 pass. Before this case existed the
// same transposition passed all 46.
//
static
VOID
XdpCpuMapTestQuiesceDurationAttribution(
    VOID
    )
{
#define XDPCPUMAP_TEST_ATTRIB_MAPS 2
#define XDPCPUMAP_TEST_ATTRIB_CPUS 8
#define XDPCPUMAP_TEST_ATTRIB_BURN_US 2000

    XDP_CPUMAP *CpuMaps[XDPCPUMAP_TEST_ATTRIB_MAPS];
    XDP_CPUMAP_PROVIDER_VALUE Values[XDPCPUMAP_TEST_ATTRIB_MAPS][XDPCPUMAP_TEST_ATTRIB_CPUS];
    XDP_CPUMAP_ENTRY_V1 Entry;
    NET_BUFFER_LIST *Sentinel = (NET_BUFFER_LIST *)(ULONG_PTR)0xF00DF00D;
    const UINT32 QuiescingToken = 0;
    const UINT32 OtherToken = 0;
    const UINT32 RingDepth = XDP_CPUMAP_RING_DEPTH_DEFAULT;
    XDP_CPUMAP_QUIESCE_STATS Before;
    XDP_CPUMAP_QUIESCE_STATS Stats;
    KDPC BurnDpc;
    LONG Baseline;

    XDPCPUMAP_TEST_BEGIN("QuiesceDurationAttribution");

    XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapStart()));
    Baseline = XdpCpuMapTestLiveAllocations;

    //
    // Direction (b) first, while the registry is still empty: the scan has
    // nothing at all to walk, so anything ScanUs reports beyond noise came from
    // somewhere it should not have.
    //
    XdpCpuMapTestBurnDpcMicroseconds = XDPCPUMAP_TEST_ATTRIB_BURN_US;
    XdpCpuMapTestBurnDpcRuns = 0;
    KeInitializeDpc(&BurnDpc, XdpCpuMapTestBurnDpcRoutine, NULL);
    XDPCPUMAP_TEST_ASSERT(KeInsertQueueDpc(&BurnDpc, NULL, NULL));

    XdpCpuMapQueryQuiesceStats(&Before);
    XdpCpuMapQuiesceInterface(&QuiescingToken);
    XdpCpuMapQueryQuiesceStats(&Stats);

    //
    // MapsVisited and EntriesScanned are process-wide aggregates, so they are
    // only meaningful as deltas. The three duration fields are per-event
    // exchanges and are read directly.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestBurnDpcRuns == 1);
    XDPCPUMAP_TEST_ASSERT(Stats.MapsVisited - Before.MapsVisited == 0);
    XDPCPUMAP_TEST_ASSERT(Stats.EntriesScanned - Before.EntriesScanned == 0);

    //
    // THE OUTCOME for (b): the cost was in the flush, and it is at least the
    // controlled interval the DPC was told to burn -- an ordering assertion
    // alone would admit a fixed offset.
    //
    XDPCPUMAP_TEST_ASSERT(Stats.LastFlushDurationUs > Stats.LastScanDurationUs);
    XDPCPUMAP_TEST_ASSERT(
        Stats.LastFlushDurationUs >= XDPCPUMAP_TEST_ATTRIB_BURN_US / 2);
    XDPCPUMAP_TEST_ASSERT(
        Stats.LastScanDurationUs + Stats.LastFlushDurationUs <= Stats.LastDurationUs);
    XDPCPUMAP_TEST_ASSERT(
        Stats.LastDurationUs <=
            Stats.LastScanDurationUs + Stats.LastFlushDurationUs + 1);

    //
    // Direction (a). Rings full of entries whose owner tokens deliberately do
    // NOT match, so every slot is compared, nothing is transferred, and no DPC
    // is ever queued -- the flush therefore runs nothing.
    //
    for (UINT32 M = 0; M < XDPCPUMAP_TEST_ATTRIB_MAPS; M++) {
        XDPCPUMAP_TEST_ASSERT(NT_SUCCESS(XdpCpuMapTestCreateMap(64, &CpuMaps[M])));

        for (UINT32 C = 0; C < XDPCPUMAP_TEST_ATTRIB_CPUS; C++) {
            XDP_CPUMAP_RING *Ring;

            Entry = XdpCpuMapTestEntry(C, RingDepth, 0);
            XDPCPUMAP_TEST_ASSERT(
                NT_SUCCESS(XdpCpuMapTestResolve(CpuMaps[M], &Entry, &Values[M][C])));

            Ring = Values[M][C].Target->Ring;
            for (UINT32 I = 0; I < RingDepth; I++) {
                Ring->Entries[I].Nbl = Sentinel;
                Ring->Entries[I].RxQueueOwner = &OtherToken;
                Ring->Entries[I].GenericOwner = &OtherToken;
            }
            Ring->Head = 0;
            Ring->Tail = RingDepth;
        }
    }

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestQueuedDpcCount == 0);

    XdpCpuMapQueryQuiesceStats(&Before);
    XdpCpuMapQuiesceInterface(&QuiescingToken);
    XdpCpuMapQueryQuiesceStats(&Stats);

    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestBurnDpcRuns == 1);

    //
    // THE OUTCOME for (a). A strict comparison, so it holds no escape for a
    // build that deleted the scan entirely: both terms would then be zero and
    // zero is not greater than zero.
    //
    XDPCPUMAP_TEST_ASSERT(Stats.LastScanDurationUs > Stats.LastFlushDurationUs);
    XDPCPUMAP_TEST_ASSERT(
        Stats.LastScanDurationUs + Stats.LastFlushDurationUs <= Stats.LastDurationUs);
    XDPCPUMAP_TEST_ASSERT(
        Stats.LastDurationUs <=
            Stats.LastScanDurationUs + Stats.LastFlushDurationUs + 1);

    //
    // SUPPORTING, for mutation rule 4: the scan really did the work this
    // direction attributes to it, rather than the ordering falling out of two
    // near-zero numbers.
    //
    XDPCPUMAP_TEST_ASSERT(
        Stats.EntriesScanned - Before.EntriesScanned ==
            (LONG64)XDPCPUMAP_TEST_ATTRIB_MAPS * XDPCPUMAP_TEST_ATTRIB_CPUS * RingDepth);

    //
    // Drain the synthetic entries before teardown: they hold no real references,
    // but leaving Tail ahead of Head would misrepresent the rings to destroy.
    //
    for (UINT32 M = 0; M < XDPCPUMAP_TEST_ATTRIB_MAPS; M++) {
        for (UINT32 C = 0; C < XDPCPUMAP_TEST_ATTRIB_CPUS; C++) {
            XDP_CPUMAP_RING *Ring = Values[M][C].Target->Ring;

            RtlZeroMemory(Ring->Entries, (SIZE_T)RingDepth * sizeof(XDP_CPUMAP_ENTRY));
            Ring->Head = 0;
            Ring->Tail = 0;
            XdpCpuMapTestRelease(CpuMaps[M], &Values[M][C]);
        }
    }

    XdpCpuMapTestDrainSweeps();

    for (UINT32 M = 0; M < XDPCPUMAP_TEST_ATTRIB_MAPS; M++) {
        XdpCpuMapTestDestroyMap(CpuMaps[M]);
    }

    XdpCpuMapTestDrainSweeps();
    XdpCpuMapTestDrainEpochFrees();
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestLiveAllocations == Baseline);
    XdpCpuMapStop();
}

typedef VOID (*XDPCPUMAP_TEST_ROUTINE)(VOID);

typedef struct _XDPCPUMAP_TEST_CASE {
    const CHAR *Name;
    XDPCPUMAP_TEST_ROUTINE Routine;
} XDPCPUMAP_TEST_CASE;

#define XDPCPUMAP_TEST_CASE_ENTRY(fn) { #fn, XdpCpuMapTest ## fn }

static const XDPCPUMAP_TEST_CASE XdpCpuMapTestCases[] = {
    XDPCPUMAP_TEST_CASE_ENTRY(ZeroSettingsInherit),
    XDPCPUMAP_TEST_CASE_ENTRY(InvalidCpuIndex),
    XDPCPUMAP_TEST_CASE_ENTRY(DpcTargetingFailure),
    XDPCPUMAP_TEST_CASE_ENTRY(SharedTargetAccounting),
    XDPCPUMAP_TEST_CASE_ENTRY(SweepRearm),
    XDPCPUMAP_TEST_CASE_ENTRY(CoalescedSweep),
    XDPCPUMAP_TEST_CASE_ENTRY(AllocationFailure),
    XDPCPUMAP_TEST_CASE_ENTRY(CapAccounting),
    XDPCPUMAP_TEST_CASE_ENTRY(QuiesceEmpty),
    XDPCPUMAP_TEST_CASE_ENTRY(QuiesceScanCost),
    XDPCPUMAP_TEST_CASE_ENTRY(TeardownWithoutEpoch),
    XDPCPUMAP_TEST_CASE_ENTRY(HelperTargetRundown),
    XDPCPUMAP_TEST_CASE_ENTRY(HelperContextOffsetGuard),
    XDPCPUMAP_TEST_CASE_ENTRY(HelperFindElement),
    XDPCPUMAP_TEST_CASE_ENTRY(HelperHighKeyRejected),
    XDPCPUMAP_TEST_CASE_ENTRY(HelperFailureFallbacks),
    XDPCPUMAP_TEST_CASE_ENTRY(HelperModeFallback),
    XDPCPUMAP_TEST_CASE_ENTRY(HelperDisallowedQueueNoLeak),
    XDPCPUMAP_TEST_CASE_ENTRY(HelperReplacementRelease),
    XDPCPUMAP_TEST_CASE_ENTRY(CommitInvalidMetadata),
    XDPCPUMAP_TEST_CASE_ENTRY(CommitRejectPaths),
    XDPCPUMAP_TEST_CASE_ENTRY(CommitNullActionNbl),
    XDPCPUMAP_TEST_CASE_ENTRY(CommitGroupBatching),
    XDPCPUMAP_TEST_CASE_ENTRY(CommitFrameReuse),
    XDPCPUMAP_TEST_CASE_ENTRY(DeepCopySuccess),
    XDPCPUMAP_TEST_CASE_ENTRY(DeepCopyFailurePaths),
    XDPCPUMAP_TEST_CASE_ENTRY(DeepCopyTeardownRecycle),
    XDPCPUMAP_TEST_CASE_ENTRY(DeepCopyCacheReuseAndCap),
    XDPCPUMAP_TEST_CASE_ENTRY(CommitGroupUnusedIsFree),
    XDPCPUMAP_TEST_CASE_ENTRY(TargetDpcAffinity),
    XDPCPUMAP_TEST_CASE_ENTRY(ZeroCopyOwnershipLifecycle),
    XDPCPUMAP_TEST_CASE_ENTRY(ZeroCopyPostCommitRejection),
    XDPCPUMAP_TEST_CASE_ENTRY(ZeroCopyTeardownDisposal),
    XDPCPUMAP_TEST_CASE_ENTRY(EnqueueInsertOrdering),
    XDPCPUMAP_TEST_CASE_ENTRY(EnqueueTargetInactive),
    XDPCPUMAP_TEST_CASE_ENTRY(EnqueueRingFull),
    XDPCPUMAP_TEST_CASE_ENTRY(DrainPartitionedIndication),
    XDPCPUMAP_TEST_CASE_ENTRY(DrainTombstoneSkip),
    XDPCPUMAP_TEST_CASE_ENTRY(DrainYieldRequeueGate),
    XDPCPUMAP_TEST_CASE_ENTRY(RetireDrainReturns),
    XDPCPUMAP_TEST_CASE_ENTRY(RetireSuppressedTargetDpc),
    XDPCPUMAP_TEST_CASE_ENTRY(QuiesceScoping),
    XDPCPUMAP_TEST_CASE_ENTRY(QuiesceInterfaceScope),
    XDPCPUMAP_TEST_CASE_ENTRY(QuiesceTailSnapshot),
    XDPCPUMAP_TEST_CASE_ENTRY(QuiescePassBudget),
    XDPCPUMAP_TEST_CASE_ENTRY(QuiesceTombstoneBalance),
    XDPCPUMAP_TEST_CASE_ENTRY(QuiesceDurationAttribution),
};

INT
__cdecl
main(
    int argc,
    char **argv
    )
{
    //
    // An optional argument selects tests by exact name.
    //
    // This exists because of a deletion criterion that reported ZERO failures
    // while actually being detected: widening the quiesce scope predicate makes
    // QuiesceScanCost dereference the poison NBLs it deliberately plants, so the
    // process died by access violation before any later test ran, and a runner
    // that only counted FAIL lines recorded that as "not detected". Being unable
    // to run one test in isolation makes every criterion's blast radius a lower
    // bound rather than a measurement, which is precisely the "harness weaker
    // than the runtime it models" failure this project keeps hitting.
    //
    // The filter measures a criterion by running EVERY case in isolation and
    // typing each outcome as pass, assertion failure, crash or hang. It is not a
    // way to run only the case a criterion is expected to hit: a filtered result
    // is evidence about that case alone and says nothing about the other 46, and
    // reporting one as a whole-suite radius is how criterion 22's radius was
    // first understated. Baseline: all 47 pass in isolation; an unknown name
    // exits 2.
    //
    const CHAR *Filter = (argc > 1) ? argv[1] : NULL;
    BOOLEAN Matched = FALSE;

    for (SIZE_T Index = 0; Index < RTL_NUMBER_OF(XdpCpuMapTestCases); Index++) {
        const XDPCPUMAP_TEST_CASE *Case = &XdpCpuMapTestCases[Index];

        if (Filter != NULL && strcmp(Filter, Case->Name) != 0) {
            continue;
        }

        Matched = TRUE;
        Case->Routine();
    }

    if (Filter != NULL && !Matched) {
        printf("cpumaptest: no test named '%s'\n", Filter);
        return 2;
    }

    XdpCpuMapTestCurrent = "<summary>";

    //
    // Every epoch region entered by a test or by the code under test must have
    // been exited. An imbalance here means a path entered a region and returned
    // without leaving it, which in production would pin reclamation forever.
    //
    XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochDepth == 0);

    if (Filter == NULL) {
        XDPCPUMAP_TEST_ASSERT(XdpCpuMapTestEpochOperations > 0);
    }

    if (XdpCpuMapTestFailures > 0) {
        printf("cpumaptest: %u test(s) run, %u FAILURES\n",
            XdpCpuMapTestRun, XdpCpuMapTestFailures);
        return 1;
    }

    printf("cpumaptest: %u test(s) run, all passed\n", XdpCpuMapTestRun);
    return 0;
}
