//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// Public ABI for BPF_MAP_TYPE_CPUMAP, the XDP software-RSS CPU redirect map.
//
// A CPUMAP is an eBPF extensible map whose key is a policy-selected selector
// slot and whose value describes the target CPU that slot redirects to. User
// programs create and populate a CPUMAP with the standard libbpf map APIs; an
// XDP eBPF program then names a slot in bpf_redirect_map(), and XDP moves the
// packet to that slot's target CPU.
//
// The key is deliberately NOT a CPU number. It is a selector index chosen by
// the user program's policy, which lets several slots share one target CPU
// without the XDP engine knowing anything about the policy.
//

#pragma once

#include <xdp/wincommon.h>

EXTERN_C_START

#define XDP_CPUMAP_ENTRY_VERSION_1 1u
#define XDP_CPUMAP_ENTRY_SIZE_V1 32u

//
// Ring depth is the number of queued packets a single target CPU can hold. It
// is a map-level setting: the first entry written to a map establishes it, and
// every later entry must either repeat it or leave it zero.
//
#define XDP_CPUMAP_RING_DEPTH_DEFAULT 1024u
#define XDP_CPUMAP_RING_DEPTH_MIN 64u
#define XDP_CPUMAP_RING_DEPTH_MAX 16384u

//
// Drain batch size is the maximum number of packets a target CPU's DPC moves
// per iteration. Also map-level.
//
#define XDP_CPUMAP_DRAIN_BATCH_DEFAULT 32u
#define XDP_CPUMAP_DRAIN_BATCH_MIN 1u
#define XDP_CPUMAP_DRAIN_BATCH_MAX 128u

//
// Per-map limits, enforced at map create and at each update.
//
#define XDP_CPUMAP_MAX_ENTRIES 4096u
#define XDP_CPUMAP_MAX_TOTAL_RING_ENTRIES 262144u
#define XDP_CPUMAP_MAX_NONPAGED_BYTES (64u * 1024u * 1024u)

//
// The selector slot. Valid keys are 0 <= Key < max_entries.
//
typedef UINT32 XDP_CPUMAP_KEY;

//
// The map value. Size and version are validated strictly: a value whose Size or
// Version is not recognized is rejected rather than silently truncated, so an
// older driver fails closed against a newer user-mode layout.
//
typedef struct _XDP_CPUMAP_ENTRY_V1 {
    UINT16 Size;            // must be XDP_CPUMAP_ENTRY_SIZE_V1
    UINT16 Version;         // must be XDP_CPUMAP_ENTRY_VERSION_1
    UINT32 TargetCpu;       // absolute NT processor index, not group-relative
    UINT32 RingDepth;       // 0 => default; nonzero is map-level
    UINT32 DrainBatchSize;  // 0 => default; nonzero is map-level
    UINT32 Flags;           // must be 0 for v1
    UINT32 Reserved[3];     // must be 0 for v1
} XDP_CPUMAP_ENTRY_V1;

C_ASSERT(sizeof(XDP_CPUMAP_ENTRY_V1) == XDP_CPUMAP_ENTRY_SIZE_V1);

EXTERN_C_END
