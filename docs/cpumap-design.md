# CPUMAP: Software Packet Redistribution for XDP on Windows

> **Branch:** `user/nijos/xdp-cpumap-azc`
> **Base:** `main`
> **Status:** Primary design document for XDP CPUMAP.

---

## Table of Contents

1. [What Problem Does CPUMAP Solve?](#1-what-problem-does-cpumap-solve)
2. [Background: How Packets Reach Your Application](#2-background-how-packets-reach-your-application)
3. [The CPUMAP Idea](#3-the-cpumap-idea)
4. [Architecture Overview](#4-architecture-overview)
5. [End-to-End Packet Flow](#5-end-to-end-packet-flow)
6. [The Two Packet Modes: Zero-Copy vs Deep-Copy](#6-the-two-packet-modes-zero-copy-vs-deep-copy)
7. [Component Deep Dives](#7-component-deep-dives)
   - 7.1 [User-Mode API (Public Headers)](#71-user-mode-api-public-headers)
   - 7.2 [XDP Program Inspection (programinspect.c)](#72-xdp-program-inspection-programinspectc)
   - 7.3 [Frame Extension: The Message Between Drivers](#73-frame-extension-the-message-between-drivers)
   - 7.4 [LWF Receive Path (recv.c)](#74-lwf-receive-path-recvc)
   - 7.5 [CPUMAP Core (cpumap.c / cpumap.h)](#75-cpumap-core-cpumapc--cpumaph)
   - 7.6 [Ring Buffer Design](#76-ring-buffer-design)
   - 7.7 [Batch Enqueue: Minimizing Lock Contention](#77-batch-enqueue-minimizing-lock-contention)
   - 7.8 [Drain DPC: Delivering Packets on the Target CPU](#78-drain-dpc-delivering-packets-on-the-target-cpu)
   - 7.9 [Deep-Copy Fallback Pool](#79-deep-copy-fallback-pool)
8. [Flow-Affinity Hashing](#8-flow-affinity-hashing)
9. [Lazy Initialization](#9-lazy-initialization)
10. [Lifecycle and Teardown](#10-lifecycle-and-teardown)
11. [Statistics and Diagnostics](#11-statistics-and-diagnostics)
12. [Test Tool: cpuredirect.exe](#12-test-tool-cpuredirectexe)
13. [File Change Summary](#13-file-change-summary)
14. [Known Limitations and Future Work](#14-known-limitations-and-future-work)

---

## 1. What Problem Does CPUMAP Solve?

When a network packet arrives at a NIC (Network Interface Card), the hardware must decide which CPU core should process it. Modern NICs use a feature called **RSS (Receive Side Scaling)** to distribute packets across multiple CPU cores. RSS works by hashing packet headers (IP addresses, ports) and mapping each hash to one of several **RSS queues**, where each queue is pinned to a specific CPU core.

When the number of active RSS queues is less than the number of available CPU cores, incoming packet processing is limited to a subset of CPUs. The remaining cores sit idle even under heavy load.

```
                    Server with N CPU cores
    ┌──────────────────────────────────────────────────┐
    │  CPU 0  CPU 1  ...  CPU K  CPU K+1 ...  CPU N   │
    │    ▲      ▲           ▲      x           x      │
    │    │      │           │    idle         idle     │
    └────┼──────┼───────────┼──────────────────────────┘
         │      │           │
    ┌────┴──────┴───────────┴──┐
    │   NIC with K RSS queues  │
    │   Q0  Q1  Q2 ... QK     │
    └──────────────────────────┘
                ▲
          Network traffic
```

CPUMAP enables **software packet redistribution**: it intercepts packets after they arrive on RSS CPUs and steers them to additional CPUs, spreading the processing load across more cores. This increases aggregate throughput and reduces per-CPU saturation.

Linux provides similar functionality through `SO_REUSEPORT` (kernel distributes sockets across CPUs), **RPS** (Receive Packet Steering), and **RFS** (Receive Flow Steering). Windows has no built-in equivalent. **CPUMAP fills this gap.**

---

## 2. Background: How Packets Reach Your Application

Before diving into CPUMAP, it helps to understand the normal packet path on Windows. Think of it as a pipeline with several stages:

```
    ┌─────────────┐
    │     NIC      │  Hardware receives the packet from the wire
    └──────┬───────┘
           │ Interrupt on RSS CPU
           ▼
    ┌─────────────┐
    │  Miniport    │  NIC driver converts hardware buffers to NDIS
    │   Driver     │  structures (NBLs) and indicates them upward
    └──────┬───────┘
           │ NdisMIndicateReceiveNetBufferLists()
           ▼
    ┌─────────────┐
    │  NDIS / LWF  │  Lightweight Filter drivers can inspect or
    │   Filters    │  modify packets (this is where XDP hooks in)
    └──────┬───────┘
           │ NdisFIndicateReceiveNetBufferLists()
           ▼
    ┌─────────────┐
    │   TCP/IP     │  The OS networking stack parses headers,
    │    Stack     │  manages connections, finds the right socket
    └──────┬───────┘
           │
           ▼
    ┌─────────────┐
    │ Application  │  Your app (DNS server, game server, etc.)
    │   Socket     │  receives the packet via recv() / recvfrom()
    └─────────────┘
```

**Key concept: NBL (Net Buffer List).**
An NBL is Windows NDIS's data structure representing a network packet. Think of it as an envelope containing the packet data plus metadata (which interface it came from, hash values, etc.). Every packet in the system is wrapped in an NBL as it moves through the stack.

**Key concept: CanPend.**
When the miniport driver hands NBLs up to the filter stack, it tells the filters whether they can **hold onto** those NBLs for a while (`CanPend = TRUE`) or must **return them immediately** (`CanPend = FALSE`, indicated with the `NDIS_RECEIVE_FLAGS_RESOURCES` flag). This distinction is critical for CPUMAP because it determines whether we can move the original packet to another CPU or must copy it first.

---

## 3. The CPUMAP Idea

CPUMAP adds a software redistribution layer that sits inside XDP's Lightweight Filter (LWF) driver. Instead of letting all packets flow up the stack on the same 8 RSS CPUs, CPUMAP:

1. **Intercepts** packets as they arrive on the RSS CPU
2. **Hashes** the packet's flow identifiers (IP addresses + ports) to pick a target CPU
3. **Moves** the packet to the target CPU's ring buffer
4. **Re-indicates** the packet into the TCP/IP stack from the target CPU

The application doesn't need to change at all. From TCP/IP's perspective, the packet simply "arrived" on a different CPU.

```
    BEFORE CPUMAP                         AFTER CPUMAP
    (8 RSS queues = 8 CPUs)              (8 RSS queues --> 24 target CPUs)

    RSS Q0 --> CPU 0 --> App             RSS Q0 --> CPU 0 -+-> CPU 56 --> App
    RSS Q1 --> CPU 1 --> App                               +-> CPU 57 --> App
    RSS Q2 --> CPU 2 --> App             RSS Q1 --> CPU 1 -+   ...
      ...                                                  +-> CPU 78 --> App
    RSS Q7 --> CPU 7 --> App             RSS Q7 --> CPU 7 -+-> CPU 79 --> App

    Max: ~650K pps                       Max: ~1.9M+ pps
```

---

## 4. Architecture Overview

CPUMAP spans two kernel-mode drivers that cooperate via a well-defined boundary:

```
  +------------------------------------------------------------------+
  |                        USER MODE                                  |
  |                                                                   |
  |   cpuredirect.exe (test tool)                                     |
  |     XdpCreateProgram(                                             |
  |       IfIndex, XDP_MATCH_UDP_DST,                                 |
  |       XDP_REDIRECT_TARGET_TYPE_CPU,                               |
  |       {CpuBase=56, CpuCount=24, RingDepth=32768})                |
  |                                                                   |
  +-----------------------------+-------------------------------------+
  |          xdp.sys            |          xdplwf.sys                  |
  |     (XDP Core Driver)      |   (Lightweight Filter Driver)        |
  |                             |                                     |
  |  +----------------------+  |  +-------------------------------+   |
  |  |   Control Plane      |  |  |   NDIS Filter Entry Point     |   |
  |  |  - Validate rules    |  |  |  FilterReceiveNetBufferLists  |   |
  |  |  - Register frame    |  |  |           |                   |   |
  |  |    extensions        |  |  |           v                   |   |
  |  |  - Bind programs     |  |  |  XdpGenericReceive()          |   |
  |  +----------+-----------+  |  |  - Convert NBLs to XDP        |   |
  |             | NMR dispatch |  |    frame ring entries          |   |
  |             v              |  |           |                   |   |
  |  +----------------------+  |  |           v                   |   |
  |  |   Inspection Engine  |<-+--+  NMR dispatch->Inspect()      |   |
  |  |  (programinspect.c)  |--+-->          |                   |   |
  |  |  - Parse headers     |  |  |           v                   |   |
  |  |  - Hash 5-tuple      |  |  |  Post-Inspect (recv.c)        |   |
  |  |  - Select target CPU |  |  |  - Read CPU_REDIRECT ext      |   |
  |  |  - Write frame ext   |  |  |  - Lazy-init CPUMAP           |   |
  |  +----------------------+  |  |  - BatchAdd -> FlushBatch      |   |
  |                             |  |           |                   |   |
  |  +----------------------+  |  |           |                   |   |
  |  |   CPUMAP Engine      |<-+--+           v                   |   |
  |  |   (cpumap.c)         |  |  |  Enqueue to target ring       |   |
  |  |  - Ring buffers      |  |  |  Schedule target DPC          |   |
  |  |  - DPC drain         |  |  |           |                   |   |
  |  |  - Deep-copy pool    |  |  |           |                   |   |
  |  +----------+-----------+  |  |           |                   |   |
  |             | DPC fires    |  |           |                   |   |
  |             v on target    |  |           v                   |   |
  |  +----------------------+  |  |  NdisFIndicateReceive-        |   |
  |  |  XdpCpuMapDrainDpc   |--+-->  NetBufferLists()             |   |
  |  |  - Dequeue from ring |  |  |  (on target CPU -> TCP/IP)    |   |
  |  |  - Build NBL chain   |  |  |                               |   |
  |  +----------------------+  |  +-------------------------------+   |
  +-----------------------------+-------------------------------------+
```

**Who owns what:**

| Responsibility | Driver | Key Functions |
|---|---|---|
| Rule validation, program binding | xdp.sys | `XdpProgramValidateRule` |
| Per-packet inspection and hashing | xdp.sys | `XdpInspect` in programinspect.c |
| CPUMAP data structures, rings, DPCs | xdp.sys | cpumap.c (`Create`, `FlushBatch`, `DrainDpc`, `Destroy`) |
| NDIS filter callbacks, NBL handling | xdplwf.sys | recv.c (`XdpGenericReceivePostInspectNbs`) |
| Lazy CPUMAP creation | xdplwf.sys | recv.c (calls `XdpCpuMapCreate`) |
| Re-indication to TCP/IP | xdplwf.sys (via DPC in xdp.sys) | `NdisFIndicateReceiveNetBufferLists` |
| CPUMAP cleanup | xdplwf.sys | generic.c (calls `XdpCpuMapDestroy`) |

The **frame extension** (`XDP_FRAME_CPU_REDIRECT`) is the single communication channel between the two drivers during data-path processing. The inspection engine in xdp.sys writes it; the post-inspect path in xdplwf.sys reads it. Both run synchronously on the same RSS CPU.

---

## 5. End-to-End Packet Flow

Here is the complete journey of a single UDP packet through CPUMAP, from wire to application:

```
 +-------------------------------------------------------------------+
 |                    RSS CPU (e.g., CPU 3)                           |
 |                                                                    |
 |  1. NIC receives packet, RSS hashes it to Queue 3 -> CPU 3        |
 |                         |                                          |
 |  2. Miniport driver indicates NBL upward                           |
 |     NdisMIndicateReceiveNetBufferLists(NBL, CanPend=TRUE)          |
 |                         |                                          |
 |  3. xdplwf.sys FilterReceiveNetBufferLists() intercepts            |
 |                         |                                          |
 |  4. XdpGenericReceive() converts NBL to XDP frame ring             |
 |                         |                                          |
 |  5. NMR dispatch calls XdpInspect() in xdp.sys                    |
 |     +-------------------------------------------+                  |
 |     |  Parse frame headers (IP + UDP)           |                  |
 |     |  Match rule: UDP dst port == 53?          |                  |
 |     |    YES -> Hash 5-tuple symmetrically      |                  |
 |     |    TargetCpu = (hash % 24) + 56 = 63     |                  |
 |     |    Write {TargetCpu=63, Base=56,           |                  |
 |     |           Count=24} into frame ext         |                  |
 |     |    Return XDP_RX_ACTION_CPU_REDIRECT       |                  |
 |     +-------------------------------------------+                  |
 |                         |                                          |
 |  6. recv.c post-inspect reads frame extension                      |
 |     - First packet? -> Lazy-init CPUMAP (allocate rings, DPCs)     |
 |     - XdpCpuMapBatchAdd(NBL, TargetCpu=63)                        |
 |     - ... process remaining packets in batch ...                   |
 |                         |                                          |
 |  7. XdpCpuMapFlushBatch() at end of indication batch               |
 |     +-------------------------------------------+                  |
 |     |  Phase 1 (no locks):                      |                  |
 |     |    CanPend? -> Use original NBL as-is      |                  |
 |     |    !CanPend? -> Deep-copy packet data      |                  |
 |     |                                            |                  |
 |     |  Phase 2 (one lock per target CPU):       |                  |
 |     |    Lock Ring[63-56=7]                      |                  |
 |     |    Enqueue NBL into ring slot              |                  |
 |     |    Unlock                                  |                  |
 |     |    KeInsertQueueDpc(DPC for CPU 63)        |                  |
 |     +-------------------------------------------+                  |
 |                         |                                          |
 |  8. Return original NBLs to miniport (for !CanPend) or             |
 |     let them flow through the ring (for CanPend)                   |
 |                                                                    |
 +-------------------------------------------------------------------+
                           |
                    DPC scheduled
                           |
                           v
 +-------------------------------------------------------------------+
 |                   TARGET CPU (CPU 63)                              |
 |                                                                    |
 |  9. XdpCpuMapDrainDpc() fires                                     |
 |     +-------------------------------------------+                  |
 |     |  Lock Ring[7]                             |                  |
 |     |  Dequeue up to 256 NBLs                   |                  |
 |     |  Build chain: NBL1 -> NBL2 -> ...         |                  |
 |     |  Unlock                                   |                  |
 |     |                                            |                  |
 |     |  Split chain into:                        |                  |
 |     |    Originals -> indicate normally          |                  |
 |     |    Deep-copies -> indicate w/ RESOURCES    |                  |
 |     |                   then recycle to pool     |                  |
 |     +-------------------------------------------+                  |
 |                         |                                          |
 | 10. NdisFIndicateReceiveNetBufferLists() from CPU 63               |
 |                         |                                          |
 | 11. TCP/IP processes packet on CPU 63                              |
 |                         |                                          |
 | 12. Application's recv() returns the DNS query on CPU 63           |
 |                                                                    |
 +-------------------------------------------------------------------+
```

**Steps 1-8** all happen on the RSS CPU at `DISPATCH_LEVEL` (the Windows kernel priority level for interrupt processing). **Steps 9-12** happen on the target CPU, also at `DISPATCH_LEVEL` for the DPC, then transitioning to user mode for the application.

---

## 6. The Two Packet Modes: Zero-Copy vs Deep-Copy

CPUMAP must handle two fundamentally different scenarios based on whether the miniport driver allows the filter to hold onto packets:

### CanPend = TRUE (Zero-Copy Path)

This is the fast, preferred path. The miniport driver allows filters to keep the NBL for asynchronous processing. CPUMAP simply takes the **original NBL** and places it directly into the target CPU's ring buffer. No memory allocation, no data copying.

```
    RSS CPU                          Target CPU
    +----------------+               +----------------+
    |  Original NBL  |               |                |
    |  from miniport |---- Ring ---->|  Same NBL      |
    |                |   (pointer)   |  indicated to  |
    |  (not returned |               |  TCP/IP        |
    |   to miniport  |               |                |
    |   yet)         |               |  TCP/IP returns |
    +----------------+               |  it eventually |
                                     +----------------+
```

The original NBL is only returned to the miniport after TCP/IP is done with it (asynchronous return). This avoids all copying overhead.

### CanPend = FALSE (Deep-Copy Path)

When the miniport indicates with `NDIS_RECEIVE_FLAGS_RESOURCES`, it is telling filters: "I need these NBLs back immediately, do not hold them." This happens with some NIC drivers that use a fixed pool of receive buffers.

CPUMAP must **copy the packet data** into an independent NBL before the original is returned:

```
    RSS CPU                          Target CPU
    +----------------+               +----------------+
    |  Original NBL  |               |                |
    |  from miniport |               |  Deep-copy NBL |
    |       |        |               |  (independent) |
    |       | memcpy |--- Ring ----->|  indicated to  |
    |       v        |  (deep copy)  |  TCP/IP with   |
    |  Returned to   |               |  RESOURCES     |
    |  miniport      |               |                |
    |  immediately   |               |  Recycled to   |
    |  via DropList  |               |  free pool     |
    +----------------+               +----------------+
```

The deep-copy path uses a **lazy fallback pool**: NBL structures are allocated on first use and recycled via a lock-free SList (interlocked singly-linked list). The data buffer pages are allocated by NDIS via `NdisRetreatNetBufferDataStart` and freed after indication via `NdisAdvanceNetBufferDataStart`. This avoids pre-allocating large memory pools for an exceptional code path.

**How the decision is made:** The `recv.c` post-inspect path checks the NDIS receive flags at the start of each indication batch and sets `Batch.CanPend` accordingly. This single boolean propagates through the entire batch.

---

## 7. Component Deep Dives

### 7.1 User-Mode API (Public Headers)

CPUMAP extends the existing XDP program API with two new enumeration values and one new structure:

**New action: `XDP_RX_ACTION_CPU_REDIRECT`** (`datapath.h`)
Added to the `XDP_RX_ACTION` enum alongside DROP, PASS, and TX. This tells the post-inspect path that a packet should be redirected to a different CPU.

**New redirect target: `XDP_REDIRECT_TARGET_TYPE_CPU`** (`program.h`)
Added alongside the existing `XDP_REDIRECT_TARGET_TYPE_XSK`. When creating an XDP program rule with `XDP_PROGRAM_ACTION_REDIRECT`, the caller sets `TargetType = CPU` to use CPUMAP instead of XSK (AF_XDP socket) redirect.

**New parameters structure: `XDP_CPU_REDIRECT_PARAMS`** (`program.h`)
Specifies the CPU redirect configuration:

| Field | Description | Default |
|---|---|---|
| `TargetCpuBase` | First CPU index in the target range (e.g., 40) | Required |
| `TargetCpuCount` | Number of CPUs to distribute across (e.g., 24) | Required |
| `RingDepth` | Per-CPU ring buffer capacity (must be power of 2) | 32,768 |
| `DrainBatchSize` | Max packets drained per DPC firing | 256 |
| `Flags` | See `XDP_CPU_REDIRECT_FLAG_*`. When `XDP_CPU_REDIRECT_FLAG_HASH_QUIC_CID` is set, uses QUIC Destination Connection ID for CPU selection instead of 5-tuple hash. Bits 8-15 encode CID byte offset, bits 16-23 encode CID byte length. | 0 |
| `IgnoreCpuBitmap` | 64-byte bitmap (512 CPUs). CPUs with their bit set are skipped as redirect targets. Used to exclude HT siblings of RSS CPUs. | All zeros (no CPUs ignored) |

The existing `XDP_REDIRECT_PARAMS` struct was extended with a union to hold either an XSK handle or CPU redirect parameters, selected by the `TargetType` discriminator.

### 7.2 XDP Program Inspection (programinspect.c)

The inspection engine runs in `xdp.sys` at `DISPATCH_LEVEL` on the RSS CPU. For CPU redirect rules, two things happen:

**Rule validation (control plane, at program creation time):**
- Verifies `CpuBase + CpuCount` does not exceed the system's active processor count
- Verifies `CpuCount > 0`
- Validates QUIC CID flags if set (offset + length within CID bounds)
- Stores the validated parameters in the compiled program rule

**Per-packet classification (data plane, at packet arrival time):**
- Parses the packet headers (Ethernet -> IP -> UDP/TCP)
- Checks whether the packet matches the rule (e.g., UDP destination port)
- If matched and QUIC CID hashing is enabled: extracts the MsQuic partition index from the QUIC Destination Connection ID using the system CPU count as the partition mask (matching MsQuic's internal `PartitionMask` computation). If the extracted partition falls within the CPUMAP range, steers directly to that CPU; otherwise remaps via modulo.
- If QUIC CID hashing is not enabled or the packet is not QUIC: computes a symmetric 5-tuple hash with murmur3 finalizer and selects a target CPU via modulo.
- Skips any target CPU that has its bit set in `IgnoreCpuBitmap`, advancing to the next CPU in the range.
- Writes the result into the frame's CPU_REDIRECT extension
- Returns `XDP_RX_ACTION_CPU_REDIRECT`

**CPU redirect has no kernel handle.** Unlike XSK redirect (which references an AF_XDP socket handle), CPU redirect is a pure configuration. The program rule carries all parameters. When the program is deleted, there is no external handle to dereference.

### 7.3 Frame Extension: The Message Between Drivers

The `XDP_FRAME_CPU_REDIRECT` frame extension is a small data structure attached to each XDP frame in the ring. It acts as the communication channel between the inspection engine (xdp.sys) and the post-inspect path (xdplwf.sys):

| Field | Written By | Read By | Purpose |
|---|---|---|---|
| `TargetCpu` | Inspection | Post-inspect | Which CPU this packet goes to |
| `CpuBase` | Inspection | Post-inspect (lazy init) | First CPU in the target range |
| `CpuCount` | Inspection | Post-inspect (lazy init) | Number of target CPUs |
| `RingDepth` | Inspection | Post-inspect (lazy init) | Ring buffer capacity |
| `DrainBatchSize` | Inspection | Post-inspect (lazy init) | DPC drain limit |
| `Flags` | Inspection | Post-inspect | Reserved behavioral flags |

**Why carry all parameters per-frame (not just the target CPU)?**
The CPUMAP is created lazily on the first redirected packet. At that point, the post-inspect path in xdplwf.sys needs to know the ring depth, CPU range, and drain batch size to allocate the right structures. Reading these from the XDP program rule would require cross-driver locking. Carrying them in the frame extension avoids this: the inspection path (which already processes each frame) stamps the parameters once, and the post-inspect path reads them directly.

The extension is registered as an **internal extension** (not visible to user-mode programs or NIC drivers). It is allocated in the XDP frame ring alongside other extensions like `RX_ACTION`.

### 7.4 LWF Receive Path (recv.c)

The `XdpGenericReceivePostInspectNbs` function in recv.c is where CPUMAP integrates into the existing XDP data path. After the inspection engine returns an action for each frame, this function dispatches based on the action:

- `PASS` -> indicate normally to TCP/IP
- `DROP` -> return to miniport
- `TX` -> transmit (hairpin)
- **`CPU_REDIRECT`** -> new path added by CPUMAP

For `CPU_REDIRECT`, the flow is:

1. **Read the frame extension** to get the target CPU and CPUMAP parameters.

2. **Lazy-initialize the CPUMAP** if this is the first redirected packet. Multiple RX queues share the same `Generic->CpuMap` pointer, so the initialization uses `InterlockedCompareExchangePointer` to handle races: if two threads try to create simultaneously, only one wins and the other's allocation is destroyed.

3. **Collect the redirect decision** via `XdpCpuMapBatchAdd()`. This adds the packet to a batch array (up to 32 entries) without taking any locks.

4. **Flush the batch** via `XdpCpuMapFlushBatch()` at the end of the indication. If the batch fills up mid-indication (32 packets all going to different CPUs), it flushes early and continues.

5. **Handle return of originals.** When `CanPend = FALSE`, the original NBL is immediately added to the drop list (returned to miniport). When `CanPend = TRUE` and the ring is full (overflow), the original is returned via `Batch->ReturnableOriginals`.

**Miniport indication tracking:** The receive path also tracks whether the miniport is indicating with or without `NDIS_RECEIVE_FLAGS_RESOURCES`, feeding this into the CPUMAP's diagnostic counters.

### 7.5 CPUMAP Core (cpumap.c / cpumap.h)

The CPUMAP is the central data structure that manages per-CPU ring buffers and DPCs:

```
    XDP_CPUMAP
    +---------------------------------------------+
    |  CpuBase = 56                               |
    |  CpuCount = 24                              |
    |  Active = TRUE                              |
    |  RefCount = 1                               |
    |                                             |
    |  DeepCopyNblPool --> NDIS NBL pool           |
    |  DeepCopyFreeList --> SList of recycled NBLs |
    |                                             |
    |  PerCpuRings[0..23] -->                     |
    |    +------------------+                     |
    |    | Ring for CPU 56  | <-- PerCpuDpcs[0]   |
    |    +------------------+                     |
    |    | Ring for CPU 57  | <-- PerCpuDpcs[1]   |
    |    +------------------+                     |
    |    |       ...        |                     |
    |    +------------------+                     |
    |    | Ring for CPU 79  | <-- PerCpuDpcs[23]  |
    |    +------------------+                     |
    +---------------------------------------------+
```

**Selective allocation:** Rings and DPCs are only allocated for the target CPU range (`CpuBase` to `CpuBase + CpuCount - 1`), not for all system CPUs. A 24-CPU target range allocates 24 rings, not 80. The ring index is computed as `TargetCpu - CpuBase`.

**DPC affinity:** Each DPC is initialized with `KeSetTargetProcessorDpcEx` to run on its corresponding target CPU. When `KeInsertQueueDpc` is called on the RSS CPU, Windows schedules the DPC to run on the target CPU.

**No clone pools:** Earlier iterations of CPUMAP allocated per-source-CPU NDIS clone pools (`NdisAllocateCloneNetBufferList`). The current implementation eliminates these entirely. When `CanPend = TRUE`, the original NBL is used directly (zero-copy). When `CanPend = FALSE`, a lightweight deep-copy pool handles the fallback (see Section 7.9).

### 7.6 Ring Buffer Design

Each per-CPU ring is a fixed-capacity circular buffer using a power-of-2 size for efficient modular indexing:

```
    XDP_CPUMAP_RING (cache-line aligned)
    +-----------------------------------------------------+
    |  Lock (KSPIN_LOCK)     <-- protects Head and Tail    |
    |  Head = 1000           <-- consumer index (DPC)      |
    |  Tail = 1005           <-- producer index (enqueue)  |
    |  Capacity = 32768      <-- power of 2                |
    |  Mask = 32767          <-- Capacity - 1              |
    |  DrainBatchSize = 256  <-- max per DPC drain         |
    |  [statistics counters]                               |
    |                                                      |
    |  Entries[0 .. 32767]:                                |
    |    +------+--------------+----------+--------+       |
    |    | Nbl  | FilterHandle | PortNum  | IsDC?  |       |
    |    +------+--------------+----------+--------+       |
    |    | ...  |     ...      |   ...    |  ...   |       |
    |    +------+--------------+----------+--------+       |
    +-----------------------------------------------------+
```

**Index arithmetic:** `slot = index & Mask` converts a monotonically increasing index to a ring slot. The ring is full when `(Tail + 1 - Head) > Capacity`. Head and Tail wrap around using natural integer overflow.

**Ring entry fields:**
- `Nbl` -- pointer to the packet (original or deep-copy)
- `FilterHandle` -- NDIS filter handle needed for re-indication
- `PortNumber` -- NDIS port number
- `IsDeepCopy` -- distinguishes originals from deep-copies for the drain path

**Lock:** Each ring has its own `KSPIN_LOCK`. Producers (RSS CPUs) and the consumer (target CPU DPC) both acquire this lock, but the batch design minimizes the number of acquisitions.

**Ring sizing:** At 32,768 entries, each ring consumes roughly 640 KB of NonPagedPool memory. With 24 target CPUs, total ring memory is approximately 15 MB. This is intentionally large to absorb bursts when the drain DPC on a target CPU cannot keep up momentarily.

### 7.7 Batch Enqueue: Minimizing Lock Contention

The batch mechanism is the key performance optimization. Without batching, every packet would require: acquire lock -> enqueue -> release lock -> schedule DPC. With batching, this becomes: collect N packets -> acquire lock once -> enqueue all N -> release lock -> schedule DPC once.

```
    Without batching:                With batching (N=5):

    Packet 1 -> Lock -> Enq -> Unlock  Packet 1 -+
    Packet 2 -> Lock -> Enq -> Unlock  Packet 2 -+
    Packet 3 -> Lock -> Enq -> Unlock  Packet 3 -+-- Lock -> Enq 5 -> Unlock -> 1 DPC
    Packet 4 -> Lock -> Enq -> Unlock  Packet 4 -+
    Packet 5 -> Lock -> Enq -> Unlock  Packet 5 -+

    5 lock acquisitions, 5 DPCs      1 lock acquisition, 1 DPC
```

**`XdpCpuMapFlushBatch` operates in two phases:**

**Phase 1 -- Prepare NBLs (no locks held):**
- For each batch entry, determine whether to use the original (CanPend) or deep-copy (!CanPend)
- Deep-copies are allocated from the fallback pool, data is copied via `MdlCopyMdlChainToMdlChainAtOffsetNonTemporal`
- Results stored in a local `Clones[]` array

**Phase 2 -- Group by target CPU and enqueue (one lock per target):**
- Scan the batch for the first unprocessed entry
- Find its target CPU and acquire that ring's lock
- Scan again for all other entries with the same target -> enqueue them all
- Release lock, schedule one DPC for that target
- Repeat for the next unprocessed entry with a different target

This O(N*T) algorithm (N entries, T unique targets) is efficient because N is capped at 32 and runs entirely at `DISPATCH_LEVEL` with no memory allocation.

**Lock contention profiling:** Each ring tracks `LockWaitCycles` (rdtsc cycles spent acquiring the spinlock) and `LockAcquireCount` (total acquisitions), enabling computation of average lock wait time. A `SourceCpuMask` bitmask tracks which RSS CPUs are contending for each ring.

### 7.8 Drain DPC: Delivering Packets on the Target CPU

The `XdpCpuMapDrainDpc` function fires on the target CPU and re-indicates packets into the TCP/IP stack. This is where the "CPU redirect" actually takes effect -- `NdisFIndicateReceiveNetBufferLists` is called from the target CPU, so TCP/IP and the application process the packet on that CPU.

**Drain strategy -- yield-aware batched drain:**
Each DPC invocation uses an inner loop. Each iteration dequeues up to `DrainBatchSize` (default 256) packets. After indicating each batch, the DPC checks whether more packets remain. If more work exists, it calls `KeShouldYieldProcessor()` to ask the OS whether the DPC has been running too long. If the OS says yield, the DPC re-queues itself and returns -- remaining packets stay in the ring and are drained when the DPC fires again. If no yield is needed, the loop continues immediately. This design eliminates the inter-DPC gap that caused ring overflow under load while also preventing DPC watchdog timeouts on heavily loaded systems.

```
    DPC fires on target CPU
    +----------------------------------------------+
    |  do {                                        |
    |    Lock ring                                  |
    |    Dequeue min(available, DrainBatchSize)     |
    |    Unlock ring                                |
    |                                              |
    |    Split into originals + deep-copies        |
    |                                              |
    |    Indicate originals normally                |
    |    (TCP/IP returns them asynchronously)       |
    |                                              |
    |    Indicate deep-copies with RESOURCES        |
    |    (synchronous return, recycle to pool)      |
    |                                              |
    |    if (ring not empty &&                     |
    |        KeShouldYieldProcessor()) {            |
    |      Re-queue DPC; return;                   |
    |    }                                         |
    |  } while (ring not empty)                    |
    +----------------------------------------------+
```

**Split indication:** The DPC splits the dequeued batch into two chains:
- **Originals** (CanPend path): indicated without `NDIS_RECEIVE_FLAGS_RESOURCES`. TCP/IP returns them to the miniport asynchronously via the normal NDIS return path.
- **Deep-copies** (!CanPend path): indicated with `NDIS_RECEIVE_FLAGS_RESOURCES`. Since the indication is synchronous, the DPC immediately recycles the NBL structures back to the free pool after the call returns.

The `IsDeepCopy` flag for each dequeued packet is tracked in a stack-allocated array (`IsDeepCopyDpc[XDP_CPUMAP_MAX_BATCH_SIZE]`), which is bounded by the per-iteration batch size.

### 7.9 Deep-Copy Fallback Pool

The deep-copy pool handles the `!CanPend` case efficiently with minimal memory overhead:

```
    +-----------------------------------------------------+
    |                 Deep-Copy Pool Lifecycle             |
    |                                                     |
    |  1. First !CanPend packet arrives                   |
    |     +-> NdisAllocateNetBufferAndNetBufferList()     |
    |         (bare NBL+NB, no data buffer)               |
    |     +-> NdisRetreatNetBufferDataStart()             |
    |         (NDIS allocates MDL + physical pages)       |
    |     +-> MdlCopyMdlChain() to copy packet data      |
    |     +-> Enqueue deep-copy into ring                 |
    |                                                     |
    |  2. DPC drains ring, indicates deep-copy            |
    |     +-> NdisFIndicateReceiveNetBufferLists          |
    |         (with RESOURCES -> synchronous return)      |
    |     +-> NdisAdvanceNetBufferDataStart(FreeMdl=TRUE) |
    |         (frees data pages)                          |
    |     +-> InterlockedPushEntrySList()                 |
    |         (recycle bare NBL struct to free list)       |
    |                                                     |
    |  3. Next !CanPend packet arrives                    |
    |     +-> InterlockedPopEntrySList()                  |
    |         (reuse recycled NBL struct -- no NDIS alloc)|
    |     +-> NdisRetreatNetBufferDataStart()             |
    |         (allocate fresh data pages)                  |
    |     +-> MdlCopyMdlChain() to copy packet data      |
    |     +-> Enqueue deep-copy into ring                 |
    +-----------------------------------------------------+
```

**Why this design?** Allocating NDIS NBL structures is expensive (pool management, tagging, context setup). By recycling the NBL struct via a lock-free SList and only allocating/freeing the data pages per packet, the deep-copy path avoids the most expensive part of NDIS allocation. Since `!CanPend` is typically an exceptional/rare path, this avoids pre-allocating large memory pools.

**Pool statistics track:**
- `DeepCopyHitCount` -- NBL reused from SList (fast path)
- `DeepCopyMissCount` -- new NBL allocated from NDIS pool (slow path)
- `DeepCopyFailCount` -- allocation failed, packet dropped
- `DeepCopyAllocCount` -- total NBLs ever allocated (pool grows on demand, never shrinks)

---

## 8. Flow-Affinity Hashing

CPUMAP must ensure that packets from the same flow (same source/destination IP and ports) always go to the same CPU. Distributing a single flow across multiple CPUs would cause packet reordering, which breaks TCP and degrades UDP application performance.

The hashing algorithm has three stages:

**Stage 1: Symmetric IP hash**

Combine source and destination IP addresses such that `hash(A->B) == hash(B->A)`. This ensures bidirectional flows map to the same CPU. The approach uses min/max decomposition with a rotate-left to avoid XOR cancellation:
- `MinIp = min(SrcIp, DstIp)`
- `MaxIp = max(SrcIp, DstIp)`
- `Hash = MinIp + rotl(MaxIp, 16)`

IPv6 addresses are first reduced to 32-bit values by XOR-folding their four dwords, then combined symmetrically using the same min/max technique.

**Stage 2: Mix in ports**

Ports are also combined symmetrically (min in low word, max in high word). The IP hash and port hash are mixed using golden-ratio multiplication (`0x9E3779B9`), which provides good avalanche properties -- small input changes propagate to all output bits.

**Stage 3: Murmur3 finalizer**

The combined hash is passed through the murmur3 32-bit finalizer to spread entropy evenly across all bits. This ensures good distribution when the hash is reduced via modulo to the CPU count, especially for non-power-of-2 CPU counts. The finalizer uses the standard constants `0x85ebca6b` and `0xc2b2ae35`.

**CPU selection:** `TargetCpu = (FinalHash % CpuCount) + CpuBase`

**Protocol support:** The hash covers both UDP and TCP flows. If the transport layer is not parsed (e.g., non-UDP/TCP traffic), only the IP addresses contribute to the hash.

---

## 9. Lazy Initialization

The CPUMAP is **not** created when the XDP program is bound to the interface. Instead, it is created on the first packet that matches a CPU redirect rule. This is called **lazy initialization** and has two benefits:

1. **Avoids wasted memory.** A CPUMAP for 24 CPUs with 32K ring depth allocates significant NonPagedPool memory (~15 MB for rings alone). If the program is bound but no matching traffic ever arrives, this memory would be wasted.

2. **Avoids a control-plane/data-plane dependency.** The CPUMAP parameters come from the XDP program rule, which lives in xdp.sys. The CPUMAP is used by xdplwf.sys. Creating it eagerly would require cross-driver coordination at bind time. Lazy creation uses the frame extension to carry parameters from inspection to creation.

**Race safety:** Multiple RX queues share `Generic->CpuMap`. If two RSS CPUs simultaneously see the first matching packet, both may try to create a CPUMAP. The code uses `InterlockedCompareExchangePointer` to atomically install the winner. The loser detects that someone else already installed a map, destroys its duplicate, and uses the shared one.

---

## 10. Lifecycle and Teardown

```
    Program Created          First Matching Packet         Program Closed
    (user calls              (lazy init)                   (handle closed)
     XdpCreateProgram)
         |                        |                              |
         v                        v                              v
    Rule validated           XdpCpuMapCreate()              Queue delete
    in xdp.sys               - Allocate rings               detects empty
    Parameters stored        - Allocate DPCs                queue list
         |                   - Create deep-copy pool             |
         |                   - Install atomically                v
         |                        |                        XdpCpuMapDestroy()
         |                        v                        - Active = FALSE
         |                   Normal operation               - KeFlushQueuedDpcs
         |                   (enqueue + drain)              - Drain remaining
         |                        |                          ring entries
         |                        |                        - Print stats
         |                        |                        - Free rings, DPCs
         |                        |                        - Free deep-copy pool
         v                        v                        - Free CPUMAP struct
```

**Teardown sequence in detail:**

1. **Mark inactive:** `CpuMap->Active = FALSE` with a memory barrier. This prevents new enqueues from `FlushBatch`.

2. **Flush all DPCs:** `KeFlushQueuedDpcs()` waits for all in-flight DPCs across all CPUs to complete. After this, no DPC can be running or scheduled.

3. **Drain remaining entries:** Any packets left in the rings were enqueued but never drained by a DPC. The teardown loop walks each ring:
   - **Deep-copy NBLs** (never indicated to TCP/IP): data pages are freed via `NdisAdvanceNetBufferDataStart`, and the bare NBL struct is pushed back to the free list.
   - **Original NBLs** (CanPend path, never indicated to TCP/IP): returned to the miniport via `NdisFReturnNetBufferLists`.

4. **Print statistics:** All per-ring and global counters are dumped via `DbgPrintEx` (viewable in WinDbg or DbgView). TSC frequency is calibrated via a short stall to convert lock-wait cycles to microseconds.

5. **Free resources:** Rings, DPC array, deep-copy pool (including any cached NBL structs on the SList), and the CPUMAP structure itself.

**Two cleanup paths exist:**
- `recv.c` (`XdpGenericRxDeleteQueue`): When the last RX queue is deleted, it destroys the CpuMap under the Generic lock.
- `generic.c` (`XdpGenericCleanupInterface`): Catches any remaining CpuMap during interface teardown as a safety net.

---

## 11. Statistics and Diagnostics

CPUMAP collects detailed statistics at every stage for performance analysis. All counters use `InterlockedIncrement` / `InterlockedAdd` for thread safety without locks.

### Per-Ring Counters

| Counter | What It Measures |
|---|---|
| `EnqueueCount` | Total packets successfully enqueued |
| `DrainCount` | Total packets successfully drained by DPC |
| `DropCount` | Total packets dropped (ring full + invalid target) |
| `RingFullCount` | Subset of drops caused by ring overflow |
| `DpcInvokeCount` | Number of times the DPC fired |
| `DpcMaxBatchDrained` | Largest batch dequeued in a single DPC |
| `DpcRequeueCount` | Number of loop iterations beyond the first (more work after batch) |
| `MaxRingDepth` | High-water mark of ring occupancy |
| `EnqueueBatchCount` | Number of FlushBatch calls that enqueued to this ring |
| `DpcLoopIterations` | Total inner-loop iterations across all DPC firings |
| `DpcMaxLoopIterations` | Maximum loop iterations in a single DPC |
| `DpcEmptyCount` | DPC fired but ring was already empty (wasted DPC) |
| `DpcYieldCount` | DPC yielded via `KeShouldYieldProcessor` and re-queued itself |

### Lock Contention Counters (per ring)

| Counter | What It Measures |
|---|---|
| `LockWaitCycles` | Total rdtsc cycles spent waiting for the spinlock |
| `LockAcquireCount` | Total lock acquisitions (producers + consumer) |
| `SourceCpuMask[2]` | Bitmask of source (RSS) CPUs that enqueued to this ring |

These enable computing average lock wait time in microseconds and identifying cross-CPU contention patterns (e.g., how many distinct RSS CPUs contend for the same ring).

### Global Counters

| Counter | What It Measures |
|---|---|
| `MiniportResourcesCount` | How often the miniport indicated with `NDIS_RECEIVE_FLAGS_RESOURCES` |
| `MiniportNoResourcesCount` | How often the miniport indicated without RESOURCES |
| `AbsoluteZeroCopyIndicateCount` | Originals indicated on target CPUs (zero-copy path) |
| `DeepCopyAllocCount` | Total deep-copy NBLs ever allocated |
| `DeepCopyHitCount` | Recycled NBL reused from SList (fast path) |
| `DeepCopyMissCount` | New NBL allocated from NDIS pool (slow path) |
| `DeepCopyFailCount` | Allocation failures (packet dropped) |
| `DeepCopyIndicateCount` | Deep-copy NBLs indicated on target CPUs |

**All statistics are printed via `DbgPrintEx` during CPUMAP destruction.** To view them:
- **With kernel debugger attached:** `ed nt!Kd_IHVNETWORK_Mask 0xffffffff`
- **Without debugger:** Set registry key `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter\IHVNETWORK = 0xffffffff` (DWORD, requires reboot), then run **DebugView** (Sysinternals) with kernel capture enabled.

---

## 12. Test Tool: cpuredirect.exe

A command-line test tool is provided for validating CPUMAP behavior. It is not intended for production use — production callers should use the `XdpCreateProgram` API directly.

```
cpuredirect.exe <IfIndex> <Port> <CpuBase> <CpuCount> [RingDepth] [DrainBatch]
                [QuicCidOffset] [QuicCidLength] [IgnoreCpus]
```

| Argument | Description |
|----------|-------------|
| `IfIndex` | Network interface index |
| `Port` | UDP destination port to match (0 = match all traffic) |
| `CpuBase` | First CPU index in the target range |
| `CpuCount` | Number of CPUs to distribute across |
| `RingDepth` | Per-CPU ring capacity, power of 2 (0 = default 32768) |
| `DrainBatch` | Max NBLs drained per DPC (0 = default 256) |
| `QuicCidOffset` | Byte offset in QUIC Dest CID to start reading (0 = default) |
| `QuicCidLength` | Number of CID bytes to use for CPU selection (0 = disabled, use 5-tuple hash) |
| `IgnoreCpus` | Comma-separated CPU numbers to skip (e.g., `65,67,69,71,73,75,77,79`) |

**Examples:**

```
# Redirect UDP port 9999 traffic across CPUs 40-59
cpuredirect.exe 6 9999 40 20

# Redirect ALL traffic across CPUs 0-15
cpuredirect.exe 6 0 0 16

# QUIC CID hashing: offset=0, length=2 (MsQuic partition ID at CID start)
cpuredirect.exe 6 4433 40 40 0 0 0 2

# QUIC CID hashing with HT sibling exclusion
cpuredirect.exe 6 4433 40 40 0 0 0 2 65,67,69,71,73,75,77,79
```

The tool creates an XDP program with `XDP_CREATE_PROGRAM_FLAG_GENERIC | XDP_CREATE_PROGRAM_FLAG_ALL_QUEUES`, waits for Ctrl+C, then closes the program handle. The kernel-side teardown dumps statistics and frees all resources.

When `Port = 0`, the tool uses `XDP_MATCH_ALL` to redirect all traffic (not just UDP). Otherwise it uses `XDP_MATCH_UDP_DST` to match a specific UDP destination port.

---

## 13. File Change Summary

| File | Change Type | Description |
|---|---|---|
| `published/external/xdp/datapath.h` | Modified | Added `XDP_RX_ACTION_CPU_REDIRECT` enum value |
| `published/external/xdp/program.h` | Modified | Added `XDP_REDIRECT_TARGET_TYPE_CPU`, `XDP_CPU_REDIRECT_PARAMS` (with `IgnoreCpuBitmap` and QUIC CID flags), extended `XDP_REDIRECT_PARAMS` union |
| `published/private/xdpcpumap.h` | **New** | Frame extension definition, CPUMAP lifecycle and batch APIs |
| `src/xdp/cpumap.h` | **New** | Internal ring/map structures, drain DPC prototype, constants |
| `src/xdp/cpumap.c` | **New** | Core CPUMAP implementation (~970 lines): Create, Destroy, FlushBatch, DrainDpc |
| `src/xdp/programinspect.c` | Modified | CPU redirect classification: hashing, CPU selection, frame extension population, rule validation |
| `src/xdp/rx.c` | Modified | Register `CPU_REDIRECT` frame extension in RX queue |
| `src/xdp/program.c` | Modified | Handle `TARGET_TYPE_CPU` in program binding (no external handle needed) |
| `src/xdplwf/recv.c` | Modified | Post-inspect CPU_REDIRECT case, lazy CPUMAP init, batch collect/flush, miniport tracking, cleanup |
| `src/xdplwf/generic.h` | Modified | Added `CpuMap` pointer and extension to `XDP_LWF_GENERIC` struct |
| `src/xdplwf/generic.c` | Modified | CPUMAP cleanup in `XdpGenericCleanupInterface` |
| `test/cpuredirect/cpuredirect.c` | **New** | User-mode test tool (~270 lines) |
| `test/cpuredirect/cpuredirect.vcxproj` | **New** | Build project for test tool |
| `tools/setup.ps1` | Modified | Fix certificate import to use `certutil` for better compatibility |
| Build/precomp/sln files | Modified | Include headers, add project to solution |

---

## 14. Known Limitations and Future Work

**Current limitations:**

1. **Single rule per interface.** Only one CPU redirect rule can be active per network interface. Multiple rules with different port matches targeting different CPU ranges are not yet supported.

2. **No eBPF integration.** The current implementation uses XDP's built-in rule matching engine (e.g., `XDP_MATCH_UDP_DST`). The plan is to expose CPUMAP as a `BPF_MAP_TYPE_CPUMAP` so eBPF programs can call `bpf_redirect_map()` with custom CPU selection logic. The ring buffer and DPC drain infrastructure is designed to serve as the backing implementation for that eBPF map type.

3. **PASSIVE_LEVEL lazy init.** `XdpCpuMapCreate` requires `PASSIVE_LEVEL` for pool allocation, but the first matching packet may trigger it at `DISPATCH_LEVEL`. A more robust solution would use a work item for deferred creation.

4. **Spinlock on ring.** Each ring is shared among multiple source CPUs under a spinlock. Under high fan-out (8 source CPUs x 24 targets), each ring may see up to 8 producers contending. Replacing MPSC rings with per-(source, target) SPSC rings would eliminate this contention.

**Performance tuning guidance:**

| Parameter | Guidance |
|---|---|
| `RingDepth` | Start with default 32768. Increase if `RingFullCount` is non-zero under load. Each doubling doubles per-CPU memory. |
| `DrainBatchSize` | Start with default 256. Lower values reduce latency spikes but increase DPC overhead. Values of 64-256 work well in practice. |
| `CpuCount` | Should match the number of application worker threads. Over-provisioning wastes ring memory; under-provisioning creates CPU hotspots. |
| `CpuBase` | Use NUMA-aware placement: pick CPUs on the same NUMA node as the NIC for lowest cross-node latency. |
