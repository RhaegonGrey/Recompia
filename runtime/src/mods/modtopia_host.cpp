// SPDX-License-Identifier: GPL-3.0-or-later
// The mods host (nxrt/mods.h): the Modtopia core's calls in the order its docs/HOST.md section 3 gives, the hook points it built into the
// game exe registered as runtime hooks, the core events and the Link Play list. The backend the core calls back is modtopia_backend.cpp.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include "nxrt/mods.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <vector>

#include "modtopia_mappings.gen.h"
#include "mods_internal.h"
#include "nxrt/config.h"
#include "nxrt/file_write.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/notices.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "nxrt/text.h"

namespace stdfs = std::filesystem;

namespace nxrt::mods {

namespace detail {
Host& host() {
    static Host h;
    return h;
}
}  // namespace detail

namespace {

using detail::Built;
using detail::host;
using detail::Stop;

constexpr const char* kTag = "mods";

std::string utf8_of(const stdfs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// The embedded mappings (third_party/modtopia/mappings) into dir, each file written when it is missing or its bytes differ (the launcher
// writes the same files from its own copy).
void write_mappings(const std::string& dir) {
    for (const ModtopiaMapping& m : kModtopiaMappings) {
        const stdfs::path p = stdfs::u8path(dir) / m.name;
        const std::string_view want(reinterpret_cast<const char*>(m.data), m.size);
        std::ifstream in(p, std::ios::binary);
        const std::string have = in ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string();
        in.close();
        if (have == want) continue;
        std::string err;
        if (!write_file_atomic(utf8_of(p), want, &err)) NXLOG_W(kTag, "mods: cannot write the mappings %s: %s", utf8_of(p).c_str(), err.c_str());
    }
}

// The hook points the game build has ("modtopia hooks 1", one `module offset kind word site` line a point; the site column names the
// lifter site that serves the plan's kind, which is the kind the runtime hook must be registered as). Missing file: none.
size_t read_built(const std::string& path, const MtBackend& be) {
    std::ifstream in(stdfs::u8path(path), std::ios::binary);
    if (!in) return 0;
    size_t n = 0;
    for (std::string line; std::getline(in, line);) {
        const std::string t = text::trim(line);
        if (t.empty() || t[0] == '#') continue;
        std::istringstream f(t);
        std::string module, offset, kind, word, site;
        if (!(f >> module >> offset >> kind >> word >> site)) {
            NXLOG_W(kTag, "mods: %s: a line without its site column is left out: %s", path.c_str(), t.c_str());
            continue;
        }
        Built b;
        b.plan_kind = kind == "observe" ? MT_HOOK_OBSERVE : kind == "take_over" ? MT_HOOK_TAKE_OVER : kind == "replace" ? MT_HOOK_REPLACE : 0;
        bool site_ok = true;
        if (site == "inline") b.site = HookKind::Inline;
        else if (site == "midhook") b.site = HookKind::Mid;
        else if (site == "entryhook") b.site = HookKind::Entry;
        else site_ok = false;
        const uint64_t base = be.module_base(be.user, module.c_str());
        const uint64_t off = std::strtoull(offset.c_str(), nullptr, 0);
        if (!b.plan_kind || !site_ok || !base) {
            NXLOG_W(kTag, "mods: %s: a line this host cannot use is left out: %s", path.c_str(), t.c_str());
            continue;
        }
        host().built[base + off] = b;
        ++n;
    }
    return n;
}

// The runtime hooks at the built points: each runs the core's handlers with the stopped thread's registers. "Handled" (take over, replace)
// returns from the game function. An entry stop that call_original asked to skip carries on into the function this once.
void on_inline(::Ctx* c, uint64_t addr, void*) {
    Stop s{c, addr};
    mt_hook_dispatch(host().L, addr, &s);
}
int on_mid(::Ctx* c, uint64_t addr, void*) {
    Stop s{c, addr};
    return mt_hook_dispatch(host().L, addr, &s) == MT_HANDLED ? 1 : 0;
}
int on_entry(::Ctx* c, uint64_t addr, void*) {
    if (detail::take_skip(addr)) return 0;
    Stop s{c, addr};
    return mt_hook_dispatch(host().L, addr, &s) == MT_HANDLED ? 1 : 0;
}

// One runtime hook at every address a mod that is on uses and the game build has, by the site that was built there.
void register_hooks(MtLoader* L) {
    std::vector<uint64_t> done;
    for (int i = 0, n = mt_loader_point_count(L); i < n; ++i) {
        uint64_t addr = 0;
        int kind = 0;
        const char *mod = nullptr, *name = nullptr;
        if (mt_loader_point(L, i, &addr, &kind, &mod, &name) != MT_OK || !addr) continue;
        if (std::find(done.begin(), done.end(), addr) != done.end()) continue;
        const auto it = host().built.find(addr);
        if (it == host().built.end()) continue;   // not built in: the core turns the mod off at start (hook_built_in)
        done.push_back(addr);
        switch (it->second.site) {
            case HookKind::Inline: hook_add_inline(addr, &on_inline); break;
            case HookKind::Mid: hook_add_mid(addr, &on_mid); break;
            case HookKind::Entry: hook_add_entry(addr, &on_entry); break;
        }
    }
}

// The data mods' folders over the game's RomFS (index 0 highest), under the player's own [fs] romfs_mod. While code is held, a code mod's
// folders wait with its code: the core lists them at scan although its code is off this time (no_code), and drops them only when the
// patches step builds the list again, so without this a code mod's files would be served or not depending on whether any patch applied.
void lay_overlay(MtLoader* L) {
    std::vector<std::string> roots;
    for (int i = 0, n = mt_loader_overlay_count(L); i < n; ++i) {
        const char* r = mt_loader_overlay_root(L, i);
        if (!r) continue;
        if (host().code_held) {
            const char* id = mt_loader_overlay_mod(L, i);
            const MtModInfo* m = id ? mt_loader_info(L, mt_loader_find(L, id)) : nullptr;
            if (m && (m->flags & MT_MOD_HAS_CODE)) continue;
        }
        roots.emplace_back(r);
    }
    services::fsapi::set_romfs_overlays(roots);
}

// A new loader on the host's folders, the mods found and resolved. no_code: no code mod is loaded this time (data mods still are).
MtLoader* open_loader(const MtBackend& be, bool no_code) {
    detail::Host& h = host();
    MtOptions o{};
    o.mods_dir = h.mods_dir.c_str();
    o.mappings_dir = h.mappings_dir.c_str();
    o.no_code = no_code ? 1 : 0;
    MtLoader* L = mt_loader_new(&o, &be);
    if (!L) {
        NXLOG_E(kTag, "mods: the mod loader could not start");
        return nullptr;
    }
    if (mt_loader_scan(L) < 0) {
        NXLOG_E(kTag, "mods: %s cannot be read, none loaded", h.mods_dir.c_str());
        mt_loader_free(L);
        return nullptr;
    }
    return L;
}

// The code mods that are on after the scan, those with code the player has not allowed (new, or changed since the launcher recorded the
// player's yes) when fresh, the others otherwise.
std::vector<std::string> code_mods(MtLoader* L, bool fresh) {
    std::vector<std::string> out;
    for (int i = 0, n = mt_loader_count(L); i < n; ++i) {
        const MtModInfo* m = mt_loader_info(L, i);
        if (m && m->state == MT_STATE_OK && (m->flags & MT_MOD_HAS_CODE) && !(m->flags & MT_MOD_NEW_CODE) == !fresh) out.emplace_back(m->id);
    }
    return out;
}

// After a start with code held: one line a new mod, and the reason in each held code mod's message (the in-game Mods list shows it). The
// note goes in after mt_loader_start, which sets the message of each code mod it leaves off.
void report_held(MtLoader* L, const std::vector<std::string>& waiting) {
    for (const std::string& id : host().new_code) {
        const MtModInfo* m = mt_loader_info(L, mt_loader_find(L, id.c_str()));
        NXLOG_W(kTag, "mods: %s %s has new or changed code: allow it in the launcher's Mods page", id.c_str(), m ? m->version : "?");
        mt_loader_note(L, id.c_str(), "new or changed code: allow it in the launcher's Mods page");
        notices::post("mods:" + id, std::string("Mod ") + (m && m->name && *m->name ? m->name : id.c_str()) +
                      " has new or changed code and did not run: allow it in the launcher's Mods page", 12);
    }
    for (const std::string& id : waiting)
        mt_loader_note(L, id.c_str(), "waits until the new code of another mod is allowed in the launcher's Mods page");
}

// For bug reports, once the mods are started, patched and checked: one line a mod (its state this run, id, version, what it holds, and the
// core's message: why it is off, or a note), then the gameplay mods every player of a Link Play session needs (gameplay_mods()).
void log_mods(MtLoader* L) {
    for (int i = 0, n = mt_loader_count(L); i < n; ++i) {
        const MtModInfo* m = mt_loader_info(L, i);
        if (!m) continue;
        const int f = m->flags;
        const char* state = m->state == MT_STATE_OK ? "on" : m->state == MT_STATE_OFF ? "off" : "error";
        const bool msg = m->message && *m->message;
        NXLOG_AT(m->state == MT_STATE_ERROR ? LogLevel::Warn : LogLevel::Info, kTag, "mods: %-5s %s %s%s%s%s%s%s%s%s%s", state, m->id, m->version,
                 f & MT_MOD_HAS_CODE ? " [code]" : "", f & MT_MOD_NEW_CODE ? " [new code]" : "", f & MT_MOD_HAS_DATA ? " [data]" : "",
                 f & MT_MOD_HAS_PATCHES ? " [patches]" : "", f & MT_MOD_LAYEREDFS ? " [LayeredFS]" : "", f & MT_MOD_GAMEPLAY ? " [gameplay]" : "",
                 msg ? ": " : "", msg ? m->message : "");
    }
    const std::vector<std::string> gameplay = gameplay_mods();
    if (gameplay.empty()) return;
    std::string list;
    for (const std::string& g : gameplay) list += (list.empty() ? "" : ", ") + g;
    NXLOG_I(kTag, "mods: Link Play: every player in a session needs these gameplay mods: %s", list.c_str());
}

// A comma- or blank-separated ini list (the launcher writes `a, b`).
std::vector<std::string> ini_list(const char* key) {
    std::vector<std::string> out;
    std::string item;
    for (const char ch : config_get_str("mods", key, "") + ",") {
        if (ch == ',' || ch == ' ' || ch == '\t') {
            if (!item.empty()) out.push_back(std::move(item));
            item.clear();
        } else {
            item += ch;
        }
    }
    return out;
}

void on_save_committed() {
    detail::Host& h = host();
    std::lock_guard<std::mutex> lk(h.events_mu);
    mt_loader_save_written(h.L);
}

}  // namespace

void start(const StartOptions& opt) {
    detail::Host& h = host();
    const game::Info& gi = game::info();
    if (!gi.mods || h.L || h.off) return;
    // Start without mods: the player's way out when a mod keeps the game from starting (the launcher passes it for one start). Nothing
    // of the mods is touched: no scan, no files laid over the game's, the player's switches and the mods' settings stay as they are.
    if (config_get_bool("mods", "off", false)) {
        h.off = true;
        NXLOG_I(kTag, "mods: [mods] off: this start runs without mods");
        notices::post("mods", "Started without mods: they are back at the next start", 10);
        return;
    }
    h.locked = ini_list("locked");
    h.locked_settings = ini_list("locked_settings");
    std::error_code ec;
    const stdfs::path mods_dir = stdfs::absolute(stdfs::u8path(gi.mods->mods_dir), ec);
    if (ec || !stdfs::is_directory(mods_dir, ec)) {
        NXLOG_I(kTag, "mods: no %s folder, none loaded", gi.mods->mods_dir);
        return;
    }
    h.mods_dir = utf8_of(mods_dir);
    h.mappings_dir = utf8_of(stdfs::absolute(stdfs::u8path(gi.mods->mappings_dir), ec));
    h.host_name = std::string(gi.mods->host) + (opt.host_version && *opt.host_version ? std::string(" ") + opt.host_version : std::string());
    h.game_version = gi.version;
    const Module* main = module_by_name("main");
    h.build_id = main ? main->build_id() : std::string();
    h.save_dir = services::save_dir();
    write_mappings(h.mappings_dir);

    // HOST.md section 3, before the game's first instruction: find and resolve the mods, lay the data over the RomFS, start the code. The
    // game's files are read from its folders, so the patches and the data check run now too (step 4), before the game opens its RomFS.
    //
    // Code the player has not allowed never runs, also when the game is started without the launcher (which asks and records the yes with
    // mt_loader_trust_mod). The core only flags it (MT_MOD_NEW_CODE) and would load it, and it has no per-mod switch for one run that is
    // not saved to the player's list. So when a mod that is on has new code, the loader is made again with no_code: no code mod runs this
    // time, the player's saved switches stay as they are, data-only mods still load.
    const MtBackend be = detail::make_backend();
    MtLoader* L = open_loader(be, false);
    if (!L) return;
    h.new_code = code_mods(L, true);
    if (!h.new_code.empty()) {
        mt_loader_free(L);   // nothing was started: it unloads nothing
        h.code_held = true;
        if (!(L = open_loader(be, true))) return;
    }
    h.L = L;
    const size_t built = read_built(gi.mods->hooks_built, be);
    if (!h.code_held) register_hooks(L);   // held: no code runs, so no handler could be at any point
    lay_overlay(L);
    const std::vector<std::string> waiting = h.code_held ? code_mods(L, false) : std::vector<std::string>();
    const int code = mt_loader_start(L);
    if (code > 0) detail::install_input();   // the pads mods read and hold, only once a code mod runs
    if (h.code_held) report_held(L, waiting);
    if (mt_loader_apply_patches(L) > 0) lay_overlay(L);
    mt_loader_check_data(L);
    log_mods(L);
    services::fsapi::romfs_game_release();
    services::fsapi::set_save_commit_listener(&on_save_committed);

    int on = 0;
    const int found = mt_loader_count(L);
    for (int i = 0; i < found; ++i)
        if (const MtModInfo* m = mt_loader_info(L, i); m && m->state == MT_STATE_OK) ++on;
    NXLOG_I(kTag, "mods: %d found, %d on, %d code; hooks built: %zu", found, on, code, built);
}

void frame() {
    if (MtLoader* L = host().L) mt_loader_frame(L);
}

std::vector<std::string> gameplay_mods() {
    std::vector<std::string> out;
    if (MtLoader* L = host().L)
        mt_loader_gameplay_mods(L, [](void* user, const char* id) { static_cast<std::vector<std::string>*>(user)->emplace_back(id); }, &out);
    std::sort(out.begin(), out.end());
    return out;
}

MtLoader* loader() { return host().L; }
bool started_without() { return host().off; }

bool locked(const char* id) {
    const std::vector<std::string>& l = host().locked;
    return id && std::find(l.begin(), l.end(), id) != l.end();
}

bool setting_locked(const char* id, const char* key) {
    if (!id || !key) return false;
    const std::vector<std::string>& l = host().locked_settings;
    return std::find(l.begin(), l.end(), std::string(id) + ":" + key) != l.end();
}

}  // namespace nxrt::mods
