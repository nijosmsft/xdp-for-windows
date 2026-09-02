//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// CPUMAP batching thresholds, split out of xdpcpumap.h so that TESTS can assert
// against them at compile time.
//
// xdpcpumap.h itself is not includable outside the driver: it declares
// NET_BUFFER_LIST, NDIS_HANDLE and KDPC members and defines inline functions
// calling ExAcquireRundownProtectionEx, all under _IRQL_requires_max_. The
// user-mode functional test needs exactly two of its constants and none of that
// machinery.
//
// These values are still INTERNAL. Nothing here is a shipped ABI, and this
// header is deliberately under published/private rather than published/external
// so that consuming it remains a build-boundary decision rather than an
// accident. It exists so a test can express "this frame count must exceed the
// batch size" as a static_assert instead of duplicating the number, which is the
// only way a later change to either constant fails the build rather than
// silently weakening the test.
//
// Keep this header free of includes and of any type that is not a plain
// preprocessor constant.
//

#pragma once

//
// Rundown credit pool chunk for one flush group. A group that commits more than
// this many packets legitimately acquires a second chunk.
//
#define XDP_CPUMAP_RUNDOWN_CREDIT_CHUNK 32u

//
// Flush batch input count (POC A's 32; section 7, "Batch input count"). A
// receive call that commits more than this many redirects invokes the full-group
// flush from inside XdpCpuMapCommitRedirect rather than only from
// XdpCpuMapCommitGroupFinish.
//
#define XDP_CPUMAP_BATCH_SIZE 32u

//
// Upper bound on deep-copy NBLs a single RX queue's pool will ever allocate.
//
// The cache grows lazily and never pre-allocates data buffers: a copy's pages
// come from NdisRetreatNetBufferDataStart at copy time and are released back at
// recycle time, so this caps bare NBL descriptors, not memory held for payload.
// Reaching it is a counted, non-fatal drop rather than an error -- CanPend ==
// FALSE is an exceptional path, and POC A's equivalent fallback pool caps at 64.
//
// Counted like the TX clone cache upstream: the count is of descriptors ever
// allocated and is not decremented on recycle, because a recycled descriptor
// stays in the cache and is reused rather than freed.
//
#define XDP_CPUMAP_DEEPCOPY_CACHE_MAX 256u
