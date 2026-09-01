//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// Software RSS via CPUMAP. This program redirects every inspected frame into a
// BPF_MAP_TYPE_CPUMAP, which moves the packet to the target CPU named by the
// map entry and re-indicates it there.
//
// The selector key is a CONSTANT, not a hash of the packet and not
// ctx->rx_queue_index. That is deliberate: it makes the target CPU a property of
// the map alone, so a test can prove the packet moved because the map said so
// rather than because the arrival hash happened to land there. A real
// software-RSS program would compute a policy key here; the CPU-selection policy
// living in the BPF program rather than in the driver is the whole point of the
// design.
//
// If the redirect cannot be honoured -- no CPUMAP entry at the key, a
// non-generic interface, the map running down -- the helper returns the fallback
// action encoded in the third argument and the packet is delivered normally.
// XDP_PASS is chosen so that a failed redirect is visible as "packet arrived on
// the wrong CPU" rather than as "packet disappeared", which is what makes the
// functional test's CPU assertion meaningful in both directions.
//

#include "bpf_endian.h"
#include "bpf_helpers.h"
#include "xdp/ebpfhook.h"

//
// BPF_MAP_TYPE_CPUMAP is an enumerator of ebpf_map_type_t, so no #ifndef in C
// can detect whether the eBPF package declares it. The package version is the
// only thing the build can test, so this guard is keyed to the exact legacy
// version that lacks the type and is defined by the same
// XdpEbpfVersion == 1.4.0 condition that governs the driver's copy in
// src/xdp/ebpfmap.h. Both stop applying the moment XdpEbpfVersion advances.
//
// There is deliberately ONE story about this value: driver and BPF program use
// the same macro name, set by the same build condition, and both unwind
// together. See src/xdp/xdp.vcxproj and test/bpf/bpf.vcxproj.
//
#if defined(XDP_EBPF_LEGACY_NO_CPUMAP_TYPE)
#define BPF_MAP_TYPE_CPUMAP 17
#endif

//
// The CPUMAP. User mode populates it with XDP_CPUMAP_ENTRY_V1 values
// (published/external/xdp/cpumap.h): key is a selector slot chosen by policy,
// value names the absolute target processor index.
//
struct
{
    __uint(type, BPF_MAP_TYPE_CPUMAP);
    __type(key, uint32_t);
    __type(value, uint32_t[8]);
    __uint(max_entries, 64);
} cpu_map SEC(".maps");

SEC("xdp/cpumap_redirect")
int
cpumap_redirect(xdp_md_t *ctx)
{
    (void)ctx;

    return bpf_redirect_map(&cpu_map, 0, XDP_PASS);
}
