//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// Private CPUMAP definitions shared between the CPUMAP engine in src/xdp and
// the generic LWF receive path in src/xdplwf.
//
// N.B. xdplwf is a static driver library linked into xdp.sys
// (src/xdplwf/xdplwf.vcxproj declares <UndockedType>drvlib</UndockedType>), so
// everything here is an internal contract, not a shipped ABI.
//

#pragma once

#include <xdp/cpumap.h>
#include <xdp/datapath.h>
#include <xdp/extension.h>
#include <xdp/rxqueueconfig.h>
#include <xdpcpumaplimits.h>

EXTERN_C_START

typedef struct _XDP_CPUMAP XDP_CPUMAP;
typedef struct _XDP_CPUMAP_TARGET XDP_CPUMAP_TARGET;

//
// Internal-only limits. Not part of the public CPUMAP ABI; these may change
// without a version bump.
//
#define XDP_CPUMAP_GLOBAL_MAX_RING_ENTRIES 1048576u
#define XDP_CPUMAP_GLOBAL_MAX_NONPAGED_BYTES (256u * 1024u * 1024u)
#define XDP_CPUMAP_MAX_LIVE_MAPS 64u
#define XDP_CPUMAP_MAX_TARGETS_PER_MAP 1024u
#define XDP_CPUMAP_QUIESCE_MAX_PASSES 8u

#define XDP_FRAME_CPUMAP_REDIRECT_VERSION_1 1u
#define XDP_FRAME_CPUMAP_REDIRECT_FLAG_OWNERSHIP_PENDING 0x00000001u
#define XDP_FRAME_CPUMAP_REDIRECT_FLAG_OWNERSHIP_COMMITTED 0x00000002u

typedef struct _XDP_FRAME_CPUMAP_REDIRECT_V1 {
    UINT16 Size;
    UINT16 Version;
    UINT32 Flags;
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_TARGET *Target;
    UINT32 TargetKey;
    UINT32 TargetCpu;
} XDP_FRAME_CPUMAP_REDIRECT_V1;

#define XDP_FRAME_EXTENSION_CPUMAP_REDIRECT_NAME L"ms_frame_cpumap_redirect"
#define XDP_FRAME_EXTENSION_CPUMAP_REDIRECT_VERSION_1 1U

inline
XDP_FRAME_CPUMAP_REDIRECT_V1 *
XdpGetCpuMapRedirectExtension(
    _In_ XDP_FRAME *Frame,
    _In_ XDP_EXTENSION *Extension
    )
{
    return (XDP_FRAME_CPUMAP_REDIRECT_V1 *)XdpGetExtensionData(Frame, Extension);
}

//
// Rundown credit pool for one flush group.
//
// Section 6.3 requires NblRundown acquisition to be batched once per flush
// group rather than taken per packet, mirroring the existing TX inject path.
// The group acquires a chunk of references with a single interlocked operation
// and then hands them to committing packets as plain group-local decrements, so
// the per-packet path performs no atomic at all. Whatever is left unconsumed is
// released in one operation when the group ends.
//
// Increment 6 extends this rather than replacing it: a packet that successfully
// inserts into the batch simply KEEPS its credit, because the credit is exactly
// the reference the ring slot goes on to own. A group that commits more than
// XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK packets therefore acquires again, which is
// legitimate and is one trip per chunk rather than one per packet.
//
// N.B. XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK and XDP_CPUMAP_BATCH_SIZE now live in
// xdpcpumaplimits.h, included above. They moved so the user-mode functional test
// can static_assert its frame count against them; this header cannot be included
// from user mode.
//

//
// Per-RX-queue deep-copy NBL cache (section 7, "NDIS resources").
//
// Owned by XDP_LWF_GENERIC_RX_QUEUE and created alongside the existing
// TxCloneNblPool from the same NdisFilterHandle, so the handle, the lifetime,
// the create-failure path and the deferred free are all pre-existing shapes.
// CPUMAP only ever reaches it through a pointer carried on the commit group and
// stamped into the ring slot, and the slot's NblRundown reference is what keeps
// it alive: XdpGenericRxDeleteQueueEntry runs only after the rundown wait
// completes, so no deep copy can outlive its own pool.
//
// Two lists, deliberately, mirroring XdpGenericRxAllocateTxCloneNbl:
//
//   FreeList   interlocked, pushed by the DRAIN DPC on the target CPU after a
//              deep copy returns synchronously from its RESOURCES indication.
//   LocalList  plain, popped by the FLUSH on the source CPU, refilled in bulk by
//              a single InterlockedFlushSList when it empties.
//
// The split is what keeps section 11 satisfied: the allocating side performs an
// interlocked operation only when its local list runs dry, not once per packet.
// LocalList needs no synchronisation because the flush runs under the receive
// queue's EC, which is single-threaded per queue -- the same argument upstream
// relies on for TxCloneNblList.
//
typedef struct DECLSPEC_CACHEALIGN _XDP_CPUMAP_DEEPCOPY_POOL {
    NDIS_HANDLE NblPool;

    //
    // The filter module handle the pool was created from, kept because an
    // originated receive NBL must carry it in SourceHandle so NDIS routes the
    // return to this filter rather than to a miniport that never owned it.
    //
    NDIS_HANDLE NdisFilterHandle;

    NET_BUFFER_LIST *LocalList;

    //
    // Descriptors ever allocated, capped at XDP_CPUMAP_DEEPCOPY_CACHE_MAX. Not
    // decremented on recycle: a recycled descriptor is reused, not freed.
    // Touched only by the flush, so it needs no interlocked update.
    //
    UINT32 CacheCount;

    //
    // Separated from the fields above because the drain DPC writes it from the
    // TARGET CPU while the flush reads LocalList and CacheCount on the source
    // CPU. Sharing a line would put an interlocked push in the middle of the
    // allocating CPU's working set on every recycle.
    //
    DECLSPEC_CACHEALIGN SLIST_HEADER FreeList;
} XDP_CPUMAP_DEEPCOPY_POOL;

_IRQL_requires_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
XdpCpuMapDeepCopyPoolInitialize(
    _Out_ XDP_CPUMAP_DEEPCOPY_POOL *Pool,
    _In_ NDIS_HANDLE NdisFilterHandle
    );

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapDeepCopyPoolCleanup(
    _Inout_ XDP_CPUMAP_DEEPCOPY_POOL *Pool
    );

//
// The flush batch. Section 7 "Batch enqueue": generic RX accumulates redirect
// decisions and the flush groups them by target so the ring lock is taken once
// per target per flush rather than once per packet (section 7.1).
//

//
// One accumulated redirect. FilterHandle, PortNumber and the owning receive
// queue are properties of the INDICATION, not of the packet, so they are held
// once on the group and stamped into every ring slot the flush writes rather
// than repeated per entry. Keeping them off the entry is what holds the batch
// -- which lives on the receive path's stack -- to a kilobyte.
//
typedef struct _XDP_CPUMAP_BATCH_ENTRY {
    NET_BUFFER_LIST *Nbl;

    //
    // The map the frame's backing reference was taken on, carried explicitly so
    // the release site names the same object as the acquire site rather than
    // rediscovering it through Target->OwnerMap.
    //
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_TARGET *Target;
} XDP_CPUMAP_BATCH_ENTRY;

typedef struct _XDP_CPUMAP_COMMIT_GROUP {
    EX_RUNDOWN_REF *NblRundown;
    ULONG Credits;      // acquired but not yet handed to a packet
    BOOLEAN RundownDown; // acquisition failed; the queue is running down

    //
    // TRUE when this group belongs to a TX-inspect queue, which is an
    // ASSERTED-IMPOSSIBLE state rather than a runtime one.
    //
    // CPUMAP is receive-side only. The generic LWF runs outbound sends through
    // the SAME post-inspection path as receives, and an XDP program may attach
    // to XDP_HOOK_L2/XDP_HOOK_TX/XDP_HOOK_INSPECT, so the exclusion has to be
    // real. It is made structurally, in XdpGenericRxActivateQueue: a TX-inspect
    // queue never gets CpuMapRedirectEnabled, so it never registers the frame
    // extension and no send NBL can reach the commit point.
    //
    // This field records which side of that boundary the group came from so the
    // boundary is checked where it matters rather than only where it is
    // established. It is not a policy knob and there is no counter: a violation
    // is a broken invariant, and checked builds assert on it exactly as they do
    // on malformed helper metadata below. Retail still falls through to Reject
    // and releases everything the metadata owns, so it fails closed rather than
    // injecting a send into the receive path.
    //
    BOOLEAN TxInspect;

    //
    // Indication identity, stamped into every ring slot this group produces.
    // The ring slot snapshots FilterHandle rather than re-deriving it from the
    // receive queue, so the drain path never dereferences generic state it does
    // not hold a reference on (section 6.3, "Why rundown, and why here").
    //
    NDIS_HANDLE FilterHandle;
    NDIS_PORT_NUMBER PortNumber;
    const VOID *RxQueueOwner;
    const VOID *GenericOwner;

    //
    // TRUE when this group's indication was low-resource, so every entry in it
    // is a DEEP COPY rather than an original (section 8.1a row 9b).
    //
    // It lives on the group rather than the entry because CanPend is a property
    // of the indication, constant for the whole post-inspection call the group
    // belongs to. It also makes the batch's shape provable: when this is TRUE
    // XdpGenericReceivePreInspectNbs admits exactly one NB per call, so the
    // group can only ever hold a single entry, which is why the per-entry
    // release the copy pre-pass performs on failure is bounded rather than a
    // per-packet interlocked cost.
    //
    BOOLEAN DeepCopy;

    //
    // The receiving queue's deep-copy cache, used only when DeepCopy is set.
    // NULL is legal and simply makes every low-resource redirect a counted
    // failure; the unit harness relies on that to exercise the allocation
    // failure row without a real NDIS pool.
    //
    XDP_CPUMAP_DEEPCOPY_POOL *DeepCopyPool;

    //
    // Originals CPUMAP committed but could not queue. Ownership was taken and
    // ActionNbl cleared, so the RX action switch can no longer deliver them and
    // the caller must return them to the miniport (section 6.3 step 6, section
    // 8.1a row 9a "Released -- post-commit failure").
    //
    // Deep copies NEVER appear here. The caller appends this list to DropList,
    // which returns its NBLs to the miniport, and the miniport never owned a
    // copy; a rejected copy is recycled into its own pool instead (row 9b,
    // "Released -- post-commit failure (ii)").
    //
    NET_BUFFER_LIST *RejectedNbls;

    UINT32 Count;
    XDP_CPUMAP_BATCH_ENTRY Entries[XDP_CPUMAP_BATCH_SIZE];
} XDP_CPUMAP_COMMIT_GROUP;

//
// N.B. Entries is deliberately left uninitialized: zeroing a kilobyte on every
// receive flush, almost all of which carry no CPUMAP traffic at all, is exactly
// the per-packet cost section 11 forbids. Count bounds every read of it. The
// caller does not initialize a group at all unless the receive queue has CPUMAP
// redirect enabled.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
XdpCpuMapCommitGroupInit(
    _Out_ XDP_CPUMAP_COMMIT_GROUP *Group,
    _In_ EX_RUNDOWN_REF *NblRundown,
    _In_opt_ NDIS_HANDLE FilterHandle,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_opt_ const VOID *RxQueueOwner,
    _In_opt_ const VOID *GenericOwner,
    _In_ BOOLEAN TxInspect,
    _In_ BOOLEAN DeepCopy,
    _In_opt_ XDP_CPUMAP_DEEPCOPY_POOL *DeepCopyPool
    )
{
    Group->NblRundown = NblRundown;
    Group->Credits = 0;
    Group->RundownDown = FALSE;
    Group->TxInspect = TxInspect;
    Group->FilterHandle = FilterHandle;
    Group->PortNumber = PortNumber;
    Group->RxQueueOwner = RxQueueOwner;
    Group->GenericOwner = GenericOwner;
    Group->DeepCopy = DeepCopy;
    Group->DeepCopyPool = DeepCopyPool;
    Group->RejectedNbls = NULL;
    Group->Count = 0;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
BOOLEAN
XdpCpuMapCommitGroupTakeCredit(
    _Inout_ XDP_CPUMAP_COMMIT_GROUP *Group
    )
{
    if (Group->Credits == 0) {
        if (Group->RundownDown) {
            return FALSE;
        }

        if (!ExAcquireRundownProtectionEx(
                Group->NblRundown, XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK)) {
            //
            // Latch the failure. Once the queue is running down it will not come
            // back within this group, so retrying per packet would reintroduce
            // exactly the per-packet interlocked cost this pool exists to avoid.
            //
            Group->RundownDown = TRUE;
            return FALSE;
        }

        Group->Credits = XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK;
    }

    Group->Credits--;
    return TRUE;
}

FORCEINLINE
VOID
XdpCpuMapCommitGroupReturnCredit(
    _Inout_ XDP_CPUMAP_COMMIT_GROUP *Group
    )
{
    Group->Credits++;
}

//
// Ends the flush group: flushes whatever the batch still holds (section 7) and
// then releases the credits no packet consumed, in one interlocked operation.
//
// RejectedNbls returns the originals CPUMAP committed and then could not queue.
// They are unconditionally dropped, not fallen back, so the caller returns them
// to the miniport rather than re-entering the RX action switch.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapCommitGroupFinish(
    _Inout_ XDP_CPUMAP_COMMIT_GROUP *Group,
    _Outptr_result_maybenull_ NET_BUFFER_LIST **RejectedNbls
    );

//
// What commit did with the caller's NBL.
//
// A BOOLEAN cannot express this: on the low-resource path ownership IS committed
// while the original stays with the caller, so "returned FALSE" covered both a
// successful deep-copy redirect and an outright rejection. The caller has to
// tell those apart -- one is a delivered packet whose original is on its way
// home, the other is a loss -- and reading the zeroed metadata could not do it,
// because every path here zeroes it.
//
typedef enum _XDP_CPUMAP_COMMIT_RESULT {
    //
    // Commit declined. The caller still owns the NBL and applies its RX action
    // normally, including whatever drop diagnostics that implies.
    //
    XdpCpuMapCommitDeclined = 0,

    //
    // Ownership transferred (section 8.1a row 9a). The caller must clear
    // ActionNbl: the original must not reach PassList, DropList or TxList.
    //
    XdpCpuMapCommitOwnershipTaken,

    //
    // A deep copy was built and queued; the caller keeps the ORIGINAL (row 9b).
    // The original still goes to DropList -- there is nothing else to do with it
    // -- but this is a SUCCESSFUL redirect, so it must not be logged as a
    // program-inspection drop.
    //
    XdpCpuMapCommitDeepCopied,
} XDP_CPUMAP_COMMIT_RESULT;

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
XDP_CPUMAP_COMMIT_RESULT
XdpCpuMapCommitRedirect(
    _Inout_ XDP_FRAME_CPUMAP_REDIRECT_V1 *Redirect,
    _In_opt_ NET_BUFFER_LIST *ActionNbl,
    _In_ BOOLEAN RxQueuePaused,
    _In_ BOOLEAN CanPend,
    _Inout_ XDP_CPUMAP_COMMIT_GROUP *Group
    );

_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN
XdpRxQueueIsCpuMapRedirectEnabled(
    _In_ XDP_RX_QUEUE_CONFIG_ACTIVATE RxQueueConfig
    );

//
// Quiesce.
//
// The generic LWF pause path calls these to make CPUMAP release every packet it
// holds on behalf of an interface or a single receive queue, before the pause
// path waits on that queue's NBL rundown.
//
// Both take OPAQUE IDENTITY TOKENS, not typed pointers. The design originally
// specified XdpCpuMapQuiesceRxQueue(XDP_LWF_GENERIC_RX_QUEUE *), which is not
// implementable: the include graph runs xdplwf -> xdp (src/xdplwf/xdplwf.vcxproj
// adds $(SolutionDir)src\xdp\inc), never the reverse, so xdp.sys cannot see an
// xdplwf-private type. CPUMAP only ever compares these tokens for equality
// against the values stamped into a ring entry at enqueue, so an opaque pointer
// is sufficient and keeps the dependency direction intact.
//
// Both run at PASSIVE_LEVEL with Generic->Lock held EXCLUSIVE by the caller.
// Neither acquires Generic->Lock or RxQueue->EcLock, and neither waits on the
// CPUMAP retire work queue; see the lock ordering in the design, section 8.3.
//
_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapQuiesceInterface(
    _In_ const VOID *GenericOwner
    );

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpCpuMapQuiesceRxQueue(
    _In_ const VOID *RxQueueOwner
    );

EXTERN_C_END
