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
// When XDP_CPUMAP_DRAIN_ALL is 1, the drain DPC dequeues the entire ring
// in one pass (no batch limit).  Shells are recycled inline after NDIS
// indication rather than via a fixed-size stack array.
// When 0, the legacy batched path (XDP_CPUMAP_MAX_BATCH_SIZE cap) is used.
//
#ifndef XDP_CPUMAP_DRAIN_ALL
#define XDP_CPUMAP_DRAIN_ALL 0
#endif

//
// Pre-allocated buffer pool hack: eliminate clone alloc/free from the
// hot path.  Enable by defining XDP_CPUMAP_PREALLOC=1 at build time
// (or toggle the default below).  Each per-CPU ring pre-allocates
// RingCapacity shells containing NBL + NB + MDL + 2048-byte buffer.
// Enqueue memcpy's packet data into a shell (no NDIS pool alloc);
// drain recycles the shell (no NDIS pool free).
//
#ifndef XDP_CPUMAP_PREALLOC
#define XDP_CPUMAP_PREALLOC 1
#endif

//
// Zero-copy indicate: when enabled, CPUMAP indicates shell NBLs to NDIS
// without NDIS_RECEIVE_FLAGS_RESOURCES, letting tcpip process the data
// in-place (no tcpip copy). Shells are recycled asynchronously when
// tcpip returns them via FilterReturnNetBufferLists.
// Requires XDP_CPUMAP_PREALLOC=1.
//
#ifndef XDP_CPUMAP_ZERO_COPY_INDICATE
#define XDP_CPUMAP_ZERO_COPY_INDICATE 0
#endif

#if XDP_CPUMAP_ZERO_COPY_INDICATE
//
// Magic tags stamped in Nbl->MiniportReserved[1] before indication.
// MiniportReserved is originator-owned; we allocated the NBL so it's ours.
//
#define XDP_CPUMAP_SHELL_MAGIC  ((PVOID)(ULONG_PTR)0x584D4150)  // 'XMAP'
#define XDP_CPUMAP_CLONE_MAGIC  ((PVOID)(ULONG_PTR)0x584D4151)  // 'XMAQ'
#endif

#if XDP_CPUMAP_PREALLOC
#define XDP_CPUMAP_PREALLOC_BUFFER_SIZE 2048

typedef struct DECLSPEC_CACHEALIGN _XDP_CPUMAP_PREALLOC_SHELL {
    SLIST_ENTRY SListEntry;
    NET_BUFFER_LIST *Nbl;
    MDL *Mdl;
#if XDP_CPUMAP_ZERO_COPY_INDICATE
    PSLIST_HEADER OwnerFreeList;            // Points to Ring->PreallocFreeList for async recycle
    struct _XDP_CPUMAP *OwnerMap;           // Back-pointer for OutstandingIndications decrement
#endif
    UCHAR DataBuffer[XDP_CPUMAP_PREALLOC_BUFFER_SIZE];
} XDP_CPUMAP_PREALLOC_SHELL;
#endif

//
// Ring entry: NBL + metadata for re-indication
//
typedef struct _XDP_CPUMAP_ENTRY {
    NET_BUFFER_LIST *Nbl;
    NDIS_HANDLE FilterHandle;
    NDIS_PORT_NUMBER PortNumber;
    BOOLEAN IsDeepCopy;  // TRUE = deep-copy NBL (AZC !CanPend fallback), FALSE = original or shell
#if XDP_CPUMAP_PREALLOC
    struct _XDP_CPUMAP_PREALLOC_SHELL *Shell;
#endif
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
    volatile LONG CloneFailCount;        // NdisAllocateCloneNetBufferList returned NULL
    volatile LONG PreallocHitCount;      // Pre-alloc shell used successfully
    volatile LONG PreallocMissCount;     // Pre-alloc SLIST empty, fell back to clone
    volatile LONG PreallocOversizeCount; // Packet > PREALLOC_BUFFER_SIZE
    volatile LONG DpcInvokeCount;        // Number of DPC firings
    volatile LONG DpcMaxBatchDrained;    // Largest batch in a single DPC
    volatile LONG DpcRequeueCount;       // DPC re-queued itself (more work)
    volatile LONG MaxRingDepth;          // High-water mark of (Tail - Head)
    volatile LONG EnqueueBatchCount;     // Number of FlushBatch calls that enqueued to this ring
    volatile LONG DpcLoopIterations;     // Total inner-loop iterations (batched drain-until-empty)
    volatile LONG DpcMaxLoopIterations;  // Max loop iterations in a single DPC invocation
    volatile LONG DpcEmptyCount;         // DPC fired but ring was already empty

    // Lock contention profiling
    volatile LONG64 LockWaitCycles;       // Total rdtsc cycles waiting on spinlock (producers + consumer)
    volatile LONG LockAcquireCount;       // Total lock acquisitions
    volatile LONG64 SourceCpuMask[2];     // Bitmask of source CPUs that enqueued (up to 128)

#if XDP_CPUMAP_PREALLOC
    DECLSPEC_CACHEALIGN SLIST_HEADER PreallocFreeList;
    XDP_CPUMAP_PREALLOC_SHELL *PreallocShellBlock;  // Single contiguous allocation for all shells
#endif

#if XDP_CPUMAP_ZERO_COPY_INDICATE
    struct _XDP_CPUMAP *OwnerMap;  // Back-pointer for OutstandingIndications in DrainDpc
#endif

    XDP_CPUMAP_ENTRY Entries[ANYSIZE_ARRAY];
} XDP_CPUMAP_RING;

//
// CPUMAP instance implementation (opaque to external callers).
//
struct _XDP_CPUMAP {
    UINT32 CpuBase;    // First target CPU (absolute processor index)
    UINT32 CpuCount;   // Number of target CPUs; rings are indexed [0, CpuCount)
    UINT32 Flags;      // XDP_CPUMAP_FLAG_* runtime behavior flags
    volatile BOOLEAN Active;

    //
    // Per-source-CPU NBL clone pools. Indexed by KeGetCurrentProcessorIndex().
    // Each RSS CPU allocates clones from its own pool; NDIS auto-routes frees
    // back to the originating pool regardless of which CPU calls
    // NdisFreeCloneNetBufferList.
    //
    UINT32 ClonePoolCount;
    NDIS_HANDLE *PerCpuClonePools;

#if XDP_CPUMAP_PREALLOC
    NDIS_HANDLE PreallocNblPool;
    volatile LONG64 CopyTotalCycles;      // Sum of rdtsc cycles spent in RtlCopyMemory
    volatile LONG CopyCount;              // Number of RtlCopyMemory calls
    volatile LONG64 CopyTotalBytes;       // Total bytes copied
#endif

#if XDP_CPUMAP_ZERO_COPY_INDICATE
    volatile LONG OutstandingIndications;  // NBLs indicated but not yet returned (shell + clone)
    KEVENT AllReturnedEvent;               // Signaled when OutstandingIndications reaches 0
    volatile LONG ZeroCopyIndicateCount;   // Total NBLs indicated without RESOURCES flag
    volatile LONG ShellReturnCount;        // Shells recycled via async return path
    volatile LONG CloneReturnCount;        // Clones freed via async return path
#endif

    //
    // AZC !CanPend deep-copy fallback pool.
    // Lazy allocation: NBL structs allocated on first use, cached in SList.
    // No pre-allocated data buffers — NdisRetreatNetBufferDataStart allocates
    // pages on each use, NdisAdvanceNetBufferDataStart frees them on recycle.
    //
    NDIS_HANDLE DeepCopyNblPool;              // NdisAllocateNetBufferListPool (bare NBL+NB)
    DECLSPEC_CACHEALIGN
    SLIST_HEADER DeepCopyFreeList;             // Cross-CPU SList: DrainDpc pushes recycled NBLs
    volatile LONG DeepCopyAllocCount;          // Total NBLs ever allocated (monotonic up to limit)
    LONG DeepCopyAllocLimit;                   // Cap on total allocated NBLs
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
