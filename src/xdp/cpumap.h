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
#define XDP_CPUMAP_MAX_BATCH_SIZE 256

#define POOLTAG_CPUMAP 'PMUC' // 'CUPM'

//
// Ring entry: NBL + metadata for re-indication
//
typedef struct _XDP_CPUMAP_ENTRY {
    NET_BUFFER_LIST *Nbl;
    NDIS_HANDLE FilterHandle;
    NDIS_PORT_NUMBER PortNumber;
    BOOLEAN IsDeepCopy;  // TRUE = deep-copy NBL (!CanPend fallback), FALSE = original
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
    UINT32 DrainBatchSize; // Max NBLs to drain per DPC iteration

    // Statistics
    volatile LONG EnqueueCount;
    volatile LONG DrainCount;
    volatile LONG DropCount;

    // Diagnostic counters (POC profiling)
    volatile LONG RingFullCount;         // Enqueue saw ring full (subset of DropCount)
    volatile LONG DpcInvokeCount;        // Number of DPC firings
    volatile LONG DpcMaxBatchDrained;    // Largest batch in a single DPC
    volatile LONG DpcRequeueCount;       // DPC re-queued itself (more work)
    volatile LONG MaxRingDepth;          // High-water mark of (Tail - Head)
    volatile LONG EnqueueBatchCount;     // Number of FlushBatch calls that enqueued to this ring
    volatile LONG DpcLoopIterations;     // Total inner-loop iterations (batched drain-until-empty)
    volatile LONG DpcMaxLoopIterations;  // Max loop iterations in a single DPC invocation
    volatile LONG DpcEmptyCount;         // DPC fired but ring was already empty
    volatile LONG DpcYieldCount;         // DPC yielded via KeShouldYieldProcessor

    // Lock contention profiling
    volatile LONG64 LockWaitCycles;       // Total rdtsc cycles waiting on spinlock (producers + consumer)
    volatile LONG LockAcquireCount;       // Total lock acquisitions
    volatile LONG64 SourceCpuMask[2];     // Bitmask of source CPUs that enqueued (up to 128)

    struct _XDP_CPUMAP *OwnerMap;  // Back-pointer for map flags and stats in DrainDpc

    XDP_CPUMAP_ENTRY Entries[ANYSIZE_ARRAY];
} XDP_CPUMAP_RING;

//
// CPUMAP instance implementation (opaque to external callers).
//
struct _XDP_CPUMAP {
    UINT32 CpuBase;    // First target CPU (absolute processor index)
    UINT32 CpuCount;   // Number of target CPUs; rings are indexed [0, CpuCount)
    volatile BOOLEAN Active;

    //
    // !CanPend deep-copy fallback pool.
    // Lazy allocation: NBL structs allocated on first use, cached in SList.
    // No pre-allocated data buffers — NdisRetreatNetBufferDataStart allocates
    // pages on each use, NdisAdvanceNetBufferDataStart frees them on recycle.
    //
    NDIS_HANDLE DeepCopyNblPool;              // NdisAllocateNetBufferListPool (bare NBL+NB)
    DECLSPEC_CACHEALIGN
    SLIST_HEADER DeepCopyFreeList;             // Cross-CPU SList: DrainDpc pushes recycled NBLs
    volatile LONG DeepCopyAllocCount;          // Total NBLs ever allocated (grows on demand)
    volatile LONG DeepCopyHitCount;            // Stats: reused from free list
    volatile LONG DeepCopyMissCount;           // Stats: allocated new from pool
    volatile LONG DeepCopyFailCount;           // Stats: allocation or retreat failed
    volatile LONG DeepCopyIndicateCount;       // Stats: deep-copy NBLs indicated to tcpip

    // Miniport indication flags tracking
    volatile LONG MiniportResourcesCount;    // Miniport indicated WITH RESOURCES
    volatile LONG MiniportNoResourcesCount;  // Miniport indicated WITHOUT RESOURCES

    // Absolute zero-copy stats
    volatile LONG AbsoluteZeroCopyIndicateCount;  // Originals indicated directly (no copy)

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
