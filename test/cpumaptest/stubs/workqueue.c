//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// Deferring work-queue stub. See stubs/xdpworkqueue.h for why this defers
// rather than running inline.
//

#include "precomp.h"

struct _XDP_WORK_QUEUE {
    XDP_WORK_QUEUE_ROUTINE *Routine;
    SINGLE_LIST_ENTRY Head;
    SINGLE_LIST_ENTRY *Tail;
    UINT32 Count;
    BOOLEAN ShutDown;
};

static XDP_WORK_QUEUE XdpCpuMapTestWorkQueue;

XDP_WORK_QUEUE *
XdpCreateWorkQueue(
    _In_ XDP_WORK_QUEUE_ROUTINE *WorkQueueRoutine,
    _In_ KIRQL MaxIrql,
    _In_opt_ DRIVER_OBJECT *DriverObject,
    _In_opt_ DEVICE_OBJECT *DeviceObject
    )
{
    UNREFERENCED_PARAMETER(MaxIrql);
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(DeviceObject);

    RtlZeroMemory(&XdpCpuMapTestWorkQueue, sizeof(XdpCpuMapTestWorkQueue));
    XdpCpuMapTestWorkQueue.Routine = WorkQueueRoutine;
    XdpCpuMapTestWorkQueue.Head.Next = NULL;
    XdpCpuMapTestWorkQueue.Tail = &XdpCpuMapTestWorkQueue.Head;

    return &XdpCpuMapTestWorkQueue;
}

VOID
XdpShutdownWorkQueue(
    _In_ XDP_WORK_QUEUE *WorkQueue,
    _In_ BOOLEAN Wait
    )
{
    UNREFERENCED_PARAMETER(Wait);

    //
    // Shutdown must not silently discard queued work: an entry still queued here
    // means the module was stopped with a sweep outstanding, which in the driver
    // would be a use-after-free of the map.
    //
    XDPCPUMAP_TEST_ASSERT(WorkQueue->Count == 0);
    WorkQueue->ShutDown = TRUE;
    WorkQueue->Routine = NULL;
}

VOID
XdpSetWorkQueuePriority(
    _In_ XDP_WORK_QUEUE *WorkQueue,
    _In_ WORK_QUEUE_TYPE Priority
    )
{
    UNREFERENCED_PARAMETER(WorkQueue);
    UNREFERENCED_PARAMETER(Priority);
}

VOID
XdpInsertWorkQueue(
    _In_ XDP_WORK_QUEUE *WorkQueue,
    _In_ SINGLE_LIST_ENTRY *WorkQueueEntry
    )
{
    XDPCPUMAP_TEST_ASSERT(!WorkQueue->ShutDown);

    //
    // Append, so dispatch order matches insertion order and a multi-map
    // coalesced list is deterministic.
    //
    WorkQueueEntry->Next = NULL;
    WorkQueue->Tail->Next = WorkQueueEntry;
    WorkQueue->Tail = WorkQueueEntry;
    WorkQueue->Count++;
}

UINT32
XdpCpuMapTestPendingWorkQueueEntries(
    VOID
    )
{
    return XdpCpuMapTestWorkQueue.Count;
}

UINT32
XdpCpuMapTestRunWorkQueue(
    VOID
    )
{
    SINGLE_LIST_ENTRY *List;
    UINT32 Dispatched;

    if (XdpCpuMapTestWorkQueue.Count == 0) {
        return 0;
    }

    //
    // Detach the whole list before dispatching, as the real queue does. The
    // routine may re-arm and insert again during the call, and those insertions
    // must land in the NEXT dispatch, not this one.
    //
    List = XdpCpuMapTestWorkQueue.Head.Next;
    Dispatched = XdpCpuMapTestWorkQueue.Count;

    XdpCpuMapTestWorkQueue.Head.Next = NULL;
    XdpCpuMapTestWorkQueue.Tail = &XdpCpuMapTestWorkQueue.Head;
    XdpCpuMapTestWorkQueue.Count = 0;

    XdpCpuMapTestWorkQueue.Routine(List);

    return Dispatched;
}
