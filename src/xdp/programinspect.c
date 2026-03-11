//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// This module handles XDP rule parsing and configuration.
//

#include "precomp.h"
#include "programinspect.h"

#define ICMP4_ECHOREPLY_TYPE 0
#define ICMP4_ECHOREPLY_CODE 0
#define ICMP6_ECHOREPLY_TYPE 129
#define ICMP6_ECHOREPLY_CODE 0
#define TCP_HDR_LEN_TO_BYTES(x) (((UINT64)(x)) * 4)

#define XDP_CPUMAP_SMALL_PACKET_BYPASS

//
// Data path routines.
//

static
VOID
XdpInitializeFrameCache(
    _Out_ XDP_PROGRAM_FRAME_CACHE *Cache
    )
{
    Cache->Flags = 0;
}

static
UINT32
XdpGetContiguousHeaderLength(
    _In_ XDP_FRAME *Frame,
    _Inout_ XDP_BUFFER **Buffer,
    _Inout_ UINT32 *BufferDataOffset,
    _Inout_ UINT32 *FragmentIndex,
    _Inout_ UINT32 *FragmentsRemaining,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _In_ VOID *HeaderStorage,
    _In_ UINT32 HeaderSize,
    _Out_ VOID **Header
    )
{
    UCHAR *Hdr = HeaderStorage;

    UNREFERENCED_PARAMETER(Frame);

    while (HeaderSize > 0) {
        UINT32 CopyLength = min(HeaderSize, (*Buffer)->DataLength - *BufferDataOffset);
        UCHAR *Va = XdpGetVirtualAddressExtension(*Buffer, VirtualAddressExtension)->VirtualAddress;

        //
        // If the current buffer is depleted, advance to the next fragment.
        //
        if (CopyLength == 0) {
            if (*FragmentsRemaining == 0) {
                break;
            } else {
                *FragmentIndex = (*FragmentIndex + 1) & FragmentRing->Mask;
                *FragmentsRemaining -= 1;
                *Buffer = XdpRingGetElement(FragmentRing, *FragmentIndex);
                *BufferDataOffset = 0;
                continue;
            }
        }

        //
        // If the buffer contains the contiguous header, return it directly.
        //
        if (Hdr == HeaderStorage && *BufferDataOffset + HeaderSize <= (*Buffer)->DataLength) {
            *Header = Va + (*Buffer)->DataOffset + *BufferDataOffset;
            *BufferDataOffset += HeaderSize;
            return HeaderSize;
        }

        RtlCopyMemory(Hdr, Va + (*Buffer)->DataOffset + *BufferDataOffset, CopyLength);
        *BufferDataOffset += CopyLength;
        Hdr += CopyLength;
        HeaderSize -= CopyLength;
    }

    *Header = HeaderStorage;
    return (UINT32)(Hdr - (UCHAR*)HeaderStorage);
}

static
_Success_(return != FALSE)
BOOLEAN
XdpGetContiguousHeader(
    _In_ XDP_FRAME *Frame,
    _Inout_ XDP_BUFFER **Buffer,
    _Inout_ UINT32 *BufferDataOffset,
    _Inout_ UINT32 *FragmentIndex,
    _Inout_ UINT32 *FragmentsRemaining,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _In_ VOID *HeaderStorage,
    _In_ UINT32 HeaderSize,
    _Out_ VOID **Header
    )
{
    ASSERT(HeaderSize != 0);
    UINT32 ReadLength =
        XdpGetContiguousHeaderLength(
            Frame, Buffer, BufferDataOffset, FragmentIndex, FragmentsRemaining,
            FragmentRing, VirtualAddressExtension, HeaderStorage, HeaderSize, Header);
    return ReadLength == HeaderSize;
}

static
VOID
XdpCopyMemoryToFrame(
    _In_ XDP_FRAME *Frame,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _In_ UINT32 FrameDataOffset,
    _In_ VOID *Data,
    _In_ UINT32 DataLength
    )
{
    XDP_BUFFER *Buffer = &Frame->Buffer;
    UINT32 FragmentCount;

    //
    // The first buffer is stored in the frame ring, so bias the fragment index
    // so the initial increment yields the first buffer in the fragment ring.
    //
    FragmentIndex--;
    FragmentCount = XdpGetFragmentExtension(Frame, FragmentExtension)->FragmentBufferCount;

    while (DataLength > 0) {
        if (FrameDataOffset >= Buffer->DataLength) {
            FrameDataOffset -= Buffer->DataLength;
            FragmentIndex = (FragmentIndex + 1) & FragmentRing->Mask;
            Buffer = XdpRingGetElement(FragmentRing, FragmentIndex);
            ASSERT(FragmentCount > 0);
            FragmentCount--;
        } else {
            UINT32 CopyLength;
            UCHAR *Va;

            CopyLength = min(DataLength, Buffer->DataLength - FrameDataOffset);
            Va = XdpGetVirtualAddressExtension(Buffer, VirtualAddressExtension)->VirtualAddress;
            RtlCopyMemory(Va + Buffer->DataOffset, Data, CopyLength);

            Data = RTL_PTR_ADD(Data, CopyLength);
            DataLength -= CopyLength;
            FrameDataOffset += CopyLength;
        }
    }
}

static
VOID
XdpParseFragmentedEthernet(
    _In_ XDP_FRAME *Frame,
    _Inout_ XDP_BUFFER **Buffer,
    _Inout_ UINT32 *BufferDataOffset,
    _Inout_ UINT32 *FragmentIndex,
    _Inout_ UINT32 *FragmentsRemaining,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _Out_ XDP_PROGRAM_FRAME_CACHE *Cache,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *Storage
    )
{
    Cache->EthValid =
        XdpGetContiguousHeader(
            Frame, Buffer, BufferDataOffset, FragmentIndex, FragmentsRemaining, FragmentRing,
            VirtualAddressExtension, &Storage->EthHdr, sizeof(Storage->EthHdr), &Cache->EthHdr);
}

static
VOID
XdpParseFragmentedIp4(
    _In_ XDP_FRAME *Frame,
    _Inout_ XDP_BUFFER **Buffer,
    _Inout_ UINT32 *BufferDataOffset,
    _Inout_ UINT32 *FragmentIndex,
    _Inout_ UINT32 *FragmentsRemaining,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _Out_ XDP_PROGRAM_FRAME_CACHE *Cache,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *Storage
    )
{
    Cache->Ip4Valid =
        XdpGetContiguousHeader(
            Frame, Buffer, BufferDataOffset, FragmentIndex, FragmentsRemaining, FragmentRing,
            VirtualAddressExtension, &Storage->Ip4Hdr, sizeof(Storage->Ip4Hdr), &Cache->Ip4Hdr) &&
        (((UINT64)Cache->Ip4Hdr->HeaderLength) << 2) == sizeof(*Cache->Ip4Hdr);
}

static
VOID
XdpParseFragmentedIp6(
    _In_ XDP_FRAME *Frame,
    _Inout_ XDP_BUFFER **Buffer,
    _Inout_ UINT32 *BufferDataOffset,
    _Inout_ UINT32 *FragmentIndex,
    _Inout_ UINT32 *FragmentsRemaining,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _Out_ XDP_PROGRAM_FRAME_CACHE *Cache,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *Storage
    )
{
    Cache->Ip6Valid =
        XdpGetContiguousHeader(
            Frame, Buffer, BufferDataOffset, FragmentIndex, FragmentsRemaining, FragmentRing,
            VirtualAddressExtension, &Storage->Ip6Hdr, sizeof(Storage->Ip6Hdr), &Cache->Ip6Hdr);
}

static
VOID
XdpParseFragmentedUdp(
    _In_ XDP_FRAME *Frame,
    _Inout_ XDP_BUFFER **Buffer,
    _Inout_ UINT32 *BufferDataOffset,
    _Inout_ UINT32 *FragmentIndex,
    _Inout_ UINT32 *FragmentsRemaining,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _Out_ XDP_PROGRAM_FRAME_CACHE *Cache,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *Storage
    )
{
    Cache->UdpValid =
        XdpGetContiguousHeader(
            Frame, Buffer, BufferDataOffset, FragmentIndex, FragmentsRemaining, FragmentRing,
            VirtualAddressExtension, &Storage->UdpHdr, sizeof(Storage->UdpHdr), &Cache->UdpHdr);
}

static
VOID
XdpParseFragmentedIcmpHeader(
    _In_ XDP_FRAME *Frame,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _In_ IPPROTO IpProto,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *FrameStore,
    _Inout_ XDP_PROGRAM_FRAME_CACHE *FrameCache
    )
{
    XDP_BUFFER *Buffer = FrameCache->IpPayload.Buffer;
    UINT32 BufferDataOffset = FrameCache->IpPayload.BufferDataOffset;
    UINT32 FragmentCount = FrameCache->IpPayload.FragmentCount;

    if (FrameCache->IpPayload.IsFragmentedBuffer) {
        FragmentIndex = FrameCache->IpPayload.FragmentIndex;
    } else {
        FragmentIndex--;
        FragmentCount = XdpGetFragmentExtension(Frame, FragmentExtension)->FragmentBufferCount;
    }

    if (IpProto == IPPROTO_IPV4) {
        FrameCache->Icmp4Valid =
            XdpGetContiguousHeader(
                Frame, &Buffer, &BufferDataOffset, &FragmentIndex, &FragmentCount, FragmentRing,
                VirtualAddressExtension, &FrameStore->Icmpv4Hdr, sizeof(FrameStore->Icmpv4Hdr), &FrameCache->Icmpv4Hdr);
    } else if (IpProto == IPPROTO_IPV6) {
        FrameCache->Icmp6Valid =
            XdpGetContiguousHeader(
                Frame, &Buffer, &BufferDataOffset, &FragmentIndex, &FragmentCount, FragmentRing,
                VirtualAddressExtension, &FrameStore->Icmpv6Hdr, sizeof(FrameStore->Icmpv6Hdr), &FrameCache->Icmpv6Hdr);
    }
}

static
VOID
XdpParseIcmp4Header(
    _In_ XDP_FRAME *Frame,
    _In_opt_ XDP_RING *FragmentRing,
    _In_opt_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _In_ XDP_PROGRAM_PAYLOAD_CACHE *Payload,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *FrameStore,
    _Inout_ XDP_PROGRAM_FRAME_CACHE *FrameCache
    )
{
    UINT32 BufferDataOffset = Payload->BufferDataOffset;
    XDP_BUFFER *Buffer = Payload->Buffer;
    UCHAR *Va = XdpGetVirtualAddressExtension(Buffer, VirtualAddressExtension)->VirtualAddress + Buffer->DataOffset;
    FrameCache->Icmp4Cached = TRUE;

    IPPROTO IpProto = IPPROTO_MAX;
    if (FrameCache->Ip4Valid) {
        IpProto = FrameCache->Ip4Hdr->Protocol;
        if (Buffer->DataLength < BufferDataOffset + sizeof(*FrameCache->Icmpv4Hdr)) {
            goto BufferTooSmall;
        }
        FrameCache->Icmp4Valid = TRUE;
        FrameCache->Icmpv4Hdr = (ICMP_HEADER *) &Va[BufferDataOffset];
    } else {
        return;
    }

BufferTooSmall:

    if (FragmentRing != NULL) {
        ASSERT(FragmentExtension);
        XdpParseFragmentedIcmpHeader(
            Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
            IpProto, FrameStore, FrameCache);
    }
}

static
VOID
XdpParseIcmp6Header(
    _In_ XDP_FRAME *Frame,
    _In_opt_ XDP_RING *FragmentRing,
    _In_opt_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _In_ XDP_PROGRAM_PAYLOAD_CACHE *Payload,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *FrameStore,
    _Inout_ XDP_PROGRAM_FRAME_CACHE *FrameCache
    )
{
    UINT32 BufferDataOffset = Payload->BufferDataOffset;
    XDP_BUFFER *Buffer = Payload->Buffer;
    UCHAR *Va = XdpGetVirtualAddressExtension(Buffer, VirtualAddressExtension)->VirtualAddress + Buffer->DataOffset;
    FrameCache->Icmp6Cached = TRUE;

    IPPROTO IpProto = IPPROTO_MAX;
    if (FrameCache->Ip6Valid) {
        IpProto = FrameCache->Ip6Hdr->NextHeader;
        if (Buffer->DataLength < BufferDataOffset + sizeof(*FrameCache->Icmpv6Hdr)) {
            goto BufferTooSmall;
        }
        FrameCache->Icmp6Valid = TRUE;
        FrameCache->Icmpv6Hdr = (ICMPV6_HEADER *) &Va[BufferDataOffset];
    } else {
        return;
    }

BufferTooSmall:

    if (FragmentRing != NULL) {
        ASSERT(FragmentExtension);
        XdpParseFragmentedIcmpHeader(
            Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
            IpProto, FrameStore, FrameCache);
    }
}

static
VOID
XdpParseFragmentedTcp(
    _In_ XDP_FRAME *Frame,
    _Inout_ XDP_BUFFER **Buffer,
    _Inout_ UINT32 *BufferDataOffset,
    _Inout_ UINT32 *FragmentIndex,
    _Inout_ UINT32 *FragmentsRemaining,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _Out_ XDP_PROGRAM_FRAME_CACHE *Cache,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *Storage
    )
{
    UINT32 HeaderLengh;
    BOOLEAN Valid =
        XdpGetContiguousHeader(
            Frame, Buffer, BufferDataOffset, FragmentIndex, FragmentsRemaining, FragmentRing,
            VirtualAddressExtension, &Storage->TcpHdr, sizeof(Storage->TcpHdr), &Cache->TcpHdr);
    if (!Valid) {
        return;
    }

    HeaderLengh = TCP_HDR_LEN_TO_BYTES(Cache->TcpHdr->th_len);
    if (HeaderLengh < sizeof(Storage->TcpHdr)) {
        return;
    }

    if (HeaderLengh > sizeof(Storage->TcpHdr)) {
        //
        // Attempt to read TCP options.
        //
        Valid =
            XdpGetContiguousHeader(
                Frame, Buffer, BufferDataOffset, FragmentIndex, FragmentsRemaining, FragmentRing,
                VirtualAddressExtension,
                &Storage->TcpHdrOptions,
                TCP_HDR_LEN_TO_BYTES(Cache->TcpHdr->th_len) - sizeof(Storage->TcpHdr),
                &Cache->TcpHdrOptions);
    }

    Cache->TcpValid = Valid;
}

static
VOID
XdpParseFragmentedFrame(
    _In_ XDP_FRAME *Frame,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _In_ UINT32 BufferDataOffset,
    _Inout_ XDP_PROGRAM_FRAME_CACHE *Cache,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *Storage
    )
{
    XDP_BUFFER *Buffer = &Frame->Buffer;
    UINT32 FragmentCount;
    IPPROTO IpProto = IPPROTO_MAX;

    //
    // The first buffer is stored in the frame ring, so bias the fragment index
    // so the initial increment yields the first buffer in the fragment ring.
    //
    FragmentIndex--;
    FragmentCount = XdpGetFragmentExtension(Frame, FragmentExtension)->FragmentBufferCount;

    if (!Cache->EthValid) {
        XdpParseFragmentedEthernet(
            Frame, &Buffer, &BufferDataOffset, &FragmentIndex, &FragmentCount, FragmentRing,
            VirtualAddressExtension, Cache, Storage);

        if (!Cache->EthValid) {
            return;
        }
    }

    if (Cache->EthHdr->Type == htons(ETHERNET_TYPE_IPV4)) {
        if (!Cache->Ip4Valid) {
            XdpParseFragmentedIp4(
                Frame, &Buffer, &BufferDataOffset, &FragmentIndex, &FragmentCount, FragmentRing,
                VirtualAddressExtension, Cache, Storage);

            if (!Cache->Ip4Valid) {
                return;
            }
        }
        IpProto = Cache->Ip4Hdr->Protocol;
    } else if (Cache->EthHdr->Type == htons(ETHERNET_TYPE_IPV6)) {
        if (!Cache->Ip6Valid) {
            XdpParseFragmentedIp6(
                Frame, &Buffer, &BufferDataOffset, &FragmentIndex, &FragmentCount, FragmentRing,
                VirtualAddressExtension, Cache, Storage);

            if (!Cache->Ip6Valid) {
                return;
            }
        }
        IpProto = Cache->Ip6Hdr->NextHeader;
    } else {
        return;
    }

    if (IpProto == IPPROTO_UDP) {
        if (!Cache->UdpValid) {
            XdpParseFragmentedUdp(
                Frame, &Buffer, &BufferDataOffset, &FragmentIndex, &FragmentCount, FragmentRing,
                VirtualAddressExtension, Cache, Storage);

            if (!Cache->UdpValid) {
                return;
            }

            Cache->TransportPayload.Buffer = Buffer;
            Cache->TransportPayload.BufferDataOffset = BufferDataOffset;
            Cache->TransportPayload.FragmentCount = FragmentCount;
            Cache->TransportPayload.FragmentIndex = FragmentIndex;
            Cache->TransportPayload.IsFragmentedBuffer = TRUE;
            Cache->TransportPayloadValid = TRUE;
        }
    } else if (IpProto == IPPROTO_TCP) {
        if (!Cache->TcpValid) {
            XdpParseFragmentedTcp(
                Frame, &Buffer, &BufferDataOffset, &FragmentIndex, &FragmentCount, FragmentRing,
                VirtualAddressExtension, Cache, Storage);

            if (!Cache->TcpValid) {
                return;
            }

            Cache->TransportPayload.Buffer = Buffer;
            Cache->TransportPayload.BufferDataOffset = BufferDataOffset;
            Cache->TransportPayload.FragmentCount = FragmentCount;
            Cache->TransportPayload.FragmentIndex = FragmentIndex;
            Cache->TransportPayload.IsFragmentedBuffer = TRUE;
            Cache->TransportPayloadValid = TRUE;
        }
    } else {
        Cache->IpPayload.Buffer = Buffer;
        Cache->IpPayload.BufferDataOffset = BufferDataOffset;
        Cache->IpPayload.FragmentCount = FragmentCount;
        Cache->IpPayload.FragmentIndex = FragmentIndex;
        Cache->IpPayload.IsFragmentedBuffer = TRUE;
        Cache->IpPayloadValid = TRUE;
    }
}

static
VOID
XdpParseFrame(
    _In_ XDP_FRAME *Frame,
    _In_opt_ XDP_RING *FragmentRing,
    _In_opt_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _Out_ XDP_PROGRAM_FRAME_CACHE *Cache,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *Storage
    )
{
    XDP_BUFFER *Buffer;
    UCHAR *Va;
    IPPROTO IpProto = IPPROTO_MAX;
    UINT32 Offset = 0;

    //
    // This routine always attempts to parse Ethernet through TCP/UDP headers.
    //
    Cache->EthCached = TRUE;
    Cache->Ip4Cached = TRUE;
    Cache->Ip6Cached = TRUE;
    Cache->UdpCached = TRUE;
    Cache->TcpCached = TRUE;
    Cache->TransportPayloadCached = TRUE;

    //
    // Attempt to parse all headers in a single pass over the first buffer. If
    // there's not enough data in the first buffer, fall back to handling
    // discontiguous headers via the fragment ring.
    //

    Buffer = &Frame->Buffer;
    Va = XdpGetVirtualAddressExtension(Buffer, VirtualAddressExtension)->VirtualAddress;
    Va += Buffer->DataOffset;

    if (Buffer->DataLength < sizeof(*Cache->EthHdr)) {
        goto BufferTooSmall;
    }
    Cache->EthHdr = (ETHERNET_HEADER *)&Va[Offset];
    Cache->EthValid = TRUE;
    Offset += sizeof(*Cache->EthHdr);

    if (Cache->EthHdr->Type == htons(ETHERNET_TYPE_IPV4)) {
        if (Buffer->DataLength < Offset + sizeof(*Cache->Ip4Hdr)) {
            goto BufferTooSmall;
        }
        Cache->Ip4Hdr = (IPV4_HEADER *)&Va[Offset];
        if ((((UINT64)Cache->Ip4Hdr->HeaderLength) << 2) != sizeof(*Cache->Ip4Hdr)) {
            return;
        }
        Cache->Ip4Valid = TRUE;
        Offset += sizeof(*Cache->Ip4Hdr);
        IpProto = Cache->Ip4Hdr->Protocol;
    } else if (Cache->EthHdr->Type == htons(ETHERNET_TYPE_IPV6)) {
        if (Buffer->DataLength < Offset + sizeof(*Cache->Ip6Hdr)) {
            goto BufferTooSmall;
        }
        Cache->Ip6Hdr = (IPV6_HEADER *)&Va[Offset];
        Cache->Ip6Valid = TRUE;
        Offset += sizeof(*Cache->Ip6Hdr);
        IpProto = Cache->Ip6Hdr->NextHeader;
    } else {
        return;
    }

    if (IpProto == IPPROTO_UDP) {
        if (Buffer->DataLength < Offset + sizeof(*Cache->UdpHdr)) {
            goto BufferTooSmall;
        }
        Cache->UdpHdr = (UDP_HDR *)&Va[Offset];
        Cache->UdpValid = TRUE;
        Offset += sizeof(*Cache->UdpHdr);
        Cache->TransportPayload.Buffer = Buffer;
        Cache->TransportPayload.BufferDataOffset = Offset;
        Cache->TransportPayload.IsFragmentedBuffer = FALSE;
        Cache->TransportPayloadValid = TRUE;
    } else if (IpProto == IPPROTO_TCP) {
        UINT32 HeaderLength;
        if (Buffer->DataLength < Offset + sizeof(*Cache->TcpHdr)) {
            goto BufferTooSmall;
        }

        HeaderLength = TCP_HDR_LEN_TO_BYTES(((TCP_HDR *)&Va[Offset])->th_len);
        if (Buffer->DataLength < Offset + HeaderLength) {
            goto BufferTooSmall;
        }

        Cache->TcpHdr = (TCP_HDR *)&Va[Offset];
        Cache->TcpValid = TRUE;
        Offset += HeaderLength;
        Cache->TransportPayload.Buffer = Buffer;
        Cache->TransportPayload.BufferDataOffset = Offset;
        Cache->TransportPayload.IsFragmentedBuffer = FALSE;
        Cache->TransportPayloadValid = TRUE;
    } else {
        Cache->IpPayload.Buffer = Buffer;
        Cache->IpPayload.BufferDataOffset = Offset;
        Cache->IpPayload.IsFragmentedBuffer = FALSE;
        Cache->IpPayloadValid = TRUE;
    }

    return;

BufferTooSmall:

    if (FragmentRing != NULL) {
        ASSERT(FragmentExtension);
        XdpParseFragmentedFrame(
            Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
            Offset, Cache, Storage);
    }
}

static
BOOLEAN
Ipv4PrefixMatch(
    _In_ const IN_ADDR *Ip,
    _In_ const IN_ADDR *Prefix,
    _In_ const IN_ADDR *Mask
    )
{
    return (Ip->s_addr & Mask->s_addr) == Prefix->s_addr;
}

static
BOOLEAN
Ipv6PrefixMatch(
    _In_ const IN6_ADDR *Ip,
    _In_ const IN6_ADDR *Prefix,
    _In_ const IN6_ADDR *Mask
    )
{
    const UINT64 *Ip64 = (CONST UINT64 *)Ip;
    const UINT64 *Prefix64 = (CONST UINT64 *)Prefix;
    const UINT64 *Mask64 = (CONST UINT64 *)Mask;

    return
        ((Ip64[0] & Mask64[0]) == Prefix64[0]) &
        ((Ip64[1] & Mask64[1]) == Prefix64[1]);
}

static
BOOLEAN
UdpTupleMatch(
    _In_ XDP_MATCH_TYPE Type,
    _In_ const XDP_PROGRAM_FRAME_CACHE *Cache,
    _In_ const XDP_TUPLE *Tuple
    )
{
    if (Cache->EthHdr->Type == htons(ETHERNET_TYPE_IPV4)) {
        return
            Type == XDP_MATCH_IPV4_UDP_TUPLE &&
            Cache->UdpHdr->uh_sport == Tuple->SourcePort &&
            Cache->UdpHdr->uh_dport == Tuple->DestinationPort &&
            IN4_ADDR_EQUAL(&Cache->Ip4Hdr->SourceAddress, &Tuple->SourceAddress.Ipv4) &&
            IN4_ADDR_EQUAL(&Cache->Ip4Hdr->DestinationAddress, &Tuple->DestinationAddress.Ipv4);
    } else { // IPv6
        return
            Type == XDP_MATCH_IPV6_UDP_TUPLE &&
            Cache->UdpHdr->uh_sport == Tuple->SourcePort &&
            Cache->UdpHdr->uh_dport == Tuple->DestinationPort &&
            IN6_ADDR_EQUAL(&Cache->Ip6Hdr->SourceAddress, &Tuple->SourceAddress.Ipv6) &&
            IN6_ADDR_EQUAL(&Cache->Ip6Hdr->DestinationAddress, &Tuple->DestinationAddress.Ipv6);
    }
}

static
BOOLEAN
QuicCidMatch(
    _In_ XDP_MATCH_TYPE Type,
    _In_ const XDP_PROGRAM_FRAME_CACHE *QuicHeader,
    _In_ const XDP_QUIC_FLOW *Flow
    )
{
    if ((Type == XDP_MATCH_QUIC_FLOW_SRC_CID ||
         Type == XDP_MATCH_TCP_QUIC_FLOW_SRC_CID) !=
        (QuicHeader->QuicIsLongHeader == 1)) {
        return FALSE;
    }
    ASSERT(Flow->CidOffset + Flow->CidLength <= XDP_QUIC_MAX_CID_LENGTH);
    if (QuicHeader->QuicCidLength < Flow->CidOffset + Flow->CidLength) {
        return FALSE;
    }
    return memcmp(&QuicHeader->QuicCid[Flow->CidOffset], Flow->CidData, Flow->CidLength) == 0;
}

static
_Success_(return != FALSE)
BOOLEAN
XdpParseQuicHeaderPayload(
    _In_ const UINT8 *Payload,
    _In_ UINT32 DataLength,
    _Inout_ XDP_PROGRAM_FRAME_CACHE *FrameCache
    )
{
    const QUIC_HEADER_INVARIANT* QuicHdr = (CONST QUIC_HEADER_INVARIANT*)Payload;

    if (DataLength < RTL_SIZEOF_THROUGH_FIELD(QUIC_HEADER_INVARIANT, COMMON_HDR)) {
        return FALSE;
    }

    if (QuicHdr->COMMON_HDR.IsLongHeader) {
        if (DataLength < RTL_SIZEOF_THROUGH_FIELD(QUIC_HEADER_INVARIANT, LONG_HDR)) {
            return FALSE;
        }
        if (QuicHdr->LONG_HDR.DestCidLength > XDP_QUIC_MAX_CID_LENGTH) {
            return FALSE;
        }
        if (DataLength <
                RTL_SIZEOF_THROUGH_FIELD(QUIC_HEADER_INVARIANT, LONG_HDR) +
                QuicHdr->LONG_HDR.DestCidLength + sizeof(UCHAR)) {
            return FALSE;
        }
        FrameCache->QuicCidLength =
            QuicHdr->LONG_HDR.DestCid[QuicHdr->LONG_HDR.DestCidLength];
        if (DataLength <
                RTL_SIZEOF_THROUGH_FIELD(QUIC_HEADER_INVARIANT, LONG_HDR) +
                QuicHdr->LONG_HDR.DestCidLength +
                sizeof(UCHAR) +
                FrameCache->QuicCidLength) {
            return FALSE;
        }
        FrameCache->QuicCid =
            QuicHdr->LONG_HDR.DestCid +
            QuicHdr->LONG_HDR.DestCidLength +
            sizeof(UCHAR);

        //
        // Capture Dest CID for QUIC CID hashing (CPU redirect).
        //
        FrameCache->QuicDestCidLength = QuicHdr->LONG_HDR.DestCidLength;
        FrameCache->QuicDestCid = QuicHdr->LONG_HDR.DestCid;
        FrameCache->QuicDestCidValid = TRUE;

        FrameCache->QuicValid = TRUE;
        FrameCache->QuicIsLongHeader = TRUE;
        return TRUE;
    }

    FrameCache->QuicCidLength =
        (UINT8)min(
            DataLength - RTL_SIZEOF_THROUGH_FIELD(QUIC_HEADER_INVARIANT, SHORT_HDR),
            XDP_QUIC_MAX_CID_LENGTH);
    FrameCache->QuicCid = QuicHdr->SHORT_HDR.DestCid;

    //
    // Short header: the only CID is the Dest CID.
    //
    FrameCache->QuicDestCidLength = FrameCache->QuicCidLength;
    FrameCache->QuicDestCid = FrameCache->QuicCid;
    FrameCache->QuicDestCidValid = TRUE;

    FrameCache->QuicValid = TRUE;
    FrameCache->QuicIsLongHeader = FALSE;
    return FrameCache->QuicCidLength == XDP_QUIC_MAX_CID_LENGTH;
}

static
VOID
XdpParseFragmentedQuicHeader(
    _In_ XDP_FRAME *Frame,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *FrameStore,
    _Inout_ XDP_PROGRAM_FRAME_CACHE *FrameCache
    )
{
    XDP_BUFFER *Buffer = FrameCache->TransportPayload.Buffer;
    UINT32 BufferDataOffset = FrameCache->TransportPayload.BufferDataOffset;
    UINT32 FragmentCount = FrameCache->TransportPayload.FragmentCount;
    UINT8* QuicPayload = NULL;
    UINT32 ReadLength;

    if (FrameCache->TransportPayload.IsFragmentedBuffer) {
        FragmentIndex = FrameCache->TransportPayload.FragmentIndex;
    } else {
        //
        // The first buffer is stored in the frame ring, so bias the fragment index
        // so the initial increment yields the first buffer in the fragment ring.
        //
        FragmentIndex--;
        FragmentCount = XdpGetFragmentExtension(Frame, FragmentExtension)->FragmentBufferCount;
    }

    ReadLength =
        XdpGetContiguousHeaderLength(
            Frame, &Buffer, &BufferDataOffset, &FragmentIndex, &FragmentCount,
            FragmentRing, VirtualAddressExtension, FrameStore->QuicStorage,
            ARRAYSIZE(FrameStore->QuicStorage), &QuicPayload);

    XdpParseQuicHeaderPayload(QuicPayload, ReadLength, FrameCache);
}

static
VOID
XdpParseQuicHeader(
    _In_ XDP_FRAME *Frame,
    _In_opt_ XDP_RING *FragmentRing,
    _In_opt_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _In_ XDP_PROGRAM_PAYLOAD_CACHE *Payload,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *FrameStore,
    _Inout_ XDP_PROGRAM_FRAME_CACHE *FrameCache
    )
{
    UINT32 BufferDataOffset = Payload->BufferDataOffset;
    XDP_BUFFER *Buffer = Payload->Buffer;
    UCHAR *Va =
        XdpGetVirtualAddressExtension(Payload->Buffer, VirtualAddressExtension)->VirtualAddress;
    Va += Buffer->DataOffset;

    FrameCache->QuicCached = TRUE;

    if (Buffer->DataLength < BufferDataOffset) {
        goto BufferTooSmall;
    }

    if (XdpParseQuicHeaderPayload(
            &Va[BufferDataOffset], Buffer->DataLength - BufferDataOffset, FrameCache)) {
        return;
    }

BufferTooSmall:

    if (FragmentRing != NULL) {
        ASSERT(FragmentExtension);
        XdpParseFragmentedQuicHeader(
            Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
            FrameStore, FrameCache);
    }
}

static
_Success_(return != FALSE)
BOOLEAN
XdpParseInnerIpHeaderPayload(
    _In_ const UINT8 *Payload,
    _In_ UINT32 DataLength,
    _In_ IPPROTO IpProto,
    _Inout_ XDP_PROGRAM_FRAME_CACHE *FrameCache
    )
{
    if (IpProto == IPPROTO_IPV4) {
        if (DataLength < sizeof(IPV4_HEADER)) {
            return FALSE;
        }

        FrameCache->InnerIp4Hdr = (IPV4_HEADER *)Payload;
        if (FrameCache->InnerIp4Hdr->Version == IPV4_VERSION &&
            (((UINT64)FrameCache->InnerIp4Hdr->HeaderLength) << 2) == sizeof(IPV4_HEADER)) {
            FrameCache->InnerIp4Valid = TRUE;
        }
    } else if (IpProto == IPPROTO_IPV6) {
        if (DataLength < sizeof(IPV6_HEADER)) {
            return FALSE;
        }

        FrameCache->InnerIp6Hdr = (IPV6_HEADER *)Payload;
        if ((FrameCache->InnerIp6Hdr->VersionClassFlow & IP_VER_MASK) == IPV6_VERSION) {
            FrameCache->InnerIp6Valid = TRUE;
        }
    }

    return TRUE;
}

static
VOID
XdpParseFragmentedInnerIpHeader(
    _In_ XDP_FRAME *Frame,
    _In_ XDP_RING *FragmentRing,
    _In_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _In_ IPPROTO IpProto,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *FrameStore,
    _Inout_ XDP_PROGRAM_FRAME_CACHE *FrameCache
    )
{
    XDP_BUFFER *Buffer = FrameCache->IpPayload.Buffer;
    UINT32 BufferDataOffset = FrameCache->IpPayload.BufferDataOffset;
    UINT32 FragmentCount = FrameCache->IpPayload.FragmentCount;
    UINT8* IpPayload = NULL;
    UINT32 ReadLength;

    if (FrameCache->IpPayload.IsFragmentedBuffer) {
        FragmentIndex = FrameCache->IpPayload.FragmentIndex;
    } else {
        FragmentIndex--;
        FragmentCount = XdpGetFragmentExtension(Frame, FragmentExtension)->FragmentBufferCount;
    }

    if (IpProto == IPPROTO_IPV4) {
        ReadLength =
            XdpGetContiguousHeaderLength(
                Frame, &Buffer, &BufferDataOffset, &FragmentIndex, &FragmentCount,
                FragmentRing, VirtualAddressExtension, &FrameStore->InnerIp4Hdr,
                sizeof(FrameStore->InnerIp4Hdr), &IpPayload);
    } else if (IpProto == IPPROTO_IPV6) {
        ReadLength =
            XdpGetContiguousHeaderLength(
                Frame, &Buffer, &BufferDataOffset, &FragmentIndex, &FragmentCount,
                FragmentRing, VirtualAddressExtension, &FrameStore->InnerIp6Hdr,
                sizeof(FrameStore->InnerIp6Hdr), &IpPayload);
    } else {
        return;
    }

    XdpParseInnerIpHeaderPayload(IpPayload, ReadLength, IpProto, FrameCache);
}

static
VOID
XdpParseInnerIpHeader(
    _In_ XDP_FRAME *Frame,
    _In_opt_ XDP_RING *FragmentRing,
    _In_opt_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _In_ XDP_PROGRAM_PAYLOAD_CACHE *Payload,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *FrameStore,
    _Inout_ XDP_PROGRAM_FRAME_CACHE *FrameCache
    )
{
    UINT32 BufferDataOffset = Payload->BufferDataOffset;
    XDP_BUFFER *Buffer = Payload->Buffer;
    UCHAR *Va = XdpGetVirtualAddressExtension(Buffer, VirtualAddressExtension)->VirtualAddress + Buffer->DataOffset;
    FrameCache->InnerIpCached = TRUE;

    IPPROTO IpProto = IPPROTO_MAX;
    if (FrameCache->Ip4Valid) {
        IpProto = FrameCache->Ip4Hdr->Protocol;
    } else if (FrameCache->Ip6Valid) {
        IpProto = FrameCache->Ip6Hdr->NextHeader;
    } else {
        return;
    }

    if (Buffer->DataLength < BufferDataOffset) {
        goto BufferTooSmall;
    }

    if (XdpParseInnerIpHeaderPayload(&Va[BufferDataOffset], Buffer->DataLength - BufferDataOffset, IpProto, FrameCache)) {
        return;
    }

BufferTooSmall:

    if (FragmentRing != NULL) {
        ASSERT(FragmentExtension);
        XdpParseFragmentedInnerIpHeader(
            Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
            IpProto, FrameStore, FrameCache);
    }
}

static
BOOLEAN
XdpTestBitNoFence(
    _In_ const UINT8 *BitMap,
    _In_ UINT32 Index
    )
{
    return (ReadUCharNoFence(&BitMap[Index >> 3]) >> (Index & 0x7)) & 0x1;
}

static
XDP_RX_ACTION
XdpL2Fwd(
    _In_ XDP_FRAME *Frame,
    _In_opt_ XDP_RING *FragmentRing,
    _In_opt_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension,
    _Inout_ XDP_PROGRAM_FRAME_CACHE *Cache,
    _Inout_ XDP_PROGRAM_FRAME_STORAGE *Storage,
    _Inout_ XDP_PCW_RX_QUEUE *RxQueueStats
    )
{
    DL_EUI48 TempDlAddress;

    if (!Cache->EthCached) {
        XdpParseFrame(
            Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
            Cache, Storage);
    }

    if (!Cache->EthValid) {
        STAT_INC(RxQueueStats, InspectFramesDropped);

        return XDP_RX_ACTION_DROP;
    }

    TempDlAddress = Cache->EthHdr->Destination;
    Cache->EthHdr->Destination = Cache->EthHdr->Source;
    Cache->EthHdr->Source = TempDlAddress;

    if (Frame->Buffer.DataLength < sizeof(*Cache->EthHdr)) {
        ASSERT(FragmentRing != NULL);
        ASSERT(FragmentExtension != NULL);
        XdpCopyMemoryToFrame(
            Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension, 0,
            Cache->EthHdr, sizeof(*Cache->EthHdr));
    }

    STAT_INC(RxQueueStats, InspectFramesForwarded);

    return XDP_RX_ACTION_TX;
}

static
UINT32
XdpComputeQuicCidHash(
    _In_reads_(CidLength) const UINT8 *Cid,
    _In_ UINT8 CidOffset,
    _In_ UINT8 CidHashLength,
    _In_ UINT8 CidLength
    )
{
    const UINT8 *Data;
    UINT32 Len;
    UINT32 Hash = 0;
    const UINT32 C1 = 0xcc9e2d51;
    const UINT32 C2 = 0x1b873593;

    //
    // If CID is too short for the requested range, return 0 so caller
    // falls back to 5-tuple hashing.
    //
    if (CidOffset + CidHashLength > CidLength) {
        return 0;
    }

    Data = Cid + CidOffset;
    Len = CidHashLength;

    //
    // Murmur3-32 body: process 4-byte chunks.
    //
    while (Len >= 4) {
        UINT32 K;
        RtlCopyMemory(&K, Data, sizeof(K));
        K *= C1;
        K = (K << 15) | (K >> 17);
        K *= C2;
        Hash ^= K;
        Hash = (Hash << 13) | (Hash >> 19);
        Hash = Hash * 5 + 0xe6546b64;
        Data += 4;
        Len -= 4;
    }

    //
    // Handle tail bytes (1-3).
    //
    if (Len > 0) {
        UINT32 K = 0;
        RtlCopyMemory(&K, Data, Len);
        K *= C1;
        K = (K << 15) | (K >> 17);
        K *= C2;
        Hash ^= K;
    }

    //
    // Note: caller applies the Murmur3 finalizer (shared with 5-tuple path).
    //
    return Hash;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
XDP_RX_ACTION
XdpInspect(
    _In_ XDP_PROGRAM *Program,
    _In_ XDP_INSPECTION_CONTEXT *InspectionContext,
    _In_ XDP_RING *FrameRing,
    _In_ UINT32 FrameIndex,
    _In_opt_ XDP_RING *FragmentRing,
    _In_opt_ XDP_EXTENSION *FragmentExtension,
    _In_ UINT32 FragmentIndex,
    _In_ XDP_EXTENSION *VirtualAddressExtension
    )
{
    XDP_RX_ACTION Action = XDP_RX_ACTION_PASS;
    XDP_PROGRAM_FRAME_CACHE FrameCache;
    XDP_FRAME *Frame;
    BOOLEAN Matched = FALSE;
    XDP_PCW_RX_QUEUE *RxQueueStats = XdpRxQueueGetStatsFromInspectionContext(InspectionContext);

    ASSERT(FrameIndex <= FrameRing->Mask);
    ASSERT(
        (FragmentRing == NULL && FragmentIndex == 0) ||
        (FragmentRing && FragmentIndex <= FragmentRing->Mask));

    XdpInitializeFrameCache(&FrameCache);
    Frame = XdpRingGetElement(FrameRing, FrameIndex);

    for (ULONG RuleIndex = 0; RuleIndex < Program->RuleCount; RuleIndex++) {
        XDP_RULE *Rule = &Program->Rules[RuleIndex];

        //
        // Check the match conditions.
        //

        switch (Rule->Match) {
        case XDP_MATCH_ALL:
            Matched = TRUE;
            break;

        case XDP_MATCH_UDP:
            if (!FrameCache.UdpCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.UdpValid) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_UDP_DST:
            if (!FrameCache.UdpCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.UdpValid &&
                FrameCache.UdpHdr->uh_dport == Rule->Pattern.Port) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_IPV4_DST_MASK:
            if (!FrameCache.Ip4Cached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.Ip4Valid &&
                Ipv4PrefixMatch(
                    &FrameCache.Ip4Hdr->DestinationAddress, &Rule->Pattern.IpMask.Address.Ipv4,
                    &Rule->Pattern.IpMask.Mask.Ipv4)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_IPV6_DST_MASK:
            if (!FrameCache.Ip6Cached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.Ip6Valid &&
                Ipv6PrefixMatch(
                    &FrameCache.Ip6Hdr->DestinationAddress,
                    &Rule->Pattern.IpMask.Address.Ipv6,
                    &Rule->Pattern.IpMask.Mask.Ipv6)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_INNER_IPV4_DST_MASK_UDP:
            if (!FrameCache.Ip4Cached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }

            if (!FrameCache.IpPayloadValid) {
                break;
            }

            if (!FrameCache.InnerIpCached) {
                XdpParseInnerIpHeader(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache.IpPayload, &Program->FrameStorage, &FrameCache);
            }

            if (FrameCache.InnerIp4Valid &&
                FrameCache.InnerIp4Hdr->Protocol == IPPROTO_UDP &&
                Ipv4PrefixMatch(
                    &FrameCache.InnerIp4Hdr->DestinationAddress, &Rule->Pattern.IpMask.Address.Ipv4,
                    &Rule->Pattern.IpMask.Mask.Ipv4)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_INNER_IPV6_DST_MASK_UDP:
            if (!FrameCache.Ip6Cached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }

            if (!FrameCache.IpPayloadValid) {
                break;
            }

            if (!FrameCache.InnerIpCached) {
                XdpParseInnerIpHeader(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache.IpPayload, &Program->FrameStorage, &FrameCache);
            }

            if (FrameCache.InnerIp6Valid &&
                FrameCache.InnerIp6Hdr->NextHeader == IPPROTO_UDP &&
                Ipv6PrefixMatch(
                    &FrameCache.InnerIp6Hdr->DestinationAddress,
                    &Rule->Pattern.IpMask.Address.Ipv6,
                    &Rule->Pattern.IpMask.Mask.Ipv6)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_QUIC_FLOW_SRC_CID:
        case XDP_MATCH_QUIC_FLOW_DST_CID:
            if (!FrameCache.UdpCached || !FrameCache.TransportPayloadCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }

            if (!FrameCache.UdpValid || !FrameCache.TransportPayloadValid ||
                FrameCache.UdpHdr->uh_dport != Rule->Pattern.QuicFlow.UdpPort) {
                break;
            }

            if (!FrameCache.QuicCached) {
                XdpParseQuicHeader(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache.TransportPayload, &Program->FrameStorage, &FrameCache);
            }

            if (FrameCache.QuicValid &&
                QuicCidMatch(
                    Rule->Match,
                    &FrameCache,
                    &Rule->Pattern.QuicFlow)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_IPV4_UDP_TUPLE:
        case XDP_MATCH_IPV6_UDP_TUPLE:
            if (!FrameCache.UdpCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.UdpValid &&
                UdpTupleMatch(
                    Rule->Match,
                    &FrameCache,
                    &Rule->Pattern.Tuple)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_UDP_PORT_SET:
            if (!FrameCache.UdpCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.UdpValid &&
                XdpTestBitNoFence(Rule->Pattern.PortSet.PortSet, FrameCache.UdpHdr->uh_dport)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_IPV4_UDP_PORT_SET:
            if (!FrameCache.UdpCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.Ip4Valid &&
                IN4_ADDR_EQUAL(
                    &FrameCache.Ip4Hdr->DestinationAddress,
                    &Rule->Pattern.IpPortSet.Address.Ipv4) &&
                FrameCache.UdpValid &&
                XdpTestBitNoFence(Rule->Pattern.IpPortSet.PortSet.PortSet, FrameCache.UdpHdr->uh_dport)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_IPV6_UDP_PORT_SET:
            if (!FrameCache.UdpCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.Ip6Valid &&
                IN6_ADDR_EQUAL(
                    &FrameCache.Ip6Hdr->DestinationAddress,
                    &Rule->Pattern.IpPortSet.Address.Ipv6) &&
                FrameCache.UdpValid &&
                XdpTestBitNoFence(Rule->Pattern.IpPortSet.PortSet.PortSet, FrameCache.UdpHdr->uh_dport)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_IPV4_TCP_PORT_SET:
            if (!FrameCache.TcpCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.Ip4Valid &&
                IN4_ADDR_EQUAL(
                    &FrameCache.Ip4Hdr->DestinationAddress,
                    &Rule->Pattern.IpPortSet.Address.Ipv4) &&
                FrameCache.TcpValid &&
                XdpTestBitNoFence(Rule->Pattern.IpPortSet.PortSet.PortSet, FrameCache.TcpHdr->th_dport)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_IPV6_TCP_PORT_SET:
            if (!FrameCache.TcpCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.Ip6Valid &&
                IN6_ADDR_EQUAL(
                    &FrameCache.Ip6Hdr->DestinationAddress,
                    &Rule->Pattern.IpPortSet.Address.Ipv6) &&
                FrameCache.TcpValid &&
                XdpTestBitNoFence(Rule->Pattern.IpPortSet.PortSet.PortSet, FrameCache.TcpHdr->th_dport)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_TCP_DST:
            if (!FrameCache.TcpCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.TcpValid &&
                FrameCache.TcpHdr->th_dport == Rule->Pattern.Port) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_TCP_QUIC_FLOW_SRC_CID:
        case XDP_MATCH_TCP_QUIC_FLOW_DST_CID:
            if (!FrameCache.TcpCached || !FrameCache.TransportPayloadCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }

            if (!FrameCache.TcpValid || !FrameCache.TransportPayloadValid ||
                FrameCache.TcpHdr->th_dport != Rule->Pattern.QuicFlow.UdpPort) {
                break;
            }

            if (!FrameCache.QuicCached) {
                XdpParseQuicHeader(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache.TransportPayload, &Program->FrameStorage, &FrameCache);
            }

            if (FrameCache.QuicValid &&
                QuicCidMatch(
                    Rule->Match,
                    &FrameCache,
                    &Rule->Pattern.QuicFlow)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_TCP_CONTROL_DST:
            if (!FrameCache.TcpCached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if (FrameCache.TcpValid &&
                FrameCache.TcpHdr->th_dport == Rule->Pattern.Port &&
                (FrameCache.TcpHdr->th_flags & (TH_SYN | TH_FIN | TH_RST)) != 0) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_IP_NEXT_HEADER:
            if (!(FrameCache.Ip4Cached || FrameCache.Ip6Cached)) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }
            if ((FrameCache.Ip4Valid && FrameCache.Ip4Hdr->Protocol == Rule->Pattern.NextHeader) ||
                (FrameCache.Ip6Valid && FrameCache.Ip6Hdr->NextHeader == Rule->Pattern.NextHeader)) {
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_ICMPV4_ECHO_REPLY_IP_DST:
            if (!FrameCache.Ip4Cached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }

            if (!FrameCache.IpPayloadValid || !FrameCache.Ip4Valid) {
                break;
            }

            if (!FrameCache.Icmp4Cached) {
                XdpParseIcmp4Header(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache.IpPayload, &Program->FrameStorage, &FrameCache);
            }

            if (FrameCache.Icmp4Valid &&
                FrameCache.Icmpv4Hdr->Type == ICMP4_ECHOREPLY_TYPE &&
                FrameCache.Icmpv4Hdr->Code == ICMP4_ECHOREPLY_CODE &&
                IN4_ADDR_EQUAL(
                    &FrameCache.Ip4Hdr->DestinationAddress,
                    &Rule->Pattern.IpMask.Address.Ipv4)) {
                ASSERT(FrameCache.Ip4Valid);
                Matched = TRUE;
            }
            break;

        case XDP_MATCH_ICMPV6_ECHO_REPLY_IP_DST:
            if (!FrameCache.Ip6Cached) {
                XdpParseFrame(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache, &Program->FrameStorage);
            }

            if (!FrameCache.IpPayloadValid || !FrameCache.Ip6Valid) {
                break;
            }

            if (!FrameCache.Icmp6Cached) {
                XdpParseIcmp6Header(
                    Frame, FragmentRing, FragmentExtension, FragmentIndex, VirtualAddressExtension,
                    &FrameCache.IpPayload, &Program->FrameStorage, &FrameCache);
            }

            if (FrameCache.Icmp6Valid &&
                FrameCache.Icmpv6Hdr->Type == ICMP6_ECHOREPLY_TYPE &&
                FrameCache.Icmpv6Hdr->Code == ICMP6_ECHOREPLY_CODE &&
                IN6_ADDR_EQUAL(
                    &FrameCache.Ip6Hdr->DestinationAddress,
                    &Rule->Pattern.IpMask.Address.Ipv6)) {
                ASSERT(FrameCache.Ip6Valid);
                Matched = TRUE;
            }
            break;

        default:
            ASSERT(FALSE);
            break;
        }

        if (Matched) {
            //
            // Apply the action.
            //
            switch (Rule->Action) {

            case XDP_PROGRAM_ACTION_REDIRECT:
                if (Rule->Redirect.TargetType == XDP_REDIRECT_TARGET_TYPE_CPU) {
                    UINT32 CpuBase = Rule->Redirect.CpuRedirect.TargetCpuBase;
                    UINT32 CpuCount = Rule->Redirect.CpuRedirect.TargetCpuCount;
                    UINT32 TargetCpu = UINT32_MAX;

                    //
                    // Hash-based mode: compute hash from frame headers for
                    // flow affinity (same 5-tuple always maps to same CPU).
                    // When QUIC CID hashing is enabled and the packet is QUIC,
                    // hash the Dest CID instead for per-connection distribution.
                    //
                    {
                    UINT32 Hash = 0;
                    UINT32 RedirectFlags = Rule->Redirect.CpuRedirect.Flags;
                    BOOLEAN UsedQuicCid = FALSE;

                    //
                    // Ensure frame is parsed for hash computation.
                    //
                    if (!FrameCache.UdpCached) {
                        XdpParseFrame(
                            Frame, FragmentRing, FragmentExtension, FragmentIndex,
                            VirtualAddressExtension, &FrameCache, &Program->FrameStorage);
                    }

                    //
                    // QUIC CID hashing: when the flag is set and the packet is
                    // UDP, try to hash the Dest CID instead of the 5-tuple.
                    // This distributes QUIC connections that share a 5-tuple
                    // across different CPUs based on their connection IDs.
                    //
                    if ((RedirectFlags & XDP_CPU_REDIRECT_FLAG_HASH_QUIC_CID) &&
                        FrameCache.UdpValid) {
                        if (!FrameCache.QuicCached) {
                            if (FrameCache.TransportPayloadValid) {
                                XdpParseQuicHeader(
                                    Frame, FragmentRing, FragmentExtension,
                                    FragmentIndex, VirtualAddressExtension,
                                    &FrameCache.TransportPayload,
                                    &Program->FrameStorage, &FrameCache);
                            }
                        }

                        if (FrameCache.QuicDestCidValid) {
#ifdef XDP_CPUMAP_SMALL_PACKET_BYPASS
                            //
                            // Skip CPUMAP redirect for small packets (likely
                            // QUIC ACKs). ACKs are latency-sensitive for
                            // congestion control and don't benefit from CPU
                            // spreading. Let them process on the RSS CPU
                            // directly to avoid DPC redirect overhead.
                            //
                            UINT16 UdpLen = RtlUshortByteSwap(FrameCache.UdpHdr->uh_ulen);
                            if (UdpLen < 100) {
                                Action = XDP_RX_ACTION_PASS;
                                STAT_INC(RxQueueStats, InspectFramesPassed);
                                goto Done;
                            }
#endif

                            UINT8 CidOffset =
                                (UINT8)XDP_CPU_REDIRECT_QUIC_CID_OFFSET(RedirectFlags);
                            UINT8 CidHashLength =
                                (UINT8)XDP_CPU_REDIRECT_QUIC_CID_LENGTH(RedirectFlags);

                            //
                            // Direct partition index extraction: read the raw
                            // PartitionID bytes from the CID and compute the
                            // target CPU using the same mask+modulo that MsQuic
                            // uses internally. This avoids hash scrambling that
                            // would map to a different CPU than the one encoded
                            // in the CID, preventing unnecessary connection
                            // migrations.
                            //
                            if (CidOffset + CidHashLength <= FrameCache.QuicDestCidLength &&
                                CidHashLength <= 2) {
                                UINT16 PartitionId = 0;
                                RtlCopyMemory(&PartitionId,
                                    FrameCache.QuicDestCid + CidOffset,
                                    CidHashLength);

                                //
                                // Compute partition mask (same algorithm as
                                // MsQuicCalculatePartitionMask): round CpuCount
                                // up to next power-of-2 minus 1.
                                //
                                UINT16 Mask = (UINT16)CpuCount;
                                Mask |= Mask >> 1;
                                Mask |= Mask >> 2;
                                Mask |= Mask >> 4;
                                Mask |= Mask >> 8;

                                TargetCpu = ((PartitionId & Mask) % CpuCount) + CpuBase;
                                UsedQuicCid = TRUE;
                            }

                            if (!UsedQuicCid) {
                                Hash = XdpComputeQuicCidHash(
                                    FrameCache.QuicDestCid, CidOffset,
                                    CidHashLength, FrameCache.QuicDestCidLength);
                                if (Hash != 0) {
                                    UsedQuicCid = TRUE;
                                }
                            }
                        }
                    }

                    if (!UsedQuicCid) {
                    //
                    // Compute improved hash from IP addresses and ports.
                    // Uses symmetric hash to ensure bidirectional flows
                    // map to the same CPU for better cache locality and
                    // more uniform distribution across CPUs.
                    //
                    if (FrameCache.Ip4Valid) {
                        UINT32 SrcIp = *(UINT32 *)&FrameCache.Ip4Hdr->SourceAddress;
                        UINT32 DstIp = *(UINT32 *)&FrameCache.Ip4Hdr->DestinationAddress;

                        //
                        // Symmetric combination: ensures hash(A→B) == hash(B→A).
                        // Using min + rotl(max, 16) provides better distribution
                        // than simple XOR while maintaining symmetry.
                        //
                        UINT32 MinIp = (SrcIp < DstIp) ? SrcIp : DstIp;
                        UINT32 MaxIp = (SrcIp > DstIp) ? SrcIp : DstIp;
                        Hash = MinIp + ((MaxIp << 16) | (MaxIp >> 16));  // Rotate left 16
                    } else if (FrameCache.Ip6Valid) {
                        //
                        // Hash IPv6 symmetrically by combining all address dwords.
                        //
                        UINT32 SrcHash = 0;
                        UINT32 DstHash = 0;

                        for (ULONG i = 0; i < 4; i++) {
                            SrcHash ^= ((UINT32 *)&FrameCache.Ip6Hdr->SourceAddress)[i];
                            DstHash ^= ((UINT32 *)&FrameCache.Ip6Hdr->DestinationAddress)[i];
                        }

                        //
                        // Symmetric combination for IPv6 hash values.
                        //
                        UINT32 MinHash = (SrcHash < DstHash) ? SrcHash : DstHash;
                        UINT32 MaxHash = (SrcHash > DstHash) ? SrcHash : DstHash;
                        Hash = MinHash + ((MaxHash << 16) | (MaxHash >> 16));  // Rotate left 16
                    }

                    if (FrameCache.UdpValid) {
                        UINT16 SrcPort = FrameCache.UdpHdr->uh_sport;
                        UINT16 DstPort = FrameCache.UdpHdr->uh_dport;

                        //
                        // Symmetric port combination: min in low word, max in high word.
                        //
                        UINT16 MinPort = (SrcPort < DstPort) ? SrcPort : DstPort;
                        UINT16 MaxPort = (SrcPort > DstPort) ? SrcPort : DstPort;
                        UINT32 PortHash = (UINT32)MinPort + ((UINT32)MaxPort << 16);

                        //
                        // Mix IP and port hashes using multiplicative constant.
                        // 0x9E3779B9 is the golden ratio, provides good avalanche.
                        //
                        Hash = Hash * 0x9E3779B9 + PortHash;
                    } else if (FrameCache.TcpValid) {
                        UINT16 SrcPort = FrameCache.TcpHdr->th_sport;
                        UINT16 DstPort = FrameCache.TcpHdr->th_dport;

                        //
                        // Symmetric port combination: min in low word, max in high word.
                        //
                        UINT16 MinPort = (SrcPort < DstPort) ? SrcPort : DstPort;
                        UINT16 MaxPort = (SrcPort > DstPort) ? SrcPort : DstPort;
                        UINT32 PortHash = (UINT32)MinPort + ((UINT32)MaxPort << 16);

                        //
                        // Mix IP and port hashes using multiplicative constant.
                        // 0x9E3779B9 is the golden ratio, provides good avalanche.
                        //
                        Hash = Hash * 0x9E3779B9 + PortHash;
                    }
                    } // !UsedQuicCid

                    if (TargetCpu == UINT32_MAX) {
                        //
                        // Finalize hash: spread entropy across all bits so that
                        // the subsequent modulo distributes evenly, especially
                        // for non-power-of-2 CPU counts.  Uses murmur3 32-bit
                        // finalizer constants.
                        //
                        Hash ^= Hash >> 16;
                        Hash *= 0x85ebca6b;
                        Hash ^= Hash >> 13;
                        Hash *= 0xc2b2ae35;
                        Hash ^= Hash >> 16;

                        TargetCpu = (Hash % CpuCount) + CpuBase;
                    }
                    } // end hash computation scope (opened at line 1487)

                    //
                    // Store target CPU in frame extension.
                    //
                    XDP_FRAME_CPU_REDIRECT *CpuRedirect =
                        XdpGetCpuRedirectExtension(Frame, &InspectionContext->CpuRedirectExtension);
                    CpuRedirect->TargetCpu = TargetCpu;
                    CpuRedirect->CpuBase = CpuBase;
                    CpuRedirect->CpuCount = CpuCount;
                    CpuRedirect->RingDepth = Rule->Redirect.CpuRedirect.RingDepth;
                    CpuRedirect->DrainBatchSize = Rule->Redirect.CpuRedirect.DrainBatchSize;
                    CpuRedirect->Flags = Rule->Redirect.CpuRedirect.Flags;

                    Action = XDP_RX_ACTION_CPU_REDIRECT;
                    STAT_INC(RxQueueStats, InspectFramesRedirected);
                } else {
                    //
                    // XSK redirect.
                    //
                    XdpRedirect(
                        &InspectionContext->RedirectContext, FrameIndex, FragmentIndex,
                        Rule->Redirect.TargetType, Rule->Redirect.Target);

                    Action = XDP_RX_ACTION_DROP;
                    STAT_INC(RxQueueStats, InspectFramesRedirected);
                }
                break;

            case XDP_PROGRAM_ACTION_EBPF:
                //
                // Programs containing an eBPF action are expected to use the
                // XdpInspectEbpf routine instead of XdpInspect.
                //
                ASSERT(FALSE);
                __fallthrough;

            case XDP_PROGRAM_ACTION_DROP:
                Action = XDP_RX_ACTION_DROP;
                STAT_INC(RxQueueStats, InspectFramesDropped);
                break;

            case XDP_PROGRAM_ACTION_PASS:
                Action = XDP_RX_ACTION_PASS;
                STAT_INC(RxQueueStats, InspectFramesPassed);
                break;

            case XDP_PROGRAM_ACTION_L2FWD:
                Action =
                    XdpL2Fwd(
                        Frame, FragmentRing, FragmentExtension, FragmentIndex,
                        VirtualAddressExtension, &FrameCache, &Program->FrameStorage, RxQueueStats);
                break;


            default:
                ASSERT(FALSE);
                break;
            }

            goto Done;
        }
    }

    //
    // No match resulted in a terminating action; perform the default action.
    //
    ASSERT(Action == XDP_RX_ACTION_PASS);
    STAT_INC(RxQueueStats, InspectFramesPassed);

Done:

    return Action;
}

//
// Control path routines.
//

VOID
XdpProgramDeleteRule(
    _Inout_ XDP_RULE *Rule
    )
{
    if (Rule->Match == XDP_MATCH_IPV4_UDP_PORT_SET ||
        Rule->Match == XDP_MATCH_IPV6_UDP_PORT_SET ||
        Rule->Match == XDP_MATCH_IPV4_TCP_PORT_SET ||
        Rule->Match == XDP_MATCH_IPV6_TCP_PORT_SET) {
        XdpProgramReleasePortSet(&Rule->Pattern.IpPortSet.PortSet);
    }

    if (Rule->Match == XDP_MATCH_UDP_PORT_SET) {
        XdpProgramReleasePortSet(&Rule->Pattern.PortSet);
    }

    if (Rule->Action == XDP_PROGRAM_ACTION_REDIRECT) {

        switch (Rule->Redirect.TargetType) {

        case XDP_REDIRECT_TARGET_TYPE_XSK:
            if (Rule->Redirect.Target != NULL) {
                XskDereferenceDatapathHandle(Rule->Redirect.Target);
                Rule->Redirect.Target = NULL;
            }
            break;

        case XDP_REDIRECT_TARGET_TYPE_CPU:
            //
            // CPU redirect has no handle to dereference.
            //
            break;

        default:
            ASSERT(FALSE);
        }
    }
}

NTSTATUS
XdpProgramValidateQuicFlow(
    _Out_ XDP_QUIC_FLOW *ValidatedFlow,
    _In_ const XDP_QUIC_FLOW *UserFlow
    )
{
    NTSTATUS Status;
    UINT32 TotalSize;

    RtlZeroMemory(ValidatedFlow, sizeof(*ValidatedFlow));

    Status = RtlUInt32Add(UserFlow->CidOffset, UserFlow->CidLength, &TotalSize);
    if (!NT_SUCCESS(Status)) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    if (TotalSize > RTL_FIELD_SIZE(XDP_QUIC_FLOW, CidData)) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    *ValidatedFlow = *UserFlow;

Exit:

    return Status;
}

NTSTATUS
XdpProgramValidateRule(
    _Out_ XDP_RULE *ValidatedRule,
    _In_ KPROCESSOR_MODE RequestorMode,
    _In_ const XDP_RULE *UserRule,
    _In_ UINT32 RuleCount,
    _In_ UINT32 RuleIndex
    )
{
    NTSTATUS Status;

    //
    // Initialize the trusted kernel rule buffer and increment the count of
    // validated rules. The error path will not attempt to clean up
    // unvalidated rules.
    //
    RtlZeroMemory(ValidatedRule, sizeof(*ValidatedRule));

    if (UserRule->Match < XDP_MATCH_ALL || UserRule->Match > XDP_MATCH_ICMPV6_ECHO_REPLY_IP_DST) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    ValidatedRule->Match = UserRule->Match;

    //
    // Validate each match condition. Many match conditions support all
    // possible input pattern values.
    //
    switch (ValidatedRule->Match) {
    case XDP_MATCH_QUIC_FLOW_SRC_CID:
    case XDP_MATCH_QUIC_FLOW_DST_CID:
    case XDP_MATCH_TCP_QUIC_FLOW_SRC_CID:
    case XDP_MATCH_TCP_QUIC_FLOW_DST_CID:
        Status =
            XdpProgramValidateQuicFlow(
                &ValidatedRule->Pattern.QuicFlow, &UserRule->Pattern.QuicFlow);
        if (!NT_SUCCESS(Status)) {
            goto Exit;
        }
        break;
    case XDP_MATCH_UDP_PORT_SET:
        Status =
            XdpProgramCapturePortSet(
                &UserRule->Pattern.PortSet, RequestorMode, &ValidatedRule->Pattern.PortSet);
        if (!NT_SUCCESS(Status)) {
            goto Exit;
        }
        break;
    case XDP_MATCH_IPV4_UDP_PORT_SET:
    case XDP_MATCH_IPV6_UDP_PORT_SET:
    case XDP_MATCH_IPV4_TCP_PORT_SET:
    case XDP_MATCH_IPV6_TCP_PORT_SET:
        Status =
            XdpProgramCapturePortSet(
                &UserRule->Pattern.IpPortSet.PortSet, RequestorMode,
                &ValidatedRule->Pattern.IpPortSet.PortSet);
        if (!NT_SUCCESS(Status)) {
            goto Exit;
        }
        ValidatedRule->Pattern.IpPortSet.Address = UserRule->Pattern.IpPortSet.Address;
        break;
    default:
        ValidatedRule->Pattern = UserRule->Pattern;
        break;
    }

    if (UserRule->Action < XDP_PROGRAM_ACTION_DROP ||
        UserRule->Action > XDP_PROGRAM_ACTION_EBPF) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    ValidatedRule->Action = UserRule->Action;

    //
    // Capture object handle references in the context of the calling thread.
    // The handle will be validated further on the control path.
    //
    switch (UserRule->Action) {
    case XDP_PROGRAM_ACTION_REDIRECT:
        switch (UserRule->Redirect.TargetType) {

        case XDP_REDIRECT_TARGET_TYPE_XSK:
            ValidatedRule->Redirect.TargetType = XDP_REDIRECT_TARGET_TYPE_XSK;
            Status =
                XskReferenceDatapathHandle(
                    RequestorMode, &UserRule->Redirect.Target, TRUE,
                    &ValidatedRule->Redirect.Target);
            break;

        case XDP_REDIRECT_TARGET_TYPE_CPU:
            {
                UINT32 ActiveCpuCount;
                UINT32 CpuBase;
                UINT32 CpuCount;

                //
                // Validate CPU redirect parameters.
                //
                ActiveCpuCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
                CpuBase = UserRule->Redirect.CpuRedirect.TargetCpuBase;
                CpuCount = UserRule->Redirect.CpuRedirect.TargetCpuCount;

                if (CpuBase >= ActiveCpuCount ||
                    CpuCount == 0 ||
                    CpuBase + CpuCount > ActiveCpuCount) {
                    Status = STATUS_INVALID_PARAMETER;
                    goto Exit;
                }

                {
                    UINT32 CpuFlags = UserRule->Redirect.CpuRedirect.Flags;

                    //
                    // Reject reserved bits (bits 24-31).
                    //
                    if (CpuFlags & XDP_CPU_REDIRECT_RESERVED_MASK) {
                        Status = STATUS_INVALID_PARAMETER;
                        goto Exit;
                    }

                    //
                    // Reject unknown flag combinations (only bit 0 + offset/length fields defined).
                    //
                    if (CpuFlags & ~(XDP_CPU_REDIRECT_FLAG_HASH_QUIC_CID |
                                     XDP_CPU_REDIRECT_QUIC_CID_OFFSET_MASK |
                                     XDP_CPU_REDIRECT_QUIC_CID_LENGTH_MASK)) {
                        Status = STATUS_INVALID_PARAMETER;
                        goto Exit;
                    }

                    //
                    // Validate QUIC CID hash parameters when the flag is set.
                    //
                    if (CpuFlags & XDP_CPU_REDIRECT_FLAG_HASH_QUIC_CID) {
                        UINT8 CidOffset =
                            (UINT8)XDP_CPU_REDIRECT_QUIC_CID_OFFSET(CpuFlags);
                        UINT8 CidHashLength =
                            (UINT8)XDP_CPU_REDIRECT_QUIC_CID_LENGTH(CpuFlags);

                        if (CidHashLength == 0 ||
                            CidOffset >= XDP_QUIC_MAX_CID_LENGTH ||
                            CidOffset + CidHashLength > XDP_QUIC_MAX_CID_LENGTH) {
                            Status = STATUS_INVALID_PARAMETER;
                            goto Exit;
                        }
                    }
                }

                //
                // Copy CPU redirect target type and parameters.
                //
                ValidatedRule->Redirect.TargetType = XDP_REDIRECT_TARGET_TYPE_CPU;
                ValidatedRule->Redirect.CpuRedirect = UserRule->Redirect.CpuRedirect;
                Status = STATUS_SUCCESS;
            }
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (!NT_SUCCESS(Status)) {
            goto Exit;
        }

        break;

    case XDP_PROGRAM_ACTION_EBPF:
        if (RequestorMode != KernelMode) {
            Status = STATUS_INVALID_PARAMETER;
            goto Exit;
        }

        //
        // eBPF programs must be the sole, unconditional action.
        //
        if (RuleCount != 1 || ValidatedRule->Match != XDP_MATCH_ALL) {
            Status = STATUS_INVALID_PARAMETER;
            goto Exit;
        }

        DBG_UNREFERENCED_PARAMETER(RuleIndex);
        ASSERT(RuleIndex == 0);
        ValidatedRule->Ebpf.Target = UserRule->Ebpf.Target;

        break;
    }

    Status = STATUS_SUCCESS;

Exit:

    if (!NT_SUCCESS(Status)) {
        XdpProgramDeleteRule(ValidatedRule);
    }

    return Status;
}
