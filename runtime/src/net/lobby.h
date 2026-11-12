// SPDX-License-Identifier: GPL-3.0-or-later
// The lobby under the ldn HLE (multiplayer phase 2): rooms (= LDN networks) on a relay server, and the vnet::Link that carries a room's
// UDP datagrams. Game-independent.
//
//   Lobby          abstract: connect (HELLO), scan, create, join, leave, set_advertise, reject, plus one event sink (room state pushes and
//                  "you are out of the room"). Each Lobby is also the Link of its node while it is in a room (link()).
//   RelayClient    Lobby + Link over ONE TCP connection to the relay server (relay protocol v3, served by net/relay_server.cpp; the
//                  frame format is defined by net/relay_wire.h, there is no other spec). A reader thread turns frames into events, request answers and Link deliveries;
//                  sends share one mutex; PING every 5 s; SCAN/CREATE/JOIN wait up to 5 s for their answer. A v2 relay (ERROR 1 to the
//                  v3 HELLO) is dialled again with proto 2: everything as before, TCP only.
//                  UDP datagram path (v3, RelayOptions::udp): in a room the relay hands out a token (UDP_OFFER); a third thread proves
//                  it over UDP to the relay's port (HELLO every 100 ms for 3 s, then every 5 s as keepalive / NAT refresh) and on the
//                  first ACK switches the game datagrams to UDP both ways (UDP_MODE 1). No ACK for 15 s: back to TCP (UDP_MODE 0),
//                  still probing. Until then, and whenever UDP is blocked, datagrams go over the TCP connection as in v2.
//
// Room semantics (the server's): room R (1..254) is 169.254.R.0/24, the host is .1 (node 0), guests the lowest free .2...8 (node N-1),
// node MAC 02:00:a9:fe:R:N. A datagram to .255 goes to every other member, never back to the sender; to a member's vIP, to that member.
// Room codes: a lobby with a room key (RelayOptions::room_key) creates its rooms with that key; such a room is
// listed to and joinable by lobbies with the same key only (kLobbyRoomCode otherwise). A lobby without a key creates open rooms that
// everyone sees (LAN play). Scans of a keyed lobby list the open rooms and the rooms with its key.
//
// Threads: events and Link deliveries arrive on the lobby's own thread (the reader thread of a RelayClient). A sink may call room() / in_room() and vnet::set_current_link(); it must not call a blocking request
// (scan/create/join) of the same lobby. Requests are serialised per lobby; call them from one thread at a time anyway (the ldn HLE does).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vnet.h"

namespace nxrt::vnet {

// ---- protocol values -------------------------------------------------------------------------------------------------------------------
constexpr uint16_t kRelayProto = 3;      // 2: room keys in SCAN / CREATE / JOIN; 3: the UDP datagram path
constexpr uint16_t kRelayMinProto = 2;   // the oldest relay the client still talks to (TCP only)
constexpr uint16_t kRelayDefaultPort = 39660;
constexpr size_t kMaxNick = 32, kMaxAdvertise = 384;
constexpr uint8_t kMaxNodes = 8;

// Lobby results: 0 ok, 1..10 the relay's ERROR codes, 100+ local failures.
enum LobbyErr : int {
    kLobbyOk = 0,
    kLobbyBadProto = 1, kLobbyBuild = 2, kLobbyPassword = 3, kLobbyRoomFull = 4, kLobbyNoRoom = 5, kLobbyVersion = 6,
    kLobbyNotHost = 7, kLobbyRejected = 8, kLobbyRate = 9, kLobbyBadFrame = 10, kLobbyRoomCode = 11,
    kLobbyTimeout = 100,      // no answer in time
    kLobbyNoServer = 101,     // not connected / the connection failed or was lost
    kLobbyState = 102,        // CREATE/JOIN while in a room, LEAVE/ADVERTISE/REJECT while in none (checked locally)
};
const char* lobby_err_str(int e);

// Why the lobby left a room on its own (LEFT reasons 0..3 of the protocol, plus the connection going away).
enum class LeftReason : uint16_t { YouLeft = 0, HostClosed = 1, Rejected = 2, Shutdown = 3, ConnectionLost = 0x100 };

struct RoomNode {
    uint8_t node_id = 0;
    uint32_t vip = 0;
    std::array<uint8_t, 6> mac{};
    std::string nickname;
};

struct Room {
    uint32_t room_id = 0;
    uint64_t local_comm_id = 0;
    uint16_t scene_id = 0;
    int16_t comm_version = 0;
    uint8_t node_count_max = 0;
    uint8_t security_mode = 0;
    std::vector<RoomNode> nodes;   // in the server's order (node 0, the host, first)
    std::vector<uint8_t> adv;
};

struct RoomConfig {
    uint64_t local_comm_id = 0;
    uint16_t scene_id = 0;
    int16_t comm_version = 0;
    uint8_t node_count_max = kMaxNodes;   // 1..8
    uint8_t security_mode = 1;
    std::array<uint8_t, 16> pass_hash{};
    std::vector<uint8_t> adv;            // <= 384 B
};

struct Joined {
    uint32_t room_id = 0;
    uint32_t vip = 0;
    uint8_t node_id = 0;
};

struct LobbyEvents {
    std::function<void(const Room&)> room_state;   // a ROOM_STATE of the room this lobby is in (join, leave, advertise)
    std::function<void(LeftReason)> left;          // out of the room without asking (host closed, rejected, shutdown, connection lost)
};

class Lobby {
public:
    virtual ~Lobby() = default;
    // Connects and says HELLO when not connected yet. kLobbyOk or the failure (the ERROR code answering HELLO, kLobbyNoServer ...).
    virtual int connect() = 0;
    virtual bool connected() const = 0;
    // Rooms that are not full and match (local_comm_id 0 = any, scene_id 0xffff = any).
    virtual int scan(uint64_t local_comm_id, uint16_t scene_id, std::vector<Room>* out) = 0;
    virtual int create(const RoomConfig& cfg, Joined* out) = 0;
    virtual int join(uint32_t room_id, int16_t comm_version, const std::array<uint8_t, 16>& pass_hash, Joined* out) = 0;
    // Leaves the room (host: closes it). No event follows for this lobby.
    virtual void leave() = 0;
    virtual int set_advertise(const std::vector<uint8_t>& adv) = 0;   // host only
    virtual int reject(uint32_t vip) = 0;                              // host only
    // The latest state of the room this lobby is in (room_id 0 when in none) and whether it is in one.
    virtual Room room() const = 0;
    virtual bool in_room() const = 0;
    // This node's datagram link (its local_ip() is the vIP while in a room, 0 otherwise).
    virtual std::shared_ptr<Link> link() = 0;
    // Replaces the event sink. Waits for an event in progress to finish, so it must not be called from inside a sink.
    virtual void set_events(LobbyEvents ev) = 0;
    // The game datagrams currently go over UDP (relay v3 path proven both ways). Diagnostics and tests.
    virtual bool udp_active() const { return false; }
    // The server version string of the relay's WELCOME (empty before the first connect). The embedded relay
    // puts a random instance id in it, so it names one relay whatever address it was reached by.
    virtual std::string server_version() const { return {}; }
};

// ---- RelayClient ----------------------------------------------------------------------------------------------------------------------
struct RelayOptions {
    std::string host;             // name or IPv4 address
    uint16_t port = kRelayDefaultPort;
    std::string nickname = "Player";
    std::string password;
    std::string build_stamp;      // empty: build_stamp()
    std::array<uint8_t, 16> room_key{};   // room_key(code); all zero: no room code (open rooms only)
    int timeout_ms = 5000;        // connect, HELLO and request answers
    int ping_ms = 5000;
    bool udp = true;              // [net] udp: try the v3 UDP datagram path (false: TCP only)
    uint16_t udp_test_port = 0;   // tests: send the UDP packets to this port instead of the offered one (a dead port = blocked UDP)
};

// Parses "host:port" (port optional, default 39660). False when empty or the port is bad.
bool parse_server(const std::string& s, std::string* host, uint16_t* port);

std::shared_ptr<Lobby> make_relay_client(const RelayOptions& opt);

// ---- helpers -----------------------------------------------------------------------------------------------------------------------------
// What HELLO sends as the build stamp: identifies the runtime's relay client (NXRT_BUILD_STAMP at compile time, else a fixed string).
const char* build_stamp();
// The relay's passphrase hash: the first 16 bytes of SHA-256 of the passphrase bytes.
std::array<uint8_t, 16> passphrase_hash(const void* p, size_t n);
std::array<uint8_t, 32> sha256(const void* p, size_t n);

// Room codes (same format as launcher/relay/src/room_code.rs): 16 symbols of Crockford base32 (80 bits), shown XXXX-XXXX-XXXX-XXXX.
constexpr size_t kRoomCodeLen = 16;
// The 16 symbols of a typed code; false when it is not one. Case, '-' and spaces do not matter; O reads as 0, I and L as 1.
bool room_code_normalize(const std::string& in, std::string* out);
std::string room_code_format(const std::string& norm);   // XXXX-XXXX-XXXX-XXXX
std::string room_code_new();                              // normalized, from the OS random source; empty if that failed
// What goes on the wire instead of the code: the first 16 bytes of SHA-256("pokopia-relay room v1" + the normalized code).
std::array<uint8_t, 16> room_key(const std::string& norm);

}  // namespace nxrt::vnet
