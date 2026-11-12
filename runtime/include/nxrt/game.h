// SPDX-License-Identifier: GPL-3.0-or-later
// The game boundary (docs/GAME_BOUNDARY.md): everything one title supplies to the game-independent base.
//
// The runtime and the test tooling know no game. A game module (runtime/games/<id>/, a static library the game exe links)
// fills one Info and registers it with set_current() before anything reads it: before nxrt::init() in the game exe and at the top of the
// test mains. Every part is optional. A null table or an empty string turns off what needs it (no hooks, no
// probes, neutral title facts), so a new title can start with the identity alone and add tables as its hooks are found.
//
// What each table is for, and where the Pokopia one lives:
//   title_defaults   services::TitleProfile defaults (main.npdm, control.nacp); ini keys still override them    games/pkp/core/pkp_core.cpp
//   timestep         the sites of the real-time frame step (nxrt/timestep.h, Sites)                           (Pokopia sets none)
//   probes           test-tooling game-state probes and script steps (nxrt/tt.h, GameProbes)                  games/pkp/test/pkp_tt_probe.cpp
//   play_defaults    factory controls.ini text and the bindings added by later revisions (play_types.h)       games/pkp/core/pkp_data.cpp
//   play             play-mode hooks: aim, camera, display, menu pointer, F1 entries (play_game.h)            games/pkp/core/pkp_game.cpp
//   prompts          the game's button icon textures (play/prompt_resolve.h, PromptGame)                      (Pokopia sets none)
//   poll_sleeps      SleepThread calls inside the game's own polling loops, shortened (PollSleep below)        games/pkp/core/pkp_core.cpp
//   poll_waits       SleepThread calls of polling loops that end when the polled value changes (PollWait below) games/pkp/core/pkp_core.cpp
//   settings_doc     the keys of the game's own game-ini section, described (nxrt/setting_doc.h)              games/pkp/core/pkp_settings_table.cpp
//   render           graphics facts: readback shape, camera block, tone-map shaders (nxrt/game_render.h)   games/pkp/render/pkp_render_info.cpp
//   mods             the mods host's folders, name and shader files (nxrt/mods.h, ModHost below)             games/pkp/core/pkp_data.cpp
// The registry is header-only, so programs that do not link the runtime library (the launcher's tools) use it too.
#pragma once
#include <cstdint>
#include <vector>

struct Ctx;   // the guest CPU context (nxrecomp/ctx.h)

namespace nxrt {
namespace setting_doc { struct Row; }
namespace services { struct TitleProfile; }
namespace timestep { struct Sites; }
namespace tt { struct GameProbes; }
namespace play { struct GamePlay; struct GameDefaults; struct PromptGame; }
}  // namespace nxrt

namespace nxrt::game {

struct Render;   // nxrt/game_render.h

// One nn::os::SleepThread call that sits in a polling loop of the game: the loop re-checks its condition after every sleep, so a shorter
// sleep only checks more often (a 1 s poll for work that takes milliseconds costs up to a second each time). `ret` is the return address
// of the call (the instruction after the bl), as an offset in main; `ns` the sleep it gets instead, when that is shorter than the request.
// The kernel's SleepThread looks the caller up while [sched] poll_sleeps is on (default on).
struct PollSleep {
    uint64_t ret = 0;
    int64_t ns = 0;
};

// One nn::os::SleepThread call that sits in a polling loop whose condition is a value in guest memory: the loop sleeps while the value
// is still `idle` and goes on once it is not. `ret` is the return address of the call, as an offset in main. `watch` runs on the sleeping
// thread at the start of every such sleep, with the thread's registers (the callee-saved ones are the loop's own), and names what this
// round of the loop waits on; false: nothing to watch this time, the sleep runs its time. While [kernel] poll_wait is on the kernel ends
// the sleep once the value has left `idle` (src/kernel/poll_wait.cpp), at the latest on its time: the loop then finds its condition true,
// as it would have after the full sleep, so nothing but the waiting time changes. The value must stay changed until the loop has seen it.
struct PollWatch {
    uint64_t addr = 0;   // guest address of the value; mapped for as long as the loop polls it
    uint64_t idle = 0;   // what it holds while the loop has nothing to do
    uint8_t size = 0;    // 4 or 8 bytes
};
struct PollWait {
    uint64_t ret = 0;
    bool (*watch)(const ::Ctx& c, PollWatch* w) = nullptr;
};

// What the mods host (nxrt/mods.h: Modtopia mods) needs from the game. Paths are relative to the game folder (the working directory of an
// installed game). The game version the mods' hook points and the mappings are picked by is Info::version.
struct ModHost {
    const char* host = "";                                     // the host's name shown to mods, before the build's version ("Recompia")
    const char* mods_dir = "mods";                             // the player's mods (the core's own layout inside)
    const char* mappings_dir = "modtopia/mappings";            // the embedded mappings are written here when absent or different
    const char* hooks_built = "build/mod-hooks-built.tsv";     // the hook points built into the game exe ("modtopia hooks 1" + site column)
    // The game's files that hold shaders (RomFS paths, '/' separated, compared without case). A data mod that replaces one brings shaders
    // the shader cache has no translation for, which are translated only after the game closes; the core notes that on the mod (file_note).
    const char* const* shader_files = nullptr;
    uint32_t shader_file_count = 0;
};

// The console the game was built for. Switch 2 games run on the same Horizon model with a few platform facts that differ
// (system tick 31.25 MHz instead of 19.2 MHz, the NVN2 graphics API).
enum class Platform : uint8_t { Switch1, Switch2 };

struct Info {
    // ---- identity ----
    const char* id = "";                    // short id, also the stem of the game's file names: "pkp"
    const char* title = "";                 // the game's own name: "Pokémon Pokopia"
    const char* product = "";               // the PC port's name in window titles and dialogs: "Pokémon Pokopia PC"
    const char* short_title = "";           // a short name for file headers: "Pokopia" (empty: product)
    uint64_t program_id = 0;                // the Switch title id
    const char* version = "";               // the game version the hooks were found in: "2.0.1"
    uint64_t main_base = 0x7100000000ull;   // where main is mapped (the lifter's base for it): hook offsets are relative to this
    uint32_t languages = 0;                 // control.nacp SupportedLanguage: bit n = services::Language n (0: American English only)
    Platform platform = Platform::Switch1;  // the console the game targets

    // ---- files and branding ----
    const char* exe_name = "";              // the game exe: "pkp.exe"
    const char* game_ini = "";              // the settings file the game exe reads: "pkp.ini"
    const char* shader_pack = "";           // the shader pack file: "shader_cache.pkpshc"
    const char* save_folder = "";           // save data folder under %APPDATA%: "PokopiaRecompPC" (empty: "nxrt")

    // ---- game data ----
    // The RomFS of the supported version holds at least this many files and bytes (0: not checked). A RomFS with less is missing files and
    // the game fails at an unpredictable point when it reaches one (a null read at start for Pokopia, fork #128), so the exe stops at start
    // with a message instead. Extra files (desktop.ini and the like) pass.
    uint32_t romfs_min_files = 0;
    uint64_t romfs_min_bytes = 0;

    // ---- tables (null = the feature is off) ----
    void (*title_defaults)(services::TitleProfile& p) = nullptr;
    const timestep::Sites* timestep = nullptr;
    const tt::GameProbes* probes = nullptr;
    const play::GameDefaults* play_defaults = nullptr;
    const play::GamePlay* play = nullptr;
    const play::PromptGame* prompts = nullptr;
    const PollSleep* poll_sleeps = nullptr;
    uint32_t poll_sleep_count = 0;
    const PollWait* poll_waits = nullptr;
    uint32_t poll_wait_count = 0;
    // The keys the game reads from its own sections of the game ini ([pkp]), one row each: the docs (runtime/docs/CONFIG.md) and the
    // launcher's schema of the game ini are generated from it (apps/settings_doc.cpp). Pure data: the pure-data library can set it.
    std::vector<setting_doc::Row> (*settings_doc)() = nullptr;
    // What the graphics back end may know about the game's rendering (nxrt/game_render.h); null: those features are off.
    const Render* render = nullptr;
    // Modtopia mods (nxrt/mods.h); null: no mods are loaded. Pure data.
    const ModHost* mods = nullptr;
};

// The registered game, or nullptr. One slot per program (the function-local static is shared by every translation unit that inlines it).
inline const Info*& current_slot() {
    static const Info* slot = nullptr;
    return slot;
}
inline void set_current(const Info* g) { current_slot() = g; }
inline const Info* current() { return current_slot(); }
// The registered game, or an empty Info (every table null) when none is.
inline const Info& info() {
    static const Info none;
    const Info* g = current();
    return g ? *g : none;
}

}  // namespace nxrt::game
