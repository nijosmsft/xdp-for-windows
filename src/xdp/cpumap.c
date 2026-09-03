//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// CPUMAP engine: target lifetime, resource accounting, the retire sweep, the
// live-map registry, quiesce, and the packet data path.
//
// This module owns everything about a CPUMAP that is not the eBPF provider
// surface. The provider callbacks live in ebpfcpumap.c and call in here.
//
// Increment scope: the generic data path. The helper pins a target, the
// ownership commit accumulates the packet into a per-flush batch, the flush
// performs the six-step enqueue of design section 7, and the target CPU's DPC
// drains the ring and indicates. Deep copy is not implemented: a low-resource
// indication makes the commit decline ownership, counted, and the packet is
// dropped by the caller's already-decided RX action until the per-receive-queue
// pool lands. Like every commit-time outcome that is a drop, not a fallback --
// XdpInvokeEbpf converts XDP_REDIRECT to XDP_RX_ACTION_DROP before
// post-inspection runs, so no declared fallback action remains.
//
// Lock ordering (design section 8.3). Two disjoint chains sharing Ring->Lock as
// a common leaf; they are never both held:
//
//   Control chain (eBPF map callbacks, retire sweep):
//     XdpCpuMapGlobalLock -> CpuMap->ConfigLock -> Ring->Lock
//
//   Quiesce chain (entered from the xdplwf pause path):
//     Generic->Lock (held by caller) -> XdpCpuMapRegistryLock
//                                    -> CpuMap->ConfigLock -> Ring->Lock
//
// CPUMAP never acquires Generic->Lock or RxQueue->EcLock; both may already be
// held by the caller of quiesce.
//

#include "precomp.h"
#include "cpumap.tmh"

//
// Global cap accounting. Guarded by XdpCpuMapGlobalLock, which is OUTERMOST in
// the control chain -- not a leaf. It is PASSIVE-only and is never taken by the
// data path or by quiesce, so making it outermost costs only serialization of
// control-plane updates, in exchange for making the charge atomic with the
// decide-and-allocate step.
//
static EX_PUSH_LOCK XdpCpuMapGlobalLock;
static UINT32 XdpCpuMapGlobalRingEntries;
static SIZE_T XdpCpuMapGlobalNonPagedBytes;

//
// The live-map registry. A fixed array with a live count, iterated BY INDEX so
// quiesce needs no snapshot allocation and therefore has no allocation-failure
// contract to define. Quiesce has no failure mode and no return value, which it
// must not have: pause correctness depends on it completing.
//
static EX_PUSH_LOCK XdpCpuMapRegistryLock;
static XDP_CPUMAP *XdpCpuMapRegistry[XDP_CPUMAP_MAX_LIVE_MAPS];
static UINT32 XdpCpuMapRegistryCount;
static UINT32 XdpCpuMapNextId;

static
SIZE_T
XdpCpuMapGetPoolChargeSize(
    _In_ SIZE_T Size
    )
{
    return
        (Size + MEMORY_ALLOCATION_ALIGNMENT - 1) &
        ~((SIZE_T)MEMORY_ALLOCATION_ALIGNMENT - 1);
}

static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapReleaseBackingRefs(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ LONG Count
    );

//
// One retire work queue for every map, owned by this module rather than by any
// map, so a map being destroyed never has to tear down the queue that is
// sweeping it.
//
// It MUST be DISPATCH-capable. XdpLifetimeDelete cannot be used: its queue is
// created with MaxIrql = PASSIVE_LEVEL, so XdpWorkQueueAcquireLock takes an
// EX_PUSH_LOCK, which is illegal beneath the base-map bucket spin lock that can
// deliver postprocess_map_delete_element. At MaxIrql >= DISPATCH_LEVEL the work
// queue uses a KSPIN_LOCK instead, and the routine still runs at PASSIVE_LEVEL
// because dispatch goes through IoQueueWorkItemEx.
//
static XDP_WORK_QUEUE *XdpCpuMapRetireQueue;

//
// Quiesce instrumentation. The open architectural risk in this design is pause
// latency, and the bound claimed is one pointer comparison per ring entry
// bounded by XDP_CPUMAP_GLOBAL_MAX_RING_ENTRIES and XDP_CPUMAP_MAX_LIVE_MAPS.
// These counters exist so that bound can be MEASURED rather than asserted; they
// are traced at the end of every quiesce.
//

//
// Quiesce instrumentation; the counter block is declared in cpumap.h so tests
// and diagnostics can read it.
//
static XDP_CPUMAP_QUIESCE_STATS XdpCpuMapQuiesceStats;

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapQueryQuiesceStats(
    _Out_ XDP_CPUMAP_QUIESCE_STATS *Stats
    )
{
    *Stats = XdpCpuMapQuiesceStats;
}

//
// Sweep instrumentation. EmptySweepCount and RetireCheckNoOp are DIFFERENT
// conditions and must not share a counter: an empty sweep found no pending
// releases at all, whereas a no-op retire check applied releases and found the
// target resurrected or still referenced by another key.
//
typedef struct _XDP_CPUMAP_SWEEP_STATS {
    volatile LONG64 EmptySweepCount;
    volatile LONG64 RetireCheckNoOp;

    //
    // Packets returned to the miniport WITHOUT indication because their target
    // retired while its ring still held them (section 8.3, "Delivery on
    // retire"). Interlocked rather than sharded: the retire worker runs at
    // PASSIVE_LEVEL, where it can be preempted and migrated, so the
    // one-writer-per-shard property the data-path counters rely on does not
    // hold here.
    //
    volatile LONG64 RetireDropCount;
} XDP_CPUMAP_SWEEP_STATS;

static XDP_CPUMAP_SWEEP_STATS XdpCpuMapSweepStats;

//
// Helper fallback diagnostics. These per-map counters are fallback reasons:
// they record why bpf_redirect_map declined to set a CPUMAP redirect intent.
// They are not enqueue/drop counters, because the packet's outcome is the
// program-selected fallback action (PASS, DROP, or TX).
//
// The helper packet path updates exactly one shard: the current processor's
// cache-aligned slot. At DISPATCH_LEVEL that shard has one running writer, so
// these are ordinary writes rather than locked RMWs. Readers aggregate all
// shards with aligned 64-bit loads; the result is a coherent monotonic total,
// not a stop-the-world snapshot.
//
static
_IRQL_requires_max_(DISPATCH_LEVEL)
XDP_CPUMAP_HELPER_STATS *
XdpCpuMapGetCurrentHelperStats(
    _Inout_ XDP_CPUMAP *CpuMap
    )
{
    ULONG ProcessorIndex = KeGetCurrentProcessorIndex();

    ASSERT(CpuMap->HelperStats != NULL);
    ASSERT(CpuMap->HelperStatsCount > 0);
    ASSERT(ProcessorIndex < CpuMap->HelperStatsCount);

    if (ProcessorIndex >= CpuMap->HelperStatsCount) {
        ProcessorIndex = 0;
    }

    return &CpuMap->HelperStats[ProcessorIndex];
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapRecordHelperCall(
    _Inout_ XDP_CPUMAP *CpuMap
    )
{
    XdpCpuMapGetCurrentHelperStats(CpuMap)->Calls++;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapRecordHelperSuccess(
    _Inout_ XDP_CPUMAP *CpuMap
    )
{
    XdpCpuMapGetCurrentHelperStats(CpuMap)->Success++;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapRecordHelperFallback(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ XDP_CPUMAP_HELPER_FALLBACK_REASON Reason
    )
{
    XDP_CPUMAP_HELPER_STATS *Stats = XdpCpuMapGetCurrentHelperStats(CpuMap);
    volatile ULONG64 *Counter = NULL;

    switch (Reason) {
    case XdpCpuMapHelperFallbackBadFlags:
        Counter = &Stats->HelperBadFlags;
        break;
    case XdpCpuMapHelperFallbackRedirectSlotUnconfigured:
        Counter = &Stats->RedirectSlotUnconfigured;
        break;
    case XdpCpuMapHelperFallbackRedirectModeUnsupported:
        Counter = &Stats->RedirectModeUnsupported;
        break;
    case XdpCpuMapHelperFallbackTargetInactive:
        Counter = &Stats->HelperTargetInactive;
        break;
    default:
        ASSERT(FALSE);
        return;
    }

    (*Counter)++;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapQueryHelperStats(
    _In_ const XDP_CPUMAP *CpuMap,
    _Out_ XDP_CPUMAP_HELPER_STATS *Stats
    )
{
    RtlZeroMemory(Stats, sizeof(*Stats));

    for (UINT32 Index = 0; Index < CpuMap->HelperStatsCount; Index++) {
        const XDP_CPUMAP_HELPER_STATS *Current = &CpuMap->HelperStats[Index];

        Stats->Calls += ReadULong64NoFence(&Current->Calls);
        Stats->Success += ReadULong64NoFence(&Current->Success);
        Stats->HelperBadFlags += ReadULong64NoFence(&Current->HelperBadFlags);
        Stats->RedirectSlotUnconfigured +=
            ReadULong64NoFence(&Current->RedirectSlotUnconfigured);
        Stats->RedirectModeUnsupported +=
            ReadULong64NoFence(&Current->RedirectModeUnsupported);
        Stats->HelperTargetInactive += ReadULong64NoFence(&Current->HelperTargetInactive);
        Stats->CommitPauseDrop += ReadULong64NoFence(&Current->CommitPauseDrop);
        Stats->CommitRundownDrop += ReadULong64NoFence(&Current->CommitRundownDrop);
        Stats->EnqueueCount += ReadULong64NoFence(&Current->EnqueueCount);
        Stats->EnqueueTargetInactive += ReadULong64NoFence(&Current->EnqueueTargetInactive);
        Stats->RingFullCount += ReadULong64NoFence(&Current->RingFullCount);
        Stats->DeepCopyBuildCount += ReadULong64NoFence(&Current->DeepCopyBuildCount);
        Stats->DeepCopyFailCount += ReadULong64NoFence(&Current->DeepCopyFailCount);
        Stats->DeepCopyMetadataUnsupported +=
            ReadULong64NoFence(&Current->DeepCopyMetadataUnsupported);
        Stats->DeepCopyDescriptorResidue +=
            ReadULong64NoFence(&Current->DeepCopyDescriptorResidue);
        Stats->DrainCount += ReadULong64NoFence(&Current->DrainCount);
        Stats->DrainTombstoneCount += ReadULong64NoFence(&Current->DrainTombstoneCount);
        Stats->IndicateChainCount += ReadULong64NoFence(&Current->IndicateChainCount);
        Stats->DpcInvokeCount += ReadULong64NoFence(&Current->DpcInvokeCount);
        Stats->DpcRequeueCount += ReadULong64NoFence(&Current->DpcRequeueCount);
        Stats->DpcEmptyCount += ReadULong64NoFence(&Current->DpcEmptyCount);
    }
}

static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapRecordCommitReject(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ XDP_CPUMAP_COMMIT_REJECT_REASON Reason
    )
{
    XDP_CPUMAP_HELPER_STATS *Stats = XdpCpuMapGetCurrentHelperStats(CpuMap);
    volatile ULONG64 *Counter = NULL;

    switch (Reason) {
    case XdpCpuMapCommitRejectPause:
        Counter = &Stats->CommitPauseDrop;
        break;
    case XdpCpuMapCommitRejectRundown:
        Counter = &Stats->CommitRundownDrop;
        break;
    default:
        ASSERT(FALSE);
        return;
    }

    (*Counter)++;
}

//
// Deep-copy cache (section 7, "NDIS resources"; section 8.1a row 9b).
//
// The pool belongs to the LWF receive queue, but the allocate/copy/recycle
// mechanics live here because the flush and the drain are the only callers and
// both are CPUMAP code. xdplwf creates and destroys the pool; it never touches
// a copy.
//

_IRQL_requires_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
XdpCpuMapDeepCopyPoolInitialize(
    _Out_ XDP_CPUMAP_DEEPCOPY_POOL *Pool,
    _In_ NDIS_HANDLE NdisFilterHandle
    )
{
    NET_BUFFER_LIST_POOL_PARAMETERS PoolParams = {0};

    RtlZeroMemory(Pool, sizeof(*Pool));
    InitializeSListHead(&Pool->FreeList);
    Pool->NdisFilterHandle = NdisFilterHandle;

    PoolParams.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    PoolParams.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    PoolParams.Header.Size = sizeof(PoolParams);
    PoolParams.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    PoolParams.PoolTag = XDP_POOLTAG_CPUMAP_DEEPCOPY;

    //
    // A NET_BUFFER is allocated with each NBL and no context is reserved: a deep
    // copy carries payload and RSS metadata, never an XDP-private context. Data
    // buffers are NOT pre-allocated; NdisRetreatNetBufferDataStart supplies
    // pages per copy and the recycle gives them straight back.
    //
    PoolParams.fAllocateNetBuffer = TRUE;
    PoolParams.ContextSize = 0;
    PoolParams.DataSize = 0;

    Pool->NblPool = NdisAllocateNetBufferListPool(NdisFilterHandle, &PoolParams);
    if (Pool->NblPool == NULL) {
        return STATUS_NO_MEMORY;
    }

    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapDeepCopyPoolCleanup(
    _Inout_ XDP_CPUMAP_DEEPCOPY_POOL *Pool
    )
{
    NET_BUFFER_LIST *Cached;

    //
    // Every descriptor is bare here: a copy holds pages only between its retreat
    // and its recycle, and both the drain and the teardown paths recycle before
    // releasing the NblRundown reference that gates this queue's deletion. So
    // nothing in either list can still own pages.
    //
    Cached = (NET_BUFFER_LIST *)InterlockedFlushSList(&Pool->FreeList);
    while (Cached != NULL) {
        NET_BUFFER_LIST *Next = Cached->Next;

        ASSERT(NET_BUFFER_LIST_FIRST_NB(Cached)->MdlChain == NULL);
        NdisFreeNetBufferList(Cached);
        Cached = Next;
    }

    Cached = Pool->LocalList;
    Pool->LocalList = NULL;
    while (Cached != NULL) {
        NET_BUFFER_LIST *Next = Cached->Next;

        ASSERT(NET_BUFFER_LIST_FIRST_NB(Cached)->MdlChain == NULL);
        NdisFreeNetBufferList(Cached);
        Cached = Next;
    }

    if (Pool->NblPool != NULL) {
        NdisFreeNetBufferListPool(Pool->NblPool);
        Pool->NblPool = NULL;
    }
}

//
// Return a bare descriptor to the cache.
//
// Pushed to the interlocked list because the caller is usually the drain DPC on
// the TARGET CPU, while the allocating flush runs on the source CPU. The
// allocator drains this list in bulk, so the interlocked cost here is not paid
// again per allocation.
//
static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapDeepCopyPushFree(
    _Inout_ XDP_CPUMAP_DEEPCOPY_POOL *Pool,
    _Inout_ NET_BUFFER_LIST *Nbl
    )
{
    //
    // The NBL's Next field doubles as the SLIST_ENTRY, which is what upstream's
    // TxCloneNblSList already does. SLIST_ENTRY must be 16-byte aligned on x64,
    // and Next is at offset 0 of a pool-allocated NBL, so the descriptor's own
    // alignment is the guarantee.
    //
    ASSERT(((ULONG_PTR)Nbl & (MEMORY_ALLOCATION_ALIGNMENT - 1)) == 0);
    ASSERT(NET_BUFFER_LIST_FIRST_NB(Nbl)->MdlChain == NULL);

    InterlockedPushEntrySList(&Pool->FreeList, (PSLIST_ENTRY)&Nbl->Next);
}

//
// Release a completed deep copy: give its pages back, then cache the descriptor.
//
static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapDeepCopyRecycle(
    _Inout_ XDP_CPUMAP_DEEPCOPY_POOL *Pool,
    _Inout_ NET_BUFFER_LIST *Nbl
    )
{
    NET_BUFFER *Nb = NET_BUFFER_LIST_FIRST_NB(Nbl);

    //
    // FreeMdl is TRUE: the MDL and pages came from this copy's own retreat, so
    // advancing the full data length gives them back to NDIS and leaves a bare
    // descriptor. Advancing by DataLength rather than a remembered retreat size
    // is correct because nothing on the redirect path trims a copy.
    //
    NdisAdvanceNetBufferDataStart(Nb, NET_BUFFER_DATA_LENGTH(Nb), TRUE, NULL);

    NET_BUFFER_LIST_NEXT_NBL(Nbl) = NULL;
    XdpCpuMapDeepCopyPushFree(Pool, Nbl);
}

//
// The receive OOB slots a deep copy carries.
//
// Every one is a VALUE packed into the pointer-sized entry -- a checksum result
// union, an 802.1Q priority/VID union, a frame type, a filtering-info union, an
// RSS hash value and the packed hash type/function word. None of them owns
// storage, so carrying one costs nothing to release and clearing one on reuse
// discharges no obligation.
//
// Anything NOT in this table causes the redirect to be refused rather than
// carried; see XdpCpuMapDeepCopyAllocate. That is what makes the list safe to
// maintain by hand: a slot nobody has classified fails closed instead of being
// copied on a guess.
//
static const NDIS_NET_BUFFER_LIST_INFO XdpCpuMapDeepCopyCarriedSlots[] = {
    TcpIpChecksumNetBufferListInfo,
    Ieee8021QNetBufferListInfo,
    NetBufferListFrameType,
    NetBufferListFilteringInfo,
    NetBufferListHashValue,
    NetBufferListHashInfo,
};

static
BOOLEAN
XdpCpuMapDeepCopyIsCarriedSlot(
    _In_ ULONG Slot
    )
{
    for (ULONG Index = 0; Index < RTL_NUMBER_OF(XdpCpuMapDeepCopyCarriedSlots); Index++) {
        if ((ULONG)XdpCpuMapDeepCopyCarriedSlots[Index] == Slot) {
            return TRUE;
        }
    }

    return FALSE;
}

static
BOOLEAN
XdpCpuMapDeepCopyIsDescriptorClean(
    _In_ const NET_BUFFER_LIST *Nbl,
    _Out_opt_ ULONG *DirtySlot
    )
{
    if (DirtySlot != NULL) {
        *DirtySlot = MaxNetBufferListInfo;
    }

    for (ULONG Slot = 0; Slot < MaxNetBufferListInfo; Slot++) {
        if (XdpCpuMapDeepCopyIsCarriedSlot(Slot)) {
            //
            // Ours: set by the carry loop and cleared before it. Whatever is
            // here now is this code's own leftover and is safe to overwrite.
            //
            continue;
        }

        if (Nbl->NetBufferListInfo[Slot] != NULL) {
            //
            // Report WHICH slot, for the same reason the metadata refusal does:
            // "a descriptor was dirty" says something broke, "slot N was dirty"
            // says what to go and look at.
            //
            if (DirtySlot != NULL) {
                *DirtySlot = Slot;
            }
            return FALSE;
        }
    }

    return TRUE;
}

//
// Build an independent copy of Original from Pool, or return NULL.
//
// Runs in the flush's pre-pass, OUTSIDE the ring lock: it allocates, retreats
// and memcpys, none of which belongs under a spinlock the drain also contends
// for. POC A does the same, in the phase before its per-target enqueue.
//
static
_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NET_BUFFER_LIST *
XdpCpuMapDeepCopyAllocate(
    _Inout_ XDP_CPUMAP_DEEPCOPY_POOL *Pool,
    _In_ NET_BUFFER_LIST *Original,
    _Inout_ XDP_CPUMAP_HELPER_STATS *Stats
    )
{
    NET_BUFFER_LIST *Copy;
    NET_BUFFER *SourceNb = NET_BUFFER_LIST_FIRST_NB(Original);
    NET_BUFFER *CopyNb;
    ULONG DataLength;
    ULONG DirtySlot = MaxNetBufferListInfo;

    //
    // A low-resource indication carries one NB, and the receive path asserts it
    // (recv.c, "only one NB should be processed at a time"). Copying only the
    // first would silently truncate, so refuse rather than guess.
    //
    if (SourceNb == NULL || NET_BUFFER_NEXT_NB(SourceNb) != NULL) {
        return NULL;
    }

    DataLength = NET_BUFFER_DATA_LENGTH(SourceNb);
    if (DataLength == 0) {
        return NULL;
    }

    //
    // RULE 2 -- refuse the redirect if the original carries anything else.
    //
    // The drift protection rule 1 gives up is bought back by refusing rather
    // than guessing. If a slot we do not carry is occupied we cannot know
    // whether it is load-bearing, so we decline to build a copy instead of
    // delivering a packet we have quietly damaged. That covers
    // MediaSpecificInformation and its Ex form, the WFP context, and every slot
    // a future NDIS version adds -- new slots fail closed.
    //
    // This is deliberately NOT an assertion. A conforming miniport supplying
    // media-specific information on a receive is VALID EXTERNAL INPUT, and a
    // checked build must never bugcheck on input a peer driver is entitled to
    // produce. Asserting would turn a legitimate configuration into a crash.
    //
    // What it is instead is a counted DROP, and the packet is LOST. It is not
    // `delivered but not steered'': XdpInvokeEbpf already converted the
    // program's XDP_REDIRECT into XDP_RX_ACTION_DROP before post-inspection ran,
    // so by the time this refusal fires the caller's action is drop and there is
    // no fallback left to honour. The original goes back to the miniport exactly
    // once and nothing is delivered. Calling it graceful degradation would be
    // the same overstatement the design carried until Ash caught it.
    //
    // ORDERING IS LOAD-BEARING, for two independent reasons.
    //
    // It must stay BEFORE the descriptor is taken, so refusing costs no retreat
    // and leaves no partially-built copy to unwind. Placing it after the retreat
    // leaked a page on every refusal -- the exact row 9b failure the design
    // calls out.
    //
    // Its domain stays "every slot" so that metadata a future NDIS version adds
    // fails CLOSED. Narrowing it to a known list would silently start carrying
    // whatever appears next.
    //
    // N.B. an earlier revision claimed these facts also made the carry loop
    // below unobservable, and retired its deletion criterion as vacuous. That
    // was WRONG. The argument compared the carry loop against
    // NdisCopyReceiveNetBufferListInfo and found them equivalent -- but a
    // deletion criterion DELETES the operation, it does not swap it for another
    // correct one, and two equivalent implementations say nothing about whether
    // the step is load-bearing. Deleting the carry loop outright fails six
    // assertions, one per carried slot. The criterion is reinstated.
    //
    for (ULONG Slot = 0; Slot < MaxNetBufferListInfo; Slot++) {
        if (XdpCpuMapDeepCopyIsCarriedSlot(Slot)) {
            continue;
        }

        if (Original->NetBufferListInfo[Slot] != NULL) {
            Stats->DeepCopyMetadataUnsupported++;

            //
            // Section 12 observability. The offending SLOT is the whole point:
            // section 12 says a non-zero DeepCopyMetadataUnsupported means the
            // carried set needs revisiting, and that guidance is unusable
            // without knowing WHICH slot forced the refusal. Logging the count
            // alone would say something is wrong but not what to change.
            //
            TraceVerbose(
                TRACE_CORE,
                "DeepCopy refuse Original=%p Reason=MetadataUnsupported Slot=%u",
                Original, Slot);
            return NULL;
        }
    }

    if (Pool->LocalList == NULL) {
        Pool->LocalList = (NET_BUFFER_LIST *)InterlockedFlushSList(&Pool->FreeList);
    }

    //
    // Take a descriptor we can prove is clean, discarding any we cannot -- and
    // apply that test UNIFORMLY, to freshly allocated descriptors as well as
    // recycled ones.
    //
    // A cached descriptor can be dirty in slots this code never sets: the upper
    // stack stamps its own metadata during an indication, and nothing requires
    // it to clear the slot afterwards. Those slots are NOT ours to zero -- the
    // array holds entries with mixed ownership, and blanket-clearing one that
    // NDIS reference-manages would bypass its handling and leak the reference.
    // So a dirty descriptor is not repaired, it is FREED through
    // NdisFreeNetBufferList, the documented disposal for a pool-allocated NBL,
    // which runs whatever cleanup NDIS owns.
    //
    // The fresh path gets the same test for a narrower reason. Drivers rely on
    // NdisAllocateNetBufferAndNetBufferList returning zeroed NetBufferListInfo
    // slots, and it evidently does, but Microsoft documents initialization only
    // for specific fields such as Scratch and says nothing about that array. An
    // earlier revision of this code argued the behaviour must hold because
    // residue would leave no correct handling available -- which reasons from
    // "this must be so" to "this is contractually so", and that is the same step
    // that made an earlier, wrong metadata policy look reasonable. Checking
    // costs one pass over an array on an exceptional path and removes the
    // dependency entirely: if NDIS always zeroes, this never fires.
    //
    while (Pool->LocalList != NULL) {
        Copy = Pool->LocalList;
        Pool->LocalList = Copy->Next;

        if (XdpCpuMapDeepCopyIsDescriptorClean(Copy, NULL)) {
            goto Found;
        }

        Copy->Next = NULL;
        NdisFreeNetBufferList(Copy);
        ASSERT(Pool->CacheCount > 0);
        Pool->CacheCount--;
    }

    if (Pool->CacheCount < XDP_CPUMAP_DEEPCOPY_CACHE_MAX) {
        Copy = NdisAllocateNetBufferAndNetBufferList(Pool->NblPool, 0, 0, NULL, 0, 0);
        if (Copy == NULL) {
            return NULL;
        }

        Pool->CacheCount++;

        //
        // A dirty FRESH descriptor fails the copy; it is not freed and retried.
        //
        // Retrying would loop for as long as the allocator kept handing back
        // residue, on a path that runs at DISPATCH_LEVEL. One counted loss is
        // the bounded outcome, and it is the honest one: if this ever fires, the
        // allocator assumption above has broken and we want a counter saying so,
        // not a spin.
        //
        if (!XdpCpuMapDeepCopyIsDescriptorClean(Copy, &DirtySlot)) {
            Copy->Next = NULL;
            NdisFreeNetBufferList(Copy);
            Pool->CacheCount--;
            Stats->DeepCopyDescriptorResidue++;

            //
            // Section 12 observability. Distinct from the refusal above and
            // deliberately so: this says the ALLOCATOR handed back a descriptor
            // with residue, which means the assumption behind the uniform
            // cleanliness check has broken. The response is to investigate that,
            // never to widen the carried set, so the two must be
            // distinguishable in a trace and not just in a counter.
            //
            // The dirty slot is reported for the same reason the metadata
            // refusal reports its offending slot: the counter says something
            // broke, the slot says where to look.
            //
            TraceVerbose(
                TRACE_CORE,
                "DeepCopy refuse Original=%p Copy=%p Reason=DescriptorResidue Slot=%u Pool=%p",
                Original, Copy, DirtySlot, Pool);
            return NULL;
        }
    } else {
        //
        // Cache cap reached. A counted, non-fatal drop: CanPend == FALSE is an
        // exceptional path and the cap exists so a burst of it cannot consume
        // unbounded non-paged pool.
        //
        return NULL;
    }

Found:

    CopyNb = NET_BUFFER_LIST_FIRST_NB(Copy);

    //
    // A recycled descriptor carries the previous copy's stale geometry. Retreat
    // reads CurrentMdl and the offsets, so they have to be cleared rather than
    // assumed zero.
    //
    CopyNb->MdlChain = NULL;
    CopyNb->CurrentMdl = NULL;
    CopyNb->DataLength = 0;
    CopyNb->DataOffset = 0;
    CopyNb->CurrentMdlOffset = 0;
    NET_BUFFER_LIST_NEXT_NBL(Copy) = NULL;

    //
    // Clear ONLY the slots this code sets, and only those.
    //
    // A recycled descriptor carries values in the slots the previous copy
    // carried, and a fresh one carries whatever the allocator left. Both must go
    // before the carry loop runs, or a slot the source leaves empty would keep a
    // stale value from the packet before it -- a cross-packet metadata leak
    // through the cache.
    //
    // It is a targeted clear rather than an RtlZeroMemory over the whole array,
    // and the distinction is load-bearing. The array holds entries with MIXED
    // ownership: some are values, some are NDIS-managed references, some are
    // reserved. Blanket-zeroing would clobber a referenced WfpNetBufferListInfo
    // on a recycled descriptor and bypass NDIS's reference handling entirely,
    // leaking it. This code never puts anything in those slots -- rule 2 refuses
    // the redirect instead -- so a descriptor reaching here can only be dirty in
    // slots this loop owns.
    //
    for (ULONG Index = 0; Index < RTL_NUMBER_OF(XdpCpuMapDeepCopyCarriedSlots); Index++) {
        Copy->NetBufferListInfo[XdpCpuMapDeepCopyCarriedSlots[Index]] = NULL;
    }

    if (NdisRetreatNetBufferDataStart(CopyNb, DataLength, 0, NULL) != NDIS_STATUS_SUCCESS) {
        //
        // Section 8.1a row 9b: the retreat failed, so no pages were attached and
        // the descriptor is still bare. It goes back on the free list rather
        // than being leaked -- this is the "partially built copy" obligation in
        // its no-pages form.
        //
        XdpCpuMapDeepCopyPushFree(Pool, Copy);
        return NULL;
    }

#pragma prefast(suppress:6387, "NdisRetreatNetBufferDataStart sets CurrentMdl on success.")
    if (!NT_SUCCESS(
            MdlCopyMdlChainToMdlChainAtOffsetNonTemporal(
                CopyNb->CurrentMdl, CopyNb->CurrentMdlOffset,
                SourceNb->CurrentMdl, SourceNb->CurrentMdlOffset, DataLength))) {
        //
        // The retreat SUCCEEDED and a later step did not. This is the exact case
        // row 9b calls out as the one that leaks if unhandled: the pages are
        // already attached, so the descriptor must have its data start advanced
        // back before it can be cached.
        //
        XdpCpuMapDeepCopyRecycle(Pool, Copy);
        return NULL;
    }

    //
    // Receive metadata policy: carry values, refuse everything else.
    //
    // Rule 2 above has already established that every non-carried slot of the
    // source is empty, so what remains is to move the carried ones.
    //
    // N.B. an earlier revision justified copying the hash slots raw on the
    // grounds that NET_BUFFER_LIST_SET_HASH_TYPE and _SET_HASH_FUNCTION share
    // one slot and clobber each other. That is FALSE: nblhash.h shows each is a
    // read-modify-write masked to its own field
    // (`... & ~NDIS_HASH_TYPE_MASK) | (HashType & NDIS_HASH_TYPE_MASK)`), so
    // they compose safely. POC A's lost hash function is a real defect with a
    // different cause -- it never calls _SET_HASH_FUNCTION at all.
    //
    //
    // RULE 1 -- carry an explicit set of VALUE-typed slots, by hand.
    //
    // An earlier revision delegated to NdisCopyReceiveNetBufferListInfo, on the
    // grounds that "whatever NDIS considers receive metadata" cannot drift the
    // way a hand-written list can. That is true, and it is not sufficient: the
    // routine also brings OWNERSHIP we have no way to discharge. Verified
    // against ndis.sys 10.0.26100.8521, it moves seventeen slots with plain
    // load/store pairs and handles WfpNetBufferListInfo specially -- masked,
    // tag-bit tested, routed through a call -- which is NDIS taking a REFERENCE
    // on the WFP context. A copy that acquires that reference and is then
    // refused by the ring is recycled into this pool, and nothing documented
    // lets us release it from a cached descriptor. Delegating trades a drift
    // risk for a reference leak.
    //
    // Every slot carried below is a VALUE packed into the pointer-sized entry.
    // A copy that carries one owns nothing, and a descriptor that is reused can
    // have it overwritten with no cleanup obligation at all.

    for (ULONG Index = 0; Index < RTL_NUMBER_OF(XdpCpuMapDeepCopyCarriedSlots); Index++) {
        NDIS_NET_BUFFER_LIST_INFO Slot = XdpCpuMapDeepCopyCarriedSlots[Index];

        Copy->NetBufferListInfo[Slot] = Original->NetBufferListInfo[Slot];
    }

    //
    // Ownership, not metadata.
    //
    Copy->SourceHandle = Pool->NdisFilterHandle;

    return Copy;
}

//
// Flush the batch. Design section 7, "Batch enqueue": six steps, in this order,
// once per unique target in the batch.
//
// Every entry in the batch already carries ONE helper-acquired
// Target->PacketRundown reference, and the group holds them all until step 6.
// That ordering is precisely what makes the KeInsertQueueDpc in step 5 safe
// without a separate producer reference: while the group still holds a
// reference, the retire path's ExWaitForRundownProtectionRelease cannot have
// completed, so Dpc cannot have been freed. An earlier design revision
// introduced a distinct producer reference and could not say when it was
// released; ordering step 6 after step 5 makes it unnecessary.
//
// The ring lock is taken ONCE PER TARGET, not once per packet (section 7.1),
// and it is an in-stack queued lock so that many contending source CPUs are
// served FIFO without bouncing one cache line.
//
static
_IRQL_requires_max_(DISPATCH_LEVEL)
UINT32
XdpCpuMapFlushBatch(
    _Inout_ XDP_CPUMAP_COMMIT_GROUP *Group
    )
{
    UINT32 TotalEnqueued = 0;

    //
    // Pre-pass: on a low-resource group, replace each entry's borrowed ORIGINAL
    // with an independent deep copy (section 8.1a row 9b).
    //
    // It runs here, before any ring lock is taken, because building a copy
    // allocates a descriptor, retreats for pages and memcpys the payload, none
    // of which belongs inside a spinlock the drain CPU also contends for. POC A
    // splits its flush the same way.
    //
    // This is the last moment at which Entry->Nbl may be the caller's original.
    // The original is NOT owned by CPUMAP on this path: commit leaves it with
    // the caller. It is unambiguously alive here because commit calls this
    // function inline, before returning, so the caller has not yet applied its
    // RX action and cannot have released the EC lock. After this loop every
    // surviving entry holds a copy CPUMAP owns outright.
    //
    if (Group->DeepCopy) {
        for (UINT32 Index = 0; Index < Group->Count; Index++) {
            XDP_CPUMAP_BATCH_ENTRY *BatchEntry = &Group->Entries[Index];
            XDP_CPUMAP_HELPER_STATS *Stats;
            NET_BUFFER_LIST *Copy = NULL;
            NET_BUFFER_LIST *Original;

            ASSERT(BatchEntry->Target != NULL);
            ASSERT(BatchEntry->Nbl != NULL);

            Original = BatchEntry->Nbl;

            Stats = XdpCpuMapGetCurrentHelperStats(BatchEntry->CpuMap);

            if (Group->DeepCopyPool != NULL) {
                Copy = XdpCpuMapDeepCopyAllocate(Group->DeepCopyPool, BatchEntry->Nbl, Stats);
            }

            if (Copy != NULL) {
                BatchEntry->Nbl = Copy;
                Stats->DeepCopyBuildCount++;

                //
                // Section 12 observability. The deep-copy path is exceptional --
                // one NB per low-resource indication -- so per-copy verbose
                // tracing adds no volume on the normal path, and it is the only
                // way to correlate the original with the copy from outside the
                // driver. Original and copy are both logged because "O != C" is
                // the property the design turns on: the original goes home to
                // the miniport while the copy travels to another CPU.
                //
                TraceVerbose(
                    TRACE_CORE,
                    "CpuMapId=%u DeepCopy build Original=%p Copy=%p Pool=%p TargetCpu=%u",
                    BatchEntry->CpuMap->CpuMapId, Original, Copy, Group->DeepCopyPool,
                    BatchEntry->Target->AbsoluteCpu);
                continue;
            }

            //
            // Row 9b, "Released -- post-commit failure (i)". Three obligations,
            // and all three are discharged here rather than left to a later
            // stage that has no entry to act on:
            //
            //   - The ORIGINAL needs nothing: it was never taken, and the
            //     caller's DROP action returns it to the miniport exactly once.
            //     Simply dropping the borrowed pointer is the whole action.
            //   - The entry's backing reference and NblRundown credit are
            //     released here.
            //   - The entry's target rundown reference is released here too,
            //     because clearing Target below removes the entry from the
            //     per-target accounting that step 6 would otherwise use.
            //
            // Releasing per failed entry rather than in one batch is acceptable
            // precisely because Group->DeepCopy implies a single-entry group:
            // the receive path admits one NB per low-resource call.
            //
            Stats->DeepCopyFailCount++;

            ExReleaseRundownProtection(&BatchEntry->Target->PacketRundown);
            XdpCpuMapReleaseBacking(BatchEntry->CpuMap);
            XdpCpuMapCommitGroupReturnCredit(Group);

            BatchEntry->Nbl = NULL;
            BatchEntry->CpuMap = NULL;
            BatchEntry->Target = NULL;
        }
    }

    for (UINT32 First = 0; First < Group->Count; First++) {
        XDP_CPUMAP_TARGET *Target = Group->Entries[First].Target;
        XDP_CPUMAP *CpuMap;
        XDP_CPUMAP_HELPER_STATS *Stats;
        XDP_CPUMAP_RING *Ring;
        KLOCK_QUEUE_HANDLE LockHandle;
        UINT32 Held = 0;
        UINT32 Enqueued = 0;
        UINT32 RingFull = 0;
        UINT32 Inactive = 0;
        BOOLEAN Active;
        NET_BUFFER_LIST *DeepCopyRejects = NULL;

        //
        // A NULL target marks an entry that an earlier target group in this
        // same flush has already consumed, or one the deep-copy pre-pass above
        // released.
        //
        if (Target == NULL) {
            continue;
        }

        CpuMap = Group->Entries[First].CpuMap;
        Ring = Target->Ring;
        Stats = XdpCpuMapGetCurrentHelperStats(CpuMap);

        //
        // Step 1.
        //
        KeAcquireInStackQueuedSpinLock(&Ring->Lock, &LockHandle);

        //
        // Step 2. The re-check is against the TARGET, not the selector key. A
        // key-scoped check is insufficient: an in-place replacement can repoint
        // a key at a new target while the old one retires, so a still-live key
        // says nothing about the ring this packet resolved to. The sweep
        // publishes Target->Active = FALSE under ConfigLock and only then waits
        // on the rundown, so a producer that acquired before the publish is
        // forced through the reject path here.
        //
        Active = CpuMap->Active && Target->Active;

        //
        // Step 3. Write the ring slots, transferring the NBL, the CPUMAP
        // backing reference and the receive queue's NblRundown reference into
        // each. The target rundown reference is deliberately NOT transferred
        // (section 8.1a).
        //
        for (UINT32 Index = First; Index < Group->Count; Index++) {
            XDP_CPUMAP_BATCH_ENTRY *BatchEntry = &Group->Entries[Index];
            XDP_CPUMAP_ENTRY *Slot;

            if (BatchEntry->Target != Target) {
                continue;
            }

            ASSERT(BatchEntry->CpuMap == CpuMap);
            ASSERT(BatchEntry->Nbl != NULL);
            Held++;
            BatchEntry->Target = NULL;

            if (!Active || Ring->Tail - Ring->Head >= Ring->Capacity) {
                //
                // Ownership was committed and ActionNbl cleared, so there is no
                // RX action left to fall back to: the packet is counted as a
                // drop. Only pointer stores happen under the lock; the reference
                // releases are batched below it.
                //
                // Row 9b, "Released -- post-commit failure (ii)": a deep copy is
                // NOT chained here. RejectedNbls is appended to the caller's
                // DropList, which returns its NBLs to the MINIPORT, and the
                // miniport never owned a copy -- returning one would hand it a
                // buffer from our pool. The copy is instead collected for
                // recycling once the lock is dropped. The original on this path
                // is unaffected: it went to DropList at commit and is already
                // being returned by the normal receive path.
                //
                if (Group->DeepCopy) {
                    NET_BUFFER_LIST_NEXT_NBL(BatchEntry->Nbl) = DeepCopyRejects;
                    DeepCopyRejects = BatchEntry->Nbl;
                } else {
                    NET_BUFFER_LIST_NEXT_NBL(BatchEntry->Nbl) = Group->RejectedNbls;
                    Group->RejectedNbls = BatchEntry->Nbl;
                }
                BatchEntry->Nbl = NULL;

                if (!Active) {
                    Inactive++;
                } else {
                    RingFull++;
                }

                continue;
            }

            Slot = &Ring->Entries[Ring->Tail & Ring->Mask];
            Slot->Nbl = BatchEntry->Nbl;
            Slot->FilterHandle = Group->FilterHandle;
            Slot->RxQueueOwner = Group->RxQueueOwner;
            Slot->GenericOwner = Group->GenericOwner;
            Slot->BackingRef = BatchEntry->CpuMap;
            Slot->NblRundown = Group->NblRundown;
            Slot->PortNumber = Group->PortNumber;
            Slot->DeepCopyPool = Group->DeepCopyPool;
            Slot->IsDeepCopy = Group->DeepCopy;

            BatchEntry->Nbl = NULL;
            BatchEntry->CpuMap = NULL;

            Ring->Tail++;
            Enqueued++;
        }

        if (Ring->Tail - Ring->Head > Ring->MaxDepth) {
            Ring->MaxDepth = Ring->Tail - Ring->Head;
        }

        //
        // Step 4.
        //
        KeReleaseInStackQueuedSpinLock(&LockHandle);

        //
        // Deep copies the ring refused, recycled now that the lock is gone.
        //
        // Placed between steps 4 and 5 for the same reason the counter writes
        // below are: the group still holds every target rundown reference here,
        // so the receive queue cannot have reached deletion and DeepCopyPool is
        // still alive. Recycling costs no ring lock and no NDIS call -- the
        // pages go back and the descriptor lands on the free list.
        //
        while (DeepCopyRejects != NULL) {
            NET_BUFFER_LIST *Next = NET_BUFFER_LIST_NEXT_NBL(DeepCopyRejects);

            ASSERT(Group->DeepCopyPool != NULL);

            //
            // Section 12 observability. A ring-rejected copy is the case most
            // easily confused with a leak from outside the driver: it was built,
            // never indicated, and never returned to the miniport. Tracing it
            // with an explicit disposition is what distinguishes "recycled
            // correctly" from "lost".
            //
            TraceVerbose(
                TRACE_CORE, "DeepCopy recycle Head=%p Count=1 Pool=%p Disposition=RingRejected",
                DeepCopyRejects, Group->DeepCopyPool);

            XdpCpuMapDeepCopyRecycle(Group->DeepCopyPool, DeepCopyRejects);
            DeepCopyRejects = Next;
        }

        //
        // Counters are written HERE, before step 6, while the group still holds
        // every target rundown reference. That is what keeps Stats -- which
        // points into CpuMap->HelperStats -- alive: a held target rundown
        // reference blocks XdpCpuMapRetireTarget's wait, which blocks the sweep
        // from completing, which keeps the sweep's backing reference held, which
        // keeps RefCount above zero and HelperStats unfreed.
        //
        // Writing them after step 6 is NOT safe, even before the backing release
        // below. Step 6 unblocks the retire wait; the target can then retire,
        // the sweep can complete and drop its reference, and if this group's
        // entries were all enqueued -- so it holds no backing reference of its
        // own -- the drained ring slots can release the last ones. An earlier
        // revision of this fix placed the writes between step 6 and the backing
        // release and left exactly that window open.
        //
        Stats->EnqueueCount += Enqueued;
        Stats->EnqueueTargetInactive += Inactive;
        Stats->RingFullCount += RingFull;
        TotalEnqueued += Enqueued;

        //
        // Step 5. Safe without a producer reference because step 6 has not run.
        //
        if (Enqueued > 0) {
            KeInsertQueueDpc(Target->Dpc, NULL, NULL);
        }

        //
        // Step 6. One interlocked operation for the whole target group rather
        // than one per packet (section 11). Every entry, enqueued or rejected,
        // released here and only here; section 6.3 step 6 deliberately does not
        // release it, because that would be a double release.
        //
        ASSERT(Held > 0);
        ExReleaseRundownProtectionEx(&Target->PacketRundown, Held);

        //
        // The rejected entries' other two references, also batched. A rejected
        // entry's NblRundown credit returns to the group's pool as plain
        // group-local state.
        //
        // N.B. this is the LAST access to CpuMap in this iteration.
        //
        if (Inactive + RingFull > 0) {
            Group->Credits += Inactive + RingFull;
            XdpCpuMapReleaseBackingRefs(CpuMap, (LONG)(Inactive + RingFull));
        }
    }

    Group->Count = 0;
    return TotalEnqueued;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
XDP_CPUMAP_COMMIT_RESULT
XdpCpuMapCommitRedirect(
    _Inout_ XDP_FRAME_CPUMAP_REDIRECT_V1 *Redirect,
    _In_opt_ NET_BUFFER_LIST *ActionNbl,
    _In_ BOOLEAN RxQueuePaused,
    _In_ BOOLEAN CanPend,
    _Inout_ XDP_CPUMAP_COMMIT_GROUP *Group
    )
{
    XDP_CPUMAP *CpuMap = Redirect->CpuMap;
    XDP_CPUMAP_TARGET *Target = Redirect->Target;
    XDP_CPUMAP_BATCH_ENTRY *BatchEntry;
    BOOLEAN RedirectReferencesHeld;
    BOOLEAN NblRundownCreditHeld = FALSE;
    UINT32 Enqueued;

    //
    // Two independent questions, deliberately not conflated.
    //
    // First: does this metadata own helper references? That is decided purely by
    // the metadata, never by the caller's NBL state. An earlier revision folded
    // the ActionNbl check into this test, so a frame with valid metadata but a
    // NULL ActionNbl was zeroed WITHOUT releasing the target rundown and backing
    // reference it genuinely held. A leaked rundown reference is exactly what
    // deadlocks ExWaitForRundownProtectionRelease at queue teardown.
    //
    RedirectReferencesHeld =
        Redirect->Size == sizeof(*Redirect) &&
        Redirect->Version == XDP_FRAME_CPUMAP_REDIRECT_VERSION_1 &&
        (Redirect->Flags & XDP_FRAME_CPUMAP_REDIRECT_FLAG_OWNERSHIP_PENDING) != 0 &&
        (Redirect->Flags & XDP_FRAME_CPUMAP_REDIRECT_FLAG_OWNERSHIP_COMMITTED) == 0 &&
        CpuMap != NULL &&
        Target != NULL;

    //
    // Section 6.3 step 1: every one of these is guaranteed by the helper, so
    // each is a broken invariant rather than a runtime condition, and each
    // asserts independently in checked builds. Retail still falls through to
    // Reject and releases whatever the metadata legitimately owns.
    //
    // Asserting the conjunction alone is not sufficient: it cannot distinguish
    // "metadata never came from the helper" from "helper metadata arrived with a
    // NULL ActionNbl", which are different defects with different causes.
    //
    ASSERT(Redirect->Size == sizeof(*Redirect));
    ASSERT(Redirect->Version == XDP_FRAME_CPUMAP_REDIRECT_VERSION_1);
    ASSERT((Redirect->Flags & XDP_FRAME_CPUMAP_REDIRECT_FLAG_OWNERSHIP_PENDING) != 0);
    ASSERT((Redirect->Flags & XDP_FRAME_CPUMAP_REDIRECT_FLAG_OWNERSHIP_COMMITTED) == 0);
    ASSERT(CpuMap != NULL);
    ASSERT(Target != NULL);
    ASSERT(ActionNbl != NULL);

    //
    // CPUMAP is receive-side only and the exclusion is structural: a TX-inspect
    // queue never has CpuMapRedirectEnabled set, so it never registers the frame
    // extension and no send NBL can reach here. This is therefore a broken
    // invariant, not a runtime state, and it gets an assertion rather than a
    // counter -- the same treatment XdpReceive gives CpuMapRedirectEnabled.
    // Retail falls through to Reject and fails closed.
    //
    ASSERT(!Group->TxInspect);

    //
    // CanPend and Group->DeepCopy are two derivations of one value: both come
    // from the CanPend argument of the post-inspection call this group belongs
    // to. The code below uses the group's copy, because the flush needs it long
    // after this function has returned, so the parameter is checked rather than
    // read. They can only disagree through a plumbing defect -- a group
    // initialized outside the loop that calls this, or a group reused across
    // calls with different CanPend -- and the consequence would be silent: a
    // low-resource original lent across a DPC and indicated from another CPU, or
    // a pendable original needlessly copied and dropped. The assertion converts
    // that into a checked-build failure at the first packet.
    //
    // ASSERT compiles out in Release, and this is CanPend's ONLY remaining use
    // since the deep-copy refactor moved the low-resource decision to
    // Group->DeepCopy -- so without the explicit reference below, /WX fails the
    // Release build on C4100 while Debug builds clean. The parameter is kept
    // rather than removed because the assertion it feeds is load-bearing.
    //
    ASSERT(Group->DeepCopy == !CanPend);
    UNREFERENCED_PARAMETER(CanPend);

    if (!RedirectReferencesHeld || ActionNbl == NULL || Group->TxInspect) {
        goto Reject;
    }

    if (RxQueuePaused) {
        XdpCpuMapRecordCommitReject(CpuMap, XdpCpuMapCommitRejectPause);
        goto Reject;
    }

    //
    // Section 6.3 requires rundown acquisition to be batched once per flush
    // group, not taken per packet. The group hands out pre-acquired credits, so
    // the common case costs no interlocked operation at all; only an exhausted
    // group touches the shared rundown.
    //
    if (!XdpCpuMapCommitGroupTakeCredit(Group)) {
        XdpCpuMapRecordCommitReject(CpuMap, XdpCpuMapCommitRejectRundown);
        goto Reject;
    }
    NblRundownCreditHeld = TRUE;

    //
    // Section 6.3 step 4. Flushing a full batch first is what makes the insert
    // infallible: there is no batch-add failure to unwind, and nothing before
    // this point has mutated the batch, so steps 2 and 3 have nothing to undo
    // either.
    //
    if (Group->Count == RTL_NUMBER_OF(Group->Entries)) {
        (VOID)XdpCpuMapFlushBatch(Group);
    }
    ASSERT(Group->Count < RTL_NUMBER_OF(Group->Entries));

    BatchEntry = &Group->Entries[Group->Count++];
    BatchEntry->Nbl = ActionNbl;
    BatchEntry->CpuMap = CpuMap;
    BatchEntry->Target = Target;

    //
    // The batch now owns all three references. In particular the NblRundown
    // credit is KEPT rather than returned: it is exactly the reference the ring
    // slot goes on to own.
    //
    // The metadata is then cleared, exactly as the reject path clears it. The
    // design says to set OWNERSHIP_COMMITTED; leaving OWNERSHIP_PENDING set
    // alongside it would leave a frame-ring slot advertising a pending redirect
    // AND holding CpuMap/Target pointers this frame no longer has references on.
    // Frame ring slots are reused constantly, and stale frame metadata is the
    // mechanism that hung the test machine twice in increment 5, so the
    // post-transfer state must own nothing and claim nothing. Clearing makes the
    // caller's OWNERSHIP_PENDING gate refuse a second visit outright rather than
    // re-entering this function to assert.
    //
    // No counter fires here. Stats.Success is the HELPER's counter -- it records
    // that bpf_redirect_map set an intent -- and the enqueue outcome is counted
    // by the flush, which is the first point at which it is known.
    //
    RtlZeroMemory(Redirect, sizeof(*Redirect));

    //
    // Section 8.1a row 9b: on the low-resource path the ORIGINAL is not taken.
    //
    // Returning FALSE is not a rejection here, and this is deliberately not the
    // Reject label: the batch entry above is live and owns all three references,
    // exactly as on the CanPend path. What FALSE means to the caller is only
    // "you still own this NBL", and that is precisely right -- the caller's
    // XDP_RX_ACTION_DROP appends the original to DropList and the normal receive
    // path returns it to the miniport once. That is the whole point of the
    // low-resource path: the original goes home immediately and a copy travels
    // to the target CPU instead.
    //
    // Entry->Nbl is therefore a BORROWED copy source until the flush replaces it
    // with a real copy. It stays valid because the group is flushed by
    // XdpCpuMapCommitGroupFinish at the end of the same post-inspection call
    // that put the original on DropList, and nothing between those two points
    // returns it: XdpGenericReceiveLowResources only relinks it onto the
    // reassembled low-resources chain the miniport gets back when the whole
    // receive call unwinds.
    //
    if (Group->DeepCopy) {
        //
        // Flush NOW, before returning to the caller, and not at the end of the
        // post-inspection call.
        //
        // Section 8.4's quiesce guarantee is that a pause does not depend on any
        // target DPC running: pause publishes its gate, then quiesce scans every
        // ring and tombstones what it finds. A batch entry is invisible to that
        // scan -- it lives on the receive path's stack, not in a ring. On the
        // low-resource path the caller goes on to call
        // XdpGenericReceiveLowResources, which RELEASES AND REACQUIRES the EC
        // spinlock in order to indicate its pass list. A pause running in that
        // window would publish, scan, find nothing, and complete -- and then the
        // deferred flush would enqueue behind the scan, putting a packet in a
        // ring that quiesce has already passed and making its delivery depend on
        // the target DPC after all.
        //
        // Flushing here closes the window rather than detecting it: by the time
        // the lock can be dropped, the entry is either in a ring where quiesce
        // can see it or already disposed of. The group can hold only this one
        // entry -- XdpGenericReceivePreInspectNbs admits a single NB per
        // low-resource call -- so this costs nothing a deferred flush would have
        // saved.
        //
        // It also removes the borrowed-pointer window entirely: the copy is
        // built while the caller is still holding the original, before the RX
        // action has even been applied.
        //
        Enqueued = XdpCpuMapFlushBatch(Group);
        ASSERT(Group->RejectedNbls == NULL);

        //
        // The flush's enqueue count is what makes this result honest.
        //
        // Returning DeepCopied unconditionally would report success for a copy
        // that was never built -- pool exhausted, retreat failed, metadata
        // refused -- and for one the ring rejected, and the caller would then
        // suppress the drop record for a packet that really was lost. That is
        // the diagnostic regression the tri-state exists to prevent, so the
        // tri-state has to be driven by the outcome rather than by reaching this
        // line.
        //
        // Declined is exactly right on failure: the caller still owns the
        // original, applies its DROP action, and logs the drop it should log.
        //
        if (Enqueued == 0) {
            return XdpCpuMapCommitDeclined;
        }

        return XdpCpuMapCommitDeepCopied;
    }

    return XdpCpuMapCommitOwnershipTaken;

Reject:

    //
    // Returning the credit is a plain decrement against group-local state. The
    // group releases whatever remains unconsumed in a single interlocked
    // operation when the flush group ends.
    //
    if (NblRundownCreditHeld) {
        XdpCpuMapCommitGroupReturnCredit(Group);
    }

    if (RedirectReferencesHeld) {
        XdpCpuMapReleaseTargetReference(Target);
        XdpCpuMapReleaseBacking(CpuMap);
    }

    RtlZeroMemory(Redirect, sizeof(*Redirect));
    return XdpCpuMapCommitDeclined;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapCommitGroupFinish(
    _Inout_ XDP_CPUMAP_COMMIT_GROUP *Group,
    _Outptr_result_maybenull_ NET_BUFFER_LIST **RejectedNbls
    )
{
    (VOID)XdpCpuMapFlushBatch(Group);

    if (Group->Credits > 0) {
        ExReleaseRundownProtectionEx(Group->NblRundown, Group->Credits);
        Group->Credits = 0;
    }

    *RejectedNbls = Group->RejectedNbls;
    Group->RejectedNbls = NULL;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapQueryGlobalStats(
    _Out_opt_ UINT32 *RingEntries,
    _Out_opt_ SIZE_T *NonPagedBytes
    )
{
    RtlAcquirePushLockShared(&XdpCpuMapGlobalLock);
    if (RingEntries != NULL) {
        *RingEntries = XdpCpuMapGlobalRingEntries;
    }
    if (NonPagedBytes != NULL) {
        *NonPagedBytes = XdpCpuMapGlobalNonPagedBytes;
    }
    RtlReleasePushLockShared(&XdpCpuMapGlobalLock);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
BOOLEAN
XdpCpuMapTryAcquireTargetReference(
    _Inout_ XDP_CPUMAP *CpuMap,
    _Inout_ XDP_CPUMAP_TARGET *Target
    )
{
    //
    // This function is called only from the eBPF helper path while the eBPF
    // runtime still holds the program epoch. Target is reached through a base
    // map value, so the target shell is epoch-safe here. Ring and DPC are not:
    // the caller may read them only after this acquire succeeds.
    //
    if (!CpuMap->Active) {
        return FALSE;
    }

    return ExAcquireRundownProtection(&Target->PacketRundown);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapReleaseTargetReference(
    _Inout_ XDP_CPUMAP_TARGET *Target
    )
{
    ExReleaseRundownProtection(&Target->PacketRundown);
}

//
// Indication partitioning.
//
// A single ring holds packets from every source CPU that redirects to this
// target, so one drain batch can legitimately carry packets captured on
// different NDIS ports and -- since v1 does not stop a map being used by two
// interfaces -- on different filters. Design section 7 requires the drain to
// partition by (FilterHandle, PortNumber, indication flags) and to indicate each
// partition with its OWN captured handle and port. Both POCs instead capture the
// handle and port of whichever entry they visited last and indicate the whole
// chain with those, which is latent only because their maps were bound to one
// filter and their port was almost always zero.
//
// The key here additionally includes the backing map and the receive queue's
// rundown. That can only ever SPLIT a chain further, never merge two that the
// design would keep apart, and it is what lets the post-indication reference
// releases be one interlocked operation per chain instead of one per packet
// (section 11). In practice both are functions of the filter handle and the
// receive queue, so no real chain is fragmented by their presence.
//
#define XDP_CPUMAP_INDICATION_CHAINS 8u

typedef struct _XDP_CPUMAP_NBL_CHAIN {
    NDIS_HANDLE FilterHandle;
    NDIS_PORT_NUMBER PortNumber;
    BOOLEAN IsDeepCopy;
    XDP_CPUMAP *BackingRef;
    EX_RUNDOWN_REF *NblRundown;

    //
    // Constant across a partition, because the partition key includes
    // NblRundown and both it and the pool are per receive queue. Carried so the
    // disposal paths can recycle without revisiting slots that are already gone.
    //
    XDP_CPUMAP_DEEPCOPY_POOL *DeepCopyPool;

    NET_BUFFER_LIST *Head;
    NET_BUFFER_LIST *Tail;
    ULONG Count;
} XDP_CPUMAP_NBL_CHAIN;

typedef struct _XDP_CPUMAP_NBL_CHAIN_SET {
    UINT32 Used;
    XDP_CPUMAP_NBL_CHAIN Chains[XDP_CPUMAP_INDICATION_CHAINS];
} XDP_CPUMAP_NBL_CHAIN_SET;

static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapChainSetInit(
    _Out_ XDP_CPUMAP_NBL_CHAIN_SET *Set
    )
{
    Set->Used = 0;
}

//
// Move one occupied slot into its chain. The NBL and every reference the slot
// owned leave in the SAME critical section that clears the slot, so what is left
// behind is a slot that owns NOTHING. Ownership transfers exactly once.
//
// Returns FALSE when the batch needs more distinct chains than the set holds, in
// which case the caller must stop, flush, and revisit this slot. Merging is not
// an option: that is precisely the POC defect above.
//
static
_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
BOOLEAN
XdpCpuMapChainSetTake(
    _Inout_ XDP_CPUMAP_NBL_CHAIN_SET *Set,
    _Inout_ XDP_CPUMAP_ENTRY *Entry
    )
{
    XDP_CPUMAP_NBL_CHAIN *Chain = NULL;

    ASSERT(Entry->Nbl != NULL);
    ASSERT(Entry->BackingRef != NULL);
    ASSERT(Entry->NblRundown != NULL);

    for (UINT32 Index = 0; Index < Set->Used; Index++) {
        XDP_CPUMAP_NBL_CHAIN *Candidate = &Set->Chains[Index];

        if (Candidate->FilterHandle == Entry->FilterHandle &&
            Candidate->PortNumber == Entry->PortNumber &&
            Candidate->IsDeepCopy == Entry->IsDeepCopy &&
            Candidate->BackingRef == Entry->BackingRef &&
            Candidate->NblRundown == Entry->NblRundown) {
            Chain = Candidate;
            break;
        }
    }

    if (Chain == NULL) {
        if (Set->Used == RTL_NUMBER_OF(Set->Chains)) {
            return FALSE;
        }

        Chain = &Set->Chains[Set->Used++];
        Chain->FilterHandle = Entry->FilterHandle;
        Chain->PortNumber = Entry->PortNumber;
        Chain->IsDeepCopy = Entry->IsDeepCopy;
        Chain->BackingRef = Entry->BackingRef;
        Chain->NblRundown = Entry->NblRundown;
        Chain->DeepCopyPool = Entry->DeepCopyPool;
        Chain->Head = NULL;
        Chain->Tail = NULL;
        Chain->Count = 0;
    }

    //
    // A deep-copy slot must name the pool its copy came from; there is nowhere
    // else to recycle it to. The partition key makes this constant per chain,
    // so checking it on every take is what keeps that assumption honest.
    //
    ASSERT(!Entry->IsDeepCopy || Entry->DeepCopyPool != NULL);
    ASSERT(Chain->DeepCopyPool == Entry->DeepCopyPool);

    NET_BUFFER_LIST_NEXT_NBL(Entry->Nbl) = NULL;
    if (Chain->Tail != NULL) {
        NET_BUFFER_LIST_NEXT_NBL(Chain->Tail) = Entry->Nbl;
    } else {
        Chain->Head = Entry->Nbl;
    }
    Chain->Tail = Entry->Nbl;
    Chain->Count++;

    RtlZeroMemory(Entry, sizeof(*Entry));
    return TRUE;
}

//
// Release what the chain's slots owned, AFTER the NBLs have been disposed of.
// Both releases are batched: one interlocked operation each, for the whole
// chain.
//
// N.B. NblRundown is released last-touch: once it is dropped the receive queue's
// pause may complete and the queue may be freed, so nothing may read the pointer
// afterwards.
//
static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapChainRelease(
    _Inout_ XDP_CPUMAP_NBL_CHAIN *Chain
    )
{
    ASSERT(Chain->Count > 0);

    XdpCpuMapReleaseBackingRefs(Chain->BackingRef, (LONG)Chain->Count);
    ExReleaseRundownProtectionEx(Chain->NblRundown, Chain->Count);

    Chain->BackingRef = NULL;
    Chain->NblRundown = NULL;
    Chain->DeepCopyPool = NULL;
    Chain->Head = NULL;
    Chain->Tail = NULL;
    Chain->Count = 0;
}

//
// Recycle a whole partition of deep copies back into the pool they came from.
//
// Must run BEFORE XdpCpuMapChainRelease: the pool belongs to the receive queue,
// and the chain's NblRundown references are the only thing keeping that queue
// from completing its pause and being deleted. Releasing first would leave this
// loop dereferencing a freed pool.
//
// The whole partition goes back in ONE interlocked push. The chain is already
// linked through Next, which is the same field the free list uses as its
// SLIST_ENTRY, so advancing each copy's data start in place leaves a list the
// pool can take wholesale -- the shape upstream uses for returned TX clones, and
// what section 11 asks for instead of an interlocked operation per packet.
//
static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapChainRecycleDeepCopies(
    _Inout_ XDP_CPUMAP_NBL_CHAIN *Chain,
    _In_ const CHAR *Disposition
    )
{
    NET_BUFFER_LIST *First = Chain->Head;
    NET_BUFFER_LIST *Nbl = First;

    ASSERT(Chain->IsDeepCopy);
    ASSERT(Chain->DeepCopyPool != NULL);
    ASSERT(First != NULL);
    ASSERT(Chain->Tail != NULL);

    //
    // Section 12 observability. Disposition is a parameter rather than inferred
    // because the three ways a copy leaves the data path -- delivered and
    // returned by the stack, refused by the ring, or discarded at teardown --
    // are indistinguishable from here, and they are exactly what a reader needs
    // to tell apart. Every one of them ends in the SAME pool the copy came from,
    // which is the property this trace exists to make checkable.
    //
    TraceVerbose(
        TRACE_CORE, "DeepCopy recycle Head=%p Count=%u Pool=%p Disposition=%s",
        First, Chain->Count, Chain->DeepCopyPool, Disposition);
    UNREFERENCED_PARAMETER(Disposition);

    while (Nbl != NULL) {
        NET_BUFFER *Nb = NET_BUFFER_LIST_FIRST_NB(Nbl);

        //
        // FreeMdl is TRUE: the MDL and pages came from this copy's own retreat,
        // so advancing the full data length gives them back and leaves a bare
        // descriptor. Advancing by DataLength rather than a remembered retreat
        // size is correct because nothing on the redirect path trims a copy.
        //
        NdisAdvanceNetBufferDataStart(Nb, NET_BUFFER_DATA_LENGTH(Nb), TRUE, NULL);

        //
        // The free list stores its SLIST_ENTRY in the NBL's Next field, which
        // requires MEMORY_ALLOCATION_ALIGNMENT. Next is at offset 0 of a
        // pool-allocated NBL, so the descriptor's own alignment is the
        // guarantee -- checked per entry rather than assumed once.
        //
        ASSERT(((ULONG_PTR)Nbl & (MEMORY_ALLOCATION_ALIGNMENT - 1)) == 0);
        Nbl = NET_BUFFER_LIST_NEXT_NBL(Nbl);
    }

    InterlockedPushListSList(
        &Chain->DeepCopyPool->FreeList, (SLIST_ENTRY *)&First->Next,
        (SLIST_ENTRY *)&Chain->Tail->Next, Chain->Count);

    Chain->Head = NULL;
    Chain->Tail = NULL;
}

//
// Data-path disposal: indicate each partition up the stack with its own captured
// handle, port and flags. Originals go without RESOURCES; deep copies go with
// it and return synchronously.
//
static
_IRQL_requires_(DISPATCH_LEVEL)
VOID
XdpCpuMapChainSetIndicate(
    _Inout_ XDP_CPUMAP_NBL_CHAIN_SET *Set
    )
{
    for (UINT32 Index = 0; Index < Set->Used; Index++) {
        XDP_CPUMAP_NBL_CHAIN *Chain = &Set->Chains[Index];
        ULONG ReceiveFlags = NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL;

        ASSERT(Chain->Count > 0);

        //
        // RESOURCES is derived from the partition key rather than hard-coded,
        // and on a deep-copy partition it is what makes the lifetime argument
        // work: the stack must consume the copy inline, so the indication
        // returns with the chain still ours and the recycle below happens before
        // this DPC does.
        //
        if (Chain->IsDeepCopy) {
            ReceiveFlags |= NDIS_RECEIVE_FLAGS_RESOURCES;
        }

        NdisFIndicateReceiveNetBufferLists(
            Chain->FilterHandle, Chain->Head, Chain->PortNumber, Chain->Count,
            ReceiveFlags);

        //
        // Section 12 observability. ReceiveFlags is logged as ACTUALLY PASSED,
        // not as intended: the RESOURCES bit is what forces the stack to consume
        // the copy inline, and the whole recycle lifetime argument rests on it,
        // so it must be observable rather than inferred from IsDeepCopy. The
        // executing CPU is logged because the point of the feature is that this
        // is the map-selected CPU and not the arrival CPU.
        //
        if (Chain->IsDeepCopy) {
            TraceVerbose(
                TRACE_CORE,
                "DeepCopy indicate Head=%p Count=%u ReceiveFlags=%x Cpu=%u",
                Chain->Head, Chain->Count, ReceiveFlags, KeGetCurrentProcessorIndex());
        }

        if (Chain->IsDeepCopy) {
            XdpCpuMapChainRecycleDeepCopies(Chain, "Indicated");
        }

        XdpCpuMapChainRelease(Chain);
    }

    Set->Used = 0;
}

//
// Teardown disposal: dispose of each partition WITHOUT indicating it. Retire and
// quiesce both reach here, and in both cases the packet is lost by design
// (sections 8.3 and 8.4).
//
static
_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapChainSetReturn(
    _Inout_ XDP_CPUMAP_NBL_CHAIN_SET *Set
    )
{
    for (UINT32 Index = 0; Index < Set->Used; Index++) {
        XDP_CPUMAP_NBL_CHAIN *Chain = &Set->Chains[Index];

        ASSERT(Chain->Count > 0);

        //
        // Section 8.1a row 9b, "Released -- teardown": a deep copy goes back to
        // its pool, never to the miniport, which never owned it. Returning one
        // would hand the miniport a buffer allocated from our own pool.
        //
        if (Chain->IsDeepCopy) {
            XdpCpuMapChainRecycleDeepCopies(Chain, "Teardown");
        } else {
            NdisFReturnNetBufferLists(Chain->FilterHandle, Chain->Head, 0);
        }

        XdpCpuMapChainRelease(Chain);
    }

    Set->Used = 0;
}

_Function_class_(KDEFERRED_ROUTINE)
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
static
VOID
XdpCpuMapDrainDpc(
    _In_ KDPC *Dpc,
    _In_opt_ VOID *DeferredContext,
    _In_opt_ VOID *SystemArgument1,
    _In_opt_ VOID *SystemArgument2
    )
{
    XDP_CPUMAP_TARGET *Target = (XDP_CPUMAP_TARGET *)DeferredContext;
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_HELPER_STATS *Stats;
    XDP_CPUMAP_RING *Ring;
    UINT32 Batch;
    ULONG64 Drained = 0;
    ULONG64 Tombstones = 0;
    BOOLEAN MoreWork;
    BOOLEAN Yield = FALSE;

    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    //
    // Target, its ring and its DPC are all still live here: the retire path
    // cancels and flushes this DPC only after ExWaitForRundownProtectionRelease
    // has completed, and every producer holds a rundown reference across its
    // KeInsertQueueDpc (section 7 steps 5 and 6).
    //
    ASSERT(Target != NULL);
    if (Target == NULL) {
        return;
    }

    CpuMap = Target->OwnerMap;
    Ring = Target->Ring;

    ASSERT(Ring != NULL);
    if (Ring == NULL) {
        return;
    }

    Stats = XdpCpuMapGetCurrentHelperStats(CpuMap);
    Stats->DpcInvokeCount++;

    //
    // The sweep zeroes EffectiveDrainBatchSize in the same critical section that
    // takes TargetCount to zero, so this unlocked read can legitimately observe
    // zero, and a zero batch would make the loop below spin without draining.
    //
    Batch = CpuMap->EffectiveDrainBatchSize;
    if (Batch == 0 || Batch > XDP_CPUMAP_DRAIN_BATCH_MAX) {
        Batch = XDP_CPUMAP_DRAIN_BATCH_DEFAULT;
    }

    do {
        XDP_CPUMAP_NBL_CHAIN_SET ChainSet;
        KLOCK_QUEUE_HANDLE LockHandle;
        UINT32 Scanned = 0;
        ULONG64 IterationDrained = 0;
        ULONG64 IterationTombstones = 0;

        XdpCpuMapChainSetInit(&ChainSet);

        KeAcquireInStackQueuedSpinLock(&Ring->Lock, &LockHandle);

        while (Ring->Head != Ring->Tail && Scanned < Batch) {
            XDP_CPUMAP_ENTRY *Entry = &Ring->Entries[Ring->Head & Ring->Mask];

            //
            // A tombstone owns nothing: quiesce took its NBL and all of its
            // references out under this same lock in one step. Advance past it
            // and release NOTHING (sections 7.1 and 8.4).
            //
            if (Entry->Nbl == NULL) {
                Ring->Head++;
                Scanned++;
                IterationTombstones++;
                continue;
            }

            if (!XdpCpuMapChainSetTake(&ChainSet, Entry)) {
                break;
            }

            Ring->Head++;
            Scanned++;
            IterationDrained++;
        }

        MoreWork = (Ring->Head != Ring->Tail);

        KeReleaseInStackQueuedSpinLock(&LockHandle);

        //
        // Counters are written here rather than after the indication. The DPC's
        // access to Stats is already covered by the owner/sweep reference
        // argument at the end of this routine, so this ordering is defensive
        // rather than load-bearing -- unlike the flush, where the equivalent
        // ordering fixes a real window.
        //
        Drained += IterationDrained;
        Tombstones += IterationTombstones;
        Stats->DrainCount += IterationDrained;
        Stats->DrainTombstoneCount += IterationTombstones;
        Stats->IndicateChainCount += ChainSet.Used;

        if (Scanned == 0) {
            ASSERT(!MoreWork);
            Stats->DpcEmptyCount++;
            break;
        }

        //
        // Indication happens with no CPUMAP lock held (section 9).
        //
        XdpCpuMapChainSetIndicate(&ChainSet);

        if (MoreWork && KeShouldYieldProcessor()) {
            Yield = TRUE;
        }
    } while (MoreWork && !Yield);

    if (Yield) {
        //
        // Section 7, "DPC self-requeue is gated by the same rundown". Without
        // this gate a DPC can re-queue itself after the retire path's
        // KeRemoveQueueDpc has already cancelled it, defeating the cancel. A
        // failed acquire means the target is retiring, and the retire path
        // drains the remainder synchronously, so simply return.
        //
        // Stats points into CpuMap->HelperStats and is safe to touch anywhere in
        // this routine, for a reason that has nothing to do with ring
        // occupancy: a live map holds its own owner reference until
        // XdpCpuMapDestroy drops it, and a map being destroyed has already
        // armed a sweep that holds a backing reference until it completes --
        // and the sweep cannot complete this target without KeRemoveQueueDpc
        // and KeFlushQueuedDpcs, which cannot finish while this routine is
        // executing. So RefCount cannot reach zero, and HelperStats cannot be
        // freed, while a drain DPC is running.
        //
        // N.B. an earlier revision justified this with "Yield implies MoreWork,
        // so ring entries still hold backing references". That is FALSE:
        // MoreWork can consist solely of tombstones, which own nothing.
        //
        if (ExAcquireRundownProtection(&Target->PacketRundown)) {
            KeInsertQueueDpc(Dpc, NULL, NULL);
            ExReleaseRundownProtection(&Target->PacketRundown);
            Stats->DpcRequeueCount++;
        }
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapReferenceBacking(
    _Inout_ XDP_CPUMAP *CpuMap
    )
{
    LONG NewCount = InterlockedIncrement(&CpuMap->RefCount);

    //
    // Resurrection from zero is a bug: the owner reference is dropped only by
    // XdpCpuMapDestroy, which then waits, so no caller can legitimately observe
    // a zero count.
    //
    ASSERT(NewCount > 1);
    UNREFERENCED_PARAMETER(NewCount);
}

static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapReleaseBackingRefs(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ LONG Count
    )
{
    ASSERT(Count > 0);

    //
    // InterlockedExchangeAdd returns the PREVIOUS value, so the count reached
    // zero exactly when the previous value was the number released.
    //
    if (InterlockedExchangeAdd(&CpuMap->RefCount, -Count) == Count) {
        KeSetEvent(&CpuMap->RefCountZero, IO_NO_INCREMENT, FALSE);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapReleaseBacking(
    _Inout_ XDP_CPUMAP *CpuMap
    )
{
    if (InterlockedDecrement(&CpuMap->RefCount) == 0) {
        KeSetEvent(&CpuMap->RefCountZero, IO_NO_INCREMENT, FALSE);
    }
}

//
// Arm the map's retire sweep.
//
// The reference taken in step 1 is what keeps the map, and therefore its
// EMBEDDED SweepEntry, alive for as long as a sweep is queued or running. The
// caller is inside a runtime map operation, so the map object is alive here by
// construction and taking a reference is always safe.
//
// SweepRearm is set BEFORE the claim is attempted and the worker clears it
// BEFORE each pass and re-reads it AFTER dropping to Idle, so a release can
// never be lost. The work entry is inserted only by the thread that observed
// Idle, so it is never inserted while queued or running.
//
static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapArmSweep(
    _Inout_ XDP_CPUMAP *CpuMap
    )
{
    XdpCpuMapReferenceBacking(CpuMap);

    InterlockedExchange(&CpuMap->SweepRearm, 1);

    if (InterlockedCompareExchange(
            &CpuMap->SweepState, XdpCpuMapSweepQueued, XdpCpuMapSweepIdle) ==
                XdpCpuMapSweepIdle) {
        XdpInsertWorkQueue(XdpCpuMapRetireQueue, &CpuMap->SweepEntry);
    } else {
        //
        // A sweep is already queued or running and owns its own reference; it
        // will observe our SweepRearm.
        //
        XdpCpuMapReleaseBacking(CpuMap);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapQueueValueRelease(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ const XDP_CPUMAP_PROVIDER_VALUE *ProviderValue
    )
{
    XDP_CPUMAP_TARGET *Target = ProviderValue->Target;

    if (Target == NULL) {
        return;
    }

    //
    // RetireWorkCount is incremented BEFORE the pending release is recorded so
    // it can never go negative: a sweep can only consume what the next line
    // writes, and every such write is preceded by this increment. It is a
    // diagnostic and a destroy-time assertion only -- nothing waits on it.
    //
    InterlockedIncrement(&CpuMap->RetireWorkCount);
    InterlockedIncrement(&Target->PendingValueReleases);

    XdpCpuMapArmSweep(CpuMap);
}

//
// Release a target's cap charges. Control chain: global outermost.
//
static
_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapReleaseCharges(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ UINT32 RingEntries,
    _In_ SIZE_T NonPagedBytes
    )
{
    RtlAcquirePushLockExclusive(&XdpCpuMapGlobalLock);
    RtlAcquirePushLockExclusive(&CpuMap->ConfigLock);

    //
    // Per-map first, then global: the exact reverse of the charge order.
    //
    ASSERT(CpuMap->ChargedRingEntries >= RingEntries);
    ASSERT(CpuMap->ChargedNonPagedBytes >= NonPagedBytes);
    CpuMap->ChargedRingEntries -= RingEntries;
    CpuMap->ChargedNonPagedBytes -= NonPagedBytes;

    ASSERT(XdpCpuMapGlobalRingEntries >= RingEntries);
    ASSERT(XdpCpuMapGlobalNonPagedBytes >= NonPagedBytes);
    XdpCpuMapGlobalRingEntries -= RingEntries;
    XdpCpuMapGlobalNonPagedBytes -= NonPagedBytes;

    ASSERT(CpuMap->OutstandingRetires > 0);
    CpuMap->OutstandingRetires--;

    RtlReleasePushLockExclusive(&CpuMap->ConfigLock);
    RtlReleasePushLockExclusive(&XdpCpuMapGlobalLock);
}

//
// Drain a ring under its lock, in bounded chunks, releasing everything each
// entry owns.
//
// Retire RETURNS rather than indicates: by the time this runs the target has
// gone, so these packets are dropped and counted RetireDropCount (section 8.3,
// "Delivery on retire"). Tombstones (Nbl == NULL) own NOTHING and the consumer
// must release nothing from them.
//
static
_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapDrainRing(
    _Inout_ XDP_CPUMAP_TARGET *Target
    )
{
    XDP_CPUMAP_RING *Ring = Target->Ring;
    UINT32 Batch = Target->OwnerMap->EffectiveDrainBatchSize;

    if (Ring == NULL) {
        return;
    }

    //
    // The sweep resets EffectiveDrainBatchSize to zero in the same critical
    // section that takes TargetCount to zero, which is the same pass that queues
    // this target for retire. Reading it here can therefore legitimately observe
    // zero, and a zero batch would make the drain loop exit immediately WITHOUT
    // draining -- silently leaking every queued NBL. Clamp it.
    //
    if (Batch == 0 || Batch > XDP_CPUMAP_DRAIN_BATCH_MAX) {
        Batch = XDP_CPUMAP_DRAIN_BATCH_DEFAULT;
    }

    for (;;) {
        KLOCK_QUEUE_HANDLE LockHandle;
        XDP_CPUMAP_NBL_CHAIN_SET ChainSet;
        UINT32 Drained = 0;
        UINT32 Returned = 0;

        XdpCpuMapChainSetInit(&ChainSet);

        KeAcquireInStackQueuedSpinLock(&Ring->Lock, &LockHandle);

        while (Ring->Head != Ring->Tail && Drained < Batch) {
            XDP_CPUMAP_ENTRY *Entry = &Ring->Entries[Ring->Head & Ring->Mask];

            if (Entry->Nbl != NULL) {
                if (!XdpCpuMapChainSetTake(&ChainSet, Entry)) {
                    break;
                }

                Returned++;
            }

            Ring->Head++;
            Drained++;
        }

        KeReleaseInStackQueuedSpinLock(&LockHandle);

        XdpCpuMapChainSetReturn(&ChainSet);

        if (Returned > 0) {
            InterlockedAdd64(&XdpCpuMapSweepStats.RetireDropCount, (LONG64)Returned);
        }

        if (Drained == 0) {
            break;
        }
    }
}

//
// Retire one target. Called from the sweep with no CPUMAP lock held.
//
// Step order is load-bearing and was wrong twice before:
//
//   1. Wait on the rundown FIRST. This stops producers. It is bounded and
//      CPU-independent because NO RING ENTRY HOLDS A RUNDOWN REFERENCE -- the
//      only holders are producers, DPC self-requeue windows, and quiesce ring
//      passes, all of which release without needing the target CPU to run.
//   2. KeRemoveQueueDpc cancels an instance queued before the wait completed.
//   3. KeFlushQueuedDpcs waits out an executing instance. It cannot re-queue
//      itself, because the self-requeue path must acquire the rundown, which now
//      fails.
//   4. Only then drain the ring, synchronously, on this thread.
//
// Draining before waiting races live producers (round 2). Waiting on a rundown
// that ring entries hold deadlocks whenever the target CPU cannot run its DPC
// (round 3). Splitting producer gating from DPC-instance gating removes both.
//
static
_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapRetireTarget(
    _Inout_ _Post_invalid_ XDP_CPUMAP_TARGET *Target
    )
{
    XDP_CPUMAP *CpuMap = Target->OwnerMap;
    UINT32 ChargedRingEntries = Target->ChargedRingEntries;
    SIZE_T ChargedNonPagedBytes = Target->ChargedNonPagedBytes;

    TraceVerbose(
        TRACE_CORE, "CpuMapId=%u Cpu=%u Retiring target",
        CpuMap->CpuMapId, Target->AbsoluteCpu);

    ExWaitForRundownProtectionRelease(&Target->PacketRundown);

    if (Target->Dpc != NULL) {
        KeRemoveQueueDpc(Target->Dpc);
        KeFlushQueuedDpcs();
    }

    XdpCpuMapDrainRing(Target);

    if (Target->Dpc != NULL) {
        ExFreePoolWithTag(Target->Dpc, XDP_POOLTAG_CPUMAP);
        Target->Dpc = NULL;
    }

    if (Target->Ring != NULL) {
        ExFreePoolWithTag(Target->Ring, XDP_POOLTAG_CPUMAP);
        Target->Ring = NULL;
    }

    //
    // RetireWorkCount is NOT decremented here. It counts released VALUES, not
    // retired TARGETS, and with targets shared across keys the two differ: two
    // keys naming one CPU produce two releases but at most one retire. The sweep
    // subtracts exactly what it consumed, which is the only balanced accounting.
    //

    XdpCpuMapReleaseCharges(CpuMap, ChargedRingEntries, ChargedNonPagedBytes);

    //
    // The shell is not part of the nonpaged cap charge above, because
    // epoch_free only queues it for later reclamation. It is still epoch-freed
    // last, so a caller that reached this target through a provider value
    // inside its epoch may still touch PacketRundown and PendingValueReleases
    // safely.
    //
    // This is the ONE epoch memory operation in CPUMAP that is not already
    // inside a provider dispatch callback: the sweep runs on XDP's own work
    // queue. ebpf_extension.h states that every epoch memory operation MUST be
    // made inside an epoch-protected region, that provider dispatch invocations
    // and BPF helper callbacks are already protected, and that a provider using
    // these APIs outside those contexts must establish the region itself. A work
    // item is exactly that case, so enter one explicitly.
    //
    // The region deliberately wraps the FREE ALONE and not the retire. Retiring
    // blocks on ExWaitForRundownProtectionRelease and KeFlushQueuedDpcs, and an
    // epoch held across those would stall reclamation for every other epoch user
    // for a duration set by the data path rather than by this thread. epoch_enter
    // and epoch_exit are re-entrant but must be paired, so the narrow scope costs
    // nothing and bounds the exposure to a few instructions.
    //
    {
        epoch_state_t EpochState;

        CpuMap->ClientDispatch->epoch_enter(&EpochState);
        CpuMap->ClientDispatch->epoch_free(Target);
        CpuMap->ClientDispatch->epoch_exit(&EpochState);
    }
}

//
// One sweep pass over one map.
//
// Applies pending value releases under ConfigLock -- which is the same lock that
// governs target lookup and reuse in XdpCpuMapResolveTarget -- so the decision
// to retire and the decision to reuse can never race. postprocess_map_delete_element
// records the pending release and decides nothing precisely so that this can be
// true.
//
static
_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapSweepPass(
    _Inout_ XDP_CPUMAP *CpuMap
    )
{
    SINGLE_LIST_ENTRY Retiring = {0};
    UINT32 Consumed = 0;
    UINT32 NoOp = 0;

    RtlAcquirePushLockExclusive(&XdpCpuMapGlobalLock);
    RtlAcquirePushLockExclusive(&CpuMap->ConfigLock);

    for (UINT32 Index = 0; Index < CpuMap->TargetTableSize; Index++) {
        XDP_CPUMAP_TARGET *Target = CpuMap->TargetTable[Index];
        LONG Pending;

        if (Target == NULL) {
            continue;
        }

        Pending = InterlockedExchange(&Target->PendingValueReleases, 0);
        if (Pending == 0) {
            continue;
        }

        Consumed += (UINT32)Pending;

        ASSERT(Target->ValueRefCount >= Pending);
        Target->ValueRefCount -= Pending;

        if (Target->ValueRefCount > 0) {
            //
            // Either the target was resurrected by an update that landed after
            // the release was recorded, or other keys still name it. Either way
            // it stays live; the decision is made here, under the lock that
            // governs reuse, and never in the delete callback.
            //
            NoOp++;
            continue;
        }

        Target->Active = FALSE;
        MemoryBarrier();

        CpuMap->TargetTable[Index] = NULL;
        ASSERT(CpuMap->TargetCount > 0);
        CpuMap->TargetCount--;

        if (CpuMap->TargetCount == 0) {
            //
            // Deferred reset of the map-level settings. It happens here, keyed
            // off the committed aggregate, because preprocess_map_update_element
            // cannot know whether its operation will insert or replace and so
            // cannot maintain a provider-side entry count.
            //
            CpuMap->EffectiveRingDepth = 0;
            CpuMap->EffectiveDrainBatchSize = 0;
        }

        CpuMap->OutstandingRetires++;

        Target->RetireLink.Next = Retiring.Next;
        Retiring.Next = &Target->RetireLink;
    }

    RtlReleasePushLockExclusive(&CpuMap->ConfigLock);
    RtlReleasePushLockExclusive(&XdpCpuMapGlobalLock);

    while (Retiring.Next != NULL) {
        XDP_CPUMAP_TARGET *Target =
            CONTAINING_RECORD(Retiring.Next, XDP_CPUMAP_TARGET, RetireLink);

        Retiring.Next = Target->RetireLink.Next;
        XdpCpuMapRetireTarget(Target);
    }

    //
    // Balance RetireWorkCount against what this pass actually applied, AFTER
    // applying it. One increment was taken per released value in
    // XdpCpuMapQueueValueRelease, and Consumed is the sum of the pending counts
    // this pass exchanged out, so the two match exactly -- including for targets
    // that turned out to be no-ops, which consume a release without retiring.
    //
    if (Consumed > 0) {
        InterlockedAdd(&CpuMap->RetireWorkCount, -(LONG)Consumed);
    }

    if (Consumed == 0) {
        InterlockedIncrement64(&XdpCpuMapSweepStats.EmptySweepCount);
        TraceVerbose(TRACE_CORE, "CpuMapId=%u Empty sweep", CpuMap->CpuMapId);
    }

    if (NoOp > 0) {
        InterlockedAdd64(&XdpCpuMapSweepStats.RetireCheckNoOp, NoOp);
        TraceVerbose(
            TRACE_CORE, "CpuMapId=%u RetireCheckNoOp=%u", CpuMap->CpuMapId, NoOp);
    }
}

//
// Sweep one map, then release its reference as the FINAL access to it. The map
// may be freed the instant that release happens.
//
static
_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapSweepMap(
    _Inout_ XDP_CPUMAP *CpuMap
    )
{
    InterlockedExchange(&CpuMap->SweepState, XdpCpuMapSweepRunning);

    for (;;) {
        InterlockedExchange(&CpuMap->SweepRearm, 0);

        XdpCpuMapSweepPass(CpuMap);

        InterlockedExchange(&CpuMap->SweepState, XdpCpuMapSweepIdle);

        if (InterlockedExchange(&CpuMap->SweepRearm, 0) == 0) {
            break;
        }

        if (InterlockedCompareExchange(
                &CpuMap->SweepState, XdpCpuMapSweepQueued, XdpCpuMapSweepIdle) !=
                    XdpCpuMapSweepIdle) {
            //
            // Another Arm claimed the state and owns its own reference.
            //
            break;
        }

        InterlockedExchange(&CpuMap->SweepState, XdpCpuMapSweepRunning);
    }

    XdpCpuMapReleaseBacking(CpuMap);
}

//
// XDP_WORK_QUEUE_ROUTINE. The routine is handed the WHOLE captured list, not a
// single entry, and XdpIoWorkItemRoutine calls it once per flush.
//
// Entry->Next MUST be cached before the map is processed: XdpCpuMapSweepMap
// passes through a transient Idle between passes, and an Arm that wins the CAS
// at that instant calls XdpInsertWorkQueue, which rewrites SweepEntry.Next to
// link the entry into the queue's NEW list. Reading Next afterwards would follow
// that new list and skip or repeat maps.
//
_Function_class_(XDP_WORK_QUEUE_ROUTINE)
_IRQL_requires_(PASSIVE_LEVEL)
static
VOID
XdpCpuMapSweepWorker(
    _In_ SINGLE_LIST_ENTRY *WorkQueueHead
    )
{
    SINGLE_LIST_ENTRY *Entry = WorkQueueHead;

    while (Entry != NULL) {
        SINGLE_LIST_ENTRY *Next = Entry->Next;
        XDP_CPUMAP *CpuMap = CONTAINING_RECORD(Entry, XDP_CPUMAP, SweepEntry);

        XdpCpuMapSweepMap(CpuMap);

        //
        // CpuMap may already have been freed by a concurrent destroy. Do not
        // touch it -- including its embedded SweepEntry -- from here.
        //
        Entry = Next;
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
XdpCpuMapCreate(
    _In_ ebpf_base_map_client_dispatch_table_t *ClientDispatch,
    _In_ UINT32 MaxEntries,
    _Outptr_ XDP_CPUMAP **CpuMap
    )
{
    XDP_CPUMAP *Map = NULL;
    UINT32 TableSize;
    SIZE_T HelperStatsBytes = 0;
    NTSTATUS Status;
    BOOLEAN HelperStatsCharged = FALSE;

    *CpuMap = NULL;

    //
    // The target table is indexed by ABSOLUTE PROCESSOR INDEX, so it is sized by
    // the processor index range. XDP_CPUMAP_MAX_TARGETS_PER_MAP caps the live
    // target COUNT and must not be used here: on a machine with more logical
    // processors than that cap, a system-valid index would write out of bounds.
    //
    TableSize = KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (TableSize == 0) {
        Status = STATUS_INTERNAL_ERROR;
        goto Exit;
    }

    HelperStatsBytes = (SIZE_T)TableSize * sizeof(XDP_CPUMAP_HELPER_STATS);
    if (HelperStatsBytes > XDP_CPUMAP_MAX_NONPAGED_BYTES) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    Map = ClientDispatch->epoch_allocate_with_tag(sizeof(*Map), XDP_POOLTAG_CPUMAP);
    if (Map == NULL) {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    RtlZeroMemory(Map, sizeof(*Map));

    Map->TargetTable =
        ExAllocatePoolZero(
            NonPagedPoolNx, sizeof(XDP_CPUMAP_TARGET *) * TableSize, XDP_POOLTAG_CPUMAP);
    if (Map->TargetTable == NULL) {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    RtlAcquirePushLockExclusive(&XdpCpuMapGlobalLock);
    if (XdpCpuMapGlobalNonPagedBytes >
            XDP_CPUMAP_GLOBAL_MAX_NONPAGED_BYTES - HelperStatsBytes) {
        RtlReleasePushLockExclusive(&XdpCpuMapGlobalLock);
        TraceError(TRACE_CORE, "CPUMAP helper stats global cap exhausted");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    XdpCpuMapGlobalNonPagedBytes += HelperStatsBytes;
    RtlReleasePushLockExclusive(&XdpCpuMapGlobalLock);
    HelperStatsCharged = TRUE;

    Map->HelperStats =
        ExAllocatePoolZero(NonPagedPoolNxCacheAligned, HelperStatsBytes, XDP_POOLTAG_CPUMAP);
    if (Map->HelperStats == NULL) {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    Map->Header.Type = XdpEbpfMapTypeCpuMap;
    Map->ClientDispatch = ClientDispatch;
    Map->MaxEntries = MaxEntries;
    Map->TargetTableSize = TableSize;
    Map->HelperStatsCount = TableSize;
    Map->HelperStatsBytes = HelperStatsBytes;
    Map->ChargedNonPagedBytes = HelperStatsBytes;
    Map->SweepState = XdpCpuMapSweepIdle;

    ExInitializePushLock(&Map->ConfigLock);
    KeInitializeEvent(&Map->RefCountZero, NotificationEvent, FALSE);

    //
    // The OWNER reference. It means "the eBPF map object still exists".
    // XdpCpuMapDestroy drops it and waits.
    //
    Map->RefCount = 1;
    Map->Active = TRUE;

    RtlAcquirePushLockExclusive(&XdpCpuMapRegistryLock);

    if (XdpCpuMapRegistryCount >= RTL_NUMBER_OF(XdpCpuMapRegistry)) {
        RtlReleasePushLockExclusive(&XdpCpuMapRegistryLock);
        TraceError(TRACE_CORE, "CPUMAP registry full");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    for (UINT32 Index = 0; Index < RTL_NUMBER_OF(XdpCpuMapRegistry); Index++) {
        if (XdpCpuMapRegistry[Index] == NULL) {
            XdpCpuMapRegistry[Index] = Map;
            Map->RegistryIndex = Index;
            Map->Registered = TRUE;
            XdpCpuMapRegistryCount++;
            break;
        }
    }

    ASSERT(Map->Registered);
    Map->CpuMapId = ++XdpCpuMapNextId;

    RtlReleasePushLockExclusive(&XdpCpuMapRegistryLock);

    TraceVerbose(
        TRACE_CORE, "CpuMapId=%u MaxEntries=%u TargetTableSize=%u Created",
        Map->CpuMapId, MaxEntries, TableSize);

    *CpuMap = Map;
    Map = NULL;
    Status = STATUS_SUCCESS;

Exit:

    if (Map != NULL) {
        if (Map->HelperStats != NULL) {
            //
            // The helper stats reservation is a hard nonpaged cap, so the
            // actual allocation must be freed before its charge is released.
            // If the allocation itself failed, HelperStats is NULL and the
            // reservation is released below with nothing to free.
            //
            ExFreePoolWithTag(Map->HelperStats, XDP_POOLTAG_CPUMAP);
            Map->HelperStats = NULL;
        }
        if (HelperStatsCharged) {
            RtlAcquirePushLockExclusive(&XdpCpuMapGlobalLock);
            ASSERT(XdpCpuMapGlobalNonPagedBytes >= HelperStatsBytes);
            XdpCpuMapGlobalNonPagedBytes -= HelperStatsBytes;
            RtlReleasePushLockExclusive(&XdpCpuMapGlobalLock);
        }
        if (Map->TargetTable != NULL) {
            ExFreePoolWithTag(Map->TargetTable, XDP_POOLTAG_CPUMAP);
        }
        ClientDispatch->epoch_free(Map);
    }

    return Status;
}

//
// Destroy the backing object. Reached from postprocess_map_delete in two shapes:
// normal destruction, where the runtime has already delivered the element-delete
// callback for every stored value; and failed base-map creation, where no value
// was ever committed.
//
// This waits on the BACKING REFCOUNT, not on counters. Every queued or running
// sweep owns a reference, so the wait subsumes the case of a sweep still queued
// against this map's EMBEDDED SweepEntry -- which a counter-based wait does not,
// because XdpInsertWorkQueue returns before the routine runs.
//
_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapDestroy(
    _Inout_ _Post_invalid_ XDP_CPUMAP *CpuMap
    )
{
    ebpf_base_map_client_dispatch_table_t *ClientDispatch = CpuMap->ClientDispatch;

    TraceEnter(TRACE_CORE, "CpuMapId=%u", CpuMap->CpuMapId);

    CpuMap->Active = FALSE;
    MemoryBarrier();

    //
    // Registry removal must precede dropping the owner reference. Quiesce takes
    // its per-map reference under this same lock, so after this returns quiesce
    // can never find this map, and any quiesce that already found it holds a
    // reference the wait below covers.
    //
    RtlAcquirePushLockExclusive(&XdpCpuMapRegistryLock);
    if (CpuMap->Registered) {
        ASSERT(XdpCpuMapRegistry[CpuMap->RegistryIndex] == CpuMap);
        XdpCpuMapRegistry[CpuMap->RegistryIndex] = NULL;
        XdpCpuMapRegistryCount--;
        CpuMap->Registered = FALSE;
    }
    RtlReleasePushLockExclusive(&XdpCpuMapRegistryLock);

    XdpCpuMapReleaseBacking(CpuMap);

    KeWaitForSingleObject(&CpuMap->RefCountZero, Executive, KernelMode, FALSE, NULL);

    //
    // Sound only because the wait above proved no sweep is outstanding, so every
    // value's release has been applied. A surviving target here is a real leak.
    //
    ASSERT(CpuMap->RetireWorkCount == 0);
    ASSERT(CpuMap->OutstandingRetires == 0);
    ASSERT(CpuMap->TargetCount == 0);
    ASSERT(CpuMap->ChargedRingEntries == 0);
    ASSERT(CpuMap->ChargedNonPagedBytes == CpuMap->HelperStatsBytes);

    if (CpuMap->HelperStats != NULL) {
        //
        // Section 12 observability: the ONLY way these counters leave the
        // driver. They are per-CPU and map-scoped, so this aggregates them here,
        // at map destruction, which is the last moment the array is valid and
        // the only point at which the totals are final. Queue teardown would be
        // the wrong scope -- a map outlives any one queue.
        //
        // This exists because the section 12 guidance is operational: a non-zero
        // MetadataUnsupported means revisit the carried set, a non-zero
        // DescriptorResidue means the allocator assumption broke. Guidance about
        // counters nobody can read is not guidance, and shipping it that way is
        // what made this trace necessary.
        //
        XDP_CPUMAP_HELPER_STATS Totals;

        XdpCpuMapQueryHelperStats(CpuMap, &Totals);

        TraceInfo(
            TRACE_CORE,
            "CpuMapId=%u DeepCopy summary Build=%llu Fail=%llu MetadataUnsupported=%llu "
            "DescriptorResidue=%llu RingFull=%llu EnqueueTargetInactive=%llu",
            CpuMap->CpuMapId, Totals.DeepCopyBuildCount, Totals.DeepCopyFailCount,
            Totals.DeepCopyMetadataUnsupported, Totals.DeepCopyDescriptorResidue,
            Totals.RingFullCount, Totals.EnqueueTargetInactive);

        ExFreePoolWithTag(CpuMap->HelperStats, XDP_POOLTAG_CPUMAP);
        CpuMap->HelperStats = NULL;
    }

    RtlAcquirePushLockExclusive(&XdpCpuMapGlobalLock);
    RtlAcquirePushLockExclusive(&CpuMap->ConfigLock);
    ASSERT(XdpCpuMapGlobalNonPagedBytes >= CpuMap->HelperStatsBytes);
    ASSERT(CpuMap->ChargedNonPagedBytes >= CpuMap->HelperStatsBytes);
    XdpCpuMapGlobalNonPagedBytes -= CpuMap->HelperStatsBytes;
    CpuMap->ChargedNonPagedBytes -= CpuMap->HelperStatsBytes;
    RtlReleasePushLockExclusive(&CpuMap->ConfigLock);
    RtlReleasePushLockExclusive(&XdpCpuMapGlobalLock);

    ASSERT(CpuMap->ChargedNonPagedBytes == 0);

    if (CpuMap->TargetTable != NULL) {
        ExFreePoolWithTag(CpuMap->TargetTable, XDP_POOLTAG_CPUMAP);
        CpuMap->TargetTable = NULL;
    }

    //
    // postprocess_map_delete is NOT epoch-protected on the destruction path,
    // even though it is a provider callback. The runtime registers the object's
    // free routine as an EPOCH WORK ITEM (ebpf_object.c
    // ebpf_epoch_allocate_work_item(object, _ebpf_object_epoch_free)), and
    // _ebpf_epoch_work_item_callback invokes work_item->callback with no
    // ebpf_epoch_enter around it. By construction that runs AFTER the epoch has
    // retired -- which is exactly why a provider is permitted to block here, and
    // necessarily why there is no epoch to inherit.
    //
    // Callback category is therefore the wrong discriminator; DISPATCH PATH is
    // the right one. Callbacks reached through protocol dispatch are protected
    // because ebpf_core.c enters an epoch around every handler. This one is not
    // reached that way.
    //
    // N.B. the runtime also invokes postprocess_map_delete from the map-create
    // FAILURE path in ebpf_maps.c, which IS inside protocol dispatch and so IS
    // protected. The callback can thus run in either state. Bracketing
    // unconditionally is correct for both, because epoch_enter and epoch_exit
    // are documented re-entrant so long as they are paired.
    //
    {
        epoch_state_t EpochState;

        ClientDispatch->epoch_enter(&EpochState);
        ClientDispatch->epoch_free(CpuMap);
        ClientDispatch->epoch_exit(&EpochState);
    }

    TraceExitSuccess(TRACE_CORE);
}

//
// Allocate and publish a target for one absolute CPU.
//
// Called with XdpCpuMapGlobalLock and CpuMap->ConfigLock held EXCLUSIVE, so the
// cap reservation, the allocation and the publication are one atomic step. That
// is why the global lock is outermost rather than a leaf: releasing ConfigLock
// to charge would open duplicate-target and sizing races.
//
static
_IRQL_requires_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
XdpCpuMapCreateTarget(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ UINT32 AbsoluteCpu,
    _In_ const PROCESSOR_NUMBER *ProcNumber,
    _Outptr_ XDP_CPUMAP_TARGET **NewTarget
    )
{
    XDP_CPUMAP_TARGET *Target = NULL;
    XDP_CPUMAP_RING *Ring = NULL;
    KDPC *Dpc = NULL;
    SIZE_T RingBytes;
    SIZE_T ChargeBytes;
    UINT32 RingDepth = CpuMap->EffectiveRingDepth;
    NTSTATUS Status;

    *NewTarget = NULL;

    RingBytes =
        sizeof(XDP_CPUMAP_RING) + ((SIZE_T)RingDepth * sizeof(XDP_CPUMAP_ENTRY));
    ChargeBytes = XdpCpuMapGetPoolChargeSize(RingBytes + sizeof(KDPC));

    //
    // Cap admission. Order is global reservation, then per-map reservation, then
    // allocate, then publish; every failure releases in exact reverse. An update
    // that reuses an existing target never reaches here and charges nothing.
    //
    if (XdpCpuMapGlobalRingEntries > XDP_CPUMAP_GLOBAL_MAX_RING_ENTRIES - RingDepth ||
        XdpCpuMapGlobalNonPagedBytes >
            XDP_CPUMAP_GLOBAL_MAX_NONPAGED_BYTES - ChargeBytes) {
        TraceError(
            TRACE_CORE, "CpuMapId=%u Global cap exhausted", CpuMap->CpuMapId);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    XdpCpuMapGlobalRingEntries += RingDepth;
    XdpCpuMapGlobalNonPagedBytes += ChargeBytes;

    if (CpuMap->TargetCount >= XDP_CPUMAP_MAX_TARGETS_PER_MAP ||
        CpuMap->TargetCount >= KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS) ||
        CpuMap->ChargedRingEntries > XDP_CPUMAP_MAX_TOTAL_RING_ENTRIES - RingDepth ||
        CpuMap->ChargedNonPagedBytes > XDP_CPUMAP_MAX_NONPAGED_BYTES - ChargeBytes) {
        TraceError(
            TRACE_CORE, "CpuMapId=%u Per-map cap exhausted", CpuMap->CpuMapId);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto ExitReleaseGlobal;
    }

    CpuMap->ChargedRingEntries += RingDepth;
    CpuMap->ChargedNonPagedBytes += ChargeBytes;

    Ring = ExAllocatePoolZero(NonPagedPoolNxCacheAligned, RingBytes, XDP_POOLTAG_CPUMAP);
    if (Ring == NULL) {
        Status = STATUS_NO_MEMORY;
        goto ExitReleasePerMap;
    }

    Dpc = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Dpc), XDP_POOLTAG_CPUMAP);
    if (Dpc == NULL) {
        Status = STATUS_NO_MEMORY;
        goto ExitReleasePerMap;
    }

    Target =
        CpuMap->ClientDispatch->epoch_allocate_with_tag(
            sizeof(*Target), XDP_POOLTAG_CPUMAP);
    if (Target == NULL) {
        Status = STATUS_NO_MEMORY;
        goto ExitReleasePerMap;
    }

    RtlZeroMemory(Target, sizeof(*Target));

    KeInitializeSpinLock(&Ring->Lock);
    Ring->Capacity = RingDepth;
    Ring->Mask = RingDepth - 1;

    KeInitializeDpc(Dpc, XdpCpuMapDrainDpc, Target);

    //
    // A DPC whose target processor was never set runs on whichever CPU queues
    // it, which for this map means redirect silently degrades to no redirect at
    // all. Proceeding past a failure here would produce a target that looks
    // healthy and steers nothing, so fail the update instead.
    //
    Status = KeSetTargetProcessorDpcEx(Dpc, (PPROCESSOR_NUMBER)ProcNumber);
    if (!NT_SUCCESS(Status)) {
        TraceError(
            TRACE_CORE, "CpuMapId=%u Cpu=%u KeSetTargetProcessorDpcEx failed Status=%!STATUS!",
            CpuMap->CpuMapId, AbsoluteCpu, Status);
        goto ExitReleasePerMap;
    }

    ExInitializeRundownProtection(&Target->PacketRundown);
    Target->AbsoluteCpu = AbsoluteCpu;
    Target->Ring = Ring;
    Target->Dpc = Dpc;
    Target->OwnerMap = CpuMap;
    Target->ValueRefCount = 1;
    Target->ChargedRingEntries = RingDepth;
    Target->ChargedNonPagedBytes = ChargeBytes;
    MemoryBarrier();
    Target->Active = TRUE;

    CpuMap->TargetTable[AbsoluteCpu] = Target;
    CpuMap->TargetCount++;

    TraceVerbose(
        TRACE_CORE, "CpuMapId=%u Cpu=%u RingDepth=%u Target created",
        CpuMap->CpuMapId, AbsoluteCpu, RingDepth);

    *NewTarget = Target;
    return STATUS_SUCCESS;

ExitReleasePerMap:

    //
    // Keep the hard-cap invariant conservative on failure: any charged
    // allocation that exists is synchronously freed while its charge is still
    // accounted, then the charge is released below. Target is deliberately not
    // part of ChargeBytes because epoch_free only queues it for later
    // reclamation, so it is released after the charge.
    //
    if (Dpc != NULL) {
        ExFreePoolWithTag(Dpc, XDP_POOLTAG_CPUMAP);
        Dpc = NULL;
    }
    if (Ring != NULL) {
        ExFreePoolWithTag(Ring, XDP_POOLTAG_CPUMAP);
        Ring = NULL;
    }

    CpuMap->ChargedRingEntries -= RingDepth;
    CpuMap->ChargedNonPagedBytes -= ChargeBytes;

ExitReleaseGlobal:

    XdpCpuMapGlobalRingEntries -= RingDepth;
    XdpCpuMapGlobalNonPagedBytes -= ChargeBytes;

Exit:

    if (Target != NULL) {
        CpuMap->ClientDispatch->epoch_free(Target);
    }

    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
XdpCpuMapResolveTarget(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ const XDP_CPUMAP_ENTRY_V1 *Entry,
    _Out_ XDP_CPUMAP_PROVIDER_VALUE *ProviderValue
    )
{
    XDP_CPUMAP_TARGET *Target;
    PROCESSOR_NUMBER ProcNumber;
    NTSTATUS Status;

    RtlZeroMemory(ProviderValue, sizeof(*ProviderValue));

    if (!CpuMap->Active) {
        return STATUS_DELETE_PENDING;
    }

    //
    // Validation. THREE CPU checks are required and they reject different
    // things, and all three run on EVERY update -- including one that will reuse
    // an existing target. Validating only on the create path would accept an
    // invalid CPU index whenever some other key already named a valid one.
    //
    if (Entry->Size != XDP_CPUMAP_ENTRY_SIZE_V1 ||
        Entry->Version != XDP_CPUMAP_ENTRY_VERSION_1 ||
        Entry->Flags != 0 ||
        Entry->Reserved[0] != 0 || Entry->Reserved[1] != 0 || Entry->Reserved[2] != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    //
    // (1) The index must be within the table the map allocated, which is sized by
    // the processor index range.
    //
    if (Entry->TargetCpu >= CpuMap->TargetTableSize) {
        TraceError(
            TRACE_CORE, "CpuMapId=%u Cpu=%u exceeds target table size %u",
            CpuMap->CpuMapId, Entry->TargetCpu, CpuMap->TargetTableSize);
        return STATUS_INVALID_PARAMETER;
    }

    //
    // (2) The index must name a processor that exists.
    //
    Status = KeGetProcessorNumberFromIndex(Entry->TargetCpu, &ProcNumber);
    if (!NT_SUCCESS(Status)) {
        TraceError(
            TRACE_CORE, "CpuMapId=%u Cpu=%u is not a valid processor index",
            CpuMap->CpuMapId, Entry->TargetCpu);
        return STATUS_INVALID_PARAMETER;
    }

    //
    // Ring depth and drain batch size are MAP-LEVEL settings, and zero means
    // "inherit whatever the map established" -- section 5.3 defines zero as the
    // default-inheriting value, and loaders rely on writing zero for every entry
    // after the first. Only a NONZERO value that disagrees is an error. Range
    // validation therefore applies to nonzero values only; the zero case is
    // resolved against the map under ConfigLock below.
    //
    if (Entry->RingDepth != 0 &&
        (Entry->RingDepth < XDP_CPUMAP_RING_DEPTH_MIN ||
         Entry->RingDepth > XDP_CPUMAP_RING_DEPTH_MAX ||
         (Entry->RingDepth & (Entry->RingDepth - 1)) != 0)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Entry->DrainBatchSize != 0 &&
        (Entry->DrainBatchSize < XDP_CPUMAP_DRAIN_BATCH_MIN ||
         Entry->DrainBatchSize > XDP_CPUMAP_DRAIN_BATCH_MAX)) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlAcquirePushLockExclusive(&XdpCpuMapGlobalLock);
    RtlAcquirePushLockExclusive(&CpuMap->ConfigLock);

    //
    // Map-level settings are established by the entry that creates the FIRST
    // target and reset by the sweep when TargetCount returns to zero. There is
    // no provider-side entry count, because this callback cannot know whether
    // the core operation will insert or replace.
    //
    if (CpuMap->TargetCount == 0) {
        CpuMap->EffectiveRingDepth =
            (Entry->RingDepth != 0) ? Entry->RingDepth : XDP_CPUMAP_RING_DEPTH_DEFAULT;
        CpuMap->EffectiveDrainBatchSize =
            (Entry->DrainBatchSize != 0) ?
                Entry->DrainBatchSize : XDP_CPUMAP_DRAIN_BATCH_DEFAULT;
    } else if ((Entry->RingDepth != 0 &&
                Entry->RingDepth != CpuMap->EffectiveRingDepth) ||
               (Entry->DrainBatchSize != 0 &&
                Entry->DrainBatchSize != CpuMap->EffectiveDrainBatchSize)) {
        TraceError(
            TRACE_CORE, "CpuMapId=%u Nonzero ring/drain config disagrees with map",
            CpuMap->CpuMapId);
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    Target = CpuMap->TargetTable[Entry->TargetCpu];

    if (Target != NULL) {
        //
        // A target reachable from the table has never begun retiring: the sweep
        // unlinks it under this same lock BEFORE it publishes inactive. There is
        // no intermediate observable state and so no resurrection window.
        //
        ASSERT(Target->Active);
        Target->ValueRefCount++;
    } else {
        Status = XdpCpuMapCreateTarget(CpuMap, Entry->TargetCpu, &ProcNumber, &Target);
        if (!NT_SUCCESS(Status)) {
            goto Exit;
        }
    }

    ProviderValue->Entry = *Entry;
    ProviderValue->Target = Target;
    Status = STATUS_SUCCESS;

Exit:

    RtlReleasePushLockExclusive(&CpuMap->ConfigLock);
    RtlReleasePushLockExclusive(&XdpCpuMapGlobalLock);

    return Status;
}

//
// Quiesce.
//
// Tombstones in place rather than draining: the NBL and both references a slot
// owns are moved out under the ring lock in the same critical section that
// clears the slot, and Head/Tail are never touched. That means no compaction, no
// reordering within a target's ring, and -- crucially -- the expensive work is
// proportional to the PAUSING QUEUE's occupancy, while only a pointer comparison
// per entry is proportional to global occupancy.
//
// The tail is SNAPSHOTTED. Following a live tail would let a peer producer
// extend a single pass indefinitely, because ring capacity bounds occupancy but
// not cumulative scanning.
//
static
_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapQuiesceScope(
    _In_opt_ const VOID *RxQueueOwner,
    _In_opt_ const VOID *GenericOwner
    )
{
    LARGE_INTEGER Start;
    LARGE_INTEGER End;
    LARGE_INTEGER Frequency;
    UINT32 Passes = 0;
    UINT32 Matched;
    LONG64 MapsVisited = 0;
    LONG64 TargetsVisited = 0;
    LONG64 EntriesScanned = 0;
    LONG64 Tombstoned = 0;
    LONG64 DurationUs;

    ASSERT((RxQueueOwner != NULL) != (GenericOwner != NULL));

    Start = KeQueryPerformanceCounter(&Frequency);

    do {
        Matched = 0;

        for (UINT32 MapIndex = 0; MapIndex < RTL_NUMBER_OF(XdpCpuMapRegistry); MapIndex++) {
            XDP_CPUMAP *CpuMap;

            //
            // Reference the map under the registry lock, then release the lock
            // before doing any ring work. Destroy removes the map from the
            // registry before dropping its owner reference, so we either miss it
            // entirely or hold a reference destroy will wait for.
            //
            RtlAcquirePushLockShared(&XdpCpuMapRegistryLock);
            CpuMap = XdpCpuMapRegistry[MapIndex];
            if (CpuMap != NULL) {
                XdpCpuMapReferenceBacking(CpuMap);
            }
            RtlReleasePushLockShared(&XdpCpuMapRegistryLock);

            if (CpuMap == NULL) {
                continue;
            }

            MapsVisited++;

            for (UINT32 CpuIndex = 0; CpuIndex < CpuMap->TargetTableSize; CpuIndex++) {
                XDP_CPUMAP_TARGET *Target;
                XDP_CPUMAP_RING *Ring;
                UINT32 TailSnapshot;
                UINT32 Index;
                UINT32 Batch;
                BOOLEAN Held = FALSE;

                RtlAcquirePushLockShared(&CpuMap->ConfigLock);
                Target = CpuMap->TargetTable[CpuIndex];
                if (Target != NULL) {
                    //
                    // Pin the target with the same rundown the data path uses,
                    // so retire cannot free the ring underneath this pass. A
                    // failed acquire means the target is already retiring and
                    // its own sweep drains the ring synchronously.
                    //
                    Held = ExAcquireRundownProtection(&Target->PacketRundown);
                }
                Batch = CpuMap->EffectiveDrainBatchSize;
                RtlReleasePushLockShared(&CpuMap->ConfigLock);

                if (Target == NULL || !Held) {
                    continue;
                }

                TargetsVisited++;

                Ring = Target->Ring;
                if (Batch == 0) {
                    Batch = XDP_CPUMAP_DRAIN_BATCH_DEFAULT;
                }

                {
                    KLOCK_QUEUE_HANDLE LockHandle;

                    KeAcquireInStackQueuedSpinLock(&Ring->Lock, &LockHandle);
                    TailSnapshot = Ring->Tail;
                    Index = Ring->Head;
                    KeReleaseInStackQueuedSpinLock(&LockHandle);
                }

                while (Index != TailSnapshot) {
                    KLOCK_QUEUE_HANDLE LockHandle;
                    XDP_CPUMAP_NBL_CHAIN_SET ChainSet;
                    UINT32 Scanned = 0;

                    XdpCpuMapChainSetInit(&ChainSet);

                    KeAcquireInStackQueuedSpinLock(&Ring->Lock, &LockHandle);

                    while (Index != TailSnapshot && Scanned < Batch) {
                        XDP_CPUMAP_ENTRY *Slot = &Ring->Entries[Index & Ring->Mask];

                        if (Slot->Nbl != NULL &&
                            ((RxQueueOwner != NULL && Slot->RxQueueOwner == RxQueueOwner) ||
                             (GenericOwner != NULL && Slot->GenericOwner == GenericOwner))) {
                            //
                            // Atomic ownership transfer: the NBL and every
                            // reference the slot owned move out in the same
                            // critical section that clears the slot, so the
                            // tombstone left behind owns NOTHING and consumers
                            // release nothing from it.
                            //
                            // Head and Tail are never touched: the slot stays
                            // where it is, so there is no compaction, no
                            // reordering, and no interaction with producers'
                            // index arithmetic (section 8.4, correction A).
                            //
                            if (!XdpCpuMapChainSetTake(&ChainSet, Slot)) {
                                //
                                // More distinct filters/ports in flight than the
                                // chain set holds. Stop without consuming this
                                // slot; the next chunk revisits it with an empty
                                // set.
                                //
                                break;
                            }

                            Matched++;
                            Tombstoned++;
                        }

                        Index++;
                        Scanned++;
                        EntriesScanned++;
                    }

                    KeReleaseInStackQueuedSpinLock(&LockHandle);

                    //
                    // No CPUMAP lock is held across the NDIS return (section 9).
                    //
                    XdpCpuMapChainSetReturn(&ChainSet);

                    if (Scanned == 0) {
                        //
                        // The set filled on the first slot of this chunk, which
                        // is impossible: it was just initialized empty. Guard
                        // against an infinite loop regardless.
                        //
                        ASSERT(FALSE);
                        break;
                    }
                }

                ExReleaseRundownProtection(&Target->PacketRundown);
            }

            XdpCpuMapReleaseBacking(CpuMap);
        }

        Passes++;
    } while (Matched > 0 && Passes < XDP_CPUMAP_QUIESCE_MAX_PASSES);

    //
    // Exactly once, after the loop. Its cost is O(processor count) and outside
    // every CPUMAP cap, so bounding the call count at one bounds it absolutely.
    //
    KeFlushQueuedDpcs();

    End = KeQueryPerformanceCounter(NULL);
    DurationUs =
        Frequency.QuadPart != 0 ?
            ((End.QuadPart - Start.QuadPart) * 1000000) / Frequency.QuadPart : 0;

    InterlockedIncrement64(&XdpCpuMapQuiesceStats.Count);
    InterlockedAdd64(&XdpCpuMapQuiesceStats.MapsVisited, MapsVisited);
    InterlockedAdd64(&XdpCpuMapQuiesceStats.TargetsVisited, TargetsVisited);
    InterlockedAdd64(&XdpCpuMapQuiesceStats.EntriesScanned, EntriesScanned);
    InterlockedAdd64(&XdpCpuMapQuiesceStats.Tombstoned, Tombstoned);
    InterlockedAdd64(&XdpCpuMapQuiesceStats.PassesTotal, Passes);
    InterlockedExchange64(&XdpCpuMapQuiesceStats.LastDurationUs, DurationUs);
    for (;;) {
        LONG64 Max = XdpCpuMapQuiesceStats.MaxDurationUs;

        if (DurationUs <= Max ||
            InterlockedCompareExchange64(
                &XdpCpuMapQuiesceStats.MaxDurationUs, DurationUs, Max) == Max) {
            break;
        }
    }
    if (Passes >= XDP_CPUMAP_QUIESCE_MAX_PASSES && Matched > 0) {
        InterlockedIncrement64(&XdpCpuMapQuiesceStats.MaxPassesExhausted);
    }

    //
    // Traced unconditionally: this line is the measurement surface for the pause
    // latency bound. Capture it with a WPP/ETW session while pausing an interface
    // at maximum queue count.
    //
    TraceInfo(
        TRACE_CORE,
        "CPUMAP quiesce Scope=%s Maps=%I64d Targets=%I64d Scanned=%I64d "
        "Tombstoned=%I64d Passes=%u DurationUs=%I64d",
        RxQueueOwner != NULL ? "RxQueue" : "Interface",
        MapsVisited, TargetsVisited, EntriesScanned, Tombstoned, Passes, DurationUs);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapQuiesceInterface(
    _In_ const VOID *GenericOwner
    )
{
    XdpCpuMapQuiesceScope(NULL, GenericOwner);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapQuiesceRxQueue(
    _In_ const VOID *RxQueueOwner
    )
{
    XdpCpuMapQuiesceScope(RxQueueOwner, NULL);
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
XdpCpuMapStart(
    VOID
    )
{
    NTSTATUS Status;

    TraceEnter(TRACE_CORE, "-");

    ExInitializePushLock(&XdpCpuMapGlobalLock);
    ExInitializePushLock(&XdpCpuMapRegistryLock);
    RtlZeroMemory(XdpCpuMapRegistry, sizeof(XdpCpuMapRegistry));
    XdpCpuMapRegistryCount = 0;
    XdpCpuMapGlobalRingEntries = 0;
    XdpCpuMapGlobalNonPagedBytes = 0;

    XdpCpuMapRetireQueue =
        XdpCreateWorkQueue(XdpCpuMapSweepWorker, DISPATCH_LEVEL, XdpDriverObject, NULL);
    if (XdpCpuMapRetireQueue == NULL) {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    Status = STATUS_SUCCESS;

Exit:

    TraceExitStatus(TRACE_CORE);
    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapStop(
    VOID
    )
{
    TraceEnter(TRACE_CORE, "-");

    //
    // Ordering: the eBPF provider is unregistered and every map destroyed before
    // this runs, so no sweep can be armed after this point.
    //
    if (XdpCpuMapRetireQueue != NULL) {
        XdpShutdownWorkQueue(XdpCpuMapRetireQueue, TRUE);
        XdpCpuMapRetireQueue = NULL;
    }

    ASSERT(XdpCpuMapRegistryCount == 0);
    ASSERT(XdpCpuMapGlobalRingEntries == 0);
    ASSERT(XdpCpuMapGlobalNonPagedBytes == 0);

    TraceExitSuccess(TRACE_CORE);
}
