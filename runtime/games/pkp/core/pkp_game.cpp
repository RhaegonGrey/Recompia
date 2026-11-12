// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the whole game module as one nxrt::game::Info (pkp_game.h). Play-mode hooks come here as they are found.
#include "core/pkp_game.h"
#include "input/pkp_aim.h"
#include "input/pkp_aim_debug.h"
#include "ui/pkp_craft.h"
#include "ui/pkp_item_edit.h"
#include "core/pkp_feature.h"
#include "input/pkp_jump.h"
#include "input/pkp_kbm.h"
#include "ui/pkp_key_guide.h"
#include "core/pkp_layout.h"
#include "core/pkp_linkplay.h"
#include "core/pkp_mods_notice.h"
#include "core/pkp_mods.h"
#include "ui/pkp_menu.h"
#include "render/pkp_ui_backdrop.h"
#if NXRT_TESTTOOLS
#include "test/pkp_movie_test.h"
#include "test/pkp_taa_probe.h"
#endif
#include "ui/pkp_options.h"
#include "input/pkp_play.h"
#include "render/pkp_quality.h"
#include "core/pkp_pokemon_cap.h"
#include "core/pkp_save_uuid.h"
#include "render/pkp_shoulder_camera.h"
#include "ui/pkp_sort.h"
#include "render/pkp_taa.h"
#include "ui/pkp_title.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>

#include "core/pkp_settings_table.h"

#include "play_game.h"
#include "play_types.h"

namespace nxrt::games::pkp {

namespace {

// The main loop (0x7100882c60, called from nnMain's startup) calls 0x7101fdcde4 once per iteration on the main thread: the frame boundary
// that pumps window events and samples the pad/keyboard for play mode (needs `entryhook 0x7101fdcde4` in recomp/games/pkp/main.sites).
constexpr uint64_t kFrameUpdate = 0x1fdcde4;

// ---- the feature registry ----
// Every feature module, in install order. Their frame, game-frame, settings and status callbacks run in this order too; removal runs in
// reverse. Before any of them the registry adds its game-frame tick at layout::kFrameStep, so the game-frame callbacks run before play's
// frame-rate hook at that site, as they did when that hook called them. Where the order matters:
//   save_uuid           first: its install repairs the save before the game is loaded (pkp_save_uuid.h)
//   linkplay            before play: its frame callback (the one-time check of main) runs first in play mode's frame
//   mods                the Modtopia mods' frame tick (pkp_mods.h): first of the game-frame callbacks, so mods see each frame before our
//                       features change anything in it
//   play, kbm           play's status fields come first in the status line, kbm's right after them
//   kbm, movie_test     game-frame callbacks, kbm's first (movie_test and taa_probe are test tools: in the table only with NXRT_TESTTOOLS, which
//                       a release package builds without; the registry has no stub for them)
//   shoulder_camera    a port() row on purpose: the repo owner chose no on/off switch, since its two distances at 0 are the game's own camera
//   aim_debug           camera aim's debug view: a port() row (a debugging key, not a mod); aim reports to it through its calls
//   title, sort, craft  key guide clients (pkp_key_guide.h): they add themselves at install, and the key guide's callbacks run in that
//                       order
//   key_guide           after its clients: its install registers the hooks they need
//   options             after title: title's frame callback runs first
//   ui_backdrop         after menu: it registers a client of the menu's pane draw hooks (pkp_menu.h) and holds no hook of its own
//   mods_notice         any position: it only logs which gameplay mods are on, at install
//   item_edit           no hooks and no callbacks: the Options screen's Items tab calls it when it opens and commits (pkp_item_edit.h)
// No other two features hook the same address.
//
// Each row says what the feature is to the player. mod(): a gameplay mod, named with its switches (the gameplay() rows of the [pkp]
// table, pkp_settings_table.h), which the launcher shows in Settings > Gameplay on its own. port(): part of playing the game on a PC
// (display, input, menus, Link Play, test tools), whose settings are not mod switches. Checked below at compile time: every switch of
// the table belongs to exactly one mod row, and a mod row names only switches. So a new mod cannot ship without a launcher switch.
struct Entry {
    const Feature* feature;
    settings::Key switches[2];   // a mod's switches (Key::Count: none); a port row has none
};
constexpr Entry mod(const Feature* f, settings::Key a, settings::Key b = settings::Key::Count) { return {f, {a, b}}; }
constexpr Entry port(const Feature* f) { return {f, {settings::Key::Count, settings::Key::Count}}; }

using enum settings::Key;
constexpr Entry kEntries[] = {
    port(&kSaveUuidFeature), port(&kLinkplayFeature), port(&kModsFeature), port(&kPlayFeature), port(&kKbmFeature), port(&kModsNoticeFeature),
#if NXRT_TESTTOOLS
    port(&kMovieTestFeature),
#endif
    port(&kMenuFeature), port(&kUiBackdropFeature), port(&kQualityFeature),
    port(&kTaaFeature),
#if NXRT_TESTTOOLS
    port(&kTaaProbeFeature),
#endif
    port(&kShoulderCameraFeature),
    mod(&kJumpFeature, JumpMomentum), mod(&kAimFeature, CameraAim), port(&kAimDebugFeature), mod(&kTitleFeature, TitleQuit), mod(&kSortFeature, InventorySort),
    mod(&kCraftFeature, CraftSearch), mod(&kItemEditFeature, ItemEditor), mod(&kPokemonCapFeature, PokemonCap),
    port(&key_guide::kKeyGuideFeature),
    port(&kOptionsFeature),
};
constexpr size_t kFeatureCount = std::size(kEntries);

constexpr bool every_switch_claimed_once() {
    for (size_t k = 0; k < static_cast<size_t>(settings::Key::Count); ++k) {
        const auto key = static_cast<settings::Key>(k);
        int owners = 0;
        for (const Entry& e : kEntries)
            for (settings::Key sw : e.switches) {
                if (sw == settings::Key::Count) continue;
                if (!settings::table::is_switch(sw)) return false;   // a mod row names a key that is not a gameplay() switch
                if (sw == key) ++owners;
            }
        if (settings::table::is_switch(key) && owners != 1) return false;
    }
    return true;
}
static_assert(every_switch_claimed_once(),
              "feature registry: every gameplay() switch of the [pkp] table belongs to exactly one mod() row, and a mod() row names only switches");

constexpr auto kFeatures = [] {
    std::array<const Feature*, kFeatureCount> a{};
    for (size_t i = 0; i < kFeatureCount; ++i) a[i] = kEntries[i].feature;
    return a;
}();

struct Registry {
    HookSet sets[kFeatureCount];   // each feature's hooks (pkp_hook_set.h)
    HookSet tick;                  // the game-frame tick
    bool render_installed = false, installed = false;
};
Registry& reg() {
    static Registry r;
    return r;
}

// The game thread, once a frame: every feature's game-frame callback (before the frame-rate hook at the same site, pkp_frame_rate.cpp).
int hook_game_frame(::Ctx* c, uint64_t, void*) {
    for (const Feature* f : kFeatures)
        if (f->game_frame) f->game_frame(c);
    return 0;
}

void install_render_all(uint64_t base, const play::DisplaySettings& d, int window_w, int window_h) {
    Registry& r = reg();
    if (r.render_installed) return;
    for (size_t i = 0; i < kFeatureCount; ++i)
        if (kFeatures[i]->install_render) kFeatures[i]->install_render(base, d, window_w, window_h, r.sets[i]);
    r.render_installed = true;
}
void install_all(uint64_t base, play::GameHost* host, const play::Settings& s) {
    Registry& r = reg();
    if (r.installed) return;
    r.tick.add_mid(base + layout::kFrameStep, &hook_game_frame);
    for (size_t i = 0; i < kFeatureCount; ++i)
        if (kFeatures[i]->install) kFeatures[i]->install(FeatureInstall{base, host, s, r.sets[i]});
    r.installed = true;
}
void frame_all(bool in_game, double step_seconds, play::GameFrame* out) {
    for (const Feature* f : kFeatures)
        if (f->frame) f->frame(in_game, step_seconds, out);
}
void settings_changed_all(const play::Settings& s) {
    for (const Feature* f : kFeatures)
        if (f->settings_changed) f->settings_changed(s);
}
void remove_all() {
    Registry& r = reg();
    r.tick.clear();
    for (size_t i = kFeatureCount; i-- > 0;) {
        r.sets[i].clear();
        if (kFeatures[i]->remove) kFeatures[i]->remove();
    }
    r.installed = r.render_installed = false;
}
// The overlay's presenting thread: the features' layers over the picture, in table order.
bool layer_on(const Feature* f) { return f->overlay_active && f->overlay_draw && f->overlay_active(); }
bool overlay_active_all() { return std::any_of(kFeatures.begin(), kFeatures.end(), layer_on); }
void overlay_draw_all(gfx::OverlayCanvas& c) {
    for (const Feature* f : kFeatures)
        if (layer_on(f)) f->overlay_draw(c);
}
void status_all(char* buf, size_t n) {
    if (!n) return;
    buf[0] = 0;
    for (const Feature* f : kFeatures) {
        if (!f->status_text) continue;
        const size_t k = std::strlen(buf);
        if (k + 1 < n) f->status_text(buf + k, n - k);
    }
}

const play::GamePlay& game_play() {
    static const play::GamePlay t = [] {
        play::GamePlay p;
        p.frame_update = kFrameUpdate;
        p.display_options = play::kDisplayOptQuality;   // the quality preset (pkp_quality.cpp)
        p.quality_preset = &quality_preset;
        p.install_render = &install_render_all;
        p.install = &install_all;
        p.remove = &remove_all;
        p.frame = &frame_all;
        p.settings_changed = &settings_changed_all;
        p.display_live = &play_display_live;
        p.display_hook_calls = &play_display_calls;
        p.status_text = &status_all;
        p.menu_frame = &menu_frame;
        p.menu_over_value = &menu_over_value;
        p.menu_active = &menu_active;
        p.menu_in_game = &menu_in_game;
        p.overlay_layer = {&overlay_active_all, &overlay_draw_all};
        return p;
    }();
    return t;
}

}  // namespace

const game::Info& full_info() {
    static const game::Info info = [] {
        game::Info g = core_info();
        g.play = &game_play();
        g.render = &render_info();
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::pkp

void nxrt_register_game_pkp() { nxrt::game::set_current(&nxrt::games::pkp::full_info()); }
