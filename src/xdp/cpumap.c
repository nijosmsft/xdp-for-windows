//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

#include "precomp.h"
#include "cpumap.h"

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
XdpCpuMapCreate(
    _In_ UINT32 CpuBase,
    _In_ UINT32 CpuCount,
    _In_ UINT32 RingCapacity,
    _In_ UINT32 DrainBatchSize,
    _In_ UINT32 Flags,
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

    Map->CpuBase = CpuBase;
    Map->CpuCount = CpuCount;
    Map->Flags = Flags;
    Map->Active = TRUE;
    Map->RefCount = 1;

    //
    // Clamp DrainBatchSize to [1, XDP_CPUMAP_MAX_BATCH_SIZE].
    //
    if (DrainBatchSize == 0 || DrainBatchSize > XDP_CPUMAP_MAX_BATCH_SIZE) {
        DrainBatchSize = XDP_CPUMAP_MAX_BATCH_SIZE;
    }

    //
    // Allocate per-CPU ring array — one ring per TARGET CPU only.
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
    // Allocate per-CPU DPC array — one DPC per TARGET CPU only.
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
    // Allocate per-source-CPU NBL clone pools for ALL system CPUs.
    // Any RSS CPU can be a packet source, so we need a pool per source CPU
    // regardless of the target CPU range.  NdisFreeCloneNetBufferList
    // auto-routes frees back to the originating pool.
    //
    {
        UINT32 AllCpuCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
        Map->ClonePoolCount = 0;
        Map->PerCpuClonePools = ExAllocatePoolZero(
            NonPagedPoolNx,
            sizeof(NDIS_HANDLE) * AllCpuCount,
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

        for (UINT32 i = 0; i < AllCpuCount; i++) {
            Map->PerCpuClonePools[i] = NdisAllocateNetBufferListPool(NdisHandle, &PoolParams);
            if (Map->PerCpuClonePools[i] == NULL) {
                Status = STATUS_NO_MEMORY;
                goto Exit;
            }
            Map->ClonePoolCount++;
        }
    }

#if XDP_CPUMAP_PREALLOC
    //
    // Create a single NBL pool for pre-allocated shells.
    //
    Map->PreallocNblPool = NdisAllocateNetBufferListPool(NdisHandle, &PoolParams);
    if (Map->PreallocNblPool == NULL) {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }
#endif

#if XDP_CPUMAP_ZERO_COPY_INDICATE
    //
    // Initialize zero-copy indicate tracking. Event starts signaled;
    // destroy only waits if OutstandingIndications > 0.
    //
    Map->OutstandingIndications = 0;
    KeInitializeEvent(&Map->AllReturnedEvent, NotificationEvent, TRUE);
#endif

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
        Ring->DrainBatchSize = DrainBatchSize;
        Ring->EnqueueCount = 0;
        Ring->DrainCount = 0;
        Ring->DropCount = 0;
        Ring->RingFullCount = 0;
        Ring->CloneFailCount = 0;
        Ring->PreallocHitCount = 0;
        Ring->PreallocMissCount = 0;
        Ring->PreallocOversizeCount = 0;
        Ring->DpcInvokeCount = 0;
        Ring->DpcMaxBatchDrained = 0;
        Ring->DpcRequeueCount = 0;
        Ring->MaxRingDepth = 0;

#if XDP_CPUMAP_PREALLOC
        //
        // Pre-allocate NBL+NB+MDL+Buffer shells for this ring.
        // Allocate all shells in a single contiguous block to eliminate
        // per-shell pool headers and reduce NonPagedPool fragmentation.
        // Each shell is pushed onto a lock-free SLIST; enqueue pops,
        // drain pushes back — no NDIS pool alloc/free on the hot path.
        //
        InitializeSListHead(&Ring->PreallocFreeList);

        {
            SIZE_T BlockSize = (SIZE_T)RingCapacity * sizeof(XDP_CPUMAP_PREALLOC_SHELL);
            Ring->PreallocShellBlock = ExAllocatePoolZero(
                NonPagedPoolNx, BlockSize, POOLTAG_CPUMAP);
            if (Ring->PreallocShellBlock == NULL) {
                Status = STATUS_NO_MEMORY;
                goto Exit;
            }
        }

        for (UINT32 k = 0; k < RingCapacity; k++) {
            XDP_CPUMAP_PREALLOC_SHELL *Shell = &Ring->PreallocShellBlock[k];

            Shell->Mdl = IoAllocateMdl(
                Shell->DataBuffer,
                XDP_CPUMAP_PREALLOC_BUFFER_SIZE,
                FALSE, FALSE, NULL);
            if (Shell->Mdl == NULL) {
                Status = STATUS_NO_MEMORY;
                goto Exit;
            }
            MmBuildMdlForNonPagedPool(Shell->Mdl);

            Shell->Nbl = NdisAllocateNetBufferAndNetBufferList(
                Map->PreallocNblPool, 0, 0, Shell->Mdl, 0, 0);
            if (Shell->Nbl == NULL) {
                IoFreeMdl(Shell->Mdl);
                Shell->Mdl = NULL;
                Status = STATUS_NO_MEMORY;
                goto Exit;
            }

#if XDP_CPUMAP_ZERO_COPY_INDICATE
            Shell->OwnerFreeList = &Ring->PreallocFreeList;
            Shell->OwnerMap = Map;
#endif

            InterlockedPushEntrySList(
                &Ring->PreallocFreeList, &Shell->SListEntry);
        }
#endif

#pragma prefast(suppress:6386, "Buffer size is sizeof(XDP_CPUMAP_RING *) * CpuCount, indexed by i < CpuCount.")
        Map->PerCpuRings[i] = Ring;

#if XDP_CPUMAP_ZERO_COPY_INDICATE
        Ring->OwnerMap = Map;
#endif

        //
        // Initialize DPC with target CPU affinity
        //
        KeInitializeDpc(&Map->PerCpuDpcs[i], XdpCpuMapDrainDpc, Ring);

        Status = KeGetProcessorNumberFromIndex(CpuBase + i, &ProcNumber);
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

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapTrackMiniportIndication(
    _In_ XDP_CPUMAP *Map,
    _In_ BOOLEAN IsResources
    )
{
    if (IsResources) {
        InterlockedIncrement(&Map->MiniportResourcesCount);
    } else {
        InterlockedIncrement(&Map->MiniportNoResourcesCount);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
UINT32
XdpCpuMapGetFlags(
    _In_ XDP_CPUMAP *Map
    )
{
    return Map->Flags;
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
    // Drain any remaining ring entries for absolute zero-copy maps.
    // After Active=FALSE + DPC flush, no new enqueues or DPC drains
    // can happen. Originals in the ring were never indicated to tcpip,
    // so return them to the miniport.
    //
    // Non-AZC maps do not need this — their ring entries (shells/clones)
    // are freed during the per-CPU ring teardown below.
    //
    if ((CpuMap->Flags & XDP_CPUMAP_FLAG_ABSOLUTE_ZERO_COPY) &&
        CpuMap->PerCpuRings != NULL) {
        for (UINT32 i = 0; i < CpuMap->CpuCount; i++) {
            XDP_CPUMAP_RING *Ring = CpuMap->PerCpuRings[i];
            if (Ring == NULL) {
                continue;
            }
            while (Ring->Head != Ring->Tail) {
                XDP_CPUMAP_ENTRY *Entry = &Ring->Entries[Ring->Head & Ring->Mask];
                //
                // Original miniport NBL — return to miniport.
                //
                if (Entry->FilterHandle != NULL) {
                    NdisFReturnNetBufferLists(
                        Entry->FilterHandle, Entry->Nbl, 0);
                }
                Ring->Head++;
            }
        }
    }

#if XDP_CPUMAP_ZERO_COPY_INDICATE
    //
    // Wait for all outstanding indicated NBLs to be returned by tcpip.
    // After KeFlushQueuedDpcs, no new indications can happen, so the
    // count can only decrease.  The event starts signaled and is set
    // on each decrement-to-zero in XdpCpuMapReturnShells.
    //
    if (InterlockedCompareExchange(&CpuMap->OutstandingIndications, 0, 0) > 0) {
        LARGE_INTEGER Timeout;
        Timeout.QuadPart = -10LL * 1000 * 1000 * 30; // 30 seconds
        NTSTATUS WaitStatus =
            KeWaitForSingleObject(
                &CpuMap->AllReturnedEvent, Executive, KernelMode, FALSE, &Timeout);
        ASSERT(WaitStatus == STATUS_SUCCESS);
        UNREFERENCED_PARAMETER(WaitStatus);
    }
#endif

    //
    // Dump per-ring statistics via DbgPrintEx (works at any IRQL).
    // View with WinDbg or DbgView (enable kernel capture).
    //
    if (CpuMap->PerCpuRings != NULL) {
        LONG TotalEnqueue = 0, TotalDrain = 0, TotalDrop = 0;
        LONG TotalRingFull = 0, TotalCloneFail = 0;
        LONG TotalPreallocHit = 0, TotalPreallocMiss = 0, TotalOversize = 0;
        LONG TotalDpcInvoke = 0, TotalDpcRequeue = 0;
        LONG TotalEnqBatch = 0, TotalDpcLoop = 0, TotalDpcEmpty = 0;

        //
        // Calibrate rdtsc frequency: measure cycles over a known QPC interval.
        //
        LONG64 TscFreqHz = 1;
#if XDP_CPUMAP_PREALLOC
        {
            LARGE_INTEGER QpcStart, QpcEnd, QpcFreq;
            UINT64 TscStart, TscEnd;
            KeQueryPerformanceCounter(&QpcFreq);
            QpcStart = KeQueryPerformanceCounter(NULL);
            TscStart = __rdtsc();
            KeStallExecutionProcessor(100); // 100us stall
            QpcEnd = KeQueryPerformanceCounter(NULL);
            TscEnd = __rdtsc();
            LONG64 QpcElapsed = QpcEnd.QuadPart - QpcStart.QuadPart;
            if (QpcElapsed > 0) {
                TscFreqHz = (LONG64)((TscEnd - TscStart) * QpcFreq.QuadPart / QpcElapsed);
            }
        }
#endif

        DbgPrintEx(
            DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
            "CPUMAP: Destroying map (%u CPUs, ring capacity %u)\n",
            CpuMap->CpuCount,
            CpuMap->PerCpuRings[0] ? CpuMap->PerCpuRings[0]->Capacity : 0);

        for (UINT32 s = 0; s < CpuMap->CpuCount; s++) {
            XDP_CPUMAP_RING *R = CpuMap->PerCpuRings[s];
            if (R != NULL && (R->EnqueueCount || R->DrainCount || R->DropCount)) {
                TotalEnqueue += R->EnqueueCount;
                TotalDrain += R->DrainCount;
                TotalDrop += R->DropCount;
                TotalRingFull += R->RingFullCount;
                TotalCloneFail += R->CloneFailCount;
                TotalPreallocHit += R->PreallocHitCount;
                TotalPreallocMiss += R->PreallocMissCount;
                TotalOversize += R->PreallocOversizeCount;
                TotalDpcInvoke += R->DpcInvokeCount;
                TotalDpcRequeue += R->DpcRequeueCount;
                TotalEnqBatch += R->EnqueueBatchCount;
                TotalDpcLoop += R->DpcLoopIterations;
                TotalDpcEmpty += R->DpcEmptyCount;

                //
                // Count distinct source CPUs from bitmask.
                //
                UINT32 SrcCount = 0;
                {
                    LONG64 m0 = R->SourceCpuMask[0];
                    LONG64 m1 = R->SourceCpuMask[1];
                    while (m0) { SrcCount++; m0 &= m0 - 1; }
                    while (m1) { SrcCount++; m1 &= m1 - 1; }
                }

                LONG64 LkUs = 0;
                if (TscFreqHz > 1 && R->LockAcquireCount > 0) {
                    LkUs = R->LockWaitCycles * 1000000LL / TscFreqHz;
                }

                DbgPrintEx(
                    DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
                    "Ring[%2u]: Enq=%d Drain=%d Drop=%d "
                    "Full=%d CFail=%d "
                    "PHit=%d PMiss=%d Over=%d "
                    "DPC=%d MaxB=%d Req=%d MaxD=%d "
                    "EBat=%d Loop=%d MxL=%d Emp=%d "
                    "Src=%u Lk=%d LkUs=%lld\n",
                    s, R->EnqueueCount, R->DrainCount, R->DropCount,
                    R->RingFullCount, R->CloneFailCount,
                    R->PreallocHitCount, R->PreallocMissCount,
                    R->PreallocOversizeCount,
                    R->DpcInvokeCount, R->DpcMaxBatchDrained,
                    R->DpcRequeueCount, R->MaxRingDepth,
                    R->EnqueueBatchCount, R->DpcLoopIterations,
                    R->DpcMaxLoopIterations, R->DpcEmptyCount,
                    SrcCount, R->LockAcquireCount, LkUs);
            }
        }

        {
            LONG LossWhole = 0, LossFrac = 0;
            if (TotalEnqueue > 0) {
                LONG Bp = (LONG)((LONGLONG)TotalDrop * 10000 / TotalEnqueue);
                LossWhole = Bp / 100;
                LossFrac = Bp % 100;
            }
            DbgPrintEx(
                DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
                "Totals: Enq=%d Drain=%d Drop=%d (%d.%02d%%) "
                "Full=%d CFail=%d "
                "PHit=%d PMiss=%d Over=%d "
                "DPC=%d Req=%d "
                "EBat=%d Loop=%d Emp=%d\n",
                TotalEnqueue, TotalDrain, TotalDrop, LossWhole, LossFrac,
                TotalRingFull, TotalCloneFail,
                TotalPreallocHit, TotalPreallocMiss, TotalOversize,
                TotalDpcInvoke, TotalDpcRequeue,
                TotalEnqBatch, TotalDpcLoop, TotalDpcEmpty);
#if XDP_CPUMAP_PREALLOC
            if (CpuMap->CopyCount > 0 && TscFreqHz > 1) {
                LONG64 TotalCycles = CpuMap->CopyTotalCycles;
                LONG CopyN = CpuMap->CopyCount;
                LONG64 CopyBytes = CpuMap->CopyTotalBytes;
                LONG64 AvgNs = TotalCycles * 1000000000LL / TscFreqHz / CopyN;
                LONG64 TotalUs = TotalCycles * 1000000LL / TscFreqHz;
                LONG64 TotalMB = CopyBytes / (1024 * 1024);
                DbgPrintEx(
                    DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
                    "Copy: N=%d TotUs=%lld AvgNs=%lld MB=%lld TscHz=%lld\n",
                    CopyN, TotalUs, AvgNs, TotalMB, TscFreqHz);
            }
#endif
#if XDP_CPUMAP_ZERO_COPY_INDICATE
            DbgPrintEx(
                DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
                "ZeroCopy: Indicated=%d ShellRet=%d CloneRet=%d Outstanding=%d\n",
                CpuMap->ZeroCopyIndicateCount,
                CpuMap->ShellReturnCount,
                CpuMap->CloneReturnCount,
                CpuMap->OutstandingIndications);
#endif
            DbgPrintEx(
                DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
                "Miniport: Resources=%d NoResources=%d\n",
                CpuMap->MiniportResourcesCount,
                CpuMap->MiniportNoResourcesCount);
            DbgPrintEx(
                DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
                "AbsoluteZeroCopy: Flags=0x%x Indicated=%d\n",
                CpuMap->Flags,
                CpuMap->AbsoluteZeroCopyIndicateCount);

        }
    }

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
#pragma prefast(suppress:6001, "Ring entries between Head and Tail are always initialized by enqueue.")
                    if (Entry->Nbl != NULL) {
#if XDP_CPUMAP_PREALLOC
                        if (Entry->Shell != NULL) {
                            InterlockedPushEntrySList(
                                &Ring->PreallocFreeList, &Entry->Shell->SListEntry);
                        } else
#endif
                        {
#pragma prefast(suppress:6001, "Ring entries between Head and Tail are always initialized by enqueue.")
                            NdisFreeCloneNetBufferList(Entry->Nbl, 0);
                        }
                    }
                    Ring->Head++;
                }

#if XDP_CPUMAP_PREALLOC
                //
                // Free all pre-allocated shells.
                // Shells live in a contiguous block; free NBL/MDL per-shell,
                // then free the single block.
                //
                if (Ring->PreallocShellBlock != NULL) {
                    for (UINT32 k = 0; k < Ring->Capacity; k++) {
                        XDP_CPUMAP_PREALLOC_SHELL *Shell = &Ring->PreallocShellBlock[k];
                        if (Shell->Nbl != NULL) {
                            NdisFreeNetBufferList(Shell->Nbl);
                        }
                        if (Shell->Mdl != NULL) {
                            IoFreeMdl(Shell->Mdl);
                        }
                    }
                    ExFreePoolWithTag(Ring->PreallocShellBlock, POOLTAG_CPUMAP);
                }
#endif

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

#if XDP_CPUMAP_PREALLOC
    if (CpuMap->PreallocNblPool != NULL) {
        NdisFreeNetBufferListPool(CpuMap->PreallocNblPool);
    }
#endif

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
    if (!CpuMap->Active ||
        TargetCpu < CpuMap->CpuBase ||
        TargetCpu >= CpuMap->CpuBase + CpuMap->CpuCount) {
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

    Ring = CpuMap->PerCpuRings[TargetCpu - CpuMap->CpuBase];

    //
    // Acquire spinlock and enqueue
    //
    {
        UINT64 LockStart = __rdtsc();
        KeAcquireInStackQueuedSpinLock(&Ring->Lock, &LockHandle);
        InterlockedAdd64(&Ring->LockWaitCycles, (LONG64)(__rdtsc() - LockStart));
        InterlockedIncrement(&Ring->LockAcquireCount);
    }

    //
    // Track source CPU.
    //
    if (SourceCpu < 128) {
        InterlockedOr64(&Ring->SourceCpuMask[SourceCpu / 64], 1LL << (SourceCpu % 64));
    }

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
    KeInsertQueueDpc(&CpuMap->PerCpuDpcs[TargetCpu - CpuMap->CpuBase], NULL, NULL);

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapBatchInit(
    _Out_ XDP_CPUMAP_BATCH *Batch
    )
{
    Batch->Count = 0;
    Batch->ReturnableOriginals = NULL;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
XdpCpuMapBatchAdd(
    _Inout_ XDP_CPUMAP_BATCH *Batch,
    _In_ NET_BUFFER_LIST *Nbl,
    _In_ UINT32 TargetCpu,
    _In_ NDIS_HANDLE FilterHandle,
    _In_ NDIS_PORT_NUMBER PortNumber
    )
{
    if (Batch->Count >= XDP_CPUMAP_MAX_BATCH_ENTRIES) {
        return FALSE;
    }

    XDP_CPUMAP_BATCH_ENTRY *Entry = &Batch->Entries[Batch->Count];
    Entry->OriginalNbl = Nbl;
    Entry->TargetCpu = TargetCpu;
    Entry->FilterHandle = FilterHandle;
    Entry->PortNumber = PortNumber;
    Batch->Count++;

    return TRUE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapFlushBatch(
    _In_ XDP_CPUMAP *CpuMap,
    _Inout_ XDP_CPUMAP_BATCH *Batch
    )
/*++

Routine Description:

    Flush all collected CPU redirect decisions in a single pass.

    Phase 1: Prepare all NBLs in a tight loop (no locks held).
             When XDP_CPUMAP_PREALLOC=1 (default), pops a pre-allocated
             shell from the target ring's lock-free SLIST and memcpys the
             packet data into it.  Falls back to NdisAllocateCloneNetBufferList
             (using the source CPU's per-CPU clone pool) for oversized packets
             or when the SLIST is empty.
             When XDP_CPUMAP_PREALLOC=0, clones all NBLs via the source CPU's
             per-CPU clone pool for cache-hot L1 lookaside.

    Phase 2: For each unique target CPU, acquire the ring lock once,
             enqueue all clones destined for that target, release the lock,
             and schedule one DPC.

    This reduces lock acquisitions from N (one per packet) to T (one per
    unique target CPU in the batch) and DPC inserts from N to T.

--*/
{
    UINT32 i, j;
    NET_BUFFER_LIST *Clones[XDP_CPUMAP_MAX_BATCH_ENTRIES];
    BOOLEAN Processed[XDP_CPUMAP_MAX_BATCH_ENTRIES];
#if XDP_CPUMAP_PREALLOC
    XDP_CPUMAP_PREALLOC_SHELL *Shells[XDP_CPUMAP_MAX_BATCH_ENTRIES];
#endif

    if (Batch->Count == 0) {
        return;
    }

    if (!CpuMap->Active) {
        if (CpuMap->Flags & XDP_CPUMAP_FLAG_ABSOLUTE_ZERO_COPY) {
            for (i = 0; i < Batch->Count; i++) {
                NET_BUFFER_LIST *Original = Batch->Entries[i].OriginalNbl;
                NET_BUFFER_LIST_NEXT_NBL(Original) = Batch->ReturnableOriginals;
                Batch->ReturnableOriginals = Original;
            }
        }
        Batch->Count = 0;
        return;
    }

    //
    // Resolve the source CPU's clone pool (once per flush, outside any lock).
    //
    UINT32 SourceCpu = KeGetCurrentProcessorIndex();
    NDIS_HANDLE ClonePool;

    if (SourceCpu < CpuMap->ClonePoolCount) {
        ClonePool = CpuMap->PerCpuClonePools[SourceCpu];
    } else {
        ClonePool = CpuMap->PerCpuClonePools[0];
    }

#if XDP_CPUMAP_PREALLOC
    UINT64 BatchCopyStart = 0, BatchCopyCycles = 0;
    LONG BatchCopyCount = 0;
    LONG64 BatchCopyBytes = 0;
#endif

    //
    // Phase 1: Prepare NBLs for each batch entry. No locks held.
    //
    for (i = 0; i < Batch->Count; i++) {
        XDP_CPUMAP_BATCH_ENTRY *BatchEntry = &Batch->Entries[i];
        NET_BUFFER_LIST *Original = BatchEntry->OriginalNbl;

#if XDP_CPUMAP_PREALLOC
        Shells[i] = NULL;
#endif
        Clones[i] = NULL;
        Processed[i] = TRUE;

        //
        // Absolute zero-copy: use the original miniport NBL directly.
        // No shell pop, no clone, no memcpy. The original is enqueued
        // to the ring and indicated to tcpip from DrainDpc.
        //
        if (CpuMap->Flags & XDP_CPUMAP_FLAG_ABSOLUTE_ZERO_COPY) {
            Clones[i] = Original;
            Processed[i] = FALSE;
            continue;
        }

#if XDP_CPUMAP_PREALLOC
        //
        // Try pre-allocated shell path: pop a shell from the target ring's
        // lock-free free list, memcpy the packet data, and use the shell's
        // pre-allocated NBL.  This eliminates NdisAllocateCloneNetBufferList
        // and NdisFreeCloneNetBufferList from the hot path.
        //
        {
            UINT32 TargetCpu = BatchEntry->TargetCpu;
            BOOLEAN InRange = (TargetCpu >= CpuMap->CpuBase &&
                               TargetCpu < CpuMap->CpuBase + CpuMap->CpuCount);

            if (InRange) {
                XDP_CPUMAP_RING *TargetRing = CpuMap->PerCpuRings[TargetCpu - CpuMap->CpuBase];
                PSLIST_ENTRY Sle =
                    InterlockedPopEntrySList(&TargetRing->PreallocFreeList);

                if (Sle != NULL) {
                    XDP_CPUMAP_PREALLOC_SHELL *Shell =
                        CONTAINING_RECORD(Sle, XDP_CPUMAP_PREALLOC_SHELL, SListEntry);
                    NET_BUFFER *SrcNb = NET_BUFFER_LIST_FIRST_NB(Original);
                    ULONG DataLen = NET_BUFFER_DATA_LENGTH(SrcNb);

                    if (DataLen <= XDP_CPUMAP_PREALLOC_BUFFER_SIZE) {
                        PVOID SrcData = NdisGetDataBuffer(
                            SrcNb, DataLen, Shell->DataBuffer, 1, 0);

                        if (SrcData != NULL) {
                            NET_BUFFER *DstNb;

                            if (SrcData != Shell->DataBuffer) {
                                BatchCopyStart = __rdtsc();
                                RtlCopyMemory(
                                    Shell->DataBuffer, SrcData, DataLen);
                                BatchCopyCycles += __rdtsc() - BatchCopyStart;
                                BatchCopyCount++;
                                BatchCopyBytes += DataLen;
                            }

                            //
                            // Wire up the pre-allocated NB to describe the
                            // copied data.
                            //
                            DstNb = NET_BUFFER_LIST_FIRST_NB(Shell->Nbl);
                            NET_BUFFER_DATA_LENGTH(DstNb) = DataLen;
                            NET_BUFFER_DATA_OFFSET(DstNb) = 0;
                            NET_BUFFER_CURRENT_MDL(DstNb) = Shell->Mdl;
                            NET_BUFFER_CURRENT_MDL_OFFSET(DstNb) = 0;

                            //
                            // Preserve RSS hash metadata.
                            //
                            NET_BUFFER_LIST_SET_HASH_VALUE(Shell->Nbl,
                                NET_BUFFER_LIST_GET_HASH_VALUE(Original));
                            NET_BUFFER_LIST_SET_HASH_TYPE(Shell->Nbl,
                                NET_BUFFER_LIST_GET_HASH_TYPE(Original));

                            Clones[i] = Shell->Nbl;
                            Shells[i] = Shell;
                            Processed[i] = FALSE;
                            InterlockedIncrement(&TargetRing->PreallocHitCount);
                            continue;
                        }
                    }

                    //
                    // Packet too large or data retrieval failed;
                    // return the shell and fall through to clone path.
                    //
                    if (DataLen > XDP_CPUMAP_PREALLOC_BUFFER_SIZE) {
                        InterlockedIncrement(&TargetRing->PreallocOversizeCount);
                    }
                    InterlockedPushEntrySList(
                        &TargetRing->PreallocFreeList, &Shell->SListEntry);
                } else {
                    //
                    // SLIST was empty — all shells in-flight.
                    //
                    InterlockedIncrement(&TargetRing->PreallocMissCount);
                }
            }
        }
#endif // XDP_CPUMAP_PREALLOC

        //
        // Fallback: clone the NBL via the NDIS pool allocator.
        //
        Clones[i] = NdisAllocateCloneNetBufferList(Original, ClonePool, NULL, 0);

        if (Clones[i] != NULL) {
            //
            // Preserve RSS hash and metadata on the clone.
            //
            NET_BUFFER_LIST_SET_HASH_VALUE(Clones[i],
                NET_BUFFER_LIST_GET_HASH_VALUE(Original));
            NET_BUFFER_LIST_SET_HASH_TYPE(Clones[i],
                NET_BUFFER_LIST_GET_HASH_TYPE(Original));
            Processed[i] = FALSE;
        } else {
            //
            // Clone allocation failed.
            //
            UINT32 Tc = BatchEntry->TargetCpu;
            if (Tc >= CpuMap->CpuBase && Tc < CpuMap->CpuBase + CpuMap->CpuCount) {
                InterlockedIncrement(&CpuMap->PerCpuRings[Tc - CpuMap->CpuBase]->CloneFailCount);
            }
        }
    }

    //
    // Phase 2: Group by target CPU and enqueue with one lock acquisition
    // per target. With N <= 32, the O(N*T) scan is trivial at DISPATCH_LEVEL.
    //
    for (i = 0; i < Batch->Count; i++) {
        KLOCK_QUEUE_HANDLE LockHandle;
        XDP_CPUMAP_RING *Ring;
        UINT32 TargetCpu;
        UINT32 Enqueued = 0;
        UINT32 Dropped = 0;

        if (Processed[i]) {
            continue;
        }

        TargetCpu = Batch->Entries[i].TargetCpu;

        if (TargetCpu < CpuMap->CpuBase || TargetCpu >= CpuMap->CpuBase + CpuMap->CpuCount) {
            //
            // Invalid target — free the clone (or return original) and skip.
            //
            if (CpuMap->Flags & XDP_CPUMAP_FLAG_ABSOLUTE_ZERO_COPY) {
                NET_BUFFER_LIST_NEXT_NBL(Clones[i]) = Batch->ReturnableOriginals;
                Batch->ReturnableOriginals = Clones[i];
            } else {
                NdisFreeCloneNetBufferList(Clones[i], 0);
            }
            Processed[i] = TRUE;
            continue;
        }

        Ring = CpuMap->PerCpuRings[TargetCpu - CpuMap->CpuBase];

        //
        // Acquire the target ring lock ONCE for all entries going to this CPU.
        //
        {
            UINT64 LockStart = __rdtsc();
            KeAcquireInStackQueuedSpinLock(&Ring->Lock, &LockHandle);
            InterlockedAdd64(&Ring->LockWaitCycles, (LONG64)(__rdtsc() - LockStart));
            InterlockedIncrement(&Ring->LockAcquireCount);
        }

        //
        // Track source CPU.
        //
        if (SourceCpu < 128) {
            InterlockedOr64(&Ring->SourceCpuMask[SourceCpu / 64], 1LL << (SourceCpu % 64));
        }

        //
        // Enqueue this entry and all subsequent entries with the same target.
        //
        for (j = i; j < Batch->Count; j++) {
            XDP_CPUMAP_ENTRY *RingEntry;
            UINT32 NextTail;

            if (Processed[j] || Batch->Entries[j].TargetCpu != TargetCpu) {
                continue;
            }

            NextTail = Ring->Tail + 1;
            if ((NextTail - Ring->Head) > Ring->Capacity) {
                //
                // Ring full — recycle shell, free clone, or return original.
                //
                if (CpuMap->Flags & XDP_CPUMAP_FLAG_ABSOLUTE_ZERO_COPY) {
                    NET_BUFFER_LIST_NEXT_NBL(Clones[j]) = Batch->ReturnableOriginals;
                    Batch->ReturnableOriginals = Clones[j];
                } else {
#if XDP_CPUMAP_PREALLOC
                if (Shells[j] != NULL) {
                    InterlockedPushEntrySList(
                        &Ring->PreallocFreeList, &Shells[j]->SListEntry);
                } else
#endif
                {
                    NdisFreeCloneNetBufferList(Clones[j], 0);
                }
                }
                Dropped++;
                InterlockedIncrement(&Ring->RingFullCount);
                Processed[j] = TRUE;
                continue;
            }

            RingEntry = &Ring->Entries[Ring->Tail & Ring->Mask];
            RingEntry->Nbl = Clones[j];
            RingEntry->FilterHandle = Batch->Entries[j].FilterHandle;
            RingEntry->PortNumber = Batch->Entries[j].PortNumber;
#if XDP_CPUMAP_PREALLOC
            RingEntry->Shell = Shells[j];
#endif

            Ring->Tail = NextTail;
            Enqueued++;
            Processed[j] = TRUE;

            //
            // Track high-water mark of ring occupancy.
            //
            {
                LONG Depth = (LONG)(Ring->Tail - Ring->Head);
                if (Depth > Ring->MaxRingDepth) {
                    Ring->MaxRingDepth = Depth;
                }
            }
        }

        if (Enqueued > 0) {
            InterlockedAdd(&Ring->EnqueueCount, Enqueued);
            InterlockedIncrement(&Ring->EnqueueBatchCount);
        }
        if (Dropped > 0) {
            InterlockedAdd(&Ring->DropCount, Dropped);
        }

        KeReleaseInStackQueuedSpinLock(&LockHandle);

        //
        // Schedule exactly one DPC per target CPU.
        //
        if (Enqueued > 0) {
            KeInsertQueueDpc(&CpuMap->PerCpuDpcs[TargetCpu - CpuMap->CpuBase], NULL, NULL);
        }
    }

    Batch->Count = 0;

#if XDP_CPUMAP_PREALLOC
    //
    // Flush batch-local copy stats to map-level counters (one interlocked
    // add per FlushBatch call instead of per packet).
    //
    if (BatchCopyCount > 0) {
        InterlockedAdd64(&CpuMap->CopyTotalCycles, (LONG64)BatchCopyCycles);
        InterlockedAdd(&CpuMap->CopyCount, BatchCopyCount);
        InterlockedAdd64(&CpuMap->CopyTotalBytes, BatchCopyBytes);
    }
#endif
}

#if XDP_CPUMAP_ZERO_COPY_INDICATE
_IRQL_requires_max_(DISPATCH_LEVEL)
NET_BUFFER_LIST *
XdpCpuMapReturnShells(
    _In_ NET_BUFFER_LIST *NetBufferLists
    )
/*++

Routine Description:

    Walk a returned NBL chain and pull out any CPUMAP-indicated NBLs
    (identified by magic tags in MiniportReserved[1]).

    Shell-backed NBLs (SHELL_MAGIC): push the shell back to its ring's
    lock-free free list and decrement the map's outstanding count.

    Clone-backed NBLs (CLONE_MAGIC): free the clone NBL and decrement
    the map's outstanding count.

    Returns the remaining (non-CPUMAP) NBL chain.

--*/
{
    NET_BUFFER_LIST *PassHead = NULL;
    NET_BUFFER_LIST **PassTail = &PassHead;
    NET_BUFFER_LIST *Current = NetBufferLists;

    while (Current != NULL) {
        NET_BUFFER_LIST *Next = NET_BUFFER_LIST_NEXT_NBL(Current);

        if (Current->MiniportReserved[1] == XDP_CPUMAP_SHELL_MAGIC) {
            //
            // Shell-backed: recycle shell to ring's SLIST.
            //
            XDP_CPUMAP_PREALLOC_SHELL *Shell =
                (XDP_CPUMAP_PREALLOC_SHELL *)Current->MiniportReserved[0];

            Current->MiniportReserved[0] = NULL;
            Current->MiniportReserved[1] = NULL;

            InterlockedPushEntrySList(Shell->OwnerFreeList, &Shell->SListEntry);
            InterlockedIncrement(&Shell->OwnerMap->ShellReturnCount);

            if (InterlockedDecrement(&Shell->OwnerMap->OutstandingIndications) == 0) {
                KeSetEvent(&Shell->OwnerMap->AllReturnedEvent, IO_NO_INCREMENT, FALSE);
            }
        } else if (Current->MiniportReserved[1] == XDP_CPUMAP_CLONE_MAGIC) {
            //
            // Clone-backed (fallback path): free clone, decrement count.
            //
            XDP_CPUMAP *Map = (XDP_CPUMAP *)Current->MiniportReserved[0];

            Current->MiniportReserved[0] = NULL;
            Current->MiniportReserved[1] = NULL;

            NdisFreeCloneNetBufferList(Current, 0);
            InterlockedIncrement(&Map->CloneReturnCount);

            if (InterlockedDecrement(&Map->OutstandingIndications) == 0) {
                KeSetEvent(&Map->AllReturnedEvent, IO_NO_INCREMENT, FALSE);
            }
        } else {
            //
            // Not a CPUMAP NBL — pass through.
            //
            NET_BUFFER_LIST_NEXT_NBL(Current) = NULL;
            *PassTail = Current;
            PassTail = &NET_BUFFER_LIST_NEXT_NBL(Current);
        }

        Current = Next;
    }

    return PassHead;
}
#endif // XDP_CPUMAP_ZERO_COPY_INDICATE

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
#if !XDP_CPUMAP_DRAIN_ALL && XDP_CPUMAP_PREALLOC
    XDP_CPUMAP_PREALLOC_SHELL *ShellBatch[XDP_CPUMAP_MAX_BATCH_SIZE];
#endif

    if (Ring == NULL) {
        return;
    }

    InterlockedIncrement(&Ring->DpcInvokeCount);

#if !XDP_CPUMAP_DRAIN_ALL
    //
    // Batched drain-until-empty: loop inside the DPC, draining up to
    // XDP_CPUMAP_MAX_BATCH_SIZE per iteration. This keeps per-indication
    // batch sizes small (low latency) while eliminating the DPC re-queue
    // gap that causes ring drops under load.
    //
    {
    UINT32 LoopIter = 0;
    do {
    LoopIter++;
#endif

    BatchHead = NULL;
    BatchTail = NULL;
    Count = 0;
    FilterHandle = NULL;
    PortNumber = 0;

    //
    // Dequeue batch
    //
    {
        UINT64 LockStart = __rdtsc();
        KeAcquireInStackQueuedSpinLock(&Ring->Lock, &LockHandle);
        InterlockedAdd64(&Ring->LockWaitCycles, (LONG64)(__rdtsc() - LockStart));
        InterlockedIncrement(&Ring->LockAcquireCount);
    }

#if XDP_CPUMAP_DRAIN_ALL
    //
    // Drain-all mode: dequeue the entire ring in one pass. No batch cap.
    // Shell pointers are stored inline in the NBL scratch field and
    // recycled after NDIS indication, avoiding a fixed-size stack array.
    //
    while (Ring->Head != Ring->Tail) {
#else
    while (Ring->Head != Ring->Tail && Count < Ring->DrainBatchSize) {
#endif
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

#if XDP_CPUMAP_DRAIN_ALL && XDP_CPUMAP_PREALLOC
        //
        // Stash shell pointer in NBL->MiniportReserved[0] so we can
        // recycle it after indication without a separate array.
        //
        Entry->Nbl->MiniportReserved[0] = Entry->Shell;
#elif XDP_CPUMAP_PREALLOC
        ShellBatch[Count] = Entry->Shell;
#endif

        Ring->Head++;
        Count++;
    }

    MoreWork = (Ring->Head != Ring->Tail);
    if (Count > 0) {
        InterlockedAdd(&Ring->DrainCount, Count);
        //
        // Track largest batch drained in a single DPC.
        //
        if ((LONG)Count > Ring->DpcMaxBatchDrained) {
            Ring->DpcMaxBatchDrained = (LONG)Count;
        }
    } else if (!MoreWork) {
        //
        // DPC fired but ring was already empty.
        //
        InterlockedIncrement(&Ring->DpcEmptyCount);
    }

    KeReleaseInStackQueuedSpinLock(&LockHandle);

    //
    // Re-indicate to NDIS
    //
    if (BatchHead != NULL && FilterHandle != NULL) {
        //
        // Absolute zero-copy: indicate original miniport NBLs directly.
        // No magic stamping, no OutstandingIndications tracking (originals
        // return through normal NDIS ReturnNetBufferLists → miniport).
        //
        if (Ring->OwnerMap->Flags & XDP_CPUMAP_FLAG_ABSOLUTE_ZERO_COPY) {
            NdisFIndicateReceiveNetBufferLists(
                FilterHandle,
                BatchHead,
                PortNumber,
                Count,
                NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL);

            InterlockedAdd(&Ring->OwnerMap->AbsoluteZeroCopyIndicateCount, Count);
        } else {
#if XDP_CPUMAP_ZERO_COPY_INDICATE
        //
        // Zero-copy indicate: stamp magic tags so the return path can
        // identify and recycle CPUMAP NBLs.  Increment outstanding count
        // for each NBL.  Indicate WITHOUT RESOURCES flag — tcpip takes
        // ownership and returns NBLs asynchronously.
        //
        {
            NET_BUFFER_LIST *Cur = BatchHead;
#if !XDP_CPUMAP_DRAIN_ALL
            UINT32 StampIdx = 0;
#endif
            while (Cur != NULL) {
#if XDP_CPUMAP_DRAIN_ALL
                XDP_CPUMAP_PREALLOC_SHELL *Shell =
                    (XDP_CPUMAP_PREALLOC_SHELL *)Cur->MiniportReserved[0];
#else
                XDP_CPUMAP_PREALLOC_SHELL *Shell = ShellBatch[StampIdx];
#endif
                if (Shell != NULL) {
                    Cur->MiniportReserved[0] = Shell;
                    Cur->MiniportReserved[1] = XDP_CPUMAP_SHELL_MAGIC;
                } else {
                    Cur->MiniportReserved[0] = Ring->OwnerMap;
                    Cur->MiniportReserved[1] = XDP_CPUMAP_CLONE_MAGIC;
                }
                InterlockedIncrement(&Ring->OwnerMap->OutstandingIndications);
#if !XDP_CPUMAP_DRAIN_ALL
                StampIdx++;
#endif
                Cur = NET_BUFFER_LIST_NEXT_NBL(Cur);
            }
        }

        NdisFIndicateReceiveNetBufferLists(
            FilterHandle,
            BatchHead,
            PortNumber,
            Count,
            NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL);

        InterlockedAdd(&Ring->OwnerMap->ZeroCopyIndicateCount, Count);
        //
        // No synchronous recycle — shells/clones are returned via
        // XdpCpuMapReturnShells when tcpip calls ReturnNetBufferLists.
        //
#else // !XDP_CPUMAP_ZERO_COPY_INDICATE
        NdisFIndicateReceiveNetBufferLists(
            FilterHandle,           // FilterModuleContext
            BatchHead,              // NetBufferLists (the NBL chain to indicate)
            PortNumber,             // PortNumber
            Count,                  // NumberOfNetBufferLists
            NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL | NDIS_RECEIVE_FLAGS_RESOURCES);

        //
        // RESOURCES flag means synchronous return — recycle shells or free
        // clones immediately.
        //
        {
            NET_BUFFER_LIST *Current = BatchHead;
#if !XDP_CPUMAP_DRAIN_ALL && XDP_CPUMAP_PREALLOC
            UINT32 Idx = 0;
#endif
            while (Current != NULL) {
                NET_BUFFER_LIST *Next = NET_BUFFER_LIST_NEXT_NBL(Current);
#if XDP_CPUMAP_DRAIN_ALL && XDP_CPUMAP_PREALLOC
                {
                    XDP_CPUMAP_PREALLOC_SHELL *Shell =
                        (XDP_CPUMAP_PREALLOC_SHELL *)Current->MiniportReserved[0];
                    if (Shell != NULL) {
                        InterlockedPushEntrySList(
                            &Ring->PreallocFreeList,
                            &Shell->SListEntry);
                    } else {
                        NdisFreeCloneNetBufferList(Current, 0);
                    }
                }
#elif XDP_CPUMAP_PREALLOC
                if (ShellBatch[Idx] != NULL) {
                    //
                    // Pre-allocated shell — push back to the ring's lock-free
                    // free list.  No NDIS pool free required.
                    //
                    InterlockedPushEntrySList(
                        &Ring->PreallocFreeList,
                        &ShellBatch[Idx]->SListEntry);
                } else
                {
                    NdisFreeCloneNetBufferList(Current, 0);
                }
#else
                {
                    NdisFreeCloneNetBufferList(Current, 0);
                }
#endif
#if !XDP_CPUMAP_DRAIN_ALL && XDP_CPUMAP_PREALLOC
                Idx++;
#endif
                Current = Next;
            }
        }
#endif // XDP_CPUMAP_ZERO_COPY_INDICATE
        } // end else (non-absolute-zero-copy path)
    }

#if XDP_CPUMAP_DRAIN_ALL
    //
    // Drain-all mode: re-queue DPC if more work remains (shouldn't happen).
    //
    if (MoreWork) {
        InterlockedIncrement(&Ring->DpcRequeueCount);
        KeInsertQueueDpc(Dpc, NULL, NULL);
    }
#else
    //
    // Batched mode: loop back if more work remains. Track re-queue count
    // for stats parity (counts loop iterations beyond the first).
    //
    if (MoreWork) {
        InterlockedIncrement(&Ring->DpcRequeueCount);
    }
    } while (MoreWork);

    InterlockedAdd(&Ring->DpcLoopIterations, (LONG)LoopIter);
    if ((LONG)LoopIter > Ring->DpcMaxLoopIterations) {
        Ring->DpcMaxLoopIterations = (LONG)LoopIter;
    }
    } // end LoopIter scope
#endif
}
