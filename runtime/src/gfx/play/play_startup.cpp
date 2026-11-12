// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: loading the settings, start and stop, the process exit and the public API (play.h).
#include "play_state.h"
#include "nxrt/key_state.h"
#include "nxrt/screenshot.h"
#include "nxrt/text.h"
#include "nxrt/vi_sinks.h"

namespace nxrt::play {
using namespace detail;

[[noreturn]] void exit_process_impl(int code, const char* verdict, const std::string& message, bool flush_gpu_cache) {
    G& st = g();
    NXLOG_I(kTag, "ending the process (%s): %s", verdict, message.c_str());
    if (st.dirty) {
        std::string err;
        std::lock_guard<std::mutex> lk(st.mu);
        if (!save_settings(st.path, st.settings, &err)) NXLOG_W(kTag, "cannot save %s: %s", st.path.c_str(), err.c_str());
        st.dirty = false;
    }
    if (flush_gpu_cache) nvn::backend_flush_caches();   // unsafe after VK_ERROR_DEVICE_LOST; normal early-exit paths still save it
    nxrt::log_flush();
    if (tt::enabled() && !tt::finished()) tt::end_run(code, verdict, message);     // writes result.json, then terminates
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    for (;;) Sleep(1000);
}

[[noreturn]] void exit_process(int code, const char* verdict, const std::string& message) {
    exit_process_impl(code, verdict, message, true);
}
namespace detail {

[[noreturn]] void quit_now(const char* why) { exit_process(0, "pass", why); }

// The game's ini and the environment override the settings file for the display extras. A key that is set there ([hdr] mode in the ini, or
// NXRT_HDR_MODE) replaces the file's value with the one extras::settings_from_config() reads; other keys keep the file's value (the F1 page
// edits the file).
extras::Settings merge_env(const extras::Settings& from_file) {
    const extras::Settings env = extras::settings_from_config();
    extras::Settings r = from_file;
    if (config_has("hdr", "mode")) r.hdr.mode = env.hdr.mode;
    if (config_has("hdr", "format")) r.hdr.format = env.hdr.format;
    if (config_has("hdr", "peak_nits")) r.hdr.peak_nits = env.hdr.peak_nits;
    if (config_has("hdr", "paper_white_nits")) r.hdr.paper_white_nits = env.hdr.paper_white_nits;
    if (config_has("hdr", "highlight_boost")) r.hdr.highlight_boost = env.hdr.highlight_boost;
    if (config_has("upscale", "mode")) r.upscale.mode = env.upscale.mode;
    if (config_has("upscale", "aa")) r.upscale.aa = env.upscale.aa;
    if (config_has("upscale", "sharpness")) r.upscale.sharpness = env.upscale.sharpness;
    if (config_has("upscale", "quality")) r.upscale.quality = env.upscale.quality;
    if (config_has("upscale", "scene_scale")) r.upscale.scene_scale = env.upscale.scene_scale;
    if (config_has("upscale", "fsr_dll")) r.upscale.fsr_dll = env.upscale.fsr_dll;
    if (config_has("upscale", "dlss_dll")) r.upscale.dlss_dll = env.upscale.dlss_dll;
    if (config_has("latency", "mode")) r.latency.mode = env.latency.mode;
    if (config_has("latency", "log_timings")) r.latency.log_timings = env.latency.log_timings;
    // [picture]: a preset or any of the four keys in the game's ini / the environment replaces the file's picture controls as a whole
    if (config_has("picture", "preset") || config_has("picture", "contrast") || config_has("picture", "saturation") || config_has("picture", "black") ||
        config_has("picture", "gamma"))
        r.picture = env.picture;
    return r;
}

}  // namespace detail

// ---- public API ---------------------------------------------------------------------------------------------------------------------------
bool load() {
    G& st = g();
    if (st.loaded) return st.load_ok;
    st.loaded = true;
    st.path = config_get_str("play", "controls", "controls.ini");
    std::string err;
    const bool existed = GetFileAttributesA(st.path.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::vector<std::string> warnings;
    st.load_ok = load_settings(st.path, &st.settings, &err, &warnings);
    for (const std::string& w : warnings) NXLOG_W(kTag, "%s: %s", st.path.c_str(), w.c_str());
    if (!existed) {
        std::string e2;
        if (save_settings(st.path, st.settings, &e2)) NXLOG_I(kTag, "wrote the factory settings to %s", st.path.c_str());
    } else if (!st.load_ok) {
        NXLOG_W(kTag, "%s: %s (the factory settings are in use)", st.path.c_str(), err.c_str());
        // A later save (the F1 page, the exit) writes the factory settings over the file: keep what the player wrote first.
        const std::string bad = st.path + ".bad";
        if (CopyFileA(st.path.c_str(), bad.c_str(), FALSE)) NXLOG_W(kTag, "kept the unreadable %s as %s", st.path.c_str(), bad.c_str());
    } else {
        NXLOG_I(kTag, "settings loaded from %s (%zu bindings)", st.path.c_str(), st.settings.bindings.size());
    }
    // An older controls.ini got the newer factory bindings (play_config.cpp, kDefaultAdditions): keep the old file once as .bak, then
    // write the upgraded one. CopyFileA with bFailIfExists = TRUE leaves an existing .bak alone.
    if (existed && st.load_ok && st.settings.migrated) {
        const std::string bak = st.path + ".bak";
        if (CopyFileA(st.path.c_str(), bak.c_str(), TRUE)) NXLOG_I(kTag, "kept the previous %s as %s", st.path.c_str(), bak.c_str());
        std::string e3;
        if (save_settings(st.path, st.settings, &e3))
            NXLOG_I(kTag, "controls.ini updated to defaults revision %d (added: %s)", st.settings.defaults_rev,
                    st.settings.migration_added.empty() ? "nothing new" : st.settings.migration_added.c_str());
        else
            NXLOG_W(kTag, "cannot save the upgraded %s: %s", st.path.c_str(), e3.c_str());
    }
    // Display extras: a value set in the game's ini or in the environment (NXRT_HDR_MODE ...) overrides the file; the file overrides the built-in defaults.
    st.settings.extras = merge_env(st.settings.extras);
    extras::set_settings(st.settings.extras);
    // S38 GPU paths: the file's choice becomes [nvn] pool_mirror / pass_barriers unless the game's ini or the environment set them. The replay
    // reads both once, at its first pool and its first submission, after this.
    if (!config_has("nvn", "pool_mirror")) config_set("nvn", "pool_mirror", st.settings.display.fast_gpu_memory ? "on" : "off");
    if (!config_has("nvn", "pass_barriers")) config_set("nvn", "pass_barriers", st.settings.display.fewer_gpu_waits ? "tracked" : "full");
    // Prewarm: the file's choice becomes [nvn] prewarm unless the ini or the environment set it (read once, by the pipeline prewarm).
    if (!config_has("nvn", "prewarm")) config_set("nvn", "prewarm", st.settings.display.shader_prewarm ? "true" : "false");
    // ASTC texture quality: the file's choice becomes [gfx] astc_quality unless the ini or the environment set it (the texture back end reads it at its first ASTC texture).
    if (!config_has("gfx", "astc_quality")) config_set("gfx", "astc_quality", st.settings.display.astc_quality.c_str());
    // Audio: a volume or device set in the ini or the environment wins over the file; the page then shows what is in force.
    if (config_has("audio", "volume")) st.settings.audio.volume = audio::master_volume();
    else audio::set_master_volume(static_cast<float>(st.settings.audio.volume));
    if (config_has("audio", "device")) st.settings.audio.device = config_get_str("audio", "device", "default");
    else config_set("audio", "device", st.settings.audio.device.c_str());
    st.capture_mouse = config_get_bool("play", "capture_mouse", true);
    st.late_sample = config_get_bool("play", "late_sample", true);
    st.quit_on_close = config_get_bool("play", "quit_on_close", true);
    st.log_status = config_get_bool("play", "log_status", false);
    st.pointer_draw = config_get_bool("play", "pointer_draw", false);
    st.base = static_cast<uint64_t>(config_get_int("play", "game_base", static_cast<int64_t>(game::info().main_base)));
    apply_frame_cap(st.settings);
    return st.load_ok;
}

DisplayConfig display() {
    G& st = g();
    load();
    DisplayConfig d;
    d.width = st.settings.display.width;
    d.height = st.settings.display.height;
    d.window_mode = st.settings.display.window_mode;
    d.vsync = st.settings.display.vsync;
    d.fps_cap = st.settings.display.fps_cap;
    d.keep_aspect = st.settings.display.keep_aspect;
    d.present_mode = present_mode_for(st.settings.display);
    return d;
}

FrameLimiter* limiter() {
    load();
    return &g().limiter;
}

std::string settings_path() {
    load();
    return g().path;
}

// The default render size on a small GPU. Based on pokopia-native (MIT) windows/src/nxrt/vk/vk_dev.c (vk_vram_bytes) and
// windows/src/nxrt/front/menu.c (monitor_h) @e5d9ac6: with 4 GB of video memory or less the size the game starts at is kept to 2560x1440
// (a 4 GB laptop card on a 4K screen runs out of memory at 3840x2160). Only an unset size: render_width / render_height or a
// render_scale in the settings win, and [display] vram_cap = 0 turns it off. NXRT_VK_VRAM_MB pretends another amount (tests). The window
// size is lowered here, keeping its shape, so every path of the game's render setup sees the capped size.
static void cap_default_size(const DisplaySettings& d, gfx::Device* dev, int* ww, int* wh) {
    if (!d.vram_cap) return;
    if (d.render_width > 0 && d.render_height > 0) return;
    if (d.render_scale > 0) return;
    uint64_t vram = 0;
    const char* e = std::getenv("NXRT_VK_VRAM_MB");
    if (e && *e) {
        vram = static_cast<uint64_t>(std::strtoull(e, nullptr, 10)) << 20;
    } else if (dev && dev->physical()) {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(dev->physical(), &mp);
        for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
            if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max<uint64_t>(vram, mp.memoryHeaps[i].size);
    }
    if (vram == 0 || vram > (4608ull << 20)) return;   // unknown, or more than 4 GB (a 4 GB card reports about 4096 MB)
    const int w0 = *ww, h0 = *wh;
    if (h0 > 1440 || w0 > 2560) {
        const double f = std::min(1440.0 / std::max(h0, 1), 2560.0 / std::max(w0, 1));
        *ww = static_cast<int>(w0 * f) & ~1;
        *wh = static_cast<int>(h0 * f) & ~1;
    }
    NXLOG_I(kTag, "vram_cap: %llu MB of video memory (4 GB or less): default render size kept to 2560x1440 (%dx%d -> %dx%d; [display] vram_cap = 0 turns it off)",
            static_cast<unsigned long long>(vram >> 20), w0, h0, *ww, *wh);
}

bool start(const StartOptions& opt) {
    G& st = g();
    load();
    if (st.started) return true;
    st.renderer = opt.renderer;
    st.device = opt.device;
    st.window = opt.window && platform::has_window();
    apply_frame_cap(st.settings);                      // the display's refresh rate is known now (vsync cap)
    kernel::sched_set_process_priority(st.window);     // a person is playing: keep the frame out of the background load
    st.pump_thread = std::this_thread::get_id();
    st.engine = std::make_unique<Engine>(st.settings);
    st.engine->set_context(Ctx::Menu, now_ns());
    st.source_owner = std::make_unique<PlaySource>(st.engine.get());
    st.source = std::shared_ptr<hid::InputSource>(st.source_owner.get(), [](hid::InputSource*) {});   // owned by G
    hid::set_input_source(st.source);
    hid::set_vibration_sink(&vib_sink, nullptr);
    tt::set_mouse_sink(&input::mouse_delta_add);
    tt::set_key_sink(&key_sink);
    key_state::set_name_lookup(&key_scancode_by_name);
    if (st.renderer) screenshot::set_provider(&screenshot_provider, nullptr);
    if (st.renderer) tt::set_screenshot_flush(&gfx::wait_screenshot_writes);
    set_hdr_luminance_sink(&extras::hdr_note_layer_luminance);
    if (const std::string script = config_get_str("play", "selftest", ""); !script.empty()) {
        std::string err;
        if (!st.selftest.parse(script, &err)) {
            NXLOG_E(kTag, "[play] selftest: %s", err.c_str());
            nxrt::log_flush();
            TerminateProcess(GetCurrentProcess(), 2);
        }
        NXLOG_I(kTag, "selftest script: %s", script.c_str());
    }

    if (st.renderer) {
        st.limiter.set_pre_frame_hook(&pre_frame, nullptr);
        if (!st.renderer->headless()) st.renderer->set_present_mode(present_mode_for(st.settings.display));
        // [applet] swkbd_prompt = force makes the overlay for the on-screen keyboard even where the host asked for none (headless test runs)
        const bool swkbd_force = config_get_str("applet", "swkbd_prompt", "true") == "force";
        if ((opt.overlay || swkbd_force) && opt.device) {
            gfx::Overlay::Context oc;
            oc.device = opt.device;
            oc.renderer = opt.renderer;
            oc.limiter = &st.limiter;
            oc.has_window = st.window;
            std::string err;
            st.overlay = gfx::Overlay::create(oc, &err);
            if (!st.overlay) {
                NXLOG_W(kTag, "no overlay: %s", err.c_str());
            } else {
                st.overlay->add_panel("Settings", [] { draw_settings_page(ui_host()); });
                st.overlay->install_on_renderer();
                if (const GamePlay* gp = game_play()) st.overlay->set_game_layer(gp->overlay_layer);   // its place: play_frame.cpp (picture rect)
                apply_fps_counter(st.settings.fps);
                {   // [overlay] touch_button: auto (the default: when the machine has a touch screen), on, off
                    std::string tb = config_get_str("overlay", "touch_button", "auto");
                    tb = nxrt::text::lower(std::move(tb));
                    const bool on = tb == "on" || tb == "true" || tb == "1" || tb == "yes" || (tb == "auto" && st.window && platform::touchscreen_present());
                    st.overlay->set_touch_button(on);
                    NXLOG_I(kTag, "touch: touch screen %s, menu button %s ([overlay] touch_button = %s)", st.window && platform::touchscreen_present() ? "found" : "not found",
                            on ? "on" : "off", tb.c_str());
                }
            }
        }
    }
    // The software keyboard asks the player on the overlay's on-screen keyboard while there is a game window ([applet] swkbd_prompt = false: never;
    // force: also without a window, for scripted headless runs).
    {
        const bool force = config_get_str("applet", "swkbd_prompt", "true") == "force";
        services::swkbdui::enable_prompt(st.overlay && (force || (st.window && config_get_bool("applet", "swkbd_prompt", true))));
    }
    // the window keeps answering while the game boots or loads (no frame boundary for seconds), and the on-screen keyboard works while the
    // game waits for it (keyboard_service; also headless with swkbd_prompt = force, which has an overlay and no window)
    if (st.window || st.overlay) svc_set_busy_hook(GetCurrentThreadId(), &busy_pump);
    kernel::set_blocked_hook(GetCurrentThreadId(), &keyboard_service);
    if (st.window && st.settings.display.monitor > 0) platform::move_to_display(st.settings.display.monitor);
    if (st.window && st.settings.display.window_mode != "windowed") set_window_mode(st.settings.display.window_mode);

    const GamePlay* gp = game_play();
    {   // display options: the internal resolution comes from the window as it is now (after the window mode change above)
        int ww = 0, wh = 0;                    // no window (headless, test runs): the configured window size, not a made-up one
        if (st.window) platform::pixel_size(&ww, &wh);
        if (ww <= 0 || wh <= 0) { ww = st.settings.display.width; wh = st.settings.display.height; }
        display_tuning(st.settings.display);
        cap_default_size(st.settings.display, st.device, &ww, &wh);
        if (gp && gp->install_render) gp->install_render(st.base, st.settings.display, ww, wh);   // the game's display options, tone curve
    }
    // the frame boundary, then the game's input, camera, lock-on and menu hooks (hooks of one address run in registration order)
    if (gp && gp->frame_update) st.hook_update = hook_add_entry(st.base + gp->frame_update, &update_hook);
    else NXLOG_W(kTag, "the game (%s) names no frame update: play mode is not pumped", game::info().id);
    if (gp && gp->install) gp->install(st.base, &game_host(), st.settings);
    st.started = true;
    NXLOG_I(kTag, "play mode started (%s, overlay %s, base 0x%llx, game %s, frame hook %u)", st.window ? "window" : "no window", st.overlay ? "on" : "off",
            static_cast<unsigned long long>(st.base), game::info().id, st.hook_update);
    return true;
}

void stop() {
    G& st = g();
    if (!st.started) return;
    vpad_shutdown();      // a virtual pad from a test script does not outlive play mode
    if (const GamePlay* gp = game_play(); gp && gp->remove) gp->remove();
    set_hdr_luminance_sink(nullptr);   // (the screenshot flush stays: a run can end after play stops, and it only waits on a counter)
    svc_set_busy_hook(0, nullptr);
    kernel::set_blocked_hook(0, nullptr);
    if (st.hook_update) hook_remove(st.hook_update);
    st.hook_update = 0;
    hid::set_vibration_sink(nullptr, nullptr);
    if (hid::input_source() == st.source) hid::set_input_source(nullptr);
    if (st.dirty) {
        std::string err;
        std::lock_guard<std::mutex> lk(st.mu);
        save_settings(st.path, st.settings, &err);
        st.dirty = false;
    }
    st.limiter.set_pre_frame_hook(nullptr, nullptr);
    st.started = false;
}

Stats stats() {
    G& st = g();
    Stats s;
    s.frames = st.frames.load();
    s.process_input_calls = st.pi_calls.load();
    s.aim_frames = st.aim_frames.load();
    s.events = st.events.load();
    s.mouse_counts_applied = st.counts.load();
    s.pump_thread_mismatch = st.thread_mismatch.load();
    return s;
}

Settings current_settings() { return ui_host().settings(); }
static std::atomic<uint64_t> g_game_menu_edits{0};
void apply_settings(const Settings& s) { ui_host().apply(s); g_game_menu_edits.fetch_add(1, std::memory_order_relaxed); }
uint64_t game_menu_edits() { return g_game_menu_edits.load(std::memory_order_relaxed); }
}  // namespace nxrt::play
