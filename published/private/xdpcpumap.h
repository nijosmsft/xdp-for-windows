//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

#pragma once

//
// CPU redirect frame extension definition (public API).
//
typedef struct _XDP_FRAME_CPU_REDIRECT {
    UINT32 TargetCpu;
    UINT32 CpuBase;        // First CPU in the redirect range
    UINT32 CpuCount;       // Number of CPUs in the redirect range
    UINT32 RingDepth;      // Per-CPU ring capacity (0 = default)
    UINT32 DrainBatchSize; // DPC drain batch limit (0 = default)
} XDP_FRAME_CPU_REDIRECT;

#define XDP_FRAME_EXTENSION_CPU_REDIRECT_NAME L"ms_frame_cpu_redirect"
#define XDP_FRAME_EXTENSION_CPU_REDIRECT_VERSION_1 1U

//
// Opaque CPUMAP handle for xdplwf.
//
typedef struct _XDP_CPUMAP XDP_CPUMAP;

#define XDP_CPUMAP_RING_DEFAULT_CAPACITY 32768

//
// CPU redirect extension inline accessor.
//
inline
XDP_FRAME_CPU_REDIRECT *
XdpGetCpuRedirectExtension(
    _In_ XDP_FRAME *Frame,
    _In_ XDP_EXTENSION *Extension
    )
{
    return (XDP_FRAME_CPU_REDIRECT *)XdpGetExtensionData(Frame, Extension);
}

#ifdef _KERNEL_MODE

//
// Zero-copy indicate toggle.  When enabled, CPUMAP indicates NBLs
// without NDIS_RECEIVE_FLAGS_RESOURCES, avoiding a copy in tcpip.
// Requires XDP_CPUMAP_PREALLOC=1.
//
#ifndef XDP_CPUMAP_ZERO_COPY_INDICATE
#define XDP_CPUMAP_ZERO_COPY_INDICATE 1
#endif

//
// CPUMAP lifecycle APIs (callable from xdplwf).
// Kernel-mode only.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
XdpCpuMapCreate(
    _In_ UINT32 CpuBase,
    _In_ UINT32 CpuCount,
    _In_ UINT32 RingCapacity,
    _In_ UINT32 DrainBatchSize,
    _In_ NDIS_HANDLE NdisHandle,
    _Out_ XDP_CPUMAP **CpuMap
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
XdpCpuMapDestroy(
    _In_ XDP_CPUMAP *CpuMap
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
XdpCpuMapEnqueue(
    _In_ XDP_CPUMAP *CpuMap,
    _In_ UINT32 TargetCpu,
    _In_ NET_BUFFER_LIST *Nbl,
    _In_ NDIS_HANDLE FilterHandle,
    _In_ NDIS_PORT_NUMBER PortNumber
    );

//
// Batch enqueue API: collect redirect decisions, then flush in one pass.
// Reduces lock acquisitions from N (per-packet) to T (per-target-CPU).
//

#define XDP_CPUMAP_MAX_BATCH_ENTRIES 32

typedef struct _XDP_CPUMAP_BATCH_ENTRY {
    NET_BUFFER_LIST *OriginalNbl;
    UINT32 TargetCpu;
    NDIS_HANDLE FilterHandle;
    NDIS_PORT_NUMBER PortNumber;
} XDP_CPUMAP_BATCH_ENTRY;

typedef struct _XDP_CPUMAP_BATCH {
    UINT32 Count;
    XDP_CPUMAP_BATCH_ENTRY Entries[XDP_CPUMAP_MAX_BATCH_ENTRIES];
} XDP_CPUMAP_BATCH;

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapBatchInit(
    _Out_ XDP_CPUMAP_BATCH *Batch
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
XdpCpuMapBatchAdd(
    _Inout_ XDP_CPUMAP_BATCH *Batch,
    _In_ NET_BUFFER_LIST *Nbl,
    _In_ UINT32 TargetCpu,
    _In_ NDIS_HANDLE FilterHandle,
    _In_ NDIS_PORT_NUMBER PortNumber
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapFlushBatch(
    _In_ XDP_CPUMAP *CpuMap,
    _Inout_ XDP_CPUMAP_BATCH *Batch
    );

//
// Track whether the miniport indicates with or without RESOURCES.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapTrackMiniportIndication(
    _In_ XDP_CPUMAP *CpuMap,
    _In_ BOOLEAN IsResources
    );

#if XDP_CPUMAP_ZERO_COPY_INDICATE
//
// Return path for zero-copy indicated NBLs.  Called from the LWF
// ReturnNetBufferLists handler.  Walks the chain, recycles CPUMAP
// shells/clones, and passes through any non-CPUMAP NBLs via the
// returned PassThrough chain.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
NET_BUFFER_LIST *
XdpCpuMapReturnShells(
    _In_ NET_BUFFER_LIST *NetBufferLists
    );
#endif // XDP_CPUMAP_ZERO_COPY_INDICATE

#endif // _KERNEL_MODE
