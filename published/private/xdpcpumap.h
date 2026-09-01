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
// inserts into the batch simply keeps its credit instead of returning it.
//
#define XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK 32u

typedef struct _XDP_CPUMAP_COMMIT_GROUP {
    EX_RUNDOWN_REF *NblRundown;
    ULONG Credits;      // acquired but not yet handed to a packet
    BOOLEAN RundownDown; // acquisition failed; the queue is running down
} XDP_CPUMAP_COMMIT_GROUP;

FORCEINLINE
VOID
XdpCpuMapCommitGroupInit(
    _Out_ XDP_CPUMAP_COMMIT_GROUP *Group,
    _In_ EX_RUNDOWN_REF *NblRundown
    )
{
    Group->NblRundown = NblRundown;
    Group->Credits = 0;
    Group->RundownDown = FALSE;
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

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
XdpCpuMapCommitGroupFinish(
    _Inout_ XDP_CPUMAP_COMMIT_GROUP *Group
    )
{
    if (Group->Credits > 0) {
        ExReleaseRundownProtectionEx(Group->NblRundown, Group->Credits);
        Group->Credits = 0;
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
BOOLEAN
XdpCpuMapCommitRedirect(
    _Inout_ XDP_FRAME_CPUMAP_REDIRECT_V1 *Redirect,
    _In_opt_ const NET_BUFFER_LIST *ActionNbl,
    _In_ BOOLEAN RxQueuePaused,
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
