// SPDX-License-Identifier: GPL-3.0-or-later
// The frame loop of the graphics shell (scene.h: run_scene). The order inside one frame is the low-latency design: wait for the
// frame's scheduled start (FrameLimiter), THEN pump the platform and read input, then record and present.
#include <windows.h>

#include <algorithm>

#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/input.h"
#include "nxrt/log.h"
#include "nxrt/overlay.h"
#include "nxrt/platform.h"
#include "nxrt/scene.h"

namespace nxrt::gfx {

// vk_renderer.cpp: waits for the PNG workers a requested screenshot runs on.
void wait_screenshot_writes();

SceneRunResult run_scene(const SceneRunDesc& desc) {
    // Environment switches for automated runs (also ini keys): [gfx] headless / NXRT_GFX_HEADLESS=1 runs offscreen without a
    // window; [gfx] screenshot / NXRT_GFX_SCREENSHOT=<file.png> captures [gfx] screenshot_frame (default 60).
    SceneRunDesc d = desc;
    if (!d.headless && config_get_bool("gfx", "headless", false)) d.headless = true;
    if (d.screenshot_path.empty()) {
        d.screenshot_path = config_get_str("gfx", "screenshot", "");
        if (!d.screenshot_path.empty() && d.screenshot_frame < 0) d.screenshot_frame = static_cast<int>(config_get_int("gfx", "screenshot_frame", 60));
    }
    SceneRunResult res;
    auto fail = [&](const std::string& m) {
        res.error = m;
        NXLOG_E("scene", "%s", m.c_str());
        return res;
    };

    bool window = false;
    if (!d.headless) {
        if (!platform::init()) return fail("platform init failed");
        platform::WindowDesc wd;
        wd.title = "nxrt graphics shell";
        wd.width = static_cast<int>(d.width);
        wd.height = static_cast<int>(d.height);
        wd.hidden = d.window_hidden;
        if (!platform::create_window(wd)) return fail("window creation failed");
        window = true;
    }

    {
        DeviceDesc dd;
        dd.validation = d.validation;
        dd.gpu = d.gpu;
        dd.headless = d.headless ? 1 : 0;
        std::string err;
        auto dev = Device::create(dd, &err);
        if (!dev) {
            if (window) { platform::destroy_window(); platform::shutdown(); }
            return fail("device: " + err);
        }
        res.gpu_name = dev->gpu().name;
        res.gpu_type = dev->gpu().type;

        RendererDesc rd;
        rd.present_mode = d.present_mode;
        rd.headless_width = d.width;
        rd.headless_height = d.height;
        auto rend = Renderer::create(*dev, rd, &err);
        if (!rend) {
            rend.reset();
            dev.reset();
            if (window) { platform::destroy_window(); platform::shutdown(); }
            return fail("renderer: " + err);
        }
        auto scene = TestScene::create(*dev, rend->format(), &err);
        if (!scene) {
            scene.reset();
            rend.reset();
            dev.reset();
            if (window) { platform::destroy_window(); platform::shutdown(); }
            return fail("scene: " + err);
        }
        FrameLimiter limiter;
        limiter.set_target_fps(d.target_fps);

        std::unique_ptr<Overlay> overlay;
        if (d.overlay) {
            Overlay::Context oc;
            oc.device = dev.get();
            oc.renderer = rend.get();
            oc.limiter = &limiter;
            oc.has_window = window;
            overlay = Overlay::create(oc, &err);
            if (!overlay) return fail("overlay: " + err);
            overlay->install_on_renderer();
        }

        DWORD old_class = 0;
        int old_prio = 0;
        if (d.boost_priority) {
            old_class = GetPriorityClass(GetCurrentProcess());
            old_prio = GetThreadPriority(GetCurrentThread());
            SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        }

        input::queue_clear();
        dev->validation_reset();
        const uint64_t t0 = FrameLimiter::now_ns();
        uint64_t rendered = 0, attempts = 0, lat_n = 0;
        double lat_sum = 0, lat_max = 0;
        bool overlay_shown = false;
        while (d.frames == 0 || rendered < d.frames) {
            limiter.begin_frame();                      // 1. wait for the frame start, before touching input
            if (window) platform::pump_events();        // 2. sample input as late as possible
            const uint64_t t_input = FrameLimiter::now_ns();
            if (overlay && d.overlay_visible_from_frame >= 0 && !overlay_shown && static_cast<int64_t>(rendered) >= d.overlay_visible_from_frame) {
                input::Event ev;                        // the real path: a key event through the queue
                ev.type = input::EventType::KeyDown;
                ev.u.key.scancode = overlay->toggle_scancode();
                input::queue_push(ev);
                overlay_shown = true;
            }
            input::Event ev;
            while (input::queue_pop(&ev)) {
                if (overlay) overlay->handle_event(ev);
            }
            if (platform::initialised() && platform::quit_requested()) break;
            if (d.per_frame) d.per_frame(rendered, *rend);
            if (overlay) overlay->apply_pending();

            FrameInfo fi;
            if (!rend->begin_frame(&fi)) {
                if (window) platform::pump_events(true, 50);   // minimised: wait for the window to come back
                limiter.reset_schedule();
                if (++attempts > rendered + 100000) return fail("begin_frame keeps failing");
                continue;
            }
            if (d.screenshot_frame >= 0 && rendered == static_cast<uint64_t>(d.screenshot_frame) && !d.screenshot_path.empty())
                rend->request_screenshot(d.screenshot_path);
            scene->record(fi);
            rend->end_frame();
            ++rendered;
            if (!d.warmup_frames || rendered > d.warmup_frames) {
                const double ms = static_cast<double>(FrameLimiter::now_ns() - t_input) * 1e-6;
                lat_sum += ms;
                lat_max = std::max(lat_max, ms);
                ++lat_n;
            }
            if (d.warmup_frames && rendered == d.warmup_frames) limiter.reset_stats();
        }
        const uint64_t t1 = FrameLimiter::now_ns();
        rend->wait_idle();
        wait_screenshot_writes();   // a requested shot is encoded on a worker; do not exit out from under it (vk_renderer.cpp)

        if (d.boost_priority) {
            SetThreadPriority(GetCurrentThread(), old_prio);
            SetPriorityClass(GetCurrentProcess(), old_class);
        }
        res.frames = rendered;
        res.input_to_submit_mean_ms = lat_n ? lat_sum / static_cast<double>(lat_n) : 0;
        res.input_to_submit_max_ms = lat_max;
        res.pacing = limiter.stats();
        res.renderer = rend->stats();
        res.validation = dev->validation_stats();
        res.wall_seconds = static_cast<double>(t1 - t0) * 1e-9;
        overlay.reset();
        scene.reset();
        rend.reset();
        res.memory_end = dev->memory_stats();
        dev.reset();
    }
    if (window) {
        platform::destroy_window();
        platform::shutdown();
    }
    res.ok = true;
    return res;
}

}  // namespace nxrt::gfx
