//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

#pragma once

typedef struct _XDP_EBPF_MAP_CONTEXT_OFFSET {
    ULONG64 Offset;
    volatile LONG Published;
} XDP_EBPF_MAP_CONTEXT_OFFSET;

typedef struct _XDP_CPUMAP_REDIRECT_CONTEXT {
    XDP_CPUMAP *CpuMap;
    XDP_CPUMAP_TARGET *CpuMapTarget;
    UINT32 TargetKey;
    UINT32 TargetCpu;
} XDP_CPUMAP_REDIRECT_CONTEXT;

static
FORCEINLINE
VOID
XdpEbpfMapContextOffsetPublish(
    _Inout_ XDP_EBPF_MAP_CONTEXT_OFFSET *ContextOffset,
    _In_ ULONG64 Offset
    )
{
    WriteULong64NoFence(&ContextOffset->Offset, Offset);
    InterlockedExchange(&ContextOffset->Published, TRUE);
}

static
FORCEINLINE
ebpf_result_t
XdpEbpfMapContextResolve(
    _Inout_ XDP_EBPF_MAP_CONTEXT_OFFSET *ContextOffset,
    _In_ const VOID *Map,
    _Outptr_result_maybenull_ XDP_EBPF_MAP_HEADER **Header
    )
{
    *Header = NULL;

    //
    // Publish is write-once: XdpEbpfMapContextOffsetPublish writes Offset, then
    // uses InterlockedExchange to publish this flag. The hot helper path only
    // needs the matching acquire load here before reading Offset below; do not
    // replace this with a locked RMW read, which would make every redirecting
    // CPU acquire this global cache line exclusively on every packet.
    //
    if (ReadULongAcquire((ULONG *)&ContextOffset->Published) == FALSE) {
        return EBPF_OPERATION_NOT_SUPPORTED;
    }

    *Header =
        *(XDP_EBPF_MAP_HEADER **)MAP_CONTEXT(Map, ReadULong64NoFence(&ContextOffset->Offset));
    if (*Header == NULL) {
        return EBPF_OPERATION_NOT_SUPPORTED;
    }

    return EBPF_SUCCESS;
}

//
// Header is the first field of XDP_CPUMAP, so the CONTAINING_RECORD below is a
// type recovery from the provider context pointer rather than a byte read from
// the smaller header object.
//
#pragma warning(push)
#pragma warning(disable:6385)
static
FORCEINLINE
ebpf_result_t
XdpCpuMapGetMapFromContextOffset(
    _Inout_ XDP_EBPF_MAP_CONTEXT_OFFSET *ContextOffset,
    _In_ const VOID *Map,
    _Out_ XDP_EBPF_MAP_TYPE *MapType,
    _Outptr_result_maybenull_ XDP_CPUMAP **CpuMap
    )
{
    XDP_EBPF_MAP_HEADER *Header;

    *MapType = (XDP_EBPF_MAP_TYPE)0;
    *CpuMap = NULL;

    if (XdpEbpfMapContextResolve(ContextOffset, Map, &Header) != EBPF_SUCCESS) {
        return EBPF_OPERATION_NOT_SUPPORTED;
    }
    if (Header == NULL) {
        ASSERT(FALSE);
        return EBPF_OPERATION_NOT_SUPPORTED;
    }

    *MapType = Header->Type;
    if (Header->Type == XdpEbpfMapTypeCpuMap) {
        *CpuMap = CONTAINING_RECORD(Header, XDP_CPUMAP, Header);
    }

    return EBPF_SUCCESS;
}
#pragma warning(pop)

//
// The eBPF base-map callback reports a uint8_t* because values are opaque bytes
// to the runtime. CPUMAP's provider value is the actual object stored in those
// bytes; the cast below is the same pattern XSKMAP uses in its provider path.
//
#pragma warning(push)
#pragma warning(disable:6385)
static
FORCEINLINE
ebpf_result_t
XdpCpuMapFindElementFromBaseMap(
    _In_ const VOID *Map,
    _In_ XDP_CPUMAP *CpuMap,
    _In_ const VOID *Key,
    _Outptr_result_maybenull_ XDP_CPUMAP_PROVIDER_VALUE **Value
    )
{
    ebpf_result_t Result;
    VOID *RawValue = NULL;

    *Value = NULL;

    if (!CpuMap->Active) {
        return EBPF_INVALID_OBJECT;
    }

    Result = CpuMap->ClientDispatch->find_element_function(Map, Key, (uint8_t **)&RawValue);
    if (Result != EBPF_SUCCESS) {
        return Result;
    }

    *Value = (XDP_CPUMAP_PROVIDER_VALUE *)RawValue;
    return EBPF_SUCCESS;
}
#pragma warning(pop)

static
FORCEINLINE
VOID
XdpCpuMapClearRedirectContext(
    _Inout_ XDP_CPUMAP_REDIRECT_CONTEXT *Redirect
    )
{
    if (Redirect->CpuMapTarget != NULL) {
        XdpCpuMapReleaseTargetReference(Redirect->CpuMapTarget);
        Redirect->CpuMapTarget = NULL;
    }

    if (Redirect->CpuMap != NULL) {
        XdpCpuMapReleaseBacking(Redirect->CpuMap);
        Redirect->CpuMap = NULL;
    }

    Redirect->TargetKey = 0;
    Redirect->TargetCpu = 0;
}

static
FORCEINLINE
intptr_t
XdpCpuMapRedirectMap(
    _In_ const VOID *Map,
    _In_ UINT64 Key,
    _In_ intptr_t FallbackAction,
    _In_ BOOLEAN IsProgTestRun,
    _In_ XDP_INTERFACE_MODE InterfaceMode,
    _Inout_ XDP_CPUMAP *CpuMap,
    _Inout_ XDP_CPUMAP_REDIRECT_CONTEXT *Redirect
    )
{
    XDP_CPUMAP_PROVIDER_VALUE *CpuMapValue = NULL;
    ebpf_result_t FindResult;

    XdpCpuMapRecordHelperCall(CpuMap);

    if (IsProgTestRun || InterfaceMode != XDP_INTERFACE_MODE_GENERIC) {
        XdpCpuMapRecordHelperFallback(
            CpuMap, XdpCpuMapHelperFallbackRedirectModeUnsupported);
        return FallbackAction;
    }

    //
    // Key comes straight from the eBPF program and is unconstrained. A CPUMAP
    // key is XDP_CPUMAP_KEY (UINT32), and the base-map lookup below reads only
    // that many bytes, so a key with nonzero upper bits would silently alias the
    // low 32 bits onto a configured slot. Reject it here, BEFORE the lookup and
    // before any reference is acquired.
    //
    // This must not be an assertion. Program input is untrusted: a checked
    // xdp.sys with no kernel debugger attached fail-fasts the machine, which a
    // loaded BPF program could then trigger at will.
    //
    if (Key > MAXUINT32) {
        XdpCpuMapRecordHelperFallback(
            CpuMap, XdpCpuMapHelperFallbackRedirectSlotUnconfigured);
        return FallbackAction;
    }

    FindResult = XdpCpuMapFindElementFromBaseMap(Map, CpuMap, &Key, &CpuMapValue);
    if (FindResult != EBPF_SUCCESS ||
        CpuMapValue == NULL ||
        CpuMapValue->Target == NULL) {
        XdpCpuMapRecordHelperFallback(
            CpuMap,
            FindResult == EBPF_INVALID_OBJECT ?
                XdpCpuMapHelperFallbackTargetInactive :
                XdpCpuMapHelperFallbackRedirectSlotUnconfigured);
        return FallbackAction;
    }

    if (!XdpCpuMapTryAcquireTargetReference(CpuMap, CpuMapValue->Target)) {
        XdpCpuMapRecordHelperFallback(CpuMap, XdpCpuMapHelperFallbackTargetInactive);
        return FallbackAction;
    }

    XdpCpuMapReferenceBacking(CpuMap);
    XdpCpuMapRecordHelperSuccess(CpuMap);
    Redirect->CpuMap = CpuMap;
    Redirect->CpuMapTarget = CpuMapValue->Target;
    Redirect->TargetKey = (UINT32)Key;
    Redirect->TargetCpu = CpuMapValue->Target->AbsoluteCpu;
    return XDP_REDIRECT;
}

NTSTATUS
XdpCpuMapProviderStart(
    VOID
    );

VOID
XdpCpuMapProviderStop(
    VOID
    );

ebpf_result_t
XdpCpuMapGetMap(
    _In_ const VOID *Map,
    _Out_ XDP_EBPF_MAP_TYPE *MapType,
    _Outptr_result_maybenull_ XDP_CPUMAP **CpuMap
    );
