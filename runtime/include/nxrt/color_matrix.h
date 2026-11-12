// SPDX-License-Identifier: GPL-3.0-or-later
// A colour matrix over the final picture, set by code that has no renderer to call (the mods host: Modtopia's color_matrix) and read by the
// output pass once a frame (gfx/extras/output_pass.h). 3x4, row-major: r' = m[0] r + m[1] g + m[2] b + m[3], g' = m[4..7], b' = m[8..11],
// on the sRGB-encoded picture in 0..1, after the game's own image and any upscaling. None set: the picture is not touched. Any thread.
#pragma once

namespace nxrt::color_matrix {

void set(const float* m12);   // null: none
bool get(float out[12]);      // false (out untouched): none is set

}  // namespace nxrt::color_matrix
