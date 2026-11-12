// SPDX-License-Identifier: GPL-3.0-or-later
// The game's 2D left out of the picture while code asks for it (nxrt/ui_layer.h, #572): the textures the game presents, and the draw rule.
// Render thread only (the replay and the present both run there).
#include "nvn_replay_internal.h"
#include "nxrt/ui_layer.h"

namespace nxrt::nvn::be::gpu {

namespace {
// The last textures presented. The game presents its window textures in turn (two or three), so these are exactly those within a few
// frames. Held by reference so a slot never names a texture object that was freed and made again at the same address.
struct Presented {
    static constexpr size_t kSlots = 4;
    Ref<Texture> tex[kSlots];
    size_t next = 0;
};
Presented g_presented;
}  // namespace

void replay_note_presented(Texture* t) {
    if (!t) return;
    for (const Ref<Texture>& r : g_presented.tex)
        if (r.get() == t) return;
    g_presented.tex[g_presented.next++ % Presented::kSlots] = Ref<Texture>::share(t);
}

namespace replay {

bool ui_skip_draw(const ShaderInfo* vs) {
    if (!ui_layer::hidden()) return false;
    if (!vs || vs->refl.vertex_inputs().empty() || S.ncolor == 0 || !S.color_tex[0] || g_xfb.active) return false;
    Texture* target = S.color_tex[0].get();
    for (const Ref<Texture>& r : g_presented.tex)
        if (shares_image(target, r.get())) return true;
    return false;
}

void ui_shutdown() { g_presented = Presented(); }

}  // namespace replay
}  // namespace nxrt::nvn::be::gpu
