// SPDX-License-Identifier: GPL-3.0-or-later
// Short messages over the game's picture: the overlay draws the ones that are up (overlay_hud.cpp). Each comes from a source (a mod's id, a
// part of the runtime) and a newer one from the same source replaces its earlier one. Its time runs from the first frame that shows it, so a
// message posted while the game starts up is still seen. A message may carry a small picture, drawn left of its text. Any thread.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nxrt::notices {

// A message's picture: straight (not premultiplied) RGBA8, rows top down, at most kMaxImageSide a side (post scales a larger one down).
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};
inline constexpr int kMaxImageSide = 256;   // what one message's picture may cost in video memory: 256 KiB

// rgba: w x h pixels as Image has them, copied; null for text alone. Empty text and no picture, or seconds <= 0: takes the source's message away.
void post(const std::string& source, const std::string& text, double seconds, const uint8_t* rgba = nullptr, int w = 0, int h = 0);

struct Notice {
    std::string source, text;
    std::shared_ptr<const Image> image;   // null: text alone. One object per post: the overlay keys its texture on it
};
// The messages to draw now, oldest first, at steady-clock time now_s (seconds); expired ones are dropped. The overlay's draw calls it.
std::vector<Notice> shown(double now_s);
bool any();   // a message is up or waiting to be shown (cheap: the overlay asks every frame)

}  // namespace nxrt::notices
