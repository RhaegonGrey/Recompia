// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options: the key capture of capture rows (pkp_options_capture.h).
#include "ui/pkp_options_capture.h"

#include <cstdio>

#include "nxrt/log.h"
#include "ui/pkp_options_internal.h"

namespace nxrt::games::pkp::options_detail::capture {
namespace {

struct State {
    play::GameHost* host = nullptr;
    CaptureClient* client = nullptr;   // the row of the last capture
    bool waiting = false;              // waiting for the key
    bool redraw = false;               // the row's text changed outside a value change
    std::string shown;                 // the key just picked, until the client's bound() shows it (play::apply_settings lands next frame)
} g;

}  // namespace

void set_host(play::GameHost* host) { g.host = host; }

void begin(CaptureClient& c) {
    if (!g.host || g.waiting) return;
    g.host->begin_capture();
    if (g.client != &c) g.shown.clear();
    g.client = &c;
    g.waiting = true;
    g.redraw = true;
    NXLOG_I(kTag, "options: %s: waiting for a key", c.name());
}

const char* text(const CaptureClient& c, char* out, size_t n) {
    if (g.waiting && g.client == &c) return "Press the new key (Esc: cancel)";
    std::snprintf(out, n, "%s", g.client == &c && !g.shown.empty() ? g.shown.c_str() : c.bound().c_str());
    return out;
}

bool take_redraw(const CaptureClient& c) {
    if (!g.redraw || g.client != &c) return false;
    g.redraw = false;
    return true;
}

void frame(bool screen_open) {
    if (!g.shown.empty() && !g.waiting && g.client && g.client->bound() == g.shown) g.shown.clear();
    if (!g.waiting || !g.host) return;
    play::InputId in;
    if (g.host->capture_result(&in)) {
        if (g.host->reserved_key(in)) {              // F1 opens the settings page: keep the old key
            NXLOG_I(kTag, "options: %s: %s opens the settings page, the key stays", g.client->name(), play::input_name(in).c_str());
            g.waiting = false;
            g.redraw = true;
            return;
        }
        if (g.client->take(in)) {
            g.shown = play::input_name(in);
            NXLOG_I(kTag, "options: %s = %s", g.client->name(), g.shown.c_str());
        } else {
            NXLOG_I(kTag, "options: %s: %s cannot be used here, the key stays", g.client->name(), play::input_name(in).c_str());
        }
    } else if (g.host->capturing()) {
        if (screen_open) return;
        g.host->cancel_capture();                    // the screen closed while waiting
    }
    g.waiting = false;
    g.redraw = true;
}

}  // namespace nxrt::games::pkp::options_detail::capture
