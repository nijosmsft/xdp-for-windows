//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// Shadowing precompiled header for the CPUMAP control-plane harness.
//
// src/xdp/cpumap.c includes "precomp.h". The project puts $(ProjectDir) ahead of
// $(SolutionDir)src\xdp on the include path, so this file is picked up instead
// of the driver's, exactly as test/pktfuzz does for programinspect.c. The source
// file under test is compiled UNMODIFIED -- there is no test-only #ifdef in
// cpumap.c, so the harness cannot drift from the shipping code.
//

#pragma once

#include <xdp/wincommon.h>
#include <winsock2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <xdp/rtl.h>

#include <stubs/ntos.h>
#include <stubs/ebpf.h>
#include <stubs/xdpworkqueue.h>

#include <xdp/ebpfhook.h>
#include <xdp/hookid.h>
#include <xdpifmode.h>

#define XDP_POOLTAG_CPUMAP 'mCdX'
#define XDP_POOLTAG_CPUMAP_DEEPCOPY 'dCdX'

//
// cpumap.c passes the driver object through to XdpCreateWorkQueue, which the
// harness stub ignores.
//
extern DRIVER_OBJECT *XdpDriverObject;

//
// ASSERT in the driver is XDP's own; here it must fail the harness rather than
// break into a debugger that is not attached.
//
// Some tests deliberately drive paths whose checked-build assertions are the
// behaviour under test — a poisoned frame with invalid metadata, for example,
// must assert in checked builds AND still clean up safely in retail. Those
// tests set XdpCpuMapTestExpectAssert to count the assertion instead of failing
// on it, so the retail cleanup path stays reachable in the harness.
//
extern ULONG XdpCpuMapTestExpectAssert;
extern ULONG XdpCpuMapTestAssertsObserved;

#define XDPCPUMAP_DRIVER_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            if (XdpCpuMapTestExpectAssert) { \
                XdpCpuMapTestAssertsObserved++; \
            } else { \
                XDPCPUMAP_TEST_ASSERT(expr); \
            } \
        } \
        _Analysis_assume_(expr); \
    } while (0)

#define ASSERT(expr) XDPCPUMAP_DRIVER_ASSERT(expr)
#define FRE_ASSERT(expr) XDPCPUMAP_DRIVER_ASSERT(expr)

//
// XDP_EBPF_MAP_HEADER, copied rather than included: src/xdp/ebpfmap.h pulls the
// eBPF SDK headers, and the harness deliberately does not depend on the pinned
// package. Only the shape matters here, and it is asserted below.
//
typedef enum _XDP_EBPF_MAP_TYPE {
    XdpEbpfMapTypeXsk = 1,
    XdpEbpfMapTypeCpuMap = 2,
} XDP_EBPF_MAP_TYPE;

typedef struct _XDP_EBPF_MAP_HEADER {
    XDP_EBPF_MAP_TYPE Type;
} XDP_EBPF_MAP_HEADER;

#ifndef MAP_CONTEXT
#define MAP_CONTEXT(Map, Offset) ((UCHAR *)(Map) + (Offset))
#endif

#include <xdpcpumap.h>
#include "cpumap.h"
#include "ebpfcpumap.h"

//
// Harness-only entry points implemented in cpumaptest.c and the stubs.
//

extern ebpf_base_map_client_dispatch_table_t XdpCpuMapTestClientDispatch;
