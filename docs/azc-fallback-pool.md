# AZC Fallback Pool for !CanPend (TxCloneNbl-style)

## Problem

When an AZC (Absolute Zero Copy) CPUMAP receives a `!CanPend` batch — where the miniport
indicates with `NDIS_RECEIVE_FLAGS_RESOURCES` — the original NBL cannot be held for async
ring indication. The current code strips `XDP_CPUMAP_FLAG_ABSOLUTE_ZERO_COPY` from
`EffectiveFlags` and falls through to the PREALLOC shell path.

This is wasteful: the PREALLOC shell pre-allocates `ring_capacity × num_CPUs × 2048 bytes`
regardless of whether `!CanPend` ever fires. Since `!CanPend` is an exceptional/rare path,
paying that memory cost unconditionally is undesirable for AZC maps.

## Solution

Add a dedicated **lazy fallback pool** to each AZC CPUMAP, modelled on `TxCloneNblPool` /
`TxCloneNblSList` in `recv.c`. The pool has **no pre-allocated data buffers** — NBL structs
are allocated on first use (up to a configurable limit) and recycled via SList. The PREALLOC
shell code is untouched; for AZC maps this pool is the sole `!CanPend` fallback.

```
Normal AZC path  (CanPend=TRUE)   → original NBL held in ring → tcpip → returned
AZC Fallback path (!CanPend)      → lazy clone alloc'd, data copied → clone indicated
                                    original returned to miniport immediately
Non-AZC maps                      → unchanged (PREALLOC shell or NdisAllocateCloneNetBufferList)
```

---

## Files Changed

| File | Change |
|------|--------|
| `src/xdp/cpumap.h` | New fields on `_XDP_CPUMAP`; new magic define; limit default |
| `src/xdp/cpumap.c` | Pool create/destroy; alloc/free helpers; FlushBatch new branch; DrainDpc stamp; ring drain update; stats print |
| `src/xdplwf/recv.c` | `XdpCpuMapReturnShells`: new case for `AZC_FALLBACK_MAGIC` |

---

## Data Structures

### New fields on `_XDP_CPUMAP`

```c
// AZC !CanPend fallback pool (lazy, no pre-allocated data buffer)
NDIS_HANDLE             AzcFallbackNblPool;
DECLSPEC_CACHEALIGN
SLIST_HEADER            AzcFallbackNblSList;  // recycled NBLs (cross-CPU safe)
volatile LONG           AzcFallbackNblCount;  // total ever allocated
ULONG                   AzcFallbackNblLimit;  // cap on total allocated
volatile LONG           AzcFallbackIndicateCount; // stats
```

### New magic constant

```c
#define XDP_CPUMAP_AZC_FALLBACK_MAGIC  ((PVOID)(ULONG_PTR)0x584D4152)  // 'XMAR'
#define XDP_CPUMAP_AZC_FALLBACK_NBL_LIMIT_DEFAULT  64U
```

Stamped in `Nbl->MiniportReserved[1]` (alongside existing `SHELL_MAGIC` / `CLONE_MAGIC`) so
`XdpCpuMapReturnShells` can dispatch to the right free path.

### NBL context area

Each fallback NBL carries a back-pointer to its owning `XDP_CPUMAP`:

```c
#define XDP_CPUMAP_AZC_FALLBACK_CONTEXT_SIZE  sizeof(PVOID)
// Access via: *(XDP_CPUMAP **)NET_BUFFER_LIST_CONTEXT_DATA_START(Nbl)
```

---

## Implementation Steps

### Step 1 — Pool creation (`XdpCpuMapCreate`, cpumap.c)

Conditional on `XDP_CPUMAP_FLAG_ABSOLUTE_ZERO_COPY`:

```c
NET_BUFFER_LIST_POOL_PARAMETERS FbPoolParams = {0};
FbPoolParams.Header.Type     = NDIS_OBJECT_TYPE_DEFAULT;
FbPoolParams.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
FbPoolParams.Header.Size     = sizeof(FbPoolParams);
FbPoolParams.PoolTag         = POOLTAG_CPUMAP;
FbPoolParams.fAllocateNetBuffer = TRUE;
FbPoolParams.ContextSize     = XDP_CPUMAP_AZC_FALLBACK_CONTEXT_SIZE;

Map->AzcFallbackNblPool =
    NdisAllocateNetBufferListPool(NdisHandle, &FbPoolParams);
if (Map->AzcFallbackNblPool == NULL) {
    Status = STATUS_NO_MEMORY;
    goto Exit;
}
InitializeSListHead(&Map->AzcFallbackNblSList);
Map->AzcFallbackNblLimit = XDP_CPUMAP_AZC_FALLBACK_NBL_LIMIT_DEFAULT;
```

### Step 2 — Alloc helper

`XdpCpuMapAzcFallbackAllocNbl` (DISPATCH_LEVEL safe):

1. Flush the SList — if recycled NBLs exist, pop the head, push remaining back.
2. Otherwise lazy-allocate via `NdisAllocateNetBufferAndNetBufferList` if under limit.
3. On allocation, stamp `CpuMap` pointer into the NBL's context area.
4. Returns `NULL` on limit exhaustion or NDIS failure (packet is dropped).

> **Buffer lifecycle note**: `NdisAdvanceNetBufferDataStart(FreeMdl=TRUE)` is called on every
> return, freeing MDL and backing pages. `NdisRetreatNetBufferDataStart` re-allocates them on
> reuse. This avoids per-packet NBL struct allocation (expensive NDIS pool) at the cost of one
> NDIS page alloc/free per packet — acceptable since `!CanPend` is an exceptional path.
> This matches the `TxCloneNbl` pattern in `recv.c` exactly.

### Step 3 — Free helper

`XdpCpuMapAzcFallbackFreeNbl`:

1. Advance data start by `NET_BUFFER_DATA_LENGTH` with `FreeMdl=TRUE` — releases data pages.
2. Clear `MiniportReserved[0..1]` and `NEXT_NBL`.
3. Read `CpuMap` from context area.
4. Push NBL struct onto `AzcFallbackNblSList` for reuse.

### Step 4 — FlushBatch new branch (cpumap.c, Phase 1 loop)

Insert a new `else if` between the existing AZC branch and the `#if XDP_CPUMAP_PREALLOC`
shell-pop block:

```
if (EffectiveFlags & AZC)         → existing: use original directly
else if (CpuMap->Flags & AZC)     → NEW: !CanPend fallback
    allocate fallback NBL
    NdisRetreatNetBufferDataStart
    MdlCopyMdlChain (data copy)
    Clones[i] = FbNbl, Shells[i] = NULL
    on failure → drop, return original via ReturnableOriginals
    continue                      (skips all PREALLOC / clone code)
#if XDP_CPUMAP_PREALLOC            → untouched
```

`Shells[i] = NULL` with `IsOriginal=FALSE` is the signal used in DrainDpc to distinguish
fallback clones from regular clones.

### Step 5 — DrainDpc stamp loop (cpumap.c)

In the `#if XDP_CPUMAP_ZERO_COPY_INDICATE` stamp block, the `Shell == NULL` branch:

```c
if (Shell != NULL) {
    Cur->MiniportReserved[0] = Shell;
    Cur->MiniportReserved[1] = XDP_CPUMAP_SHELL_MAGIC;
} else if (Ring->OwnerMap->Flags & XDP_CPUMAP_FLAG_ABSOLUTE_ZERO_COPY) {
    // AZC fallback: stamp magic; CpuMap already in context area
    Cur->MiniportReserved[1] = XDP_CPUMAP_AZC_FALLBACK_MAGIC;
    InterlockedIncrement(&Ring->OwnerMap->AzcFallbackIndicateCount);
} else {
    Cur->MiniportReserved[0] = Ring->OwnerMap;
    Cur->MiniportReserved[1] = XDP_CPUMAP_CLONE_MAGIC;
}
InterlockedIncrement(&Ring->OwnerMap->OutstandingIndications);
```

### Step 6 — Return path (`XdpCpuMapReturnShells`, recv.c)

New case inserted between `SHELL_MAGIC` and `CLONE_MAGIC`:

```c
} else if (Current->MiniportReserved[1] == XDP_CPUMAP_AZC_FALLBACK_MAGIC) {
    XDP_CPUMAP *Map =
        *(XDP_CPUMAP **)NET_BUFFER_LIST_CONTEXT_DATA_START(Current);

    XdpCpuMapAzcFallbackFreeNbl(Current);   // advance data, clear stamps, push to SList

    if (InterlockedDecrement(&Map->OutstandingIndications) == 0) {
        KeSetEvent(&Map->AllReturnedEvent, IO_NO_INCREMENT, FALSE);
    }
```

`XdpCpuMapAzcFallbackFreeNbl` must be accessible from `recv.c` — expose via a private header
(`xdpcpumap.h`) or as a thin inline wrapper.

### Step 7 — Destroy drain (cpumap.c)

The existing AZC ring drain loop only handled `IsOriginal=TRUE`. Update to handle fallback
clones (`IsOriginal=FALSE`):

```c
if (Entry->IsOriginal) {
    NdisFReturnNetBufferLists(Entry->FilterHandle, Entry->Nbl, 0);
} else {
    // AZC fallback clone (never indicated): recycle to pool
    XdpCpuMapAzcFallbackFreeNbl(Entry->Nbl);
}
```

After the `OutstandingIndications` wait, drain and free the pool:

```c
if (CpuMap->AzcFallbackNblPool != NULL) {
    NET_BUFFER_LIST *Chain =
        (NET_BUFFER_LIST *)InterlockedFlushSList(&CpuMap->AzcFallbackNblSList);
    while (Chain != NULL) {
        NET_BUFFER_LIST *Next = NET_BUFFER_LIST_NEXT_NBL(Chain);
        NdisFreeNetBufferList(Chain);
        Chain = Next;
    }
    NdisFreeNetBufferListPool(CpuMap->AzcFallbackNblPool);
    CpuMap->AzcFallbackNblPool = NULL;
}
```

### Step 8 — Stats debug print (cpumap.c, Destroy)

```c
DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL,
    "AzcFallback: Indicated=%d Allocated=%d Limit=%d\n",
    CpuMap->AzcFallbackIndicateCount,
    CpuMap->AzcFallbackNblCount,
    CpuMap->AzcFallbackNblLimit);
```

---

## Invariants Preserved

| Invariant | How preserved |
|-----------|---------------|
| PREALLOC shell code untouched | New `else if` branch `continue`s, skips all PREALLOC logic |
| `EffectiveFlags` stripping stays | FlushBatch detects AZC-map + non-AZC-effective via `CpuMap->Flags & AZC` |
| `IsOriginal=FALSE` routing | Fallback clones enter DrainDpc's clone chain (CloneHead) correctly |
| `OutstandingIndications` gate | Incremented in DrainDpc stamp loop; decremented in return path — destroy waits correctly |
| Non-AZC maps unaffected | New `else if` is never entered when `CpuMap->Flags` has no AZC bit |

---

## Verification Checklist

1. **Build** with `XDP_CPUMAP_PREALLOC=1` (default) and `XDP_CPUMAP_ZERO_COPY_INDICATE=1`.
2. **Steady-state AZC traffic** (`CanPend=TRUE`): confirm `AzcFallbackIndicateCount=0` — fallback never fires.
3. **Simulated `!CanPend`** (force `CanPend=FALSE` in debug build):
   - `AzcFallbackIndicateCount > 0`
   - `AzcFallbackNblCount <= AzcFallbackNblLimit`
   - No use-after-free (NDIS driver verifier clean)
   - Originals returned to miniport via DropList
4. **Destroy under load**: `AllReturnedEvent` fires, fallback pool drains cleanly, no leaked NBLs.
5. **NDIS driver verifier** enabled throughout all test runs.
