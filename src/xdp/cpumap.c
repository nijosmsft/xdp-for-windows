//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

#include "precomp.h"
#include "cpumap.h"

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
XdpCpuMapCreate(
    _In_ UINT32 CpuCount,
    _In_ UINT32 RingCapacity,
    _In_ NDIS_HANDLE NdisHandle,
    _Out_ XDP_CPUMAP **CpuMap
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    XDP_CPUMAP *Map = NULL;
    SIZE_T RingSize;
    NET_BUFFER_LIST_POOL_PARAMETERS PoolParams;

    *CpuMap = NULL;

    //
    // Validate ring capacity is power of 2
    //
    if (RingCapacity == 0 || (RingCapacity & (RingCapacity - 1)) != 0) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    //
    // Allocate CPUMAP structure
    //
    Map = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Map), POOLTAG_CPUMAP);
    if (Map == NULL) {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    Map->CpuCount = CpuCount;
    Map->Active = TRUE;
    Map->RefCount = 1;

    //
    // Allocate per-CPU ring array
    //
    Map->PerCpuRings = ExAllocatePoolZero(
        NonPagedPoolNx,
        sizeof(XDP_CPUMAP_RING *) * CpuCount,
        POOLTAG_CPUMAP);
    if (Map->PerCpuRings == NULL) {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    //
    // Allocate per-CPU DPC array
    //
    Map->PerCpuDpcs = ExAllocatePoolZero(
        NonPagedPoolNx,
        sizeof(KDPC) * CpuCount,
        POOLTAG_CPUMAP);
    if (Map->PerCpuDpcs == NULL) {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    //
    // Allocate per-source-CPU NBL clone pools.
    // Each source CPU allocates clones from its own pool, eliminating
    // contention on the pool-global spinlock at high CPU counts.
    // NdisFreeCloneNetBufferList auto-routes frees to the originating pool.
    //
    Map->ClonePoolCount = 0;
    Map->PerCpuClonePools = ExAllocatePoolZero(
        NonPagedPoolNx,
        sizeof(NDIS_HANDLE) * CpuCount,
        POOLTAG_CPUMAP);
    if (Map->PerCpuClonePools == NULL) {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    RtlZeroMemory(&PoolParams, sizeof(PoolParams));
    PoolParams.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    PoolParams.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    PoolParams.Header.Size = sizeof(PoolParams);
    PoolParams.PoolTag = POOLTAG_CPUMAP;
    PoolParams.fAllocateNetBuffer = TRUE;
    PoolParams.ContextSize = 0;

    for (UINT32 i = 0; i < CpuCount; i++) {
        Map->PerCpuClonePools[i] = NdisAllocateNetBufferListPool(NdisHandle, &PoolParams);
        if (Map->PerCpuClonePools[i] == NULL) {
            Status = STATUS_NO_MEMORY;
            goto Exit;
        }
        Map->ClonePoolCount++;
    }

    //
    // Allocate and initialize per-CPU rings and DPCs
    //
    RingSize = sizeof(XDP_CPUMAP_RING) + (sizeof(XDP_CPUMAP_ENTRY) * RingCapacity);

    for (UINT32 i = 0; i < CpuCount; i++) {
        XDP_CPUMAP_RING *Ring;
        PROCESSOR_NUMBER ProcNumber;

        //
        // Allocate ring
        //
        Ring = ExAllocatePoolZero(NonPagedPoolNx, RingSize, POOLTAG_CPUMAP);
        if (Ring == NULL) {
            Status = STATUS_NO_MEMORY;
            goto Exit;
        }

        KeInitializeSpinLock(&Ring->Lock);
        Ring->Head = 0;
        Ring->Tail = 0;
        Ring->Capacity = RingCapacity;
        Ring->Mask = RingCapacity - 1;
        Ring->EnqueueCount = 0;
        Ring->DrainCount = 0;
        Ring->DropCount = 0;

#pragma prefast(suppress:6386, "Buffer size is sizeof(XDP_CPUMAP_RING *) * CpuCount, indexed by i < CpuCount.")
        Map->PerCpuRings[i] = Ring;

        //
        // Initialize DPC with target CPU affinity
        //
        KeInitializeDpc(&Map->PerCpuDpcs[i], XdpCpuMapDrainDpc, Ring);

        Status = KeGetProcessorNumberFromIndex(i, &ProcNumber);
        if (!NT_SUCCESS(Status)) {
            goto Exit;
        }

        KeSetTargetProcessorDpcEx(&Map->PerCpuDpcs[i], &ProcNumber);
    }

    *CpuMap = Map;
    Status = STATUS_SUCCESS;

Exit:
    if (!NT_SUCCESS(Status)) {
        if (Map != NULL) {
            XdpCpuMapDestroy(Map);
        }
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
XdpCpuMapDestroy(
    _In_ XDP_CPUMAP *CpuMap
    )
{
    if (CpuMap == NULL) {
        return;
    }

    //
    // Mark inactive to prevent new enqueues
    //
    CpuMap->Active = FALSE;
    MemoryBarrier();

    //
    // Flush all DPCs to ensure no more are executing
    //
    KeFlushQueuedDpcs();

    //
    // Free per-CPU rings
    //
    if (CpuMap->PerCpuRings != NULL) {
        for (UINT32 i = 0; i < CpuMap->CpuCount; i++) {
            XDP_CPUMAP_RING *Ring = CpuMap->PerCpuRings[i];
            if (Ring != NULL) {
                //
                // Drain any remaining NBLs
                //
                while (Ring->Head != Ring->Tail) {
                    XDP_CPUMAP_ENTRY *Entry = &Ring->Entries[Ring->Head & Ring->Mask];
                    if (Entry->Nbl != NULL) {
                        NdisFreeCloneNetBufferList(Entry->Nbl, 0);
                    }
                    Ring->Head++;
                }

                ExFreePoolWithTag(Ring, POOLTAG_CPUMAP);
            }
        }
        ExFreePoolWithTag(CpuMap->PerCpuRings, POOLTAG_CPUMAP);
    }

    //
    // Free DPC array
    //
    if (CpuMap->PerCpuDpcs != NULL) {
        ExFreePoolWithTag(CpuMap->PerCpuDpcs, POOLTAG_CPUMAP);
    }

    //
    // Free per-CPU NBL clone pools
    //
    if (CpuMap->PerCpuClonePools != NULL) {
        for (UINT32 i = 0; i < CpuMap->ClonePoolCount; i++) {
#pragma prefast(suppress:6001, "Pool handles are initialized in XdpCpuMapCreate.")
            if (CpuMap->PerCpuClonePools[i] != NULL) {
                NdisFreeNetBufferListPool(CpuMap->PerCpuClonePools[i]);
            }
        }
        ExFreePoolWithTag(CpuMap->PerCpuClonePools, POOLTAG_CPUMAP);
    }

    //
    // Free CPUMAP structure
    //
    ExFreePoolWithTag(CpuMap, POOLTAG_CPUMAP);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
XdpCpuMapEnqueue(
    _In_ XDP_CPUMAP *CpuMap,
    _In_ UINT32 TargetCpu,
    _In_ NET_BUFFER_LIST *Nbl,
    _In_ NDIS_HANDLE FilterHandle,
    _In_ NDIS_PORT_NUMBER PortNumber
    )
{
    KLOCK_QUEUE_HANDLE LockHandle;
    XDP_CPUMAP_RING *Ring;
    XDP_CPUMAP_ENTRY *Entry;
    UINT32 NextTail;
    NET_BUFFER_LIST *Clone;

    //
    // Validate inputs
    //
    if (!CpuMap->Active || TargetCpu >= CpuMap->CpuCount) {
        return STATUS_INVALID_PARAMETER;
    }

    //
    // Clone the NBL using the source CPU's dedicated pool.
    //
    UINT32 SourceCpu = KeGetCurrentProcessorIndex();
    NDIS_HANDLE ClonePool;

    if (SourceCpu < CpuMap->ClonePoolCount) {
        ClonePool = CpuMap->PerCpuClonePools[SourceCpu];
    } else {
        //
        // Guard against CPU hot-add: fall back to pool 0.
        //
        ClonePool = CpuMap->PerCpuClonePools[0];
    }

    Clone = NdisAllocateCloneNetBufferList(
        Nbl,
        ClonePool,
        NULL,
        0);

    if (Clone == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Preserve RSS hash and metadata.
    //
    NET_BUFFER_LIST_SET_HASH_VALUE(Clone,
        NET_BUFFER_LIST_GET_HASH_VALUE(Nbl));
    NET_BUFFER_LIST_SET_HASH_TYPE(Clone,
        NET_BUFFER_LIST_GET_HASH_TYPE(Nbl));

    Ring = CpuMap->PerCpuRings[TargetCpu];

    //
    // Acquire spinlock and enqueue
    //
    KeAcquireInStackQueuedSpinLock(&Ring->Lock, &LockHandle);

    NextTail = Ring->Tail + 1;
    if ((NextTail - Ring->Head) > Ring->Capacity) {
        //
        // Ring is full, drop packet and free clone.
        //
        InterlockedIncrement(&Ring->DropCount);
        KeReleaseInStackQueuedSpinLock(&LockHandle);
        NdisFreeCloneNetBufferList(Clone, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Enqueue entry (cloned NBL).
    //
    Entry = &Ring->Entries[Ring->Tail & Ring->Mask];
    Entry->Nbl = Clone;
    Entry->FilterHandle = FilterHandle;
    Entry->PortNumber = PortNumber;

    Ring->Tail = NextTail;
    InterlockedIncrement(&Ring->EnqueueCount);

    KeReleaseInStackQueuedSpinLock(&LockHandle);

    //
    // Schedule DPC on target CPU
    //
    KeInsertQueueDpc(&CpuMap->PerCpuDpcs[TargetCpu], NULL, NULL);

    return STATUS_SUCCESS;
}

_Function_class_(KDEFERRED_ROUTINE)
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
VOID
XdpCpuMapDrainDpc(
    _In_ KDPC *Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
    )
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    XDP_CPUMAP_RING *Ring = (XDP_CPUMAP_RING *)Context;
    NET_BUFFER_LIST *BatchHead = NULL;
    NET_BUFFER_LIST *BatchTail = NULL;
    UINT32 Count = 0;
    NDIS_HANDLE FilterHandle = NULL;
    NDIS_PORT_NUMBER PortNumber = 0;
    KLOCK_QUEUE_HANDLE LockHandle;
    BOOLEAN MoreWork;

    if (Ring == NULL) {
        return;
    }

    //
    // Dequeue batch
    //
    KeAcquireInStackQueuedSpinLock(&Ring->Lock, &LockHandle);

    while (Ring->Head != Ring->Tail && Count < XDP_CPUMAP_MAX_BATCH_SIZE) {
        XDP_CPUMAP_ENTRY *Entry = &Ring->Entries[Ring->Head & Ring->Mask];

        FilterHandle = Entry->FilterHandle;
        PortNumber = Entry->PortNumber;

        //
        // Build NBL chain
        //
        NET_BUFFER_LIST_NEXT_NBL(Entry->Nbl) = NULL;
        if (BatchTail != NULL) {
            NET_BUFFER_LIST_NEXT_NBL(BatchTail) = Entry->Nbl;
        } else {
            BatchHead = Entry->Nbl;
        }
        BatchTail = Entry->Nbl;

        Ring->Head++;
        Count++;
    }

    MoreWork = (Ring->Head != Ring->Tail);
    if (Count > 0) {
        InterlockedAdd(&Ring->DrainCount, Count);
    }

    KeReleaseInStackQueuedSpinLock(&LockHandle);

    //
    // Re-indicate to NDIS
    //
    if (BatchHead != NULL && FilterHandle != NULL) {
        NdisFIndicateReceiveNetBufferLists(
            FilterHandle,           // FilterModuleContext
            BatchHead,              // NetBufferLists (the NBL chain to indicate)
            PortNumber,             // PortNumber
            Count,                  // NumberOfNetBufferLists
            NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL | NDIS_RECEIVE_FLAGS_RESOURCES);

        //
        // RESOURCES flag means synchronous return, free clones immediately
        //
        NET_BUFFER_LIST *Current = BatchHead;
        while (Current != NULL) {
            NET_BUFFER_LIST *Next = NET_BUFFER_LIST_NEXT_NBL(Current);
            NdisFreeCloneNetBufferList(Current, 0);
            Current = Next;
        }
    }

    //
    // Re-queue DPC if more work remains
    //
    if (MoreWork) {
        KeInsertQueueDpc(Dpc, NULL, NULL);
    }
}
