//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// This module implements the BPF_MAP_TYPE_CPUMAP extensible map provider,
// enabling XDP eBPF programs to redirect packets to a chosen CPU via
// bpf_redirect_map().
//
// Modelled on ebpfxskmap.c, which is the working precedent for NMR registration,
// the context-offset pattern, and the version 1 callback set. Like XSKMAP, this
// registers a VERSION 1 provider dispatch table with the same six callbacks and
// requires no dispatch-table change of any kind.
//
// The CPUMAP relies on the eBPF base hash map for key storage. The provider
// value is larger than the public map value: it carries the public
// XDP_CPUMAP_ENTRY_V1 plus the referenced XDP_CPUMAP_TARGET the entry resolved
// to. Putting the referenced object in the value is the XSKMAP pattern, and it
// is what makes retirement correlate correctly -- every delete notification
// carries the exact value its operation produced, so there is no per-key side
// state to race. The runtime copies only the public value_size to user mode, so
// the target pointer is never surfaced.
//
// Increment scope: control plane plus helper lookup/reference acquisition only.
// bpf_redirect_map can resolve a target and take the producer-side rundown
// reference, but the packet data path is a later increment: nothing is enqueued
// and no NBL ownership changes here.
//

#include "precomp.h"
#include "ebpfcpumap.tmh"

typedef struct _XDP_CPUMAP_BINDING_CONTEXT {
    ebpf_base_map_client_dispatch_table_t ClientDispatch;
} XDP_CPUMAP_BINDING_CONTEXT;

//
// CPUMAP provider module ID.
// {6f2f6f27-6c1a-4f27-9a1e-2f0f9c4b8d31}
//
static const NPI_MODULEID EbpfCpuMapProviderModuleId = {
    .Length = sizeof(NPI_MODULEID),
    .Type = MIT_GUID,
    .Guid = {
        0x6f2f6f27,
        0x6c1a,
        0x4f27,
        {0x9a, 0x1e, 0x2f, 0x0f, 0x9c, 0x4b, 0x8d, 0x31}
    },
};

static EBPF_EXTENSION_PROVIDER *EbpfCpuMapProvider;

//
// Offset within the eBPF map structure where the provider context is stored.
// Published during client attach; the data path will use it to resolve a raw
// map pointer to its XDP_EBPF_MAP_HEADER, exactly as XSKMAP does.
//
// The explicit Published flag is separate from the offset value. Offset zero is
// a valid structure offset in general, so zero must not be overloaded to mean
// "the CPUMAP map-provider client never attached successfully".
//
static XDP_EBPF_MAP_CONTEXT_OFFSET XdpCpuMapContextOffset;

static
XDP_CPUMAP_BINDING_CONTEXT *
XdpCpuMapGetBindingContext(
    _In_ void *BindingContext
    )
{
    return (XDP_CPUMAP_BINDING_CONTEXT *)EbpfExtensionClientGetProviderData(
        (const EBPF_EXTENSION_CLIENT *)BindingContext);
}

static
ebpf_result_t
XdpCpuMapPreprocessMapCreate(
    _In_ void *BindingContext,
    uint32_t MapType,
    uint32_t KeySize,
    uint32_t ValueSize,
    uint32_t MaxEntries,
    _Out_ uint32_t *ActualValueSize,
    _Outptr_ void **MapContext
    )
{
    XDP_CPUMAP_BINDING_CONTEXT *Binding = XdpCpuMapGetBindingContext(BindingContext);
    XDP_CPUMAP *CpuMap = NULL;
    ebpf_result_t Result;
    NTSTATUS Status;

    TraceEnter(
        TRACE_CORE, "MapType=%u KeySize=%u ValueSize=%u MaxEntries=%u",
        MapType, KeySize, ValueSize, MaxEntries);

    *ActualValueSize = 0;
    *MapContext = NULL;

    if (MapType != (uint32_t)BPF_MAP_TYPE_CPUMAP) {
        Result = EBPF_OPERATION_NOT_SUPPORTED;
        goto Exit;
    }

    if (KeySize != sizeof(XDP_CPUMAP_KEY) || ValueSize != sizeof(XDP_CPUMAP_ENTRY_V1)) {
        Result = EBPF_INVALID_ARGUMENT;
        goto Exit;
    }

    if (MaxEntries == 0 || MaxEntries > XDP_CPUMAP_MAX_ENTRIES) {
        Result = EBPF_INVALID_ARGUMENT;
        goto Exit;
    }

    //
    // The stored value carries the referenced target alongside the public entry.
    //
    *ActualValueSize = sizeof(XDP_CPUMAP_PROVIDER_VALUE);

    Status = XdpCpuMapCreate(&Binding->ClientDispatch, MaxEntries, &CpuMap);
    if (!NT_SUCCESS(Status)) {
        Result = EBPF_NO_MEMORY;
        goto Exit;
    }

    *MapContext = &CpuMap->Header;
    Result = EBPF_SUCCESS;

Exit:

    TraceExitEbpfResult(TRACE_CORE);
    return Result;
}

static
void
XdpCpuMapPostprocessMapDelete(
    _In_ void *BindingContext,
    _In_ _Post_invalid_ void *MapContext
    )
{
    XDP_CPUMAP *CpuMap = CONTAINING_RECORD(MapContext, XDP_CPUMAP, Header);

    UNREFERENCED_PARAMETER(BindingContext);

    TraceEnter(TRACE_CORE, "MapContext=%p", MapContext);

    //
    // Two shapes reach here. Normal destruction, after the runtime has already
    // delivered the element-delete callback for EVERY stored value; and failed
    // base-map creation, where no value was ever committed. XdpCpuMapDestroy
    // handles both: it waits on the backing refcount, which is zero-extra in the
    // failed-create shape and covers every queued sweep in the normal one.
    //
    // Blocking here is legal: map destruction is epoch-deferred to
    // PASSIVE_LEVEL, not called inline at refcount zero.
    //
    XdpCpuMapDestroy(CpuMap);

    TraceExitSuccess(TRACE_CORE);
}

static
ebpf_result_t
XdpCpuMapPreprocessAssociateProgramType(
    _In_ void *BindingContext,
    _In_ void *MapContext,
    _In_ const ebpf_program_type_t *ProgramType
    )
{
    static const ebpf_program_type_t ExpectedProgramType = EBPF_PROGRAM_TYPE_XDP_INIT;
    ebpf_result_t Result;

    UNREFERENCED_PARAMETER(BindingContext);

    TraceEnter(TRACE_CORE, "MapContext=%p", MapContext);

    if (!IsEqualGUID(ProgramType, &ExpectedProgramType)) {
        TraceError(TRACE_CORE, "CPUMAP only supports XDP program type");
        Result = EBPF_OPERATION_NOT_SUPPORTED;
        goto Exit;
    }

    Result = EBPF_SUCCESS;

Exit:

    TraceExitEbpfResult(TRACE_CORE);
    return Result;
}

static
ebpf_result_t
XdpCpuMapPostprocessMapFindElement(
    _In_ void *BindingContext,
    _In_ void *MapContext,
    size_t KeySize,
    _In_reads_opt_(KeySize) const uint8_t *Key,
    size_t InValueSize,
    _In_reads_(InValueSize) const uint8_t *InValue,
    size_t OutValueSize,
    _Out_writes_opt_(OutValueSize) uint8_t *OutValue,
    uint32_t Flags
    )
{
    const XDP_CPUMAP_PROVIDER_VALUE *Stored;

    UNREFERENCED_PARAMETER(BindingContext);
    UNREFERENCED_PARAMETER(MapContext);
    UNREFERENCED_PARAMETER(KeySize);
    UNREFERENCED_PARAMETER(Key);
    DBG_UNREFERENCED_PARAMETER(Flags);

    //
    // This provider sets updates_original_value, so the runtime blocks
    // find-element lookups issued by a kernel BPF program. This callback
    // therefore only runs for BPF user-mode API lookups.
    //
    ASSERT(!(Flags & EBPF_MAP_OPERATION_HELPER));

    if (InValue == NULL || InValueSize != sizeof(XDP_CPUMAP_PROVIDER_VALUE)) {
        return EBPF_INVALID_ARGUMENT;
    }

    if (OutValue == NULL || OutValueSize != sizeof(XDP_CPUMAP_ENTRY_V1)) {
        return EBPF_INVALID_ARGUMENT;
    }

    //
    // Project the public entry back to the caller. The referenced target that
    // follows it in the stored value is a kernel pointer and is deliberately not
    // copied; the runtime sizes OutValue at the public value_size, so there is
    // no path by which it could be.
    //
    Stored = (const XDP_CPUMAP_PROVIDER_VALUE *)InValue;
    RtlCopyMemory(OutValue, &Stored->Entry, sizeof(Stored->Entry));

    return EBPF_SUCCESS;
}

static
ebpf_result_t
XdpCpuMapPreprocessMapUpdateElement(
    _In_ void *BindingContext,
    _In_ void *MapContext,
    size_t KeySize,
    _In_reads_opt_(KeySize) const uint8_t *Key,
    size_t InValueSize,
    _In_reads_(InValueSize) const uint8_t *InValue,
    size_t OutValueSize,
    _Out_writes_opt_(OutValueSize) uint8_t *OutValue,
    uint32_t Flags
    )
{
    XDP_CPUMAP *CpuMap = CONTAINING_RECORD(MapContext, XDP_CPUMAP, Header);
    XDP_CPUMAP_PROVIDER_VALUE ProviderValue;
    XDP_CPUMAP_KEY SlotKey;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(BindingContext);
    DBG_UNREFERENCED_PARAMETER(Flags);

    TraceEnter(TRACE_CORE, "MapContext=%p", MapContext);

    //
    // This callback is the allocation site for rings and DPCs. It runs at
    // PASSIVE_LEVEL for user-mode callers, and because the provider sets
    // updates_original_value the runtime blocks BPF-program updates outright, so
    // it can never be reached at DISPATCH_LEVEL.
    //
    // It runs OUTSIDE the base-map bucket lock, so two calls for the same key can
    // overlap, and it CANNOT know whether the core operation will insert or
    // replace: it receives neither the map option nor a completion callback.
    // Nothing below depends on knowing.
    //
    ASSERT(!(Flags & EBPF_MAP_OPERATION_HELPER));

    if (Key == NULL || KeySize != sizeof(XDP_CPUMAP_KEY)) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    if (InValue == NULL || InValueSize != sizeof(XDP_CPUMAP_ENTRY_V1)) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    if (OutValue == NULL || OutValueSize != sizeof(XDP_CPUMAP_PROVIDER_VALUE)) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    //
    // Entry count is the base map's job: max_entries is enforced by the base hash
    // map. The provider validates only the key range.
    //
    SlotKey = *(const XDP_CPUMAP_KEY *)Key;
    if (SlotKey >= CpuMap->MaxEntries) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    Status =
        XdpCpuMapResolveTarget(
            CpuMap, (const XDP_CPUMAP_ENTRY_V1 *)InValue, &ProviderValue);
    if (!NT_SUCCESS(Status)) {
        goto Exit;
    }

    //
    // The runtime commits exactly these bytes to the base map, so the reference
    // taken by XdpCpuMapResolveTarget is now owned by the committed value. If
    // the commit fails, the runtime delivers the delete callback for THIS value,
    // which releases it.
    //
    RtlCopyMemory(OutValue, &ProviderValue, sizeof(ProviderValue));

Exit:

    TraceExitStatus(TRACE_CORE);

    if (NT_SUCCESS(Status)) {
        return EBPF_SUCCESS;
    }

    if (Status == STATUS_INSUFFICIENT_RESOURCES || Status == STATUS_NO_MEMORY) {
        //
        // Both cap exhaustion and pool failure are resource conditions, not bad
        // input. Reporting EBPF_INVALID_ARGUMENT for them would tell a loader its
        // entry was malformed, so it would retry the same valid entry forever
        // instead of backing off or reducing ring depth.
        //
        return EBPF_NO_MEMORY;
    }

    if (Status == STATUS_DELETE_PENDING) {
        return EBPF_INVALID_OBJECT;
    }

    return EBPF_INVALID_ARGUMENT;
}

static
void
XdpCpuMapPostprocessMapDeleteElement(
    _In_ void *BindingContext,
    _In_ void *MapContext,
    size_t KeySize,
    _In_reads_opt_(KeySize) const uint8_t *Key,
    size_t ValueSize,
    _In_reads_(ValueSize) const uint8_t *Value,
    uint32_t Flags
    )
{
    XDP_CPUMAP *CpuMap = CONTAINING_RECORD(MapContext, XDP_CPUMAP, Header);

    UNREFERENCED_PARAMETER(BindingContext);
    UNREFERENCED_PARAMETER(KeySize);
    UNREFERENCED_PARAMETER(Key);
    DBG_UNREFERENCED_PARAMETER(Flags);

    //
    // HARD CONSTRAINT. The runtime may deliver this at DISPATCH_LEVEL beneath the
    // base map's per-bucket spin lock, and during map cleanup it holds the custom
    // map's own lock. This function therefore:
    //
    //   * acquires NO lock but the retire work queue's own spin lock, which is a
    //     KSPIN_LOCK because that queue is created with MaxIrql = DISPATCH_LEVEL;
    //   * allocates nothing;
    //   * blocks never;
    //   * and DECIDES NOTHING. Whether this release is the last one, and so
    //     whether the target retires, is decided by the sweep under ConfigLock.
    //
    // No trace call is made here: WPP would be safe, but keeping this function
    // free of everything but interlocked operations and the work-queue insert
    // makes the constraint checkable by inspection.
    //
    ASSERT(!(Flags & EBPF_MAP_OPERATION_HELPER));

    if (Value == NULL || ValueSize != sizeof(XDP_CPUMAP_PROVIDER_VALUE)) {
        return;
    }

    XdpCpuMapQueueValueRelease(CpuMap, (const XDP_CPUMAP_PROVIDER_VALUE *)Value);
}

//
// Version 1 provider dispatch table, the same six callbacks XSKMAP registers.
//
static const ebpf_base_map_provider_dispatch_table_t XdpCpuMapProviderDispatchTable = {
    .header = EBPF_BASE_MAP_PROVIDER_DISPATCH_TABLE_HEADER,
    .preprocess_map_create = XdpCpuMapPreprocessMapCreate,
    .postprocess_map_delete = XdpCpuMapPostprocessMapDelete,
    .preprocess_associate_program_type = XdpCpuMapPreprocessAssociateProgramType,
    .postprocess_map_find_element = XdpCpuMapPostprocessMapFindElement,
    .preprocess_map_update_element = XdpCpuMapPreprocessMapUpdateElement,
    .postprocess_map_delete_element = XdpCpuMapPostprocessMapDeleteElement,
};

static const ebpf_base_map_provider_properties_t XdpCpuMapProviderProperties = {
    .header = EBPF_BASE_MAP_PROVIDER_PROPERTIES_HEADER,
    //
    // TRUE, matching XSKMAP. This is what makes the runtime reject BPF-program
    // lookup/update/delete on a CPUMAP before any provider mutation callback can
    // be reached at DISPATCH_LEVEL, which is what lets
    // preprocess_map_update_element allocate.
    //
    .updates_original_value = TRUE,
};

static const ebpf_map_provider_data_t XdpCpuMapProviderData = {
    .header = EBPF_MAP_PROVIDER_DATA_HEADER,
    .map_type = BPF_MAP_TYPE_CPUMAP,
    .base_map_type = BPF_MAP_TYPE_HASH,
    //
    // ebpf_map_provider_data_t stores non-const pointers, so cast away const.
    // The eBPF runtime treats the provider data as read-only.
    //
    .base_properties = (ebpf_base_map_provider_properties_t *)&XdpCpuMapProviderProperties,
    .base_provider_table =
        (ebpf_base_map_provider_dispatch_table_t *)&XdpCpuMapProviderDispatchTable,
};

static
NTSTATUS
XdpCpuMapOnClientAttach(
    _In_ const EBPF_EXTENSION_CLIENT *AttachingClient,
    _In_ const EBPF_EXTENSION_PROVIDER *AttachingProvider
    )
{
    const ebpf_extension_data_t *ClientExtData;
    const ebpf_map_client_data_t *ClientData;
    XDP_CPUMAP_BINDING_CONTEXT *Binding;

    UNREFERENCED_PARAMETER(AttachingProvider);

    TraceEnter(TRACE_CORE, "Client=%p", AttachingClient);

    ClientExtData = EbpfExtensionClientGetClientData(AttachingClient);
    if (ClientExtData == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    ClientData = (const ebpf_map_client_data_t *)ClientExtData;

    Binding = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Binding), XDP_POOLTAG_EBPF_NMR);
    if (Binding == NULL) {
        return STATUS_NO_MEMORY;
    }

    RtlCopyMemory(
        &Binding->ClientDispatch, ClientData->base_client_table,
        min(sizeof(Binding->ClientDispatch), ClientData->base_client_table->header.total_size));

    XdpEbpfMapContextOffsetPublish(&XdpCpuMapContextOffset, ClientData->map_context_offset);

    EbpfExtensionClientSetProviderData(AttachingClient, Binding);

    TraceExitSuccess(TRACE_CORE);
    return STATUS_SUCCESS;
}

static
NTSTATUS
XdpCpuMapOnClientDetach(
    _In_ const EBPF_EXTENSION_CLIENT *DetachingClient
    )
{
    XDP_CPUMAP_BINDING_CONTEXT *Binding = EbpfExtensionClientGetProviderData(DetachingClient);

    TraceEnter(TRACE_CORE, "Client=%p", DetachingClient);

    if (Binding != NULL) {
        ExFreePoolWithTag(Binding, XDP_POOLTAG_EBPF_NMR);
    }

    TraceExitSuccess(TRACE_CORE);
    return STATUS_SUCCESS;
}

ebpf_result_t
XdpCpuMapGetMap(
    _In_ const VOID *Map,
    _Out_ XDP_EBPF_MAP_TYPE *MapType,
    _Outptr_result_maybenull_ XDP_CPUMAP **CpuMap
    )
{
    //
    // Resolve the provider context stored at the published map-context offset,
    // exactly as XSKMAP does after its attach. If the CPUMAP provider client
    // never attached successfully, do not touch the map object at all; report
    // the same "no XDP map provider" result as the NULL-context path.
    //
    return XdpCpuMapGetMapFromContextOffset(&XdpCpuMapContextOffset, Map, MapType, CpuMap);
}

NTSTATUS
XdpCpuMapProviderStart(
    VOID
    )
{
    const EBPF_EXTENSION_PROVIDER_PARAMETERS Parameters = {
        .ProviderModuleId = &EbpfCpuMapProviderModuleId,
        .ProviderData = &XdpCpuMapProviderData,
    };
    NTSTATUS Status;

    TraceEnter(TRACE_CORE, "-");

    Status = XdpCpuMapStart();
    if (!NT_SUCCESS(Status)) {
        TraceError(TRACE_CORE, "Failed to start CPUMAP engine Status=%!STATUS!", Status);
        goto Exit;
    }

    Status =
        EbpfExtensionProviderRegister(
            &EBPF_MAP_INFO_EXTENSION_IID, &Parameters,
            XdpCpuMapOnClientAttach, XdpCpuMapOnClientDetach,
            NULL, &EbpfCpuMapProvider);
    if (!NT_SUCCESS(Status)) {
        TraceError(TRACE_CORE, "Failed to register CPUMAP provider Status=%!STATUS!", Status);
        XdpCpuMapStop();
        goto Exit;
    }

Exit:

    TraceExitStatus(TRACE_CORE);
    return Status;
}

VOID
XdpCpuMapProviderStop(
    VOID
    )
{
    TraceEnter(TRACE_CORE, "-");

    //
    // Unregister first so no new map can be created, then stop the engine, which
    // shuts down the retire work queue. Every map must already have been
    // destroyed by the time the provider is unregistered.
    //
    if (EbpfCpuMapProvider != NULL) {
        EbpfExtensionProviderUnregister(EbpfCpuMapProvider);
        EbpfCpuMapProvider = NULL;
    }

    XdpCpuMapStop();

    TraceExitSuccess(TRACE_CORE);
}
