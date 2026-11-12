// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: test automation sinks (scripted pointer and keys, virtual pad, screenshot) and the self-test's window onto play mode.
#include "play_state.h"

namespace nxrt::play {
using namespace detail;
namespace detail {

bool key_sink(const char* name, bool down) {
    InputId id;
    static float s_mx = 0, s_my = 0;     // the last scripted pointer position: a scripted click lands there, as a real one would
    // Scripted pointer: `keydown mpos <x> <y>` puts the pointer at window pixel (x, y) the way a real motion event does (S28 tests).
    if (name && std::strncmp(name, "mpos ", 5) == 0) {
        if (!down) return true;
        float px = 0, py = 0;
        if (std::sscanf(name + 5, "%f %f", &px, &py) != 2) return false;
        s_mx = px;
        s_my = py;
        input::Event m;
        m.type = input::EventType::MouseMotion;
        m.u.motion.x = px;
        m.u.motion.y = py;
        input::queue_push(m);
        return true;
    }
    // Scripted virtual gamepad (S35 tests): `keydown vpad attach xbox`, `keydown vpad axis ry 0.8`, `keydown vpad button south 1`, `keydown vpad gyro 0 1 0` ...
    if (name && std::strncmp(name, "vpad ", 5) == 0) return !down || vpad_command(name + 5);
    if (!name || !parse_input(name, &id)) return false;
    input::Event e;
    e.timestamp_ns = 0;
    switch (id.kind) {
        case InKind::Key:
            e.type = down ? input::EventType::KeyDown : input::EventType::KeyUp;
            e.u.key.scancode = id.code;
            e.u.key.keycode = static_cast<int32_t>(SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(id.code), SDL_KMOD_NONE, false));
            break;
        case InKind::Mouse:
            e.type = down ? input::EventType::MouseButtonDown : input::EventType::MouseButtonUp;
            e.u.button.button = static_cast<uint8_t>(id.code);
            e.u.button.clicks = 1;
            e.u.button.x = s_mx;
            e.u.button.y = s_my;
            break;
        case InKind::Wheel:
            if (!down) return true;      // a notch has no release
            e.type = input::EventType::MouseWheel;
            e.u.wheel.dy = static_cast<float>(id.code);
            break;
        default: return false;
    }
    input::queue_push(e);
    return true;
}

// The screenshot provider (nxrt/screenshot.h; @shot in a script, a mod's screenshot): the renderer writes the next presented frame as a PNG.
bool screenshot_provider(const char* path, bool bare, void*) {
    G& st = g();
    if (!st.renderer || !path) return false;
    DeleteFileA(path);
    if (std::strstr(path, "nvntrace_")) nvn::backend_trace_next_frame();   // `@shot nvntrace_<name>`: also trace the frame that is captured
    // The caller is the game's thread at a frame boundary, and the capture happens at the render thread's next end of frame, which
    // needs the game's next frame: waiting here for the file held every shot for the 5 s timeout. Wait only for an earlier request to
    // be taken (one capture path at a time), then return; the file is written on a worker and the test runner waits for it at exit.
    // A mod's (bare) request comes from a game thread inside a hook or an event: it never waits, a busy slot answers false at once.
    const int tries = bare ? 1 : 200;
    for (int i = 0; i < tries; ++i) {
        if (st.renderer->request_screenshot(path, bare)) return true;
        if (i + 1 < tries) Sleep(5);
    }
    return false;
}

// The self-test's window onto play mode: the same entry points the settings page uses.
class SelfTestHostImpl : public SelfTestHost {
public:
    bool set(const std::string& key, const std::string& value) override {
        G& st = g();
        Settings s = ui_host().settings();
        const double d = std::atof(value.c_str());
        if (key == "vsync") s.display.vsync = d != 0;
        else if (key == "fps_cap" || key == "fps_limit") s.display.fps_cap = static_cast<int>(d);
        else if (key == "window_mode") s.display.window_mode = value;
        else if (key == "yaw_source") s.mouse.yaw_rad = rad_per_count(d);
        else if (key == "pitch_source") s.mouse.pitch_rad = rad_per_count(d);
        else if (key == "invert_y") s.mouse.invert_y = d != 0;
        else if (key == "invert_x") s.mouse.invert_x = d != 0;
        else if (key == "fps") s.fps.show = d != 0;
        else if (key == "fps_in_shots") s.fps.in_screenshots = d != 0;
        else if (key == "fps_corner") {
            if (!parse_corner(value, &s.fps.corner)) return false;
        } else if (key == "fps_detail") s.fps.detail = std::clamp(static_cast<int>(d), 0, 2);
        else if (key == "prompt_style") {
            if (!parse_prompt_style(value, &s.prompts.style)) return false;
        } else return false;
        ui_host().apply(s);
        (void)st;
        return true;
    }
    std::string present_mode() override { G& st = g(); return st.renderer ? gfx::present_mode_name(st.renderer->present_mode()) : ""; }
    std::string settings_text() override {
        G& st = g();
        std::string text;
        if (FILE* f = std::fopen(st.path.c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        return text;
    }
};

SelfTestHost* selftest_host() {
    static SelfTestHostImpl h;
    return &h;
}

}  // namespace detail
}  // namespace nxrt::play
