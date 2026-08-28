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
// everything here is an internal call, not a shipped ABI. There are no version
// or size fields and no opaque reference handles: those would be ceremony for a
// direct function call.
//

#pragma once

#include <xdp/cpumap.h>

EXTERN_C_START

//
// Internal-only limits. Not part of the public CPUMAP ABI; these may change
// without a version bump.
//
#define XDP_CPUMAP_GLOBAL_MAX_RING_ENTRIES 1048576u
#define XDP_CPUMAP_GLOBAL_MAX_NONPAGED_BYTES (256u * 1024u * 1024u)
#define XDP_CPUMAP_MAX_LIVE_MAPS 64u
#define XDP_CPUMAP_MAX_TARGETS_PER_MAP 1024u
#define XDP_CPUMAP_QUIESCE_MAX_PASSES 8u

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
