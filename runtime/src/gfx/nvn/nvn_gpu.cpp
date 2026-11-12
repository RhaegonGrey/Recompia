// SPDX-License-Identifier: GPL-3.0-or-later
// The Vulkan side of the NVN back end: the render thread, submission slots (command buffer, descriptor pools, upload memory),
// the timeline semaphore, deferred destruction, barriers and the public backend API (include/nxrt/nvn_backend.h).
// Design follows smo-recomp (record on game threads, replay on one render thread) and SuperMarioWonderRecomp docs/nvn (a submit
// is replayed under one lock and never overlaps the next); the code is new.
#include "nxrt/nvn_backend.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/early_wake.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/host_device.h"
#include "nxrt/log.h"
#include "nxrt/lowlat.h"
#include "nxrt/present_timing.h"
#include "vk_gauges.h"
#include "nvn_gpu_internal.h"
#include "nvn_thread.h"
#include "nvn_impl.h"
#include "nvn_pipeline.h"
#include "nvn_present_pass.h"
#include "nvn_submit_park.h"
#include "nvn_submit_worker.h"
#include "extras/output_pass.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.gpu";
constexpr uint32_t kSlots = 36;                  // about three frames of submissions (the game submits ~11 times a frame): with fewer, recording waits on
                                                 // the GPU for a submission of a recent frame and the CPU cannot get frames ahead
constexpr uint32_t kGpMax = 1024;      // GPU pass profiler: timestamps per submission
constexpr VkDeviceSize kUploadBytes = 8ull << 20;   // per slot; larger uploads get their own staging buffer
constexpr uint32_t kMaxPieces = 8;               // [nvn] gpu_split: command buffers per slot (the most pieces one submission is cut into)

// Live-count gauges of this file (leak monitor, nxrt/gauge.h).
nxrt::GaugeCounter g_gauge_deferred("nvn.deferred_destroy");     // closures waiting for their submission to finish (defer_destroy, Frame::defer)
nxrt::GaugeCounter g_gauge_render_queue("nvn.render_queue");     // jobs queued for the render thread
nxrt::GaugeCounter g_gauge_barriers_mem("nvn.cnt.barriers_mem");   // full memory barriers recorded (cumulative; per frame = delta / frames)
nxrt::GaugeCounter g_gauge_barriers_img("nvn.cnt.barriers_img");   // image layout barriers recorded (cumulative)
// [nvn] poll_flush: what flushed the batch in submit_batch = frame (cumulative; only batches a poll or wait asked for are attributed).
nxrt::GaugeCounter g_gauge_flush_poll("nvn.flush.by_poll");               // a poll flushed it at once (poll_flush = now)
nxrt::GaugeCounter g_gauge_flush_deadline("nvn.flush.by_deadline");       // the poll_flush = <ms> deadline passed first
nxrt::GaugeCounter g_gauge_flush_present("nvn.flush.by_present");         // a deferred poll was pending and the batch went some other way (the present)
nxrt::GaugeCounter g_gauge_flush_wait("nvn.flush.by_wait");               // a blocking wait flushed it
nxrt::GaugeCounter g_gauge_polls_deferred("nvn.flush.polls_deferred");    // polls that did not flush (all kinds)
nxrt::GaugeCounter g_gauge_polls_deferred_sync("nvn.flush.polls_deferred.sync");   // ... nvnSyncWait with timeout 0
constexpr uint64_t kPollDeferred = UINT64_MAX;   // flush_deadline_ns: a poll was deferred and there is no deadline (poll_flush = defer)

uint64_t steady_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
nxrt::GaugeCounter g_gauge_merged("nvn.submit.merged_into_present");   // [nvn] one_submit: batch flushes that rode in a present's submission (cumulative)
nxrt::GaugeCounter g_gauge_park_expired("nvn.submit.park_expired");    // [nvn] one_submit: held batch flushes the present did not take in time (cumulative)

struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;  // the command buffer being recorded; once recorded, the submission's last one (the one that goes in `batch`)
    // [nvn] gpu_split: the command buffers of the submission's pieces, all from `pool`: cbs[0] is made with the slot, the others on first
    // use. `cb` is cbs[piece]. Earlier pieces are already on the queue (split_piece). The pool reset in acquire_slot resets them all.
    VkCommandBuffer cbs[kMaxPieces] = {};
    uint32_t piece = 0;
    std::atomic<uint64_t> queued_ns{0};   // frame trace: when the submission's first command buffer was handed to vkQueueSubmit2 (0: unknown)
    std::vector<VkDescriptorPool> dpools;
    size_t dpool_cur = 0;
    gfx::Buffer upload;
    VkDeviceSize upload_off = 0;
    std::vector<gfx::Buffer> extra;       // staging / overflow buffers of the submission
    uint64_t serial = 0;                  // last submission that used the slot
    bool timed = false;                   // frame trace: the submission wrote GPU timestamps (query pair index = slot index * 2)
    uint64_t cpu_t0 = 0;
    uint64_t sets_live = 0;               // descriptor sets allocated from this slot's pools since the last reset (vk.descriptor_sets)
    uint32_t gp_n = 0;                    // GPU pass profiler: timestamps written by the submission
    std::vector<std::pair<std::string, uint32_t>> gp_labels;   // label and argument of each interval (stamp i closes interval i)
};

struct Deferred {
    uint64_t serial;
    std::function<void()> fn;
};

struct Gpu {
    gfx::Device* dev = nullptr;
    gfx::Renderer* rend = nullptr;
    FrameLimiter* limiter = nullptr;
    bool keep_aspect = false;
    bool installed_entry_points = false;
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::atomic<uint64_t> completed{0};
    uint64_t submitted = 0;               // render thread: last serial recorded (handed to vkQueueSubmit2, or waiting in `batch`)
    uint64_t flushed = 0;                 // render thread: last serial handed to vkQueueSubmit2
    // [nvn] submit_batch: recorded submissions wait here and go to the GPU in one vkQueueSubmit2 (flush_batch). Render thread.
    std::vector<Slot*> batch;
    uint32_t batch_max = 1;
    // submit_batch = frame: the batch is not flushed when the render thread runs out of work; it goes with the present, in the present's
    // own vkQueueSubmit2 (take_batch_for_present), or earlier when a thread waits for one of its serials (flush_wanted).
    bool frame_batch = false;
    std::atomic<uint64_t> flushed_pub{0};          // `flushed`, readable from any thread
    std::atomic<bool> flush_wanted{false};         // a thread waits for (or polls) a serial still in the batch
    // [nvn] poll_flush (only with frame_batch): 0 = now (a poll flushes at once), -1 = defer (the batch goes with the present), N > 0 = defer for at
    // most N ms. Read once at start.
    int poll_flush_ms = 0;
    // A poll of a serial in the batch was deferred. 0 = none pending; kPollDeferred = pending without a deadline; else the steady_clock deadline
    // in ns. Set by any polling thread (compare-exchange from 0, the first deferred poll of the batch), cleared by the render thread when the
    // batch is flushed or gone.
    std::atomic<uint64_t> flush_deadline_ns{0};
    std::atomic<uint64_t> flush_poll_serial{0};    // the highest serial a deferred poll asked for (it may still be queued, not yet in the batch)
    std::atomic<bool> flush_by_wait{false};        // the pending flush_wanted came from (or was upgraded by) a blocking wait, not a poll
    uint64_t pf_logged_presents = 0;               // render thread: presents at the last summary line
    uint64_t recording = 0;               // render thread: serial of the submission being recorded (0 outside)
    std::atomic<uint64_t> last_submitted{0};
    // Serials are handed out when a submission is queued (the game thread gets its serial at once and goes on while the render thread
    // records), in queue order, so the timeline is signalled in increasing order. Under qmu.
    std::atomic<uint64_t> reserved{0};
    // Presents queued and not yet done; the game thread waits while [nvn] present_ahead of them are pending (back pressure).
    std::mutex pmu;
    std::condition_variable pcv;
    int presents_pending = 0;
    // The game's last present (under pmu): present_again shows it once more while the game presents nothing (it waits for a library applet).
    Ref<Window> last_window;
    int last_index = -1;
    std::chrono::steady_clock::time_point last_present{};
    Slot slots[kSlots];
    VkQueryPool qpool = VK_NULL_HANDLE;   // frame trace: two timestamps per slot
    VkQueryPool gpool = VK_NULL_HANDLE;   // GPU pass profiler ([prof] gpu_passes): kGpMax timestamps per slot
    uint32_t next_slot = 0;
    Counters ctr;

    // render thread
    BigThread th;
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<std::function<void()>> queue;
    bool stop = false;
    std::thread::id tid;

    // deferred destruction
    std::mutex dmu;
    std::vector<std::function<void()>> pending_destroy;   // from any thread, assigned a serial by the render thread
    std::deque<Deferred> deferred;                        // render thread

    PresentPass present;
    bool present_ok = false;
    extras::OutputPass output;                 // HDR encode and FSR 1 (S14); used instead of `present` when the extras are on
    bool output_ok = false;
    std::mutex unsupported_mu;
    std::map<std::string, uint64_t> unsupported;

    // [nvn] submit_thread (default auto: on on Linux): the vkQueueSubmit2 of flush_batch and the present's submit + vkQueuePresentKHR run on this one
    // FIFO worker (nvn_submit_worker.h) instead of the render thread. null = off: everything inline, as before.
    //
    // What depends on a submission having been made, and how each is kept right with the worker:
    // - CPU waits on the timeline: wait_serial_blocking (render thread: slot reuse in acquire_slot, run_frame_sync readbacks, finish,
    //   tear_down) and wait_serial (game threads: nvnSyncWait, the low-latency GPU wait, compute out_fetch) first wait until the worker
    //   has submitted the serial (sw->wait_submitted, zone submit.wait_submitted), then on the timeline. A serial completed on the CPU
    //   (device lost, failed submit) counts as submitted.
    // - Polls (serial_done, poll_completed, nvnSyncWait with timeout 0) read the timeline value only: a value cannot be reached before it
    //   was submitted, so a poll never answers early; it answers "not yet" a little longer at most. want_flush is unchanged: `flushed`
    //   now means "handed to the worker", which will submit it without being asked again.
    // - Slot reuse (command pool / descriptor pool / upload ring / staging buffers reset in acquire_slot): only after the slot's serial
    //   completed on the GPU, which needs it submitted first (above). Frame trace / GPU pass query results are read there too.
    // - defer_destroy / Frame::defer: run in retire() when the timeline reached their serial: after the worker submitted it.
    // - Submit failure: the worker signals the timeline on the CPU (vkSignalSemaphore) and publishes, as flush_batch / present_batch_done
    //   did inline. Device lost: the worker submits nothing, completes the serial on the CPU and publishes, so no waiter hangs.
    // - Swapchain (acquire, recreate on resize / OUT_OF_DATE, present mode change from the pre-frame hook) and the frame slot's fence:
    //   present_on_render_thread drains the worker first (submit_drain), so the previous present has run and its `dirty` is visible.
    //   A screenshot frame drains right after queueing (it waits on the fence and reads the copy back).
    // - Pool mirror / compute copy-back paths wait through wait_serial (above); texture readbacks through run_frame_sync (above).
    // - Sparse binds (vkQueueBindSparse, nvn_gpu_texture.cpp) and the debug submit with vkQueueWaitIdle drain first: they must follow
    //   everything recorded before them on the queue.
    // - Shutdown: the render thread's last flush goes to the worker, then tear_down stops (drains) the worker before vkDeviceWaitIdle and
    //   before any object is destroyed; the renderer's submit queue is unset.
    // Queue access: the VkQueue stays protected by the one Device::queue_mutex (the worker holds it for submit + present; the one-shot
    // device submits, the low-latency markers and the sparse bind take it too). The render thread itself no longer calls vkQueueSubmit2
    // while the worker runs, except the drained debug / sparse paths.
    std::unique_ptr<SubmitWorker> sw;

    // [nvn] one_submit (needs submit_batch = frame and the worker): a batch flush handed to the worker is parked (nvn_submit_park.h) until
    // its job submits it, or the next present takes it while the job has not started (the game's fence polls flush part of each frame
    // early, and under Proton every kernel submission pays the userptr validation again). Taken flushes ride in the present's
    // vkQueueSubmit2, before the frame's own batch, and are published by its after_submit. 0 off, 1 on (in the present's VkSubmitInfo2),
    // 2 split (a VkSubmitInfo2 of their own in the same call, without the swapchain acquire wait).
    int one_submit = 0;
    std::chrono::nanoseconds park_hold{0};     // [nvn] one_submit_hold_ms: how long a job waits for a present that is already queued
    using Cbi = std::vector<VkCommandBufferSubmitInfo>;
    SubmitPark<Cbi> park;
    std::vector<SubmitPark<Cbi>::Entry> taken;     // render thread: taken by the present being built, in serial order

    // [nvn] gpu_split (split_piece): the recording time after which a submission's command buffer is sent to the GPU at the next safe point
    // (in TSC ticks, ft::tsc; 0 = off), and the most pieces one submission is cut into. Read once at start.
    uint64_t split_ticks = 0;
    uint32_t split_max = 1;
    // Counters of the 10 s summary line (split_summary): pieces sent before their submission was recorded to the end, submissions that were
    // split, safe points a due split could not use (a query or debug label open), and vkQueueSubmit2 calls made here (submit_cbs; the present's
    // own submit is counted by ctr.presents).
    std::atomic<uint64_t> split_pieces{0}, split_subs{0}, split_blocked{0}, queue_submits{0};
};

Gpu* g = nullptr;
void flush_batch(Slot* piece = nullptr, bool direct = false);   // [nvn] submit_batch, below
std::mutex g_install_mu;

void complete_serial(uint64_t serial) {
    uint64_t done = g->completed.load();
    while (done < serial && !g->completed.compare_exchange_weak(done, serial)) {}
    if (done < serial) kernel::early_wake_fire();   // this call advanced it: game threads sleeping between sync polls go on (nvn_sync_wake.cpp)
}

uint64_t read_completed() {
    if (g && g->dev->device_lost()) return g->completed.load();
    uint64_t v = 0;
    if (g && g->timeline) vkGetSemaphoreCounterValue(g->dev->device(), g->timeline, &v);
    // [nvn] pool_copy: the GPU writes of every serial up to v go back into guest memory before v is published, so no caller (a sync wait,
    // a poll, the render thread's touch) sees a serial complete whose writes have not landed (nvn_pool_copy.cpp)
    if (g && pool_copy_any()) pcopy_land(v);
    if (g) complete_serial(v);
    return g->completed.load();
}

}  // namespace

// [nvn] pool_copy's landing thread: blocks until the timeline reaches serial (or timeout_ns). Any thread.
bool timeline_wait(uint64_t serial, uint64_t timeout_ns) {
    if (!g || !g->timeline || g->dev->device_lost()) return false;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g->timeline;
    wi.pValues = &serial;
    return vkWaitSemaphores(g->dev->device(), &wi, timeout_ns) == VK_SUCCESS;
}

// [nvn] pool_copy: the serial being recorded, 0 outside a recording (current_serial() gives the last submitted one there). Render thread.
uint64_t recording_serial() { return g ? g->recording : 0; }
// ... and the serial of the last submission queued (queue_submit reserves it on the queueing thread, in queue order). Any thread.
uint64_t queued_serial() { return g ? g->reserved.load() : 0; }

namespace {

void drain_pending_destroy() {
    std::vector<std::function<void()>> pend;
    {
        std::lock_guard<std::mutex> lk(g->dmu);
        pend.swap(g->pending_destroy);
    }
    const uint64_t s = g->recording ? g->recording : g->submitted;
    for (auto& fn : pend) g->deferred.push_back({s, std::move(fn)});
}

void retire() {
    drain_pending_destroy();
    const uint64_t done = read_completed();
    while (!g->deferred.empty() && g->deferred.front().serial <= done) {
        auto fn = std::move(g->deferred.front().fn);
        g->deferred.pop_front();
        g_gauge_deferred.sub(1);
        fn();
    }
}

void render_loop() {
    g->tid = std::this_thread::get_id();
    log_set_thread_name("nvn-render");
    diag_register_thread("nvn-render", nullptr);   // so the freeze watchdog and the stall sampler can see its stack
    nxrt::hitch_set_render_thread();
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(g->qmu);
            if (!g->batch.empty()) {
                const bool wanted = g->flush_wanted.load();
                const uint64_t dl = g->flush_deadline_ns.load();
                const bool due = dl && dl != kPollDeferred && steady_ns() >= dl;
                if (wanted || due || (!g->frame_batch && !g->stop && g->queue.empty())) {
                    lk.unlock();
                    if (wanted) (g->flush_by_wait.exchange(false) ? g_gauge_flush_wait : g_gauge_flush_poll).add();
                    else if (due) { g_gauge_flush_deadline.add(); ft::mark("submit.poll_deadline"); }
                    flush_batch();      // nothing more to record for now (or a thread waits for it): the GPU gets what there is
                    if (g->flushed >= g->flush_poll_serial.load()) g->flush_deadline_ns.store(0);   // else the polled submission is still to be recorded: stays armed
                    lk.lock();
                }
            } else if (g->flush_deadline_ns.load() && g->flushed >= g->flush_poll_serial.load() && g->flush_deadline_ns.exchange(0)) {
                g_gauge_flush_present.add();    // the batch went with the present (or filled up) while a deferred poll was pending
            }
            if (g->frame_batch && g->poll_flush_ms != 0) {      // poll_flush summary line, every 1200 presents
                const uint64_t pr = g->ctr.presents.load(std::memory_order_relaxed);
                if (pr >= g->pf_logged_presents + 1200) {
                    g->pf_logged_presents = pr;
                    NXLOG_I(kTag, "poll_flush: %lld polls deferred (sync %lld); flushes by poll %lld, deadline %lld, present %lld, wait %lld; submits %llu, presents %llu",
                            static_cast<long long>(g_gauge_polls_deferred.get()), static_cast<long long>(g_gauge_polls_deferred_sync.get()),
                            static_cast<long long>(g_gauge_flush_poll.get()), static_cast<long long>(g_gauge_flush_deadline.get()),
                            static_cast<long long>(g_gauge_flush_present.get()), static_cast<long long>(g_gauge_flush_wait.get()),
                            static_cast<unsigned long long>(g->ctr.submits.load()), static_cast<unsigned long long>(pr));
                }
            }
            if (!g->stop && g->queue.empty()) {
                nxrt::HitchTimer ht(nxrt::kHpRenderIdle);      // the hitch line: the render thread waited for the game
                ft::SplitScope sp_idle(ft::kSplIdle);
                // A poll_flush = <ms> deadline wakes the render thread to flush; a deadline set while it waits wakes it to re-arm.
                const uint64_t dl0 = g->flush_deadline_ns.load();
                auto ready = [dl0] {
                    return g->stop || !g->queue.empty() || (g->flush_wanted.load() && !g->batch.empty()) || g->flush_deadline_ns.load() != dl0;
                };
                if (dl0 && dl0 != kPollDeferred && !g->batch.empty())
                    g->qcv.wait_until(lk, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(dl0)), ready);
                else
                    g->qcv.wait(lk, ready);
                if (g->queue.empty() && !g->stop) continue;    // woken to flush
            }
            if (g->queue.empty()) {   // stop requested and drained
                lk.unlock();
                flush_batch();
                return;
            }
            job = std::move(g->queue.front());
            g->queue.pop_front();
            g_gauge_render_queue.sub(1);
        }
        try {
            job();
        } catch (const std::exception& e) {
            NXLOG_E(kTag, "render job threw: %s", e.what());
        }
    }
}

bool create_slot(Slot& s) {
    gfx::Device& d = *g->dev;
    VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    ci.queueFamilyIndex = d.queue_family();
    if (vkCreateCommandPool(d.device(), &ci, nullptr, &s.pool) != VK_SUCCESS) return false;
    vk_gauge_command_pools().add(1);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(d.device(), &ai, &s.cb) != VK_SUCCESS) return false;
    vk_gauge_command_buffers().add(1);
    s.cbs[0] = s.cb;
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;   // quad_args_gpu: indexed indirect arguments a dispatch writes
    if (d.create_buffer(kUploadBytes, usage, gfx::MemoryUse::Upload, &s.upload, "nvn upload") != VK_SUCCESS) return false;
    return true;
}

VkDescriptorPool new_dpool() {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u << 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u << 13},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u << 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u << 12},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1u << 10},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1u << 10},
    };
    VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = 1u << 14;
    ci.poolSizeCount = sizeof(sizes) / sizeof(sizes[0]);
    ci.pPoolSizes = sizes;
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(g->dev->device(), &ci, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_gauge_descriptor_pools().add(1);
    return p;
}

// [diag] slow_submit_ms (default 30, [diag] stutter_logs): a vkQueueSubmit2 that took longer logs one line, at most one per second.
void note_slow_submit(double ms, size_t cbs, bool worker) {
    static const bool on = config_get_bool("diag", "stutter_logs", true);
    static const double limit = static_cast<double>(config_get_int("diag", "slow_submit_ms", 30));
    static std::atomic<int64_t> last_ns{INT64_MIN};
    if (!on || ms <= limit) return;
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t prev = last_ns.load(std::memory_order_relaxed);
    if (prev != INT64_MIN && now - prev < 1000000000ll) return;
    if (!last_ns.compare_exchange_strong(prev, now)) return;
    NXLOG_I(kTag, "slow submission %.1f ms (%zu command buffers, worker %d, lazy live %lld MiB, memory objects %lld)", ms, cbs, worker ? 1 : 0,
            static_cast<long long>(pool_lazy_stats().live_bytes >> 20), static_cast<long long>(vk_gauge_memory_allocs().get()));
}

// Hands the recorded submissions of the batch to the GPU in one vkQueueSubmit2, in recording order, signalling the timeline with the last
// serial (the earlier ones of the batch complete with it). Every kernel submission costs a pass over all memory the device holds: on Linux
// (Proton, RADV) the multi-gigabyte host-memory pool imports are user pages the kernel looks up again on each one, about 2 ms a
// submission on a Legion Go S, so 11 submissions a frame took 20 of its 25 ms. Render thread.
// The kernel submission of flush_batch: inline on the render thread, or as a job on the submit worker ([nvn] submit_thread; worker = true:
// no render-thread hitch phases, and the serial is published as submitted afterwards). serial 0 ([nvn] gpu_split): the command buffers are
// pieces of a submission still being recorded and signal nothing; the timeline value comes with the submission's last piece.
void submit_cbs(const std::vector<VkCommandBufferSubmitInfo>& cbi, uint64_t serial, bool worker) {
    if (worker && g->dev->device_lost()) {      // lost after the hand-over: nothing goes to the queue, the waiters are released
        complete_serial(serial);
        g->sw->publish(serial);
        return;
    }
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = g->timeline;
    sig.value = serial;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = static_cast<uint32_t>(cbi.size());
    si.pCommandBufferInfos = cbi.data();
    si.signalSemaphoreInfoCount = serial ? 1 : 0;
    si.pSignalSemaphoreInfos = serial ? &sig : nullptr;
    g->queue_submits.fetch_add(1, std::memory_order_relaxed);
    VkResult r;
    if (worker) {
        std::lock_guard<std::mutex> lk(g->dev->queue_mutex());
        const auto s0 = std::chrono::steady_clock::now();
        r = vkQueueSubmit2(g->dev->queue(), 1, &si, VK_NULL_HANDLE);
        note_slow_submit(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count(), cbi.size(), true);
    } else {
        nxrt::HitchTimer ht(nxrt::kHpRenderSubmit);
        const uint64_t tl = nxrt::hitch_ticks();
        std::lock_guard<std::mutex> lk(g->dev->queue_mutex());
        nxrt::hitch_phase_add(nxrt::kHpRenderLockWait, nxrt::hitch_ticks() - tl);
        const auto s0 = std::chrono::steady_clock::now();
        r = vkQueueSubmit2(g->dev->queue(), 1, &si, VK_NULL_HANDLE);
        note_slow_submit(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count(), cbi.size(), false);
    }
    if (r != VK_SUCCESS) {
        g->dev->note_result(r, "vkQueueSubmit2 (NVN)");
        if (!g->dev->device_lost()) {
            NXLOG_E(kTag, r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? "vkQueueSubmit2 failed: VK_ERROR_OUT_OF_DEVICE_MEMORY (%d)" : "vkQueueSubmit2 failed (%d)", static_cast<int>(r));
            // A recoverable submit failure still needs to release timeline waiters.
            VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            ssi.semaphore = g->timeline;
            ssi.value = serial;
            if (serial) vkSignalSemaphore(g->dev->device(), &ssi);
        } else {
            // Vulkan calls may keep returning DEVICE_LOST. Complete the emulated serials on the CPU
            // so game threads leave their waits and reach the main-thread fatal error path.
            complete_serial(serial);
        }
    }
    if (worker) g->sw->publish(serial);
}

// [nvn] one_submit: the worker job of a parked batch flush. When a present is already queued on the render thread, it waits up to
// [nvn] one_submit_hold_ms for that present to take the flush (zone submit.park.hold, arg = how it ended: 1 taken by the present,
// 2 released by a CPU wait or a drain, 3 hold time over); a CPU wait for one of its serials (wait_submitted) or a drain ends the wait at once.
// Taken by the present: nothing to do here, the present's submit publishes the serial. Worker thread.
void park_job(uint64_t serial) {
    using Took = SubmitPark<Gpu::Cbi>::Took;
    bool coming = false;
    if (g->park_hold.count() > 0) {
        std::lock_guard<std::mutex> lk(g->pmu);
        coming = g->presents_pending > 0;
    }
    Took how = Took::Now;
    std::optional<Gpu::Cbi> cbi;
    {
        static const uint16_t z_hold = ft::zone_id("submit.park.hold");
        const uint64_t t0 = coming && ft::on() ? ft::now_ns() : 0;
        cbi = g->park.take_for_job(serial, g->park_hold, coming, &how);
        if (t0) ft::event(z_hold, t0, ft::now_ns(), static_cast<uint32_t>(how));
    }
    if (!cbi) return;
    if (how == Took::Expired) g_gauge_park_expired.add(1);
    static const uint16_t z_batch = ft::zone_id("submit.worker.batch");     // as flush_batch's own job
    ft::Scope sc(z_batch, 0, static_cast<uint32_t>(cbi->size()));
    submit_cbs(*cbi, serial, true);
}

// Frame trace: the time the slots' first command buffer was handed to vkQueueSubmit2 (gpu.exec.queued, acquire_slot). Any thread.
void stamp_queued(const std::vector<Slot*>& slots) {
    if (slots.empty()) return;
    const uint64_t t = ft::now_ns();
    for (Slot* s : slots) {
        uint64_t none = 0;
        s->queued_ns.compare_exchange_strong(none, t);
    }
}

// piece ([nvn] gpu_split, split_piece): also the slot's command buffer being recorded, after the batch's, in the same vkQueueSubmit2; it signals
// what the batch signals (nothing when the batch is empty). direct: sent at once, never parked for the present ([nvn] one_submit), and what is
// parked goes first with its own jobs: the point of a piece, and of the last piece of a split submission, is to reach the GPU now.
void flush_batch(Slot* piece, bool direct) {
    if (g->batch.empty() && !piece) return;
    // [nvn] one_submit: flushes a present took must reach the worker before any later serial, or the timeline would be signalled backwards
    // (nothing flushes between present_take_parked and take_batch_for_present today; this keeps it so).
    if (g->one_submit && !g->taken.empty()) present_give_back();
    ft::SplitScope sp_submit(ft::kSplSubmit);     // [prof] render_split: with the worker, only the hand-over
    if (g->dev->device_lost()) {     // a piece goes nowhere: its submission completes on the CPU like everything else
        if (g->batch.empty()) return;
        const uint64_t serial = g->batch.back()->serial;
        complete_serial(serial);
        if (g->sw) g->sw->publish(serial);
        g->flushed = serial;
        g->flushed_pub.store(serial);
        g->flush_wanted.store(false);
        g->batch.clear();
        return;
    }
    std::vector<VkCommandBufferSubmitInfo> cbi(g->batch.size() + (piece ? 1 : 0), VkCommandBufferSubmitInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO});
    for (size_t i = 0; i < g->batch.size(); i++) cbi[i].commandBuffer = g->batch[i]->cb;
    if (piece) cbi.back().commandBuffer = piece->cb;
    const uint64_t serial = g->batch.empty() ? 0 : g->batch.back()->serial;
    std::vector<Slot*> stamp;
    if (g->qpool) {
        stamp = g->batch;
        if (piece) stamp.push_back(piece);
    }
    if (g->sw && g->one_submit && !direct) {   // [nvn] one_submit: its job submits it, unless the next present takes it first (still "handed over")
        g->park.park(g->flushed + 1, serial, std::move(cbi));
        g->sw->push([serial] { park_job(serial); });
    } else if (g->sw) {    // [nvn] submit_thread: the worker submits; `flushed` = handed over (it will reach the queue without being asked again)
        if (direct && g->one_submit) g->park.release_all();     // parked flushes: their jobs, pushed earlier, submit them first
        g->sw->push([cbi = std::move(cbi), serial, stamp = std::move(stamp), with_piece = piece != nullptr] {
            // the batch's vkQueueSubmit2 (+ the queue lock wait); arg = command buffers in it. The present job is submit.worker.present.
            // submit.worker.piece: the same with a piece of a submission still being recorded ([nvn] gpu_split).
            static const uint16_t z_batch = ft::zone_id("submit.worker.batch"), z_piece = ft::zone_id("submit.worker.piece");
            ft::Scope sc(with_piece ? z_piece : z_batch, 0, static_cast<uint32_t>(cbi.size()));
            stamp_queued(stamp);
            submit_cbs(cbi, serial, true);
        });
    } else {
        stamp_queued(stamp);
        submit_cbs(cbi, serial, false);
    }
    if (serial) {
        g->flushed = serial;
        g->flushed_pub.store(serial);
        g->flush_wanted.store(false);
    }
    g->batch.clear();
    g->ctr.submits.fetch_add(1);
}

// [nvn] submit_thread: waits until the worker has submitted `serial` (or it completed another way). Any thread. true without the worker.
bool wait_submitted(uint64_t serial, uint64_t timeout_ns = UINT64_MAX) {
    if (!g->sw || !serial || g->sw->submitted() >= serial) return true;
    if (g->one_submit) {    // [nvn] one_submit: a CPU wait sends its serial now, never held back for a present
        g->park.release(serial);
        // the render thread itself, inside a present that has taken parked flushes: it cannot wait for that present's submit
        if (on_render_thread() && !g->taken.empty() && serial >= g->taken.front().first) present_give_back();
    }
    NXFT("submit.wait_submitted");
    return g->sw->wait_submitted(serial, timeout_ns, [serial] { return g->completed.load() >= serial || g->dev->device_lost(); });
}

// submit_batch = frame: a thread outside the render thread waits for a serial that may still sit in the batch; the render thread flushes
// it at once instead of with the next present (which that thread may be the one to queue).
// by_wait: a blocking wait asks (a poll may be deferred, see poll_flush_request; a wait never is).
void want_flush(uint64_t serial, bool by_wait = true) {
    if (!g->frame_batch || serial <= g->flushed_pub.load()) return;
    if (by_wait) g->flush_by_wait.store(true);
    if (g->flush_wanted.exchange(true)) return;
    std::lock_guard<std::mutex> lk(g->qmu);
    g->qcv.notify_one();
}

// [nvn] poll_flush: a poll of a serial still in the batch. now: flush at once (want_flush). defer / <ms>: leave it in the batch; the first deferred poll of
// the batch arms the deadline (or the no-deadline marker) and, with a deadline, wakes the render thread so its idle wait ends at it. Any thread.
void poll_flush_request(uint64_t serial, PollKind kind) {
    if (!g->frame_batch || serial <= g->flushed_pub.load()) return;
    if (g->poll_flush_ms == 0 || kind != PollKind::Sync) {
        want_flush(serial, kind == PollKind::Wait);
        return;
    }
    ft::mark("submit.poll_deferred");      // the poll that did not flush
    g_gauge_polls_deferred.add();
    g_gauge_polls_deferred_sync.add();
    for (uint64_t cur = g->flush_poll_serial.load(); cur < serial && !g->flush_poll_serial.compare_exchange_weak(cur, serial);) {}
    uint64_t none = 0;
    const uint64_t dl = g->poll_flush_ms < 0 ? kPollDeferred : steady_ns() + static_cast<uint64_t>(g->poll_flush_ms) * 1000000ull;
    if (g->flush_deadline_ns.compare_exchange_strong(none, dl) && g->poll_flush_ms > 0) {
        std::lock_guard<std::mutex> lk(g->qmu);
        g->qcv.notify_one();
    }
}

bool wait_serial_blocking(uint64_t serial) {
    if (!serial || read_completed() >= serial) return true;
    if (g->dev->device_lost()) {
        complete_serial(serial);
        return false;
    }
    if (serial > g->flushed && on_render_thread()) flush_batch();     // a serial still in the batch would never signal
    wait_submitted(serial);     // [nvn] submit_thread: the worker has it in the queue first
    if (g->dev->device_lost()) {
        complete_serial(serial);
        return false;
    }
    NXFT("gpu.wait");
    ft::SplitScope sp_gw(on_render_thread() ? ft::kSplGpuWait : ft::kSplBuckets);
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g->timeline;
    wi.pValues = &serial;
    VkResult r = vkWaitSemaphores(g->dev->device(), &wi, UINT64_MAX);
    g->dev->note_result(r, "vkWaitSemaphores (NVN)");
    if (r != VK_SUCCESS && !g->dev->device_lost()) NXLOG_E(kTag, "vkWaitSemaphores failed (%d)", static_cast<int>(r));
    if (g->dev->device_lost()) complete_serial(serial);
    else read_completed();
    return r == VK_SUCCESS;
}

Slot* acquire_slot() {
    NXFT_MIN("slot.acquire", 20000);
    Slot& s = g->slots[g->next_slot];
    g->next_slot = (g->next_slot + 1) % kSlots;
    {
        nxrt::HitchTimer ht(nxrt::kHpRenderGpuWait);
        if (!wait_serial_blocking(s.serial)) return nullptr;
    }
    const uint64_t queued = s.queued_ns.exchange(0);
    if (s.timed) {
        s.timed = false;
        uint64_t ts[2] = {0, 0};
        const uint32_t idx = static_cast<uint32_t>(&s - g->slots) * 2;
        if (vkGetQueryPoolResults(g->dev->device(), g->qpool, idx, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) {
            // gpu.exec: drawn from the recording start. gpu.exec.queued: the same GPU time drawn from when the submission's first command buffer
            // was handed to vkQueueSubmit2 (the GPU cannot start before; it starts later when it is still busy with earlier work). Missing for a
            // flush that rode in the present's submit after being parked ([nvn] one_submit).
            static const uint16_t z = ft::zone_id("gpu.exec"), zq = ft::zone_id("gpu.exec.queued");
            const double ns = static_cast<double>(ts[1] - ts[0]) * g->dev->properties().limits.timestampPeriod;
            ft::event(z, s.cpu_t0, s.cpu_t0 + static_cast<uint64_t>(ns), static_cast<uint32_t>(s.serial));
            if (queued) ft::event(zq, queued, queued + static_cast<uint64_t>(ns), static_cast<uint32_t>(s.serial));
        }
    }
    if (s.gp_n >= 2 && g->gpool) {      // GPU pass profiler: one event per interval between two timestamps, named gp:<label>, arg = draws
        uint64_t ts[kGpMax];
        const uint32_t n = std::min(s.gp_n, kGpMax);
        const uint32_t base = static_cast<uint32_t>(&s - g->slots) * kGpMax;
        if (vkGetQueryPoolResults(g->dev->device(), g->gpool, base, n, sizeof(uint64_t) * n, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            static std::map<std::string, uint16_t> zones;
            const double period = g->dev->properties().limits.timestampPeriod;
            uint64_t t = s.cpu_t0;
            for (uint32_t i = 1; i < n && i < s.gp_labels.size(); i++) {
                const uint64_t d = ts[i] >= ts[i - 1] ? static_cast<uint64_t>(static_cast<double>(ts[i] - ts[i - 1]) * period) : 0;
                auto it = zones.find(s.gp_labels[i].first);
                if (it == zones.end()) it = zones.emplace(s.gp_labels[i].first, ft::zone_id(("gp:" + s.gp_labels[i].first).c_str())).first;
                ft::event(it->second, t, t + d, s.gp_labels[i].second);
                t += d;
            }
        }
    }
    s.gp_n = 0;
    s.gp_labels.clear();
    retire();
    vkResetCommandPool(g->dev->device(), s.pool, 0);
    s.cb = s.cbs[0];
    s.piece = 0;
    for (VkDescriptorPool p : s.dpools) vkResetDescriptorPool(g->dev->device(), p, 0);
    vk_gauge_descriptor_sets().sub(static_cast<int64_t>(s.sets_live));   // the reset freed them
    s.sets_live = 0;
    s.dpool_cur = 0;
    s.upload_off = 0;
    for (auto& b : s.extra) g->dev->destroy_buffer(b);
    s.extra.clear();
    return &s;
}

}  // namespace

// ---- Frame ---------------------------------------------------------------------------------------------------------------
UploadAlloc Frame::upload(VkDeviceSize size, VkDeviceSize align) {
    Slot& s = *static_cast<Slot*>(slot);
    if (align == 0) align = 16;
    VkDeviceSize off = (s.upload_off + align - 1) & ~(align - 1);
    if (off + size <= kUploadBytes) {
        s.upload_off = off + size;
        UploadAlloc a;
        a.buffer = s.upload.buffer;
        a.offset = off;
        a.ptr = static_cast<uint8_t*>(s.upload.mapped) + off;
        a.size = size;
        return a;
    }
    return staging(size + align);
}

UploadAlloc Frame::staging(VkDeviceSize size) {
    Slot& s = *static_cast<Slot*>(slot);
    gfx::Buffer b;
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;   // quad_args_gpu: indexed indirect arguments a dispatch writes
    if (g->dev->create_buffer(std::max<VkDeviceSize>(size, 256), usage, gfx::MemoryUse::Upload, &b, "nvn staging") != VK_SUCCESS) {
        NXLOG_E(kTag, "staging buffer of %llu bytes failed", static_cast<unsigned long long>(size));
        return {};
    }
    UploadAlloc a;
    a.buffer = b.buffer;
    a.offset = 0;
    a.ptr = static_cast<uint8_t*>(b.mapped);
    a.size = size;
    s.extra.push_back(b);
    return a;
}

UploadAlloc Frame::adopt_staging(gfx::Buffer b) {
    Slot& s = *static_cast<Slot*>(slot);
    UploadAlloc a;
    a.buffer = b.buffer;
    a.offset = 0;
    a.ptr = static_cast<uint8_t*>(b.mapped);
    a.size = b.size;
    s.extra.push_back(b);
    return a;
}

void Frame::defer(std::function<void()> fn) {
    g->deferred.push_back({serial, std::move(fn)});
    g_gauge_deferred.add(1);
}

VkDescriptorSet Frame::alloc_set(VkDescriptorSetLayout layout) {
    Slot& s = *static_cast<Slot*>(slot);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (s.dpool_cur >= s.dpools.size()) {
            VkDescriptorPool p = new_dpool();
            if (!p) return VK_NULL_HANDLE;
            s.dpools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.dpools[s.dpool_cur];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult r = vkAllocateDescriptorSets(g->dev->device(), &ai, &set);
        if (r == VK_SUCCESS) {
            g->ctr.descriptor_sets.fetch_add(1, std::memory_order_relaxed);
            s.sets_live++;
            vk_gauge_descriptor_sets().add(1);
            return set;
        }
        ++s.dpool_cur;   // exhausted or fragmented: move on to the next pool (created on demand)
    }
    return VK_NULL_HANDLE;
}

// ---- general helpers ---------------------------------------------------------------------------------------------------
bool on_render_thread() { return g && std::this_thread::get_id() == g->tid; }
gfx::Device& device() { return *g->dev; }
VkDevice vkdev() { return g->dev->device(); }
Counters& counters() { return g->ctr; }
Counters* counters_if() { return g ? &g->ctr : nullptr; }
uint64_t completed_serial() { return g ? g->completed.load() : 0; }
uint64_t poll_completed() { return g ? read_completed() : 0; }
uint64_t current_serial() { return g ? (g->recording ? g->recording : g->submitted) : 0; }

void note_unsupported(const char* what) {
    if (!g) return;
    g->ctr.unsupported_ops.fetch_add(1);
    std::lock_guard<std::mutex> lk(g->unsupported_mu);
    uint64_t& n = g->unsupported[what];
    if (n++ == 0) NXLOG_W(kTag, "not implemented, skipped: %s", what);
}

void defer_destroy(std::function<void()> fn) {
    if (!g) {
        fn();
        return;
    }
    std::lock_guard<std::mutex> lk(g->dmu);
    g->pending_destroy.push_back(std::move(fn));
    g_gauge_deferred.add(1);
}

void run_on_render_thread(const std::function<void()>& fn) {
    if (!g) return;
    if (on_render_thread()) {
        fn();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    {
        std::lock_guard<std::mutex> lk(g->qmu);
        g->queue.push_back([&] {
            fn();
            std::lock_guard<std::mutex> l2(m);
            done = true;
            cv.notify_all();
        });
        g_gauge_render_queue.add(1);
    }
    g->qcv.notify_one();
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return done; });
}

bool gpass_on() { return g && g->gpool != VK_NULL_HANDLE; }

void gpass_stamp(Frame& f, const char* label, uint32_t arg) {
    if (!g || !g->gpool || !f.slot) return;
    Slot& s = *static_cast<Slot*>(f.slot);
    if (s.gp_n >= kGpMax) return;
    vkCmdWriteTimestamp2(f.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g->gpool, static_cast<uint32_t>(&s - g->slots) * kGpMax + s.gp_n);
    s.gp_labels.emplace_back(label, arg);
    s.gp_n++;
}

namespace {
// [nvn] pass_barriers = tracked (S38): a layout change waits only for the stages that can have used the image in its old layout and blocks
// only the stages that use it in the new one. The layout says how the image was used: the replay keeps every image in the layout of its
// last use (attachments in the attachment layouts, sampled images in the read-only layouts, copies in the transfer layouts); GENERAL and
// anything else keep the full barrier.
std::atomic<bool> g_narrow_barriers{false};

constexpr VkPipelineStageFlags2 kShaderStages = VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 kDepthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

// Stages and accesses of an image's use in `layout`; writes_only: the access part names the writes only (the source half of a barrier).
void layout_scope(VkImageLayout layout, bool src, VkPipelineStageFlags2* stage, VkAccessFlags2* access) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            // the old contents do not matter, but the memory may have belonged to something still in flight: wait for all earlier commands
            *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            *access = src ? VK_ACCESS_2_NONE : (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            return;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            *access = src ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            return;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            *stage = kDepthStages;
            *access = src ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                          : (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            return;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            *stage = kDepthStages | kShaderStages;
            *access = src ? VK_ACCESS_2_NONE : (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            return;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            *stage = kShaderStages;
            *access = src ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            return;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            *access = src ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_READ_BIT;
            return;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            *access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            return;
        default:
            *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            *access = src ? VK_ACCESS_2_MEMORY_WRITE_BIT : (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            return;
    }
}
}  // namespace

void set_narrow_barriers(bool on) { g_narrow_barriers.store(on, std::memory_order_relaxed); }

void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect, uint32_t base_level,
                   uint32_t level_count, uint32_t base_layer, uint32_t layer_count) {
    g_gauge_barriers_img.add(1);
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    if (g_narrow_barriers.load(std::memory_order_relaxed)) {
        layout_scope(from, true, &b.srcStageMask, &b.srcAccessMask);
        layout_scope(to, false, &b.dstStageMask, &b.dstAccessMask);
    }
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, base_level, level_count, base_layer, layer_count};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    ft::split_count(ft::kCntCmdBarrier);
    vkCmdPipelineBarrier2(cb, &di);
}

void memory_barrier(VkCommandBuffer cb) {
    g_gauge_barriers_mem.add(1);
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    ft::split_count(ft::kCntCmdBarrier);
    vkCmdPipelineBarrier2(cb, &di);
}

// ---- submission --------------------------------------------------------------------------------------------------------
namespace {

// `serial`: the one reserved when the job was queued (queue_submit). 0: a submission made inside a render job (texture dumps of the trace):
// it takes the next serial when nothing queued holds one, else it signals nothing and the queue is drained after it.
uint64_t submit_on_render_thread(const std::function<void(Frame&)>& record, uint64_t serial = 0) {
    NXFT("replay");
    ft::SplitScope sp_replay(ft::kSplReplay);
    nxrt::HitchTimer ht_replay(nxrt::kHpRenderReplay);
    const auto t0 = std::chrono::steady_clock::now();
    if (g->dev->device_lost()) {
        if (serial) {
            g->submitted = std::max(g->submitted, serial);
            g->last_submitted.store(g->submitted);
            complete_serial(serial);
        }
        return serial;
    }
    static const uint16_t z_cbsetup = ft::zone_id("rp.cbsetup"), z_cbend = ft::zone_id("rp.cbend");
    const uint64_t t_cb0 = ft::on() ? ft::now_ns() : 0;
    Slot* acquired = acquire_slot();
    if (!acquired) {
        if (serial) {
            g->submitted = std::max(g->submitted, serial);
            g->last_submitted.store(g->submitted);
            complete_serial(serial);
        }
        return serial;
    }
    Slot& s = *acquired;
    bool unsignalled = false;
    if (!serial) {
        std::lock_guard<std::mutex> lk(g->qmu);
        if (g->reserved.load() == g->submitted) serial = g->reserved.fetch_add(1) + 1;
        else { serial = g->submitted; unsignalled = true; }
    }
    g->recording = serial;
    Frame f;
    f.cb = s.cb;
    f.serial = serial;
    f.slot = &s;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cb, &bi);
    const uint32_t qi = static_cast<uint32_t>(&s - g->slots) * 2;
    if (g->gpool) {
        vkCmdResetQueryPool(s.cb, g->gpool, static_cast<uint32_t>(&s - g->slots) * kGpMax, kGpMax);
        s.gp_n = 0;
        s.gp_labels.clear();
        gpass_stamp(f, "(start)", 0);
    }
    if (g->qpool) {
        vkCmdResetQueryPool(s.cb, g->qpool, qi, 2);
        vkCmdWriteTimestamp(s.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g->qpool, qi);
        s.timed = true;
        s.cpu_t0 = ft::now_ns();
    }
    if (t_cb0) ft::event(z_cbsetup, t_cb0, ft::now_ns());   // slot, begin command buffer, query resets
    record(f);      // [nvn] gpu_split: may have sent pieces (split_piece); s.cb is then the last one
    const uint64_t t_cb1 = ft::on() ? ft::now_ns() : 0;
    if (g->gpool) gpass_stamp(f, "tail", 0);
    if (g->qpool) vkCmdWriteTimestamp(s.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g->qpool, qi + 1);
    vkEndCommandBuffer(s.cb);
    if (!unsignalled) {
        s.serial = serial;
        g->submitted = serial;
        g->last_submitted.store(serial);
        g->recording = 0;
        g->batch.push_back(&s);
        if (s.piece) {      // [nvn] gpu_split: the last piece follows the others at once (it signals the serial)
            g->split_subs.fetch_add(1, std::memory_order_relaxed);
            flush_batch(nullptr, true);
        } else if (g->batch.size() >= g->batch_max) {
            flush_batch();
        }
        if (t_cb1) ft::event(z_cbend, t_cb1, ft::now_ns());     // end command buffer, the batch flush
        g->ctr.submit_replay_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
        return serial;
    }
    flush_batch();      // debug path: submitted on its own, after everything recorded before it, signalling nothing
    submit_drain();     // [nvn] submit_thread: ... and after the worker has submitted that
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = s.cb;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VkResult r;
    {
        nxrt::HitchTimer ht(nxrt::kHpRenderSubmit);
        const uint64_t tl = nxrt::hitch_ticks();
        std::lock_guard<std::mutex> lk(g->dev->queue_mutex());
        nxrt::hitch_phase_add(nxrt::kHpRenderLockWait, nxrt::hitch_ticks() - tl);
        r = vkQueueSubmit2(g->dev->queue(), 1, &si, VK_NULL_HANDLE);
        if (r == VK_SUCCESS) r = vkQueueWaitIdle(g->dev->queue());   // nothing else tells when it is done
    }
    g->dev->note_result(r, "vkQueueSubmit2 (NVN debug)");
    g->recording = 0;
    s.serial = 0;
    return serial;
}

}  // namespace

void submit_recorded() {
    if (g) flush_batch();
}

// ---- [nvn] gpu_split ---------------------------------------------------------------------------------------------------
// A large submission (the game's scene: several ms of recording) used to reach the queue only once it was recorded to the end, and with
// submit_batch = frame only with the present: the GPU idled while the render thread recorded it. The replay calls split_piece at a safe point
// (nvn_replay.cpp piece_point) once a piece has taken split_ticks of recording: the command buffer so far goes to the queue now, with the
// batch recorded before it, in one vkQueueSubmit2 that signals what the batch signals and nothing of this submission; recording goes on in
// the next command buffer of the same slot. The last piece carries the serial (submit_on_render_thread flushes it at once, never parked), so
// everything keyed to the serial (sync objects, slot reuse, Frame::defer, pool landing, readbacks, the frame's GPU-done) waits for all of it:
// the queue runs the pieces in order and a timeline signal covers everything submitted before it. Descriptor sets, the upload ring and the
// staging buffers belong to the slot, so they live until that serial completes, as before. Render thread.
uint64_t split_budget_ticks() { return g ? g->split_ticks : 0; }
uint32_t split_max_pieces() { return g ? g->split_max : 1; }
void split_note_blocked() { if (g) g->split_blocked.fetch_add(1, std::memory_order_relaxed); }

bool split_piece(Frame& f) {
    if (!g || !f.slot || !f.serial || g->dev->device_lost()) return false;
    Slot& s = *static_cast<Slot*>(f.slot);
    if (f.cb != s.cb || s.piece + 1 >= kMaxPieces) return false;
    VkCommandBuffer& next = s.cbs[s.piece + 1];
    if (!next) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(g->dev->device(), &ai, &next) != VK_SUCCESS) {
            next = VK_NULL_HANDLE;
            return false;
        }
        vk_gauge_command_buffers().add(1);
    }
    static const uint16_t z_split = ft::zone_id("submit.split");    // end the piece, hand it over, begin the next; arg = pieces sent so far
    ft::Scope sc(z_split, 0, s.piece + 1);
    vkEndCommandBuffer(s.cb);
    flush_batch(&s, true);
    s.piece++;
    s.cb = next;
    f.cb = next;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(next, &bi);
    if (g->split_pieces.fetch_add(1, std::memory_order_relaxed) == 0)
        NXLOG_I(kTag, "gpu_split: first piece sent while its submission (serial %llu) is still being recorded", static_cast<unsigned long long>(f.serial));
    return true;
}

namespace {
// One line per 10 s while gpu_split is on (render thread, at each present): per frame, the pieces sent early and the submissions they came
// from, the kernel submissions (vkQueueSubmit2 calls of nvn_gpu.cpp plus the present's own) and the safe points a due split could not use.
void split_summary() {
    if (!g->split_ticks) return;
    static uint64_t t_last = 0, p_last = 0, pieces_last = 0, subs_last = 0, blocked_last = 0, queue_last = 0;
    const uint64_t now = steady_ns(), presents = g->ctr.presents.load(std::memory_order_relaxed);
    const uint64_t pieces = g->split_pieces.load(std::memory_order_relaxed), subs = g->split_subs.load(std::memory_order_relaxed);
    const uint64_t blocked = g->split_blocked.load(std::memory_order_relaxed), queue = g->queue_submits.load(std::memory_order_relaxed);
    if (t_last && now - t_last < 10000000000ull) return;
    if (t_last && presents > p_last) {
        const double f = static_cast<double>(presents - p_last);
        NXLOG_I(kTag, "gpu_split, %llu frames, per frame: %.2f pieces sent early from %.2f split submissions, %.2f kernel submissions (%.2f + the present), %.2f split points blocked by an open query or label",
                static_cast<unsigned long long>(presents - p_last), static_cast<double>(pieces - pieces_last) / f, static_cast<double>(subs - subs_last) / f,
                static_cast<double>(queue - queue_last) / f + 1.0, static_cast<double>(queue - queue_last) / f, static_cast<double>(blocked - blocked_last) / f);
    }
    t_last = now;
    p_last = presents;
    pieces_last = pieces;
    subs_last = subs;
    blocked_last = blocked;
    queue_last = queue;
}
}  // namespace

void submit_drain() {
    if (!g || !g->sw) return;
    NXFT("submit.drain");
    if (g->one_submit) g->park.release_all();     // [nvn] one_submit: a parked flush is not held for a present while anything drains
    g->sw->drain();
}

void present_take_parked() {
    if (!g || !g->one_submit) return;
    for (auto& e : g->park.take_for_present()) g->taken.push_back(std::move(e));
    if (!g->taken.empty()) {
        static const uint16_t z_take = ft::zone_id("present.park.take");     // arg = batch flushes taken
        if (ft::on()) { const uint64_t t = ft::now_ns(); ft::event(z_take, t, t, static_cast<uint32_t>(g->taken.size())); }
    }
}

void present_give_back() {
    if (!g || g->taken.empty()) return;
    for (auto& e : g->taken)    // in serial order, each on its own as flush_batch's job would have (they were never published)
        g->sw->push([cbi = std::move(e.payload), serial = e.serial] {
            static const uint16_t z_batch = ft::zone_id("submit.worker.batch");
            ft::Scope sc(z_batch, 0, static_cast<uint32_t>(cbi.size()));
            submit_cbs(cbi, serial, true);
        });
    g->taken.clear();
}

bool take_batch_for_present(std::vector<VkCommandBuffer>* cmds, gfx::Renderer::SubmitPrefix* p) {
    if (g) split_summary();
    if (!g || !g->frame_batch || (g->batch.empty() && g->taken.empty())) {
        submit_recorded();
        return false;
    }
    cmds->clear();
    uint64_t value = 0;
    if (!g->taken.empty()) {    // [nvn] one_submit: the parked flushes first (earlier serials), then the frame's own batch
        const size_t n = g->taken.size();
        for (auto& e : g->taken) {
            for (const VkCommandBufferSubmitInfo& c : e.payload) cmds->push_back(c.commandBuffer);
            value = e.serial;
        }
        g_gauge_merged.add(static_cast<int64_t>(n));
        static bool logged = false;
        if (!logged) {
            logged = true;
            NXLOG_I(kTag, "one_submit: first merged present: %zu parked flush(es) + %zu batched + the frame's own command buffer in one vkQueueSubmit2 (%s)",
                    n, g->batch.size(), g->one_submit == 2 ? "split: 2 VkSubmitInfo2" : "1 VkSubmitInfo2");
        }
        g->taken.clear();
    }
    for (Slot* s : g->batch) cmds->push_back(s->cb);
    if (!g->batch.empty()) value = g->batch.back()->serial;
    p->cmds = cmds->data();
    p->count = static_cast<uint32_t>(cmds->size());
    p->timeline = g->timeline;
    p->value = value;
    p->split = g->one_submit == 2;
    if (g->sw) {    // [nvn] submit_thread: what present_batch_done does with the result happens on the worker, right after the submit
        const uint64_t value = p->value;
        std::vector<Slot*> stamp;
        if (g->qpool) stamp = g->batch;     // gpu.exec.queued: stamped just after the submit (before the publish that lets the slot be reused)
        p->after_submit = [value, stamp = std::move(stamp)](bool submitted) {
            stamp_queued(stamp);
            if (!submitted && !g->dev->device_lost()) {
                VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
                ssi.semaphore = g->timeline;
                ssi.value = value;
                vkSignalSemaphore(g->dev->device(), &ssi);
            } else if (!submitted) {
                complete_serial(value);
            }
            g->sw->publish(value);
        };
    }
    return true;
}

void present_batch_done(const gfx::Renderer::SubmitPrefix& p) {
    if (p.queued) {
        // [nvn] submit_thread: the worker runs p.after_submit. end_frame returned early (device lost) when it did not queue: below.
    } else if (!p.submitted && !g->dev->device_lost()) {   // as flush_batch: keep the timeline moving so waiters do not hang
        VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        ssi.semaphore = g->timeline;
        ssi.value = p.value;
        vkSignalSemaphore(g->dev->device(), &ssi);
    } else if (!p.submitted) {
        complete_serial(p.value);
    }
    if (g->sw && !p.queued) g->sw->publish(p.value);
    g->flushed = std::max(g->flushed, p.value);     // ([nvn] one_submit: a present of parked flushes only carries serials already flushed)
    g->flushed_pub.store(g->flushed);
    g->flush_wanted.store(false);
    g->batch.clear();
    g->ctr.submits.fetch_add(1);
}

// Queues a submission with the next serial (in queue order). wait: until the render thread has run it (and, with gpu_done, the GPU too).
uint64_t queue_submit(std::function<void(Frame&)> record, bool wait, bool gpu_done, bool flush_after = false) {
    if (g->dev->device_lost()) return g->reserved.load();
    if (on_render_thread()) {
        const uint64_t serial = submit_on_render_thread(record);
        if (flush_after) {
            if (g->one_submit && g->sw) g->park.release(serial);
            flush_batch();
        }
        if (gpu_done) { wait_serial_blocking(serial); retire(); }
        return serial;
    }
    struct Done { std::mutex m; std::condition_variable cv; bool done = false; };
    auto done = wait ? std::make_shared<Done>() : nullptr;
    uint64_t serial = 0;
    {
        std::lock_guard<std::mutex> lk(g->qmu);
        serial = g->reserved.fetch_add(1) + 1;
        g->queue.push_back([record = std::move(record), serial, done, gpu_done, flush_after] {
            submit_on_render_thread(record, serial);
            if (flush_after) {
                // Release is a threshold: it also covers the entry flush_batch is about to park.
                if (g->one_submit && g->sw) g->park.release(serial);
                flush_batch();
            }
            if (gpu_done) { wait_serial_blocking(serial); retire(); }
            if (done) {
                std::lock_guard<std::mutex> l2(done->m);
                done->done = true;
                done->cv.notify_all();
            }
        });
        g_gauge_render_queue.add(1);
    }
    g->qcv.notify_one();
    if (done) {
        std::unique_lock<std::mutex> lk(done->m);
        done->cv.wait(lk, [&] { return done->done; });
    }
    return serial;
}

uint64_t run_frame_sync(const std::function<void(Frame&)>& fn) {
    return queue_submit(fn, true, true);
}

// [nvn] readback_async (download_texture): the job is recorded in queue order (after every submission queued before it) and this returns
// once the render thread has recorded it. No GPU wait, on either thread; the caller waits for the serial itself (wait_serial_grace).
uint64_t queue_record(std::function<void(Frame&)> record) {
    return queue_submit(std::move(record), true, false);
}
// [nvn] readback_defer: returns at once with the reserved serial; the render thread records the job in queue order and flushes the batch
// right after it, so the copy reaches the GPU without any thread waiting for it. The flush is in the job itself on purpose: a wait_serial
// from the reading thread for a serial the render thread had not recorded yet made the render thread flush the batch before it, consume
// flush_wanted, and then hold the recorded job in the next batch until a present; when the reading thread was the presenting one, the game
// froze for good (#327, #331).
uint64_t queue_readback_post(std::function<void(Frame&)> record) {
    return queue_submit(std::move(record), false, false, true);
}

// [nvn] readback_defer: has the GPU finished the serial? A query only: wait_serial(serial, 0) would ask for a flush.
ReadbackPoll poll_readback_serial(uint64_t serial, ReadbackProgress* progress) {
    if (!g || !serial || !g->timeline || g->dev->device_lost()) return ReadbackPoll::Failed;
    uint64_t completed = 0;
    const VkResult result = vkGetSemaphoreCounterValue(g->dev->device(), g->timeline, &completed);
    if (result != VK_SUCCESS) {
        g->dev->note_result(result, "deferred readback completion poll");
        return ReadbackPoll::Failed;
    }
    if (progress) {
        progress->reserved = g->reserved.load();
        progress->flushed = g->flushed_pub.load();
        progress->submitted = g->last_submitted.load();
        progress->completed = completed;
    }
    return completed >= serial ? ReadbackPoll::Complete : ReadbackPoll::Pending;
}

// Gives the render thread up to grace_ns to hand the serial to the GPU on its own (with [nvn] submit_batch = frame the batch goes with the
// next present), polling every 0.5 ms, then waits for it on the GPU (wait_serial: a serial still in the batch is flushed at once, an
// extra kernel submission). grace_ns 0: wait_serial at once. Not on the render thread (it is the one that flushes).
bool wait_serial_grace(uint64_t serial, uint64_t grace_ns, WaitTimes* times) {
    if (!g || !serial) return true;
    const auto t0 = std::chrono::steady_clock::now();
    if (grace_ns > 0 && serial > g->flushed_pub.load()) {
        const auto end = t0 + std::chrono::nanoseconds(grace_ns);
        while (serial > g->flushed_pub.load() && g->completed.load() < serial && !g->dev->device_lost() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (!times) return wait_serial(serial, UINT64_MAX);
    const auto t1 = std::chrono::steady_clock::now();
    times->forced_flush = serial > g->flushed_pub.load() && g->completed.load() < serial;   // the render thread did not get to it: wait_serial flushes
    const bool ok = wait_serial(serial, UINT64_MAX);
    const auto t2 = std::chrono::steady_clock::now();
    times->grace_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    times->wait_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
    return ok;
}

// ---- gpu.h ---------------------------------------------------------------------------------------------------------------
bool active() { return g != nullptr; }
void cpu_will_read(uint64_t addr, uint64_t size) {
    if (g && out_any()) out_fetch(addr, size, false);
}
bool debug_labels() { return g && g->dev && g->dev->features().debug_labels; }

namespace {
std::atomic<uint64_t> g_idle_serial{0};   // without a back end submissions still get serials and complete at once (tests, tools)
}

// [nvn] set_reuse (default on): see build_set.
bool set_reuse_on() {
    static const bool on = nxrt::config_get_bool("nvn", "set_reuse", true);
    return on;
}

// [nvn] async_submit (default on): the game thread queues its command lists and goes on; the render thread records and submits them in
// order. The serial is known at once (reserved in queue order), so sync objects and waits work as before: a wait for a serial the render
// thread has not submitted yet waits on the timeline until it has (wait-before-signal).
bool async_submit() {
    static const bool on = nxrt::config_get_bool("nvn", "async_submit", true);
    return on;
}

uint64_t submit(const std::vector<Ref<CmdList>>& lists) {
    if (!g) return ++g_idle_serial;
    NXFT("submit");
    return queue_submit([lists](Frame& f) {
        replay_begin_submit(f);
        replay_lists(f, lists);
        replay_end_submit(f);
    }, !async_submit(), false);
}

uint64_t flush() { return g ? g->reserved.load() : g_idle_serial.load(); }
uint64_t last_serial() { return g ? g->reserved.load() : g_idle_serial.load(); }

void finish() {
    if (!g) return;
    run_on_render_thread([] {
        wait_serial_blocking(g->submitted);
        retire();
    });
}

bool serial_done(uint64_t serial, PollKind kind) {
    if (!g || !serial) return true;
    if (g->dev->device_lost()) {
        complete_serial(std::max(serial, g->reserved.load()));
        return true;
    }
    if (g->completed.load() >= serial) return true;
    if (read_completed() >= serial) return true;
    poll_flush_request(serial, kind);     // polled: it must reach the GPU to ever complete (poll_flush: now, or with the present / the deadline)
    return false;
}

namespace {
std::atomic<uint64_t> g_waits_begun{0}, g_waits_done{0};   // sync waits entered and left: a live graphics thread moves both
std::atomic<int> g_waiters{0};                 // game threads inside wait_serial (freeze report)
std::atomic<uint64_t> g_wait_serial{0}, g_wait_timeout{0};   // the latest wait: serial and timeout

std::string gpu_report() {
    if (!g) return {};
    uint64_t v = 0;
    if (g->dev->device_lost()) v = g->completed.load();
    else if (g->timeline) vkGetSemaphoreCounterValue(g->dev->device(), g->timeline, &v);
    char b[400];
    size_t q = 0;
    bool got = false;
    if (g->qmu.try_lock()) { q = g->queue.size(); got = true; g->qmu.unlock(); }
    std::snprintf(b, sizeof b, "NVN back end: last submitted serial %llu, timeline value %llu, %d game thread(s) in a sync wait (latest wait: serial %llu, timeout %llu ns), sync waits begun %llu done %llu, submits %llu, presents %llu, render queue %s%zu\n",
                  static_cast<unsigned long long>(g->last_submitted.load()), static_cast<unsigned long long>(v), g_waiters.load(),
                  static_cast<unsigned long long>(g_wait_serial.load()), static_cast<unsigned long long>(g_wait_timeout.load()),
                  static_cast<unsigned long long>(g_waits_begun.load()), static_cast<unsigned long long>(g_waits_done.load()),
                  static_cast<unsigned long long>(g->ctr.submits.load()), static_cast<unsigned long long>(g->ctr.presents.load()), got ? "" : "(busy) ", q);
    std::string out = b;
    if (g->sw) {
        std::snprintf(b, sizeof b, "NVN submit thread: submitted serial %llu, jobs queued or running %zu\n",
                      static_cast<unsigned long long>(g->sw->submitted()), g->sw->depth());
        out += b;
    }
    return out;
}
}  // namespace

bool wait_serial(uint64_t serial, uint64_t timeout_ns) {
    if (!g || !serial) return true;
    if (g->dev->device_lost()) {
        complete_serial(std::max(serial, g->reserved.load()));
        return true;
    }
    if (serial_done(serial, timeout_ns == 0 ? PollKind::Sync : PollKind::Wait)) return true;
    if (timeout_ns == 0) return false;
    // [nvn] one_submit: release before asking for the flush (release is a threshold, it also covers the entry parked by that flush), so a
    // present cannot take it between the flush and wait_submitted's release and make this wait follow the present's acquire.
    if (g->one_submit && g->sw) g->park.release(serial);
    want_flush(serial);
    NXFT("sync.wait");
    nxrt::HitchTimer ht(nxrt::kHpGameSyncWait);
    g_wait_serial.store(serial);
    g_wait_timeout.store(timeout_ns);
    struct Counted { Counted() { g_waiters++; g_waits_begun++; } ~Counted() { g_waiters--; g_waits_done++; } } counted;
    if (g->sw) {    // [nvn] submit_thread: wait for the worker to submit it first, within the same timeout
        const auto t0 = std::chrono::steady_clock::now();
        if (!wait_submitted(serial, timeout_ns)) return false;
        if (timeout_ns != UINT64_MAX) {
            const uint64_t spent = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            timeout_ns = spent >= timeout_ns ? 0 : timeout_ns - spent;
        }
        if (g->dev->device_lost()) {
            complete_serial(std::max(serial, g->reserved.load()));
            return true;
        }
    }
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g->timeline;
    wi.pValues = &serial;
    const auto w0 = std::chrono::steady_clock::now();
    const VkResult r = vkWaitSemaphores(g->dev->device(), &wi, timeout_ns);
    {   // a long wait of the game on the graphics card is logged (at most 20 lines a session): which serial, how long the game was willing to
        // wait, and where the queue stood, so a report of "the game suddenly runs at 4 fps" can say what it was waiting for
        static std::atomic<int> logged{0};
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
        if (ms >= 100.0 && logged.load(std::memory_order_relaxed) < 20 && logged.fetch_add(1, std::memory_order_relaxed) < 20)
            NXLOG_W(kTag, "slow sync wait: %.0f ms for serial %llu (timeout %llu ns, result %d), last submitted %llu, completed %llu", ms,
                    static_cast<unsigned long long>(serial), static_cast<unsigned long long>(timeout_ns), static_cast<int>(r),
                    static_cast<unsigned long long>(g->last_submitted.load()), static_cast<unsigned long long>(g->completed.load()));
    }
    g->dev->note_result(r, "vkWaitSemaphores (NVN sync)");
    if (g->dev->device_lost()) complete_serial(std::max(serial, g->reserved.load()));
    else read_completed();
    return r == VK_SUCCESS;
}

namespace {
// The last serial queued before each of the game's presents (the end of that frame's GPU work), for the low-latency GPU wait (lowlat.h).
// Written by the game thread in present(), read by the main thread at the frame boundary (the same thread in Pokopia).
std::atomic<uint64_t> g_frame_work[4];
std::atomic<uint64_t> g_frame_count{0};

bool gpu_wait_frames(int frames_back, uint64_t timeout_ns) {
    const uint64_t n = g_frame_count.load(std::memory_order_acquire);
    if (!g || n <= static_cast<uint64_t>(frames_back)) return true;
    const uint64_t serial = g_frame_work[(n - 1 - static_cast<uint64_t>(frames_back)) & 3].load(std::memory_order_relaxed);
    NXFT("lowlat.gpu_wait");
    return wait_serial(serial, timeout_ns);
}
}  // namespace

// [nvn] present_ahead (default 1; 0 = wait for each present): the game thread queues the present and goes on while fewer than this many
// presents are still pending on the render thread (the frame limiter and the swapchain's frame slots pace the render thread). 1 keeps the
// render thread one present behind the game at most (2 let a second finished frame wait in the queue: one more frame between input and screen).
void present(Window* window, int index) {
    if (!g || !window || g->dev->device_lost()) return;
    NXFT("present.call");
    nxrt::hitch_note_game_present();
    // the frame boundary's limiter wait for the next frame is due from here (lowlat.h); the frame's presentation record starts (present_timing.h)
    const uint64_t frame = extras::present_timing_game_present(extras::pace_note_game_present());
    {
        std::lock_guard<std::mutex> lk(g->pmu);
        g->last_window = Ref<Window>::share(window);
        g->last_index = index;
        g->last_present = std::chrono::steady_clock::now();
    }
    {
        const uint64_t n = g_frame_count.load(std::memory_order_relaxed);
        g_frame_work[n & 3].store(g->reserved.load(), std::memory_order_relaxed);
        g_frame_count.store(n + 1, std::memory_order_release);
    }
    static const int ahead = static_cast<int>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "present_ahead", 1), 0, 3));
    if (ahead == 0 || on_render_thread()) {
        Ref<Window> keep = Ref<Window>::share(window);
        run_on_render_thread([&] {
            extras::present_timing_render_begin(frame);
            present_on_render_thread(window, index);
        });
        return;
    }
    {
        NXFT("present.backpressure");
        nxrt::HitchTimer ht(nxrt::kHpGameBackpressure);
        std::unique_lock<std::mutex> lk(g->pmu);
        g->pcv.wait(lk, [] { return g->presents_pending < ahead; });
        ++g->presents_pending;
    }
    Ref<Window> keep = Ref<Window>::share(window);
    {
        std::lock_guard<std::mutex> lk(g->qmu);
        g->queue.push_back([keep, index, frame] {
            extras::present_timing_render_begin(frame);
            present_on_render_thread(keep.get(), index);
            std::lock_guard<std::mutex> l2(g->pmu);
            --g->presents_pending;
            g->pcv.notify_all();
        });
        g_gauge_render_queue.add(1);
    }
    g->qcv.notify_one();
}

// The game's last frame once more, queued like a present (the overlay is drawn on top as on every frame). Only when the game has not
// presented for 50 ms and nothing is queued, so it never competes with the game's own frames.
void present_again() {
    if (!g || g->dev->device_lost()) return;
    Ref<Window> keep;
    int index = -1;
    {
        std::lock_guard<std::mutex> lk(g->pmu);
        if (!g->last_window || g->presents_pending > 0) return;
        if (std::chrono::steady_clock::now() - g->last_present < std::chrono::milliseconds(50)) return;
        keep = g->last_window;
        index = g->last_index;
        ++g->presents_pending;
    }
    {
        std::lock_guard<std::mutex> lk(g->qmu);
        g->queue.push_back([keep, index] {
            extras::present_timing_render_begin(0);    // a repeated frame has no record
            present_on_render_thread(keep.get(), index, true);
            std::lock_guard<std::mutex> l2(g->pmu);
            --g->presents_pending;
            g->pcv.notify_all();
        });
        g_gauge_render_queue.add(1);
    }
    g->qcv.notify_one();
}

void destroy_texture_gpu(TexGpu* t) {
    if (!t) return;
    if (!g) { tex_destroy(t); return; }
    defer_destroy([t] { tex_destroy(t); });
}

void release_texture_gpu(Texture* t) {
    if (!g || !t) return;
    Ref<Texture> keep = Ref<Texture>::share(t);
    {
        std::lock_guard<std::mutex> lk(g->qmu);
        if (g->stop) return;
        g->queue.push_back([keep] {
            Texture* x = keep.get();
            if (!x->destroyed.load() || !x->gpu) return;   // initialized again meanwhile, or never got an image
            TexGpu* img = x->gpu;
            x->gpu = nullptr;
            g_bind_gen.fetch_add(1, std::memory_order_relaxed);
            destroy_texture_gpu(img);
        });
        g_gauge_render_queue.add(1);
    }
    g->qcv.notify_one();
}

void destroy_pool_gpu(PoolGpu* p) {
    if (!p) return;
    if (!g) { pool_gpu_destroy(p); return; }
    defer_destroy([p] { pool_gpu_destroy(p); });
}

namespace {
std::atomic<int> g_origin{NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT}, g_depth_mode{NVN_DEPTH_MODE_NEAR_IS_MINUS_W};
}
void set_window_modes(int origin_mode, int depth_mode) { g_origin.store(origin_mode); g_depth_mode.store(depth_mode); }
int window_origin_mode() { return g_origin.load(); }
int depth_mode() { return g_depth_mode.load(); }
gfx::Renderer* renderer() { return g ? g->rend : nullptr; }
FrameLimiter* limiter() { return g ? g->limiter : nullptr; }
PresentPass* present_pass() { return g && g->present_ok ? &g->present : nullptr; }
extras::OutputPass* output_pass() { return g && g->output_ok ? &g->output : nullptr; }
bool keep_aspect() { return g && g->keep_aspect; }

uint64_t timestamp_ns() {
    static const auto t0 = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) + 1000000000ull;
}

std::string unsupported_report() {
    std::string out;
    if (!g) return out;
    std::lock_guard<std::mutex> lk(g->unsupported_mu);
    for (auto& [name, n] : g->unsupported) out += name + ": " + std::to_string(n) + "\n";
    return out;
}

// ---- lifecycle -----------------------------------------------------------------------------------------------------------
namespace {

void register_counter_gauges();

bool bring_up(gfx::Device& dev, gfx::Renderer* rend, FrameLimiter* limiter, bool keep_aspect) {
    std::lock_guard<std::mutex> lk(g_install_mu);
    if (g) return g->dev == &dev;
    auto* gp = new Gpu();
    gp->dev = &dev;
    gp->rend = rend;
    gp->limiter = limiter;
    extras::present_timing_set_limiter(limiter);
    gp->keep_aspect = keep_aspect;
    const bool linux_kernel = host_is_linux();
    {   // [nvn] submit_batch: auto = frame on Linux (RADV revalidates every imported userptr BO of a submission, so each one walks the
        // imported pools again: flush_batch), 1 elsewhere.
        // frame: one kernel submission per frame, the present's; N: flush every N submissions and whenever the render thread idles.
        std::string b = nxrt::config_get_str("nvn", "submit_batch", "auto");
        const std::string how = b == "auto" ? (linux_kernel ? "auto, Linux" : "auto, not Linux") : b;
        if (b == "auto") b = linux_kernel ? "frame" : "1";
        gp->frame_batch = b == "frame";
        gp->batch_max = gp->frame_batch ? kSlots / 2 : static_cast<uint32_t>(std::clamp<int64_t>(std::atoll(b.c_str()), 1, kSlots / 2));
        NXLOG_I(kTag, "submit_batch %s (%s)", gp->frame_batch ? "frame" : std::to_string(gp->batch_max).c_str(), how.c_str());
    }
    {   // [nvn] poll_flush: auto = now (a poll of a batched serial flushes at once); defer = leave it for the present; 1-50 = defer at most that many ms.
        // Only with submit_batch = frame.
        const std::string v = nxrt::config_get_str("nvn", "poll_flush", "auto");
        int ms = 0;
        if (v == "defer") ms = -1;
        else if (v != "auto" && v != "now") ms = static_cast<int>(std::clamp<int64_t>(std::atoll(v.c_str()), 0, 50));   // junk reads as 0 = now
        if (!gp->frame_batch) ms = 0;
        gp->poll_flush_ms = ms;
        NXLOG_I(kTag, "poll_flush %s%s", ms < 0 ? "defer" : ms > 0 ? (std::to_string(ms) + " ms").c_str() : "now", gp->frame_batch ? "" : " (submit_batch is not frame)");
    }
    {   // [nvn] gpu_split_ms: see split_piece. auto = 2 on Linux (with submit_batch = frame the GPU got a submission only with the present),
        // off elsewhere; 0 = off; 0.25-50 = the recording time of one piece. [nvn] gpu_split_pieces: the most pieces one submission is cut into
        // (2-8, default 3). Each piece is one more kernel submission (on the submit thread), so the budget keeps them to a few per frame.
        const std::string v = nxrt::config_get_str("nvn", "gpu_split_ms", "auto");
        double ms = v == "auto" ? (linux_kernel ? 2.0 : 0.0) : std::atof(v.c_str());     // junk reads as 0 = off
        ms = ms > 0.0 ? std::clamp(ms, 0.25, 50.0) : 0.0;
        if (ms > 0.0) {     // the replay checks the budget with the TSC (ft::tsc, a few ns; it is read at many ops between passes)
            const double ns_per_tick = static_cast<double>(ft::tsc_to_ns(1ull << 32)) / 4294967296.0;    // calibrates once (~20 ms) if no one has
            gp->split_ticks = static_cast<uint64_t>(ms * 1e6 / (ns_per_tick > 0.0 ? ns_per_tick : 0.3));
        }
        gp->split_max = static_cast<uint32_t>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "gpu_split_pieces", 3), 2, kMaxPieces));
        if (ms > 0.0)
            NXLOG_I(kTag, "gpu_split %.2f ms (%s), at most %u pieces: a submission still being recorded after that long is sent to the GPU in pieces, cut between passes",
                    ms, v.c_str(), gp->split_max);
        else NXLOG_I(kTag, "gpu_split off (%s%s)", v.c_str(), v == "auto" ? ", not Linux" : "");
    }
    g = gp;
    if (!dev.features().timeline_semaphore) {
        NXLOG_E(kTag, "the GPU has no timeline semaphores: the NVN back end cannot run");
        g = nullptr;
        delete gp;
        return false;
    }
    VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
    if (vkCreateSemaphore(dev.device(), &sci, nullptr, &gp->timeline) != VK_SUCCESS) {
        g = nullptr;
        delete gp;
        return false;
    }
    vk_gauge_semaphores().add(1);
    for (Slot& s : gp->slots)
        if (!create_slot(s)) {
            NXLOG_E(kTag, "creating a submission slot failed");
            g = nullptr;
            delete gp;
            return false;
        }
    if (ft::on()) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kSlots * 2;
        if (vkCreateQueryPool(dev.device(), &qi, nullptr, &gp->qpool) != VK_SUCCESS) gp->qpool = VK_NULL_HANDLE;
        else vk_gauge_query_pools().add(1);
    }
    if (ft::on() && config_get_bool("prof", "gpu_passes", false)) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kSlots * kGpMax;
        if (vkCreateQueryPool(dev.device(), &qi, nullptr, &gp->gpool) != VK_SUCCESS) gp->gpool = VK_NULL_HANDLE;
        else vk_gauge_query_pools().add(1);
    }
    if (ft::on()) extras::lowlat_set_backlog_fn([]() -> uint64_t {      // latency probe: submissions the GPU has not finished
        if (!g || !g->timeline) return 0;
        uint64_t v = 0;
        vkGetSemaphoreCounterValue(g->dev->device(), g->timeline, &v);
        const uint64_t sub = g->last_submitted.load();
        return sub > v ? sub - v : 0;
    });
    extras::lowlat_set_gpu_wait_fn(&gpu_wait_frames);
    static const bool report_registered = (diag_add_report(gpu_report), true);   // the freeze dump shows the GPU state
    (void)report_registered;
    register_counter_gauges();
    // [nvn] submit_thread: see Gpu::sw. Started before the render thread, so every submission of the run goes through it. auto = on on
    // Linux, where a kernel submission can block the render thread for most of a frame (Steam Deck: 25-41 ms, 26 -> 53 fps with the
    // worker), off on Windows, where it measured no change.
    const std::string st = nxrt::config_get_str("nvn", "submit_thread", "auto");
    const bool st_on = st == "auto" ? linux_kernel : (st == "on" || st == "1" || st == "true");
    if (!st_on) NXLOG_I(kTag, "submit_thread off (%s%s)", st.c_str(), st == "auto" ? ", not Linux" : "");
    if (st_on) {
        gp->sw = std::make_unique<SubmitWorker>();
        gp->sw->start([] {
            log_set_thread_name("nvn-submit");
            diag_register_thread("nvn-submit", nullptr);
        });
        if (rend) rend->set_submit_queue([](std::function<void()> job) {
            g->sw->push([job = std::move(job)] {
                NXFT("submit.worker.present");     // the present's vkQueueSubmit2 + vkQueuePresentKHR (zones present.vk_submit / present.vk_present)
                job();
            });
        }, [] { submit_drain(); });
        NXLOG_I(kTag, "submit_thread on (%s%s): submissions and presents go to the queue from the nvn-submit thread", st.c_str(), st == "auto" ? ", Linux" : "");
    }
    {   // [nvn] one_submit: see Gpu::one_submit. auto = on on Linux with submit_batch = frame and the worker; on | split | off.
        const std::string o = nxrt::config_get_str("nvn", "one_submit", "auto");
        const bool can = gp->frame_batch && gp->sw;
        int v = o == "auto" ? (linux_kernel && can ? 1 : 0) : (o == "on" || o == "1" || o == "true") ? 1 : o == "split" ? 2 : 0;
        if (v && !can) {
            NXLOG_W(kTag, "one_submit %s: no effect without submit_batch = frame and submit_thread on", o.c_str());
            v = 0;
        }
        gp->one_submit = v;
        const int64_t hold = std::clamp<int64_t>(nxrt::config_get_int("nvn", "one_submit_hold_ms", 0), 0, 50);
        gp->park_hold = std::chrono::milliseconds(hold);
        if (v) NXLOG_I(kTag, "one_submit %s (%s): batch flushes still queued at the present go in its vkQueueSubmit2 (%s), hold %lld ms",
                       v == 2 ? "split" : "on", o.c_str(), v == 2 ? "own VkSubmitInfo2" : "one VkSubmitInfo2", static_cast<long long>(hold));
        else NXLOG_I(kTag, "one_submit off (%s%s)", o.c_str(), o == "auto" ? (can ? ", not Linux" : ", no frame batch or no submit thread") : "");
    }
    sync_wake_start();
    gp->th.start("nvn-render", 32, render_loop);
    // wait for the thread id to be published
    for (int i = 0; i < 1000 && gp->tid == std::thread::id(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (rend) {
        run_on_render_thread([&] { gp->present_ok = gp->present.init(dev, rend->format()); });
        if (!gp->present_ok) NXLOG_E(kTag, "present pass failed to initialise: frames will not be shown");
        run_on_render_thread([&] { gp->output_ok = gp->output.init(dev); });
        if (!gp->output_ok) NXLOG_W(kTag, "output pass (HDR, FSR 1) failed to initialise: those extras are off");
    }
    NXLOG_I(kTag, "NVN back end up on %s (%u submission slots, timeline semaphore, host-memory import %s)", dev.properties().deviceName, kSlots,
            dev.features().external_memory_host ? "available" : "NOT available");
    return true;
}

const char* const kCounterGauges[] = {"nvn.cnt.draws", "nvn.cnt.passes", "nvn.cnt.pass_breaks", "nvn.cnt.dispatches", "nvn.cnt.clears", "nvn.cnt.copies",
                                      "nvn.cnt.submits", "nvn.cnt.descriptor_sets"};

void register_counter_gauges() {
    auto reg = [](const char* name, std::atomic<uint64_t> Counters::*m) {
        nxrt::gauge_register(name, [m]() -> int64_t { Gpu* gp = g; return gp ? static_cast<int64_t>((gp->ctr.*m).load(std::memory_order_relaxed)) : 0; });
    };
    reg("nvn.cnt.draws", &Counters::draws);
    reg("nvn.cnt.passes", &Counters::passes);
    reg("nvn.cnt.pass_breaks", &Counters::pass_breaks);
    reg("nvn.cnt.dispatches", &Counters::dispatches);
    reg("nvn.cnt.clears", &Counters::clears);
    reg("nvn.cnt.copies", &Counters::copies);
    reg("nvn.cnt.submits", &Counters::submits);
    reg("nvn.cnt.descriptor_sets", &Counters::descriptor_sets);
    // What the hitch log (nxrt/gauge.h) shows for a slow frame: growth of these during the frame, read lock-free on the presenting thread.
    static const bool hitch_registered = [] {
        nxrt::hitch_add_counter("pipelines_created", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.pipelines_created.load(std::memory_order_relaxed) : 0; });
        nxrt::hitch_add_counter("texture_uploads", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.texture_uploads.load(std::memory_order_relaxed) : 0; });
        nxrt::hitch_add_counter("texture_upload_kb", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.texture_upload_bytes.load(std::memory_order_relaxed) >> 10 : 0; });
        nxrt::hitch_add_counter("bc7_transcodes", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.bc7_transcodes.load(std::memory_order_relaxed) : 0; });
        nxrt::hitch_add_level("pipeline_compiles_in_flight", []() -> int64_t { return compiles_in_flight().load(std::memory_order_relaxed); });
        return true;
    }();
    (void)hitch_registered;
}

void tear_down() {
    std::lock_guard<std::mutex> lk(g_install_mu);
    if (!g) return;
    for (const char* n : kCounterGauges) nxrt::gauge_unregister(n);
    Gpu* gp = g;
    run_on_render_thread([&] {
        {
            std::lock_guard<std::mutex> l2(gp->pmu);
            gp->last_window = nullptr;
        }
        submit_drain();     // [nvn] submit_thread: every submit and present handed over has run before anything is destroyed
        wait_serial_blocking(gp->submitted);   // (flushes the batch and waits for the worker to submit it)
        retire();
        replay_shutdown();
        tex_shutdown();
        pool_shutdown();
        while (!gp->deferred.empty()) {
            auto fn = std::move(gp->deferred.front().fn);
            gp->deferred.pop_front();
            g_gauge_deferred.sub(1);
            fn();
        }
        drain_pending_destroy();
        for (auto& d : gp->deferred) { d.fn(); g_gauge_deferred.sub(1); }
        gp->deferred.clear();
        gp->present.shutdown();
        gp->output.shutdown();
    });
    {
        std::lock_guard<std::mutex> ql(gp->qmu);
        gp->stop = true;
    }
    gp->qcv.notify_all();
    if (gp->th.joinable()) gp->th.join();
    if (gp->sw) {   // [nvn] submit_thread: the render thread's last flush runs, then nothing more goes to the queue from the worker
        gp->park.release_all();     // [nvn] one_submit: no present comes any more
        gp->sw->stop();
        if (gp->rend) gp->rend->set_submit_queue({}, {});
    }
    sync_wake_stop();   // its timeline wait ends before the semaphore goes
    extras::present_timing_set_limiter(nullptr);
    gfx::Device& d = *gp->dev;
    vkDeviceWaitIdle(d.device());
    for (Slot& s : gp->slots) {
        for (auto& b : s.extra) d.destroy_buffer(b);
        for (uint32_t i = 1; i < kMaxPieces; ++i)
            if (s.cbs[i]) vk_gauge_command_buffers().sub(1);     // [nvn] gpu_split: freed with the pool below
        vk_gauge_descriptor_sets().sub(static_cast<int64_t>(s.sets_live));
        s.sets_live = 0;
        for (VkDescriptorPool p : s.dpools) {
            vkDestroyDescriptorPool(d.device(), p, nullptr);
            vk_gauge_descriptor_pools().sub(1);
        }
        d.destroy_buffer(s.upload);
        if (s.pool) {
            vkDestroyCommandPool(d.device(), s.pool, nullptr);
            vk_gauge_command_pools().sub(1);
            if (s.cb) vk_gauge_command_buffers().sub(1);
        }
    }
    if (gp->qpool) { vkDestroyQueryPool(d.device(), gp->qpool, nullptr); vk_gauge_query_pools().sub(1); }
    if (gp->gpool) { vkDestroyQueryPool(d.device(), gp->gpool, nullptr); vk_gauge_query_pools().sub(1); }
    if (gp->timeline) { vkDestroySemaphore(d.device(), gp->timeline, nullptr); vk_gauge_semaphores().sub(1); }
    g = nullptr;
    delete gp;
}

}  // namespace

bool init_for_test(gfx::Device& dev, gfx::Renderer* renderer) { return bring_up(dev, renderer, nullptr, false); }
void shutdown_for_test() { tear_down(); }

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn {

bool install_backend(const BackendDesc& desc) {
    if (!desc.device) return false;
    if (!be::gpu::bring_up(*desc.device, desc.renderer, desc.limiter, desc.keep_aspect)) return false;
    if (desc.register_entry_points) {
        install();
        register_all_impls();
        NXLOG_I("nvn", "NVN back end installed: %zu of %zu entry points implemented", implemented_count(), entry_count());
    }
    return true;
}

void shutdown_backend() { be::gpu::tear_down(); }
void present_last_frame_again() {
    if (be::gpu::active()) be::gpu::present_again();
}

bool prepare_shaders(const ShaderPrepareOptions& o, ShaderPrepareResult* r) {
    ShaderPrepareResult local;
    ShaderPrepareResult& out = r ? *r : local;
    if (!be::gpu::active()) { out = ShaderPrepareResult{}; out.error = "the graphics back end is not running"; return false; }
    return be::gpu::gpl_prepare(o, &out);
}
bool backend_installed() { return be::gpu::active(); }
void backend_flush_caches() {
    if (be::gpu::active()) be::gpu::flush_caches();
}

void backend_trace_next_frame() {
    if (be::gpu::active()) be::gpu::replay_request_trace();
}

BackendStats backend_stats() {
    BackendStats s;
    if (!be::gpu::active()) return s;
    auto& c = be::gpu::counters();
    s.submits = c.submits; s.lists_replayed = c.lists_replayed; s.draws = c.draws; s.draws_skipped = c.draws_skipped;
    s.dispatches = c.dispatches; s.clears = c.clears; s.copies = c.copies; s.presents = c.presents;
    s.passes = c.passes; s.pass_breaks = c.pass_breaks; s.pipelines_created = c.pipelines_created;
    s.pipeline_cache_hits = c.pipeline_cache_hits; s.descriptor_sets = c.descriptor_sets;
    s.textures_created = c.textures_created; s.texture_uploads = c.texture_uploads; s.texture_upload_bytes = c.texture_upload_bytes;
    s.bc7_transcodes = c.bc7_transcodes; s.textures_failed = c.textures_failed; s.pools_imported = c.pools_imported;
    s.unresolved_addresses = c.unresolved_addresses; s.missing_handles = be::handles_missing(); s.unsupported_ops = c.unsupported_ops;
    s.submit_replay_ns = c.submit_replay_ns;
    return s;
}

std::string backend_unsupported_report() { return be::gpu::unsupported_report(); }

}  // namespace nxrt::nvn
