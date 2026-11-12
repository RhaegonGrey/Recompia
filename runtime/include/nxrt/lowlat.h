// SPDX-License-Identifier: GPL-3.0-or-later
// Low-latency modes (S14): NVIDIA Reflex through VK_NV_low_latency2 and AMD Anti-Lag 2 through VK_AMD_anti_lag.
//
// One frame of the game crosses three threads: the guest main thread (Update: pacing, input sampling, simulation), the host render thread
// that replays the game's command lists and presents, and the driver. The markers follow that:
//
//   main thread    sim_begin()        sleeps (Reflex vkLatencySleepNV / Anti-Lag INPUT stage) so the frame starts as late as the GPU allows, then
//                                     SIMULATION_START; input_sample() sets INPUT_SAMPLE just before the mouse and pad are read; sim_end()
//   render thread  submit_begin()     RENDERSUBMIT_START at the first replayed submission of the frame
//                  present_begin()    RENDERSUBMIT_END, PRESENT_START, returns the present id to pass in VkPresentIdKHR
//                  present_end()      PRESENT_END
//
// Frame ids pair the threads: a frame leaves sim_end() into a queue and the next present takes the oldest id, so a frame keeps its id from
// simulation to present however many frames the render thread lags. All calls are no-ops when the mode is off or the GPU has neither
// extension, and every entry point is safe to call from any thread.
#pragma once
#include <cstdint>
#include <string>

#include "nxrt/extras.h"
#include "nxrt/gfx.h"

namespace nxrt::extras {

class LowLatency {
public:
    enum class Backend : uint8_t { None = 0, Reflex, AntiLag };

    // Reads the settings; the backend is the one the GPU supports (Reflex first) when the mode is not Off.
    explicit LowLatency(gfx::Device& dev);
    ~LowLatency();
    LowLatency(const LowLatency&) = delete;
    LowLatency& operator=(const LowLatency&) = delete;

    Backend backend() const { return backend_; }
    bool enabled() const { return enabled_; }
    const char* backend_name() const;

    // Swapchain life cycle (render thread). prepare_swapchain chains VkSwapchainLatencyCreateInfoNV into ci (storage lives in *this until the
    // call returns) when Reflex is enabled; swapchain_created / swapchain_destroyed tell the module the handle (the sleep mode is per swapchain).
    void prepare_swapchain(VkSwapchainCreateInfoKHR* ci);
    void swapchain_created(VkSwapchainKHR sc);
    void swapchain_destroyed(VkSwapchainKHR sc);
    // The frame cap Reflex / Anti-Lag pace to (0 = none). Called when the cap changes.
    void set_cap_fps(double fps);

    // Main thread.
    void wait_in_flight();        // blocks while more than frames_ahead earlier frames are unpresented (no-op when the wait is off); sim_begin() calls it too
    void sim_begin();
    void input_sample();
    void sim_end();
    // True while Reflex sets the frame pace itself (its sleep keeps the cap as the minimum interval between simulation starts): the frame limiter then does not
    // wait at the frame boundary, because two pacers on one frame accumulate each other's sleeps (S21, [latency] reflex_limiter).
    bool paces_frames() const;
    // Render thread.
    void submit_begin();
    uint64_t present_begin();     // the id for VkPresentIdKHR (0 when disabled)
    void present_end();
    uint32_t present_sim_id() const;   // simulation id the present in progress shows (0: none); for the frame trace probe

    // Timings. Reflex: the driver's own frame reports; always: the host proxy input sample -> present returned.
    struct Stats {
        Backend backend = Backend::None;
        bool enabled = false;
        uint64_t frames = 0;
        double sleep_ms_avg = 0;          // time spent blocked in sim_begin
        double input_to_present_ms = 0;   // proxy: INPUT_SAMPLE to the end of vkQueuePresentKHR (EMA)
        double input_to_gpu_ms = 0;       // Reflex reports: inputSample to gpuRenderEnd (EMA; 0 when no reports)
        double sim_to_present_ms = 0;     // Reflex reports: simStart to presentEnd
        uint64_t reports = 0;
    };
    Stats stats() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    Backend backend_ = Backend::None;
    bool enabled_ = false;
};

// Frame pacing placement (S18, [latency] pace = main | present, env NXRT_LATENCY_PACE; default main). With "main" the frame limiter waits at the game's frame
// boundary, before the input is sampled, and not inside the present call: the frame that is shown was simulated one render time before, not one
// queue depth before. The presenting thread still runs the limiter for a present that no boundary paced (loading screens, movies).
// The presents that make the next boundary wait due are counted where the game calls present ([latency] boundary_count = game, default), so every
// game frame gets exactly one wait, at the first main-loop pass after its present call. With `render` they are counted when the render thread
// presents, which runs about one frame behind the game ([nvn] present_ahead): when that present came after the game's present call, the boundary
// right after it was not due, the next main-loop pass (in the middle of the following frame) waited instead, and the frame after that got no
// wait (Steam Deck, cap 45: 35-40 ms / 15 ms interval pairs, 3 % of the intervals standing still).
bool pace_in_main();
void pace_note_present();         // render thread: a frame was presented (counted with boundary_count = render)
uint32_t pace_note_game_present();   // game thread, at its present call (counted with boundary_count = game); returns the boundary waits since the
                                     // previous call: 1 per frame when the pacing is right
bool pace_boundary_due();         // main thread: true when a frame was presented since the boundary last waited (the game's main loop runs more
                                  // iterations than it presents frames: waiting in each of them paced the game below the cap); the first call is due
void pace_boundary_done();        // main thread: the boundary hook ran the limiter wait for the frame it starts
bool pace_boundary_consume();     // presenting thread: true if a boundary wait ran in the last 250 ms: the boundary is the one pacer, the present must
                                  // not wait as well (two waits on one schedule take two slots per frame); false while the main loop is stalled (loading, movies)

// The frame cap (0 = none), from the settings. Remembered even before the renderer exists, so an instance created later starts with it (before this, a Reflex
// sleep mode set at swapchain creation had a minimum interval of 0 until the next settings change); forwarded to the running instance.
void lowlat_note_cap(double fps);

// The instance of the running renderer (set by the renderer when it creates one; null otherwise). Hooks and the replay code go through it.
LowLatency* lowlat();
void lowlat_set(LowLatency* l);
// Frame trace probe (S18): the GPU submissions still unfinished, read at each simulation start (set by the NVN back end).
void lowlat_set_backlog_fn(uint64_t (*fn)());
// GPU wait before the input sample ([latency] gpu_frames_ahead, default 2; -1 = off): sim_begin waits, after the frames_ahead wait and before the
// Reflex sleep, until the GPU has finished the game's work of the frame presented that many presents before the latest one (1: the GPU keeps at
// most the latest frame queued, so a GPU-bound game does not pile frames up between the input and the screen, and the GPU never runs dry; 0: the
// latest one too, which serialises the CPU and the GPU). fn(frames_back, timeout_ns) is set by the NVN back end; it returns at once when the
// GPU is that far already, so a CPU-bound game does not wait.
void lowlat_set_gpu_wait_fn(bool (*fn)(int frames_back, uint64_t timeout_ns));

}  // namespace nxrt::extras
