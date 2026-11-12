// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode settings page: the Display tab (window, frame rate, resolution, upscaling, low latency).
#include "play_ui_page.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "nxrt/audio_sink.h"
#include "nxrt/extras.h"
#include "extras/temporal.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/host_device.h"
#include "nxrt/input_source.h"
#include "nxrt/lowlat.h"
#include "nxrt/overlay.h"
#include "nxrt/platform.h"
#include "play_game.h"

namespace nxrt::play {
namespace ui {

constexpr int kWindowModeCount = 3;
constexpr const char* kWindowModeNames[kWindowModeCount] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
constexpr const char* kWindowModeKeys[kWindowModeCount] = {"windowed", "borderless", "exclusive"};
constexpr int kSizePresets[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
constexpr int kSizePresetCount = static_cast<int>(sizeof kSizePresets / sizeof kSizePresets[0]);
constexpr int kFpsChoices[] = {-1, 0, 30, 40, 45, 60, 72, 90, 120, 144, 165, 240, 360};   // -1 = the refresh rate

// ---- Display ------------------------------------------------------------------------------------------------------------------------

int window_mode_index(const std::string& mode) {
    for (int i = 0; i < kWindowModeCount; ++i)
        if (mode == kWindowModeKeys[i]) return i;
    return 0;
}

void fps_label(int fps, char* buf, std::size_t n) {
    if (fps < 0) std::snprintf(buf, n, "Refresh rate");
    else if (fps == 0) std::snprintf(buf, n, "Unlimited");
    else std::snprintf(buf, n, "%d fps", fps);
}

// The frame rate counter. Its controls act on the host at once, not on the working copy: the game's hotkey flips `show` too, so the
// page reads the host's value every frame and never writes a stale copy. These controls do not mark the page as changed.
constexpr const char* kCornerNames[] = {"Top left", "Top right", "Bottom left", "Bottom right"};   // in Corner order
constexpr int kCornerCount = static_cast<int>(sizeof kCornerNames / sizeof kCornerNames[0]);
constexpr const char* kFpsDetailNames[] = {"Frame rate", "Frame rate and frame time", "Plus 1% low and worst frame"};
constexpr int kFpsDetailCount = static_cast<int>(sizeof kFpsDetailNames / sizeof kFpsDetailNames[0]);

void set_fps_live(UiHost& host, const FpsSettings& f) {
    Settings cur = host.settings();
    cur.fps = f;
    host.apply(cur);
}

void fps_group(UiHost& host, Page& p) {
    const FpsSettings fps = host.settings().fps;
    ImGui::SeparatorText("Frame rate counter");

    bool show = fps.show;
    if (ImGui::Checkbox("Show frame rate counter", &show)) {
        FpsSettings f = fps;
        f.show = show;
        set_fps_live(host, f);
    }

    const int corner = static_cast<int>(fps.corner);
    if (ImGui::BeginCombo("Corner##fps_corner", kCornerNames[corner])) {
        for (int i = 0; i < kCornerCount; ++i) {
            if (ImGui::Selectable(kCornerNames[i], i == corner) && i != corner) {
                FpsSettings f = fps;
                f.corner = static_cast<Corner>(i);
                set_fps_live(host, f);
            }
        }
        ImGui::EndCombo();
    }

    const int detail = std::clamp(fps.detail, 0, kFpsDetailCount - 1);
    if (ImGui::BeginCombo("Detail##fps_detail", kFpsDetailNames[detail])) {
        for (int i = 0; i < kFpsDetailCount; ++i) {
            if (ImGui::Selectable(kFpsDetailNames[i], i == detail) && i != detail) {
                FpsSettings f = fps;
                f.detail = i;
                set_fps_live(host, f);
            }
        }
        ImGui::EndCombo();
    }

    bool shots = fps.in_screenshots;
    if (ImGui::Checkbox("Also draw it in screenshots", &shots)) {
        FpsSettings f = fps;
        f.in_screenshots = shots;
        set_fps_live(host, f);
    }

    const std::string key = input_name(fps.key);
    ImGui::Text("Hotkey: %s", key.empty() ? "?" : key.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Change##fps_key")) {
        stop_rebind(host, p);
        p.fps_capture = true;
        host.begin_capture();
    }
    if (p.fps_capture) {
        ImGui::SameLine();
        ImGui::TextDisabled("press a keyboard key...");
    }
}

// ---- Display page groups (HDR, upscaling, low latency) ----------------------------------------------------------------------------------

// HDR output, upscaling and low latency (nxrt/extras.h). Items that apply at the next start say so in their labels, as the Display tab does.
constexpr const char* kLatencyModeNames[] = {"Off", "Auto", "On", "Boost"};
constexpr const char* kUpscaleNames[] = {"Off", "FSR 1 (spatial)", "FSR 3", "DLSS", "XeSS", "NIS (spatial)", "DLAA", "Nearest (sharp pixels)"};   // indexed by extras::UpscaleMode
// The order the list shows them in. ponytail: NIS, FSR 3, DLSS, DLAA and XeSS are off until reworked (extras::upscale_mode_enabled, #232).
constexpr extras::UpscaleMode kUpscaleOrder[] = {extras::UpscaleMode::Off, extras::UpscaleMode::Fsr1, extras::UpscaleMode::Nearest};
constexpr const char* kAaNames[] = {"Default (none)", "SMAA Low", "SMAA Medium", "SMAA High", "SMAA Ultra"};   // indexed by extras::AaMode
constexpr const char* kUpscaleQualityNames[] = {"Native (AA only)", "Quality", "Balanced", "Performance", "Ultra performance", "Custom (scene resolution)"};

// Off and the spatial upscalers (FSR 1, NIS) always work. The temporal ones (src/gfx/extras/temporal.h) get depth, camera motion vectors made by
// the runtime and the game's projection jitter: FSR 3 when the FidelityFX DLLs are found, DLSS / DLAA when the Streamline DLLs and nvngx_dlss.dll are found (the GPU
// check runs when the renderer starts them). XeSS is not wired. A file that holds an unavailable one shows it as selected (the game runs as Off).
bool upscale_available(extras::UpscaleMode m) {
    switch (m) {
        case extras::UpscaleMode::Off:
        case extras::UpscaleMode::Fsr1:
        case extras::UpscaleMode::Nearest: return true;
        default: return false;   // switched off for now (#232)
    }
}
const char* temporal_why(extras::UpscaleMode m) {
    if (m == extras::UpscaleMode::Fsr3)
        return "Needs amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll (AMD FidelityFX SDK v2.4.0) in upscalers\\fsr3 next to the "
               "executable, or a path in [upscale] fsr_dll.";
    if (m == extras::UpscaleMode::Dlss || m == extras::UpscaleMode::Dlaa)
        return "Needs an NVIDIA RTX GPU and the Streamline DLLs (sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll) in upscalers\\streamline "
               "next to the executable, or their folder in [upscale] dlss_dll. The launcher's Game files page downloads nvngx_dlss.dll from NVIDIA.";
    return "Switched off for now: it will come back once it is reworked.";
}

void upscale_group(Page& p) {
    extras::UpscaleSettings& u = p.edit.extras.upscale;
    ImGui::SeparatorText("Upscaling");

    // Only the modes that work are offered. A file that holds another one shows it as the current entry, marked, until the player picks.
    const int mode = static_cast<int>(u.mode);
    std::string preview = kUpscaleNames[mode];
    if (!upscale_available(u.mode)) preview += " (not available)";
    const bool open = ImGui::BeginCombo("Upscaler##upscale_mode", preview.c_str());
    tip("How the game's picture is enlarged from a lower render or scene resolution. Off: smooth (bilinear). FSR 1 (AMD): filtered and sharpened, any GPU. "
        "Nearest: every pixel kept as a hard block, no blur. FSR 3, DLSS and NIS are switched off until they are reworked.");
    if (open) {
        for (extras::UpscaleMode m : kUpscaleOrder) {
            const int i = static_cast<int>(m);
            const bool ok = upscale_available(m);
            if (!ok) ImGui::BeginDisabled();
            if (ImGui::Selectable(kUpscaleNames[i], i == mode) && i != mode && ok) {
                u.mode = m;
                mark(p);
            }
            if (!ok) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", temporal_why(m));
            }
        }
        ImGui::EndCombo();
    }
    if ((u.mode == extras::UpscaleMode::Dlss || u.mode == extras::UpscaleMode::Dlaa) && !extras::temporal_dlss_started()) restart_badge();

    {   // anti-aliasing of the 3D scene: SMAA on the HDR scene before the tone map, so the HUD and the menus stay sharp. Applies with Apply and save.
        const int aa = static_cast<int>(u.aa);
        if (ImGui::BeginCombo("Anti-aliasing##upscale_aa", kAaNames[aa])) {
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(kAaNames[i], i == aa) && i != aa) {
                    u.aa = static_cast<extras::AaMode>(i);
                    quality_preset_edited(p.edit.display, QualityField::AntiAliasing);
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        tip("Smooths jagged edges of the 3D scene only (the HUD and the menus are not filtered). Default: as the game draws it, no anti-aliasing. SMAA: "
            "Low: the lightest, finds the obvious edges. Medium and High: more edges and longer edge search. Ultra: the finest and the most costly. "
            "Not hardware multisampling. Not combined with a scene resolution below 100% and an upscaler. Applies with Apply and save, no restart.");
    }

    if (u.mode == extras::UpscaleMode::Fsr3 || u.mode == extras::UpscaleMode::Dlss) {
        const int q = static_cast<int>(u.quality);
        if (ImGui::BeginCombo("Quality##upscale_quality", kUpscaleQualityNames[q])) {
            for (int i = 0; i < 6; ++i) {
                if (ImGui::Selectable(kUpscaleQualityNames[i], i == q) && i != q) {
                    u.quality = static_cast<extras::UpscaleQuality>(i);
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        tip("The scene's render size per side: Native 100%, Quality 67%, Balanced 59%, Performance 50%, Ultra performance 33%, Custom the scene resolution "
            "below. Takes effect where the game renders its scene below the output size; otherwise the upscaler runs as native anti-aliasing.");
    }

    if (slider("Sharpness", &u.sharpness, 0.0, 1.0, "%.2f")) mark(p);
    tip("How much FSR 1, NIS or FSR 3 sharpens the upscaled picture; 1 is the sharpest (DLSS has no sharpening).");
    if (ImGui::SliderInt("Scene resolution##upscale_scene", &u.scene_scale, 25, 100, "%d%%")) mark(p);
    tip("Renders the 3D scene at this percent of the output size per side; the game scales it up and keeps the HUD and the post effects at full size. 100 turns it off. "
        "FSR 3 and DLSS use their quality preset's percent instead, unless it is Custom. Applies live.");
    {   // what is in force now (#116): the percent the game module applies and who enlarges the scene
        const int pct = extras::upscale_scene_scale();
        float fx = 1.0f, fy = 1.0f;
        extras::scene_fraction(&fx, &fy);
        const extras::UpscaleMode m = extras::upscale_mode();
        if (pct >= 100) {
            ImGui::TextDisabled("Scene: full size%s", extras::upscale_temporal() ? " (the temporal upscaler runs as anti-aliasing)" : "");
        } else if (fx >= 1.0f && fy >= 1.0f) {
            ImGui::TextDisabled("Scene: %d%% per side, stretched by the game (dynamic resolution or split screen on, or not in the game yet)", pct);
        } else {
            const char* who = m == extras::UpscaleMode::Fsr1 ? "upscaled with FSR 1" : m == extras::UpscaleMode::Nearest ? "enlarged with nearest neighbour" : m == extras::UpscaleMode::Nis ? "upscaled with NIS"
                            : m == extras::UpscaleMode::Fsr3 ? "upscaled with FSR 3" : m == extras::UpscaleMode::Dlss ? "upscaled with DLSS"
                            : "stretched by the game (pick an upscaler to make it sharper)";
            ImGui::TextDisabled("Scene: %d%% per side, %s", pct, who);
        }
    }
    ImGui::TextDisabled("Or render everything below the window size with the render scale above; the upscaler then fills the rest.");
}

void latency_group(Page& p) {
    extras::LatencySettings& l = p.edit.extras.latency;
    ImGui::SeparatorText("Low latency");   // NVIDIA Reflex or AMD Anti-Lag 2 (nxrt/lowlat.h)

    const int mode = static_cast<int>(l.mode);
    const bool open = ImGui::BeginCombo("Mode##latency_mode", kLatencyModeNames[mode]);
    tip("Lowers the delay from input to picture with NVIDIA Reflex or AMD Anti-Lag 2, whichever the GPU has; Boost asks Reflex for its boost mode.");
    if (open) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(kLatencyModeNames[i], i == mode) && i != mode) {
                l.mode = static_cast<extras::LatencyMode>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();

    if (ImGui::SliderInt("Frames ahead##latency_ahead", &l.frames_ahead, -1, 3, l.frames_ahead < 0 ? "no limit" : "%d")) mark(p);
    tip("With the mode on, the game starts simulating a frame only when at most this many earlier frames are still on their way to the screen. 1 keeps the game's frame queue short; 0 is the shortest delay and costs frame rate.");
    restart_badge();
    if (ImGui::Checkbox("Log timings##latency_log", &l.log_timings)) mark(p);
    tip("Writes one line of latency timings to the log every second.");
    restart_badge();

    if (const extras::LowLatency* ll = extras::lowlat()) {
        const extras::LowLatency::Stats st = ll->stats();
        ImGui::Text("%s: enabled %s, input to present %.2f ms, driver input to GPU end %.2f ms, sleep %.2f ms", ll->backend_name(),
                    yes_no(st.enabled), st.input_to_present_ms, st.input_to_gpu_ms, st.sleep_ms_avg);
    } else {
        ImGui::TextDisabled("Not running: the renderer has not started its low-latency part yet, or the mode is off.");
    }
}

// The monitor list ("1: name (1920x1080 @ 144 Hz)"), read again every couple of seconds while the page is open.
void refresh_monitors(Page& p) {
    const double now = ImGui::GetTime();
    if (p.monitors_at >= 0.0 && now - p.monitors_at < 2.0) return;
    p.monitors_at = now;
    p.monitors.clear();
    const int n = platform::display_count();
    for (int i = 1; i <= n; ++i) {
        char buf[192];
        if (platform::display_name(i, buf, sizeof buf)) p.monitors.emplace_back(buf);
    }
}

void window_group(const LiveStatus& live, Page& p) {
    DisplaySettings& d = p.edit.display;
    ImGui::SeparatorText("Window");
    const int mode = window_mode_index(d.window_mode);
    if (ImGui::BeginCombo("Window mode", kWindowModeNames[mode])) {
        for (int i = 0; i < kWindowModeCount; ++i) {
            if (ImGui::Selectable(kWindowModeNames[i], i == mode) && i != mode) {
                d.window_mode = kWindowModeKeys[i];
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    tip("Borderless fullscreen keeps the desktop resolution and switches instantly. Exclusive fullscreen takes over the display. Alt+Enter flips between windowed and fullscreen in the game.");

    refresh_monitors(p);
    {
        char fallback[48];
        const char* preview = "Automatic (where the window is)";
        if (d.monitor > 0) {
            if (d.monitor <= static_cast<int>(p.monitors.size())) {
                preview = p.monitors[static_cast<std::size_t>(d.monitor - 1)].c_str();
            } else {
                std::snprintf(fallback, sizeof fallback, "%d: not connected", d.monitor);
                preview = fallback;
            }
        }
        if (ImGui::BeginCombo("Monitor", preview)) {
            if (ImGui::Selectable("Automatic (where the window is)", d.monitor <= 0) && d.monitor > 0) {
                d.monitor = 0;
                mark(p);
            }
            for (std::size_t i = 0; i < p.monitors.size(); ++i) {
                const int number = static_cast<int>(i) + 1;
                if (ImGui::Selectable(p.monitors[i].c_str(), d.monitor == number) && d.monitor != number) {
                    d.monitor = number;
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        tip("The display the window moves to. Fullscreen modes use this display.");
    }

    int preset = -1;
    for (int i = 0; i < kSizePresetCount; ++i)
        if (kSizePresets[i][0] == d.width && kSizePresets[i][1] == d.height) preset = i;
    char size_label[48];
    std::snprintf(size_label, sizeof size_label, "%d x %d%s", d.width, d.height, preset < 0 ? " (custom)" : "");
    // Only the windowed mode's size; the picture's own size is under Resolution. Greyed out in the fullscreen modes (#117).
    const bool windowed = window_mode_index(d.window_mode) == window_mode_index("windowed");
    ImGui::BeginDisabled(!windowed);
    if (ImGui::BeginCombo("Windowed size", size_label)) {
        for (int i = 0; i < kSizePresetCount; ++i) {
            char name[32];
            std::snprintf(name, sizeof name, "%d x %d", kSizePresets[i][0], kSizePresets[i][1]);
            if (ImGui::Selectable(name, i == preset) && i != preset) {
                d.width = kSizePresets[i][0];
                d.height = kSizePresets[i][1];
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", windowed ? "The size of the game window in windowed mode. It is not the size the game renders at: see Resolution below."
                                         : "Only used in windowed mode: the size the window returns to. Fullscreen shows the display's size; the size "
                                           "the game renders at is under Resolution below.");
    if (int_field("Width##win_w", &d.width, 320, 16384, 1, 100)) mark(p);
    if (int_field("Height##win_h", &d.height, 320, 16384, 1, 100)) mark(p);
    if (windowed && live.width > 0 && (live.width != d.width || live.height != d.height)) {
        if (ImGui::Button("Use the current window size")) {
            d.width = live.width;
            d.height = live.height;
            mark(p);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("now %d x %d", live.width, live.height);
    }
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Letterbox instead of stretching", &d.keep_aspect)) mark(p);
}

void framerate_group(UiHost& host, const LiveStatus& live, Page& p) {
    DisplaySettings& d = p.edit.display;
    ImGui::SeparatorText("Frame rate");
    if (ImGui::Checkbox("Vsync", &d.vsync)) mark(p);
    tip("Off: no forced vsync, lowest latency, works well with G-Sync / FreeSync together with a frame limit. On: the display paces the game.");

    char cap[32];
    fps_label(d.fps_cap, cap, sizeof cap);
    if (ImGui::BeginCombo("Frame limit", cap)) {
        for (int fps : kFpsChoices) {
            char name[32];
            fps_label(fps, name, sizeof name);
            if (ImGui::Selectable(name, fps == d.fps_cap) && fps != d.fps_cap) {
                d.fps_cap = fps;
                quality_preset_edited(d, QualityField::FrameLimit);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    tip("Refresh rate follows the display. With vsync on, a limit only counts when it is below the refresh rate.");
    {   // [display] fps_snap: on by default on a detected Steam Deck only (auto); this box writes true / false
        const bool deck = nxrt::host_is_steam_deck();
        bool snap = d.fps_snap == "true" || (d.fps_snap == "auto" && deck);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 1.0f, 1.0f));
        if (ImGui::Checkbox("Cap FPS to 30 for Steam Deck", &snap)) {
            d.fps_snap = snap ? "true" : "false";
            mark(p);
        }
        ImGui::PopStyleColor();
        tip("The Steam Deck's screen refreshes at a fixed 90 Hz (OLED) or 60 Hz (LCD) and cannot follow the game. At 40 fps, frames stay on "
            "screen for uneven times, which feels like stutter; at 30 every frame stays for the same time. Needs vsync on. With a higher limit "
            "it takes the next even step instead (60 becomes 45 at 90 Hz). Off: your frame limit stays exactly as set.");
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "THIS ACTUALLY MAKES THE GAME SMOOTHER on a Steam Deck.");
        ImGui::TextDisabled(deck ? "On by default on this Steam Deck. Untick it to keep your frame limit exactly as set."
                                 : "Off by default here: this is not a Steam Deck. Tick it only on a screen without G-Sync / FreeSync.");
        if (snap && !d.vsync) ImGui::TextColored(kOrange, "Turn vsync on for this to work.");
    }
    if (live.snap_k >= 2 && d.fps_cap > 0) {
        ImGui::TextDisabled("%d (runs at %g on this %g Hz screen%s)", d.fps_cap, live.snap_cap, static_cast<double>(live.refresh_hz),
                            d.fps_cap == 40 ? "; set 80 Hz in Steam's Performance menu for 40" : "; pick a refresh rate that is a multiple of it");
    }
    ImGui::BeginDisabled(live.refresh_hz <= 0.0f);
    if (ImGui::Button("Just under the refresh rate (G-Sync / FreeSync)")) {
        // Below 100 Hz suggest_vrr_cap is the refresh rate itself: round it (59.94 -> 60, 89.98 -> 90) so the cap is not a frame short.
        const double cap = nxrt::suggest_vrr_cap(live.refresh_hz);
        d.fps_cap = static_cast<int>(live.refresh_hz < 100.0f ? std::lround(cap) : std::floor(cap));
        quality_preset_edited(d, QualityField::FrameLimit);
        mark(p);
    }
    tip("For G-Sync / FreeSync screens: sets the limit a few fps under the refresh rate (116 at 120 Hz, 138 at 144 Hz) so the screen "
        "keeps following the game instead of tearing or stuttering at the top. Below 100 Hz it sets the refresh rate itself (60, 90): "
        "a lower limit there only makes the game less smooth. On a screen without G-Sync / FreeSync, use vsync instead.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (live.refresh_hz > 0.0f) ImGui::TextDisabled("display %.2f Hz", live.refresh_hz);
    else ImGui::TextDisabled("display refresh rate unknown");

    fps_group(host, p);
}

// The size the game renders at and what happens to it on the way to the screen, live (#117): the picture the NVN back end presented last
// (the window texture's crop at the render scale) against the swapchain size, and the upscaler that fills the difference.
void render_size_line() {
    const gfx::Tuning& tu = gfx::tuning();
    const int sw = tu.present_src_w.load(std::memory_order_relaxed), sh = tu.present_src_h.load(std::memory_order_relaxed);
    const int ow = tu.present_out_w.load(std::memory_order_relaxed), oh = tu.present_out_h.load(std::memory_order_relaxed);
    if (sw <= 0 || sh <= 0 || ow <= 0 || oh <= 0) {
        ImGui::TextDisabled("Rendering: not known yet (no frame presented)");
        return;
    }
    const extras::UpscaleMode m = extras::upscale_mode();
    const char* how = "stretched";
    if (sw > ow || sh > oh) how = "scaled down";
    else if (m == extras::UpscaleMode::Fsr1) how = "upscaled with FSR 1";
    else if (m == extras::UpscaleMode::Nis) how = "upscaled with NIS";
    if (sw == ow && sh == oh) {
        ImGui::Text("Rendering %d x %d, shown at that size (no upscaling)", sw, sh);
    } else {
        ImGui::Text("Rendering %d x %d -> %s to %d x %d", sw, sh, how, ow, oh);
        if (sw < ow && sh <= oh && m != extras::UpscaleMode::Fsr1 && m != extras::UpscaleMode::Nis)
            ImGui::TextDisabled("Below the output size: FSR 1 or NIS (Graphics page) upscale it sharper than the plain stretch.");
    }
    tip("The game's picture size right now (its targets times the render scale) and the size it is shown at (the window or the display).");
}

void resolution_group(Page& p) {
    DisplaySettings& d = p.edit.display;
    ImGui::SeparatorText("Resolution");
    render_size_line();
    bool autos = d.render_scale <= 0;
    if (ImGui::Checkbox("Automatic render scale", &autos)) {
        d.render_scale = autos ? 0 : 100;
        quality_preset_edited(d, QualityField::RenderScale);
        mark(p);
    }
    tip("Auto renders at the size of the window (never below the game's own 1080p). Turn it off to pick a percentage.");
    restart_badge();
    if (!autos) {
        if (ImGui::SliderInt("Render scale", &d.render_scale, 25, 400, "%d%%")) {
            quality_preset_edited(d, QualityField::RenderScale);
            mark(p);
        }
        tip("The game's internal resolution as a percentage of its own size (100% = 1920 x 1080). Above 100% renders more pixels than the console did.");
        restart_badge();
    }
}
void display_page(UiHost& host, const LiveStatus& live, Page& p) {
    window_group(live, p);
    framerate_group(host, live, p);
    resolution_group(p);
    upscale_group(p);
    latency_group(p);

    ImGui::SeparatorText("Live");
    ImGui::Text("Window: %s, present mode: %s", cstr(live.window_mode), cstr(live.present_mode));
    ImGui::Text("Display %.2f Hz, window %d x %d, %.1f fps", live.refresh_hz, live.width, live.height, live.fps);
}

}  // namespace ui
}  // namespace nxrt::play
