// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] pool_copy: where the shadows' buffers come from. A shadow needs a host-visible Vulkan buffer the size of its cells (16 MiB by
// default). Making one inside a draw's bind is the largest part of the long frames of an area load on a Steam Deck (the render thread
// spent up to 500 ms of one frame in pool.copy.alloc while the game streamed a new area in), and a shadow dropped by the idle rule or
// by the cap was destroyed and made again at its next bind.
//
// [nvn] pool_copy_spare_mb (default 128; 0: every shadow is made at its first bind and destroyed at its drop, as before):
//  * reuse: the buffer of a dropped shadow is kept as a spare (up to twice that many MiB) and given to the next shadow of the same size.
//    What it held does not matter: a shadow starts with no page copied, and every page a bind reads is copied first.
//  * ahead of use: the nvn-pool-stock thread makes cell-sized spares until that many MiB are ready, whenever the stock is below it and
//    the cap ([nvn] pool_copy_mb, which spares count toward) leaves room. It writes every page once, so the first copy into the buffer
//    pays no page fault either.
// When no spare of the right size is there, the buffer is made on the calling thread as before (counted, and in the hitch line).
// Nothing but a shadow may hold one of these buffers across submissions: a dropped shadow's VkBuffer serves another range next.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"
#include "nvn_pool_copy_impl.h"

namespace nxrt::nvn::be::gpu::pcopy {
namespace {

struct Stock {
    std::mutex mu;
    std::condition_variable cv;
    std::thread th;
    bool running = false;            // between stock_start and stock_stop: spares are kept and made
    bool stop = false;
    uint64_t target = 0;             // [nvn] pool_copy_spare_mb in bytes; 0: off
    uint64_t cell = 0, cap = 0;
    std::vector<gfx::Buffer> spare;
    uint64_t spare_bytes = 0;
    uint64_t in_use = 0;             // bytes of buffers shadows hold
};
Stock g_stock;
std::atomic<uint64_t> g_made_here{0}, g_made_here_bytes{0}, g_made_here_ns{0}, g_made_ahead{0}, g_reused{0}, g_spare_now{0};

uint64_t spare_target() {
    static const uint64_t t = static_cast<uint64_t>(std::clamp<int64_t>(config_get_int("nvn", "pool_copy_spare_mb", 128), 0, 1 << 20)) << 20;
    return t;
}

VkBufferUsageFlags shadow_usage() {      // what import_range gives an import's buffer
    return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
           VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
           (device().features().transform_feedback ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0u);
}

bool make(uint64_t size, gfx::Buffer* out) {
    NXFT("pool.stock.make");
    gfx::Buffer b;
    if (device().create_buffer(size, shadow_usage(), gfx::MemoryUse::Shadow, &b, "nvn pool shadow") != VK_SUCCESS || !b.mapped) {
        if (b.buffer) device().destroy_buffer(b);
        return false;
    }
    *out = b;
    return true;
}

void destroy_all(std::vector<gfx::Buffer>& v) {
    for (gfx::Buffer& b : v) device().destroy_buffer(b);
    v.clear();
}

// with g_stock.mu held: the thread has a spare to make
bool wanted(const Stock& s) {
    return s.running && s.target && s.cell && s.spare_bytes + s.cell <= s.target && (!s.cap || s.in_use + s.spare_bytes + s.cell <= s.cap);
}

void stock_loop() {
    log_set_thread_name("nvn-pool-stock");
    Stock& s = g_stock;
    std::unique_lock<std::mutex> lk(s.mu);
    while (!s.stop) {
        if (!wanted(s)) {
            s.cv.wait(lk);
            continue;
        }
        const uint64_t size = s.cell;
        lk.unlock();
        gfx::Buffer b;
        const bool ok = make(size, &b);
        if (ok) std::memset(b.mapped, 0, static_cast<size_t>(size));     // every page mapped before a bind copies into it
        lk.lock();
        if (!ok) {      // out of memory, or the device is lost: not again at once
            s.cv.wait_for(lk, std::chrono::seconds(1));
            continue;
        }
        g_made_ahead.fetch_add(1, std::memory_order_relaxed);
        s.spare.push_back(b);
        s.spare_bytes += size;
        g_spare_now.store(s.spare_bytes, std::memory_order_relaxed);
    }
}

struct StockGauges {
    StockGauges() {
        nxrt::gauge_register("nvn.pool.copy.spare_mb", [] { return static_cast<int64_t>(g_spare_now.load(std::memory_order_relaxed) >> 20); });
        nxrt::hitch_add_counter("pool_copy_made_kb", []() -> uint64_t { return g_made_here_bytes.load(std::memory_order_relaxed) >> 10; });
    }
    ~StockGauges() { nxrt::gauge_unregister("nvn.pool.copy.spare_mb"); }
};
StockGauges g_stock_gauges;

}  // namespace

void stock_start(uint64_t cell, uint64_t cap) {
    Stock& s = g_stock;
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.running) return;
    s.running = true;
    s.stop = false;
    s.target = spare_target();
    s.cell = cell;
    s.cap = cap;
    static std::once_flag once;
    std::call_once(once, [&] {
        if (s.target)
            NXLOG_I(kTag, "pool_copy: stock of shadow buffers: up to %llu MiB of %llu MiB spares made ahead of use (thread nvn-pool-stock), a dropped "
                          "shadow's buffer kept for the next one up to %llu MiB (pool_copy_spare_mb)",
                    static_cast<unsigned long long>(s.target >> 20), static_cast<unsigned long long>(cell >> 20), static_cast<unsigned long long>(s.target >> 19));
        else
            NXLOG_I(kTag, "pool_copy: no stock of shadow buffers (pool_copy_spare_mb = 0): a shadow is made at its first bind and destroyed at its drop");
    });
    if (s.target && !s.th.joinable()) s.th = std::thread(stock_loop);
}

void stock_stop() {
    Stock& s = g_stock;
    std::vector<gfx::Buffer> spare;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.stop = true;
        s.running = false;      // from here on stock_give destroys (the pools go next)
    }
    s.cv.notify_all();
    if (s.th.joinable()) s.th.join();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        spare.swap(s.spare);
        s.spare_bytes = 0;
        g_spare_now.store(0, std::memory_order_relaxed);
    }
    destroy_all(spare);
}

bool stock_get(uint64_t size, gfx::Buffer* out) {
    Stock& s = g_stock;
    {
        std::unique_lock<std::mutex> lk(s.mu);
        for (size_t i = s.spare.size(); i-- > 0;)
            if (s.spare[i].size == size) {
                *out = s.spare[i];
                s.spare.erase(s.spare.begin() + static_cast<std::ptrdiff_t>(i));
                s.spare_bytes -= size;
                s.in_use += size;
                g_spare_now.store(s.spare_bytes, std::memory_order_relaxed);
                g_reused.fetch_add(1, std::memory_order_relaxed);
                const bool wake = wanted(s);
                lk.unlock();
                if (wake) s.cv.notify_one();
                return true;
            }
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!make(size, out)) return false;
    g_made_here.fetch_add(1, std::memory_order_relaxed);
    g_made_here_bytes.fetch_add(size, std::memory_order_relaxed);
    g_made_here_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                             std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(s.mu);
    s.in_use += size;
    return true;
}

void stock_give(gfx::Buffer& b, uint64_t want) {
    Stock& s = g_stock;
    const uint64_t size = b.size;
    bool keep = false, wake = false;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.in_use -= std::min(s.in_use, size);
        // Kept: the size an eviction is making room for (it is taken at once), or, with no eviction running, while the spares stay
        // under twice the target (the thread fills up to the target: a drop then does not destroy what the thread would make again).
        keep = s.running && s.target && (want ? size == want : s.spare_bytes + size <= 2 * s.target);
        if (keep) {
            s.spare.push_back(b);
            s.spare_bytes += size;
            g_spare_now.store(s.spare_bytes, std::memory_order_relaxed);
        } else {
            wake = wanted(s);     // the cap has room again
        }
    }
    if (!keep) device().destroy_buffer(b);
    if (wake) s.cv.notify_one();
    b = gfx::Buffer{};
}

bool stock_over(uint64_t size, uint64_t cap) {
    Stock& s = g_stock;
    std::vector<gfx::Buffer> out;
    bool over = false;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        bool have = false;
        for (const gfx::Buffer& b : s.spare) have = have || b.size == size;
        if (have) return false;
        // spares of other sizes go first: they hold memory no shadow uses
        while (s.in_use + s.spare_bytes + size > cap && !s.spare.empty()) {
            out.push_back(s.spare.back());
            s.spare_bytes -= s.spare.back().size;
            s.spare.pop_back();
        }
        g_spare_now.store(s.spare_bytes, std::memory_order_relaxed);
        over = s.in_use + s.spare_bytes + size > cap;
    }
    destroy_all(out);
    return over;
}

StockStats stock_stats() {
    StockStats st;
    st.made_here = g_made_here.load(std::memory_order_relaxed);
    st.made_here_ms = static_cast<double>(g_made_here_ns.load(std::memory_order_relaxed)) / 1e6;
    st.made_ahead = g_made_ahead.load(std::memory_order_relaxed);
    st.reused = g_reused.load(std::memory_order_relaxed);
    st.spare_bytes = g_spare_now.load(std::memory_order_relaxed);
    return st;
}

}  // namespace nxrt::nvn::be::gpu::pcopy
