// SPDX-License-Identifier: GPL-3.0-or-later
// The mods host's shared state (nxrt/mods.h): for modtopia_host.cpp (start, hooks, events) and modtopia_backend.cpp (the MtBackend the
// Modtopia core calls) only.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "modtopia/modtopia_host.h"
#include "nxrt/abi.h"
#include "nxrt/hooks.h"

namespace nxrt::mods::detail {

// What the host hands mt_hook_dispatch as host_ctx: the stopped thread's registers and the stop's guest address (the pc a mod reads).
struct Stop {
    ::Ctx* c;
    uint64_t site;
};

// One hook point built into the game exe (game::ModHost::hooks_built): the plan's kind (MT_HOOK_*) and the lifter site that serves it.
struct Built {
    int plan_kind = 0;
    HookKind site = HookKind::Inline;
};

struct Host {
    MtLoader* L = nullptr;
    bool off = false;   // [mods] off: this start runs without mods (no loader at all)
    // [mods] locked / locked_settings: the mods (ids) and settings (`id:key`) the launcher keeps on; a mod list shows them locked.
    std::vector<std::string> locked, locked_settings;
    // Strings the core keeps pointers to (MtOptions, MtBackend): they live as long as the process.
    std::string mods_dir, mappings_dir, host_name, game_version, build_id, save_dir;
    std::unordered_map<uint64_t, Built> built;   // guest address -> the point built there
    std::mutex events_mu;                        // mt_loader_save_written: one thread at a time
    // Code the player has not allowed (MT_MOD_NEW_CODE on a mod that is on): no code mod runs this time (the core's no_code), and a code
    // mod's data waits with its code. The ids of the new ones, for the log, the mod list's message and a notice.
    bool code_held = false;
    std::vector<std::string> new_code;
};
Host& host();

MtBackend make_backend();   // modtopia_backend.cpp: every callback the host provides; the rest NULL
// modtopia_input.cpp: the players' pads for mods, through the hid service's input filter (installed once, when a code mod started).
void install_input();
int pad_read(int p, MtPad* out);
int pad_hold(int p, const MtPad* add);
int pad_block(int p, const MtPad* mask);

// call_original's thread-local "skip this site once": true (and the flag cleared) when the entry stop at site is to be skipped now.
bool take_skip(uint64_t site);

}  // namespace nxrt::mods::detail
