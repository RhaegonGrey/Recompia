// SPDX-License-Identifier: GPL-3.0-or-later
// ldn:u on pokopia-native's co-op module (see the comment below): the coop node from [net] and the nn::ldn commands as coop_ldn_* calls.
#include "ldn_native.h"

#include "ldn_linkplay.h"


#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#if !defined(_WIN32)
#include "nx_os.h"   // _wfopen on POSIX: the wide path is converted to UTF-8
#endif
#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "coop.h"
#include "../net/relay_server.h"
#include "nxrt/game.h"
#include "nxrt/text.h"
#include "../net/vnet.h"
#include "services_internal.h"

namespace nxrt::services::ldn_impl {

// =============================================================================================================================================
// [net] protocol = native (the default): ldn:u on pokopia-native's co-op module (third_party/pokopia-coop, MIT @9cb7f07), so this build
// and pokopia-native builds of the same main meet in one Link Play session. The module owns the nn::ldn state machine, the 0x480
// NetworkInfo, discovery (DNS-SD on the LAN), the transport (TCP control links in a star around the host, UDP for game datagrams) and the
// game's UDP sockets on 169.254.S.0/24 (bsd: network.cpp routes descriptors 0x100.. to coop_vnet_*). Each IPC command is one coop_ldn_*
// call (the structs are the SDK's, as the IPC carries them; results are nn::Result module 203). Wiring after pokopia-native's
// windows/src/coop/nxnative/ldn_glue.c.
// Config ([net], read once when the first ldn:u service object is made): mode = auto | join:<IPv4>[:<port>] | off (auto: host on the LAN
// while the game hosts, find LAN hosts with DNS-SD while it scans; without DNS-SD, Wine / Proton, a LAN sweep and mDNS answers: lan_sweep =
// auto | off), internet (0/1: internet play, every link proves the room code),
// room_code (the host's code; a host without one gets a new one, logged and written to room-code.txt; a visitor needs the host's),
// relay (<IPv4>[:<port>], a pokopia-native coop relay; mode auto only), port_forward (0/1, default 1: a host over the internet asks
// the router to forward its port), uid (32 hex digits; default: the install's persistent uid,
// coop_uid_load). mode = off or an invalid setting: local wireless off (the Link Play backend's answers with no lobby).
// What peers compare before a join: main's build id (first 16 hex digits) and a mods fingerprint. The fingerprint is
// coop_mods_add(0, name) of the session variant's name (services_api.h ldnapi; Pokopia's "Interact everywhere" while armed, the same
// name pokopia-native adds as COOP_INTERACT_MOD) while one is set, then the co-op mods below; 0 when there is neither. The variant's
// bits 12..14 of the local communication version are a Link Play (relay) thing and are not sent here: the game's version goes out as the
// game gave it.
// The game module's co-op mods (ldnapi::set_coop_mods) follow the variant's name in its order: Pokopia's Story co-op
// (games/pkp/core/pkp_linkplay.cpp) adds COOP_STORY_MOD's name after the Interact one, so both on give pokopia-native's fingerprint
// c75a2da0380038c1.
// Not mapped (the module has no counterpart; pokopia-native cuts them too): CreateNetworkPrivate (DeviceNotAvailable), ConnectPrivate
// (ConnectNotFound), the accept filter (accepted in access point states and ignored). GetNetworkInfoLatestUpdate's per-node flags are
// built here from the node tables the game reads.
// =============================================================================================================================================

bool native_protocol() {
    static const bool native = [] {
        const std::string p = text::lower(config_get_str("net", "protocol", "native"));
        if (p == "linkplay") return false;
        if (p != "native") NXLOG_W("net", "ldn: [net] protocol = \"%s\" is not native or linkplay: native", p.c_str());
        return true;
    }();
    return native;
}

namespace {

// Owns the process's coop node (native(), made by native_node()). With the module off (no node) a service object gets the linkplay
// backend's Ldn with no lobby: its offline answers.
class NativeBackend final : public LdnBackend {
public:
    const char* name() const override { return "native"; }
    std::shared_ptr<LdnSession> open(ipc::Service& s) override {
        if (coop_node* n = native_node()) {
            auto l = std::make_shared<NativeLdn>(n);
            l->add_commands(s);
            return l;
        }
        auto l = std::make_shared<Ldn>(true);
        l->add_commands(s);
        return l;
    }
    void reset() override {
        coop_node* node = slot_.node.exchange(nullptr);
        {
            std::lock_guard<std::mutex> lk(slot_.mu);
            slot_.built = false;
            slot_.build.clear();
        }
        if (node) coop_destroy(node);   // finalizes: the host's links and the virtual sockets close
    }

    NativeSlot slot_;
};

NativeBackend& backend() {
    static NativeBackend b;
    return b;
}

}  // namespace

LdnBackend& native_backend() { return backend(); }
NativeSlot& native() { return backend().slot_; }


namespace {

void coop_hook_log(void*, const char* line) { NXLOG_I("net", "coop: %s", line); }
void coop_hook_state(void*, int) { native().ev.signal(); }
const char* coop_hook_build(void*) {
    NativeSlot& s = native();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.build.empty()) {
        const Module* m = module_by_name("main");
        if (!m || m->build_id().empty()) m = module_at(mem::kMainBase);
        if (m) s.build = m->build_id();
    }
    return s.build.empty() ? nullptr : s.build.c_str();   // set once and never changed: the pointer stays valid
}
uint64_t coop_hook_mods(void*, char* what, size_t n) {
    std::string name;
    std::vector<std::string> mods;
    if (!own_variant(&name, &mods)) name.clear();   // the variant's name is kept for the log while none is set: it counts only when one is
    if (name.empty() && mods.empty()) return 0;
    // pokopia-native's coop_hmods order: the cheats (none here), the variant's name (when one is set), then the game module's co-op mods in
    // its order (its own features, then the Modtopia gameplay mods, which count with or without a variant)
    uint64_t h = 0;
    std::string shown;
    if (!name.empty()) {
        h = coop_mods_add(h, name.c_str());
        shown = name;
    }
    for (const std::string& m : mods) {
        h = coop_mods_add(h, m.c_str());
        shown += (shown.empty() ? "" : ", ") + m;
    }
    std::snprintf(what, n, "%s", shown.c_str());
    return h;
}

// The install's persistent coop uid (nn::account::Uid as 32 hex digits): coop-uid.txt in the save folder ([fs] save_dir), made from the
// OS random source on first use. The save folder, not the game folder: an install copied to another device must not take its identity
// along, and the save folder (per Windows user, per Wine prefix) outlives reinstalls and updates of the game folder. The fs service only
// reads *.meta files there.
std::string coop_uid_load() {
    namespace stdfs = std::filesystem;
    const std::string sd = save_dir();
    const stdfs::path dir(std::u8string(sd.begin(), sd.end())), file = dir / "coop-uid.txt";
    const std::u8string u8 = file.u8string();
    const std::string shown(u8.begin(), u8.end());
    auto valid = [](const std::string& s) {
        if (s.size() != 32 || s == std::string(32, '0')) return false;
        for (char c : s)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        return true;
    };
    if (FILE* f = _wfopen(file.c_str(), L"rb")) {
        char b[64] = {};
        const size_t got = std::fread(b, 1, sizeof b - 1, f);
        std::fclose(f);
        std::string s(b, got);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (valid(s)) return s;
        NXLOG_W("net", "ldn: %s is not 32 hex digits: a new uid is made", shown.c_str());
    }
    std::random_device rd;   // rand_s on Windows: the OS random source
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    while (!valid(s)) {
        s.clear();
        for (int i = 0; i < 4; ++i) {
            const uint32_t v = rd();
            for (int k = 7; k >= 0; --k) s += kHex[(v >> (4 * k)) & 15];
        }
    }
    std::error_code ec;
    stdfs::create_directories(dir, ec);
    if (FILE* f = _wfopen(file.c_str(), L"wb")) {
        std::fprintf(f, "%s\n", s.c_str());
        std::fclose(f);
        NXLOG_I("net", "ldn: new co-op uid kept in %s", shown.c_str());
    } else {
        NXLOG_W("net", "ldn: cannot write %s: this run has a uid of its own", shown.c_str());
    }
    return s;
}

}  // namespace

// The process's coop node from [net] (made once; null: local wireless off).
coop_node* native_node() {
    NativeSlot& s = native();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.built) return s.node.load();
        s.built = true;
    }
    std::string mode = text::lower(config_get_str("net", "mode", "auto"));
    text::trim_blanks(mode);
    const bool internet = config_get_bool("net", "internet", false);
    std::string room = config_get_str("net", "room_code", "");
    if (text::lower(room) == "new") room.clear();   // a host without a code gets a new one
    const std::string relay = config_get_str("net", "relay", "");
    // Internet play through a relay is always mode = auto (the module refuses "join:" with a relay and Link Play would be off). A join:<ip>
    // left in the file from an earlier visit is not a choice in that case: use auto (#239). Every other combination is left as it is.
    if (internet && !relay.empty() && mode.compare(0, 5, "join:") == 0) {
        NXLOG_W("net", "ldn: [net] mode = %s cannot be combined with [net] relay = %s (a relay needs mode = auto): using auto", mode.c_str(), relay.c_str());
        mode = "auto";
    }
    std::string uid = config_get_str("net", "uid", "");
    if (uid.empty() && !mode.empty() && mode != "off" && mode != "0") uid = coop_uid_load();
    coop_cfg c;
    char err[256] = "";
    const int r = coop_cfg_parse_ex(&c, mode.c_str(), uid.c_str(), internet ? "1" : "0", room.c_str(), relay.c_str(), err, sizeof err);
    const std::string code_file = room_code_file();
    if (r <= 0) {
        if (r < 0) NXLOG_W("net", "ldn: [net] (protocol native): %s: local wireless off", err);
        else NXLOG_I("net", "ldn: [net] mode = off: local wireless off");
        std::remove(code_file.c_str());
        return nullptr;
    }
    // [net] lan_sweep = auto | off: auto (the default) finds and answers LAN players without DNS-SD (Wine / Proton: the module's LAN sweep and
    // mDNS responder, coop_disc.c x_*); off: there, visitors join by address. Windows with DNS-SD never uses them
    const std::string sweep = text::lower(config_get_str("net", "lan_sweep", "auto"));
    if (sweep != "auto" && sweep != "off") NXLOG_W("net", "ldn: [net] lan_sweep = \"%s\" is not auto or off: auto", sweep.c_str());
    c.x_lan_sweep_off = sweep == "off";
    // [net] port_forward (default on): hosting over the internet asks the router to forward the hosting port (NAT-PMP, PCP or UPnP IGD,
    // coop_nat.c), with a one-hour lease renewed while hosting and given back when hosting ends. The process ends with TerminateProcess
    // (play_startup.cpp), so a quit while hosting leaves the lease to run out
    c.nat = config_get_bool("net", "port_forward", true) ? 1 : 0;
    c.hooks.user = nullptr;
    c.hooks.log = &coop_hook_log;
    c.hooks.sleep_ns = nullptr;   // a plain host sleep: ldn and bsd commands run on host threads
    c.hooks.on_state = &coop_hook_state;
    c.hooks.build = &coop_hook_build;
    c.hooks.mods = &coop_hook_mods;
    coop_node* n = coop_create(&c);
    if (!n) {
        NXLOG_W("net", "ldn: the co-op module did not start (no sockets): local wireless off");
        std::remove(code_file.c_str());
        return nullptr;
    }
    const char* role = c.role == COOP_ROLE_JOIN ? "join" : c.role == COOP_ROLE_HOST ? "host" : "auto";
    if (c.role == COOP_ROLE_JOIN)   // a join:<ip> left over from a visit: Invite Others makes a room nobody can reach (#239)
        NXLOG_W("net", "ldn: [net] mode = %s: this PC can only visit that host. To host (Invite Others to Visit), set [net] mode = auto in "
                "%s, or pick Host on the launcher's Link Play page", mode.c_str(), *game::info().game_ini ? game::info().game_ini : "the ini file");
    NXLOG_I("net", "ldn: protocol native (pokopia-native's co-op), mode %s, port %u, %s", role, c.port,
            c.scope == COOP_SCOPE_INTERNET ? "internet play (links prove the room code; not encrypted)"
            : c.scope == COOP_SCOPE_LOOPBACK ? "loopback peers only" : "LAN peers only");
    if (c.auth) {
        char shown[COOP_ROOM_SHOWN];
        coop_room_format(c.room, shown);
        if (FILE* f = std::fopen(code_file.c_str(), "wb")) {
            std::fprintf(f, "%s\n", shown);
            std::fclose(f);
        }
        // only the first group: players attach pkp.log to public bug reports; the launcher reads the whole code from room-code.txt
        NXLOG_I("net", "ldn: internet play room code %.4s-****-****-****%s (the rest stays out of the log)", shown, c.room_new ? ", new: visitors need it" : "");
    } else {
        std::remove(code_file.c_str());
    }
    s.node.store(n);
    return n;
}

void NativeLdn::cmd(ipc::Service& s, uint32_t id, const char* name, H h) {
    std::weak_ptr<NativeLdn> w = weak_from_this();
    s.add_cmd(id, name, [w, id, name, h](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        note(id, name);
        auto self = w.lock();
        if (!self) return kInvalidState;
        return ((*self).*h)(req, rsp);   // the module is thread-safe: no command lock (Scan and Connect wait inside it)
    });
}

void NativeLdn::add_commands(ipc::Service& s) {
    cmd(s, 0, "GetState", &NativeLdn::get_state);
    cmd(s, 1, "GetNetworkInfo", &NativeLdn::get_network_info);
    cmd(s, 2, "GetIpv4Address", &NativeLdn::get_ipv4);
    cmd(s, 3, "GetDisconnectReason", &NativeLdn::get_disconnect_reason);
    cmd(s, 4, "GetSecurityParameter", &NativeLdn::get_security_parameter);
    cmd(s, 5, "GetNetworkConfig", &NativeLdn::get_network_config);
    cmd(s, 100, "AttachStateChangeEvent", &NativeLdn::attach_event);
    cmd(s, 101, "GetNetworkInfoLatestUpdate", &NativeLdn::get_latest_update);
    cmd(s, 102, "Scan", &NativeLdn::scan);
    cmd(s, 103, "ScanPrivate", &NativeLdn::scan);
    cmd(s, 104, "SetWirelessControllerRestriction", &NativeLdn::accept);
    cmd(s, 105, "SetWirelessAudioPolicy", &NativeLdn::accept);
    cmd(s, 106, "SetProtocol", &NativeLdn::accept);
    cmd(s, 200, "OpenAccessPoint", &NativeLdn::open_access_point);
    cmd(s, 201, "CloseAccessPoint", &NativeLdn::close_access_point);
    cmd(s, 202, "CreateNetwork", &NativeLdn::create_network);
    cmd(s, 203, "CreateNetworkPrivate", &NativeLdn::create_network_private);
    cmd(s, 204, "DestroyNetwork", &NativeLdn::destroy_network);
    cmd(s, 205, "Reject", &NativeLdn::reject);
    cmd(s, 206, "SetAdvertiseData", &NativeLdn::set_advertise_data);
    cmd(s, 207, "SetStationAcceptPolicy", &NativeLdn::set_accept_policy);
    cmd(s, 208, "AddAcceptFilterEntry", &NativeLdn::accept_filter);
    cmd(s, 209, "ClearAcceptFilter", &NativeLdn::accept_filter);
    cmd(s, 300, "OpenStation", &NativeLdn::open_station);
    cmd(s, 301, "CloseStation", &NativeLdn::close_station);
    cmd(s, 302, "Connect", &NativeLdn::connect);
    cmd(s, 303, "ConnectPrivate", &NativeLdn::connect_private);
    cmd(s, 304, "Disconnect", &NativeLdn::disconnect);
    cmd(s, 400, "Initialize", &NativeLdn::initialize);
    cmd(s, 401, "Finalize", &NativeLdn::finalize);
    cmd(s, 402, "InitializeWithVersion", &NativeLdn::initialize);
    cmd(s, 403, "SetOperationMode", &NativeLdn::accept);
}

}  // namespace nxrt::services::ldn_impl
