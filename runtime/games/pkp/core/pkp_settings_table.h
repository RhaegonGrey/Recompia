// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the table of [pkp] keys (pkp_settings.h). A header of constexpr data, so the feature registry (pkp_game.cpp) can check
// at compile time that every gameplay mod switch belongs to a feature and every mod feature names its switches. spec() and the doc rows
// are in pkp_settings_table.cpp (nxrt_game_pkp_data, which the docs generator reads without the runtime), the getters in
// pkp_settings.cpp. Defaults and ranges are the ones each module used before the table.
#pragma once
#include <cstddef>
#include <cstdint>

#include "core/pkp_settings.h"

namespace nxrt::games::pkp::settings {

namespace table {

using enum Key;
using enum Type;
using enum Applies;

constexpr Spec b(Key k, const char* name, bool def, const char* doc) { return {k, name, Bool, def ? 1.0 : 0.0, "", 0, 0, "", AtStart, doc}; }
constexpr Spec i(Key k, const char* name, int64_t def, int64_t lo, int64_t hi, const char* doc) {
    return {k, name, Int, static_cast<double>(def), "", static_cast<double>(lo), static_cast<double>(hi), "", AtStart, doc};
}
constexpr Spec d(Key k, const char* name, double def, double lo, double hi, const char* doc) { return {k, name, Double, def, "", lo, hi, "", AtStart, doc}; }
constexpr Spec s(Key k, const char* name, const char* def, const char* choices, const char* doc) { return {k, name, String, 0, def, 0, 0, choices, AtStart, doc}; }
// A setting read again while the game runs: a change takes effect at once (Applies::Live).
constexpr Spec live(Spec sp) {
    sp.applies = Live;
    return sp;
}
// Unset leaves another value in force (the getter's default is then never used): see Spec::optional.
constexpr Spec unset_by_default(Spec sp) {
    sp.optional = true;
    return sp;
}
// A gameplay mod's switch (Spec::gameplay): a bool, on by default, the player opts out. The launcher shows it in Settings > Gameplay.
constexpr Spec gameplay(Spec sp) {
    sp.gameplay = true;
    return sp;
}
// With gameplay(): a mod the repo owner made optional, off by default (the player opts in).
constexpr Spec opt_in(Spec sp) {
    sp.opt_in = true;
    return sp;
}
// With gameplay(): no launcher row (the ini still takes it).
constexpr Spec hidden(Spec sp) {
    sp.hidden = true;
    return sp;
}
// A setting of the mod switched by `parent`: in the gameplay group, shown under that switch in the launcher.
constexpr Spec under(Key parent, Spec sp) {
    sp.gameplay = true;
    sp.parent = parent;
    return sp;
}

// In Key order (checked below). Every key is read once, when its feature installs.
inline constexpr Spec kSpecs[] = {
    b(TallerFov, "taller_fov", true, "Screens taller than 16:9: the taller view through the camera's FOV and aspect (off: 16:9 with bars)."),
    b(RenderOverride, "render_override", true, "Render the scene at the chosen internal size instead of the game's 1920x1080 layout."),
    s(TargetFix, "target_fix", "screen", "off screen all", "Render targets made at a literal 1920x1080 follow the render size: screen-relative ones, all, or none."),
    b(SceneViewportFix, "scene_viewport_fix", true, "Keep the 3D scene's viewport whole when it renders below its targets' size."),
    b(FrameTimeFix, "frame_time_fix", true, "Above 60 fps, the game's 60 Hz steps run at the right speed."),
    b(EventHold, "event_hold", true, "With frame_time_fix: hold the game at 60 Hz steps while the camera cannot turn (events, loading)."),
    i(EventHoldMs, "event_hold_ms", 1500, 0, 10000, "How long the event hold stays after free play resumes, in ms."),
    b(MouseCamera, "mouse_camera", true, "Turn the follow camera with the raw mouse."),
    b(GameFrameLimit, "game_frame_limit", false, "Keep the game's own 60 Hz frame wait."),
    b(CameraEveryFrame, "camera_every_frame", true, "Place the camera every rendered frame instead of on the game's 120 Hz tick."),
    i(CameraGraceMs, "camera_grace_ms", 250, 0, 5000, "How long after the camera's turn branch last ran the mouse still turns the camera, in ms."),
    live(d(CameraShoulderSide, "camera_shoulder_side", 0.0, -2.0, 2.0,
           "Over-the-shoulder camera: how far the view is moved to the side of the character, in blocks (positive: to the right, 0: the game's own camera).")),
    live(d(CameraShoulderHeight, "camera_shoulder_height", 0.0, -1.0, 2.0,
           "Over-the-shoulder camera: how far the view is moved up, in blocks (positive: up, 0: the game's own camera).")),
    b(MenuMouse, "menu_mouse", true, "The mouse pointer in the game's menus."),
    i(MenuLog, "menu_log", 1, 0, 0, "Menu logging: 0 off, 1 what the pointer does (and the sort's and search's lists), 2 more detail, 3 the layout chain once a second."),
    b(MenuInGame, "menu_in_game", true, "The mouse pointer also works in menus opened during play."),
    s(MenuInGameIgnore, "menu_in_game_ignore", "", "", "Layouts (id/sub ...) never treated as an in-game menu."),
    s(MenuKeyGuides, "menu_key_guides", "881468973=btn_minus", "", "Key guide layouts a click sends a button for (id=action ...)."),   // gitleaks:allow (a layout id)
    b(KbmIcons, "kbm_icons", true, "Keyboard and mouse icons in the game's button prompts."),
    unset_by_default(s(QualityPreset, "quality_preset", "switch2", "switch2 high ultra steamdeck legion_go_s laptop desktop",
                       "When set, the graphics preset (wins over the F1 page's).")),
    d(FogStrength, "fog_strength", 1.0, 0.0, 1.5, "Multiplier on the height fog's amount."),
    d(LodDistance, "lod_distance", 1.0, 0.25, 8.0, "When set, multiplier on the distance where placed objects switch to their low-detail model."),
    b(QualityProbe, "quality_probe", false, "Log what the quality hooks see (diagnostics)."),
    i(GameTaa, "game_taa", 0, 0, 2, "Experimental: 1 turns on the engine's camera TAA jitter, 2 also its TemporalAA3 pass switch (0: the game's)."),
    b(LinkInteractEverywhere, "link_interact_everywhere", false, "Experimental: Link Play visitors can interact outside Palette Town."),
    b(LinkStoryCoop, "link_story_coop", false, "Experimental: in Link Play every player plays their own story, saved in their own save."),
    b(UuidRepair, "uuid_repair", true, "At start, give a save with the player UUID earlier builds shared a UUID of its own."),
    gameplay(b(JumpMomentum, "jump_momentum", true, "A running jump keeps the running speed in the air.")),
    d(JumpMomentumStrength, "jump_momentum_strength", 1.0, 0.0, 1.0, "With jump_momentum: how much of the extra running speed is kept."),
    gameplay(b(CameraAim, "camera_aim", true, "Abilities and tools aim where the camera looks instead of where the character faces.")),
    under(CameraAim, i(CameraAimReach, "camera_aim_reach", 0, 0, 4, "With camera_aim: extra cells of reach (0: the game's own range).")),
    live(b(CameraAimDebug, "camera_aim_debug", false, "Draw camera aim's ray, the cells it tested and the cell it picked (debugging).")),
    gameplay(b(InventorySort, "inventory_sort", true, "A click on Hold to Sort in the bag and the container screens sorts at once (the game's own sort, as holding X).")),
    under(InventorySort, b(InventoryAutosort, "inventory_autosort", false, "Sort the bag each time the bag or a container screen opens.")),
    opt_in(gameplay(b(CraftSearch, "craft_search", false, "A Search button in the crafting screen's key guide."))),
    opt_in(gameplay(b(ItemEditor, "item_editor", false, "Experimental: an Items tab in the game's Settings screen that changes the count of a stack in the bag."))),
    hidden(gameplay(b(TitleQuit, "title_quit", true, "A Quit Game key guide on the title screen."))),
    gameplay(b(PokemonCap, "pokemon_cap_50", true, "Experimental: raise the resident, object and active Pokemon limits beyond the game's 25 (pokemon_cap_limit).")),
    under(PokemonCap, i(PokemonCapLimit, "pokemon_cap_limit", 30, 25, 63,
                        "With pokemon_cap_50: choose 25 to 63 residents (30 default; 50 leaves 14 slots for visitors); the actor pool has 64 slots.")),
    b(OptionsMenu, "options_menu", true, "The PC settings as tabs and rows of the game's own Settings screen."),
    s(EventsFile, "events_file", "pokopia_events.tsv", "", "The Events tab's list: one event per line, start date, end date and name, separated by tabs."),
};
static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == static_cast<size_t>(Count), "one Spec per Key");

constexpr bool in_key_order() {
    for (size_t n = 0; n < static_cast<size_t>(Count); ++n)
        if (static_cast<size_t>(kSpecs[n].key) != n) return false;
    return true;
}
static_assert(in_key_order(), "kSpecs in Key order");

constexpr const Spec& at(Key k) { return kSpecs[static_cast<size_t>(k)]; }

// A mod's switch: a gameplay row that is not the setting of another switch.
constexpr bool is_switch(Key k) { return k < Count && at(k).gameplay && at(k).parent == Count; }

// The mod markers fit together: a switch is a bool whose default matches opt_in; opt_in and hidden only on a switch; a mod setting's
// parent is a switch (never itself a setting, never hidden: the launcher shows the setting under it).
constexpr bool mods_ok() {
    for (const Spec& sp : kSpecs) {
        if (!sp.gameplay) {
            if (sp.opt_in || sp.hidden || sp.parent != Count) return false;
        } else if (sp.parent == Count) {
            if (sp.type != Bool || (sp.opt_in && sp.def != 0)) return false;
        } else {
            if (sp.opt_in || sp.hidden || !is_switch(sp.parent) || at(sp.parent).hidden) return false;
        }
    }
    return true;
}
static_assert(mods_ok(), "[pkp] gameplay markers: a switch is a bool (opt_in: default off), a setting's parent is a shown switch");

// Release check (PKP_GAMEPLAY_DEFAULTS_CHECK, pkp_settings_table.cpp): every switch the repo owner did not make opt_in defaults on.
constexpr bool switches_default_on() {
    for (const Spec& sp : kSpecs)
        if (sp.gameplay && sp.parent == Count && !sp.opt_in && sp.def == 0) return false;
    return true;
}

}  // namespace table

}  // namespace nxrt::games::pkp::settings
