//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// NDIS NBL/NET_BUFFER/MDL model for the CPUMAP deep-copy path.
//
// Modelled rather than faked. NdisRetreatNetBufferDataStart really allocates a
// buffer and NdisAdvanceNetBufferDataStart with FreeMdl really frees it, so a
// path that forgets to advance leaks a counted page; the MDL copy really walks
// both chains and moves bytes, so a build that copies nothing fails a payload
// comparison rather than a length check. That distinction is the whole point:
// design section 8.1a row 9b names three failure paths whose obligation is to
// release without leaking, and a harness that only pretended to allocate could
// not tell whether they did.
//
// Failure injection exists for the same reason. Every row 9b failure has to be
// executable, or the counters section 12 requires could only ever be argued
// about.
//

#include "precomp.h"

LONG XdpCpuMapTestNblPoolLive;
LONG XdpCpuMapTestNblLive;
LONG XdpCpuMapTestPageLive;
LONG XdpCpuMapTestNblAllocTotal;
ULONG64 XdpCpuMapTestNextNblTag;

LONG XdpCpuMapTestFailNblPoolAlloc;
LONG XdpCpuMapTestFailNblAllocAfter;
LONG XdpCpuMapTestFailRetreatAfter;
LONG XdpCpuMapTestFailMdlCopyAfter;
BOOLEAN XdpCpuMapTestSourceMetadataBlobs;
BOOLEAN XdpCpuMapTestDirtyFreshAlloc;

//
// A pool is modelled as a live token rather than a container: NDIS pools do not
// track outstanding NBLs for the caller, and pretending otherwise would let a
// test lean on bookkeeping the real allocator does not do.
//
typedef struct _XDPCPUMAP_TEST_NBL_POOL {
    ULONG PoolTag;
    BOOLEAN AllocateNetBuffer;
    USHORT ContextSize;
} XDPCPUMAP_TEST_NBL_POOL;

VOID
XdpCpuMapTestResetNdisPool(
    VOID
    )
{
    XdpCpuMapTestNblPoolLive = 0;
    XdpCpuMapTestNblLive = 0;
    XdpCpuMapTestPageLive = 0;
    XdpCpuMapTestNblAllocTotal = 0;
    XdpCpuMapTestNextNblTag = 1;
    XdpCpuMapTestFailNblPoolAlloc = 0;
    XdpCpuMapTestFailNblAllocAfter = -1;
    XdpCpuMapTestFailRetreatAfter = -1;
    XdpCpuMapTestFailMdlCopyAfter = -1;
    XdpCpuMapTestSourceMetadataBlobs = FALSE;
    XdpCpuMapTestDirtyFreshAlloc = FALSE;
}

NDIS_HANDLE
NdisAllocateNetBufferListPool(
    _In_opt_ NDIS_HANDLE NdisHandle,
    _In_ NET_BUFFER_LIST_POOL_PARAMETERS *Parameters
    )
{
    XDPCPUMAP_TEST_NBL_POOL *Pool;

    UNREFERENCED_PARAMETER(NdisHandle);

    XDPCPUMAP_TEST_ASSERT(Parameters->Header.Type == NDIS_OBJECT_TYPE_DEFAULT);
    XDPCPUMAP_TEST_ASSERT(
        Parameters->Header.Revision == NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1);
    XDPCPUMAP_TEST_ASSERT(Parameters->Header.Size == sizeof(*Parameters));

    if (XdpCpuMapTestFailNblPoolAlloc) {
        return NULL;
    }

    Pool = malloc(sizeof(*Pool));
    if (Pool == NULL) {
        return NULL;
    }

    Pool->PoolTag = Parameters->PoolTag;
    Pool->AllocateNetBuffer = Parameters->fAllocateNetBuffer;
    Pool->ContextSize = Parameters->ContextSize;

    XdpCpuMapTestNblPoolLive++;
    return (NDIS_HANDLE)Pool;
}

VOID
NdisFreeNetBufferListPool(
    _In_ NDIS_HANDLE PoolHandle
    )
{
    XDPCPUMAP_TEST_ASSERT(PoolHandle != NULL);

    XdpCpuMapTestNblPoolLive--;
    free(PoolHandle);
}

NET_BUFFER_LIST *
NdisAllocateNetBufferAndNetBufferList(
    _In_ NDIS_HANDLE PoolHandle,
    _In_ USHORT ContextSize,
    _In_ USHORT ContextBackFill,
    _In_opt_ MDL *MdlChain,
    _In_ ULONG DataOffset,
    _In_ SIZE_T DataLength
    )
{
    XDPCPUMAP_TEST_NBL_POOL *Pool = (XDPCPUMAP_TEST_NBL_POOL *)PoolHandle;
    NET_BUFFER_LIST *Nbl;
    NET_BUFFER *Nb;

    UNREFERENCED_PARAMETER(ContextSize);
    UNREFERENCED_PARAMETER(ContextBackFill);

    XDPCPUMAP_TEST_ASSERT(Pool != NULL);
    XDPCPUMAP_TEST_ASSERT(Pool->AllocateNetBuffer);

    //
    // CPUMAP asks for a bare descriptor and supplies pages itself, per the
    // design's "no pre-allocated data buffers".
    //
    XDPCPUMAP_TEST_ASSERT(MdlChain == NULL);
    XDPCPUMAP_TEST_ASSERT(DataOffset == 0);
    XDPCPUMAP_TEST_ASSERT(DataLength == 0);

    if (XdpCpuMapTestFailNblAllocAfter == 0) {
        return NULL;
    }
    if (XdpCpuMapTestFailNblAllocAfter > 0) {
        XdpCpuMapTestFailNblAllocAfter--;
    }

    //
    // 16-byte aligned because the production free list stores the SLIST_ENTRY in
    // the NBL's Next field, which requires MEMORY_ALLOCATION_ALIGNMENT. Real
    // pool-allocated NBLs satisfy this; malloc does not guarantee it, so the
    // harness must not be the reason a misalignment goes unnoticed.
    //
    Nbl = _aligned_malloc(sizeof(*Nbl), MEMORY_ALLOCATION_ALIGNMENT);
    if (Nbl == NULL) {
        return NULL;
    }

    Nb = _aligned_malloc(sizeof(*Nb), MEMORY_ALLOCATION_ALIGNMENT);
    if (Nb == NULL) {
        _aligned_free(Nbl);
        return NULL;
    }

    RtlZeroMemory(Nbl, sizeof(*Nbl));
    RtlZeroMemory(Nb, sizeof(*Nb));

    //
    // A freshly allocated NBL has ZEROED OOB slots, and the harness models that
    // rather than poisoning them.
    //
    // An earlier revision poisoned the array so production's clearing would be
    // load-bearing on the allocation path too. That was wrong once B10
    // established that the array must NOT be blanket-cleared: it holds entries
    // with mixed ownership, so a blanket clear can bypass NDIS reference
    // handling. Poisoning therefore modelled a condition production is forbidden
    // from repairing -- an unsatisfiable test. Residue on a fresh descriptor
    // would leave no correct behaviour available, which is itself the argument
    // that NdisAllocateNetBufferAndNetBufferList does not produce it.
    //
    Nbl->FirstNetBuffer = Nb;
    Nbl->TestTag = XdpCpuMapTestNextNblTag++;

    //
    // Optional allocator residue in a slot CPUMAP does not carry.
    //
    // Modelled because the uniform cleanliness check applies to FRESH
    // descriptors too, and that branch is otherwise unreachable: NDIS evidently
    // zeroes the array, but does not document doing so, which is exactly why
    // production checks rather than assumes. Without this the branch could only
    // ever be argued about.
    //
    if (XdpCpuMapTestDirtyFreshAlloc) {
        Nbl->NetBufferListInfo[NetBufferListCancelId] = (VOID *)(ULONG_PTR)0xDEADBEEF;
    }

    XdpCpuMapTestNblLive++;
    XdpCpuMapTestNblAllocTotal++;
    return Nbl;
}

VOID
NdisFreeNetBufferList(
    _In_ NET_BUFFER_LIST *NetBufferList
    )
{
    XDPCPUMAP_TEST_ASSERT(NetBufferList != NULL);
    XDPCPUMAP_TEST_ASSERT(NetBufferList->FirstNetBuffer != NULL);

    //
    // Freeing a descriptor that still owns pages is the leak this model exists
    // to catch, so it is a failure here rather than a silent counter drift.
    //
    XDPCPUMAP_TEST_ASSERT(NetBufferList->FirstNetBuffer->MdlChain == NULL);

    _aligned_free(NetBufferList->FirstNetBuffer);
    _aligned_free(NetBufferList);
    XdpCpuMapTestNblLive--;
}

NDIS_STATUS
NdisRetreatNetBufferDataStart(
    _In_ NET_BUFFER *NetBuffer,
    _In_ ULONG DataOffsetDelta,
    _In_ ULONG DataBackFill,
    _In_opt_ VOID *AllocateMdlHandler
    )
{
    MDL *Mdl;

    UNREFERENCED_PARAMETER(DataBackFill);
    UNREFERENCED_PARAMETER(AllocateMdlHandler);

    XDPCPUMAP_TEST_ASSERT(NetBuffer != NULL);
    XDPCPUMAP_TEST_ASSERT(DataOffsetDelta > 0);

    if (XdpCpuMapTestFailRetreatAfter == 0) {
        return NDIS_STATUS_RESOURCES;
    }
    if (XdpCpuMapTestFailRetreatAfter > 0) {
        XdpCpuMapTestFailRetreatAfter--;
    }

    //
    // The real call prepends space, allocating an MDL and pages when the
    // existing chain has no room. CPUMAP always retreats onto a bare descriptor,
    // so the allocating case is the only one, and modelling it as such keeps the
    // page accounting exact.
    //
    XDPCPUMAP_TEST_ASSERT(NetBuffer->MdlChain == NULL);

    Mdl = malloc(sizeof(*Mdl));
    if (Mdl == NULL) {
        return NDIS_STATUS_RESOURCES;
    }

    Mdl->Buffer = malloc(DataOffsetDelta);
    if (Mdl->Buffer == NULL) {
        free(Mdl);
        return NDIS_STATUS_RESOURCES;
    }

    Mdl->Next = NULL;
    Mdl->Length = DataOffsetDelta;

    NetBuffer->MdlChain = Mdl;
    NetBuffer->CurrentMdl = Mdl;
    NetBuffer->CurrentMdlOffset = 0;
    NetBuffer->DataOffset = 0;
    NetBuffer->DataLength = DataOffsetDelta;

    XdpCpuMapTestPageLive++;
    return NDIS_STATUS_SUCCESS;
}

VOID
NdisAdvanceNetBufferDataStart(
    _In_ NET_BUFFER *NetBuffer,
    _In_ ULONG DataOffsetDelta,
    _In_ BOOLEAN FreeMdl,
    _In_opt_ VOID *FreeMdlHandler
    )
{
    UNREFERENCED_PARAMETER(FreeMdlHandler);

    XDPCPUMAP_TEST_ASSERT(NetBuffer != NULL);
    XDPCPUMAP_TEST_ASSERT(DataOffsetDelta <= NetBuffer->DataLength);

    NetBuffer->DataLength -= DataOffsetDelta;
    NetBuffer->DataOffset += DataOffsetDelta;

    if (!FreeMdl) {
        return;
    }

    //
    // FreeMdl releases the MDLs the matching retreat allocated. CPUMAP advances
    // the whole data length, so the chain goes away entirely and the descriptor
    // is left bare and reusable.
    //
    XDPCPUMAP_TEST_ASSERT(NetBuffer->DataLength == 0);

    while (NetBuffer->MdlChain != NULL) {
        MDL *Mdl = NetBuffer->MdlChain;

        NetBuffer->MdlChain = Mdl->Next;
        free(Mdl->Buffer);
        free(Mdl);
        XdpCpuMapTestPageLive--;
    }

    NetBuffer->CurrentMdl = NULL;
    NetBuffer->CurrentMdlOffset = 0;
    NetBuffer->DataOffset = 0;
}

NTSTATUS
MdlCopyMdlChainToMdlChainAtOffsetNonTemporal(
    _In_ MDL *DestinationMdl,
    _In_ ULONG DestinationOffset,
    _In_ MDL *SourceMdl,
    _In_ ULONG SourceOffset,
    _In_ ULONG Length
    )
{
    if (XdpCpuMapTestFailMdlCopyAfter == 0) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (XdpCpuMapTestFailMdlCopyAfter > 0) {
        XdpCpuMapTestFailMdlCopyAfter--;
    }

    //
    // Walks both chains rather than assuming a single MDL. A single memcpy would
    // pass just as well against a correct build and would stop modelling the
    // fragmented case entirely.
    //
    while (Length > 0) {
        ULONG DstAvailable;
        ULONG SrcAvailable;
        ULONG Chunk;

        if (DestinationMdl == NULL || SourceMdl == NULL) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        while (DestinationMdl != NULL && DestinationOffset >= DestinationMdl->Length) {
            DestinationOffset -= DestinationMdl->Length;
            DestinationMdl = DestinationMdl->Next;
        }
        while (SourceMdl != NULL && SourceOffset >= SourceMdl->Length) {
            SourceOffset -= SourceMdl->Length;
            SourceMdl = SourceMdl->Next;
        }

        if (DestinationMdl == NULL || SourceMdl == NULL) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        DstAvailable = DestinationMdl->Length - DestinationOffset;
        SrcAvailable = SourceMdl->Length - SourceOffset;
        Chunk = min(min(DstAvailable, SrcAvailable), Length);

        memcpy(
            DestinationMdl->Buffer + DestinationOffset,
            SourceMdl->Buffer + SourceOffset,
            Chunk);

        DestinationOffset += Chunk;
        SourceOffset += Chunk;
        Length -= Chunk;
    }

    return STATUS_SUCCESS;
}

//
// The slots NdisCopyReceiveNetBufferListInfo actually carries, DERIVED FROM THE
// SHIPPING BINARY rather than from documentation or memory.
//
// Decoded from ndis.sys 10.0.26100.8521, export RVA 0x26ED0. The array base is
// 0x90, so the unconditional prologue's copies at 0x90, 0x98, 0xA0, 0xB0, 0xC0,
// 0xC8, 0xD0, 0xD8, 0xE8, 0xF0, 0x100, 0x110, 0x118, 0x130, 0x140, 0x148, 0x150
// and 0x158 are slots 0, 1, 2, 4, 6, 7, 8, 9, 11, 12, 14, 16, 17, 20, 22, 23, 24
// and 25 -- EIGHTEEN of them. An earlier model listed thirteen and omitted the
// specially-handled WFP slot entirely, which meant the oracle was smaller than
// the thing it modelled and could not express a reference-lifecycle defect at
// all.
//
static const NDIS_NET_BUFFER_LIST_INFO XdpCpuMapTestCopiedInfoSlots[] = {
    TcpIpChecksumNetBufferListInfo,          // 0   0x90
    IPsecOffloadV1NetBufferListInfo,         // 1   0x98
    TcpLargeSendNetBufferListInfo,           // 2   0xA0  (== TcpReceiveNoPush)
    Ieee8021QNetBufferListInfo,              // 4   0xB0
    MediaSpecificInformation,                // 6   0xC0  pointer, miniport-owned
    NetBufferListFrameType,                  // 7   0xC8  (== NetBufferListProtocolId)
    NetBufferListHashValue,                  // 8   0xD0
    NetBufferListHashInfo,                   // 9   0xD8
    IPsecOffloadV2TunnelNetBufferListInfo,   // 11  0xE8
    IPsecOffloadV2HeaderNetBufferListInfo,   // 12  0xF0
    NetBufferListFilteringInfo,              // 14  0x100
    NblOriginalInterfaceIfIndex,             // 16  0x110
    TcpReceiveBytesTransferred,              // 17  0x118
    VirtualSubnetInfo,                       // 20  0x130
    TcpRecvSegCoalesceInfo,                  // 22  0x140 (== UdpSegmentationOffloadInfo)
    RscTcpTimestampDelta,                    // 23  0x148
    GftOffloadInformation,                   // 24  0x150
    GftFlowEntryId,                          // 25  0x158
    WfpNetBufferListInfo,                    // 10  0xE0  REFERENCED, not copied raw
};

const NDIS_NET_BUFFER_LIST_INFO *XdpCpuMapTestReceiveInfoSlots =
    XdpCpuMapTestCopiedInfoSlots;
const ULONG XdpCpuMapTestReceiveInfoSlotCount =
    RTL_NUMBER_OF(XdpCpuMapTestCopiedInfoSlots);

//
// Slots whose contents are OWNED elsewhere, so a copy that carries one is
// holding something it cannot release.
//
// MediaSpecificInformation and its Ex form point at storage the MINIPORT owns
// and reclaims when the original goes home. WfpNetBufferListInfo is worse in a
// subtler way: the disassembly shows NDIS takes a REFERENCE on it rather than
// aliasing the pointer, so a copy that acquires it and is later recycled into a
// cache leaks that reference -- and clearing the slot bypasses NDIS's handling
// rather than discharging it. Production refuses the redirect for all three
// instead of carrying or clearing them.
//
static const NDIS_NET_BUFFER_LIST_INFO XdpCpuMapTestOwnedInfoSlots[] = {
    MediaSpecificInformation,
    MediaSpecificInformationEx,
    WfpNetBufferListInfo,
};

const NDIS_NET_BUFFER_LIST_INFO *XdpCpuMapTestPointerOwnedSlots =
    XdpCpuMapTestOwnedInfoSlots;
const ULONG XdpCpuMapTestPointerOwnedSlotCount =
    RTL_NUMBER_OF(XdpCpuMapTestOwnedInfoSlots);

LONG XdpCpuMapTestWfpReferences;

//
// The slots production carries. Mirrors XdpCpuMapDeepCopyCarriedSlots, and the
// mirror is the point: if production widens or narrows what it carries without
// this list following, DeepCopySuccess fails rather than silently accepting the
// new behaviour.
//
static const NDIS_NET_BUFFER_LIST_INFO XdpCpuMapTestCarriedSlots[] = {
    TcpIpChecksumNetBufferListInfo,
    Ieee8021QNetBufferListInfo,
    NetBufferListFrameType,
    NetBufferListFilteringInfo,
    NetBufferListHashValue,
    NetBufferListHashInfo,
};

BOOLEAN
XdpCpuMapTestIsCarriedSlot(
    _In_ ULONG Slot
    )
{
    for (ULONG Index = 0; Index < RTL_NUMBER_OF(XdpCpuMapTestCarriedSlots); Index++) {
        if ((ULONG)XdpCpuMapTestCarriedSlots[Index] == Slot) {
            return TRUE;
        }
    }

    return FALSE;
}

VOID
NdisCopyReceiveNetBufferListInfo(
    _In_ NET_BUFFER_LIST *DestNetBufferList,
    _In_ const NET_BUFFER_LIST *SrcNetBufferList
    )
{
    //
    // Production no longer calls this -- it carries an explicit value-typed set
    // instead, precisely because this routine brings the WFP reference below.
    // The model is kept, with the reference semantics, so a criterion that
    // reintroduces the call is detected by the reference counter failing to
    // balance rather than by nothing at all.
    //
    for (ULONG Index = 0; Index < XdpCpuMapTestReceiveInfoSlotCount; Index++) {
        NDIS_NET_BUFFER_LIST_INFO Slot = XdpCpuMapTestReceiveInfoSlots[Index];

        if (Slot == WfpNetBufferListInfo) {
            if (SrcNetBufferList->NetBufferListInfo[Slot] != NULL) {
                XdpCpuMapTestWfpReferences++;
            }
        }

        DestNetBufferList->NetBufferListInfo[Slot] =
            SrcNetBufferList->NetBufferListInfo[Slot];
    }
}
//
// N.B. no InterlockedPushListSList wrapper here: see stubs/ntos.h. User mode
// already macro-maps that spelling onto the real InterlockedPushListSListEx.
//

NET_BUFFER_LIST *
XdpCpuMapTestCreateSourceNbl(
    _In_reads_bytes_(Length) const VOID *Payload,
    _In_ ULONG Length
    )
{    NET_BUFFER_LIST *Nbl;
    NET_BUFFER *Nb;
    MDL *Mdl;

    XDPCPUMAP_TEST_ASSERT(Length > 0);

    Nbl = _aligned_malloc(sizeof(*Nbl), MEMORY_ALLOCATION_ALIGNMENT);
    Nb = _aligned_malloc(sizeof(*Nb), MEMORY_ALLOCATION_ALIGNMENT);
    Mdl = malloc(sizeof(*Mdl));
    XDPCPUMAP_TEST_ASSERT(Nbl != NULL && Nb != NULL && Mdl != NULL);

    RtlZeroMemory(Nbl, sizeof(*Nbl));
    RtlZeroMemory(Nb, sizeof(*Nb));

    Mdl->Next = NULL;
    Mdl->Length = Length;
    Mdl->Buffer = malloc(Length);
    XDPCPUMAP_TEST_ASSERT(Mdl->Buffer != NULL);
    _Analysis_assume_(Mdl->Buffer != NULL);
    memcpy(Mdl->Buffer, Payload, Length);

    Nb->MdlChain = Mdl;
    Nb->CurrentMdl = Mdl;
    Nb->CurrentMdlOffset = 0;
    Nb->DataOffset = 0;
    Nb->DataLength = Length;

    Nbl->FirstNetBuffer = Nb;

    //
    // Pointer-owned metadata, backed by a real allocation this source OWNS.
    // XdpCpuMapTestDeleteSourceNbl frees it, so a build that carried the pointer
    // instead of refusing the redirect is left holding freed memory -- which ASan
    // reports if anything reads it, and which a NULL check catches even if
    // nothing does. This is what made the B5 defect invisible before: the model
    // had no notion of a slot whose contents outlive nothing.
    //
    // Opt-in, because production REFUSES a source carrying one of these slots.
    // A test turns it on only when the refusal is what it is testing.
    //
    if (XdpCpuMapTestSourceMetadataBlobs) {
        for (ULONG Index = 0; Index < XdpCpuMapTestPointerOwnedSlotCount; Index++) {
            VOID *Blob = malloc(XDPCPUMAP_TEST_METADATA_BLOB_SIZE);

            XDPCPUMAP_TEST_ASSERT(Blob != NULL);
            _Analysis_assume_(Blob != NULL);
            memset(Blob, 0x5A, XDPCPUMAP_TEST_METADATA_BLOB_SIZE);
            Nbl->NetBufferListInfo[XdpCpuMapTestPointerOwnedSlots[Index]] = Blob;
        }
    }

    //
    // A source NBL is the miniport's, not the pool's, so it is deliberately not
    // counted in XdpCpuMapTestNblLive: that counter exists to catch a leaked
    // COPY, and mixing the two would mask exactly that.
    //
    return Nbl;
}

VOID
XdpCpuMapTestDeleteSourceNbl(
    _In_ NET_BUFFER_LIST *Nbl
    )
{
    NET_BUFFER *Nb = Nbl->FirstNetBuffer;

    XDPCPUMAP_TEST_ASSERT(Nb != NULL);
    _Analysis_assume_(Nb != NULL);

    for (ULONG Index = 0; Index < XdpCpuMapTestPointerOwnedSlotCount; Index++) {
        NDIS_NET_BUFFER_LIST_INFO Slot = XdpCpuMapTestPointerOwnedSlots[Index];

        free(Nbl->NetBufferListInfo[Slot]);
        Nbl->NetBufferListInfo[Slot] = NULL;
    }

    while (Nb->MdlChain != NULL) {
        MDL *Mdl = Nb->MdlChain;

        Nb->MdlChain = Mdl->Next;
        free(Mdl->Buffer);
        free(Mdl);
    }

    _aligned_free(Nb);
    _aligned_free(Nbl);
}

BOOLEAN
XdpCpuMapTestNblPayloadEquals(
    _In_ const NET_BUFFER_LIST *Nbl,
    _In_reads_bytes_(Length) const VOID *Payload,
    _In_ ULONG Length
    )
{
    const NET_BUFFER *Nb = Nbl->FirstNetBuffer;
    const MDL *Mdl;
    ULONG Offset;
    const UCHAR *Expected = Payload;

    if (Nb == NULL || Nb->DataLength != Length) {
        return FALSE;
    }

    Mdl = Nb->CurrentMdl;
    Offset = Nb->CurrentMdlOffset;

    while (Length > 0) {
        ULONG Available;
        ULONG Chunk;

        if (Mdl == NULL) {
            return FALSE;
        }

        if (Offset >= Mdl->Length) {
            Offset -= Mdl->Length;
            Mdl = Mdl->Next;
            continue;
        }

        Available = Mdl->Length - Offset;
        Chunk = min(Available, Length);

        if (memcmp(Mdl->Buffer + Offset, Expected, Chunk) != 0) {
            return FALSE;
        }

        Expected += Chunk;
        Offset += Chunk;
        Length -= Chunk;
    }

    return TRUE;
}
