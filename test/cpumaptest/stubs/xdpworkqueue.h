//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// Work queue stub.
//
// This DEFERS rather than running inline. The property under test is that
// arming a sweep is decoupled from performing it -- postprocess_map_delete_element
// records a pending release at DISPATCH_LEVEL and decides nothing -- so a stub
// that ran the routine inline from XdpInsertWorkQueue would quietly convert the
// deferred design into a synchronous one and validate the wrong thing.
//
// Instead, queued entries accumulate and the test drives them with
// XdpCpuMapTestRunWorkQueue, which lets a test observe the state BETWEEN arming
// and sweeping, and lets multiple maps coalesce into a single list exactly as
// the real queue does.
//

#pragma once

typedef
VOID
XDP_WORK_QUEUE_ROUTINE(
    _In_ SINGLE_LIST_ENTRY *WorkQueueHead
    );

typedef struct _XDP_WORK_QUEUE XDP_WORK_QUEUE;

XDP_WORK_QUEUE *
XdpCreateWorkQueue(
    _In_ XDP_WORK_QUEUE_ROUTINE *WorkQueueRoutine,
    _In_ KIRQL MaxIrql,
    _In_opt_ DRIVER_OBJECT *DriverObject,
    _In_opt_ DEVICE_OBJECT *DeviceObject
    );

VOID
XdpShutdownWorkQueue(
    _In_ XDP_WORK_QUEUE *WorkQueue,
    _In_ BOOLEAN Wait
    );

VOID
XdpSetWorkQueuePriority(
    _In_ XDP_WORK_QUEUE *WorkQueue,
    _In_ WORK_QUEUE_TYPE Priority
    );

VOID
XdpInsertWorkQueue(
    _In_ XDP_WORK_QUEUE *WorkQueue,
    _In_ SINGLE_LIST_ENTRY *WorkQueueEntry
    );

//
// Test control.
//

//
// Runs one dispatch of the queue, handing the routine every entry queued since
// the last run as a single list -- the coalescing behaviour of the real queue,
// which is what makes the "cache Entry->Next before processing" requirement in
// the sweep worker load-bearing.
//
// Returns the number of entries that were dispatched.
//
UINT32
XdpCpuMapTestRunWorkQueue(
    VOID
    );

//
// Number of entries currently queued and not yet dispatched.
//
UINT32
XdpCpuMapTestPendingWorkQueueEntries(
    VOID
    );
