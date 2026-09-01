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
// One ring entry. Nothing writes these yet: the data path is a later increment.
// The layout is fixed now because the cap accounting charges sizeof() at map
// update time and must not change once maps can be created.
//
// A slot owns exactly three things: the NBL, the CPUMAP backing reference, and
// the receive queue's NBL rundown reference. It owns NO target rundown
// reference; the producer's single per-packet reference stays with the flush
// group. An entry with Nbl == NULL is a tombstone and owns NOTHING.
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
    NDIS_PORT_NUMBER PortNumber;
    BOOLEAN IsDeepCopy;
} XDP_CPUMAP_ENTRY;

typedef struct DECLSPEC_CACHEALIGN _XDP_CPUMAP_RING {
    KSPIN_LOCK Lock;
    UINT32 Head;
    UINT32 Tail;
    UINT32 Capacity;
    UINT32 Mask;
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

typedef enum _XDP_CPUMAP_COMMIT_REJECT_REASON {
    XdpCpuMapCommitRejectPause,
    XdpCpuMapCommitRejectRundown,
    XdpCpuMapCommitRejectBatchInsertFailed,
} XDP_CPUMAP_COMMIT_REJECT_REASON;

typedef struct DECLSPEC_CACHEALIGN _XDP_CPUMAP_HELPER_STATS {
    volatile ULONG64 Calls;
    volatile ULONG64 Success;
    volatile ULONG64 HelperBadFlags;
    volatile ULONG64 RedirectSlotUnconfigured;
    volatile ULONG64 RedirectModeUnsupported;
    volatile ULONG64 HelperTargetInactive;
    volatile ULONG64 CommitPauseRejected;
    volatile ULONG64 CommitRundownRejected;
    volatile ULONG64 CommitBatchInsertFailed;
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
