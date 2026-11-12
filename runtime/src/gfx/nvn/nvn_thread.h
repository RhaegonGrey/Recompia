// SPDX-License-Identifier: GPL-3.0-or-later
// A joinable thread with a stack of its own choosing for the back end's long-running threads.
//
// std::thread takes the platform default stack: 1 MiB on Windows, 8 MiB on Linux, and only 512 KiB on macOS. The render
// thread's replay (run_list -> resolve_in_pool -> pcopy_touch -> guarded_copy) and the Vulkan loader behind it go deeper than
// that on a busy pool, and a thread that runs into its stack guard dies on the guard page before the crash handler can even
// run (it would run on the exhausted stack). The threads that do that work start here instead.
#pragma once
#include <cstddef>
#include <functional>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <pthread.h>

#include "nx_os.h"   // nxos_install_altstack
#endif

namespace nxrt::nvn::be {

class BigThread {
public:
    BigThread() = default;
    BigThread(const BigThread&) = delete;
    BigThread& operator=(const BigThread&) = delete;
    BigThread(BigThread&& other) noexcept {
#if defined(_WIN32)
        thread_ = std::move(other.thread_);
#else
        th_ = other.th_;
        started_ = other.started_;
        other.started_ = false;
#endif
    }
    BigThread& operator=(BigThread&& other) noexcept {
        if (this == &other) return *this;
        if (joinable()) detach();
#if defined(_WIN32)
        thread_ = std::move(other.thread_);
#else
        th_ = other.th_;
        started_ = other.started_;
        other.started_ = false;
#endif
        return *this;
    }
    ~BigThread() {
        if (joinable()) detach();   // every user joins explicitly; a detached thread must not outlive its statics
    }

    bool start(const char* name, size_t stack_mb, std::function<void()> fn) {
        if (joinable()) return false;
#if defined(_WIN32)
        (void)name;
        (void)stack_mb;   // 1 MiB by default and the back end's Windows threads are shallow
        thread_ = std::thread(std::move(fn));
        return true;
#else
        struct Start {
            std::function<void()> fn;
            const char* name;
        };
        auto* s = new Start{std::move(fn), name};
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, (stack_mb ? stack_mb : 16) << 20);
        const int rc = pthread_create(&th_, &attr, [](void* p) -> void* {
            Start* start = static_cast<Start*>(p);
            nxos_install_altstack();   // a fault on this thread stays reportable when its stack is exhausted
            if (start->name) {
#if defined(__linux__)
                pthread_setname_np(pthread_self(), start->name);   // glibc names the calling thread only (15 characters)
#else
                pthread_setname_np(start->name);
#endif
                nxos_set_thread_name(start->name);   // the fault note reads this one, without a libc call
            }
            start->fn();
            delete start;
            return nullptr;
        }, s);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            delete s;
            return false;
        }
        started_ = true;
        return true;
#endif
    }

    bool joinable() const {
#if defined(_WIN32)
        return thread_.joinable();
#else
        return started_;
#endif
    }

    void join() {
#if defined(_WIN32)
        if (thread_.joinable()) thread_.join();
#else
        if (started_) {
            pthread_join(th_, nullptr);
            started_ = false;
        }
#endif
    }

    void detach() {
#if defined(_WIN32)
        if (thread_.joinable()) thread_.detach();
#else
        if (started_) {
            pthread_detach(th_);
            started_ = false;
        }
#endif
    }

private:
#if defined(_WIN32)
    std::thread thread_;
#else
    pthread_t th_{};
    bool started_ = false;
#endif
};

}  // namespace nxrt::nvn::be
