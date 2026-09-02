//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// User-mode stubs for the kernel primitives src/xdp/cpumap.c depends on, so the
// CPUMAP control plane can be compiled and exercised as an ordinary executable.
// This follows the established pattern in test/pktfuzz, which compiles
// src/xdp/programinspect.c the same way.
//
// The stubs are deliberately BEHAVIOURAL, not empty. The control-plane defects
// worth catching here are accounting and validation defects, and an empty stub
// that always succeeds would hide exactly those. So:
//
//   * allocations can be made to fail on demand, which is what makes the
//     resource-error path reachable;
//   * every allocation is counted, so a leaked ring, DPC or target shell fails
//     the test rather than passing silently;
//   * the processor set is a fixed synthetic machine, so "invalid CPU index" is
//     a deterministic input rather than a property of the build agent;
//   * the work queue DEFERS, exactly as the real one does. Running the sweep
//     inline from XdpInsertWorkQueue would destroy the very property under test
//     -- that arming is decoupled from sweeping -- so the test drives it.
//
// What these stubs CANNOT model is concurrency: they are single-threaded, so
// they exercise ordering and accounting, not races. Lock state is tracked and
// asserted, which catches ordering violations and unbalanced acquire/release,
// but a genuine race needs the kernel.
//

#pragma once

#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL              ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_SUPPORTED             ((NTSTATUS)0xC00000BBL)
#define STATUS_INVALID_PARAMETER         ((NTSTATUS)0xC000000DL)
#define STATUS_NO_MEMORY                 ((NTSTATUS)0xC0000017L)
#define STATUS_INSUFFICIENT_RESOURCES    ((NTSTATUS)0xC000009AL)
#define STATUS_DELETE_PENDING            ((NTSTATUS)0xC0000056L)
#define STATUS_INVALID_DEVICE_STATE      ((NTSTATUS)0xC0000184L)
#define STATUS_TOO_MANY_NODES            ((NTSTATUS)0xC000020EL)

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

#define PASSIVE_LEVEL 0
#define APC_LEVEL     1
#define DISPATCH_LEVEL 2

#define IO_NO_INCREMENT 0

typedef UCHAR KIRQL;
typedef CCHAR KPROCESSOR_MODE;

typedef enum {
    KernelMode,
    UserMode,
    MaximumMode
} MODE;

typedef enum {
    Executive,
    KernelApcWait
} KWAIT_REASON;

typedef enum {
    NotificationEvent,
    SynchronizationEvent
} EVENT_TYPE;

typedef enum {
    CriticalWorkQueue,
    DelayedWorkQueue
} WORK_QUEUE_TYPE;

typedef enum {
    PagedPool,
    NonPagedPoolNx,
    NonPagedPoolNxCacheAligned,
} POOL_TYPE;

typedef struct _DRIVER_OBJECT DRIVER_OBJECT;
typedef struct _DEVICE_OBJECT DEVICE_OBJECT;

//
// NDIS types.
//
// NET_BUFFER_LIST is modelled only as far as CPUMAP touches it: the data path
// chains NBLs through Next when it builds indication partitions, so Next has to
// be real. Everything else about an NBL is NDIS-internal and CPUMAP never reads
// it.
//
typedef VOID *NDIS_HANDLE;
typedef ULONG NDIS_PORT_NUMBER;

typedef struct _NET_BUFFER_LIST {
    struct _NET_BUFFER_LIST *Next;
} NET_BUFFER_LIST;

#define NET_BUFFER_LIST_NEXT_NBL(_NBL) ((_NBL)->Next)

#define NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL 0x00000001
#define NDIS_RECEIVE_FLAGS_RESOURCES      0x00000002
#define NDIS_RETURN_FLAGS_DISPATCH_LEVEL  0x00000001

//
// Every indication and every return is recorded, because partitioned indication
// (design section 7) is a property of HOW MANY calls are made and with WHICH
// handle, port, flags and count -- not of the NBLs themselves. A harness that
// only counted NBLs could not tell a correctly partitioned drain from the POC
// defect that indicates one merged chain with the last entry's handle and port.
//

#define XDP_CPUMAP_TEST_MAX_INDICATIONS 64

//
// Bounds the per-call identity snapshot below. A drain partition cannot exceed
// one ring's worth of entries, and the tests that assert identity use rings far
// smaller than this.
//
#define XDP_CPUMAP_TEST_MAX_NBLS_PER_CALL 128

typedef struct _XDP_CPUMAP_TEST_INDICATION {
    NDIS_HANDLE FilterHandle;
    NDIS_PORT_NUMBER PortNumber;
    ULONG NblCount;
    ULONG Flags;
    NET_BUFFER_LIST *Head;

    //
    // The chain as it was AT CALL TIME, by identity.
    //
    // Head alone is not sufficient evidence. Once a chain is handed to NDIS the
    // callee owns it and may relink it, and in this harness the NBLs are
    // caller-owned storage that later production steps -- ChainSetTake's
    // termination, a subsequent commit reusing the same NBL -- legitimately
    // rewrite. Walking Head->Next after the fact therefore reports what the
    // chain looks like NOW, not what was passed, so a mutation that corrupts
    // linkage can appear to have passed a different set than it did. Snapshot
    // the pointers while the call is on the stack.
    //
    ULONG NblSnapshotCount;
    BOOLEAN NblSnapshotTruncated;
    NET_BUFFER_LIST *NblSnapshot[XDP_CPUMAP_TEST_MAX_NBLS_PER_CALL];
} XDP_CPUMAP_TEST_INDICATION;

extern XDP_CPUMAP_TEST_INDICATION XdpCpuMapTestIndications[XDP_CPUMAP_TEST_MAX_INDICATIONS];
extern ULONG XdpCpuMapTestIndicationCount;
extern XDP_CPUMAP_TEST_INDICATION XdpCpuMapTestReturns[XDP_CPUMAP_TEST_MAX_INDICATIONS];
extern ULONG XdpCpuMapTestReturnCount;

VOID
XdpCpuMapTestResetNdis(
    VOID
    );

VOID
XdpCpuMapTestRecordNdisCall(
    _Inout_ XDP_CPUMAP_TEST_INDICATION *Log,
    _Inout_ ULONG *LogCount,
    _In_ NDIS_HANDLE FilterHandle,
    _In_ NET_BUFFER_LIST *NblChain,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG NblCount,
    _In_ ULONG Flags
    );

FORCEINLINE
VOID
NdisFIndicateReceiveNetBufferLists(
    _In_ NDIS_HANDLE FilterHandle,
    _In_ NET_BUFFER_LIST *NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG NumberOfNetBufferLists,
    _In_ ULONG ReceiveFlags
    )
{
    XdpCpuMapTestRecordNdisCall(
        XdpCpuMapTestIndications, &XdpCpuMapTestIndicationCount, FilterHandle,
        NetBufferLists, PortNumber, NumberOfNetBufferLists, ReceiveFlags);
}

FORCEINLINE
VOID
NdisFReturnNetBufferLists(
    _In_ NDIS_HANDLE FilterHandle,
    _In_ NET_BUFFER_LIST *NetBufferLists,
    _In_ ULONG ReturnFlags
    )
{
    ULONG NblCount = 0;

    for (NET_BUFFER_LIST *Nbl = NetBufferLists; Nbl != NULL; Nbl = Nbl->Next) {
        NblCount++;
    }

    XdpCpuMapTestRecordNdisCall(
        XdpCpuMapTestReturns, &XdpCpuMapTestReturnCount, FilterHandle, NetBufferLists,
        0, NblCount, ReturnFlags);
}

VOID
XdpCpuMapTestAssert(
    _In_ BOOLEAN Condition,
    _In_z_ const CHAR *Expression,
    _In_z_ const CHAR *File,
    _In_ int Line
    );

#define XDPCPUMAP_TEST_ASSERT(expr) \
    XdpCpuMapTestAssert((BOOLEAN)!!(expr), #expr, __FILE__, __LINE__)

//
// Allocation accounting and fault injection.
//

extern LONG XdpCpuMapTestLiveAllocations;
extern LONG XdpCpuMapTestFailAllocationsAfter;
extern ULONG XdpCpuMapTestCurrentProcessorIndex;

VOID
XdpCpuMapTestResetAllocator(
    VOID
    );

VOID *
XdpCpuMapTestAllocate(
    _In_ SIZE_T NumberOfBytes
    );

VOID
XdpCpuMapTestFree(
    _In_opt_ VOID *P
    );

#define ExAllocatePoolZero(PoolType, NumberOfBytes, Tag) \
    (UNREFERENCED_PARAMETER(PoolType), UNREFERENCED_PARAMETER(Tag), \
     XdpCpuMapTestAllocate(NumberOfBytes))

#define ExFreePoolWithTag(P, Tag) \
    (UNREFERENCED_PARAMETER(Tag), XdpCpuMapTestFree(P))

//
// Synthetic processor topology. Fixed so that "CPU 8 does not exist" is a
// deterministic input on any build agent.
//
// MAXIMUM deliberately exceeds ACTIVE, as it does on any machine with processor
// hot-add capacity. That gap is not cosmetic: the target table is sized by the
// MAXIMUM count, so an index in [ACTIVE, MAXIMUM) passes the table-bounds check
// and yet names no processor. It is reachable only through
// KeGetProcessorNumberFromIndex, which is exactly why that check must run on
// every update. A stub with maximum == active would let the bounds check mask
// the missing validation and the test would prove nothing.
//

#define XDP_CPUMAP_TEST_MAX_PROCESSOR_COUNT 16
#define XDP_CPUMAP_TEST_PROCESSOR_COUNT 8

#ifndef ALL_PROCESSOR_GROUPS
#define ALL_PROCESSOR_GROUPS 0xffff
#endif

FORCEINLINE
ULONG
KeGetCurrentProcessorIndex(
    VOID
    )
{
    return XdpCpuMapTestCurrentProcessorIndex;
}

FORCEINLINE
ULONG
KeQueryMaximumProcessorCountEx(
    _In_ USHORT GroupNumber
    )
{
    UNREFERENCED_PARAMETER(GroupNumber);
    return XDP_CPUMAP_TEST_MAX_PROCESSOR_COUNT;
}

FORCEINLINE
ULONG
KeQueryActiveProcessorCountEx(
    _In_ USHORT GroupNumber
    )
{
    UNREFERENCED_PARAMETER(GroupNumber);
    return XDP_CPUMAP_TEST_PROCESSOR_COUNT;
}

FORCEINLINE
NTSTATUS
KeGetProcessorNumberFromIndex(
    _In_ ULONG ProcIndex,
    _Out_ PROCESSOR_NUMBER *ProcNumber
    )
{
    if (ProcIndex >= XDP_CPUMAP_TEST_PROCESSOR_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    ProcNumber->Group = 0;
    ProcNumber->Number = (UCHAR)ProcIndex;
    ProcNumber->Reserved = 0;
    return STATUS_SUCCESS;
}

//
// DPC.
//
// The queue is modelled, not stubbed away. A DPC that is queued, cancelled,
// flushed, or re-queued by its own routine is exactly the state machine design
// section 7 makes safe with the rundown gate, and an empty stub would let a
// broken gate pass.
//

typedef struct _KDPC KDPC;

typedef
VOID
KDEFERRED_ROUTINE(
    _In_ KDPC *Dpc,
    _In_opt_ VOID *DeferredContext,
    _In_opt_ VOID *SystemArgument1,
    _In_opt_ VOID *SystemArgument2
    );

struct _KDPC {
    KDEFERRED_ROUTINE *Routine;
    VOID *Context;
    PROCESSOR_NUMBER Target;
    BOOLEAN Targeted;
    BOOLEAN Queued;
};

#define XDP_CPUMAP_TEST_MAX_QUEUED_DPCS 16

extern KDPC *XdpCpuMapTestQueuedDpcs[XDP_CPUMAP_TEST_MAX_QUEUED_DPCS];
extern ULONG XdpCpuMapTestQueuedDpcCount;
extern ULONG XdpCpuMapTestDpcInsertCalls;
extern ULONG XdpCpuMapTestDpcRunCount;

//
// The lowest Target->PacketRundown count observed at the instant
// KeInsertQueueDpc was called, over every insert since the last reset.
//
// Design section 7 orders step 5 (KeInsertQueueDpc) BEFORE step 6 (release the
// group's target rundown references), and that ordering is the entire reason
// step 5 needs no separate producer reference: while the group still holds one,
// the retire path's rundown wait cannot have completed and the DPC cannot have
// been freed. Nothing about the resulting state records the order, so it has to
// be sampled at the call.
//
extern LONG XdpCpuMapTestMinRundownAtDpcInsert;

//
// Set nonzero to make KeShouldYieldProcessor report that the DPC must yield, so
// the bounded-drain yield/requeue path and its rundown gate are reachable.
//
extern BOOLEAN XdpCpuMapTestShouldYield;

VOID
XdpCpuMapTestResetDpcs(
    VOID
    );

BOOLEAN
XdpCpuMapTestInsertQueueDpc(
    _Inout_ KDPC *Dpc
    );

BOOLEAN
XdpCpuMapTestRemoveQueueDpc(
    _Inout_ KDPC *Dpc
    );

//
// Runs every queued DPC to completion, bounded. A DPC that re-queues itself is
// run again, which is what the real KeFlushQueuedDpcs ends up doing and is the
// only way the self-requeue path is observable here.
//
VOID
XdpCpuMapTestRunQueuedDpcs(
    VOID
    );

//
// Set nonzero to make the next KeSetTargetProcessorDpcEx fail. Without this the
// "DPC targeting failed" unwind path is unreachable, and an unreachable unwind
// path is where leaks live.
//
extern BOOLEAN XdpCpuMapTestFailDpcTargeting;

FORCEINLINE
VOID
KeInitializeDpc(
    _Out_ KDPC *Dpc,
    _In_ KDEFERRED_ROUTINE *Routine,
    _In_opt_ VOID *Context
    )
{
    Dpc->Routine = Routine;
    Dpc->Context = Context;
    Dpc->Targeted = FALSE;
    Dpc->Queued = FALSE;
}

FORCEINLINE
NTSTATUS
KeSetTargetProcessorDpcEx(
    _Inout_ KDPC *Dpc,
    _In_ PROCESSOR_NUMBER *ProcNumber
    )
{
    if (XdpCpuMapTestFailDpcTargeting) {
        return STATUS_INVALID_PARAMETER;
    }

    Dpc->Target = *ProcNumber;
    Dpc->Targeted = TRUE;
    return STATUS_SUCCESS;
}

FORCEINLINE
BOOLEAN
KeInsertQueueDpc(
    _Inout_ KDPC *Dpc,
    _In_opt_ VOID *SystemArgument1,
    _In_opt_ VOID *SystemArgument2
    )
{
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    return XdpCpuMapTestInsertQueueDpc(Dpc);
}

FORCEINLINE
BOOLEAN
KeRemoveQueueDpc(
    _Inout_ KDPC *Dpc
    )
{
    return XdpCpuMapTestRemoveQueueDpc(Dpc);
}

FORCEINLINE
VOID
KeFlushQueuedDpcs(
    VOID
    )
{
    XdpCpuMapTestRunQueuedDpcs();
}

FORCEINLINE
BOOLEAN
KeShouldYieldProcessor(
    VOID
    )
{
    return XdpCpuMapTestShouldYield;
}

//
// Spin locks. Single-threaded, so these only assert that acquire and release
// are balanced and never recursive.
//
// N.B. KSPIN_LOCK is already a ULONG_PTR in user-mode winnt.h, so it is used
// directly rather than redefined.
//

typedef struct _KLOCK_QUEUE_HANDLE {
    KSPIN_LOCK *Lock;
} KLOCK_QUEUE_HANDLE;

FORCEINLINE
VOID
KeInitializeSpinLock(
    _Out_ KSPIN_LOCK *SpinLock
    )
{
    *SpinLock = 0;
}

FORCEINLINE
VOID
KeAcquireInStackQueuedSpinLock(
    _Inout_ KSPIN_LOCK *SpinLock,
    _Out_ KLOCK_QUEUE_HANDLE *Handle
    )
{
    XDPCPUMAP_TEST_ASSERT(*SpinLock == 0);
    *SpinLock = 1;
    Handle->Lock = SpinLock;
}

FORCEINLINE
VOID
KeReleaseInStackQueuedSpinLock(
    _In_ KLOCK_QUEUE_HANDLE *Handle
    )
{
    XDPCPUMAP_TEST_ASSERT(*Handle->Lock == 1);
    *Handle->Lock = 0;
}

//
// Push locks. Held state is tracked so the lock ordering in design section 8.3
// can be asserted from the test rather than assumed.
//

typedef struct _EX_PUSH_LOCK {
    LONG Exclusive;
    LONG Shared;
} EX_PUSH_LOCK;

FORCEINLINE
VOID
ExInitializePushLock(
    _Out_ EX_PUSH_LOCK *PushLock
    )
{
    PushLock->Exclusive = 0;
    PushLock->Shared = 0;
}

FORCEINLINE
VOID
RtlAcquirePushLockExclusive(
    _Inout_ EX_PUSH_LOCK *PushLock
    )
{
    XDPCPUMAP_TEST_ASSERT(PushLock->Exclusive == 0 && PushLock->Shared == 0);
    PushLock->Exclusive = 1;
}

FORCEINLINE
VOID
RtlReleasePushLockExclusive(
    _Inout_ EX_PUSH_LOCK *PushLock
    )
{
    XDPCPUMAP_TEST_ASSERT(PushLock->Exclusive == 1);
    PushLock->Exclusive = 0;
}

FORCEINLINE
VOID
RtlAcquirePushLockShared(
    _Inout_ EX_PUSH_LOCK *PushLock
    )
{
    XDPCPUMAP_TEST_ASSERT(PushLock->Exclusive == 0);
    PushLock->Shared++;
}

FORCEINLINE
VOID
RtlReleasePushLockShared(
    _Inout_ EX_PUSH_LOCK *PushLock
    )
{
    XDPCPUMAP_TEST_ASSERT(PushLock->Shared > 0);
    PushLock->Shared--;
}

//
// Rundown protection.
//
// Modelled faithfully enough to matter: acquire fails once rundown is active,
// and waiting for release asserts the count has already reached zero. In a
// single-threaded harness a nonzero count at wait time is a guaranteed deadlock
// in the kernel, so asserting is strictly better than hanging.
//
// CALLS are counted, not just references, and they are counted PER OBJECT.
// Reference totals cannot distinguish a batched flush group from per-packet
// acquisition -- both end at the same count -- so the number of trips to a given
// shared rundown is the only observable that proves batching. Counting them
// globally was not enough once the data path landed: a target's PacketRundown
// and a receive queue's NblRundown are both released in batches now, and a
// global counter cannot tell which object a call went to. Per-object counters
// let each test name the rundown whose call count carries its claim.
//
// AcquireCalls counts ATTEMPTS, so a failed acquire against an active rundown
// still increments it. ExInitializeRundownProtection zeroes all four.
//

typedef struct _EX_RUNDOWN_REF {
    LONG Count;
    BOOLEAN RundownActive;
    ULONG AcquireCalls;
    ULONG ReleaseCalls;
    ULONG AcquireExCalls;
    ULONG ReleaseExCalls;
} EX_RUNDOWN_REF;

FORCEINLINE
VOID
ExInitializeRundownProtection(
    _Out_ EX_RUNDOWN_REF *RunRef
    )
{
    RunRef->Count = 0;
    RunRef->RundownActive = FALSE;
    RunRef->AcquireCalls = 0;
    RunRef->ReleaseCalls = 0;
    RunRef->AcquireExCalls = 0;
    RunRef->ReleaseExCalls = 0;
}

FORCEINLINE
BOOLEAN
ExAcquireRundownProtection(
    _Inout_ EX_RUNDOWN_REF *RunRef
    )
{
    RunRef->AcquireCalls++;

    if (RunRef->RundownActive) {
        return FALSE;
    }

    RunRef->Count++;
    return TRUE;
}

FORCEINLINE
BOOLEAN
ExAcquireRundownProtectionEx(
    _Inout_ EX_RUNDOWN_REF *RunRef,
    _In_ ULONG Count
    )
{
    RunRef->AcquireExCalls++;

    if (RunRef->RundownActive) {
        return FALSE;
    }

    RunRef->Count += Count;
    return TRUE;
}

FORCEINLINE
VOID
ExReleaseRundownProtection(
    _Inout_ EX_RUNDOWN_REF *RunRef
    )
{
    RunRef->ReleaseCalls++;

    XDPCPUMAP_TEST_ASSERT(RunRef->Count > 0);
    RunRef->Count--;
}

FORCEINLINE
VOID
ExReleaseRundownProtectionEx(
    _Inout_ EX_RUNDOWN_REF *RunRef,
    _In_ ULONG Count
    )
{
    RunRef->ReleaseExCalls++;

    XDPCPUMAP_TEST_ASSERT(RunRef->Count >= (LONG)Count);
    RunRef->Count -= Count;
}

FORCEINLINE
VOID
ExWaitForRundownProtectionRelease(
    _Inout_ EX_RUNDOWN_REF *RunRef
    )
{
    RunRef->RundownActive = TRUE;
    XDPCPUMAP_TEST_ASSERT(RunRef->Count == 0);
}

//
// Events.
//

typedef struct _KEVENT {
    EVENT_TYPE Type;
    LONG Signalled;
} KEVENT;

FORCEINLINE
VOID
KeInitializeEvent(
    _Out_ KEVENT *Event,
    _In_ EVENT_TYPE Type,
    _In_ BOOLEAN State
    )
{
    Event->Type = Type;
    Event->Signalled = State ? 1 : 0;
}

FORCEINLINE
LONG
KeSetEvent(
    _Inout_ KEVENT *Event,
    _In_ LONG Increment,
    _In_ BOOLEAN Wait
    )
{
    LONG Previous = Event->Signalled;

    UNREFERENCED_PARAMETER(Increment);
    UNREFERENCED_PARAMETER(Wait);

    Event->Signalled = 1;
    return Previous;
}

FORCEINLINE
NTSTATUS
KeWaitForSingleObject(
    _Inout_ VOID *Object,
    _In_ KWAIT_REASON WaitReason,
    _In_ KPROCESSOR_MODE WaitMode,
    _In_ BOOLEAN Alertable,
    _In_opt_ LARGE_INTEGER *Timeout
    )
{
    KEVENT *Event = (KEVENT *)Object;

    UNREFERENCED_PARAMETER(WaitReason);
    UNREFERENCED_PARAMETER(WaitMode);
    UNREFERENCED_PARAMETER(Alertable);
    UNREFERENCED_PARAMETER(Timeout);

    //
    // Single-threaded: nothing can signal this later, so an unsignalled event
    // here is a real deadlock in the kernel and must fail the test.
    //
    XDPCPUMAP_TEST_ASSERT(Event->Signalled != 0);

    if (Event->Type == SynchronizationEvent) {
        Event->Signalled = 0;
    }

    return STATUS_SUCCESS;
}

FORCEINLINE
LARGE_INTEGER
KeQueryPerformanceCounter(
    _Out_opt_ LARGE_INTEGER *PerformanceFrequency
    )
{
    LARGE_INTEGER Counter;

    if (PerformanceFrequency != NULL) {
        QueryPerformanceFrequency(PerformanceFrequency);
    }

    QueryPerformanceCounter(&Counter);
    return Counter;
}
