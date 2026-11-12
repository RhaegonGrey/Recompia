// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_net.cpp: the POSIX side of src/os/compat/winsock2.h and iphlpapi.h.
//
// Only the calls whose Windows and BSD semantics differ are here (see the comment at the top of winsock2.h); the rest
// of the socket API is the BSD one and is called directly.
#include "winsock2.h"
#include "iphlpapi.h"

// The header renames five calls (see its comment); this file implements them and must call the BSD ones.
#undef select
#undef accept
#undef recvfrom
#undef setsockopt
#undef getsockopt
#undef getpeername
#undef getsockname

#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#if !defined(__APPLE__)
#include <sys/ioctl.h>
#endif

#include <algorithm>
#include <string>
#include <vector>

extern "C" {

int WSAStartup(WORD version, LPWSADATA data) {
    (void)version;
    if (data) memset(data, 0, sizeof *data);
    return 0;
}

int WSACleanup(void) { return 0; }

int WSAGetLastError(void) { return errno; }

void WSASetLastError(int err) { errno = err; }

int closesocket(SOCKET s) {
    if (s < 0) {
        errno = EBADF;
        return SOCKET_ERROR;
    }
    return ::close(s);
}

int ioctlsocket(SOCKET s, long cmd, unsigned long* arg) {
    if (cmd != FIONBIO || !arg) {
        errno = EINVAL;
        return SOCKET_ERROR;
    }
    const int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return SOCKET_ERROR;
    const int want = flags & ~O_NONBLOCK;
    return ::fcntl(s, F_SETFL, *arg ? (want | O_NONBLOCK) : want) == 0 ? 0 : SOCKET_ERROR;
}

int WSAIoctl(SOCKET s, DWORD code, void* in, DWORD in_len, void* out, DWORD out_len, DWORD* returned, void* ov,
             void* completion) {
    (void)s;
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_len;
    (void)ov;
    (void)completion;
    if (returned) *returned = 0;
    if (code == SIO_UDP_CONNRESET) {
        // An ICMP port-unreachable does not surface on a BSD socket, so there is no WSAECONNRESET to suppress.
        return 0;
    }
    errno = EINVAL;
    return SOCKET_ERROR;
}

// select(): the callers pass nfds = 0 (Winsock ignores it), so the descriptor set is what decides. poll() takes the
// descriptors themselves, which makes the argument irrelevant on both platforms.
int nxos_select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout) {
    (void)nfds;
    std::vector<pollfd> fds;
    auto add = [&](fd_set* set, short events) {
        if (!set) return;
        for (int fd = 0; fd < FD_SETSIZE; ++fd) {
            if (!FD_ISSET(fd, set)) continue;
            fds.push_back(pollfd{fd, events, 0});
        }
    };
    add(readfds, POLLIN);
    add(writefds, POLLOUT);
    add(exceptfds, POLLPRI);

    int ms = -1;
    if (timeout) ms = static_cast<int>(timeout->tv_sec) * 1000 + static_cast<int>(timeout->tv_usec) / 1000;
    if (fds.empty()) {
        if (ms > 0) ::poll(nullptr, 0, ms);
        return 0;
    }
    const int n = ::poll(fds.data(), fds.size(), ms);
    if (n <= 0) return n;

    // Translate back: clear every set, then set the descriptors poll reported.
    if (readfds) FD_ZERO(readfds);
    if (writefds) FD_ZERO(writefds);
    if (exceptfds) FD_ZERO(exceptfds);
    int count = 0;
    for (const pollfd& p : fds) {
        const short re = p.revents;
        if (!re) continue;
        if (readfds && (re & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(p.fd, readfds);
            ++count;
        }
        if (writefds && (re & POLLOUT)) {
            FD_SET(p.fd, writefds);
            ++count;
        }
        if (exceptfds && (re & POLLPRI)) {
            FD_SET(p.fd, exceptfds);
            ++count;
        }
    }
    return count;
}

SOCKET nxos_accept(SOCKET s, struct sockaddr* addr, int* addrlen) {
    socklen_t len = addrlen ? static_cast<socklen_t>(*addrlen) : 0;
    const int fd = ::accept(s, addr, addrlen ? &len : nullptr);
    if (fd >= 0 && addrlen) *addrlen = static_cast<int>(len);
    return fd;
}

int nxos_recvfrom(SOCKET s, char* buf, int len, int flags, struct sockaddr* from, int* fromlen) {
    socklen_t size = fromlen ? static_cast<socklen_t>(*fromlen) : 0;
    const ssize_t n = ::recvfrom(s, buf, static_cast<size_t>(len), flags, from, fromlen ? &size : nullptr);
    if (n >= 0 && fromlen) *fromlen = static_cast<int>(size);
    return static_cast<int>(n);
}

int nxos_setsockopt(SOCKET s, int level, int name, const char* value, int len) {
    if (level == SOL_SOCKET && (name == SO_SNDTIMEO || name == SO_RCVTIMEO)) {
        // Windows: a DWORD of milliseconds. BSD: a struct timeval. Passing 5000 straight through would set a
        // 5-microsecond timeout, which is the kind of difference that only shows up as a stalled connection.
        if (!value || len < static_cast<int>(sizeof(DWORD))) {
            errno = EINVAL;
            return SOCKET_ERROR;
        }
        DWORD ms = 0;
        memcpy(&ms, value, sizeof ms);
        struct timeval tv{};
        tv.tv_sec = static_cast<time_t>(ms / 1000);
        tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
        return ::setsockopt(s, level, name, &tv, sizeof tv);
    }
    return ::setsockopt(s, level, name, value, static_cast<socklen_t>(len));
}

int nxos_getpeername(SOCKET s, struct sockaddr* addr, int* addrlen) {
    socklen_t len = addrlen ? static_cast<socklen_t>(*addrlen) : 0;
    const int rc = ::getpeername(s, addr, addrlen ? &len : nullptr);
    if (rc == 0 && addrlen) *addrlen = static_cast<int>(len);
    return rc;
}

int nxos_getsockname(SOCKET s, struct sockaddr* addr, int* addrlen) {
    socklen_t len = addrlen ? static_cast<socklen_t>(*addrlen) : 0;
    const int rc = ::getsockname(s, addr, addrlen ? &len : nullptr);
    if (rc == 0 && addrlen) *addrlen = static_cast<int>(len);
    return rc;
}

int nxos_getsockopt(SOCKET s, int level, int name, char* value, int* len) {
    socklen_t size = len ? static_cast<socklen_t>(*len) : 0;
    const int rc = ::getsockopt(s, level, name, value, len ? &size : nullptr);
    if (rc == 0 && len) *len = static_cast<int>(size);
    return rc;
}

// ----------------------------------------------------------------------------------------------------------------
// GetAdaptersAddresses, over getifaddrs.
//
// The buffer layout follows the Windows contract: one contiguous block holding a linked list of
// IP_ADAPTER_ADDRESSES, each with a linked list of IP_ADAPTER_UNICAST_ADDRESS and a NUL-terminated FriendlyName.
// The block is measured first and filled second, so every pointer handed out during the fill stays valid -- a growing
// buffer would move the nodes out from under the list being built.
// ----------------------------------------------------------------------------------------------------------------
namespace {

// One AF_INET datagram socket for the MTU queries (SIOCGIFMTU needs a socket to issue the ioctl on); process lifetime, never
// closed, as the handle Windows' GetAdaptersAddresses keeps internally.
int g_if_mtu_sock() {
    static const int s = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
    return s;
}

struct Adapter {
    std::string name;
    unsigned long if_type = IF_TYPE_ETHERNET_CSMACD;
    unsigned long mtu = 1500;
    struct Unicast {
        struct sockaddr_in addr;
        unsigned long prefix;
    };
    std::vector<Unicast> v4;
};

std::vector<Adapter> collect_adapters() {
    std::vector<Adapter> out;
    struct ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (struct ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const Adapter& a) { return a.name == i->ifa_name; });
        if (it == out.end()) {
            out.push_back(Adapter{});
            it = out.end() - 1;
            it->name = i->ifa_name;
            it->if_type = (i->ifa_flags & IFF_LOOPBACK) ? IF_TYPE_SOFTWARE_LOOPBACK : IF_TYPE_ETHERNET_CSMACD;
#if defined(__APPLE__)
            it->mtu = i->ifa_data ? static_cast<unsigned long>(static_cast<struct if_data*>(i->ifa_data)->ifi_mtu)
                                  : 1500ul;
#else
            // Linux keeps the MTU in the kernel, not in getifaddrs' ifa_data (that is the per-address stats block).
            struct ifreq ifr = {};
            std::snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", i->ifa_name);
            if (::ioctl(g_if_mtu_sock(), SIOCGIFMTU, &ifr) == 0 && ifr.ifr_mtu > 0)
                it->mtu = static_cast<unsigned long>(ifr.ifr_mtu);
#endif
        }
        unsigned long prefix = 0;
        if (i->ifa_netmask) {
            const uint32_t mask = ntohl(reinterpret_cast<const struct sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
            while (prefix < 32 && (mask & (0x80000000u >> prefix))) ++prefix;
        }
        it->v4.push_back(Adapter::Unicast{*reinterpret_cast<const struct sockaddr_in*>(i->ifa_addr), prefix});
    }
    ::freeifaddrs(list);
    return out;
}

// A bump allocator over one block. In measure mode it only advances; in build mode it writes.
struct Builder {
    unsigned char* base = nullptr;
    size_t cap = 0;
    size_t at = 0;

    void* take(size_t n, size_t align) {
        const size_t aligned = (at + align - 1) & ~(align - 1);
        at = aligned + n;
        if (!base || at > cap) return nullptr;
        memset(base + aligned, 0, n);
        return base + aligned;
    }
};

size_t build(unsigned char* base, size_t cap, const std::vector<Adapter>& adapters, unsigned long flags) {
    Builder b{base, cap, 0};
    IP_ADAPTER_ADDRESSES* prev = nullptr;
    for (const Adapter& a : adapters) {
        auto* adapter = static_cast<IP_ADAPTER_ADDRESSES*>(b.take(sizeof(IP_ADAPTER_ADDRESSES), alignof(void*)));
        if (base) {
            if (prev) prev->Next = adapter;
            prev = adapter;
            adapter->IfIndex = static_cast<unsigned long>(::if_nametoindex(a.name.c_str()));
            adapter->IfType = a.if_type;
            adapter->OperStatus = IfOperStatusUp;
            adapter->Mtu = a.mtu;
            adapter->AdapterName = static_cast<char*>(b.take(a.name.size() + 1, 1));
            memcpy(adapter->AdapterName, a.name.c_str(), a.name.size());
        } else {
            (void)b.take(a.name.size() + 1, 1);
        }
        if (!(flags & GAA_FLAG_SKIP_FRIENDLY_NAME)) {
            auto* wide = static_cast<WCHAR*>(b.take((a.name.size() + 1) * sizeof(WCHAR), alignof(WCHAR)));
            if (base) {
                adapter->FriendlyName = wide;
                MultiByteToWideChar(CP_UTF8, 0, a.name.c_str(), static_cast<int>(a.name.size()), wide,
                                    static_cast<int>(a.name.size() + 1));
            }
        }
        IP_ADAPTER_UNICAST_ADDRESS* prev_u = nullptr;
        for (const auto& u : a.v4) {
            auto* uni = static_cast<IP_ADAPTER_UNICAST_ADDRESS*>(
                b.take(sizeof(IP_ADAPTER_UNICAST_ADDRESS), alignof(void*)));
            auto* sin = static_cast<struct sockaddr_in*>(b.take(sizeof(struct sockaddr_in), alignof(struct sockaddr_in)));
            if (!base) continue;
            if (prev_u) prev_u->Next = uni;
            else adapter->FirstUnicastAddress = uni;
            prev_u = uni;
            *sin = u.addr;
            uni->Address.lpSockaddr = reinterpret_cast<struct sockaddr*>(sin);
            uni->Address.iSockaddrLength = static_cast<int>(sizeof(struct sockaddr_in));
            uni->OnLinkPrefixLength = u.prefix;
        }
    }
    return b.at;
}

}  // namespace

ULONG GetAdaptersAddresses(ULONG family, ULONG flags, void* reserved, IP_ADAPTER_ADDRESSES* addresses, ULONG* size) {
    (void)family;
    (void)reserved;
    if (!size) return ERROR_INVALID_PARAMETER;
    const std::vector<Adapter> adapters = collect_adapters();

    const size_t need = build(nullptr, 0, adapters, flags);
    if (!addresses) {
        *size = static_cast<ULONG>(need);
        return ERROR_BUFFER_OVERFLOW;
    }
    if (*size < need) {
        *size = static_cast<ULONG>(need);
        return ERROR_BUFFER_OVERFLOW;
    }
    build(reinterpret_cast<unsigned char*>(addresses), static_cast<size_t>(*size), adapters, flags);
    *size = static_cast<ULONG>(need);
    return NO_ERROR;
}

}  // extern "C"

#endif  // !_WIN32
