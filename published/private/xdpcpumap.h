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
} XDP_FRAME_CPU_REDIRECT;

#define XDP_FRAME_EXTENSION_CPU_REDIRECT_NAME L"ms_frame_cpu_redirect"
#define XDP_FRAME_EXTENSION_CPU_REDIRECT_VERSION_1 1U

//
// Opaque CPUMAP handle for xdplwf.
//
typedef struct _XDP_CPUMAP XDP_CPUMAP;

#define XDP_CPUMAP_RING_DEFAULT_CAPACITY 256

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
// CPUMAP lifecycle APIs (callable from xdplwf).
// Kernel-mode only.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
XdpCpuMapCreate(
    _In_ UINT32 CpuCount,
    _In_ UINT32 RingCapacity,
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

#endif // _KERNEL_MODE
