//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

#ifndef XDPPROGRAM_H
#define XDPPROGRAM_H

#include <in6addr.h>
#include <inaddr.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma warning(push)
#pragma warning(disable:4201) // nonstandard extension used: nameless struct/union

typedef enum _XDP_MATCH_TYPE {
    XDP_MATCH_ALL,
    XDP_MATCH_UDP,
    XDP_MATCH_UDP_DST,
    XDP_MATCH_IPV4_DST_MASK,
    XDP_MATCH_IPV6_DST_MASK,
    XDP_MATCH_QUIC_FLOW_SRC_CID,
    XDP_MATCH_QUIC_FLOW_DST_CID,
    XDP_MATCH_IPV4_UDP_TUPLE,
    XDP_MATCH_IPV6_UDP_TUPLE,
    XDP_MATCH_UDP_PORT_SET,
    XDP_MATCH_IPV4_UDP_PORT_SET,
    XDP_MATCH_IPV6_UDP_PORT_SET,
    XDP_MATCH_IPV4_TCP_PORT_SET,
    XDP_MATCH_IPV6_TCP_PORT_SET,
    XDP_MATCH_TCP_DST,
    XDP_MATCH_TCP_QUIC_FLOW_SRC_CID,
    XDP_MATCH_TCP_QUIC_FLOW_DST_CID,
    XDP_MATCH_TCP_CONTROL_DST,
    XDP_MATCH_IP_NEXT_HEADER,
    XDP_MATCH_INNER_IPV4_DST_MASK_UDP,
    XDP_MATCH_INNER_IPV6_DST_MASK_UDP,
    XDP_MATCH_ICMPV4_ECHO_REPLY_IP_DST,
    XDP_MATCH_ICMPV6_ECHO_REPLY_IP_DST,
} XDP_MATCH_TYPE;

typedef union _XDP_INET_ADDR {
    IN_ADDR Ipv4;
    IN6_ADDR Ipv6;
} XDP_INET_ADDR;

typedef struct _XDP_IP_ADDRESS_MASK {
    XDP_INET_ADDR Mask;
    XDP_INET_ADDR Address;
} XDP_IP_ADDRESS_MASK;

typedef struct _XDP_TUPLE {
    XDP_INET_ADDR SourceAddress;
    XDP_INET_ADDR DestinationAddress;
    UINT16 SourcePort;
    UINT16 DestinationPort;
} XDP_TUPLE;

#define XDP_QUIC_MAX_CID_LENGTH 20

typedef struct _XDP_QUIC_FLOW {
    UINT16 UdpPort;
    UCHAR CidLength;
    UCHAR CidOffset;
    UCHAR CidData[XDP_QUIC_MAX_CID_LENGTH]; // Max allowed per QUIC v1 RFC
} XDP_QUIC_FLOW;

#define XDP_PORT_SET_BUFFER_SIZE ((MAXUINT16 + 1) / 8)

typedef struct _XDP_PORT_SET {
    const UINT8 *PortSet;
    VOID *Reserved;
} XDP_PORT_SET;

typedef struct _XDP_IP_PORT_SET {
    XDP_INET_ADDR Address;
    XDP_PORT_SET PortSet;
} XDP_IP_PORT_SET;

typedef union _XDP_MATCH_PATTERN {
    UINT16 Port;
    XDP_IP_ADDRESS_MASK IpMask;
    XDP_TUPLE Tuple;
    XDP_QUIC_FLOW QuicFlow;
    XDP_PORT_SET PortSet;
    XDP_IP_PORT_SET IpPortSet;
    UINT8 NextHeader;
} XDP_MATCH_PATTERN;

typedef enum _XDP_RULE_ACTION {
    XDP_PROGRAM_ACTION_DROP,
    XDP_PROGRAM_ACTION_PASS,
    XDP_PROGRAM_ACTION_REDIRECT,
    XDP_PROGRAM_ACTION_L2FWD,
    //
    // Reserved.
    //
    XDP_PROGRAM_ACTION_EBPF,
} XDP_RULE_ACTION;

typedef enum _XDP_REDIRECT_TARGET_TYPE {
    XDP_REDIRECT_TARGET_TYPE_XSK,
    XDP_REDIRECT_TARGET_TYPE_CPU,
} XDP_REDIRECT_TARGET_TYPE;

typedef struct _XDP_CPU_REDIRECT_PARAMS {
    UINT32 TargetCpuBase;
    UINT32 TargetCpuCount;
    UINT32 RingDepth;       // 0 = use default (XDP_CPUMAP_RING_DEFAULT_CAPACITY)
    UINT32 DrainBatchSize;  // 0 = use default (256)
    UINT32 Flags;           // See XDP_CPU_REDIRECT_FLAG_* below
} XDP_CPU_REDIRECT_PARAMS;

//
// Flags for XDP_CPU_REDIRECT_PARAMS.Flags
//

//
// When set, hash the QUIC Destination Connection ID instead of the 5-tuple
// for CPU target selection. Non-QUIC UDP packets fall back to 5-tuple hashing.
//
#define XDP_CPU_REDIRECT_FLAG_HASH_QUIC_CID  0x00000001

//
// Bits 8-15: CID byte offset to start hashing (max XDP_QUIC_MAX_CID_LENGTH-1)
// Bits 16-23: Number of CID bytes to hash (must be > 0 when flag set)
// Bits 24-31: Reserved, must be 0
//
#define XDP_CPU_REDIRECT_QUIC_CID_OFFSET_SHIFT   8
#define XDP_CPU_REDIRECT_QUIC_CID_OFFSET_MASK    0x0000FF00
#define XDP_CPU_REDIRECT_QUIC_CID_LENGTH_SHIFT   16
#define XDP_CPU_REDIRECT_QUIC_CID_LENGTH_MASK    0x00FF0000
#define XDP_CPU_REDIRECT_RESERVED_MASK           0xFF000000

#define XDP_CPU_REDIRECT_QUIC_CID_OFFSET(Flags) \
    (((Flags) & XDP_CPU_REDIRECT_QUIC_CID_OFFSET_MASK) >> XDP_CPU_REDIRECT_QUIC_CID_OFFSET_SHIFT)

#define XDP_CPU_REDIRECT_QUIC_CID_LENGTH(Flags) \
    (((Flags) & XDP_CPU_REDIRECT_QUIC_CID_LENGTH_MASK) >> XDP_CPU_REDIRECT_QUIC_CID_LENGTH_SHIFT)

#define XDP_CPU_REDIRECT_MAKE_QUIC_FLAGS(Offset, Length) \
    (XDP_CPU_REDIRECT_FLAG_HASH_QUIC_CID | \
     (((Offset) << XDP_CPU_REDIRECT_QUIC_CID_OFFSET_SHIFT) & XDP_CPU_REDIRECT_QUIC_CID_OFFSET_MASK) | \
     (((Length) << XDP_CPU_REDIRECT_QUIC_CID_LENGTH_SHIFT) & XDP_CPU_REDIRECT_QUIC_CID_LENGTH_MASK))

typedef struct _XDP_REDIRECT_PARAMS {
    XDP_REDIRECT_TARGET_TYPE TargetType;
    union {
        HANDLE Target;
        XDP_CPU_REDIRECT_PARAMS CpuRedirect;
    };
} XDP_REDIRECT_PARAMS;

typedef struct _XDP_EBPF_PARAMS {
    HANDLE Target;
} XDP_EBPF_PARAMS;

typedef struct _XDP_RULE {
    XDP_MATCH_TYPE Match;
    XDP_MATCH_PATTERN Pattern;
    XDP_RULE_ACTION Action;
    union {
        XDP_REDIRECT_PARAMS Redirect;
        XDP_EBPF_PARAMS Ebpf;
    };
} XDP_RULE;

#pragma warning(pop)

#ifdef __cplusplus
} // extern "C"
#endif

#endif
