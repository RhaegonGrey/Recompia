// SPDX-License-Identifier: GPL-3.0-or-later
// Lobby implementations (see lobby.h): the relay protocol v3 client, plus SHA-256 for the passphrase hash.
// Relay wire format: relay_wire.h (shared with the embedded server, relay_server.cpp).
// SHA-256 follows FIPS 180-4 (the round constants and the initial hash are the standard's).
#include "lobby.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "nxrt/log.h"
#include "relay_wire.h"
#include "nxrt/text.h"
#include "tcp.h"
#include "udp.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt")
#endif

#ifndef NXRT_BUILD_STAMP
#define NXRT_BUILD_STAMP "nxrt-ldn-relay-1"
#endif

namespace nxrt::vnet {

const char* build_stamp() { return NXRT_BUILD_STAMP; }

const char* lobby_err_str(int e) {
    switch (e) {
        case kLobbyOk: return "ok";
        case kLobbyBadProto: return "protocol version not supported by the server";
        case kLobbyBuild: return "the server runs a different game build";
        case kLobbyPassword: return "wrong password";
        case kLobbyRoomFull: return "room or server full";
        case kLobbyNoRoom: return "room not found";
        case kLobbyVersion: return "game version differs from the room's";
        case kLobbyNotHost: return "not the room host";
        case kLobbyRejected: return "rejected by the host";
        case kLobbyRate: return "rate limited by the server";
        case kLobbyBadFrame: return "the server refused a frame";
        case kLobbyRoomCode: return "wrong room code";
        case kLobbyTimeout: return "no answer from the server";
        case kLobbyNoServer: return "not connected to the server";
        case kLobbyState: return "not allowed in this state";
        default: return "error";
    }
}

bool parse_server(const std::string& s, std::string* host, uint16_t* port) {
    std::string h = s;
    text::trim_blanks(h);
    if (h.empty()) return false;
    uint16_t p = kRelayDefaultPort;
    const size_t colon = h.rfind(':');
    if (colon != std::string::npos) {
        const std::string ps = h.substr(colon + 1);
        h.resize(colon);
        if (ps.empty() || ps.size() > 5) return false;
        unsigned v = 0;
        for (char c : ps) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<unsigned>(c - '0');
        }
        if (v == 0 || v > 65535) return false;
        p = static_cast<uint16_t>(v);
    }
    if (h.empty()) return false;
    *host = h;
    *port = p;
    return true;
}

// ---- SHA-256 ----------------------------------------------------------------------------------------------------------------------------

std::array<uint8_t, 32> sha256(const void* data, size_t n) {
    static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int r) { return (x >> r) | (x << (32 - r)); };
    std::vector<uint8_t> m(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const uint64_t bits = static_cast<uint64_t>(n) * 8;
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t{m[off + i * 4]} << 24) | (uint32_t{m[off + i * 4 + 1]} << 16) | (uint32_t{m[off + i * 4 + 2]} << 8) | m[off + i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(h[i] >> (24 - j * 8));
    return out;
}

std::array<uint8_t, 16> passphrase_hash(const void* p, size_t n) {
    const auto full = sha256(p, n);
    std::array<uint8_t, 16> out{};
    std::memcpy(out.data(), full.data(), 16);
    return out;
}

// From pokopia-native (MIT), windows/src/coop/coop_auth.c @7c3655d: the alphabet, coop_room_normalize, coop_room_new, coop_room_format.
static const char kB32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";   // Crockford's base32

bool room_code_normalize(const std::string& in, std::string* out) {
    std::string s;
    for (char ch : in) {
        if (ch == '-' || ch == ' ') continue;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        if (ch == 'O') ch = '0';
        else if (ch == 'I' || ch == 'L') ch = '1';   // Crockford's reading of the look-alikes
        if (!ch || !std::strchr(kB32, ch) || s.size() == kRoomCodeLen) return false;
        s.push_back(ch);
    }
    if (s.size() != kRoomCodeLen) return false;
    if (out) *out = s;
    return true;
}

std::string room_code_format(const std::string& norm) {
    std::string s;
    for (size_t i = 0; i < norm.size(); ++i) {
        if (i && i % 4 == 0) s.push_back('-');
        s.push_back(norm[i]);
    }
    return s;
}

std::string room_code_new() {
    uint8_t r[kRoomCodeLen];
#ifdef _WIN32
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return {};
#else
    FILE* f = std::fopen("/dev/urandom", "rb");
    const bool ok = f && std::fread(r, 1, sizeof r, f) == sizeof r;
    if (f) std::fclose(f);
    if (!ok) return {};
#endif
    std::string s(kRoomCodeLen, '0');
    for (size_t i = 0; i < kRoomCodeLen; ++i) s[i] = kB32[r[i] & 31];   // 256 is a multiple of 32: no bias
    std::memset(r, 0, sizeof r);
    return s;
}

// The prefix is part of the relay protocol (both sides of a keyed room derive the key with it), not a game fact: the same for every game,
// and fixed, or older builds stop finding keyed rooms.
std::array<uint8_t, 16> room_key(const std::string& norm) {
    const std::string m = "pokopia-relay room v1" + norm;
    return passphrase_hash(m.data(), m.size());
}

namespace {

// =============================================================================================================================================
// Relay protocol v3 (the codec is relay_wire.h)
// =============================================================================================================================================

using namespace wire;

constexpr int kUdpProbeMs = 100, kUdpProbeForMs = 3000, kUdpKeepaliveMs = 5000, kUdpDeadMs = 15000;

// =============================================================================================================================================
// RelayClient
// =============================================================================================================================================

class RelayClient final : public Lobby, public Link, public std::enable_shared_from_this<RelayClient> {
public:
    explicit RelayClient(RelayOptions opt) : opt_(std::move(opt)) {
        if (opt_.build_stamp.empty()) opt_.build_stamp = build_stamp();
    }
    ~RelayClient() override { disconnect(); }

    // ---- Lobby ----
    int connect() override {
        std::lock_guard<std::mutex> rq(req_mu_);
        return connect_locked();
    }
    bool connected() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return connected_;
    }
    int scan(uint64_t lcid, uint16_t scene, std::vector<Room>* out) override {
        std::lock_guard<std::mutex> rq(req_mu_);
        if (const int e = connect_locked()) return e;
        Out o(kScan);
        o.u64(lcid).u16(scene).raw(opt_.room_key.data(), 16);
        const int e = request(o.finish(), kRoomList);
        if (e == 0 && out) {
            std::lock_guard<std::mutex> lk(mu_);
            *out = std::move(got_rooms_);
        }
        return e;
    }
    int create(const RoomConfig& c, Joined* out) override {
        std::lock_guard<std::mutex> rq(req_mu_);
        if (const int e = connect_locked()) return e;
        if (in_room()) return kLobbyState;
        Out o(kCreate);
        o.u64(c.local_comm_id).u16(c.scene_id).u16(static_cast<uint16_t>(c.comm_version)).u8(c.node_count_max).u8(c.security_mode);
        o.raw(c.pass_hash.data(), 16).raw(opt_.room_key.data(), 16);
        std::vector<uint8_t> adv = c.adv;
        if (adv.size() > kMaxAdvertise) adv.resize(kMaxAdvertise);
        o.blob(adv);
        return finish_join(request(o.finish(), kJoined), out);
    }
    int join(uint32_t room_id, int16_t ver, const std::array<uint8_t, 16>& hash, Joined* out) override {
        std::lock_guard<std::mutex> rq(req_mu_);
        if (const int e = connect_locked()) return e;
        if (in_room()) return kLobbyState;
        Out o(kJoin);
        o.u32(room_id).u16(static_cast<uint16_t>(ver)).raw(hash.data(), 16).raw(opt_.room_key.data(), 16);
        return finish_join(request(o.finish(), kJoined), out);
    }
    void leave() override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!in_room_) return;
            clear_room();
        }
        send_frame(Out(kLeave).finish());   // the LEFT 0 answer is ignored (the reader drops LEFT while in no room)
    }
    int set_advertise(const std::vector<uint8_t>& adv) override {
        if (!in_room()) return kLobbyState;
        Out o(kAdvertise);
        std::vector<uint8_t> a = adv;
        if (a.size() > kMaxAdvertise) a.resize(kMaxAdvertise);
        o.blob(a);
        return send_frame(o.finish()) ? kLobbyOk : kLobbyNoServer;
    }
    int reject(uint32_t vip) override {
        if (!in_room()) return kLobbyState;
        Out o(kReject);
        o.u32(vip);
        return send_frame(o.finish()) ? kLobbyOk : kLobbyNoServer;
    }
    Room room() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return room_;
    }
    bool in_room() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return in_room_;
    }
    std::shared_ptr<Link> link() override { return shared_from_this(); }
    void set_events(LobbyEvents ev) override {
        std::lock_guard<std::mutex> lk(ev_mu_);
        ev_ = std::move(ev);
    }

    // ---- Link ----
    uint32_t local_ip() const override { return vip_.load(std::memory_order_acquire); }   // lock-free: vsock asks without its lock
    bool send(const Datagram& d) override {
        const uint32_t me = local_ip();
        if (!me) return false;
        if (d.dst_ip == me) return true;   // the server drops self traffic; vsock already looped it back
        const size_t n = d.data.size() < kMaxDatagram ? d.data.size() : kMaxDatagram;
        if (udp_up_.load(std::memory_order_acquire)) {
            std::vector<uint8_t> b;
            b.reserve(kUDataHdr + n);
            b.push_back(kUData);
            put_le(b, udp_token_live_.load(std::memory_order_acquire), 8);
            put_le(b, d.src_port, 2);
            put_le(b, d.dst_ip, 4);
            put_le(b, d.dst_port, 2);
            b.insert(b.end(), d.data.data(), d.data.data() + n);
            if (udp::send(udp_sock_.load(std::memory_order_acquire), b.data(), b.size())) return true;
            // not sent (socket going away): this one over TCP
        }
        Out o(kDatagram);
        o.u16(d.src_port).u32(d.dst_ip).u16(d.dst_port);
        o.u16(static_cast<uint16_t>(n)).raw(d.data.data(), n);
        return send_frame(o.finish());
    }
    bool udp_active() const override { return udp_up_.load(std::memory_order_acquire); }
    std::string server_version() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return server_version_;
    }

private:
    enum Want : uint8_t { kNone = 0, kWantWelcome = kWelcome, kWantList = kRoomList, kWantJoined = kJoined };

    // Caller holds req_mu_.
    int connect_locked() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (connected_) return kLobbyOk;
            // A failed attempt is not retried for 3 s: a game scanning in a loop must not stall on a dead server each time.
            if (last_err_ && std::chrono::steady_clock::now() - last_try_ < std::chrono::seconds(3)) return last_err_;
            last_try_ = std::chrono::steady_clock::now();
        }
        const std::string where = opt_.host + ":" + std::to_string(opt_.port);
        for (;;) {
            join_threads();
            std::string why;
            const tcp::Handle h = tcp::dial(opt_.host, opt_.port, opt_.timeout_ms, opt_.timeout_ms, &why);
            if (h == tcp::kBad) return fail_connect(kLobbyNoServer, where + ": " + why);
            {
                std::lock_guard<std::mutex> lk(mu_);
                sock_ = h;
                connected_ = true;   // provisional: the reader runs; WELCOME confirms
                stop_ = false;
            }
            reader_ = std::thread([this] { read_loop(); });
            Out o(kHello);
            o.u16(proto_).str(opt_.build_stamp).str(opt_.nickname, kMaxNick).str(opt_.password);
            const int e = request(o.finish(), kWantWelcome);
            if (e == kLobbyOk) break;
            disconnect();
            if (e == kLobbyBadProto && proto_ > kRelayMinProto) {
                // a relay from before the UDP path: the same connection again as protocol 2 (TCP only), and from now on
                --proto_;
                NXLOG_I("net", "ldn: relay %s speaks an older protocol: protocol %u, datagrams over TCP", where.c_str(), proto_);
                continue;
            }
            return fail_connect(e, where + ": " + (err_msg_.empty() ? lobby_err_str(e) : err_msg_));
        }
        pinger_ = std::thread([this] { ping_loop(); });
        if (proto_ >= 3 && opt_.udp) udp_thread_ = std::thread([this] { udp_loop(); });
        {
            std::lock_guard<std::mutex> lk(mu_);
            last_err_ = 0;
        }
        NXLOG_I("net", "ldn: relay %s connected as %s (client %u, server %s)", where.c_str(), opt_.nickname.c_str(), client_id_,
                server_version_.c_str());
        return kLobbyOk;
    }

    int fail_connect(int e, const std::string& msg) {
        bool log;
        {
            std::lock_guard<std::mutex> lk(mu_);
            log = e != last_err_ || msg != last_msg_;   // the same failure is logged once
            last_err_ = e;
            last_msg_ = msg;
        }
        if (log) NXLOG_I("net", "ldn: relay %s", msg.c_str());
        return e;
    }

    // Sends one request frame and waits for its answer (or an ERROR). Caller holds req_mu_.
    int request(std::vector<uint8_t> frame, uint8_t want) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!connected_) return kLobbyNoServer;
            want_ = want;
            got_ = false;
            got_err_ = 0;
            err_msg_.clear();
        }
        if (!send_frame(frame)) {
            std::lock_guard<std::mutex> lk(mu_);
            want_ = kNone;
            return kLobbyNoServer;
        }
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(opt_.timeout_ms), [&] { return got_ || !connected_; });
        want_ = kNone;
        if (got_) return got_err_;
        return connected_ ? kLobbyTimeout : kLobbyNoServer;
    }

    int finish_join(int e, Joined* out) {
        if (e == 0 && out) {
            std::lock_guard<std::mutex> lk(mu_);
            *out = got_joined_;
        }
        return e;
    }

    bool send_frame(const std::vector<uint8_t>& f) {
        tcp::Handle h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!connected_) return false;
            h = sock_;
        }
        std::lock_guard<std::mutex> sl(send_mu_);
        if (tcp::send_all(h, f.data(), f.size())) return true;
        tcp::shutdown(h);   // a failed or stalled write: the reader sees the close and reports the connection lost
        return false;
    }

    void clear_room() {   // caller holds mu_
        // the relay drops the room's UDP token with the room: back to TCP until the next room's UDP_OFFER
        if (udp_token_) {
            udp_token_ = 0;
            ++udp_gen_;
            cv_.notify_all();
        }
        udp_up_.store(false, std::memory_order_release);
        in_room_ = false;
        vip_.store(0, std::memory_order_release);
        room_ = Room{};
    }

    void read_loop() {
        log_set_thread_name("lobby-read");
        tcp::Handle h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            h = sock_;
        }
        std::vector<uint8_t> buf;
        for (;;) {
            uint32_t len = 0;
            if (!tcp::recv_all(h, &len, 4) || len == 0 || len > kMaxFrame) break;
            buf.resize(len);
            if (!tcp::recv_all(h, buf.data(), len)) break;
            dispatch(buf[0], buf.data() + 1, len - 1);
        }
        lost();
    }

    void dispatch(uint8_t type, const uint8_t* p, size_t n) {
        In in(p, n);
        switch (type) {
            case kWelcome: {
                const uint32_t id = in.u32();
                std::string ver = in.str();
                answer(kWantWelcome, 0, [&] {
                    client_id_ = id;
                    server_version_ = std::move(ver);
                });
                break;
            }
            case kError: {
                const uint16_t code = in.u16();
                std::string msg = in.str();
                std::lock_guard<std::mutex> lk(mu_);
                if (want_ != kNone && !got_) {
                    got_ = true;
                    got_err_ = code ? code : kLobbyBadFrame;
                    err_msg_ = msg;
                    cv_.notify_all();
                } else {
                    NXLOG_W("net", "ldn: relay error %u: %s", code, msg.c_str());
                }
                break;
            }
            case kRoomList: {
                const uint8_t count = in.u8();
                std::vector<Room> rooms;
                for (uint8_t i = 0; i < count && in.ok; ++i) {
                    Room r = read_room(in);
                    if (in.ok) rooms.push_back(std::move(r));
                }
                answer(kWantList, 0, [&] { got_rooms_ = std::move(rooms); });
                break;
            }
            case kJoined: {
                Joined j;
                j.room_id = in.u32();
                j.vip = in.u32();
                j.node_id = in.u8();
                bool stale = false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (want_ == kWantJoined && !got_) {
                        in_room_ = true;
                        room_ = Room{};
                        room_.room_id = j.room_id;
                        vip_.store(j.vip, std::memory_order_release);
                        got_joined_ = j;
                        got_ = true;
                        got_err_ = 0;
                        cv_.notify_all();
                    } else {
                        stale = true;   // the request timed out meanwhile: do not stay in a room nobody asked for any more
                    }
                }
                if (stale) send_frame(Out(kLeave).finish());
                break;
            }
            case kRoomState: {
                Room r = read_room(in);
                if (!in.ok) break;   // malformed, or more nodes than the protocol allows
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (!in_room_ || r.room_id != room_.room_id) break;
                    room_ = r;
                }
                std::lock_guard<std::mutex> el(ev_mu_);
                if (ev_.room_state) ev_.room_state(r);
                break;
            }
            case kSDatagram: {
                Datagram d;
                d.src_ip = in.u32();
                d.src_port = in.u16();
                d.dst_ip = in.u32();
                d.dst_port = in.u16();
                d.data = in.blob();
                if (in.ok && d.data.size() <= kMaxDatagram && local_ip() != 0) deliver(d);   // the UDP path drops the same oversize ones
                break;
            }
            case kLeft: {
                const uint16_t why = in.u16();
                if (why == static_cast<uint16_t>(LeftReason::YouLeft)) break;   // the answer to our own LEAVE
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (!in_room_) break;
                    clear_room();
                }
                std::lock_guard<std::mutex> el(ev_mu_);
                if (ev_.left) ev_.left(static_cast<LeftReason>(why));
                break;
            }
            case kPong: break;
            case kUdpOffer: {
                const uint64_t token = in.u64();
                const uint16_t port = in.u16();
                std::lock_guard<std::mutex> lk(mu_);
                if (!in.ok || !in_room_) break;   // a token belongs to the room this lobby is in
                udp_token_ = token;
                udp_port_ = port;
                ++udp_gen_;
                cv_.notify_all();
                break;
            }
            default: NXLOG_D("net", "ldn: relay frame type 0x%02x ignored", type); break;
        }
    }

    template <class F>
    void answer(uint8_t want, int err, F&& fill) {
        std::lock_guard<std::mutex> lk(mu_);
        if (want_ != want || got_) return;
        fill();
        got_ = true;
        got_err_ = err;
        cv_.notify_all();
    }

    // The connection is gone (closed, failed, or disconnect()).
    void lost() {
        bool was_in_room, was_connected, asked;
        {
            std::lock_guard<std::mutex> lk(mu_);
            was_in_room = in_room_;
            was_connected = connected_;
            asked = stop_;
            clear_room();
            connected_ = false;
            cv_.notify_all();
        }
        if (was_connected && !asked) {
            NXLOG_W("net", "ldn: relay %s:%u connection lost", opt_.host.c_str(), opt_.port);
            std::lock_guard<std::mutex> lk(mu_);
            last_err_ = 0;   // reconnect at the next request, without the 3 s pause
        }
        if (was_in_room && !asked) {
            std::lock_guard<std::mutex> el(ev_mu_);
            if (ev_.left) ev_.left(LeftReason::ConnectionLost);
        }
    }

    void ping_loop() {
        log_set_thread_name("lobby-ping");
        uint32_t nonce = 0;
        std::unique_lock<std::mutex> lk(mu_);
        while (!stop_ && connected_) {
            cv_.wait_for(lk, std::chrono::milliseconds(opt_.ping_ms), [&] { return stop_ || !connected_; });
            if (stop_ || !connected_) break;
            lk.unlock();
            Out o(kPing);
            o.u32(++nonce);
            send_frame(o.finish());
            lk.lock();
        }
    }

    void disconnect() {
        tcp::Handle h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
            h = sock_;
            cv_.notify_all();
        }
        tcp::shutdown(h);
        join_threads();
        std::lock_guard<std::mutex> lk(mu_);
        if (sock_ != tcp::kBad) tcp::close(sock_);
        sock_ = tcp::kBad;
        connected_ = false;
        clear_room();
    }

    // The v3 UDP datagram path (lobby.h, RelayClient). Owns the UDP socket; game threads only read udp_sock_ / udp_up_ to send.
    void udp_loop() {
        log_set_thread_name("lobby-udp");
        using clock = std::chrono::steady_clock;
        uint32_t ip;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ip = tcp::peer_ip(sock_);
        }
        udp::Handle u = udp::kBad;
        uint64_t token = 0;
        uint32_t gen = 0;
        clock::time_point probe_until{}, next_hello{}, last_ack{};
        bool socket_failed_logged = false;
        std::vector<uint8_t> buf(kUSDataHdr + kMaxDatagram + 1);
        auto close_socket = [&] {
            udp_up_.store(false, std::memory_order_release);
            udp_sock_.store(udp::kBad, std::memory_order_release);
            udp::close(u);
            u = udp::kBad;
        };
        auto mode = [&](bool on) {
            Out o(kUdpMode);
            o.u8(on ? 1 : 0);
            send_frame(o.finish());
        };
        std::unique_lock<std::mutex> lk(mu_);
        while (!stop_ && connected_) {
            if (udp_gen_ != gen) {   // a new room's token, or the room is gone
                gen = udp_gen_;
                token = udp_token_;
                const uint16_t port = opt_.udp_test_port ? opt_.udp_test_port : udp_port_;
                udp_token_live_.store(token, std::memory_order_release);
                lk.unlock();
                close_socket();
                if (token && ip) {
                    std::string why;
                    u = udp::open(ip, port, kUdpProbeMs, &why);
                    if (u == udp::kBad && !socket_failed_logged) {
                        NXLOG_W("net", "ldn: relay UDP socket: %s (datagrams stay on TCP)", why.c_str());
                        socket_failed_logged = true;
                    }
                    udp_sock_.store(u, std::memory_order_release);
                    probe_until = clock::now() + std::chrono::milliseconds(kUdpProbeForMs);
                    next_hello = clock::now();
                }
                lk.lock();
                continue;
            }
            if (!token || u == udp::kBad) {
                cv_.wait_for(lk, std::chrono::milliseconds(250), [&] { return stop_ || !connected_ || udp_gen_ != gen; });
                continue;
            }
            lk.unlock();
            const auto now = clock::now();
            if (now >= next_hello) {
                std::vector<uint8_t> b{kUHello};
                put_le(b, token, 8);
                udp::send(u, b.data(), b.size());
                next_hello = now + std::chrono::milliseconds(now < probe_until ? kUdpProbeMs : kUdpKeepaliveMs);
            }
            if (udp_up_.load(std::memory_order_acquire) && now - last_ack > std::chrono::milliseconds(kUdpDeadMs)) {
                udp_up_.store(false, std::memory_order_release);
                mode(false);
                NXLOG_W("net", "ldn: relay UDP path lost (no answer for %d s): datagrams over TCP", kUdpDeadMs / 1000);
            }
            const int n = udp::recv(u, buf.data(), buf.size() - 1);   // waits up to kUdpProbeMs
            if (n >= static_cast<int>(kUHdr) && n <= static_cast<int>(kUSDataHdr + kMaxDatagram) && get_le(buf.data() + 1, 8) == token) {
                if (buf[0] == kUAck && n == static_cast<int>(kUHdr)) {
                    last_ack = clock::now();
                    if (!udp_up_.load(std::memory_order_acquire)) {
                        udp_up_.store(true, std::memory_order_release);
                        mode(true);
                        NXLOG_I("net", "ldn: relay UDP path up: game datagrams over UDP");
                    }
                } else if (buf[0] == kUSData && n >= static_cast<int>(kUSDataHdr)) {
                    Datagram d;
                    d.src_ip = static_cast<uint32_t>(get_le(buf.data() + 9, 4));
                    d.src_port = static_cast<uint16_t>(get_le(buf.data() + 13, 2));
                    d.dst_ip = static_cast<uint32_t>(get_le(buf.data() + 15, 4));
                    d.dst_port = static_cast<uint16_t>(get_le(buf.data() + 19, 2));
                    d.data.assign(buf.data() + kUSDataHdr, buf.data() + n);
                    if (local_ip() != 0) deliver(d);
                }
            }
            lk.lock();
        }
        lk.unlock();
        close_socket();
    }

    void join_threads() {
        for (std::thread* t : {&reader_, &pinger_, &udp_thread_}) {
            if (!t->joinable()) continue;
            if (t->get_id() == std::this_thread::get_id()) t->detach();   // the last owner let go inside an event sink
            else t->join();
        }
        std::lock_guard<std::mutex> lk(mu_);
        if (!connected_ && sock_ != tcp::kBad) {
            tcp::close(sock_);
            sock_ = tcp::kBad;
        }
    }

    RelayOptions opt_;
    std::mutex req_mu_;            // one request at a time
    std::mutex send_mu_;           // one frame write at a time
    std::mutex ev_mu_;             // held while a sink runs
    LobbyEvents ev_;
    mutable std::mutex mu_;        // everything below
    std::condition_variable cv_;
    tcp::Handle sock_ = tcp::kBad;
    bool connected_ = false, stop_ = false;
    std::thread reader_, pinger_, udp_thread_;
    uint16_t proto_ = kRelayProto;   // drops to kRelayMinProto for an older relay
    uint64_t udp_token_ = 0;         // the room's UDP token (0: none)
    uint16_t udp_port_ = 0;
    uint32_t udp_gen_ = 0;           // bumped on every token change
    std::atomic<uint64_t> udp_token_live_{0};          // the token udp_sock_ proves (read by send())
    std::atomic<udp::Handle> udp_sock_{udp::kBad};
    std::atomic<bool> udp_up_{false};                  // ACKs come back: datagrams go over UDP
    uint8_t want_ = kNone;
    bool got_ = false;
    int got_err_ = 0;
    std::string err_msg_;
    std::vector<Room> got_rooms_;
    Joined got_joined_;
    uint32_t client_id_ = 0;
    std::string server_version_;
    bool in_room_ = false;
    Room room_;
    std::atomic<uint32_t> vip_{0};
    int last_err_ = 0;
    std::string last_msg_;
    std::chrono::steady_clock::time_point last_try_{};
};

}  // namespace

std::shared_ptr<Lobby> make_relay_client(const RelayOptions& opt) { return std::make_shared<RelayClient>(opt); }

}  // namespace nxrt::vnet
