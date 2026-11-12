// SPDX-License-Identifier: GPL-3.0-or-later
// Texture rectangle patch: writes new pixels into a rectangle of one of the game's own BC7 textures (an icon in a UI atlas), in the game's
// storage, and has the texture uploaded again. Unlike tex_override.h (whole small textures, answered at upload), the change is made once,
// by the caller, in the bytes the game reads; the caller keeps what was there to put it back.
// Implementation: src/gfx/nvn/nvn_tex_patch.cpp. Any thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nxrt::gfx {

struct GuestTexInfo {
    uint32_t id = 0;              // the runtime's texture id (logs)
    uint32_t width = 0, height = 0, levels = 0, layers = 0;
    bool bc7 = false;             // BPTC_UNORM or BPTC_UNORM_SRGB, block-linear, with storage
    bool srgb = false;
    uint32_t format = 0;          // the NVN format value (NVN_FORMAT_*), for logs and for picking the patcher
};

// True when `guest` (an address in the game's memory) holds the game's NVNtexture object of a live texture.
bool guest_texture_info(const void* guest, GuestTexInfo* out);

// Layer 0 of the BC7 texture behind the NVNtexture at `guest`, the w x h rectangle at (x, y) of level 0 (all multiples of 4, inside the
// texture) and the same rectangle on each smaller level while it stays a whole number of blocks (x, y, w, h multiples of 4 << level):
//   texture_patch_bc7 writes `rgba` there (w * h straight-alpha RGBA8, rows in storage order; the smaller levels get it box-filtered),
//   encoded to BC7; `saved` (optional) receives the BC7 blocks that were there (level by level, row-major, 16 bytes each) for
//   texture_put_bc7_blocks (`size` bytes) to put back.
// Both mark the texture written by the CPU, so it is uploaded again before its next use. False: not such a texture or a bad rectangle.
bool texture_patch_bc7(const void* guest, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, std::vector<uint8_t>* saved);
bool texture_put_bc7_blocks(const void* guest, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* blocks, size_t size);

// A texture known by its content: a w x h (level 0) BC7 texture whose blocks in the rectangle (x, y, w, h) of level 0 (multiples of 4) hash
// to `hash` (FNV-1a 64 over the rectangle's 16-byte blocks, row by row, as untiled). For a texture the game loads from its files (no pointer
// to its NVNtexture at hand), e.g. the title screen's "PRESS (A) BUTTON" art.
struct Bc7Match {
    uint32_t width = 0, height = 0;
    uint32_t x = 0, y = 0, w = 0, h = 0;
    uint64_t hash = 0;
};
// Every live texture that matches gets `rgba` (m.w x m.h straight-alpha RGBA8) written over that rectangle (as texture_patch_bc7; once
// written it no longer matches). Only textures the game has drawn with are looked at (their data is in). `checked` (optional): ids of
// textures already looked at, skipped, and the ones looked at now are added.
// Returns how many were written. Reads the storage of every texture of that size and format not checked before: call it now and then.
int texture_patch_bc7_matching(const Bc7Match& m, const uint8_t* rgba, std::vector<uint32_t>* checked);

// How many textures are registered now (cheap): a caller that scans with texture_patch_bc7_matching can tell that nothing was added or
// removed since its last scan.
size_t texture_registry_count();

// The BC7 blocks of a whole level (layer 0, row-major, 16 bytes each) and its size in texels (multiples of 4). Debug dumps.
bool texture_read_bc7_level(const void* guest, uint32_t level, std::vector<uint8_t>* blocks, uint32_t* width, uint32_t* height);

}  // namespace nxrt::gfx
