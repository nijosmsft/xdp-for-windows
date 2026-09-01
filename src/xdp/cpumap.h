//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// Internal CPUMAP engine definitions, private to xdp.sys.
//
// Lifetime model, in one place, because three review rounds were lost to
// getting it wrong:
//
//   * XDP_CPUMAP_TARGET::Ring and ::Dpc are RUNDOWN-protected. They may be read
//     only while a successful ExAcquireRundownProtection(&PacketRundown) is
//     held. The retire path frees them after the rundown wait completes.
//
//   * XDP_CPUMAP_TARGET::PacketRundown and ::PendingValueReleases are
//     EPOCH-protected. The target shell is released with epoch_free, so a caller
//     that reached the target through a provider value inside its epoch may
//     touch these two even if retire has already completed.
//
//   * XDP_CPUMAP_TARGET::ValueRefCount is CONFIGLOCK-protected. It is never read
//     or written outside CpuMap->ConfigLock, and only the sweep decides
//     retirement from its value. postprocess_map_delete_element records a
//     pending release and decides nothing, because it can run at DISPATCH_LEVEL
//     beneath a base-map lock.
//
//   * XDP_CPUMAP::RefCount is the map's backing reference. Every queued or
//     running sweep owns one, as does every in-flight producer and every
//     unconsumed ring entry. postprocess_map_delete drops the owner reference
//     and waits on RefCountZero; that wait is what proves no sweep is still
//     queued against the map's embedded SweepEntry.
//

#pragma once

#include <xdpcpumap.h>

typedef struct _XDP_CPUMAP XDP_CPUMAP;

//
// One ring entry.
//
// The layout is charged by sizeof() at map update time, so it must not change
// once maps can be created in a shipped build.
//
// A slot owns exactly three things: the NBL, the CPUMAP backing reference, and
// one receive-queue NBL rundown reference. It owns NO target rundown reference;
// the producer's single per-packet reference stays with the flush group. An
// entry with Nbl == NULL is a tombstone and owns NOTHING.
//
typedef struct _XDP_CPUMAP_ENTRY {
    NET_BUFFER_LIST *Nbl;
    NDIS_HANDLE FilterHandle;

    //
    // Opaque identity tokens supplied by xdplwf at enqueue. CPUMAP compares
    // them; it never dereferences them. See xdpcpumap.h.
    //
    const VOID *RxQueueOwner;
    const VOID *GenericOwner;

    XDP_CPUMAP *BackingRef;

    //
    // The receive queue's NBL rundown, carried explicitly because the queue
    // itself is only an opaque token here. The slot owns exactly one reference
    // on it, and the object stays alive precisely because the slot owns it:
    // XdpGenericRxDeleteQueueEntry runs only after the rundown wait completes.
    //
    EX_RUNDOWN_REF *NblRundown;

    NDIS_PORT_NUMBER PortNumber;
    BOOLEAN IsDeepCopy;
} XDP_CPUMAP_ENTRY;

typedef struct DECLSPEC_CACHEALIGN _XDP_CPUMAP_RING {
    KSPIN_LOCK Lock;
    UINT32 Head;
    UINT32 Tail;
    UINT32 Capacity;
    UINT32 Mask;

    //
    // High-water occupancy. Read and written ONLY under Lock, which the
    // producer and the consumer both already hold when they touch Head or Tail,
    // so it costs no atomic and shares no discipline with anything else. This
    // is deliberately the only counter in the hot ring structure; POC A's
    // sixteen inline counters with mixed update discipline are not copied
    // (design section 7.1).
    //
    UINT32 MaxDepth;

    XDP_CPUMAP_ENTRY Entries[ANYSIZE_ARRAY];
} XDP_CPUMAP_RING;

//
// One per unique configured target CPU. Owns the ring and the DPC.
//
typedef struct _XDP_CPUMAP_TARGET {
    UINT32 AbsoluteCpu;
    XDP_CPUMAP_RING *Ring;
    KDPC *Dpc;

    //
    // Producer-side protection only. Acquired by the helper (one per packet, a
    // later increment), by the drain DPC around its own self-requeue, and by
    // quiesce around its ring pass. NEVER transferred into a ring entry, so the
    // retire wait never depends on a ring being drained and therefore never
    // depends on the target CPU being able to run.
    //
    EX_RUNDOWN_REF PacketRundown;

    LONG ValueRefCount;                 // ConfigLock
    volatile LONG PendingValueReleases; // interlocked; the only field
                                        // postprocess_map_delete_element writes

    volatile BOOLEAN Active;            // published FALSE under ConfigLock by the
                                        // sweep; re-checked under the ring lock

    SINGLE_LIST_ENTRY RetireLink;       // sweep-local list; NOT a work-queue entry
    XDP_CPUMAP *OwnerMap;

    UINT32 ChargedRingEntries;
    SIZE_T ChargedNonPagedBytes;
} XDP_CPUMAP_TARGET;

//
// The provider value. actual_value_size is sizeof(XDP_CPUMAP_PROVIDER_VALUE),
// declared in preprocess_map_create. Immutable once committed; one instance per
// successful update operation.
//
// Putting the referenced target IN THE VALUE is what makes retirement correlate
// correctly: every delete notification carries the exact value its operation
// produced, so there is no per-key side state to race or mis-correlate. This is
// the XSKMAP pattern (see ebpfxskmap.c, which stores a referenced handle).
//
// The runtime copies only the public value_size to user mode, so Target is
// never surfaced.
//
typedef struct _XDP_CPUMAP_PROVIDER_VALUE {
    XDP_CPUMAP_ENTRY_V1 Entry;
    XDP_CPUMAP_TARGET *Target;
} XDP_CPUMAP_PROVIDER_VALUE;

typedef enum _XDP_CPUMAP_SWEEP_STATE {
    XdpCpuMapSweepIdle = 0,
    XdpCpuMapSweepQueued = 1,
    XdpCpuMapSweepRunning = 2,
} XDP_CPUMAP_SWEEP_STATE;

typedef enum _XDP_CPUMAP_HELPER_FALLBACK_REASON {
    XdpCpuMapHelperFallbackBadFlags,
    XdpCpuMapHelperFallbackRedirectSlotUnconfigured,
    XdpCpuMapHelperFallbackRedirectModeUnsupported,
    XdpCpuMapHelperFallbackTargetInactive,
} XDP_CPUMAP_HELPER_FALLBACK_REASON;

//
// Quiesce instrumentation. The open architectural risk in this design is pause
// latency, and the bound claimed is one pointer comparison per ring entry
// bounded by XDP_CPUMAP_GLOBAL_MAX_RING_ENTRIES and XDP_CPUMAP_MAX_LIVE_MAPS.
// These counters exist so that bound can be MEASURED rather than asserted, and
// so that a test can assert the scan's SHAPE deterministically rather than
// through a wall-clock threshold. They are traced at the end of every quiesce.
//
typedef struct _XDP_CPUMAP_QUIESCE_STATS {
    volatile LONG64 Count;
    volatile LONG64 MapsVisited;
    volatile LONG64 TargetsVisited;
    volatile LONG64 EntriesScanned;
    volatile LONG64 Tombstoned;
    volatile LONG64 PassesTotal;
    volatile LONG64 MaxPassesExhausted;
    volatile LONG64 LastDurationUs;
    volatile LONG64 MaxDurationUs;
} XDP_CPUMAP_QUIESCE_STATS;

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapQueryQuiesceStats(
    _Out_ XDP_CPUMAP_QUIESCE_STATS *Stats
    );

//
// CPUMAP is RECEIVE-SIDE ONLY, and this is the single predicate that says so.
//
// Its drain indicates through NdisFIndicateReceiveNetBufferLists, so a send NBL
// redirected to another CPU would be injected into the receive path, and the
// teardown paths would return it to the miniport instead of completing the send.
// The generic LWF runs outbound sends through the same post-inspection path as
// receives, and an XDP program may attach to XDP_HOOK_L2/XDP_HOOK_TX, so the
// exclusion has to be real rather than assumed.
//
// It lives here, inline, so that the unit test can evaluate it against a real
// XDP_HOOK_ID rather than hand-supplying the answer. A test that supplies its
// own admission value proves nothing about the queue this decision is actually
// made for.
//
FORCEINLINE
BOOLEAN
XdpCpuMapIsHookSupported(
    _In_ CONST XDP_HOOK_ID *HookId
    )
{
    return HookId->Direction == XDP_HOOK_RX;
}

typedef enum _XDP_CPUMAP_COMMIT_REJECT_REASON {
    XdpCpuMapCommitRejectPause,
    XdpCpuMapCommitRejectRundown,
    XdpCpuMapCommitRejectDeepCopyUnsupported,
} XDP_CPUMAP_COMMIT_REJECT_REASON;

//
// Flush and drain outcomes that are NOT commit rejections: by the time they are
// reached, ownership has been committed and ActionNbl cleared, so the packet is
// unconditionally lost rather than falling back to an RX action (section 12).
//
typedef enum _XDP_CPUMAP_ENQUEUE_DROP_REASON {
    XdpCpuMapEnqueueDropTargetInactive,
    XdpCpuMapEnqueueDropRingFull,
} XDP_CPUMAP_ENQUEUE_DROP_REASON;

//
// Per-map, per-processor data-path counter shard: helper, ownership commit,
// ring enqueue, and DPC drain.
//
// Every field here is written by the DISPATCH_LEVEL data path on the shard
// belonging to the CURRENT processor, which has exactly one running writer, so
// they are ordinary increments rather than locked read-modify-writes. Readers
// aggregate all shards with aligned 64-bit loads; the result is a coherent
// monotonic total, not a stop-the-world snapshot. Teardown counters, whose
// writers run at PASSIVE_LEVEL and can be preempted, do NOT live here -- see
// XDP_CPUMAP_QUIESCE_STATS and XDP_CPUMAP_SWEEP_STATS in cpumap.c.
//
typedef struct DECLSPEC_CACHEALIGN _XDP_CPUMAP_HELPER_STATS {
    //
    // Helper FALLBACK REASONS. These record why bpf_redirect_map declined to set
    // a redirect intent, and the packet's outcome is the program's DECLARED
    // fallback action -- XDP_PASS, XDP_DROP or XDP_TX. They are the only
    // counters in this block that are not loss counters.
    //
    volatile ULONG64 Calls;
    volatile ULONG64 Success;
    volatile ULONG64 HelperBadFlags;
    volatile ULONG64 RedirectSlotUnconfigured;
    volatile ULONG64 RedirectModeUnsupported;
    volatile ULONG64 HelperTargetInactive;

    //
    // Commit-time outcomes. Every one of these is a DROP, not a fallback.
    // XdpInvokeEbpf converts a successful XDP_REDIRECT into XDP_RX_ACTION_DROP
    // before post-inspection runs, so by commit time "the normal RX action" for
    // such a frame IS drop and the program's declared fallback is no longer
    // reachable. None of these may be presented as a fallback counter.
    //
    volatile ULONG64 CommitPauseDrop;
    volatile ULONG64 CommitRundownDrop;

    //
    // Low-resource indication with no deep-copy pool. Its own counter because,
    // unlike the two above, it is a CAPABILITY gap rather than a transient
    // state, and increment 8 removes it entirely by adding the pool.
    //
    volatile ULONG64 DeepCopyUnsupportedDrop;

    //
    // Enqueue. Also drops, for the same reason.
    //
    volatile ULONG64 EnqueueCount;
    volatile ULONG64 EnqueueTargetInactive;
    volatile ULONG64 RingFullCount;

    //
    // Drain.
    //
    volatile ULONG64 DrainCount;
    volatile ULONG64 DrainTombstoneCount;
    volatile ULONG64 IndicateChainCount;
    volatile ULONG64 DpcInvokeCount;
    volatile ULONG64 DpcRequeueCount;
    volatile ULONG64 DpcEmptyCount;
} XDP_CPUMAP_HELPER_STATS;

//
// One per CPUMAP. The eBPF map context; Header must be first so a raw map
// pointer resolves through MAP_CONTEXT exactly as XSKMAP's does.
//
typedef struct _XDP_CPUMAP {
    XDP_EBPF_MAP_HEADER Header;
    ebpf_base_map_client_dispatch_table_t *ClientDispatch;

    UINT32 CpuMapId;
    UINT32 MaxEntries;
    XDP_CPUMAP_HELPER_STATS *HelperStats;
    UINT32 HelperStatsCount;
    SIZE_T HelperStatsBytes;

    EX_PUSH_LOCK ConfigLock;

    //
    // Indexed by ABSOLUTE PROCESSOR INDEX, so it is sized by the processor index
    // range, not by XDP_CPUMAP_MAX_TARGETS_PER_MAP, which caps the live target
    // COUNT. Conflating the two is an out-of-bounds write on a machine with
    // more than XDP_CPUMAP_MAX_TARGETS_PER_MAP logical processors.
    //
    XDP_CPUMAP_TARGET **TargetTable;
    UINT32 TargetTableSize;
    UINT32 TargetCount;                 // ConfigLock; the committed aggregate

    UINT32 EffectiveRingDepth;          // ConfigLock
    UINT32 EffectiveDrainBatchSize;     // ConfigLock
    UINT32 ChargedRingEntries;          // ConfigLock
    SIZE_T ChargedNonPagedBytes;        // ConfigLock

    volatile LONG RefCount;
    KEVENT RefCountZero;

    SINGLE_LIST_ENTRY SweepEntry;       // exactly one; inserted only on an
                                        // Idle -> Queued transition
    volatile LONG SweepState;
    volatile LONG SweepRearm;

    //
    // Diagnostics and destroy-time assertions ONLY. Nothing waits on these; map
    // lifetime is RefCount above.
    //
    volatile LONG RetireWorkCount;
    LONG OutstandingRetires;            // ConfigLock

    volatile BOOLEAN Active;
    BOOLEAN Registered;
    UINT32 RegistryIndex;
} XDP_CPUMAP;

//
// Module lifetime.
//
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
XdpCpuMapStart(
    VOID
    );

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapStop(
    VOID
    );

//
// Called by the eBPF provider callbacks in ebpfcpumap.c.
//
_IRQL_requires_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
XdpCpuMapCreate(
    _In_ ebpf_base_map_client_dispatch_table_t *ClientDispatch,
    _In_ UINT32 MaxEntries,
    _Outptr_ XDP_CPUMAP **CpuMap
    );

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapDestroy(
    _Inout_ _Post_invalid_ XDP_CPUMAP *CpuMap
    );

_IRQL_requires_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
XdpCpuMapResolveTarget(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ const XDP_CPUMAP_ENTRY_V1 *Entry,
    _Out_ XDP_CPUMAP_PROVIDER_VALUE *ProviderValue
    );

//
// Records a pending value release and arms the map sweep. Acquires no lock but
// the retire queue's spin lock, allocates nothing, blocks never, and DECIDES
// NOTHING: whether the release is the last one is decided by the sweep under
// ConfigLock. Safe at DISPATCH_LEVEL beneath a base-map bucket spin lock or the
// custom map's own lock.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapQueueValueRelease(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ const XDP_CPUMAP_PROVIDER_VALUE *ProviderValue
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapReferenceBacking(
    _Inout_ XDP_CPUMAP *CpuMap
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapReleaseBacking(
    _Inout_ XDP_CPUMAP *CpuMap
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapRecordHelperCall(
    _Inout_ XDP_CPUMAP *CpuMap
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapRecordHelperSuccess(
    _Inout_ XDP_CPUMAP *CpuMap
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapRecordHelperFallback(
    _Inout_ XDP_CPUMAP *CpuMap,
    _In_ XDP_CPUMAP_HELPER_FALLBACK_REASON Reason
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapQueryHelperStats(
    _In_ const XDP_CPUMAP *CpuMap,
    _Out_ XDP_CPUMAP_HELPER_STATS *Stats
    );

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapQueryGlobalStats(
    _Out_opt_ UINT32 *RingEntries,
    _Out_opt_ SIZE_T *NonPagedBytes
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
BOOLEAN
XdpCpuMapTryAcquireTargetReference(
    _Inout_ XDP_CPUMAP *CpuMap,
    _Inout_ XDP_CPUMAP_TARGET *Target
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapReleaseTargetReference(
    _Inout_ XDP_CPUMAP_TARGET *Target
    );
