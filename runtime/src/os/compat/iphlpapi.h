// SPDX-License-Identifier: GPL-3.0-or-later
// <iphlpapi.h> for the POSIX build: GetAdaptersAddresses, over getifaddrs.
//
// src/net/relay_server.cpp lists the machine's IPv4 addresses to answer LAN discovery; on Windows that is
// GetAdaptersAddresses, on BSD getifaddrs. The IP_ADAPTER_ADDRESSES layout below is the part of the Windows one the
// caller reads, filled in from getifaddrs by os_posix_net.cpp.
#pragma once

#include "winsock2.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define NO_ERROR 0

#define GAA_FLAG_SKIP_UNICAST 0x0001
#define GAA_FLAG_SKIP_ANYCAST 0x0002
#define GAA_FLAG_SKIP_MULTICAST 0x0004
#define GAA_FLAG_SKIP_DNS_SERVER 0x0008
#define GAA_FLAG_INCLUDE_PREFIX 0x0010
#define GAA_FLAG_SKIP_FRIENDLY_NAME 0x0020

#define IF_TYPE_OTHER 1
#define IF_TYPE_ETHERNET_CSMACD 6
#define IF_TYPE_SOFTWARE_LOOPBACK 24
#define IF_TYPE_IEEE80211 71

typedef enum {
    IfOperStatusUp = 1,
    IfOperStatusDown = 2,
    IfOperStatusTesting = 3,
    IfOperStatusUnknown = 4,
    IfOperStatusDormant = 5,
    IfOperStatusNotPresent = 6,
    IfOperStatusLowerLayerDown = 7,
} IF_OPER_STATUS;

typedef struct _SOCKET_ADDRESS {
    struct sockaddr* lpSockaddr;
    int iSockaddrLength;
} SOCKET_ADDRESS, *PSOCKET_ADDRESS;

typedef struct _IP_ADAPTER_UNICAST_ADDRESS {
    struct _IP_ADAPTER_UNICAST_ADDRESS* Next;
    SOCKET_ADDRESS Address;
    unsigned long OnLinkPrefixLength;
} IP_ADAPTER_UNICAST_ADDRESS, *PIP_ADAPTER_UNICAST_ADDRESS;

typedef struct _IP_ADAPTER_ADDRESSES {
    struct _IP_ADAPTER_ADDRESSES* Next;
    unsigned long IfIndex;
    char* AdapterName;
    PIP_ADAPTER_UNICAST_ADDRESS FirstUnicastAddress;
    void* FirstAnycastAddress;
    void* FirstMulticastAddress;
    void* FirstDnsServerAddress;
    WCHAR* DnsSuffix;
    WCHAR* Description;
    WCHAR* FriendlyName;
    BYTE PhysicalAddress[8];
    unsigned long PhysicalAddressLength;
    unsigned long Flags;
    unsigned long Mtu;
    unsigned long IfType;
    IF_OPER_STATUS OperStatus;
} IP_ADAPTER_ADDRESSES, *PIP_ADAPTER_ADDRESSES, *LPIP_ADAPTER_ADDRESSES;

ULONG GetAdaptersAddresses(ULONG family, ULONG flags, void* reserved, IP_ADAPTER_ADDRESSES* addresses, ULONG* size);

#if defined(__cplusplus)
}  // extern "C"
#endif
