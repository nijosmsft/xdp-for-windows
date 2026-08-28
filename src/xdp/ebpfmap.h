//
// Copyright (c) Microsoft Corporation.
//

#pragma once

//
// Common definitions shared by all eBPF extensible map providers implemented by
// XDP.
//
// The eBPF runtime hands raw map pointers to helpers such as bpf_redirect_map
// without any compile-time type information. Every XDP map context therefore
// begins with an XDP_EBPF_MAP_HEADER so that a raw map pointer can be resolved
// to its concrete map type and validated before the context is dereferenced.
//

//
// BPF_MAP_TYPE_CPUMAP compatibility guard.
//
// The CPUMAP map type is added by nijosmsft/ebpf-for-windows#3 and is not yet in
// a released eBPF-for-Windows NuGet package. src/xdp.props pins
// XdpEbpfVersion 1.4.0, whose ebpf_structs.h stops at BPF_MAP_TYPE_XSKMAP = 16.
//
// This CANNOT be detected with #ifndef. BPF_MAP_TYPE_CPUMAP is an ENUMERATOR of
// ebpf_map_type_t, not a macro, so the preprocessor cannot see it whether or not
// the SDK declares it -- an #ifndef guard would always fire, and any C_ASSERT
// beneath it would only be testing the local definition against itself.
//
// The package version is the only thing the build can actually test, so the
// guard is keyed to it in src/xdp/xdp.vcxproj and is scoped to the exact legacy
// version. It stops applying the moment XdpEbpfVersion advances, at which point
// the SDK enumerator is used and the C_ASSERTs below are compiled and are then
// genuinely testing the SDK.
//
// N.B. In the 1.4.0 SDK, BPF_MAP_TYPE_MAX happens to equal 17 because XSKMAP is
// the last type. That is a coincidence of the guard window, not a collision:
// XDP only ever compares against and passes through the value, and the eBPF
// change moves BPF_MAP_TYPE_MAX to 18.
//
#if defined(XDP_EBPF_LEGACY_NO_CPUMAP_TYPE)

#define BPF_MAP_TYPE_CPUMAP ((ebpf_map_type_t)17)

#else

C_ASSERT((UINT32)BPF_MAP_TYPE_CPUMAP == 17);
C_ASSERT((UINT32)BPF_MAP_TYPE_CPUMAP != (UINT32)BPF_MAP_TYPE_XSKMAP);

#endif

typedef enum _XDP_EBPF_MAP_TYPE {
    //
    // BPF_MAP_TYPE_XSKMAP: AF_XDP socket redirect map. See ebpfxskmap.c.
    //
    XdpEbpfMapTypeXsk = 1,

    //
    // BPF_MAP_TYPE_CPUMAP: software-RSS CPU redirect map. See ebpfcpumap.c.
    //
    XdpEbpfMapTypeCpuMap = 2,
} XDP_EBPF_MAP_TYPE;

typedef struct _XDP_EBPF_MAP_HEADER {
    XDP_EBPF_MAP_TYPE Type;
} XDP_EBPF_MAP_HEADER;
