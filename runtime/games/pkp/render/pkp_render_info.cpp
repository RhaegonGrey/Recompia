// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: what the graphics back end may know about its rendering (nxrt/game_render.h, game::Info::render).
#include <volk.h>

#include "nxrt/game_render.h"
#include "core/pkp_game.h"

namespace nxrt::games::pkp {
namespace {

// [nvn] readback_defer (#276): the scene-colour probes, two 30x16 RGBA8 sRGB render targets (one level, one layer; ids 286 and 287 in our
// logs) a draw pass reduces the scene image into. Once a second a timer (0x71010f5380) reads one back (ReadTexels then InvalidateTexels,
// no fence), averages 288 of its 480 texels on the CPU and feeds the mean into a colour the consumer smooths by 0.1 a frame: data one read
// period old only delays that tint by a second.
constexpr game::ReadbackShape kReadbackDefer = {VK_FORMAT_R8G8B8A8_SRGB, 30, 16, 1, 1, 1};

// The camera: the G-buffer pass (4+ colour targets, D24S8 depth, reversed-Z) binds its vertex shaders' block c3 at NVN vertex uniform
// slot 0; floats 0..15 are the view-projection, floats 52..67 the view matrix (272 bytes in all). The game's own TAA jitter (0x7102574a18,
// written into a view object's projection at +0xc0, pkp_taa.cpp) never reaches this block (its view-projection stays unjittered with the
// enable forced), so the runtime adds its own. Camera cut: the player's step is about 350 world units a second walking and 450 to 550
// running (pkp_jump.cpp), so the camera that follows it moves at least that fast; a cut is a jump beyond 3000 units a second (and at
// least 20 units). Measure the real camera speed before tightening it.
constexpr game::CameraBlock kCamera = {0, 0, 4, 0, 52, 20.0, 3.0};

// The tone map. The composite pass is a LUT tone map (KTGL's cbComposite): coordinate = 0.0735 * log2(5.5556 * x + 0.048) + 0.386 into a
// 32^3 grading volume; its shader is about 24.5k SPIR-V words, so that entry has no size limit. The curve tone map (two 3D grading
// volumes, small shaders) is tried first, as it always was.
constexpr game::ToneMapMatch kToneMaps[] = {
    {game::ToneMapKind::Curve, 16384, 0.0f, 0.0f},
    {game::ToneMapKind::LutShaper, 0, 5.55555582f, 0.0479959995f},
};

// The loop guards, pokopia-native's (1.4.0) in this translator's shaders. Their 634565ab (D-1's hang on an RTX 3050 Laptop) is two shaders
// here, 3a718dcc49952e74 and dc36e3636f3c2221: a loop to c3 word 0 over 5-texel items of cp_t_cb0_10. Their 560a0262 (D-11), the
// clustered light lists, is four variants here: 0f9696499923d6d3, 95a73c014ee1eb95, 99a5312bd3dbfc27 and b81c48ff486acef4, each with
// loops to c3 words 12 and 13 (compared as unsigned) over cp_t_cb0_10 and cp_t_cb0_12, one texel an item. An RTX 3060 log that fell to
// 3 fps after 8 minutes (#309) loads 95a73c01 and b81c48ff. Their third (2cb5df5c, a loop bound held in a variable) has no match found
// here yet.
constexpr game::LoopGuard kLoopGuards[] = {
    {"3a718dcc49952e74", "cp_c3", 0, "cp_t_cb0_10", 5},
    {"dc36e3636f3c2221", "cp_c3", 0, "cp_t_cb0_10", 5},
    {"0f9696499923d6d3", "cp_c3", 12, "cp_t_cb0_10", 1},
    {"0f9696499923d6d3", "cp_c3", 13, "cp_t_cb0_12", 1},
    {"95a73c014ee1eb95", "cp_c3", 12, "cp_t_cb0_10", 1},
    {"95a73c014ee1eb95", "cp_c3", 13, "cp_t_cb0_12", 1},
    {"99a5312bd3dbfc27", "cp_c3", 12, "cp_t_cb0_10", 1},
    {"99a5312bd3dbfc27", "cp_c3", 13, "cp_t_cb0_12", 1},
    {"b81c48ff486acef4", "cp_c3", 12, "cp_t_cb0_10", 1},
    {"b81c48ff486acef4", "cp_c3", 13, "cp_t_cb0_12", 1},
};

// The 2D after the picture (#572): the composite (the LUT tone map above) is the last full-screen pass into the window texture; the HUD,
// menus, text and the world markers (speech bubbles, "This way!") are drawn after it into the same texture as vertex-input draws, the fades
// as full-screen draws without vertex inputs. The camera menu and the album draw the composite into a corner of it for their blurred
// background, also without vertex inputs. Based on pokopia-native's (MIT) in-game findings for the same build (its wide view and
// anti-aliasing notes: an effect between the composite and the HUD leaves the HUD sharp, a squeeze of the later draws moves only the 2D).
// The scene stretch (#571). With [upscale] scene_scale below 100 the scene passes, the bloom, the composite (tone map) and the colour pass
// after it all draw into the top-left part of the 1920x1080 targets (scissor = the part, traced at 50%: 960x540). The enlargement comes after
// them: the game's copy shader (vs 821538a4, fs c4664b38) draws a full-screen quad into a 1920x1080 RGBA8 sRGB target from a 1920x1080
// RGBA8 sRGB input at texture coordinates 0..0.4995 x 0..0.4991, one bilinear tap (sampler mag linear). The same shader shrinks images
// earlier in the frame (into 80x45 and 960x540 targets); those are left alone by the target-size rule of game::SceneStretch.
constexpr game::SceneStretch kSceneStretch = {"c4664b38f6a87835"};

constexpr game::Render kRender = {&kReadbackDefer, &kCamera, kToneMaps, sizeof kToneMaps / sizeof kToneMaps[0],
                                  kLoopGuards, sizeof kLoopGuards / sizeof kLoopGuards[0], true, &kSceneStretch};

}  // namespace

const game::Render& render_info() { return kRender; }

}  // namespace nxrt::games::pkp
