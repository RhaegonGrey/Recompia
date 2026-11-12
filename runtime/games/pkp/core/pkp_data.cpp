// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: who the game is, what its files are called, its factory controls.ini and its [pkp] keys (nxrt/game.h Info). Pure
// data, no runtime, so the factory settings can be read without linking the rest.
#include "core/pkp_game.h"

#include <iterator>

#include "core/pkp_settings.h"
#include "play_types.h"

namespace nxrt::games::pkp {

namespace {

// Factory keyboard and mouse bindings (play_types.h settings file). Button roles from the game's default pad layout
// (pokopia.wiki.fextralife.com/Controls): A interact, B run/back, X bag, Y pick up, R jump, ZR use move, ZL strafe, L set move,
// + main menu, - camera, D-pad follow/emote/swap move. Pads keep the console layout.
constexpr const char* kSettingsText = R"INI(
[meta]
defaults_rev = 5

[display]
render_scale = auto
fps_limit = auto

[bindings.all]
W = move_forward
S = move_back
A = move_left
D = move_right
Up = look_up
Down = look_down
Left = look_left
Right = look_right
E = btn_a
Return = btn_a
Keypad Enter = btn_a
Mouse1 = btn_zr
Mouse3 = btn_zl
Space = btn_r
Left Shift = btn_b
Backspace = btn_b
Tab = btn_x
F = btn_y
Q = btn_l
Left Ctrl = btn_lstick
V = btn_rstick
Escape = btn_plus
C = btn_minus
1 = dpad_up
2 = dpad_down
3 = dpad_left
4 = dpad_right

[bindings.play]
Mouse2 = mouse_mode

[bindings.menu]
Mouse3 = btn_b
WheelUp = dpad_up
WheelDown = dpad_down
)INI";

// Revision 2: the render scale follows the window (the NVN back end renders the targets larger) unless the player changed it.
// Revision 3: the frame limit follows the display's refresh rate (the game's own 60 Hz wait is off, pkp_play.cpp) unless the player changed it.
// Revision 4: in the menus the right button is B (back) and the wheel moves the selection (pkp_menu.cpp: hover and click).
// Revision 5: the middle mouse button is the Mouse mode key in play (the Joy-Con 2 mouse, pkp_play.cpp play_frame).
constexpr play::GameDefaultAddition kAdditions[] = {
    {5, play::Ctx::Play, "Mouse2", play::Act::MouseMode},
    {4, play::Ctx::Menu, "Mouse3", play::Act::BtnB},
    {4, play::Ctx::Menu, "WheelUp", play::Act::DpadUp},
    {4, play::Ctx::Menu, "WheelDown", play::Act::DpadDown},
};

constexpr play::GameDefaultChange kChanges[] = {
    {2, "display", "render_scale", "100", "auto"},
    {3, "display", "fps_limit", "60", "auto"},
};

const play::GameDefaults& game_defaults() {
    static const play::GameDefaults d = [] {
        play::GameDefaults g;
        g.settings_text = kSettingsText;
        g.defaults_rev = 5;
        g.additions = kAdditions;
        g.addition_count = sizeof kAdditions / sizeof kAdditions[0];
        g.changes = kChanges;
        g.change_count = sizeof kChanges / sizeof kChanges[0];
        return g;
    }();
    return d;
}

}  // namespace

const game::Info& data_info() {
    static const game::Info info = [] {
        game::Info g;
        g.id = "pkp";
        g.title = "Pokémon Pokopia";
        g.product = "Pokémon Pokopia PC";
        g.short_title = "Pokopia";
        g.program_id = 0x0400A3A01EE4C000ull;
        g.version = "2.0.1";
        g.main_base = 0x7100000000ull;
        // control.nacp is not in the dump yet: every language for now (bits of services::Language)
        g.languages = 0x1ffffu;
        g.platform = game::Platform::Switch2;
        g.exe_name = "pkp.exe";
        g.game_ini = "pkp.ini";
        g.shader_pack = "shader_cache.pkpshc";
        g.save_folder = "PokopiaRecompPC";
        // 2.0.1a (update 655360) merged over the base game: 36 files, 8 folders, 7,246,052,773 bytes in every complete dump seen in the issue
        // logs (#44, #47, #49, #69, #76, #95, #119). Keep in step with game-build/release_b/dump_hashes.json "romfs".
        g.romfs_min_files = 36;
        g.romfs_min_bytes = 7246052773ull;
        g.play_defaults = &game_defaults();
        g.settings_doc = &settings::doc_rows;
        // Modtopia mods (nxrt/mods.h): the default folders of game::ModHost (the launcher and Set up use the same ones), shown to mods as
        // "Recompia <version>".
        // The shader files: the linked-data archive (GLSLC outputs stored as is) and the RDB data files with G2S containers or system
        // resources holding GLSLC outputs (tools/shaders/docs/AMPERE.md section 1), checked against 2.0.1a's RomFS. Based on pokopia-native's
        // list (MIT, host/mods.c), plus 0x38f972af.fdata (most of the game's G2S containers) and without the archive's index LI_C0_R.IDX (no
        // shaders in it).
        static constexpr const char* kShaderFiles[] = {
            "LD_C0_R.BIN",
            "motor_asset_re/KIDS_PLATFORM_OUNCE/0x05d525ae.fdata",
            "motor_asset_re/KIDS_PLATFORM_OUNCE/0x2943e4da.fdata",
            "motor_asset_re/KIDS_PLATFORM_OUNCE/0x38f972af.fdata",
            "motor_asset_re/KIDS_PLATFORM_OUNCE/0x52482d03.fdata",
        };
        static const game::ModHost kModHost{.host = "Recompia", .shader_files = kShaderFiles,
                                            .shader_file_count = static_cast<uint32_t>(std::size(kShaderFiles))};
        g.mods = &kModHost;
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::pkp

void nxrt_register_game_pkp_data() { nxrt::game::set_current(&nxrt::games::pkp::data_info()); }
