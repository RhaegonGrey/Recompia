// SPDX-License-Identifier: GPL-3.0-or-later
// relay_server.h: the embedded relay for the v3 protocol (frames in relay_wire.h, which is its only definition; the launcher's relay
// speaks the different PKR1 protocol and does not interoperate), and LAN discovery. Winsock.
#include "relay_server.h"

#include "nxrt/host_device.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

#include "nxrt/log.h"
#include "relay_wire.h"
#include "win_sock.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)   // mstcpip.h, hidden behind some SDK guards
#endif

namespace nxrt::vnet {
namespace relay_detail {

using namespace wire;
using Clock = std::chrono::steady_clock;
using Key = std::array<uint8_t, 16>;

constexpr size_t kMaxRooms = 64, kMaxClients = 32, kSendQueue = 256;
constexpr uint32_t kFramesPerSec = 2000, kUdpPacketsPerSec = 2000, kCodeFailsPerConn = 5, kCodeFailsPerIp = 30;
constexpr auto kCodeFailWindow = std::chrono::minutes(10);
constexpr DWORD kWriteTimeoutMs = 5000;
constexpr Key kNoKey{};
constexpr char kDiscRequest[8] = {'P', 'K', 'P', 'D', 'I', 'S', 'C', '1'};
constexpr char kDiscReply[8] = {'P', 'K', 'P', 'H', 'O', 'S', 'T', '1'};
constexpr size_t kDiscReplyHdr = 20;   // magic 8 + nonce 8 + u16 port + u8 open rooms + u8 nickname length

bool random_bytes(void* p, size_t n) {
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(p), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

constexpr uint32_t vip_of(uint32_t room, uint8_t n) { return 0xA9FE0000u | ((room & 0xFFu) << 8) | n; }

bool ct_eq(const Key& a, const Key& b) {   // keys and passphrase hashes: no early exit
    volatile uint8_t d = 0;
    for (size_t i = 0; i < 16; ++i) d = static_cast<uint8_t>(d | (a[i] ^ b[i]));
    return d == 0;
}

std::vector<uint8_t> error_frame(uint16_t code, const char* msg) {
    Out o(kError);
    o.u16(code).str(msg);
    return o.finish();
}

std::string clean(const std::string& s) {   // nickname for log lines: no control characters
    std::string r;
    for (char ch : s)
        if (static_cast<uint8_t>(ch) >= 0x20 && ch != 0x7F) r.push_back(ch);
    return r;
}

bool send_all(SOCKET s, const std::vector<uint8_t>& f) {
    size_t off = 0;
    while (off < f.size()) {
        const int k = ::send(s, reinterpret_cast<const char*>(f.data() + off), static_cast<int>(f.size() - off), 0);
        if (k <= 0) return false;
        off += static_cast<size_t>(k);
    }
    return true;
}

bool same_addr(const sockaddr_in& a, const sockaddr_in& b) { return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }

std::string addr_str(uint32_t ip, uint16_t port) {
    char b[32];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
    return b;
}

struct Client {
    uint32_t id = 0;
    SOCKET sock = INVALID_SOCKET;
    uint32_t ip = 0;            // the TCP peer, host order
    // under Impl::mu
    std::string nickname;       // empty until HELLO
    uint16_t proto = 0;         // HELLO's (0 before)
    uint32_t room = 0;          // 0: in none
    uint8_t n = 0;              // host octet N of 169.254.R.N (1 = the host)
    bool has_key = false;       // the first room key this connection presented; another later counts as a wrong code
    Key key{};
    uint32_t code_fails = 0;
    uint64_t token = 0;         // UDP token while in a room (0: none)
    bool udp_known = false;     // udp_addr proved the token (a HELLO came from it)
    bool udp_on = false;        // UDP_MODE 1: datagrams to udp_addr
    sockaddr_in udp_addr{};
    Clock::time_point udp_window{};
    uint32_t udp_count = 0;
    // the writer's queue (qmu; taken under Impl::mu, never the other way round)
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<std::vector<uint8_t>> q;
    bool qclosed = false;
};

struct RoomS {
    uint32_t id = 0;
    uint64_t lcid = 0;
    uint16_t scene = 0;
    int16_t ver = 0;
    uint8_t max = 0, sec = 0;
    Key pass{};
    bool keyed = false;          // false: an open room (listed to everyone)
    Key key{};
    std::vector<uint8_t> adv;
    std::map<uint8_t, uint32_t> members;   // host octet N -> client id; N = 1 is the host
    std::set<uint32_t> rejected;           // client ids the host rejected
};

struct Flow {
    bool close = false;
    std::string why;
};

enum class Code { Ok, Wrong, Limit };

}  // namespace relay_detail

using namespace relay_detail;

struct RelayServer::Impl {
    RelayServerOptions opt;
    SOCKET listener = INVALID_SOCKET, usock = INVALID_SOCKET;
    uint16_t port = 0;
    std::atomic<bool> stopping{false};
    uint32_t next_id = 1;   // accept thread only

    std::mutex mu;   // everything below up to the threads
    std::map<uint32_t, std::shared_ptr<Client>> clients;
    std::map<uint32_t, RoomS> rooms;
    std::map<uint64_t, uint32_t> tokens;                                    // UDP token -> client id
    std::map<uint32_t, std::pair<uint32_t, Clock::time_point>> ip_fails;   // wrong room codes per address: count, window start

    std::thread accept_t, udp_t;
    struct Conn {
        std::thread t;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex conns_mu;
    std::vector<Conn> conns;

    // ---- sends (caller holds mu) -------------------------------------------------------------------------------------------------------
    static void queue(Client& c, std::vector<uint8_t> f) {
        std::lock_guard<std::mutex> q(c.qmu);
        if (c.qclosed) return;
        if (c.q.size() >= kSendQueue) {   // too slow: dropped (its reader sees the close and cleans up); nobody else waits on it
            ::shutdown(c.sock, SD_BOTH);
            return;
        }
        c.q.push_back(std::move(f));
        c.qcv.notify_one();
    }
    Client* client(uint32_t id) {
        const auto it = clients.find(id);
        return it == clients.end() ? nullptr : it->second.get();
    }

    // ---- room codes (caller holds mu) --------------------------------------------------------------------------------------------------
    bool ip_blocked(uint32_t ip) {
        const auto it = ip_fails.find(ip);
        if (it == ip_fails.end()) return false;
        if (Clock::now() - it->second.second >= kCodeFailWindow) {
            ip_fails.erase(it);
            return false;
        }
        return it->second.first >= kCodeFailsPerIp;
    }
    void count_ip_fail(uint32_t ip) {
        const auto now = Clock::now();
        for (auto it = ip_fails.begin(); it != ip_fails.end();) it = now - it->second.second >= kCodeFailWindow ? ip_fails.erase(it) : std::next(it);
        auto it = ip_fails.find(ip);
        if (it == ip_fails.end()) it = ip_fails.emplace(ip, std::make_pair(0u, now)).first;
        ++it->second.first;
    }
    Code code_failed(Client& c) {
        ++c.code_fails;
        count_ip_fail(c.ip);
        return c.code_fails >= kCodeFailsPerConn || ip_blocked(c.ip) ? Code::Limit : Code::Wrong;
    }
    Code present_key(Client& c, const Key& key) {
        if (ct_eq(key, kNoKey)) return Code::Ok;
        if (ip_blocked(c.ip)) return Code::Limit;
        if (!c.has_key) {
            c.has_key = true;
            c.key = key;
            return Code::Ok;
        }
        return ct_eq(c.key, key) ? Code::Ok : code_failed(c);
    }
    static Flow limit(Client& c) {
        queue(c, error_frame(kLobbyRate, "Too many wrong room codes. Try again in a few minutes."));
        return {true, "too many wrong room codes"};
    }
    // Some(flow) when the key a request carries is refused (the ERROR is queued already).
    std::optional<Flow> key_refused(Client& c, const Key& key) {
        switch (present_key(c, key)) {
            case Code::Ok: return std::nullopt;
            case Code::Limit: return limit(c);
            case Code::Wrong: break;
        }
        queue(c, error_frame(kLobbyRoomCode, "Wrong room code."));
        return Flow{};
    }

    // ---- UDP path (caller holds mu) ------------------------------------------------------------------------------------------------------
    // A v3 client entered a room: a fresh random token, sent as UDP_OFFER right behind JOINED. No UDP socket, a v2 client or no random
    // bytes: none, the client stays on TCP.
    void udp_grant(Client& c) {
        if (usock == INVALID_SOCKET || c.proto < 3) return;
        uint64_t t = 0;
        if (!random_bytes(&t, sizeof t) || t == 0 || tokens.count(t)) return;
        udp_revoke(c);
        tokens[t] = c.id;
        c.token = t;
        Out o(kUdpOffer);
        o.u64(t).u16(port);
        queue(c, o.finish());
    }
    void udp_revoke(Client& c) {
        const uint64_t t = std::exchange(c.token, 0);
        c.udp_known = c.udp_on = false;
        if (t) tokens.erase(t);
    }

    // One game datagram to c: over UDP when it proved an endpoint and asked for it, else a TCP frame. A failed UDP send is a lost datagram.
    void send_datagram(Client& c, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t* d, size_t n) {
        if (c.udp_on && c.udp_known && usock != INVALID_SOCKET) {
            std::vector<uint8_t> b;
            b.reserve(kUSDataHdr + n);
            b.push_back(kUSData);
            put_le(b, c.token, 8);
            put_le(b, src_ip, 4);
            put_le(b, src_port, 2);
            put_le(b, dst_ip, 4);
            put_le(b, dst_port, 2);
            b.insert(b.end(), d, d + n);
            ::sendto(usock, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), 0, reinterpret_cast<const sockaddr*>(&c.udp_addr),
                     sizeof c.udp_addr);
            return;
        }
        Out o(kSDatagram);
        o.u32(src_ip).u16(src_port).u32(dst_ip).u16(dst_port).u16(static_cast<uint16_t>(n)).raw(d, n);
        queue(c, o.finish());
    }

    // A datagram from member n of room (either transport): src_ip is the sender's vip; .255 to every other member, a member's vip to
    // that member; anything else dropped.
    void route(uint32_t room, uint8_t n, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t* d, size_t len) {
        const auto it = rooms.find(room);
        if (it == rooms.end()) return;
        const uint32_t src_ip = vip_of(room, n);
        if (dst_ip == vip_of(room, 255)) {
            for (const auto& [m, id] : it->second.members)
                if (m != n)
                    if (Client* o = client(id)) send_datagram(*o, src_ip, src_port, dst_ip, dst_port, d, len);
        } else if ((dst_ip & ~0xFFu) == vip_of(room, 0) && static_cast<uint8_t>(dst_ip & 0xFF) != n) {
            const auto m = it->second.members.find(static_cast<uint8_t>(dst_ip & 0xFF));
            if (m != it->second.members.end())
                if (Client* o = client(m->second)) send_datagram(*o, src_ip, src_port, dst_ip, dst_port, d, len);
        }
    }

    // ---- rooms (caller holds mu) ---------------------------------------------------------------------------------------------------------
    Room info(const RoomS& r) {
        Room i;
        i.room_id = r.id;
        i.local_comm_id = r.lcid;
        i.scene_id = r.scene;
        i.comm_version = r.ver;
        i.node_count_max = r.max;
        i.security_mode = r.sec;
        for (const auto& [n, id] : r.members) {
            const Client* c = client(id);
            i.nodes.push_back({static_cast<uint8_t>(n - 1), vip_of(r.id, n), {0x02, 0x00, 0xA9, 0xFE, static_cast<uint8_t>(r.id), n},
                               c ? c->nickname : std::string()});
        }
        i.adv = r.adv;
        return i;
    }
    void push_room_state(uint32_t room) {
        const auto it = rooms.find(room);
        if (it == rooms.end()) return;
        Out o(kRoomState);
        write_room(o, info(it->second));
        const std::vector<uint8_t> f = o.finish();
        for (const auto& [n, id] : it->second.members)
            if (Client* c = client(id)) queue(*c, f);
    }
    // Takes c out of its room; notify: LEFT / ROOM_STATE to the others (false while the server stops). The host leaving closes the room
    // (LEFT 1 to every guest). Returns a log line when a room closed.
    std::string leave_room(Client& c, bool notify) {
        if (!c.room) return {};
        const uint32_t room = std::exchange(c.room, 0);
        const uint8_t n = std::exchange(c.n, uint8_t{0});
        udp_revoke(c);
        const auto it = rooms.find(room);
        if (it == rooms.end()) return {};
        if (n == 1) {
            const RoomS r = std::move(it->second);
            rooms.erase(it);
            for (const auto& [m, id] : r.members) {
                Client* o = m == 1 ? nullptr : client(id);
                if (!o) continue;
                o->room = 0;
                o->n = 0;
                udp_revoke(*o);
                if (notify) {
                    Out f(kLeft);
                    f.u16(static_cast<uint16_t>(LeftReason::HostClosed));
                    queue(*o, f.finish());
                }
            }
            return "room " + std::to_string(room) + " closed";
        }
        it->second.members.erase(n);
        if (notify) push_room_state(room);
        return {};
    }

    // ---- frames (reader thread) ----------------------------------------------------------------------------------------------------------
    Flow bad(Client& c, uint8_t type) {
        std::lock_guard<std::mutex> lk(mu);
        queue(c, error_frame(kLobbyBadFrame, "Malformed message."));
        char why[40];
        std::snprintf(why, sizeof why, "bad frame (type 0x%02x)", type);
        return {true, why};
    }

    Flow hello(Client& c, uint8_t type, const uint8_t* p, size_t n, bool* done) {
        if (type != kHello) {
            std::lock_guard<std::mutex> lk(mu);
            queue(c, error_frame(kLobbyBadFrame, "HELLO must come first."));
            return {true, "no hello"};
        }
        In in(p, n);
        const uint16_t proto = in.u16();
        const std::string build = in.str();
        std::string nick = in.str();
        const std::string pass = in.str();
        if (!in.done() || nick.size() > kMaxNick) return bad(c, type);
        const char* refuse = nullptr;
        uint16_t code = 0;
        if (proto < kRelayMinProto) code = kLobbyBadProto, refuse = "This server needs a newer game (relay protocol 2, room codes). Update the game.";
        else if (proto > kRelayProto) code = kLobbyBadProto, refuse = "This server is older than the game (relay protocol 3). Update the game that hosts.";
        else if (!opt.password.empty() && pass != opt.password) code = kLobbyPassword, refuse = "Wrong server password.";
        std::lock_guard<std::mutex> lk(mu);
        if (refuse) {
            queue(c, error_frame(code, refuse));
            return {true, code == kLobbyPassword ? "wrong password" : "bad proto " + std::to_string(proto)};
        }
        nick = clean(nick);
        c.nickname = nick.empty() ? "player" + std::to_string(c.id) : nick;
        c.proto = proto;
        Out o(kWelcome);
        o.u32(c.id).str(opt.server_version);
        queue(c, o.finish());
        *done = true;
        NXLOG_I("net", "relay: client %u is %s", c.id, c.nickname.c_str());
        return {};
    }

    Flow handle(Client& c, uint8_t type, const uint8_t* p, size_t n) {
        In in(p, n);
        switch (type) {
            case kHello: {
                std::lock_guard<std::mutex> lk(mu);
                queue(c, error_frame(kLobbyBadFrame, "HELLO was already sent."));
                return {true, "second hello"};
            }
            case kPing: {
                const uint32_t nonce = in.u32();
                if (!in.done()) return bad(c, type);
                Out o(kPong);
                o.u32(nonce);
                std::lock_guard<std::mutex> lk(mu);
                queue(c, o.finish());
                return {};
            }
            case kUdpMode: {
                const uint8_t on = in.u8();
                if (!in.done() || on > 1) return bad(c, type);
                bool changed, now_on;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    if (c.proto < 3) {
                        queue(c, error_frame(kLobbyBadFrame, "Malformed message."));
                        return {true, "UDP_MODE from a v2 client"};
                    }
                    now_on = on && c.udp_known;   // only with a proven endpoint
                    changed = now_on != c.udp_on;
                    c.udp_on = now_on;
                }
                if (changed) NXLOG_I("net", "relay: client %u datagrams over %s", c.id, now_on ? "UDP" : "TCP");
                return {};
            }
            case kScan: {
                const uint64_t lcid = in.u64();
                const uint16_t scene = in.u16();
                const Key key = in.b16();
                if (!in.done()) return bad(c, type);
                std::lock_guard<std::mutex> lk(mu);
                if (auto f = key_refused(c, key)) return *f;
                // open rooms and the rooms with the key presented; not full; the filter. Cut so the frame stays <= 8192 B.
                Out o(kRoomList);
                o.u8(0);
                uint8_t count = 0;
                for (const auto& [id, r] : rooms) {
                    if (r.keyed && !ct_eq(r.key, key)) continue;
                    if (r.members.size() >= r.max) continue;
                    if (lcid && r.lcid != lcid) continue;
                    if (scene != 0xFFFF && r.scene != scene) continue;
                    if (count == 255) break;
                    Out one(0);
                    write_room(one, info(r));
                    if (o.b.size() - 4 + one.b.size() - 5 > kMaxFrame) break;
                    o.raw(one.b.data() + 5, one.b.size() - 5);
                    ++count;
                }
                o.b[5] = count;
                queue(c, o.finish());
                return {};
            }
            case kCreate: {
                RoomS r;
                r.lcid = in.u64();
                r.scene = in.u16();
                r.ver = static_cast<int16_t>(in.u16());
                r.max = in.u8();
                r.sec = in.u8();
                r.pass = in.b16();
                const Key key = in.b16();
                r.adv = in.blob();
                if (!in.done() || r.max < 1 || r.max > kMaxNodes || r.adv.size() > kMaxAdvertise) return bad(c, type);
                uint32_t id = 0;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    if (auto f = key_refused(c, key)) return *f;
                    if (c.room) {
                        queue(c, error_frame(kLobbyBadFrame, "Already in a room."));
                        return {};
                    }
                    if (rooms.size() < kMaxRooms)
                        for (uint32_t i = 1; i <= 254 && !id; ++i)
                            if (!rooms.count(i)) id = i;
                    if (!id) {
                        queue(c, error_frame(kLobbyRoomFull, "The server has no free room."));
                        return {};
                    }
                    r.id = id;
                    r.keyed = !ct_eq(key, kNoKey);
                    r.key = key;
                    r.members[1] = c.id;
                    rooms[id] = std::move(r);
                    c.room = id;
                    c.n = 1;
                    Out o(kJoined);
                    o.u32(id).u32(vip_of(id, 1)).u8(0);
                    queue(c, o.finish());
                    udp_grant(c);
                    push_room_state(id);
                }
                NXLOG_I("net", "relay: room %u created by client %u%s", id, c.id, ct_eq(key, kNoKey) ? "" : " (room code)");
                return {};
            }
            case kJoin: {
                const uint32_t room = in.u32();
                const int16_t ver = static_cast<int16_t>(in.u16());
                const Key pass = in.b16();
                const Key key = in.b16();
                if (!in.done()) return bad(c, type);
                std::lock_guard<std::mutex> lk(mu);
                if (auto f = key_refused(c, key)) return *f;
                if (c.room) {
                    queue(c, error_frame(kLobbyBadFrame, "Already in a room."));
                    return {};
                }
                const auto it = rooms.find(room);
                // a room with a code: the code is checked before anything else about the room is told
                if (it != rooms.end() && it->second.keyed && !ct_eq(it->second.key, key)) {
                    if (code_failed(c) == Code::Limit) return limit(c);
                    queue(c, error_frame(kLobbyRoomCode, "Wrong room code."));
                    return {};
                }
                const char* err = nullptr;
                uint16_t code = 0;
                uint8_t node = 2;
                if (it == rooms.end()) code = kLobbyNoRoom, err = "That room does not exist any more.";
                else if (it->second.rejected.count(c.id)) code = kLobbyRejected, err = "The host turned you away from this room.";
                else if (it->second.members.size() >= it->second.max) code = kLobbyRoomFull, err = "That room is full.";
                else if (it->second.ver != ver) code = kLobbyVersion, err = "That room runs another game version.";
                else if (!ct_eq(it->second.pass, pass)) code = kLobbyPassword, err = "Wrong room passphrase.";
                else {
                    while (node <= kMaxNodes && it->second.members.count(node)) ++node;
                    if (node > kMaxNodes) code = kLobbyRoomFull, err = "That room is full.";
                }
                if (err) {
                    queue(c, error_frame(code, err));
                    return {};
                }
                it->second.members[node] = c.id;
                c.room = room;
                c.n = node;
                Out o(kJoined);
                o.u32(room).u32(vip_of(room, node)).u8(static_cast<uint8_t>(node - 1));
                queue(c, o.finish());
                udp_grant(c);
                push_room_state(room);
                return {};
            }
            case kLeave: {
                if (!in.done()) return bad(c, type);
                std::string line;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    if (!c.room) return {};
                    line = leave_room(c, true);
                    Out o(kLeft);
                    o.u16(static_cast<uint16_t>(LeftReason::YouLeft));
                    queue(c, o.finish());
                }
                if (!line.empty()) NXLOG_I("net", "relay: %s", line.c_str());
                return {};
            }
            case kAdvertise: {
                std::vector<uint8_t> adv = in.blob();
                if (!in.done() || adv.size() > kMaxAdvertise) return bad(c, type);
                std::lock_guard<std::mutex> lk(mu);
                const auto it = c.n == 1 ? rooms.find(c.room) : rooms.end();
                if (it == rooms.end()) {
                    queue(c, error_frame(kLobbyNotHost, "Only the host can do that."));
                    return {};
                }
                it->second.adv = std::move(adv);
                push_room_state(c.room);
                return {};
            }
            case kReject: {
                const uint32_t target = in.u32();
                if (!in.done()) return bad(c, type);
                std::lock_guard<std::mutex> lk(mu);
                const auto it = c.n == 1 ? rooms.find(c.room) : rooms.end();
                if (it == rooms.end()) {
                    queue(c, error_frame(kLobbyNotHost, "Only the host can do that."));
                    return {};
                }
                const uint8_t m = static_cast<uint8_t>(target & 0xFF);
                if ((target & ~0xFFu) != vip_of(c.room, 0) || m == 1) return {};   // unknown vip / the host's own: ignored
                const auto mem = it->second.members.find(m);
                Client* o = mem == it->second.members.end() ? nullptr : client(mem->second);
                if (!o) return {};
                const uint32_t room = c.room;
                leave_room(*o, true);
                if (const auto r = rooms.find(room); r != rooms.end()) r->second.rejected.insert(o->id);
                Out f(kLeft);
                f.u16(static_cast<uint16_t>(LeftReason::Rejected));
                queue(*o, f.finish());
                return {};
            }
            case kDatagram: {
                const uint16_t src_port = in.u16();
                const uint32_t dst_ip = in.u32();
                const uint16_t dst_port = in.u16();
                const size_t len = in.u16();
                if (!in.need(len) || in.o + len != in.n || len > kMaxData) return bad(c, type);
                std::lock_guard<std::mutex> lk(mu);
                if (c.room) route(c.room, c.n, src_port, dst_ip, dst_port, in.p + in.o, len);
                return {};
            }
            default: return bad(c, type);
        }
    }

    // ---- threads -----------------------------------------------------------------------------------------------------------------------
    // Writes queued frames until the queue is closed and empty (the client was removed) or a write fails (then the socket is shut so the
    // reader ends too).
    static void write_loop(Client& c) {
        for (;;) {
            std::vector<uint8_t> f;
            {
                std::unique_lock<std::mutex> lk(c.qmu);
                c.qcv.wait(lk, [&] { return c.qclosed || !c.q.empty(); });
                if (c.q.empty()) break;
                f = std::move(c.q.front());
                c.q.pop_front();
            }
            if (!send_all(c.sock, f)) {
                ::shutdown(c.sock, SD_BOTH);
                return;
            }
        }
        ::shutdown(c.sock, SD_SEND);
    }

    void serve(const std::shared_ptr<Client>& cp) {
        Client& c = *cp;
        std::thread writer([cp] { write_loop(*cp); });
        std::vector<uint8_t> buf, chunk(16 * 1024);
        buf.reserve(2 * kMaxFrame);
        const auto keepalive = std::chrono::milliseconds(opt.keepalive_ms);
        auto last = Clock::now(), window = last;
        uint32_t frames = 0;
        bool hello_done = false;
        std::string why;
        for (;;) {
            if (stopping.load()) {
                why = "server stopped";
                break;
            }
            // every complete frame in the buffer
            Flow f;
            size_t used = 0;
            while (buf.size() - used >= 4) {
                const uint32_t len = static_cast<uint32_t>(get_le(buf.data() + used, 4));
                if (len == 0 || len > kMaxFrame) {
                    std::lock_guard<std::mutex> lk(mu);
                    queue(c, error_frame(kLobbyBadFrame, "Malformed message."));
                    f = {true, "bad frame header"};
                    break;
                }
                if (buf.size() - used < 4 + size_t{len}) break;
                const uint8_t type = buf[used + 4];
                const uint8_t* p = buf.data() + used + 5;
                used += 4 + size_t{len};
                last = Clock::now();
                if (last - window >= std::chrono::seconds(1)) window = last, frames = 0;
                if (++frames > kFramesPerSec) {
                    std::lock_guard<std::mutex> lk(mu);
                    queue(c, error_frame(kLobbyRate, "Too many messages; disconnected."));
                    f = {true, "rate limit"};
                    break;
                }
                f = hello_done ? handle(c, type, p, len - 1) : hello(c, type, p, len - 1, &hello_done);
                if (f.close) break;
            }
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(used));
            if (f.close) {
                why = f.why;
                break;
            }
            if (Clock::now() - last >= keepalive) {
                why = "keepalive timeout";
                break;
            }
            fd_set r;
            FD_ZERO(&r);
            FD_SET(c.sock, &r);
            timeval tv{0, 250000};
            const int k = ::select(0, &r, nullptr, nullptr, &tv);
            if (k < 0) {
                why = "connection lost";
                break;
            }
            if (k == 0) continue;
            const int got = ::recv(c.sock, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
            if (got == 0) {
                why = "closed";
                break;
            }
            if (got < 0) {
                const int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAETIMEDOUT) continue;
                why = "connection lost";
                break;
            }
            buf.insert(buf.end(), chunk.data(), chunk.data() + got);
        }
        std::string closed;
        {
            std::lock_guard<std::mutex> lk(mu);
            closed = leave_room(c, !stopping.load());
            udp_revoke(c);
            clients.erase(c.id);
        }
        if (!closed.empty()) NXLOG_I("net", "relay: %s", closed.c_str());
        NXLOG_I("net", "relay: client %u (%s) disconnected: %s", c.id, c.nickname.empty() ? "no hello" : c.nickname.c_str(), why.c_str());
        {
            // the client is gone from the table, so nothing queues any more: the writer drains what is queued, sends FIN and ends
            std::lock_guard<std::mutex> q(c.qmu);
            c.qclosed = true;
            c.qcv.notify_all();
        }
        writer.join();
        // lingering close: read off what the peer still sends (briefly), else closing with unread input sends a RST that can destroy the
        // final ERROR before the peer reads it
        const auto end = Clock::now() + std::chrono::milliseconds(500);
        while (Clock::now() < end) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(c.sock, &r);
            timeval tv{0, 50000};
            const int k = ::select(0, &r, nullptr, nullptr, &tv);
            if (k < 0) break;
            if (k == 0) continue;
            if (::recv(c.sock, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0) <= 0) break;
        }
        ::closesocket(c.sock);
    }

    void reap() {   // joins the client threads that ended
        std::lock_guard<std::mutex> lk(conns_mu);
        for (auto it = conns.begin(); it != conns.end();) {
            if (it->done->load()) {
                it->t.join();
                it = conns.erase(it);
            } else {
                ++it;
            }
        }
    }

    void admit(SOCKET s, uint32_t ip, uint16_t peer_port) {
        const BOOL one = TRUE;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        const DWORD sto = kWriteTimeoutMs;
        ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sto), sizeof sto);
        auto c = std::make_shared<Client>();
        c->sock = s;
        c->ip = ip;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (clients.size() < kMaxClients) {
                c->id = next_id++;
                clients[c->id] = c;
            }
        }
        if (!c->id) {
            NXLOG_I("net", "relay: refused %s: server full", addr_str(ip, peer_port).c_str());
            send_all(s, error_frame(kLobbyRoomFull, "The server is full."));
            ::shutdown(s, SD_SEND);
            ::closesocket(s);
            return;
        }
        NXLOG_I("net", "relay: client %u connected from %s", c->id, addr_str(ip, peer_port).c_str());
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lk(conns_mu);
        conns.push_back({std::thread([this, c, done] {
                             serve(c);
                             done->store(true);
                         }),
                         done});
    }

    void accept_loop() {
        while (!stopping.load()) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(listener, &r);
            timeval tv{0, 100000};
            if (::select(0, &r, nullptr, nullptr, &tv) > 0) {
                sockaddr_in peer{};
                int len = sizeof peer;
                const SOCKET s = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &len);
                if (s != INVALID_SOCKET) admit(s, ntohl(peer.sin_addr.s_addr), ntohs(peer.sin_port));
            }
            reap();
        }
    }

    // ---- UDP socket: the datagram path and discovery ----------------------------------------------------------------------------------
    // Every packet is checked before anything is sent: malformed, an unknown token, another IP than the token's TCP connection, or over
    // the token's rate: dropped without an answer. The answers (ACK to a valid HELLO, a discovery reply) are never longer than what came in.
    void udp_loop() {
        std::vector<uint8_t> buf(kUMaxPacket + 1);
        while (!stopping.load()) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(usock, &r);
            timeval tv{0, 100000};
            if (::select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
            sockaddr_in from{};
            int fl = sizeof from;
            const int k = ::recvfrom(usock, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (k <= 0 || from.sin_family != AF_INET) continue;   // ICMP resets, an over-long packet (WSAEMSGSIZE): keep reading
            udp_packet(buf.data(), static_cast<size_t>(k), from);
        }
    }

    void udp_packet(const uint8_t* b, size_t n, const sockaddr_in& from) {
        const uint32_t ip = ntohl(from.sin_addr.s_addr);
        if (n >= sizeof kDiscRequest && std::memcmp(b, kDiscRequest, sizeof kDiscRequest) == 0) {
            discovery(b, n, from, ip);
            return;
        }
        if (n < kUHdr || n > kUMaxPacket) return;
        const uint64_t token = get_le(b + 1, 8);
        const bool is_hello = b[0] == kUHello && n == kUHdr;
        const bool is_data = b[0] == kUData && n >= kUDataHdr && n - kUDataHdr <= kMaxData;
        if (!is_hello && !is_data) return;
        uint32_t id = 0;
        bool log_ep = false, moved = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            const auto t = tokens.find(token);
            Client* c = t == tokens.end() ? nullptr : client(t->second);
            if (!c || c->ip != ip) return;
            const auto now = Clock::now();
            if (now - c->udp_window >= std::chrono::seconds(1)) c->udp_window = now, c->udp_count = 0;
            if (++c->udp_count > kUdpPacketsPerSec) return;
            if (is_hello) {
                // the endpoint is wherever the latest HELLO came from (a NAT may move it); ACK = the HELLO's size
                const bool same = c->udp_known && same_addr(c->udp_addr, from);
                log_ep = !same;
                moved = c->udp_known && !same;
                c->udp_addr = from;
                c->udp_known = true;
                id = c->id;
                uint8_t ack[kUHdr];
                ack[0] = kUAck;
                std::memcpy(ack + 1, b + 1, 8);
                ::sendto(usock, reinterpret_cast<const char*>(ack), sizeof ack, 0, reinterpret_cast<const sockaddr*>(&from), sizeof from);
            } else if (c->udp_known && same_addr(c->udp_addr, from) && c->room) {
                route(c->room, c->n, static_cast<uint16_t>(get_le(b + 9, 2)), static_cast<uint32_t>(get_le(b + 11, 4)),
                      static_cast<uint16_t>(get_le(b + 15, 2)), b + kUDataHdr, n - kUDataHdr);
            }
        }
        if (log_ep) NXLOG_I("net", "relay: client %u UDP endpoint %s%s", id, addr_str(ip, ntohs(from.sin_port)).c_str(), moved ? " (moved)" : "");
    }

    // relay_server.h "LAN discovery". Only from a LAN source (the relay may be internet-facing); the reply is never longer than the request.
    void discovery(const uint8_t* b, size_t n, const sockaddr_in& from, uint32_t ip) {
        if (n != kDiscoveryLen || !is_lan_ipv4(ip)) return;
        size_t open = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (const auto& [id, r] : rooms)
                if (!r.keyed && r.members.size() < r.max) ++open;
        }
        std::vector<uint8_t> rep(kDiscReply, kDiscReply + sizeof kDiscReply);
        rep.insert(rep.end(), b + 8, b + 16);
        put_le(rep, port, 2);
        rep.push_back(static_cast<uint8_t>(open < 255 ? open : 255));
        const std::string& nick = opt.nickname;
        size_t k = nick.size();
        if (k > kMaxNick) k = kMaxNick;
        if (k > n - kDiscReplyHdr) k = n - kDiscReplyHdr;
        k = text::utf8_cut(nick, k);   // not inside a UTF-8 sequence
        rep.push_back(static_cast<uint8_t>(k));
        rep.insert(rep.end(), nick.begin(), nick.begin() + static_cast<std::ptrdiff_t>(k));
        ::sendto(usock, reinterpret_cast<const char*>(rep.data()), static_cast<int>(rep.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof from);
    }
};

// =============================================================================================================================================
// RelayServer
// =============================================================================================================================================

RelayServer::RelayServer(std::unique_ptr<Impl> im) : im_(std::move(im)) {}
RelayServer::~RelayServer() { stop(); }
uint16_t RelayServer::port() const { return im_->port; }
bool RelayServer::udp() const { return im_->usock != INVALID_SOCKET; }

std::unique_ptr<RelayServer> RelayServer::start(const RelayServerOptions& opt, std::string* why, bool* in_use) {
    if (in_use) *in_use = false;
    if (!ensure_winsock()) {
        if (why) *why = "Winsock unavailable";
        return nullptr;
    }
    // No SO_EXCLUSIVEADDRUSE: it fails the bind while connections of an earlier run sit in TIME_WAIT (a host restarting the game within a
    // minute could not host); Windows' default already refuses a second bind over a live socket.
    // Linux sockets (natively and under Wine): a listener bind fails on TIME_WAIT without SO_REUSEADDR, which there still refuses a live listener.
    auto bind_any = [&](SOCKET s, bool listener) {
        if (listener && host_is_linux()) {
            const BOOL on = TRUE;
            ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
        }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(opt.port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        return ::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0 ? 0 : WSAGetLastError();
    };
    const SOCKET l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) {
        if (why) *why = "no socket";
        return nullptr;
    }
    if (const int e = bind_any(l, true)) {
        ::closesocket(l);
        const bool taken = e == WSAEADDRINUSE || e == WSAEACCES;
        if (in_use) *in_use = taken;
        if (why) *why = taken ? "port " + std::to_string(opt.port) + " is in use" : "cannot open port " + std::to_string(opt.port) + " (error " + std::to_string(e) + ")";
        return nullptr;
    }
    if (::listen(l, SOMAXCONN) != 0) {
        const int e = WSAGetLastError();
        ::closesocket(l);
        if (why) *why = "cannot listen on port " + std::to_string(opt.port) + " (error " + std::to_string(e) + ")";
        return nullptr;
    }
    auto im = std::make_unique<Impl>();
    im->opt = opt;
    im->listener = l;
    im->port = opt.port;
    if (opt.udp) {
        // UDP on the same port. A failure (port taken by another program) only costs the UDP path and discovery.
        const SOCKET u = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int e = u == INVALID_SOCKET ? -1 : bind_any(u, false);
        if (!e) {
            BOOL off = FALSE;   // no WSAECONNRESET on recvfrom for an ICMP port-unreachable from an earlier send
            DWORD got = 0;
            ::WSAIoctl(u, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
            im->usock = u;
        } else {
            if (u != INVALID_SOCKET) ::closesocket(u);
            NXLOG_W("net", "relay: UDP port %u unavailable (error %d): datagrams go over TCP and LAN discovery cannot find this relay", opt.port, e);
        }
    }
    Impl* p = im.get();
    p->accept_t = std::thread([p] { p->accept_loop(); });
    if (p->usock != INVALID_SOCKET) p->udp_t = std::thread([p] { p->udp_loop(); });
    NXLOG_I("net", "relay: listening on 0.0.0.0:%u (TCP%s)", opt.port, p->usock != INVALID_SOCKET ? " and UDP" : "");
    return std::unique_ptr<RelayServer>(new RelayServer(std::move(im)));
}

void RelayServer::stop() {
    if (!im_ || !im_->accept_t.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(im_->mu);
        for (const auto& [id, c] : im_->clients) {
            if (!c->room) continue;
            Out f(kLeft);
            f.u16(static_cast<uint16_t>(LeftReason::Shutdown));
            Impl::queue(*c, f.finish());
        }
        im_->stopping.store(true);
    }
    im_->accept_t.join();
    if (im_->udp_t.joinable()) im_->udp_t.join();
    std::vector<Impl::Conn> conns;
    {
        std::lock_guard<std::mutex> lk(im_->conns_mu);
        conns.swap(im_->conns);
    }
    for (auto& c : conns) c.t.join();   // each reader sees the stop within 250 ms, drains its writer and closes
    ::closesocket(im_->listener);
    if (im_->usock != INVALID_SOCKET) ::closesocket(im_->usock);
    im_->listener = im_->usock = INVALID_SOCKET;
    NXLOG_I("net", "relay: stopped");
}

// =============================================================================================================================================
// LAN discovery and local addresses
// =============================================================================================================================================

std::vector<LocalAddr> local_ipv4() {
    std::vector<LocalAddr> out;
    ULONG len = 16384, r = ERROR_BUFFER_OVERFLOW;
    std::vector<uint8_t> buf;
    for (int i = 0; i < 3 && r == ERROR_BUFFER_OVERFLOW; ++i) {
        buf.assign(len, 0);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                 reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &len);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::string name;
        if (a->FriendlyName) {
            const int k = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
            if (k > 1) {
                name.resize(static_cast<size_t>(k));
                WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name.data(), k, nullptr, nullptr);
                name.resize(static_cast<size_t>(k - 1));
            }
        }
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const sockaddr* sa = u->Address.lpSockaddr;
            if (!sa || sa->sa_family != AF_INET) continue;
            const uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr);
            if (!ip || (ip >> 24) == 127) continue;
            const ULONG pl = u->OnLinkPrefixLength;
            const uint32_t mask = pl == 0 || pl > 32 ? 0 : pl == 32 ? 0xFFFFFFFFu : 0xFFFFFFFFu << (32 - pl);
            out.push_back({ip, mask, name});
        }
    }
    return out;
}

std::vector<FoundRelay> discover_relays(uint16_t port, int wait_ms) {
    std::vector<FoundRelay> out;
    if (!ensure_winsock()) return out;
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return out;
    const BOOL on = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
    BOOL off = FALSE;
    DWORD got = 0;
    ::WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&any), sizeof any) != 0) {
        ::closesocket(s);
        return out;
    }
    uint8_t req[kDiscoveryLen] = {};
    std::memcpy(req, kDiscRequest, sizeof kDiscRequest);
    if (!random_bytes(req + 8, 8)) {
        const uint64_t t = static_cast<uint64_t>(Clock::now().time_since_epoch().count()) ^ GetCurrentProcessId();
        std::memcpy(req + 8, &t, 8);
    }
    std::set<uint32_t> targets{0xFFFFFFFFu, 0x7F000001u};   // limited broadcast, a relay on this PC
    for (const LocalAddr& a : local_ipv4())
        if (a.mask && a.mask != 0xFFFFFFFFu) targets.insert(a.ip | ~a.mask);   // each interface's directed broadcast
    for (const uint32_t t : targets) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = htonl(t);
        ::sendto(s, reinterpret_cast<const char*>(req), sizeof req, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }
    std::set<uint64_t> seen;
    const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
    uint8_t b[kDiscoveryLen + 1];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
        if (left <= 0) break;
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{};
        tv.tv_sec = static_cast<long>(left / 1000000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>(left % 1000000);   // long on Linux, int on Darwin
        const int k = ::select(0, &r, nullptr, nullptr, &tv);
        if (k < 0) break;
        if (k == 0) continue;   // the window is over (checked above)
        sockaddr_in from{};
        int fl = sizeof from;
        const int n = ::recvfrom(s, reinterpret_cast<char*>(b), sizeof b, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n < static_cast<int>(kDiscReplyHdr) || n > static_cast<int>(kDiscoveryLen) || from.sin_family != AF_INET) continue;
        if (std::memcmp(b, kDiscReply, sizeof kDiscReply) != 0 || std::memcmp(b + 8, req + 8, 8) != 0) continue;
        const size_t nl = b[19];
        if (kDiscReplyHdr + nl != static_cast<size_t>(n)) continue;
        FoundRelay f;
        f.ip = ntohl(from.sin_addr.s_addr);
        f.port = static_cast<uint16_t>(get_le(b + 16, 2));
        f.open_rooms = b[18];
        f.nickname.assign(reinterpret_cast<const char*>(b + kDiscReplyHdr), nl);
        if (!f.port || !seen.insert((uint64_t{f.ip} << 16) | f.port).second) continue;
        out.push_back(std::move(f));
    }
    ::closesocket(s);
    return out;
}

}  // namespace nxrt::vnet
