// SPDX-License-Identifier: GPL-3.0-or-later
// <winsock2.h> for the POSIX build.
//
// BSD sockets are the same API Winsock wraps, so the calls that only differ in a name (socket, bind, listen, connect,
// send, recv, sendto, shutdown) are the BSD ones. Five calls are not, and each is a real semantic difference rather
// than a rename, so they get a shim:
//
//   select()        Winsock ignores nfds; BSD uses it, and src/net/relay_server.cpp passes 0. nxos_select polls the
//                   descriptor set instead, so the argument means nothing on either platform.
//   setsockopt()    SO_SNDTIMEO/SO_RCVTIMEO take a DWORD of milliseconds on Windows and a struct timeval on BSD.
//                   Passing 5000 straight through would set a 5-microsecond timeout.
//   accept()        the address length is an int* on Windows and a socklen_t* on BSD.
//   recvfrom()      the same.
//   closesocket()   close().
//
// WSAStartup/WSACleanup are reference-counted no-ops, WSAGetLastError is errno, and SIO_UDP_CONNRESET is a no-op that
// succeeds: an ICMP port-unreachable does not surface on a BSD socket, so there is nothing to suppress.
#pragma once

#include "nx_os.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef int SOCKET;
typedef unsigned short USHORT_;
typedef int socklen_t_win;

#define INVALID_SOCKET ((SOCKET)(-1))
#define SOCKET_ERROR (-1)

typedef struct WSAData_ {
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char* lpVendorInfo;
} WSADATA, *LPWSADATA;

#define MAKEWORD(a, b) ((WORD)(((BYTE)((a) & 0xff)) | ((WORD)((BYTE)((b) & 0xff))) << 8))
#define MAKEWORD16(a, b) MAKEWORD(a, b)

// Winsock shutdown() constants.
#define SD_RECEIVE SHUT_RD
#define SD_SEND SHUT_WR
#define SD_BOTH SHUT_RDWR

// No BSD equivalent: the default is already "exclusive", so an option value that no BSD stack knows makes the call
// fail harmlessly and leaves the default in place, which is the behaviour the caller wants.
#define SO_EXCLUSIVEADDRUSE 0x7FFF0001

// WSAIoctl command codes.
#define SIO_UDP_CONNRESET 0x9800000C
#define SIO_RCVALL 0x98000001

#define FIONBIO 0x5421

// Winsock error values are compared against WSAGetLastError(), which is errno here, so they are errno values.
#define WSAEWOULDBLOCK EAGAIN
#define WSAEINPROGRESS EINPROGRESS
#define WSAEALREADY EALREADY
#define WSAETIMEDOUT ETIMEDOUT
#define WSAEMSGSIZE EMSGSIZE
#define WSAEINTR EINTR
#define WSAECONNRESET ECONNRESET
#define WSAECONNREFUSED ECONNREFUSED
#define WSAEADDRINUSE EADDRINUSE
#define WSAEADDRNOTAVAIL EADDRNOTAVAIL
#define WSAEACCES EACCES
#define WSAENETUNREACH ENETUNREACH
#define WSAEHOSTUNREACH EHOSTUNREACH
#define WSAEINVAL EINVAL
#define WSAENOTSOCK EBADF
#define WSAENOBUFS ENOBUFS

int WSAStartup(WORD version, LPWSADATA data);
int WSACleanup(void);
int WSAGetLastError(void);
void WSASetLastError(int err);
int closesocket(SOCKET s);
int ioctlsocket(SOCKET s, long cmd, unsigned long* arg);
int WSAIoctl(SOCKET s, DWORD code, void* in, DWORD in_len, void* out, DWORD out_len, DWORD* returned, void* ov,
             void* completion);

// The five translated calls. The macros make `::select(...)` in the callers reach them too.
int nxos_select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout);
SOCKET nxos_accept(SOCKET s, struct sockaddr* addr, int* addrlen);
int nxos_recvfrom(SOCKET s, char* buf, int len, int flags, struct sockaddr* from, int* fromlen);
int nxos_setsockopt(SOCKET s, int level, int name, const char* value, int len);
int nxos_getsockopt(SOCKET s, int level, int name, char* value, int* len);
int nxos_getpeername(SOCKET s, struct sockaddr* addr, int* addrlen);
int nxos_getsockname(SOCKET s, struct sockaddr* addr, int* addrlen);

#if defined(__cplusplus)
}  // extern "C"
#endif

#define select nxos_select
#define accept nxos_accept
#define recvfrom nxos_recvfrom
#define setsockopt nxos_setsockopt
#define getsockopt nxos_getsockopt
#define getpeername nxos_getpeername
#define getsockname nxos_getsockname
