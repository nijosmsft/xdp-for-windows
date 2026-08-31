//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// eBPF stubs.
//
// The dispatch table mirrors the REAL ebpf_base_map_client_dispatch_table_t
// member order from the pinned SDK's ebpf_extension.h, including the members
// CPUMAP does not use. Trimming it to the used members would let a mis-ordered
// initializer compile and silently bind the wrong function pointers.
//
// The stub ENFORCES the epoch contract rather than merely satisfying it.
// ebpf_extension.h requires every epoch memory operation to be made inside an
// epoch-protected region, and notes that provider dispatch invocations and BPF
// helper callbacks are already protected while anything else is not. That
// contract lives in another repository's header, so nothing in this build would
// otherwise check it -- and a violation is invisible in ordinary testing because
// the call succeeds and the memory is reclaimed anyway, right up until a reader
// happens to still be live.
//
// So: epoch_allocate_with_tag and epoch_free assert that an epoch is currently
// entered, and epoch_exit asserts one was. The tests are responsible for
// reproducing the regions the runtime supplies around provider callbacks; see
// the wrappers in cpumaptest.c. That makes the harness no more permissive than
// production, which is the only way a harness can catch this class of defect.
//

#pragma once

//
// Matches ebpf_extension.h exactly: typedef uint64_t epoch_state_t[4].
//
typedef uint64_t epoch_state_t[4];

typedef enum _ebpf_result {
    EBPF_SUCCESS = 0,
    EBPF_OPERATION_NOT_SUPPORTED = 1,
    EBPF_INVALID_ARGUMENT = 2,
    EBPF_NO_MEMORY = 3,
    EBPF_INVALID_OBJECT = 4,
} ebpf_result_t;

typedef
ebpf_result_t
ebpf_map_find_element_function_t(
    _In_ const void *map,
    _In_ const void *key,
    _Outptr_result_maybenull_ uint8_t **value
    );

typedef struct _ebpf_base_map_client_dispatch_table {
    void *header;
    ebpf_map_find_element_function_t *find_element_function;
    void (*epoch_enter)(void *epoch_state);
    void (*epoch_exit)(void *epoch_state);
    void *(*epoch_allocate_with_tag)(size_t size, uint32_t tag);
    void *(*epoch_allocate_cache_aligned_with_tag)(size_t size, uint32_t tag);
    void (*epoch_free)(void *memory);
    void (*epoch_free_cache_aligned)(void *memory);
} ebpf_base_map_client_dispatch_table_t;

extern ebpf_base_map_client_dispatch_table_t XdpCpuMapTestClientDispatch;

//
// Current epoch nesting depth. epoch_enter and epoch_exit are documented as
// re-entrant, so this is a depth rather than a flag.
//
extern LONG XdpCpuMapTestEpochDepth;

//
// Counts epoch memory operations observed inside a region, so a test can prove
// the region was actually exercised rather than merely present.
//
extern LONG XdpCpuMapTestEpochOperations;

//
// Reproduces the epoch-protected region the eBPF runtime establishes around a
// provider dispatch callback. Used by the test wrappers, never by the code under
// test -- the code under test must establish its own region wherever it is NOT
// running inside a callback, which is precisely the property being enforced.
//
VOID
XdpCpuMapTestEnterCallbackEpoch(
    _Out_ epoch_state_t *EpochState
    );

VOID
XdpCpuMapTestExitCallbackEpoch(
    _Inout_ epoch_state_t *EpochState
    );
