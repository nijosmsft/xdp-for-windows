//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

#pragma once

#include <ndis.h>
#include <xdpcpumap.h>

EXTERN_C_START

//
// Internal implementation details (not exported).
//
#define XDP_CPUMAP_MAX_BATCH_SIZE 64
#define POOLTAG_CPUMAP 'PMUC' // 'CUPM'

//
// Ring entry: NBL + metadata for re-indication
//
typedef struct _XDP_CPUMAP_ENTRY {
    NET_BUFFER_LIST *Nbl;
    NDIS_HANDLE FilterHandle;
    NDIS_PORT_NUMBER PortNumber;
} XDP_CPUMAP_ENTRY;

//
// Per-CPU ring buffer
//
typedef struct DECLSPEC_CACHEALIGN _XDP_CPUMAP_RING {
    KSPIN_LOCK Lock;
    UINT32 Head;           // Consumer index
    UINT32 Tail;           // Producer index
    UINT32 Capacity;       // Power of 2
    UINT32 Mask;           // Capacity - 1

    // Statistics
    volatile LONG EnqueueCount;
    volatile LONG DrainCount;
    volatile LONG DropCount;

    XDP_CPUMAP_ENTRY Entries[ANYSIZE_ARRAY];
} XDP_CPUMAP_RING;

//
// CPUMAP instance implementation (opaque to external callers).
//
struct _XDP_CPUMAP {
    UINT32 CpuCount;
    volatile BOOLEAN Active;

    //
    // Per-source-CPU NBL clone pools. Indexed by KeGetCurrentProcessorIndex().
    // Each RSS CPU allocates clones from its own pool; NDIS auto-routes frees
    // back to the originating pool regardless of which CPU calls
    // NdisFreeCloneNetBufferList.
    //
    UINT32 ClonePoolCount;
    NDIS_HANDLE *PerCpuClonePools;

    XDP_CPUMAP_RING **PerCpuRings;
    KDPC *PerCpuDpcs;

    volatile LONG RefCount;
};

_Function_class_(KDEFERRED_ROUTINE)
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
VOID
XdpCpuMapDrainDpc(
    _In_ KDPC *Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
    );

EXTERN_C_END
