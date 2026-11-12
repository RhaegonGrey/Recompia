// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options: the key capture behind capture rows (Use::Capture, pkp_options_model.h). Moving such a row starts play mode's rebind
// capture (GameHost::begin_capture); the next key, mouse button or wheel notch goes to the row's CaptureClient at once (Escape cancels;
// F1, which opens the settings page, is refused). One capture at a time. The row is a slider in the middle of a wide range (0..2 * kMid),
// so every left/right press or click moves it and starts a capture (with 0..1 it stuck at the end after one change).
// begin() runs in the screen's store hook (game thread), frame() on play mode's main thread, where the client's take() runs too.
#pragma once
#include <cstddef>
#include <string>

#include "play_game.h"
#include "play_types.h"

namespace nxrt::games::pkp::options_detail {

class CaptureClient {
public:
    virtual ~CaptureClient() = default;
    virtual const char* name() const = 0;                 // the row, for the log
    virtual std::string bound() const = 0;                // the key in force, as the row shows it
    virtual bool take(const play::InputId& in) = 0;       // the key picked; false: refused, the key in force stays
};

namespace capture {

constexpr int kMid = 100;   // a capture row's position whenever the screen opens

void set_host(play::GameHost* host);
void begin(CaptureClient& c);                                   // the row moved: wait for the key (nothing while a capture runs)
const char* text(const CaptureClient& c, char* out, size_t n);  // the row's text: "Press the new key", the key just picked, or bound()
bool take_redraw(const CaptureClient& c);                       // true once after the capture of c started or ended (text changed)
void frame(bool screen_open);                                   // main thread: delivers the key; a closed screen cancels the wait

}  // namespace capture
}  // namespace nxrt::games::pkp::options_detail
