//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

#pragma once

#include <xdpifmode.h>
#include "redirect.h"

typedef struct _XDP_PROGRAM XDP_PROGRAM;
typedef struct _XDP_RX_QUEUE XDP_RX_QUEUE;

typedef ebpf_execution_context_state_t XDP_INSPECTION_EBPF_CONTEXT;

typedef struct _XDP_INSPECTION_CONTEXT {
    XDP_INSPECTION_EBPF_CONTEXT EbpfContext;
    XDP_REDIRECT_CONTEXT RedirectContext;
    ULONG IfIndex;
    UINT32 QueueId;
    XDP_INTERFACE_MODE InterfaceMode;

    //
    // Mirrors XDP_RX_QUEUE::CpuMapRedirectEnabled, which is the single decision
    // XdpProgramCanCpuMapRedirect makes. The helper reads it so that admission
    // and frame-extension registration can never disagree: if the extension is
    // absent the helper declines BEFORE acquiring anything, so a reference is
    // never taken with nothing left to consume it.
    //
    BOOLEAN CpuMapRedirectAllowed;

    LOCK_STATE_EX MapLockState;
} XDP_INSPECTION_CONTEXT;

//
// Data path routines.
//

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
XDP_RX_ACTION
XDP_RX_INSPECT_ROUTINE(
    _In_ XDP_PROGRAM *Program,
    _In_ XDP_INSPECTION_CONTEXT *InspectionContext,
    _In_ XDP_RING *FrameRing,
    _In_ UINT32 FrameIndex,
    _In_opt_ XDP_RING *FragmentRing,
    _In_opt_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _In_opt_ XDP_EXTENSION *CpuMapRedirectExtension
    );

XDP_RX_INSPECT_ROUTINE XdpInspect;
XDP_RX_INSPECT_ROUTINE XdpInspectEbpf;

_IRQL_requires_max_(DISPATCH_LEVEL)
_Success_(return)
BOOLEAN
XdpInspectEbpfStartBatch(
    _In_ XDP_PROGRAM *Program,
    _Inout_ XDP_INSPECTION_CONTEXT *InspectionContext
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpInspectEbpfEndBatch(
    _In_ XDP_PROGRAM *Program,
    _Inout_ XDP_INSPECTION_CONTEXT *InspectionContext
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID *
XdpProgramGetXskBypassTarget(
    _In_ XDP_PROGRAM *Program,
    _In_ XDP_RX_QUEUE *RxQueue
    );

//
// Control path routines.
//

BOOLEAN
XdpProgramIsEbpf(
    _In_ XDP_PROGRAM *Program
    );

BOOLEAN
XdpProgramCanXskBypass(
    _In_ XDP_PROGRAM *Program,
    _In_ XDP_RX_QUEUE *RxQueue
    );

BOOLEAN
XdpProgramCanCpuMapRedirect(
    _In_ XDP_PROGRAM *Program,
    _In_ CONST XDP_HOOK_ID *HookId
    );

XDP_FILE_CREATE_ROUTINE XdpIrpCreateProgram;

NTSTATUS
XdpProgramStart(
    VOID
    );

VOID
XdpProgramStop(
    VOID
    );
