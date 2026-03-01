# XDP CPU Redirect / CPUMAP Implementation

> **Branch:** `user/nijos/xdp-cpumap`  
> **Base:** `main`  
> **Commits:** 7 commits, 22 files changed, ~1,950 lines added

This document describes the design and implementation of CPU-affinity packet steering in XDP for Windows ("CPUMAP"). It covers every layer from the user-facing API down to the kernel DPC drain path, explaining *what* was changed, *why*, and how the pieces connect.

---

## Table of Contents

1. [Motivation and Goals](#1-motivation-and-goals)
2. [Architecture Overview](#2-architecture-overview)
3. [Data Flow: End-to-End](#3-data-flow-end-to-end)
4. [API Layer Changes](#4-api-layer-changes)
   - 4.1 [XDP_RX_ACTION_CPU_REDIRECT](#41-xdp_rx_action_cpu_redirect)
   - 4.2 [XDP_REDIRECT_TARGET_TYPE_CPU](#42-xdp_redirect_target_type_cpu)
   - 4.3 [XDP_CPU_REDIRECT_PARAMS](#43-xdp_cpu_redirect_params)
5. [Private CPUMAP API (xdpcpumap.h)](#5-private-cpumap-api-xdpcpumaph)
   - 5.1 [XDP_FRAME_CPU_REDIRECT Frame Extension](#51-xdp_frame_cpu_redirect-frame-extension)
   - 5.2 [XDP_CPUMAP Lifecycle APIs](#52-xdp_cpumap-lifecycle-apis)
   - 5.3 [Batch Enqueue API](#53-batch-enqueue-api)
6. [CPUMAP Internal Implementation (cpumap.h / cpumap.c)](#6-cpumap-internal-implementation-cpumaphcpumapc)
   - 6.1 [XDP_CPUMAP struct](#61-xdp_cpumap-struct)
   - 6.2 [XDP_CPUMAP_RING struct](#62-xdp_cpumap_ring-struct)
   - 6.3 [Per-Source-CPU Clone Pools](#63-per-source-cpu-clone-pools)
   - 6.4 [Selective CPU Range Allocation](#64-selective-cpu-range-allocation)
   - 6.5 [Drain DPC Options: Batched vs DRAIN_ALL](#65-drain-dpc-options-batched-vs-drain_all)
   - 6.6 [Statistics and Diagnostics](#66-statistics-and-diagnostics)
   - 6.7 [XdpCpuMapCreate](#67-xdpcpumapcreate)
   - 6.8 [XdpCpuMapDestroy](#68-xdpcpumapdestroy)
   - 6.9 [XdpCpuMapFlushBatch](#69-xdpcpumapflushbatch)
7. [Program Inspection Layer (programinspect.c)](#7-program-inspection-layer-programinspectc)
   - 7.1 [Rule Validation](#71-rule-validation)
   - 7.2 [Per-Frame Classification: Hash and CPU Selection](#72-per-frame-classification-hash-and-cpu-selection)
   - 7.3 [Frame Extension Population](#73-frame-extension-population)
8. [LWF Receive Path (recv.c)](#8-lwf-receive-path-recvc)
   - 8.1 [CPU Redirect Extension Registration](#81-cpu-redirect-extension-registration)
   - 8.2 [Lazy CPUMAP Initialization](#82-lazy-cpumap-initialization)
   - 8.3 [Batch Collection and Flush](#83-batch-collection-and-flush)
   - 8.4 [CPUMAP Lifetime in the LWF](#84-cpumap-lifetime-in-the-lwf)
9. [LWF Generic Layer (generic.h / generic.c)](#9-lwf-generic-layer-generichgenericc)
10. [Test Tool: cpuredirect.exe](#10-test-tool-cpuredirectexe)
11. [XDP Core RX Queue Extension Registration (rx.c)](#11-xdp-core-rx-queue-extension-registration-rxc)
12. [XDP Program Binding and Inspection Context (program.c / program.h)](#12-xdp-program-binding-and-inspection-context-programc--programh)
13. [Build Integration (vcxproj, sln, precomp.h)](#13-build-integration-vcxproj-sln-precomph)
14. [Packet Fuzzer Stubs (pktfuzz)](#14-packet-fuzzer-stubs-pktfuzz)
15. [Deployment Bugfix: Certificate Import (tools/setup.ps1)](#15-deployment-bugfix-certificate-import-toolssetupps1)
16. [Build-Time Feature Flags](#16-build-time-feature-flags)
17. [Performance Design Decisions](#17-performance-design-decisions)
18. [Commit-by-Commit Walkthrough](#18-commit-by-commit-walkthrough)
19. [Complete File Change Summary](#19-complete-file-change-summary)
20. [Known Limitations and Future Work](#20-known-limitations-and-future-work)

---

## 1. Motivation and Goals

Windows does not have a native mechanism equivalent to Linux's XDP CPU redirect / CPUMAP. Packets arriving on an RSS queue can only be processed on that queue's affine CPU. For applications with a many-core worker model (e.g., a high-performance DNS server with one thread per CPU), packets received on 8 RSS queues must be steered to 24+ application CPUs without going through the full TCP/IP stack.

**Goals:**
- Allow an XDP program to classify each incoming packet and redirect it to a specific application CPU, bypassing the TCP/IP stack on the receiving CPU.
- Maintain per-flow CPU affinity (same 5-tuple always lands on the same CPU) to avoid reordering.
- Support fan-out to a configurable subset of CPUs, not necessarily starting at CPU 0.
- Operate with minimal per-packet overhead on the source (RSS) CPU.
- Be configurable enough to tune for different hardware (ring depth, drain batch size).

---

## 2. Architecture Overview

```text
                                         Target CPU (N)
                                +------------------------------+
                                |      User Application        |
                                | (DNS server workers on 56-79)|
                                +------------------------------+
                                                ^
                                                | Winsock recv()
                                                |
                                +------------------------------+
                                |           TCP/IP             |
                                +------------------------------+
                                                ^
                                                | NdisFIndicateReceiveNetBufferLists
                                                |
                                +------------------------------+
                                |      Target CPU DPC          |
                                |   XdpCpuMapDrainDpc()        |
                                | Drains ring, re-indicates NBL|
                                +------------------------------+
                                                ^
                                                |
                                                | Per-CPU Ring
                                                | (Lock-Protected)
                                                |
       Source CPU (RSS)                         |
+------------------------------+                |
| NdisMIndicateReceiveNetBufferLists            |
|             v                                 |
| xdplwf!XdpGenericReceive()                    |
|             v                                 |
| XDP Program Inspection                        |
| (programinspect.c)                            |
|  - Hash 5-tuple                               |
|  - Target = (Hash % N) + Base                 |
|             v                                 |
| recv.c: post-inspect                          |
|  - XdpCpuMapBatchAdd()                        |
|  - XdpCpuMapFlushBatch() ---------------------+
+------------------------------+
```

**Key design principles:**
- The source (RSS) CPU does *not* call the TCP/IP stack. It clones or pre-allocates the NBL and enqueues it to the target CPU's ring.
- The target CPU's DPC drains that ring and calls `NdisFIndicateReceiveNetBufferLists` — from the correct CPU, with the correct affinity.
- All redirect decisions are **batched** per RSS batch to amortize lock acquisitions.

### 2.1 Extended Architecture: Driver Boundaries and Call Flow

The diagram below shows the complete call chain across both drivers (`xdp.sys` and `xdplwf.sys`), including the NMR dispatch boundary, CPUMAP lazy initialization, and the full PREALLOC vs clone paths.

```text
                                         Target CPU (N)
                                +------------------------------+
                                |      User Application        |
                                | (DNS server workers on 56-79)|
                                +------------------------------+
                                                ^
                                                | Winsock recv()
                                                |
                                +------------------------------+
                                |           TCP/IP             |
                                +------------------------------+
                                                ^
                                                | NdisFIndicateReceiveNetBufferLists
                                                |   (NDIS_RECEIVE_FLAGS_RESOURCES)
                                                |
                                +------------------------------+
                                | [xdplwf.sys] Target CPU DPC  |
                                |   XdpCpuMapDrainDpc()        |
                                |   - Dequeue from ring        |
                                |   - Indicate NBLs to TCP/IP  |
                                |   - Recycle shells (PREALLOC) |
                                |     or free clones           |
                                +------------------------------+
                                                ^
                                                |
                                    Per-CPU Ring (KSPIN_LOCK)
                                    PerCpuRings[T - CpuBase]
                                                |
                                                |
  User Mode                                     |
+------------------------------+                |
| cpuredirect.exe              |                |
|  XdpCreateProgram(           |                |
|    XDP_MATCH_UDP_DST,        |                |
|    XDP_REDIRECT_TARGET_      |                |
|    TYPE_CPU,                 |                |
|    {Base=56,Count=24})       |                |
+-------|----------------------+                |
        | IOCTL                                 |
        v                                       |
+------------------------------+                |
| [xdp.sys] Control Plane      |                |
|  XdpProgramValidateRule()    |                |
|   - Validate CPU range       |                |
|   - Store in compiled rule   |                |
|                              |                |
|  XdpRxQueueRegister          |                |
|   ExtensionVersion()         |                |
|   - Register CPU_REDIRECT    |                |
|     frame extension          |                |
|                              |                |
|  XdpProgramBindingAttach()   |                |
|   - Handle TARGET_TYPE_CPU   |                |
|   - No external handle       |                |
|     needed (unlike XSK)      |                |
+------------------------------+                |
        | NMR dispatch table                    |
        v                                       |
     Source CPU (RSS)                           |
+-----------------------------------------------+------+
| [xdplwf.sys] Data Plane                              |
|                                                      |
| FilterReceiveNetBufferLists()                        |
|   |                                                  |
|   v                                                  |
| XdpGenericReceive()                                  |
|   - Convert NBLs to XDP frame ring entries           |
|   - Populate frame extensions                        |
|   |                                                  |
|   |--- NMR Dispatch->Inspect ----------------------> |
|   |                                                  |
|   |  [xdp.sys] XdpInspect() / programinspect.c       |
|   |    XdpParseFrame()                               |
|   |     - Extract IP src/dst, proto, UDP ports       |
|   |    XDP_MATCH_UDP_DST check (port == 53?)         |
|   |     - NO  -> RxAction = PASS (normal stack)      |
|   |     - YES -> Hash 5-tuple (symmetric + murmur3)  |
|   |              TargetCpu = (Hash % Count) + Base   |
|   |              RxAction = CPU_REDIRECT             |
|   |              Populate XDP_FRAME_CPU_REDIRECT ext |
|   |                { TargetCpu, CpuBase, CpuCount,   |
|   |                  RingDepth, DrainBatchSize }     |
|   |<-- returns RxAction per frame -------------------|
|   |                                                  |
|   v                                                  |
| XdpGenericReceivePostInspectNbs()                    |
|   |                                                  |
|   |-- RxAction == CPU_REDIRECT?                      |
|   |     |                                            |
|   |     v                                            |
|   |   Read XDP_FRAME_CPU_REDIRECT extension          |
|   |     |                                            |
|   |     v                                            |
|   |   Generic->CpuMap == NULL?                       |
|   |     YES -> XdpCpuMapCreate()  [lazy init]        |
|   |       - Alloc PerCpuClonePools (all sys CPUs)    |
|   |       - Alloc PerCpuRings[CpuCount]              |
|   |       - Alloc PerCpuDpcs[CpuCount]               |
|   |       - (PREALLOC) Alloc shell pool per ring     |
|   |       - InterlockedCompareExchangePointer        |
|   |         (race-safe install)                      |
|   |     |                                            |
|   |     v                                            |
|   |   XdpCpuMapBatchAdd()                            |
|   |     - Collect {OrigNbl, TargetCpu, Filter, Port} |
|   |     - If batch full (32): flush immediately      |
|   |     |                                            |
|   |     v  (after all frames in indication)          |
|   |   XdpCpuMapFlushBatch()                          |
|   |     |                                            |
|   |     +-- Phase 1 (no locks held):                 |
|   |     |   PREALLOC=1:                              |
|   |     |     InterlockedPopEntrySList (shell)       |
|   |     |     memcpy packet -> Shell->DataBuffer     |
|   |     |   PREALLOC=0:                              |
|   |     |     NdisAllocateCloneNetBufferList         |
|   |     |     (from PerCpuClonePools[SourceCpu])     |
|   |     |                                            |
|   |     +-- Phase 2 (one lock per target CPU):       |
|   |         KeAcquireInStackQueuedSpinLock(Ring)     |
|   |         Enqueue all clones for this target       |
|   |         KeReleaseInStackQueuedSpinLock           |
|   |         KeInsertQueueDpc(target CPU) --------->--+
|   |                                                  |
|   |-- RxAction == PASS?                              |
|   |     -> NdisFIndicateReceiveNetBufferLists        |
|   |        (normal TCP/IP stack)                     |
|   |                                                  |
|   |-- RxAction == DROP?                              |
|   |     -> Return NBL to miniport                    |
|   |                                                  |
|   v                                                  |
| Return original NBLs to miniport                     |
|   (CPU_REDIRECT originals returned like DROP -       |
|    ownership transferred to clone/shell)             |
+------------------------------------------------------+
```

**Driver boundary summary:**

| Function | Driver | Layer | IRQL |
|----------|--------|-------|------|
| `XdpProgramValidateRule` | xdp.sys | Control plane | PASSIVE |
| `XdpProgramBindingAttach` | xdp.sys | Control plane | PASSIVE |
| `XdpRxQueueRegisterExtensionVersion` | xdp.sys | Control plane | PASSIVE |
| `XdpRxQueueGetExtension` | xdp.sys | Control plane | PASSIVE |
| `XdpExtensionSetEnableEntry` | xdp.sys | Control plane | PASSIVE |
| `XdpInspect` / `XdpParseFrame` | xdp.sys | Inspection | DISPATCH |
| `FilterReceiveNetBufferLists` | xdplwf.sys | Data plane | DISPATCH |
| `XdpGenericReceive` | xdplwf.sys | Data plane | DISPATCH |
| `XdpGenericReceivePostInspectNbs` | xdplwf.sys | Data plane | DISPATCH |
| `XdpCpuMapCreate` | xdp.sys (called by xdplwf.sys) | Data plane (lazy init) | PASSIVE* |
| `XdpCpuMapBatchInit` / `BatchAdd` | xdp.sys (called by xdplwf.sys) | Data plane | DISPATCH |
| `XdpCpuMapFlushBatch` (Phase 1+2) | xdp.sys (called by xdplwf.sys) | Data plane | DISPATCH |
| `XdpCpuMapDrainDpc` | xdp.sys (DPC callback) | Data plane | DISPATCH |
| `XdpCpuMapDestroy` | xdp.sys (called by xdplwf.sys) | Teardown | PASSIVE |
| `XdpGenericCleanupInterface` | xdplwf.sys | Teardown | PASSIVE |
| `NdisFIndicateReceiveNetBufferLists` | xdplwf.sys → NDIS | Drain (target CPU) | DISPATCH |

*\* `XdpCpuMapCreate` requires PASSIVE_LEVEL for `ExAllocatePoolZero` and `NdisAllocateNetBufferListPool`. See Section 20 for the DISPATCH_LEVEL lazy-init caveat.*

**Key design boundaries:**
- **xdp.sys** owns the CPUMAP implementation: all CPUMAP functions (`XdpCpuMapCreate`, `XdpCpuMapDestroy`, `XdpCpuMapBatchInit`, `XdpCpuMapBatchAdd`, `XdpCpuMapFlushBatch`, `XdpCpuMapDrainDpc`) are defined in `src/xdp/cpumap.c` and compiled into xdp.sys. It also handles control-plane operations (program validation, binding, frame extension registration) and runs the per-frame inspection logic (`XdpInspect`) via the NMR dispatch table.
- **xdplwf.sys** is the **caller** of the CPUMAP APIs. It invokes `XdpCpuMapCreate` (lazy init from `recv.c`), `XdpCpuMapBatchInit`/`BatchAdd`/`FlushBatch` (from the post-inspect path in `recv.c`), and `XdpCpuMapDestroy` (from `generic.c` cleanup). It also owns the NDIS filter entry points (`FilterReceiveNetBufferLists`, `XdpGenericReceive`), the post-inspect dispatch logic, and the `NdisFIndicateReceiveNetBufferLists` call on the target CPU. The `XdpCpuMapDrainDpc` callback runs on the target CPU's DPC thread — though the function is in xdp.sys, it executes in the context of the target CPU.
- The `XDP_FRAME_CPU_REDIRECT` frame extension is the **only** communication channel between xdp.sys (writes it during inspection) and xdplwf.sys (reads it during post-inspect). Both run synchronously on the same RSS CPU.

---

## 3. Data Flow: End-to-End

1. `cpuredirect.exe` calls `XdpCreateProgram` with a rule: `Action = XDP_PROGRAM_ACTION_REDIRECT`, `TargetType = XDP_REDIRECT_TARGET_TYPE_CPU`, `CpuRedirect = { Base=56, Count=24, RingDepth=32768, DrainBatch=256 }`.

2. `programinspect.c` validates the rule at PASSIVE_LEVEL (CPU range within system bounds) and stores validated parameters in the compiled `XDP_PROGRAM`.

3. On the first incoming packet matching the rule, `XdpInspect` in `programinspect.c` runs at DISPATCH_LEVEL on the RSS CPU:
   - Computes a flow hash from the IP/UDP 5-tuple.
   - Selects `TargetCpu = (hash % CpuCount) + CpuBase`.
   - Sets `XDP_RX_ACTION_CPU_REDIRECT` as the frame action.
   - Writes `TargetCpu`, `CpuBase`, `CpuCount`, `RingDepth`, `DrainBatchSize` into the frame's `XDP_FRAME_CPU_REDIRECT` extension.

4. Back in `recv.c` (`XdpGenericReceivePostInspectNbs`):
   - If `Generic->CpuMap == NULL`, atomically creates a new `XDP_CPUMAP` using parameters from the frame extension (lazy initialization).
   - Calls `XdpCpuMapBatchAdd` to collect the redirect decision.
   - After processing all frames in the batch, calls `XdpCpuMapFlushBatch`.

5. `XdpCpuMapFlushBatch` in `cpumap.c`:
   - **Phase 1:** Clones (or uses pre-allocated shells for) all NBLs in the batch, no locks held.
   - **Phase 2:** Groups by target CPU; for each unique target CPU, acquires the ring lock once, enqueues all NBLs destined for that CPU, releases the lock, and inserts the DPC once.

6. The target CPU's `XdpCpuMapDrainDpc` fires:
   - Acquires the ring lock and dequeues up to `DrainBatchSize` NBLs (or the entire ring in DRAIN_ALL mode).
   - Calls `NdisFIndicateReceiveNetBufferLists(..., NDIS_RECEIVE_FLAGS_RESOURCES)` to pass the packet chain up to TCP/IP on the target CPU.
   - Frees or recycles NBL shells.

7. TCP/IP delivers the DNS query to the application socket bound on that CPU.

---

## 4. API Layer Changes

### 4.1 `XDP_RX_ACTION_CPU_REDIRECT`

**File:** `published/external/xdp/datapath.h`

A new enumerator added to `XDP_RX_ACTION`:

```c
typedef enum _XDP_RX_ACTION {
    XDP_RX_ACTION_DROP,
    XDP_RX_ACTION_PASS,
    XDP_RX_ACTION_TX,
    XDP_RX_ACTION_CPU_REDIRECT,   // ← NEW
} XDP_RX_ACTION;
```

This action is set by the XDP program inspection for frames destined for CPU redirect. The post-inspect path in `recv.c` handles this new action value. The original NBL is returned to the miniport (like DROP) because ownership transfers to the cloned NBL enqueued in the CPUMAP ring.

### 4.2 `XDP_REDIRECT_TARGET_TYPE_CPU`

**File:** `published/external/xdp/program.h`

New redirect target type added to the enum:

```c
typedef enum _XDP_REDIRECT_TARGET_TYPE {
    XDP_REDIRECT_TARGET_TYPE_XSK,
    XDP_REDIRECT_TARGET_TYPE_CPU,   // ← NEW
} XDP_REDIRECT_TARGET_TYPE;
```

### 4.3 `XDP_CPU_REDIRECT_PARAMS`

**File:** `published/external/xdp/program.h`

New struct specifying the CPU redirect rule parameters:

```c
typedef struct _XDP_CPU_REDIRECT_PARAMS {
    UINT32 TargetCpuBase;    // First target CPU (absolute index, e.g. 56)
    UINT32 TargetCpuCount;   // Number of target CPUs (e.g. 24)
    UINT32 RingDepth;        // Per-CPU ring capacity. 0 = default (32768)
    UINT32 DrainBatchSize;   // Max NBLs drained per DPC iteration. 0 = default (256)
} XDP_CPU_REDIRECT_PARAMS;
```

`XDP_REDIRECT_PARAMS` was extended with a union to accommodate both XSK handle redirects and CPU redirects:

```c
typedef struct _XDP_REDIRECT_PARAMS {
    XDP_REDIRECT_TARGET_TYPE TargetType;
    union {
        HANDLE Target;                      // XSK path
        XDP_CPU_REDIRECT_PARAMS CpuRedirect; // CPU path ← NEW
    };
} XDP_REDIRECT_PARAMS;
```

**Usage by `cpuredirect.exe`:**
```c
Rule.Redirect.TargetType = XDP_REDIRECT_TARGET_TYPE_CPU;
Rule.Redirect.CpuRedirect.TargetCpuBase  = 56;
Rule.Redirect.CpuRedirect.TargetCpuCount = 24;
Rule.Redirect.CpuRedirect.RingDepth      = 32768; // 0 = use default
Rule.Redirect.CpuRedirect.DrainBatchSize = 256;   // 0 = use default
```

---

## 5. Private CPUMAP API (xdpcpumap.h)

**File:** `published/private/xdpcpumap.h` (new file)

This header is shared between `xdp.sys` (creates and manages the CPUMAP, calls inspection) and `xdplwf.sys` (calls create/destroy/enqueue at the NDIS filter layer). It is not part of the external user-mode API.

### 5.1 `XDP_FRAME_CPU_REDIRECT` Frame Extension

The frame extension carries per-frame classification results from the XDP program inspection (`xdp.sys`) to the post-inspect receive path (`xdplwf.sys`). It is allocated in the XDP frame ring alongside other extensions.

```c
typedef struct _XDP_FRAME_CPU_REDIRECT {
    UINT32 TargetCpu;      // Absolute CPU index selected for this frame
    UINT32 CpuBase;        // First CPU in the redirect range (from rule)
    UINT32 CpuCount;       // Number of CPUs in the redirect range (from rule)
    UINT32 RingDepth;      // Per-CPU ring capacity to use (from rule, 0 = default)
    UINT32 DrainBatchSize; // DPC drain batch limit (from rule, 0 = default)
} XDP_FRAME_CPU_REDIRECT;

#define XDP_FRAME_EXTENSION_CPU_REDIRECT_NAME    L"ms_frame_cpu_redirect"
#define XDP_FRAME_EXTENSION_CPU_REDIRECT_VERSION_1 1U
```

**Why carry CpuBase/CpuCount/RingDepth/DrainBatchSize per frame?**  
The CPUMAP is created lazily on the first packet to avoid the ~1.6 GB allocation at program bind time before any traffic arrives. At the point of lazy creation (DISPATCH_LEVEL in `recv.c`), re-reading the XDP program rules would require taking locks and calling back into `xdp.sys`. Carrying the parameters in the frame extension avoids this: the inspection path (which already processes each frame) stamps the rule parameters once per frame, and the post-inspect path reads them directly.

### 5.2 `XDP_CPUMAP` Lifecycle APIs

```c
// Create a CPUMAP for [CpuBase, CpuBase+CpuCount) target CPUs.
// RingCapacity must be a power of 2. DrainBatchSize 0 = default (256).
// Called at PASSIVE_LEVEL (lazy creation happens on first packet at DISPATCH_LEVEL,
// but the actual ExAllocatePoolZero + NDIS pool create requires PASSIVE_LEVEL).
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
XdpCpuMapCreate(
    _In_ UINT32 CpuBase,
    _In_ UINT32 CpuCount,
    _In_ UINT32 RingCapacity,
    _In_ UINT32 DrainBatchSize,
    _In_ NDIS_HANDLE NdisHandle,
    _Out_ XDP_CPUMAP **CpuMap
    );

// Destroy the CPUMAP: flush any remaining NBLs, dump stats via DbgPrintEx,
// free all memory. Called at PASSIVE_LEVEL from queue delete or interface cleanup.
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
XdpCpuMapDestroy(
    _In_ XDP_CPUMAP *CpuMap
    );
```

### 5.3 Batch Enqueue API

The batch API reduces lock acquisitions from one per packet to one per unique target CPU per indication batch.

```c
#define XDP_CPUMAP_MAX_BATCH_ENTRIES 32

typedef struct _XDP_CPUMAP_BATCH_ENTRY {
    NET_BUFFER_LIST *OriginalNbl;   // Original NBL (not the clone)
    UINT32 TargetCpu;
    NDIS_HANDLE FilterHandle;
    NDIS_PORT_NUMBER PortNumber;
} XDP_CPUMAP_BATCH_ENTRY;

typedef struct _XDP_CPUMAP_BATCH {
    UINT32 Count;
    XDP_CPUMAP_BATCH_ENTRY Entries[XDP_CPUMAP_MAX_BATCH_ENTRIES];
} XDP_CPUMAP_BATCH;

// Initialize batch to empty.
VOID XdpCpuMapBatchInit(_Out_ XDP_CPUMAP_BATCH *Batch);

// Add one entry. Returns FALSE if batch full (caller must flush first).
BOOLEAN XdpCpuMapBatchAdd(_Inout_ XDP_CPUMAP_BATCH *Batch, ...);

// Flush: clone all NBLs (no lock), then group by target CPU and enqueue
// with one lock acquisition and one KeInsertQueueDpc per unique target.
VOID XdpCpuMapFlushBatch(_In_ XDP_CPUMAP *CpuMap, _Inout_ XDP_CPUMAP_BATCH *Batch);
```

---

## 6. CPUMAP Internal Implementation (cpumap.h / cpumap.c)

Both files are new (`src/xdp/cpumap.h`, `src/xdp/cpumap.c`).

### 6.1 `XDP_CPUMAP` struct

```c
struct _XDP_CPUMAP {
    UINT32 CpuBase;    // Absolute index of first target CPU
    UINT32 CpuCount;   // Number of target CPUs; rings indexed [0, CpuCount)
    UINT32 Flags;      // XDP_CPUMAP_FLAG_* runtime behavior flags

    volatile BOOLEAN Active;

    // Per-source-CPU NBL clone pools (indexed by KeGetCurrentProcessorIndex()).
    // Allocated for ALL system CPUs (any RSS queue may be a source).
    // NDIS auto-routes NdisFreeCloneNetBufferList back to originating pool.
    UINT32 ClonePoolCount;
    NDIS_HANDLE *PerCpuClonePools;

    // AZC !CanPend deep-copy fallback pool (lazy allocation).
    NDIS_HANDLE DeepCopyNblPool;
    SLIST_HEADER DeepCopyFreeList;

    XDP_CPUMAP_RING **PerCpuRings;  // [CpuCount] rings, indexed by (TargetCpu - CpuBase)
    KDPC *PerCpuDpcs;               // [CpuCount] DPCs, pinned to target CPUs

    volatile LONG RefCount;
};
```

**Key design decisions:**

- **`PerCpuRings` is indexed relative to `CpuBase`.** Ring for target CPU `T` is at `PerCpuRings[T - CpuBase]`. All array accesses are bounds-checked: if `T < CpuBase || T >= CpuBase + CpuCount`, the packet is dropped.

- **Clone pools are allocated for ALL system CPUs**, not just the target range. The reason: any of the 8 RSS source CPUs can enqueue to any target ring. Clone allocation uses `KeGetCurrentProcessorIndex()` to find the pool for the *source* CPU, not the target. Allocating only `CpuCount` pools would leave RSS CPUs outside the target range without a pool.

### 6.2 `XDP_CPUMAP_RING` struct

One ring per target CPU, cache-line aligned to prevent false sharing:

```c
typedef struct DECLSPEC_CACHEALIGN _XDP_CPUMAP_RING {
    KSPIN_LOCK Lock;
    UINT32 Head;            // Consumer index (drain DPC)
    UINT32 Tail;            // Producer index (source CPU, under lock)
    UINT32 Capacity;        // Power of 2 (configurable, default 32768)
    UINT32 Mask;            // Capacity - 1 (for fast modulo via &)
    UINT32 DrainBatchSize;  // Runtime-configurable drain cap (default 256)

    // Per-ring statistics (see Section 6.6)
    volatile LONG EnqueueCount;
    volatile LONG DrainCount;
    volatile LONG DropCount;
    // ... plus 12 more diagnostic counters

    struct _XDP_CPUMAP *OwnerMap;  // Back-pointer for map flags and stats in DrainDpc

    XDP_CPUMAP_ENTRY Entries[ANYSIZE_ARRAY]; // Ring buffer (sized at create time)
} XDP_CPUMAP_RING;
```

**Ring sizing:** At 32768 entries × ~20 bytes/entry (pointer + filter handle + port + IsDeepCopy) = ~640 KB per ring. With 24 target CPUs this is ~15 MB NonPagedPool. This is intentionally large to absorb bursts when the drain DPC doesn't fire fast enough on each target CPU.

**Previous default (32 entries)** was the original XDP CPUMAP stub size and was completely inadequate for sustained load — the ring filled in microseconds at 200K+ PPS.

### 6.3 Per-Source-CPU Clone Pools

**Problem (baseline bottleneck #1):** A single `NblClonePool` shared by all source CPUs caused ~2M lock acquisitions/sec on the NDIS pool's internal spinlock at high packet rates.

**Solution:** One `NdisAllocateNetBufferListPool` handle per system CPU (`ClonePoolCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS)`). The source CPU uses its own pool for allocation. NDIS stores the pool handle in `NBL->NblPoolHandle` at allocation time, so `NdisFreeCloneNetBufferList` auto-routes frees back to the originating pool regardless of which CPU calls it.

```c
// In XdpCpuMapFlushBatch Phase 1:
UINT32 SourceCpu = KeGetCurrentProcessorIndex();
NDIS_HANDLE ClonePool = CpuMap->PerCpuClonePools[SourceCpu];
Clone = NdisAllocateCloneNetBufferList(Nbl, ClonePool, NULL, 0);
```

Clone pools are the primary allocation path for every redirected packet. Each RSS CPU allocates clones from its own pool; NDIS auto-routes frees back to the originating pool regardless of which CPU calls `NdisFreeCloneNetBufferList`.

### 6.4 Selective CPU Range Allocation

**Before:** Rings and DPCs were allocated for all system CPUs (0 to `ActiveCpuCount - 1`), wasting ~60 × 768 KB = ~45 MB for a 24-CPU target range on an 80-CPU system.

**After:** Rings and DPCs are allocated only for `CpuCount` target CPUs. The DPC for ring `i` is pinned to absolute CPU `CpuBase + i` via `KeGetProcessorNumberFromIndex`:

```c
for (UINT32 i = 0; i < CpuCount; i++) {
    // Ring for target CPU (CpuBase + i), stored at PerCpuRings[i]
    // DPC is affine to processor number of (CpuBase + i)
    PROCESSOR_NUMBER ProcNumber;
    KeGetProcessorNumberFromIndex(CpuBase + i, &ProcNumber);
    KeSetTargetProcessorDpcEx(&Map->PerCpuDpcs[i], &ProcNumber);
}
```

Memory reduction: from ~45 MB to ~18 MB for the 80-CPU → 24-target case.

### 6.5 Drain DPC Options: Batched vs DRAIN_ALL

Controlled by `#define XDP_CPUMAP_DRAIN_ALL 0` (default: batched mode).

**Batched mode (default):**
- Dequeue up to `Ring->DrainBatchSize` NBLs per DPC invocation.
- If work remains (`Ring->Head != Ring->Tail`), the loop re-executes up to the batch limit again in the same DPC call (`DpcLoopIterations` stat tracks this).
- If the ring is still non-empty after the loop, `KeInsertQueueDpc` is called to re-queue the DPC (`DpcRequeueCount` stat).

The drain loop iterates inside the DPC (not re-queuing between each batch) to eliminate the DPC re-queue scheduling gap, which caused ring drops under sustained load:

```c
do {  // drain-until-empty inner loop
    // Acquire lock, dequeue up to DrainBatchSize, release lock
    // NdisFIndicateReceiveNetBufferLists(...)  
    // Free clones immediately (RESOURCES flag)
} while (MoreWork && ++LoopIter < MAX_LOOP_ITERATIONS);
```

**DRAIN_ALL mode (`XDP_CPUMAP_DRAIN_ALL=1`):**
- Dequeues the **entire ring** in one lock acquisition.
- Uses `NBL->MiniportReserved[1]` to stash the `IsDeepCopy` flag (avoids a fixed-size stack array which would overflow the DPC stack at large ring sizes).
- Eliminates the re-queue gap but holds the DPC thread longer, potentially starving other DPCs on the target CPU under sustained line-rate load.

### 6.6 Statistics and Diagnostics

Every `XDP_CPUMAP_RING` has 16+ counters, all `volatile LONG` (updated with `InterlockedIncrement` / `InterlockedExchangeIfGreater`):

| Counter | Description |
|---------|-------------|
| `EnqueueCount` | Total NBLs successfully enqueued |
| `DrainCount` | Total NBLs drained and indicated |
| `DropCount` | Total NBLs dropped (ring full + clone fail) |
| `RingFullCount` | Drops due to ring full at enqueue time |
| `CloneFailCount` | Drops due to `NdisAllocateCloneNetBufferList` returning NULL |
| `DpcInvokeCount` | Number of times the drain DPC fired |
| `DpcMaxBatchDrained` | Largest batch size in a single DPC iteration |
| `DpcRequeueCount` | DPC re-queued because ring had more work |
| `DpcEmptyCount` | DPC fired but ring was already empty (spurious) |
| `MaxRingDepth` | High-water mark of `Tail - Head` |
| `EnqueueBatchCount` | Number of `FlushBatch` calls that touched this ring |
| `DpcLoopIterations` | Total inner drain-until-empty loop iterations |
| `DpcMaxLoopIterations` | Max loop iterations in a single DPC invocation |
| `LockWaitCycles` | Total rdtsc cycles waiting on spinlock (producers + consumer) |
| `LockAcquireCount` | Total lock acquisitions |
| `SourceCpuMask` | Bitmask of source CPUs that enqueued (up to 128) |

**Stats dump:** `XdpCpuMapDestroy` dumps per-ring stats and totals (including loss percentage) via `DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL, ...)`. To see this output:
- **With kernel debugger attached:** `ed nt!Kd_IHVNETWORK_Mask 0xffffffff`
- **Without debugger:** Set `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter\IHVNETWORK = 0xffffffff` (DWORD, requires reboot), then run **DebugView** (Sysinternals) with `Capture → Capture Kernel`.

### 6.7 `XdpCpuMapCreate`

**IRQL:** PASSIVE_LEVEL  
**Signature:** `XdpCpuMapCreate(CpuBase, CpuCount, RingCapacity, DrainBatchSize, Flags, NdisHandle, &CpuMap)`

Allocation sequence:
1. Validate: `CpuCount > 0`, `RingCapacity` is power-of-2, `CpuBase + CpuCount <= ActiveCpuCount`.
2. Clamp `DrainBatchSize` to `[1, XDP_CPUMAP_MAX_BATCH_SIZE (256)]` if 0 or out of range.
3. Allocate `XDP_CPUMAP` struct.
4. Allocate `PerCpuClonePools` array for ALL system CPUs; call `NdisAllocateNetBufferListPool` for each.
5. If AZC flag set, allocate `DeepCopyNblPool` for !CanPend deep-copy fallback.
6. For each target CPU `i` in `[0, CpuCount)`:
   - Allocate ring: `sizeof(XDP_CPUMAP_RING) + sizeof(XDP_CPUMAP_ENTRY) * RingCapacity`
   - Initialize ring fields, stat counters, `DrainBatchSize`.
   - Store at `PerCpuRings[i]`.
   - Initialize DPC at `PerCpuDpcs[i]`, target to processor `CpuBase + i`.
7. Set `Active = TRUE`, `RefCount = 1`.

### 6.8 `XdpCpuMapDestroy`

**IRQL:** PASSIVE_LEVEL  
**Called from:** `XdpGenericRxDeleteQueue` (when last RX queue deleted) and `XdpGenericCleanupInterface`.

Sequence:
1. Set `Active = FALSE` to block new enqueues (checked in `XdpCpuMapFlushBatch`).
2. Flush all DPCs via `KeFlushQueuedDpcs`.
3. For AZC maps: drain ring entries — recycle deep-copies and return originals to miniport.
4. Dump per-ring statistics via `DbgPrintEx` (with TSC-calibrated lock timing).
5. For each ring: drain any remaining clones, free ring memory.
6. Free all per-CPU clone pools (`NdisFreeNetBufferListPool` × ClonePoolCount).
7. If AZC, drain and free `DeepCopyNblPool`.
8. Free `XDP_CPUMAP` struct.

### 6.9 `XdpCpuMapFlushBatch`

**IRQL:** DISPATCH_LEVEL (called from post-inspect on RSS CPU)

**Phase 1 — Clone all NBLs, no locks held:**
```
for each entry in Batch:
    SourceCpu = KeGetCurrentProcessorIndex()
    ClonePool = CpuMap->PerCpuClonePools[SourceCpu]
    if AZC && !CanPend:
        Allocate or pop deep-copy NBL from DeepCopyFreeList
        NdisRetreatNetBufferDataStart to allocate data pages
        MdlCopyMdlChainToMdlChainAtOffsetNonTemporal (memcpy)
        Clones[i] = DeepCopy
        IsDeepCopyBatch[i] = TRUE
    else:
        Clones[i] = NdisAllocateCloneNetBufferList(OrigNbl, ClonePool, ...)
        IsDeepCopyBatch[i] = FALSE
```

**Phase 2 — Enqueue by target CPU, one lock per target:**
```
for each unprocessed entry i:
    TargetCpu = Batch.Entries[i].TargetCpu
    RingIndex  = TargetCpu - CpuMap->CpuBase
    // bounds check
    Ring = CpuMap->PerCpuRings[RingIndex]
    KeAcquireInStackQueuedSpinLock(&Ring->Lock)
    for each entry j >= i with same TargetCpu:
        if Ring->Tail - Ring->Head < Ring->Capacity:
            Ring->Entries[Ring->Tail & Ring->Mask] = { Clone[j], FilterHandle, Port, IsDeepCopy[j] }
            Ring->Tail++
            Ring->EnqueueCount++
            MaxRingDepth = max(MaxRingDepth, Tail - Head)
        else:
            NdisFreeCloneNetBufferList or recycle deep-copy
            Ring->DropCount++
            Ring->RingFullCount++
        mark j as processed
    KeReleaseInStackQueuedSpinLock(...)
    KeInsertQueueDpc(&CpuMap->PerCpuDpcs[RingIndex], Ring, ...)
    Ring->EnqueueBatchCount++
```

Lock acquisitions = number of unique target CPUs in the batch (≤ `XDP_CPUMAP_MAX_BATCH_ENTRIES = 32`).  
DPC inserts = same.

---

## 7. Program Inspection Layer (programinspect.c)

### 7.1 Rule Validation

**Function:** `XdpProgramValidateRule` (existing, extended)  
**IRQL:** PASSIVE_LEVEL (called from `XdpCreateProgram` IOCTL handler)

For `XDP_REDIRECT_TARGET_TYPE_CPU` rules:
```c
ActiveCpuCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
CpuBase  = UserRule->Redirect.CpuRedirect.TargetCpuBase;
CpuCount = UserRule->Redirect.CpuRedirect.TargetCpuCount;

// Reject: CpuBase out of range, CpuCount==0, or range overflows
if (CpuBase >= ActiveCpuCount ||
    CpuCount == 0 ||
    CpuBase + CpuCount > ActiveCpuCount) {
    return STATUS_INVALID_PARAMETER;
}

// Copy validated params into compiled rule
ValidatedRule->Redirect.CpuRedirect = UserRule->Redirect.CpuRedirect;
```

`RingDepth` and `DrainBatchSize` are passed through without validation at this stage — range clamping happens in `XdpCpuMapCreate`.

### 7.2 Per-Frame Classification: Hash and CPU Selection

**Function:** `XdpInspect` → CPU redirect branch  
**IRQL:** DISPATCH_LEVEL

For each frame matching a `XDP_REDIRECT_TARGET_TYPE_CPU` rule:

1. **Extract 5-tuple** from frame headers (IP src/dst, protocol, UDP/TCP src/dst port) using the existing `XdpParseFrame` infrastructure.

2. **Compute 32-bit hash** using a fast mixing function over the 5-tuple fields. The hash is symmetric within a flow so the same (src, dst) 5-tuple always produces the same hash regardless of direction.

3. **Select target CPU:**
   ```c
   TargetCpu = (Hash % CpuCount) + CpuBase;
   ```
   This distributes flows across `[CpuBase, CpuBase + CpuCount)` uniformly for diverse 5-tuples.

4. **Set frame action:**
   ```c
   *RxAction = XDP_RX_ACTION_CPU_REDIRECT;
   ```

5. **Populate frame extension** (see Section 7.3).

### 7.3 Frame Extension Population

After computing `TargetCpu`, the inspection code populates all fields of `XDP_FRAME_CPU_REDIRECT` so the post-inspect path has everything it needs:

```c
XDP_FRAME_CPU_REDIRECT *CpuRedirect = XdpGetCpuRedirectExtension(Frame, &InspectionContext->CpuRedirectExtension);
CpuRedirect->TargetCpu      = TargetCpu;
CpuRedirect->CpuBase        = Rule->Redirect.CpuRedirect.TargetCpuBase;
CpuRedirect->CpuCount       = Rule->Redirect.CpuRedirect.TargetCpuCount;
CpuRedirect->RingDepth      = Rule->Redirect.CpuRedirect.RingDepth;
CpuRedirect->DrainBatchSize = Rule->Redirect.CpuRedirect.DrainBatchSize;
```

The `CpuRedirectExtension` offset in the frame ring is obtained at RX queue creation time and stored in `XDP_INSPECTION_CONTEXT`.

---

## 8. LWF Receive Path (recv.c)

### 8.1 CPU Redirect Extension Registration

**Function:** `XdpGenericRxCreateQueue` (extended)

The CPU redirect frame extension must be registered before any frames arrive so XDP allocates space for `XDP_FRAME_CPU_REDIRECT` in the frame ring:

```c
XdpInitializeExtensionInfo(
    &ExtensionInfo,
    XDP_FRAME_EXTENSION_CPU_REDIRECT_NAME,
    XDP_FRAME_EXTENSION_CPU_REDIRECT_VERSION_1,
    XDP_EXTENSION_TYPE_FRAME);
XdpRxQueueRegisterExtensionVersion(Config, &ExtensionInfo);
```

**Function:** `XdpGenericRxActivateQueue` (extended)

At queue activation, get the resolved extension offset for fast per-frame access:

```c
XdpInitializeExtensionInfo(
    &ExtensionInfo,
    XDP_FRAME_EXTENSION_CPU_REDIRECT_NAME,
    XDP_FRAME_EXTENSION_CPU_REDIRECT_VERSION_1,
    XDP_EXTENSION_TYPE_FRAME);
XdpRxQueueGetExtension(Config, &ExtensionInfo, &RxQueue->CpuRedirectExtension);
```

`RxQueue->CpuRedirectExtension` (an `XDP_EXTENSION` opaque offset) is then used in the post-inspect loop to access `XDP_FRAME_CPU_REDIRECT` in O(1) via `XdpGetCpuRedirectExtension(Frame, &RxQueue->CpuRedirectExtension)`.

### 8.2 Lazy CPUMAP Initialization

**Function:** `XdpGenericReceivePostInspectNbs`, inside the `XDP_RX_ACTION_CPU_REDIRECT` case

The CPUMAP is created on the first packet, not at program bind time, to avoid the ~1.6 GB PREALLOC allocation occurring before any traffic arrives:

```c
if (RxQueue->Generic->CpuMap == NULL) {
    UINT32 MapCpuBase  = CpuRedirect->CpuBase;
    UINT32 MapCpuCount = CpuRedirect->CpuCount != 0
        ? CpuRedirect->CpuCount
        : KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    UINT32 RingDepth = CpuRedirect->RingDepth != 0
        ? CpuRedirect->RingDepth
        : XDP_CPUMAP_RING_DEFAULT_CAPACITY;
    UINT32 BatchSize = CpuRedirect->DrainBatchSize;

    XDP_CPUMAP *NewMap = NULL;
    NTSTATUS Status = XdpCpuMapCreate(
        MapCpuBase, MapCpuCount, RingDepth, BatchSize,
        RxQueue->Generic->NdisFilterHandle, &NewMap);

    if (NT_SUCCESS(Status)) {
        // Race-safe install: if another RX queue beat us, destroy ours
        if (InterlockedCompareExchangePointer(
                (PVOID *)&RxQueue->Generic->CpuMap, NewMap, NULL) != NULL) {
            XdpCpuMapDestroy(NewMap);
        }
    } else {
        // Drop packet, record allocation failure
        NdisAppendSingleNblToNblQueue(DropList, ActionNbl);
        STAT_INC(&RxQueue->PcwStats, ForwardingFailuresAllocation);
        break;
    }
}
```

**Race safety:** Multiple RSS queues share `Generic->CpuMap`. The `InterlockedCompareExchangePointer` ensures only one CPUMAP is installed. The loser calls `XdpCpuMapDestroy` on its allocation. This is safe because `XdpCpuMapDestroy` at PASSIVE_LEVEL (via deferred work) is the normal teardown path, not DISPATCH_LEVEL — but since the newly-created map has `Active=TRUE` and no rings have been enqueued, `XdpCpuMapDestroy` immediately frees it without any DPC drain.

> **Important:** `XdpCpuMapCreate` requires PASSIVE_LEVEL due to `ExAllocatePoolZero` with `NonPagedPoolNx` and `NdisAllocateNetBufferListPool`. The lazy initialization in `recv.c` at DISPATCH_LEVEL works because `XdpCpuMapCreate` is also callable from DISPATCH_LEVEL for the allocation path — **verify this is correct for your NDIS version.** If NDIS pool creation requires PASSIVE_LEVEL, the lazy init must be reworked to use a work item.

### 8.3 Batch Collection and Flush

At the top of `XdpGenericReceivePostInspectNbs`:
```c
XDP_CPUMAP_BATCH CpuRedirectBatch;
XdpCpuMapBatchInit(&CpuRedirectBatch);
```

In the per-frame loop, for `XDP_RX_ACTION_CPU_REDIRECT`:
```c
if (!XdpCpuMapBatchAdd(&CpuRedirectBatch, ActionNbl, TargetCpu,
                        RxQueue->Generic->NdisFilterHandle, PortNumber)) {
    // Batch full (32 entries) — flush now and retry
    XdpCpuMapFlushBatch(RxQueue->Generic->CpuMap, &CpuRedirectBatch);
    XdpCpuMapBatchAdd(&CpuRedirectBatch, ActionNbl, TargetCpu, ...);
}
// Original NBL is returned to miniport (ownership transferred to clone)
NdisAppendSingleNblToNblQueue(DropList, ActionNbl);
```

After the frame loop:
```c
if (CpuRedirectBatch.Count > 0 && RxQueue->Generic->CpuMap != NULL) {
    XdpCpuMapFlushBatch(RxQueue->Generic->CpuMap, &CpuRedirectBatch);
}
```

### 8.4 CPUMAP Lifetime in the LWF

The `XDP_CPUMAP` pointer lives in `XDP_LWF_GENERIC::CpuMap` and is shared across all RX queues on the same interface.

**Creation:** Lazy, on first CPU-redirect packet (Section 8.2).

**Destruction:** In `XdpGenericRxDeleteQueue`, after the queue is removed from `Generic->Rx.Queues`, if the queue list is now empty and `CpuMap != NULL`, the CPUMAP is atomically taken under `Generic->Lock` and destroyed:

```c
XDP_CPUMAP *OldMap = NULL;
RtlAcquirePushLockExclusive(&Generic->Lock);
if (IsListEmpty(&Generic->Rx.Queues) && Generic->CpuMap != NULL) {
    OldMap = Generic->CpuMap;
    Generic->CpuMap = NULL;
}
RtlReleasePushLockExclusive(&Generic->Lock);
if (OldMap != NULL) {
    XdpCpuMapDestroy(OldMap);  // triggers stats dump
}
```

Also destroyed in `XdpGenericCleanupInterface` as a safety net for abnormal teardown.

---

## 9. LWF Generic Layer (generic.h / generic.c)

**`src/xdplwf/generic.h`** — two additions:

1. Forward declaration: `typedef struct _XDP_CPUMAP XDP_CPUMAP;`
2. New fields in `XDP_LWF_GENERIC`:
   ```c
   XDP_CPUMAP *CpuMap;               // Shared across all RX queues on this interface
   XDP_EXTENSION CpuRedirectExtension; // Extension offset (unused; per-queue used instead)
   ```

**`src/xdplwf/recv.h`** — new field in `XDP_LWF_GENERIC_RX_QUEUE`:
```c
XDP_EXTENSION CpuRedirectExtension;  // Per-queue frame extension offset
```

**`src/xdplwf/generic.c`** — `XdpGenericCleanupInterface`:
```c
if (Generic->CpuMap != NULL) {
    XdpCpuMapDestroy(Generic->CpuMap);
    Generic->CpuMap = NULL;
}
```

---

## 10. Test Tool: cpuredirect.exe

**File:** `test/cpuredirect/cpuredirect.c` (new file)  
**Project:** `test/cpuredirect/cpuredirect.vcxproj` (new project, added to `xdp.sln`)

A standalone command-line tool for attaching a CPU redirect XDP program to a network interface without needing the full DNS server setup.

**Usage:**
```
cpuredirect.exe <IfIndex> <Port> <CpuBase> <CpuCount> [RingDepth] [DrainBatch]
```

| Argument | Description |
|----------|-------------|
| `IfIndex` | NDIS interface index of the NIC |
| `Port` | UDP destination port to match |
| `CpuBase` | First target CPU (e.g., 56) |
| `CpuCount` | Number of target CPUs (e.g., 24) |
| `RingDepth` (optional) | Ring capacity, must be power-of-2 (0 = default 32768) |
| `DrainBatch` (optional) | DPC drain batch size (0 = default 256) |

**What it does:**
1. Opens the XDP API handle.
2. Constructs an XDP program with one rule: match all UDP on `Port`, redirect to CPU `[CpuBase, CpuBase + CpuCount)`.
3. Attaches the program to the interface.
4. Waits for Ctrl+C.
5. On exit, detaches the program (triggers CPUMAP destroy and stats dump).

**Validation in ParseArgs:**
- `RingDepth` must be a power of 2 or 0 (validated via `(v & (v-1)) == 0`).
- `DrainBatch` must be ≤ 256 (clamped to `XDP_CPUMAP_MAX_BATCH_SIZE`).

**Integration in `tools/setup.ps1`:** The `cpuredirect.exe` binary is included in the deployment package alongside other test binaries.

---

## 11. XDP Core RX Queue Extension Registration (rx.c)

**File:** `src/xdp/rx.c`

The CPU redirect frame extension must be registered in the XDP core RX queue infrastructure so that the frame ring allocates space for `XDP_FRAME_CPU_REDIRECT` alongside each frame.

### 11.1 Extension Registration Table

A new entry was added to the `XdpRxFrameExtensions[]` registration table:

```c
{
    .Info.ExtensionName     = XDP_FRAME_EXTENSION_CPU_REDIRECT_NAME,
    .Info.ExtensionVersion  = XDP_FRAME_EXTENSION_CPU_REDIRECT_VERSION_1,
    .Info.ExtensionType     = XDP_EXTENSION_TYPE_FRAME,
    .Size                   = sizeof(XDP_FRAME_CPU_REDIRECT),
    .Alignment              = __alignof(XDP_FRAME_CPU_REDIRECT),
    .InternalExtension      = TRUE,
},
```

Note: `InternalExtension = TRUE` means this extension is not exposed to user-mode consumers — it is purely for internal kernel-to-kernel communication between `xdp.sys` (inspection) and `xdplwf.sys` (post-inspect receive).

### 11.2 Extension Enabled at Queue Setup

In `XdpRxQueueSetCapabilities`, the CPU redirect extension is unconditionally enabled alongside the mandatory `RX_ACTION` extension:

```c
XdpExtensionSetEnableEntry(RxQueue->FrameExtensionSet, XDP_FRAME_EXTENSION_CPU_REDIRECT_NAME);
```

This ensures the extension is always present in the frame ring layout, even if no CPU redirect rule is currently attached. The overhead is one `sizeof(XDP_FRAME_CPU_REDIRECT)` (20 bytes) per frame slot.

### 11.3 Extension Offset Resolved at Queue Attach

In `XdpRxQueueAttachInterface`, the resolved extension offset is stored in the inspection context for fast per-frame access:

```c
XdpInitializeExtensionInfo(
    &ExtensionInfo, XDP_FRAME_EXTENSION_CPU_REDIRECT_NAME,
    XDP_FRAME_EXTENSION_CPU_REDIRECT_VERSION_1, XDP_EXTENSION_TYPE_FRAME);
XdpRxQueueGetExtension(
    ConfigActivate, &ExtensionInfo,
    &RxQueue->InspectionContext.CpuRedirectExtension);
```

This offset is then used by `programinspect.c` to write the `XDP_FRAME_CPU_REDIRECT` per-frame (Section 7.3) and by `recv.c` to read it (Section 8).

---

## 12. XDP Program Binding and Inspection Context (program.c / program.h)

### 12.1 Inspection Context Extension (program.h)

**File:** `src/xdp/program.h`

A new `XDP_EXTENSION CpuRedirectExtension` field was added to `XDP_INSPECTION_CONTEXT`:

```c
typedef struct _XDP_INSPECTION_CONTEXT {
    XDP_INSPECTION_EBPF_CONTEXT EbpfContext;
    XDP_REDIRECT_CONTEXT RedirectContext;
    ULONG IfIndex;
    XDP_EXTENSION CpuRedirectExtension;  // ← NEW
} XDP_INSPECTION_CONTEXT;
```

This stores the resolved frame extension offset for the CPU redirect extension, populated at queue attach time (Section 11.3) and used by the inspection code (Section 7.3) to write `XDP_FRAME_CPU_REDIRECT` data into each frame.

### 12.2 Program Binding Attach (program.c)

**File:** `src/xdp/program.c`

The `XdpProgramBindingAttach` function was extended to handle `XDP_REDIRECT_TARGET_TYPE_CPU` in the redirect rule switch:

```c
case XDP_REDIRECT_TARGET_TYPE_CPU:
    //
    // CPU redirect parameters already validated in XdpProgramValidateRule.
    // No additional validation needed at bind time.
    //
    break;
```

Unlike `XDP_REDIRECT_TARGET_TYPE_XSK`, which requires opening and referencing an XSK socket handle at bind time, CPU redirect has no external handle — the parameters (CpuBase, CpuCount, RingDepth, DrainBatchSize) are fully self-contained in the validated rule. The `default` case was also changed from `break` to `STATUS_NOT_SUPPORTED` + `goto Exit` to catch unrecognized target types as errors.

---

## 13. Build Integration (vcxproj, sln, precomp.h)

### 13.1 xdp.sys Build (src/xdp/xdp.vcxproj)

The new `cpumap.c` source file was added to the `xdp.sys` kernel driver project:

```xml
<ClCompile Include="cpumap.c" />
```

### 13.2 xdp.sys Precompiled Header (src/xdp/precomp.h)

`#include "cpumap.h"` was added to the precompiled header so all `xdp.sys` source files have access to the CPUMAP internal types and functions.

### 13.3 xdplwf.sys Precompiled Header (src/xdplwf/precomp.h)

`#include <xdpcpumap.h>` was added to the LWF precompiled header so `recv.c` and `generic.c` can use the public CPUMAP API (`XdpCpuMapCreate`, `XdpCpuMapDestroy`, `XdpCpuMapFlushBatch`, etc.) and the `XDP_FRAME_CPU_REDIRECT` extension type.

### 13.4 Solution File (xdp.sln)

The `cpuredirect` test tool project was added to the Visual Studio solution with Debug/Release × x64/ARM64 build configurations:

```
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "cpuredirect",
    "test\cpuredirect\cpuredirect.vcxproj",
    "{8FA3C9E2-1D7B-4F89-A3C1-9E5F2B8D4C6A}"
```

---

## 14. Packet Fuzzer Stubs (pktfuzz)

The `pktfuzz` test harness links against `programinspect.c` for fuzz testing of packet parsing. Since `programinspect.c` now references `xdpcpumap.h` types and `KeQueryActiveProcessorCountEx`, two stub files were updated:

### 14.1 Precompiled Header (test/pktfuzz/precomp.h)

```c
#include <xdpcpumap.h>  // ← NEW: needed by programinspect.c CPU redirect path
```

### 14.2 Kernel Function Stubs (test/pktfuzz/stubs/ntos.h)

Added a stub for `KeQueryActiveProcessorCountEx` that returns 64 during fuzzing:

```c
#ifndef ALL_PROCESSOR_GROUPS
#define ALL_PROCESSOR_GROUPS 0xffff
#endif

inline
ULONG
KeQueryActiveProcessorCountEx(
    _In_ USHORT GroupNumber
    )
{
    UNREFERENCED_PARAMETER(GroupNumber);
    return 64;  // Reasonable CPU count for fuzzing
}
```

This ensures the `XdpProgramValidateRule` CPU range validation in `programinspect.c` works correctly in the user-mode fuzz environment without requiring actual kernel APIs.

---

## 15. Deployment Bugfix: Certificate Import (tools/setup.ps1)

**File:** `tools/setup.ps1`

The `Install-DriverCertificate` function was changed from using the PowerShell `Import-Certificate` cmdlet to `certutil.exe` for test-signing certificate installation. This is a **separate bugfix** unrelated to CPUMAP, but included in the branch.

**Before:**
```powershell
Import-Certificate -FilePath $CertRootFileName -CertStoreLocation 'cert:\localmachine\root'
Import-Certificate -FilePath $CertFileName -CertStoreLocation 'cert:\localmachine\trustedpublisher'
```

**After:**
```powershell
& certutil -f -addstore Root $CertRootFileName
& certutil -f -addstore TrustedPublisher $CertFileName
```

**Why:** `Import-Certificate` has compatibility issues on some Windows Server versions where the cmdlet silently fails or the certificate is imported into the wrong store. `certutil.exe` provides consistent behavior across all Windows versions and explicit exit code checking.

---

## 16. Build-Time Feature Flags

Defined in `src/xdp/cpumap.h`, overridable via MSBuild preprocessor defines:

| Flag | Default | Description |
|------|---------|-------------|
| `XDP_CPUMAP_DRAIN_ALL` | `0` | Drain entire ring per DPC invocation. Better throughput, potential DPC starvation. |

To enable DRAIN_ALL mode at build time:
```xml
<!-- In cpumap.vcxproj or Directory.Build.props -->
<PreprocessorDefinitions>XDP_CPUMAP_DRAIN_ALL=1;...</PreprocessorDefinitions>
```

---

## 17. Performance Design Decisions

### Lock Acquisitions (Bottleneck #3 reduction)

| Scenario | Before (per-packet enqueue) | After (batch enqueue) |
|----------|----------------------------|----------------------|
| 32 packets → 24 different CPUs | 32 lock acq/rel | 24 lock acq/rel |
| 32 packets → 1 CPU | 32 lock acq/rel | **1** lock acq/rel |
| 32 packets → 4 CPUs | 32 lock acq/rel | **4** lock acq/rel |

### Clone Pool Contention (Bottleneck #1 eliminated)

| | Before | After (per-source pools) |
|--|--------|--------------------------|
| Pool count | 1 (shared) | 80 (one per system CPU) |
| Peak ops/sec on hottest pool | ~2M | ~250K |
| L1 lookaside behavior | Exhausted → pool spinlock | Typically lock-free |

With per-source-CPU clone pools, each RSS CPU allocates from its own NDIS pool with L1 lookaside behavior.

### DPC Insert Reduction (Bottleneck #4)

`KeInsertQueueDpc` costs ~50–100 ns (involves an interlocked operation on the target CPU's DPC queue). The batch flush calls it once per unique target CPU per batch, not once per packet.

### Ring Depth (Bottleneck #5)

Default ring depth is `XDP_CPUMAP_RING_DEFAULT_CAPACITY = 32768` (up from the original 32 in the stub implementation). Configurable per-session via `cpuredirect.exe` `[RingDepth]` argument.

---

## 18. Commit-by-Commit Walkthrough

The branch contains 7 commits, each building on the previous. Here is a walkthrough of what each commit introduces:

### Commit 1: `a1e997c` — "phase 1"

**Foundation commit.** Introduces the core CPUMAP infrastructure:
- New public API types: `XDP_RX_ACTION_CPU_REDIRECT`, `XDP_REDIRECT_TARGET_TYPE_CPU`, `XDP_CPU_REDIRECT_PARAMS` in `published/external/xdp/`.
- New private header `published/private/xdpcpumap.h` with `XDP_FRAME_CPU_REDIRECT` frame extension and `XDP_CPUMAP` lifecycle/batch APIs.
- New files `src/xdp/cpumap.h` and `src/xdp/cpumap.c` with the initial CPUMAP implementation (single shared clone pool, small ring, per-packet enqueue).
- Frame extension registration in `src/xdp/rx.c`.
- CPU redirect handling in `programinspect.c` (initial hash function) and `recv.c` (lazy CPUMAP creation, single-packet enqueue).
- LWF generic layer additions (`generic.h`, `generic.c`) for CPUMAP pointer and cleanup.
- Test tool `test/cpuredirect/cpuredirect.c` and its `.vcxproj`.
- Build integration: `xdp.vcxproj`, `precomp.h` files, `xdp.sln`.
- Pktfuzz stubs for `KeQueryActiveProcessorCountEx`.

### Commit 2: `9827df5` — "better hashing"

Improved the 5-tuple hash function in `programinspect.c`:
- Replaced simple XOR-based hash with a **symmetric hash** using `min/max` IP decomposition and `rotl(max, 16)` to avoid XOR cancellation.
- Added murmur3 32-bit finalizer (mix constants `0x85ebca6b`, `0xc2b2ae35`) for better avalanche across non-power-of-2 CPU counts.

### Commit 3: `fc43a38` — "hash fix"

Bug fix for hash computation:
- Fixed port combination in the hash to use symmetric `min in low word, max in high word` pattern.
- Added golden-ratio multiplicative mixing (`0x9E3779B9`) between IP hash and port hash for better entropy.
- Extended hash to handle TCP flows (in addition to UDP).

### Commit 4: `849043e` — "per source nbl pool"

First major performance optimization:
- Replaced single shared `NblClonePool` with **per-source-CPU clone pools** (`ClonePoolCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS)`).
- Each RSS CPU allocates clones from its own NDIS pool, eliminating the pool spinlock contention bottleneck (~2M lock acq/sec at 200K+ PPS).
- Added CPU hot-add guard: if `KeGetCurrentProcessorIndex() >= ClonePoolCount`, falls back to pool 0.

### Commit 5: `7c7d69c` — "batch enqueue nbls"

Second major performance optimization:
- Introduced `XDP_CPUMAP_BATCH` with 32-entry batching API (`XdpCpuMapBatchInit`, `XdpCpuMapBatchAdd`, `XdpCpuMapFlushBatch`).
- **Phase 1** (no locks): clone all NBLs in a tight loop.
- **Phase 2** (one lock per target CPU): group by target CPU, acquire ring lock once per unique target, enqueue all clones for that target, one DPC insert per target.
- Reduced lock acquisitions from N (per packet) to T (per unique target CPU in batch).
- Updated `recv.c` to use batch API instead of per-packet `XdpCpuMapEnqueue`.

### Commit 6: `287a84a` — "cpumap: perf optimizations - bulk alloc, drain loop, 32K ring, stats"

Comprehensive performance tuning:
- **Ring depth increase:** Default from 32 → 32,768 entries to absorb bursts.
- **Drain-until-empty inner loop:** DPC loops inside itself (up to `MAX_LOOP_ITERATIONS`) rather than re-queuing between batches, eliminating the DPC scheduling gap.
- **`DRAIN_ALL` mode option:** Alternative drain strategy that dequeues the entire ring per DPC (stashes `IsDeepCopy` in `NBL->MiniportReserved[1]`).
- **16+ per-ring statistics counters** (`EnqueueCount`, `DrainCount`, `DropCount`, `RingFullCount`, `CloneFailCount`, `DpcInvokeCount`, `DpcMaxBatchDrained`, `DpcRequeueCount`, `MaxRingDepth`, `EnqueueBatchCount`, `DpcLoopIterations`, `DpcMaxLoopIterations`, `DpcEmptyCount`, `LockWaitCycles`, `LockAcquireCount`).
- **Stats dump on destroy** via `DbgPrintEx` with loss percentage calculation and TSC-calibrated lock timing.

### Commit 7: `a37ecac` — "xdp/cpumap: selective CPU range allocation, configurable ring depth and drain batch"

Final optimization and configurability:
- **Selective CPU range allocation:** Rings and DPCs allocated only for `[CpuBase, CpuBase+CpuCount)` target CPUs instead of all system CPUs. Memory savings: ~45 MB → ~18 MB for 80→24 CPU case.
- **Configurable ring depth and drain batch:** `XDP_CPU_REDIRECT_PARAMS.RingDepth` and `.DrainBatchSize` exposed to user-mode via `cpuredirect.exe` arguments.
- **DPC target affinity:** Each DPC pinned to its specific target CPU via `KeSetTargetProcessorDpcEx`.
- Updated `cpuredirect.exe` with optional `[RingDepth]` and `[DrainBatch]` arguments.
- Deployment bugfix in `tools/setup.ps1` (certutil for certificate import).

---

## 19. Complete File Change Summary

| # | File | Type | Lines | Description |
|---|------|------|-------|-------------|
| 1 | `published/external/xdp/datapath.h` | Modified | +1 | Added `XDP_RX_ACTION_CPU_REDIRECT` to `XDP_RX_ACTION` enum |
| 2 | `published/external/xdp/program.h` | Modified | +13/-1 | Added `XDP_REDIRECT_TARGET_TYPE_CPU`, `XDP_CPU_REDIRECT_PARAMS` struct, union in `XDP_REDIRECT_PARAMS` |
| 3 | `published/private/xdpcpumap.h` | **New** | +117 | Frame extension type, CPUMAP lifecycle APIs, batch enqueue API (kernel-mode header shared between xdp.sys and xdplwf.sys) |
| 4 | `src/xdp/cpumap.c` | **New** | +1021 | Full CPUMAP implementation: create/destroy, enqueue (single + batch), drain DPC, per-CPU clone pools, AZC deep-copy fallback, stats |
| 5 | `src/xdp/cpumap.h` | **New** | +141 | Internal types: `XDP_CPUMAP_RING`, `XDP_CPUMAP_ENTRY`, `XDP_CPUMAP` struct, feature flag defines |
| 6 | `src/xdp/precomp.h` | Modified | +1 | `#include "cpumap.h"` |
| 7 | `src/xdp/program.c` | Modified | +9/-1 | Handle `XDP_REDIRECT_TARGET_TYPE_CPU` in binding attach; reject unknown target types |
| 8 | `src/xdp/program.h` | Modified | +1 | `XDP_EXTENSION CpuRedirectExtension` added to `XDP_INSPECTION_CONTEXT` |
| 9 | `src/xdp/programinspect.c` | Modified | +174/-6 | CPU redirect classification: 5-tuple hash, symmetric hash, murmur3 finalizer, frame extension population; rule validation; rule delete handling |
| 10 | `src/xdp/rx.c` | Modified | +14 | CPU redirect frame extension registration, enable, and offset resolution |
| 11 | `src/xdp/xdp.vcxproj` | Modified | +1 | Added `cpumap.c` to build |
| 12 | `src/xdplwf/generic.c` | Modified | +8 | CPUMAP cleanup in `XdpGenericCleanupInterface` |
| 13 | `src/xdplwf/generic.h` | Modified | +11 | `XDP_CPUMAP *CpuMap` and `XDP_EXTENSION CpuRedirectExtension` in `XDP_LWF_GENERIC` |
| 14 | `src/xdplwf/precomp.h` | Modified | +1 | `#include <xdpcpumap.h>` |
| 15 | `src/xdplwf/recv.c` | Modified | +122 | Post-inspect `XDP_RX_ACTION_CPU_REDIRECT` handler: lazy CPUMAP creation, batch collect/flush; extension offset resolution; CPUMAP teardown on last queue delete |
| 16 | `src/xdplwf/recv.h` | Modified | +1 | `XDP_EXTENSION CpuRedirectExtension` in `XDP_LWF_GENERIC_RX_QUEUE` |
| 17 | `test/cpuredirect/cpuredirect.c` | **New** | +266 | Standalone test tool for attaching CPU redirect XDP programs |
| 18 | `test/cpuredirect/cpuredirect.vcxproj` | **New** | +18 | MSBuild project for cpuredirect.exe |
| 19 | `test/pktfuzz/precomp.h` | Modified | +1 | `#include <xdpcpumap.h>` for fuzzer compatibility |
| 20 | `test/pktfuzz/stubs/ntos.h` | Modified | +18 | Stub `KeQueryActiveProcessorCountEx` (returns 64) and `ALL_PROCESSOR_GROUPS` define |
| 21 | `tools/setup.ps1` | Modified | +14/-2 | Certificate import switched from `Import-Certificate` to `certutil.exe` |
| 22 | `xdp.sln` | Modified | +10 | Added cpuredirect project to solution with all configurations |

---

## 20. Known Limitations and Future Work

### Lazy CPUMAP Initialization at DISPATCH_LEVEL

`XdpCpuMapCreate` is called from `recv.c` at `DISPATCH_LEVEL` on the first packet. If NDIS pool creation (`NdisAllocateNetBufferListPool`) internally requires `PASSIVE_LEVEL` on some driver versions, this will bugcheck. A work item deferral is the safe fix if this proves to be an issue.

### Spinlock on Ring (Future: Lock-Free SPSC Rings)

Each `XDP_CPUMAP_RING` is shared among multiple source CPUs (RSS queues) under a `KSPIN_LOCK`. Under high fan-out (8 source CPUs × 24 target CPUs), each ring's lock is contended by up to 8 producers. Replacing MPSC rings with per-(source, target) SPSC rings (zero atomics on producer path, scan-all-sources on drain) would eliminate this remaining lock contention.

### Single Rule Per Program

The current implementation assumes the XDP program has a single CPU redirect rule. Multiple CPU redirect rules with different `CpuBase`/`CpuCount` ranges would require multiple CPUMAPs or a unified CPUMAP covering the union of all ranges.

### No Egress (TX) Path

CPU redirect only operates on the RX inspect path. TX path CPU affinity is not implemented.

### `NDIS_RECEIVE_FLAGS_RESOURCES` Synchronous Drain

For non-AZC (clone-based) maps, the drain DPC uses `NDIS_RECEIVE_FLAGS_RESOURCES`, which forces TCP/IP to process the packet synchronously before the DPC returns the NBL. Under heavy stack load this adds latency to the DPC. For AZC maps with `CanPend`, originals are indicated without `RESOURCES` and returned asynchronously by tcpip; deep-copy fallback NBLs are still indicated with `RESOURCES`.
