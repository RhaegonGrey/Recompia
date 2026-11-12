// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] submit_thread: one worker thread that runs the render thread's queue work (vkQueueSubmit2 of the recorded batch, the present's
// submit + vkQueuePresentKHR) in FIFO order, so the render thread can record the next frame while the driver does the submission (each
// kernel submission is expensive on Linux: amdgpu looks up the userptr pool imports again, nvn_gpu.cpp flush_batch).
//
// Contract:
// - One worker, FIFO: jobs run in the order they were pushed, one at a time. The GPU sees the submissions in recording order, as without it.
// - `submitted()`: the highest timeline serial whose job has run (its vkQueueSubmit2 returned, or it was completed on the CPU: failure,
//   device loss). A job publishes its serial with publish() after its submit. A CPU wait for a serial waits for submitted() first
//   (wait_submitted), then on the timeline: no host wait ever starts on a value nobody has handed to the queue yet.
// - drain(): returns once every job pushed before the call has run (the queue is empty and the worker idle). Swapchain work (acquire,
//   recreate, present mode change), sparse binds, the debug wait-idle submit and shutdown drain first.
// No Vulkan in here.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "nvn_thread.h"

namespace nxrt::nvn::be::gpu {

class SubmitWorker {
public:
    using Job = std::function<void()>;

    SubmitWorker() = default;
    SubmitWorker(const SubmitWorker&) = delete;
    SubmitWorker& operator=(const SubmitWorker&) = delete;
    ~SubmitWorker() { stop(); }

    // on_start runs first on the worker thread (thread name, diag registration).
    void start(std::function<void()> on_start = {}) {
        std::lock_guard<std::mutex> lk(mu_);
        if (th_.joinable()) return;
        stop_ = false;
        th_.start("nvn-submit", 16, [this, on_start = std::move(on_start)] {
            if (on_start) on_start();
            loop();
        });
    }

    // Runs what is queued, then ends the thread. Jobs pushed after stop() run inline on the caller (nothing is ever dropped).
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!th_.joinable()) return;
            stop_ = true;
        }
        cv_.notify_all();
        th_.join();
    }

    bool running() const { return running_.load(std::memory_order_acquire); }

    void push(Job job) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (th_.joinable() && !stop_) {
                q_.push_back(std::move(job));
                ++pushed_;
                cv_.notify_all();
                return;
            }
        }
        job();      // no worker: inline, as without the thread
    }

    // Called by a job (or inline) after its vkQueueSubmit2 returned, or after the serial was completed on the CPU.
    void publish(uint64_t serial) {
        uint64_t cur = submitted_.load(std::memory_order_relaxed);
        while (cur < serial && !submitted_.compare_exchange_weak(cur, serial, std::memory_order_release, std::memory_order_relaxed)) {}
        std::lock_guard<std::mutex> lk(mu_);    // pairs with the waiters' predicate check
        done_cv_.notify_all();
    }

    uint64_t submitted() const { return submitted_.load(std::memory_order_acquire); }

    // Waits until submitted() >= serial or `also()` is true (the serial completed by another way, device lost). Polls `also` every
    // `poll` while waiting. false: timed out.
    template <class Also>
    bool wait_submitted(uint64_t serial, uint64_t timeout_ns, Also also, std::chrono::milliseconds poll = std::chrono::milliseconds(5)) {
        if (submitted() >= serial || also()) return true;
        const auto deadline = timeout_ns == UINT64_MAX ? std::chrono::steady_clock::time_point::max()
                                                       : std::chrono::steady_clock::now() + std::chrono::nanoseconds(static_cast<int64_t>(std::min<uint64_t>(timeout_ns, INT64_MAX / 2)));
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            if (submitted() >= serial) return true;
            lk.unlock();
            const bool other = also();
            lk.lock();
            if (other) return true;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            const auto until = deadline - now < poll ? deadline : now + poll;
            done_cv_.wait_until(lk, until, [&] { return submitted() >= serial; });
        }
    }

    // Every job pushed before this call has run.
    void drain() {
        std::unique_lock<std::mutex> lk(mu_);
        const uint64_t want = pushed_;
        done_cv_.wait(lk, [&] { return ran_ >= want || !th_.joinable(); });
    }

    size_t depth() {
        std::lock_guard<std::mutex> lk(mu_);
        return q_.size() + (busy_ ? 1 : 0);
    }

private:
    void loop() {
        running_.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
            if (q_.empty()) break;      // stop and drained
            Job job = std::move(q_.front());
            q_.pop_front();
            busy_ = true;
            lk.unlock();
            job();
            lk.lock();
            busy_ = false;
            ++ran_;
            done_cv_.notify_all();
        }
        running_.store(false, std::memory_order_release);
    }

    std::mutex mu_;
    std::condition_variable cv_;        // worker: jobs or stop
    std::condition_variable done_cv_;   // waiters: a job ran / a serial was published
    std::deque<Job> q_;
    uint64_t pushed_ = 0, ran_ = 0;     // under mu_
    bool busy_ = false;
    bool stop_ = false;
    BigThread th_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> submitted_{0};
};

}  // namespace nxrt::nvn::be::gpu
