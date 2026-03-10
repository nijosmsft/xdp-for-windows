//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

#include "precomp.h"
#include "cpumap.h"
#include <ndis/ndl/mdl.h>

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

    UNREFERENCED_PARAMETER(Flags);

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
    // Create !CanPend deep-copy fallback pool.
    // No NBLs pre-allocated — lazy growth on demand.
    //
    {
        NET_BUFFER_LIST_POOL_PARAMETERS DcPoolParams;
        RtlZeroMemory(&DcPoolParams, sizeof(DcPoolParams));
        DcPoolParams.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        DcPoolParams.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
        DcPoolParams.Header.Size = sizeof(DcPoolParams);
        DcPoolParams.PoolTag = POOLTAG_CPUMAP;
        DcPoolParams.fAllocateNetBuffer = TRUE;
        DcPoolParams.ContextSize = 0;

        Map->DeepCopyNblPool = NdisAllocateNetBufferListPool(NdisHandle, &DcPoolParams);
        if (Map->DeepCopyNblPool == NULL) {
            Status = STATUS_NO_MEMORY;
            goto Exit;
        }
        InitializeSListHead(&Map->DeepCopyFreeList);
        Map->DeepCopyAllocCount = 0;
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
        Ring->DrainBatchSize = DrainBatchSize;
        Ring->EnqueueCount = 0;
        Ring->DrainCount = 0;
        Ring->DropCount = 0;
        Ring->RingFullCount = 0;
        Ring->DpcInvokeCount = 0;
        Ring->DpcMaxBatchDrained = 0;
        Ring->DpcRequeueCount = 0;
        Ring->MaxRingDepth = 0;

#pragma prefast(suppress:6386, "Buffer size is sizeof(XDP_CPUMAP_RING *) * CpuCount, indexed by i < CpuCount.")
        Map->PerCpuRings[i] = Ring;
        Ring->OwnerMap = Map;

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
    // Drain any remaining ring entries.
    // After Active=FALSE + DPC flush, no new enqueues or DPC drains
    // can happen. Originals in the ring were never indicated to tcpip,
    // so return them to the miniport.
    //
    if (CpuMap->PerCpuRings != NULL) {
        for (UINT32 i = 0; i < CpuMap->CpuCount; i++) {
            XDP_CPUMAP_RING *Ring = CpuMap->PerCpuRings[i];
            if (Ring == NULL) {
                continue;
            }

            while (Ring->Head != Ring->Tail) {
                XDP_CPUMAP_ENTRY *Entry = &Ring->Entries[Ring->Head & Ring->Mask];
                if (Entry->IsDeepCopy) {
                    //
                    // Deep-copy NBL (never indicated): recycle to pool.
                    //
                    NET_BUFFER *Nb = NET_BUFFER_LIST_FIRST_NB(Entry->Nbl);
                    NdisAdvanceNetBufferDataStart(Nb, Nb->DataLength, TRUE, NULL);
                    InterlockedPushEntrySList(
                        &CpuMap->DeepCopyFreeList, (PSLIST_ENTRY)&Entry->Nbl->Next);
                } else {
                    //
                    // Original miniport NBL — return to miniport.
                    //
                    if (Entry->FilterHandle != NULL) {
                        NdisFReturnNetBufferLists(
                            Entry->FilterHandle, Entry->Nbl, 0);
                    }
                }
                Ring->Head++;
            }
        }
    }

    //
    // Dump per-ring statistics via DbgPrintEx (works at any IRQL).
    // View with WinDbg or DbgView (enable kernel capture).
    //
    if (CpuMap->PerCpuRings != NULL) {
        LONG TotalEnqueue = 0, TotalDrain = 0, TotalDrop = 0;
        LONG TotalRingFull = 0;
        LONG TotalDpcInvoke = 0, TotalDpcRequeue = 0;
        LONG TotalEnqBatch = 0, TotalDpcLoop = 0, TotalDpcEmpty = 0, TotalDpcYield = 0;
        LONG64 TscFreqHz = 1;

        //
        // Calibrate rdtsc frequency: measure cycles over a known QPC interval.
        //
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
                TotalDpcInvoke += R->DpcInvokeCount;
                TotalDpcRequeue += R->DpcRequeueCount;
                TotalEnqBatch += R->EnqueueBatchCount;
                TotalDpcLoop += R->DpcLoopIterations;
                TotalDpcEmpty += R->DpcEmptyCount;
                TotalDpcYield += R->DpcYieldCount;

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
                    "Full=%d "
                    "DPC=%d MaxB=%d Req=%d MaxD=%d "
                    "EBat=%d Loop=%d MxL=%d Emp=%d Yld=%d "
                    "Src=%u Lk=%d LkUs=%lld\n",
                    s, R->EnqueueCount, R->DrainCount, R->DropCount,
                    R->RingFullCount,
                    R->DpcInvokeCount, R->DpcMaxBatchDrained,
                    R->DpcRequeueCount, R->MaxRingDepth,
                    R->EnqueueBatchCount, R->DpcLoopIterations,
                    R->DpcMaxLoopIterations, R->DpcEmptyCount,
                    R->DpcYieldCount,
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
                "Full=%d "
                "DPC=%d Req=%d Yld=%d "
                "EBat=%d Loop=%d Emp=%d\n",
                TotalEnqueue, TotalDrain, TotalDrop, LossWhole, LossFrac,
                TotalRingFull,
                TotalDpcInvoke, TotalDpcRequeue, TotalDpcYield,
                TotalEnqBatch, TotalDpcLoop, TotalDpcEmpty);
            DbgPrintEx(
                DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
                "Miniport: Resources=%d NoResources=%d\n",
                CpuMap->MiniportResourcesCount,
                CpuMap->MiniportNoResourcesCount);
            DbgPrintEx(
                DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
                "ZeroCopy: Indicated=%d\n",
                CpuMap->AbsoluteZeroCopyIndicateCount);
            DbgPrintEx(
                DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
                "DeepCopy: Alloc=%d Hit=%d Miss=%d Fail=%d Indicated=%d\n",
                CpuMap->DeepCopyAllocCount,
                CpuMap->DeepCopyHitCount,
                CpuMap->DeepCopyMissCount,
                CpuMap->DeepCopyFailCount,
                CpuMap->DeepCopyIndicateCount);

        }
    }

    //
    // Free per-CPU rings
    //
    if (CpuMap->PerCpuRings != NULL) {
        for (UINT32 i = 0; i < CpuMap->CpuCount; i++) {
            if (CpuMap->PerCpuRings[i] != NULL) {
                ExFreePoolWithTag(CpuMap->PerCpuRings[i], POOLTAG_CPUMAP);
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
    // Drain and free the deep-copy fallback pool.
    //
    if (CpuMap->DeepCopyNblPool != NULL) {
        PSLIST_ENTRY Sle;
        while ((Sle = InterlockedPopEntrySList(&CpuMap->DeepCopyFreeList)) != NULL) {
            NET_BUFFER_LIST *Nbl = CONTAINING_RECORD(Sle, NET_BUFFER_LIST, Next);
            NdisFreeNetBufferList(Nbl);
        }
        NdisFreeNetBufferListPool(CpuMap->DeepCopyNblPool);
        CpuMap->DeepCopyNblPool = NULL;
    }

    //
    // Free CPUMAP structure
    //
    ExFreePoolWithTag(CpuMap, POOLTAG_CPUMAP);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpCpuMapBatchInit(
    _Out_ XDP_CPUMAP_BATCH *Batch
    )
{
    Batch->Count = 0;
    Batch->CanPend = TRUE;
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
             When CanPend, uses original miniport NBLs directly.
             When !CanPend, deep-copies packet data into independent NBLs.

    Phase 2: For each unique target CPU, acquire the ring lock once,
             enqueue all NBLs destined for that target, release the lock,
             and schedule one DPC.

    This reduces lock acquisitions from N (one per packet) to T (one per
    unique target CPU in the batch) and DPC inserts from N to T.

--*/
{
    UINT32 i, j;
    NET_BUFFER_LIST *Clones[XDP_CPUMAP_MAX_BATCH_ENTRIES];
    BOOLEAN Processed[XDP_CPUMAP_MAX_BATCH_ENTRIES];
    BOOLEAN IsDeepCopyBatch[XDP_CPUMAP_MAX_BATCH_ENTRIES];
    UINT32 SourceCpu = 0;

    if (Batch->Count == 0) {
        return;
    }

    if (!CpuMap->Active) {
        if (Batch->CanPend) {
            for (i = 0; i < Batch->Count; i++) {
                NET_BUFFER_LIST *Original = Batch->Entries[i].OriginalNbl;
                NET_BUFFER_LIST_NEXT_NBL(Original) = Batch->ReturnableOriginals;
                Batch->ReturnableOriginals = Original;
            }
        }
        //
        // When !CanPend, recv.c already added originals to DropList.
        //
        Batch->Count = 0;
        return;
    }

    //
    // Phase 1: Prepare NBLs for each batch entry. No locks held.
    // When CanPend, use the original miniport NBL directly.
    // When !CanPend, deep-copy the packet data into a lazily-allocated
    // NBL so the original can return to the miniport.
    //
    for (i = 0; i < Batch->Count; i++) {
        XDP_CPUMAP_BATCH_ENTRY *BatchEntry = &Batch->Entries[i];
        NET_BUFFER_LIST *Original = BatchEntry->OriginalNbl;

        Clones[i] = NULL;
        Processed[i] = TRUE;
        IsDeepCopyBatch[i] = FALSE;

        if (Batch->CanPend) {
            //
            // Use original miniport NBL directly.
            //
            Clones[i] = Original;
            Processed[i] = FALSE;
            continue;
        }

        //
        // !CanPend deep-copy fallback.
        // Allocate bare NBL+NB from SList or pool, retreat to get pages,
        // copy data.  Result is fully independent of original.
        //
            {
                NET_BUFFER_LIST *DeepCopy = NULL;
                PSLIST_ENTRY Sle = InterlockedPopEntrySList(&CpuMap->DeepCopyFreeList);

                if (Sle != NULL) {
                    DeepCopy = CONTAINING_RECORD(Sle, NET_BUFFER_LIST, Next);
                    InterlockedIncrement(&CpuMap->DeepCopyHitCount);
                } else {
                    DeepCopy = NdisAllocateNetBufferAndNetBufferList(
                        CpuMap->DeepCopyNblPool, 0, 0, NULL, 0, 0);
                    if (DeepCopy != NULL) {
                        InterlockedIncrement(&CpuMap->DeepCopyAllocCount);
                        InterlockedIncrement(&CpuMap->DeepCopyMissCount);
                    }
                }

                if (DeepCopy == NULL) {
                    InterlockedIncrement(&CpuMap->DeepCopyFailCount);
                    //
                    // Deep-copy alloc failed.  The original is not enqueued.
                    // recv.c already adds the original to DropList when
                    // !CanPend, so no action needed here.
                    //
                    continue;
                }

                //
                // Clear stale NB fields from previous use (recycled NBL).
                //
                {
                    NET_BUFFER *DstNb = NET_BUFFER_LIST_FIRST_NB(DeepCopy);
                    DstNb->MdlChain = NULL;
                    DstNb->CurrentMdl = NULL;
                    DstNb->DataLength = 0;
                    DstNb->DataOffset = 0;
                    DstNb->CurrentMdlOffset = 0;
                }

                //
                // Retreat: NDIS allocates MDL + physical pages.
                //
                {
                    NET_BUFFER *SrcNb = NET_BUFFER_LIST_FIRST_NB(Original);
                    ULONG DataLen = NET_BUFFER_DATA_LENGTH(SrcNb);
                    NET_BUFFER *DstNb = NET_BUFFER_LIST_FIRST_NB(DeepCopy);

                    NDIS_STATUS NdisStatus =
                        NdisRetreatNetBufferDataStart(DstNb, DataLen, 0, NULL);

                    if (NdisStatus != NDIS_STATUS_SUCCESS) {
                        //
                        // Page alloc failed.  Return bare NBL to free list.
                        // Original is not enqueued; recv.c handles return
                        // to miniport via DropList when !CanPend.
                        //
                        InterlockedPushEntrySList(
                            &CpuMap->DeepCopyFreeList, (PSLIST_ENTRY)&DeepCopy->Next);
                        InterlockedIncrement(&CpuMap->DeepCopyFailCount);
                        continue;
                    }

                    //
                    // Non-temporal MDL-to-MDL data copy.
                    // NdisRetreatNetBufferDataStart populates CurrentMdl on success.
                    //
#pragma prefast(suppress:6387, "NdisRetreatNetBufferDataStart sets CurrentMdl on success.")
                    NT_VERIFY(NT_SUCCESS(
                        MdlCopyMdlChainToMdlChainAtOffsetNonTemporal(
                            DstNb->CurrentMdl, DstNb->CurrentMdlOffset,
                            SrcNb->CurrentMdl, SrcNb->CurrentMdlOffset,
                            DataLen)));

                    //
                    // Preserve RSS hash metadata.
                    //
                    NET_BUFFER_LIST_SET_HASH_VALUE(DeepCopy,
                        NET_BUFFER_LIST_GET_HASH_VALUE(Original));
                    NET_BUFFER_LIST_SET_HASH_TYPE(DeepCopy,
                        NET_BUFFER_LIST_GET_HASH_TYPE(Original));
                }

                Clones[i] = DeepCopy;
                IsDeepCopyBatch[i] = TRUE;
                Processed[i] = FALSE;

                //
                // Original NBL goes back to miniport via DropList.
                // (recv.c already adds to DropList when !CanPend.)
                //
            }
    }

    //
    // Phase 2: Group by target CPU and enqueue with one lock acquisition
    // per target. With N <= 32, the O(N*T) scan is trivial at DISPATCH_LEVEL.
    //
    SourceCpu = KeGetCurrentProcessorIndex();
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
            // Invalid target — free the clone/deep-copy or return original.
            //
            if (IsDeepCopyBatch[i]) {
                //
                // Deep-copy: advance frees pages, push bare NBL to SList.
                // IsDeepCopyBatch[i]==TRUE implies Clones[i] was assigned
                // non-NULL DeepCopy in Phase 1 (NULL path hits continue).
                //
#pragma prefast(suppress:28182, "IsDeepCopyBatch[i] true implies Clones[i] is non-NULL.")
                NET_BUFFER *Nb = NET_BUFFER_LIST_FIRST_NB(Clones[i]);
                NdisAdvanceNetBufferDataStart(Nb, Nb->DataLength, TRUE, NULL);
                InterlockedPushEntrySList(
                    &CpuMap->DeepCopyFreeList, (PSLIST_ENTRY)&Clones[i]->Next);
            } else {
                NET_BUFFER_LIST_NEXT_NBL(Clones[i]) = Batch->ReturnableOriginals;
                Batch->ReturnableOriginals = Clones[i];
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
                // Ring full — recycle deep-copy or return original.
                //
                if (IsDeepCopyBatch[j]) {
                    // IsDeepCopyBatch[j]==TRUE implies Clones[j] is non-NULL (same
                    // invariant as the invalid-target path above for index j).
#pragma prefast(suppress:28182, "IsDeepCopyBatch[j] true implies Clones[j] is non-NULL.")
                    NET_BUFFER *Nb = NET_BUFFER_LIST_FIRST_NB(Clones[j]);
                    NdisAdvanceNetBufferDataStart(Nb, Nb->DataLength, TRUE, NULL);
                    InterlockedPushEntrySList(
                        &CpuMap->DeepCopyFreeList, (PSLIST_ENTRY)&Clones[j]->Next);
                } else {
                    NET_BUFFER_LIST_NEXT_NBL(Clones[j]) = Batch->ReturnableOriginals;
                    Batch->ReturnableOriginals = Clones[j];
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
            RingEntry->IsDeepCopy = IsDeepCopyBatch[j];

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
    BOOLEAN IsDeepCopyDpc[XDP_CPUMAP_MAX_BATCH_SIZE];
    BOOLEAN ShouldYield = FALSE;
    UINT32 LoopIter = 0;

    if (Ring == NULL) {
        return;
    }

    InterlockedIncrement(&Ring->DpcInvokeCount);

    //
    // Yield-aware batched drain: loop inside the DPC, draining up to
    // DrainBatchSize per iteration. After each batch, check
    // KeShouldYieldProcessor() to avoid DPC watchdog timeout.
    // If the OS says yield, re-queue the DPC and return — remaining
    // packets stay in the ring.
    //
    do {
        LoopIter++;

        BatchHead = NULL;
        BatchTail = NULL;
        Count = 0;
        FilterHandle = NULL;
        PortNumber = 0;

        //
        // Dequeue batch under lock.
        //
        {
            UINT64 LockStart = __rdtsc();
            KeAcquireInStackQueuedSpinLock(&Ring->Lock, &LockHandle);
            InterlockedAdd64(&Ring->LockWaitCycles, (LONG64)(__rdtsc() - LockStart));
            InterlockedIncrement(&Ring->LockAcquireCount);
        }

        while (Ring->Head != Ring->Tail && Count < Ring->DrainBatchSize) {
            XDP_CPUMAP_ENTRY *Entry = &Ring->Entries[Ring->Head & Ring->Mask];

            FilterHandle = Entry->FilterHandle;
            PortNumber = Entry->PortNumber;

            NET_BUFFER_LIST_NEXT_NBL(Entry->Nbl) = NULL;
            if (BatchTail != NULL) {
                NET_BUFFER_LIST_NEXT_NBL(BatchTail) = Entry->Nbl;
            } else {
                BatchHead = Entry->Nbl;
            }
            BatchTail = Entry->Nbl;

            IsDeepCopyDpc[Count] = Entry->IsDeepCopy;

            Ring->Head++;
            Count++;
        }

        MoreWork = (Ring->Head != Ring->Tail);
        if (Count > 0) {
            InterlockedAdd(&Ring->DrainCount, Count);
            if ((LONG)Count > Ring->DpcMaxBatchDrained) {
                Ring->DpcMaxBatchDrained = (LONG)Count;
            }
        } else if (!MoreWork) {
            InterlockedIncrement(&Ring->DpcEmptyCount);
        }

        KeReleaseInStackQueuedSpinLock(&LockHandle);

        //
        // Split into orig/dc chains and indicate to NDIS.
        //
        if (BatchHead != NULL && FilterHandle != NULL) {
            NET_BUFFER_LIST *OrigHead = NULL, **OrigTailPtr = &OrigHead;
            NET_BUFFER_LIST *DcHead = NULL, **DcTailPtr = &DcHead;
            UINT32 OrigCount = 0, DcCount = 0;
            NET_BUFFER_LIST *Cur = BatchHead;
            UINT32 SplitIdx = 0;

            while (Cur != NULL) {
                NET_BUFFER_LIST *Next = NET_BUFFER_LIST_NEXT_NBL(Cur);
                NET_BUFFER_LIST_NEXT_NBL(Cur) = NULL;

                if (IsDeepCopyDpc[SplitIdx]) {
                    *DcTailPtr = Cur;
                    DcTailPtr = &NET_BUFFER_LIST_NEXT_NBL(Cur);
                    DcCount++;
                } else {
                    *OrigTailPtr = Cur;
                    OrigTailPtr = &NET_BUFFER_LIST_NEXT_NBL(Cur);
                    OrigCount++;
                }
                SplitIdx++;
                Cur = Next;
            }

            //
            // Indicate originals without RESOURCES (async return to miniport).
            //
            if (OrigHead != NULL) {
                NdisFIndicateReceiveNetBufferLists(
                    FilterHandle, OrigHead, PortNumber, OrigCount,
                    NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL);
                InterlockedAdd(&Ring->OwnerMap->AbsoluteZeroCopyIndicateCount, OrigCount);
            }

            //
            // Indicate deep-copies with RESOURCES (synchronous return), then recycle.
            //
            if (DcHead != NULL) {
                NdisFIndicateReceiveNetBufferLists(
                    FilterHandle, DcHead, PortNumber, DcCount,
                    NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL | NDIS_RECEIVE_FLAGS_RESOURCES);
                InterlockedAdd(&Ring->OwnerMap->DeepCopyIndicateCount, DcCount);

                //
                // Recycle: free data pages, push bare NBL to SList.
                //
                {
                    NET_BUFFER_LIST *DcCur = DcHead;
                    while (DcCur != NULL) {
                        NET_BUFFER_LIST *DcNext = NET_BUFFER_LIST_NEXT_NBL(DcCur);
                        NET_BUFFER *Nb = NET_BUFFER_LIST_FIRST_NB(DcCur);
                        NdisAdvanceNetBufferDataStart(Nb, Nb->DataLength, TRUE, NULL);
                        InterlockedPushEntrySList(
                            &Ring->OwnerMap->DeepCopyFreeList,
                            (PSLIST_ENTRY)&DcCur->Next);
                        DcCur = DcNext;
                    }
                }
            }
        }

        MoreWork = (Ring->Head != Ring->Tail);
        if (MoreWork) {
            InterlockedIncrement(&Ring->DpcRequeueCount);
            if (KeShouldYieldProcessor()) {
                ShouldYield = TRUE;
            }
        }
    } while (MoreWork && !ShouldYield);

    if (ShouldYield) {
        InterlockedIncrement(&Ring->DpcYieldCount);
        KeInsertQueueDpc(Dpc, NULL, NULL);
    }

    InterlockedAdd(&Ring->DpcLoopIterations, (LONG)LoopIter);
    if ((LONG)LoopIter > Ring->DpcMaxLoopIterations) {
        Ring->DpcMaxLoopIterations = (LONG)LoopIter;
    }
}
