// SPDX-License-Identifier: GPL-3.0-or-later
// Pipeline layouts, shader modules and pipelines (see nvn_pipeline.h). A pipeline the key file's prewarm or the pipeline libraries do not
// provide is created on the render thread at its first draw while the game loads, and in play ([nvn] async_whole, the loading gate of
// nvn_gpl.cpp) on a background thread while the draw is skipped. Compute pipelines are built ahead at boot from the key file ([nvn]
// compute_prewarm) and in the background when the game sets a compute program ([nvn] async_compute); a dispatch whose pipeline is missing in
// play is skipped only when the CPU cannot read what it writes (do_dispatch, nvn_replay.cpp). A VkPipelineCache persists between runs
// ([nvn] pipeline_cache, default "pipeline_cache.bin").
#include "nvn_pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/game.h"
#include "nxrt/game_render.h"
#include "nxrt/gauge.h"
#include "nxrt/host_device.h"
#include "nxrt/log.h"
#include "nvn_format_guard.h"
#include "nvn_fnv.h"
#include "nvn_gpu_internal.h"
#include "nvn_spirv_patch.h"
#include "extras/hdr_nvn.h"
#include "nxrt/extras.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.pipe";

struct LayoutKey {
    const ShaderInfo* s[6];
    bool operator<(const LayoutKey& o) const { return std::memcmp(s, o.s, sizeof s) < 0; }
};

// Shader modules and layouts are also made by the prewarm workers (nvn_prewarm.cpp), so they have locks; the pipeline buckets below stay
// render thread only.
std::mutex g_layout_mu;
std::recursive_mutex g_module_mu;   // every module cache below; the variant makers call shader_module
std::map<LayoutKey, ProgLayout> g_layouts;
// Descriptor set layouts and the pipeline layout built from them, shared by every program whose descriptors are the same (S24): two programs with
// the same signature get the same VkPipelineLayout handle, so the shader parts compiled for one are found again for the other.
struct SharedLayout { VkDescriptorSetLayout sets[4] = {}; VkPipelineLayout layout = VK_NULL_HANDLE; };
std::map<std::string, SharedLayout> g_shared_layouts;
std::unordered_map<const ShaderInfo*, VkShaderModule> g_modules;
std::mutex g_module_id_mu;
std::unordered_map<VkShaderModule, uint64_t> g_module_ids;   // every module create_module made: the hash of its words (module_content_id)
std::atomic<uint64_t> g_built_at_draw{0};                    // whole pipelines built by the render thread at a draw
std::atomic<bool> g_cache_dirty{false};
std::atomic<uint32_t> g_new_since_save{0};      // pipelines and parts created since the cache file was last written
bool g_cache_big = false;                       // the file loaded at start was large (a shader preparation made it): rewritten rarely, see pipeline_cache_dirty
// Producer modules with extra outputs (nvn_spirv_patch.h): (shader, slots as location * 4 + scalar type).
std::map<std::pair<const ShaderInfo*, std::vector<uint32_t>>, VkShaderModule> g_variants;
std::unordered_map<const ShaderInfo*, VkShaderModule> g_dual_src;
std::unordered_map<const ShaderInfo*, std::pair<VkShaderModule, bool>> g_no_out;   // (module, owned by this map)      // fragment shaders with private outputs (passes without colour attachments)
std::unordered_map<const ShaderInfo*, std::pair<VkShaderModule, bool>> g_hdr_frag;   // (module, owned by this map): HDR tone-map variants (hdr_fragment_module)
std::unordered_map<const ShaderInfo*, std::pair<VkShaderModule, uint32_t>> g_xfb_mod;   // vertex shaders with the captured outputs, their stride       // fragment shaders with the second blend source at Location 0, Index 1
// One pipeline per key. `up`: a linked pipeline (nvn_gpl.cpp) that a background thread is replacing with an optimised one; the lookup swaps it in.
struct GfxEntry {
    GraphicsKey first;
    VkPipeline second = VK_NULL_HANDLE;
    VkPipeline alt = VK_NULL_HANDLE;                 // measurement only ([nvn] gpl_ab): the optimised pipeline, kept next to the linked one
    std::shared_ptr<PipeUpgrade> up;
};
std::unordered_map<uint64_t, std::vector<GfxEntry>> g_gfx;
// Compute pipelines, by the shader's content (its cache hash, so the key file's stand-in shader and the game's own find the same entry; the
// pointer for a shader without one) and the shared pipeline layout. Background threads (prewarm, async jobs) insert, the render thread
// looks up and builds: every access holds g_compute_mu; g_compute_cv is signalled when an entry becomes done.
struct ComputeEntry {
    int state = 0;                                   // 0 queued for an async thread, 1 being built, 2 done (pipe null: the build failed)
    VkPipeline pipe = VK_NULL_HANDLE;
};
using ComputeKey = std::pair<uint64_t, VkPipelineLayout>;
std::mutex g_compute_mu;
std::condition_variable g_compute_cv;
std::map<ComputeKey, ComputeEntry> g_compute;      // node based: an entry stays where it is while others are added
std::atomic<uint64_t> g_cs_skipped{0}, g_cs_at_dispatch{0}, g_cs_prewarmed{0}, g_cs_background{0};
VkPipelineCache g_cache = VK_NULL_HANDLE;
bool g_cache_tried = false;
std::string g_cache_path;
nxrt::GaugeCounter g_pipeline_count{"nvn.pipelines"};   // live VkPipelines (graphics and compute, linked ones included); render thread
std::unordered_map<const ShaderInfo*, std::pair<gfx::Buffer, VkDeviceSize>> g_const_bufs;
gfx::Buffer g_driver_const;
bool g_driver_const_made = false;
// Counters mirrored at the sites that change the maps above (they are render thread only, so the gauge reads these instead).
nxrt::GaugeCounter g_pipes_created{"nvn.pipelines_created"};     // ever; same events as counters().pipelines_created (shader parts included)
nxrt::GaugeCounter g_gfx_keys{"nvn.pipeline_keys"};              // entries in g_gfx: one per graphics key tried (a failed build keeps its entry)
nxrt::GaugeCounter g_compute_keys{"nvn.compute_pipeline_keys"};  // entries in g_compute
nxrt::GaugeCounter g_const_n{"nvn.const_buffers"};               // entries in g_const_bufs
nxrt::GaugeCounter g_const_bytes{"nvn.const_buffers.bytes"};     // bytes of the constant buffers in g_const_bufs

// ---- async threads: [nvn] async_whole and [nvn] async_compute (nvn_pipeline.h) --------------------------------------------------
// A graphics job builds the whole pipeline of a draw that is skipped meanwhile. Its `up` is the bucket entry's: the lookup swaps the
// pipeline in with gpl_take_upgrade once state is 1 (the entry's own pipeline is null until then, so the draw is skipped). A compute job
// (cs set) builds a compute pipeline into g_compute; with no layout (the game set the program, nothing dispatched yet) the thread derives it.
// Layouts: ProgLayout objects live in g_layouts (a std::map, node based) until pipeline_shutdown, which stops these threads (aw_stop) before
// it clears g_layouts, so a job's layout pointer outlives the job.
struct AsyncJob {
    GraphicsKey key;
    const ProgLayout* layout = nullptr;
    std::shared_ptr<PipeUpgrade> up;
    const ShaderInfo* cs = nullptr;                  // a compute job
};
std::mutex g_aw_mu;
std::condition_variable g_aw_cv;
std::deque<AsyncJob> g_aw_q;
std::vector<std::thread> g_aw_threads;
bool g_aw_stop = false;                              // stays set after pipeline_shutdown: later requests build at the draw / dispatch
std::atomic<uint64_t> g_aw_built{0}, g_aw_failed{0}, g_aw_skipped{0};

bool compute_build_bg(const ShaderInfo* cs, const ProgLayout& layout, bool prewarm);   // below, with compute_pipeline

void aw_main() {
    log_set_thread_name("nvn-async-pipe");
    set_compile_thread_priority();
    for (;;) {
        AsyncJob j;
        {
            std::unique_lock<std::mutex> lk(g_aw_mu);
            g_aw_cv.wait(lk, [] { return g_aw_stop || !g_aw_q.empty(); });
            if (g_aw_stop) return;
            j = std::move(g_aw_q.front());
            g_aw_q.pop_front();
        }
        if (j.cs) {
            const ShaderInfo* st6[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, j.cs};
            const ProgLayout* pl = j.layout ? j.layout : prog_layout(st6);
            if (pl) compute_build_bg(j.cs, *pl, false);
            continue;
        }
        VkPipeline p = VK_NULL_HANDLE;
        {
            NXFT("pipe.async_whole");
            p = build_graphics_pipeline(j.key, *j.layout);
        }
        (p ? g_aw_built : g_aw_failed).fetch_add(1, std::memory_order_relaxed);
        j.up->pipe = p;
        j.up->state.store(1, std::memory_order_release);
    }
}

// Any thread. False once the threads were stopped (the caller builds it itself). front = ahead of everything queued.
bool aw_push(AsyncJob&& j, bool front) {
    std::lock_guard<std::mutex> lk(g_aw_mu);
    if (g_aw_stop) return false;
    if (g_aw_threads.empty()) {
        const int n = static_cast<int>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "async_whole_threads", 1), 1, 4));
        for (int i = 0; i < n; ++i) g_aw_threads.emplace_back(aw_main);
        NXLOG_I(kTag, "async pipelines: %d background thread%s for pipelines that would be built at a draw or a dispatch", n, n == 1 ? "" : "s");
    }
    if (front) g_aw_q.push_front(std::move(j));
    else g_aw_q.push_back(std::move(j));
    g_aw_cv.notify_one();
    return true;
}

// Render thread. The entry's upgrade, built on the async threads (null: the threads were stopped).
std::shared_ptr<PipeUpgrade> aw_queue(const GraphicsKey& key, const ProgLayout& layout) {
    AsyncJob j;
    j.key = key;
    j.layout = &layout;
    j.up = std::make_shared<PipeUpgrade>();
    auto up = j.up;
    return aw_push(std::move(j), false) ? up : nullptr;
}

// Before the device is torn down: after prewarm_shutdown, before gpl_stop (pipeline_shutdown). A running build finishes first (join).
void aw_stop() {
    std::vector<std::thread> t;
    {
        std::lock_guard<std::mutex> lk(g_aw_mu);
        g_aw_stop = true;
        g_aw_q.clear();               // graphics entries keep state 0 (nothing to destroy); compute entries left at 0 are built at the dispatch
        t.swap(g_aw_threads);
    }
    g_aw_cv.notify_all();
    for (auto& th : t) th.join();
}

VkShaderStageFlagBits stage_bit(ShaderStage s) {
    switch (s) {
        case ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderStage::TessControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderStage::TessEval: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
        default: return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

std::mutex g_cache_mu;
std::atomic<bool> g_cache_ready{false};

void ensure_cache() {
    if (g_cache_ready.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_cache_mu);       // the prewarm workers and the render thread may get here together
    if (g_cache_tried) return;
    g_cache_tried = true;
    g_cache_path = config_get_str("nvn", "pipeline_cache", "pipeline_cache.bin");
    std::vector<char> data;
    if (!g_cache_path.empty()) {
        if (FILE* f = std::fopen(g_cache_path.c_str(), "rb")) {          // one read: the file can be hundreds of MB after a shader preparation
            std::fseek(f, 0, SEEK_END);
            const long len = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (len > 0) {
                data.resize(static_cast<size_t>(len));
                if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
            }
            std::fclose(f);
        }
    }
    g_cache_big = data.size() >= (64u << 20);
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(vkdev(), &ci, nullptr, &g_cache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(vkdev(), &ci, nullptr, &g_cache);
    } else if (!data.empty()) {
        NXLOG_I(kTag, "pipeline cache loaded: %zu bytes from %s", data.size(), g_cache_path.c_str());
    }
    g_cache_ready.store(true, std::memory_order_release);
}

// [nvn] pipeline_cache_save: whether the pipeline cache is ever read back from the driver and written to pipeline_cache. auto (default):
// not with Intel's Windows driver, whose vkGetPipelineCacheData faults while copying the data out (igvk64.dll, UHD Graphics on a 12th-gen
// i3, driver 0x001957f1: an access violation on the first save, 3 s into every start; #270); the cache file is still loaded at start,
// and the driver keeps its own disk cache. Under Wine the driver is Mesa's, so auto keeps the save there. on: always. off: never.
bool cache_save_on() {
    static const bool on = [] {
        const std::string v = config_get_str("nvn", "pipeline_cache_save", "auto");
        const uint32_t vendor = device().properties().vendorID;
        const bool intel_win = vendor == 0x8086 && !host_is_wine();
        const bool r = v == "on" || v == "true" || v == "1" || ((v == "auto" || v.empty()) && !intel_win);
        NXLOG_I(kTag, "pipeline_cache_save = %s (%s, vendor 0x%04x%s)", v.c_str(), r ? "on" : "off", vendor,
                intel_win && !r ? ": Intel's Windows driver faults reading the cache back, #270" : "");
        return r;
    }();
    return on;
}

// [nvn] pipeline_cache_save_s: the least time between two background writes of the cache file (the saver thread's, pipeline_cache_dirty).
// A write reads the whole cache out of the driver and rewrites the whole file, whatever was added, and any pipeline created in play made
// one within 2 s. The first write of a session is not held back, and the write at exit (pipeline_cache_flush) is not either, so a
// session that ends without a shutdown loses at most this many seconds of new pipelines from the file. 0: no least time.
std::atomic<int64_t> g_last_save_ms{-1};        // steady clock at the last write, -1: none in this session
int64_t steady_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool save_spacing_passed() {
    static const int64_t gap_ms = [] {
        const int64_t s = std::clamp<int64_t>(config_get_int("nvn", "pipeline_cache_save_s", 30), 0, 3600);
        if (s) NXLOG_I(kTag, "pipeline cache: background writes at least %lld s apart ([nvn] pipeline_cache_save_s)", static_cast<long long>(s));
        return s * 1000;
    }();
    const int64_t last = g_last_save_ms.load(std::memory_order_relaxed);
    return !gap_ms || last < 0 || steady_ms() - last >= gap_ms;
}

void save_cache() {
    if (!g_cache || g_cache_path.empty() || !cache_save_on()) return;
    NXFT("pipe.cache.save");
    g_last_save_ms.store(steady_ms(), std::memory_order_relaxed);
    g_cache_dirty.store(false);
    g_new_since_save.store(0);
    size_t n = 0;
    if (vkGetPipelineCacheData(vkdev(), g_cache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<char> data(n);
    if (vkGetPipelineCacheData(vkdev(), g_cache, &n, data.data()) != VK_SUCCESS) return;
    // written to a temporary file and renamed, so a process that ends in the middle of it leaves the old cache intact
    const std::string tmp = g_cache_path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(data.data(), static_cast<std::streamsize>(n));
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, g_cache_path, ec);
    if (ec) {
        std::filesystem::remove(g_cache_path, ec);
        std::filesystem::rename(tmp, g_cache_path, ec);
    }
}

}  // namespace

// A cache of hundreds of MB (the shader preparation's) is rewritten only after thousands of new pipelines: the new ones are few next to it and
// the write takes seconds. The ordinary small cache is written whenever something new appeared, as before.
bool pipeline_cache_dirty() {
    if (!g_cache_dirty.load(std::memory_order_relaxed) || !cache_save_on()) return false;
    if (g_cache_big && g_new_since_save.load(std::memory_order_relaxed) < 3000) return false;
    return save_spacing_passed();
}
void pipeline_cache_save() { save_cache(); }
// The write at the end of a session: skipped when the cache is the big one of a preparation and little was added.
void pipeline_cache_flush() {
    if (!g_cache_big || g_new_since_save.load(std::memory_order_relaxed) >= 300) save_cache();
}

bool GraphicsKey::operator==(const GraphicsKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
// In-process only: g_gfx buckets. The prewarm key file stores the key bytes and its own digest (nvn_prewarm.cpp digest_of), never this value,
// so the function may change freely. The key has no padding (static_assert), so hashing its words covers exactly the bytes operator== compares.
static_assert(sizeof(GraphicsKey) % 8 == 0 && alignof(GraphicsKey) >= 8, "GraphicsKey is hashed in 8-byte words");
static_assert(offsetof(GraphicsKey, pad_) + sizeof(GraphicsKey::pad_) == offsetof(GraphicsKey, tess), "no padding before tess");
static_assert(sizeof(KeyAttr) == 8, "KeyAttr has no padding");
uint64_t GraphicsKey::hash() const {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(this);
    constexpr uint64_t k0 = 0x9E3779B97F4A7C15ull, k1 = 0xC2B2AE3D27D4EB4Full;
    uint64_t h = k0;
    for (size_t i = 0; i < sizeof *this; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h ^= w;
#if defined(_MSC_VER) && defined(_M_X64)
        uint64_t hi;
        const uint64_t lo = _umul128(h, k1, &hi);
        h = lo ^ hi;
#else
        const unsigned __int128 m = static_cast<unsigned __int128>(h) * k1;
        h = static_cast<uint64_t>(m) ^ static_cast<uint64_t>(m >> 64);
#endif
        h += k0;
    }
    return h ^ (h >> 32);
}

// A module for SPIR-V words. The translator declares OpCapability Float64 in every module although no module uses a 64-bit float;
// devices without shaderFloat64 (Intel iGPUs) reject that, so the capability is dropped when no OpTypeFloat 64 exists.
static VkShaderModule create_module(const std::vector<uint32_t>& words, const char* what) {
    std::vector<uint32_t> fixed;
    const std::vector<uint32_t>* code = &words;
    {
        const auto& w = words;
        size_t cap_at = 0;
        bool uses64 = false;
        for (size_t i = 5; i < w.size();) {
            const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
            if (len == 0) break;
            if (op == 17 && w[i + 1] == 10) cap_at = i;                       // OpCapability Float64
            if (op == 22 && i + 2 < w.size() && w[i + 2] == 64) uses64 = true;  // OpTypeFloat 64
            i += len;
        }
        if (cap_at && !uses64) {
            fixed = w;
            fixed.erase(fixed.begin() + static_cast<std::ptrdiff_t>(cap_at), fixed.begin() + static_cast<std::ptrdiff_t>(cap_at) + 2);
            code = &fixed;
        }
    }
    // c[0] + 0x644 (the draw's base instance, which GLSLC adds to the instance ID) read as Vulkan's BaseInstance: right for direct,
    // indirect and multi-draw indirect draws alike (spirv_base_instance_from_c0; the replay's c[0] bank holds only a direct draw's).
    std::vector<uint32_t> base_inst;
    if (spirv_base_instance_from_c0(*code, base_inst)) code = &base_inst;
    // [nvn] ballot_fix: NVN's warp-aggregated atomic counters made per lane (spirv_ballot_per_lane). After some lanes have returned,
    // their ballot(true) can stop NVIDIA GPUs for good (device lost; pokopia-native #12, #21, #22 on RTX 50, D-1 on an RTX 3050 Laptop).
    // auto (default): NVIDIA (vendor 0x10de) only, as pokopia-native; the rewrite assumes 32-lane subgroups. on: any GPU. off: never.
    static const bool ballot_fix = [] {
        const std::string v = config_get_str("nvn", "ballot_fix", "auto");
        const uint32_t vendor = device().properties().vendorID;
        const bool on = v == "on" || v == "true" || v == "1" || ((v == "auto" || v.empty()) && vendor == 0x10de);
        NXLOG_I(kTag, "ballot_fix = %s (%s, vendor 0x%04x): warp-aggregated counters %s", v.c_str(), on ? "on" : "off", vendor,
                on ? "made per lane" : "left as translated");
        return on;
    }();
    std::vector<uint32_t> per_lane;
    if (ballot_fix) {
        BallotRewrite br;
        if (spirv_ballot_per_lane(*code, per_lane, &br)) {
            code = &per_lane;
            static std::mutex mu;
            static std::unordered_map<std::string, int> seen;
            std::lock_guard<std::mutex> lk(mu);
            if (seen.emplace(what ? what : "?", 1).second)
                NXLOG_I(kTag, "ballot_fix: shader %s: %d ballot%s of %d counter%s made per lane", what ? what : "?", br.ballots,
                        br.ballots == 1 ? "" : "s", br.groups, br.groups == 1 ? "" : "s");
        }
    }
    // [nvn] loop_guard: the game's compute loops whose count is a constant buffer word get it limited (game::Render::loop_guards,
    // spirv_limit_count). auto (default): by the table's size on NVIDIA and under Wine (Mesa's drivers: RADV on the Deck and other AMD
    // handhelds, #531); a plain cap of 16384 elsewhere (AMD's Windows driver crashed making pokopia-native's size-query version). A loop of
    // up to 16384 items a lane can still pass the Deck's 5 s GPU timeout, so the table size is used wherever it builds. table / cap: that on
    // any GPU. off: never.
    static const int loop_guard = [] {   // 0 off, 1 table, 2 cap
        const game::Render* gr = game::info().render;
        if (!gr || !gr->loop_guard_count) return 0;
        const std::string v = config_get_str("nvn", "loop_guard", "auto");
        const uint32_t vendor = device().properties().vendorID;
        const int m = v == "off" || v == "false" || v == "0" ? 0 : v == "table" ? 1 : v == "cap" ? 2 : vendor == 0x10de || host_is_wine() ? 1 : 2;
        NXLOG_I(kTag, "loop_guard = %s (%s, vendor 0x%04x%s)", v.c_str(), m == 0 ? "off" : m == 1 ? "table size" : "cap 16384", vendor,
                host_is_wine() ? ", Wine" : "");
        return m;
    }();
    std::vector<uint32_t> guarded[2];
    if (loop_guard && what) {
        const game::Render& gr = *game::info().render;
        int n = 0, k = 0;
        for (uint32_t g = 0; g < gr.loop_guard_count; g++) {
            const game::LoopGuard& lg = gr.loop_guards[g];
            if (std::strcmp(lg.shader, what) != 0) continue;
            const int c = spirv_limit_count(*code, {lg.count_var, lg.word, lg.table_var, lg.stride}, loop_guard == 1, guarded[k]);
            if (c) code = &guarded[k], k ^= 1, n += c;
        }
        if (n) NXLOG_I(kTag, "loop_guard: shader %s: %d count comparison%s limited", what, n, n == 1 ? "" : "s");
    }
    // Every Location output of a vertex / tessellation evaluation / geometry shader starts at (0,0,0,1) as on the console: a component the
    // shader never writes is undefined in Vulkan, and RADV hands NaN where AMD's Windows driver hands 0 (spirv_init_outputs). Every
    // module goes through here, the ones spirv_add_outputs / spirv_gs_passthrough rewrote included. [nvn] init_outputs = false: off (A/B).
    // [nvn] reduced_precision (default off): the module as the translator's reduced-precision mode writes it (spirv_reduced_precision:
    // no NoContraction, Invariant Position). Done here, so a toggle costs new pipelines, not a new translation; the module words differ,
    // so module_content_id, the driver's caches and the VkPipelineCache never hand a pipeline of the other mode.
    static const bool reduced = config_get_bool("nvn", "reduced_precision", false);
    std::vector<uint32_t> reduced_words;
    if (reduced && spirv_reduced_precision(*code, reduced_words)) code = &reduced_words;
    // [nvn] shadow_blur (default true): 3 x 3 samples for each shadow map sample when the map was made larger (spirv_shadow_blur)
    static const bool shadow_blur = config_get_bool("nvn", "shadow_blur", true);
    std::vector<uint32_t> blurred;
    int blur_taps = 0;
    if (shadow_blur && spirv_shadow_blur(*code, blurred, &blur_taps)) {
        code = &blurred;
        static std::atomic<int> logged{0};
        if (logged++ < 4) NXLOG_I(kTag, "shadow_blur: shader %s: %d shadow map samples", what ? what : "?", blur_taps);
    }
    static const bool init_outputs = config_get_bool("nvn", "init_outputs", true);
    std::vector<uint32_t> initialised;
    if (init_outputs && spirv_init_outputs(*code, initialised)) code = &initialised;
    // [nvn] spirv_salt (measurement only): an OpSourceExtension with the salt in every module gives the driver a new shader hash, so a run
    // compiles every shader from scratch without touching the driver's own cache on disk.
    static const std::string salt = config_get_str("nvn", "spirv_salt", "");
    std::vector<uint32_t> salted;
    if (!salt.empty() && code->size() > 5) {
        salted = *code;
        size_t at = 5;
        while (at < salted.size()) {
            const uint32_t op = salted[at] & 0xffff, len = salted[at] >> 16;
            if (len == 0) break;
            if (op != 17 && op != 10 && op != 11 && op != 14 && op != 15 && op != 16 && op != 331) break;   // capability .. execution mode
            at += len;
        }
        std::vector<uint32_t> ins((salt.size() + 1 + 3) / 4 + 1, 0);
        ins[0] = (static_cast<uint32_t>(ins.size()) << 16) | 4u;                 // OpSourceExtension
        std::memcpy(&ins[1], salt.c_str(), salt.size());
        salted.insert(salted.begin() + static_cast<std::ptrdiff_t>(at), ins.begin(), ins.end());
        code = &salted;
    }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code->size() * 4;
    ci.pCode = code->data();
    VkShaderModule m = VK_NULL_HANDLE;
    const VkResult r = vkCreateShaderModule(vkdev(), &ci, nullptr, &m);
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "vkCreateShaderModule failed (%d) for %s", static_cast<int>(r), what);
        return m;
    }
    uint64_t id = kFnvBasisShort ^ (static_cast<uint64_t>(code->size()) * 0x9e3779b97f4a7c15ull);
    for (uint32_t w : *code) id = fnv_step(id, w);
    {
        std::lock_guard<std::mutex> lk(g_module_id_mu);
        g_module_ids[m] = id;
    }
    return m;
}

VkShaderModule shader_module(const ShaderInfo* s) {
    if (!s) return VK_NULL_HANDLE;
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_modules.find(s);
    if (it != g_modules.end()) return it->second;
    VkShaderModule m = create_module(s->spirv, s->hash.c_str());
    g_modules[s] = m;
    return m;
}

static const std::pair<VkShaderModule, uint32_t>& xfb_variant(const ShaderInfo* vs) {
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_xfb_mod.find(vs);
    if (it != g_xfb_mod.end()) return it->second;
    std::vector<uint32_t> patched;
    std::vector<SpvXfbOutput> lay;
    uint32_t stride = 0;
    VkShaderModule m = VK_NULL_HANDLE;
    if (spirv_add_xfb(vs->spirv, patched, &lay, &stride)) {
        m = create_module(patched, vs->hash.c_str());
        std::string desc;
        for (const SpvXfbOutput& o : lay) desc += " loc" + std::to_string(o.location) + "x" + std::to_string(o.components) + "@" + std::to_string(o.offset);
        NXLOG_I(kTag, "%s: transform feedback variant, %u bytes per vertex:%s", vs->hash.c_str(), stride, desc.c_str());
    } else {
        NXLOG_W(kTag, "%s: no transform feedback variant could be made (no Location outputs?)", vs->hash.c_str());
    }
    return g_xfb_mod.emplace(vs, std::make_pair(m, stride)).first->second;
}

uint32_t xfb_stride(const ShaderInfo* vs) { return vs ? xfb_variant(vs).second : 0; }

static VkShaderModule no_output_module(const ShaderInfo* fs) {
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_no_out.find(fs);
    if (it != g_no_out.end()) return it->second.first;
    std::vector<uint32_t> patched;
    std::pair<VkShaderModule, bool> e = {VK_NULL_HANDLE, false};
    if (spirv_outputs_to_private(fs->spirv, patched)) e = {create_module(patched, fs->hash.c_str()), true};
    else e = {shader_module(fs), false};
    g_no_out[fs] = e;
    return e.first;
}

// The tone-map fragment shader runs as its HDR variant while the HDR scene is active (extras/hdr_nvn.h); every other fragment shader is the translated one.
static VkShaderModule hdr_fragment_module(const ShaderInfo* fs) {
    if (!extras::hdr_scene_active() || fs->spirv.size() > 65536) return shader_module(fs);   // Pokopia's composite tone map is about 24.5k words
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_hdr_frag.find(fs);
    if (it != g_hdr_frag.end()) return it->second.first;
    std::vector<uint32_t> patched;
    const bool variant = extras::hdr_tonemap_variant(fs->spirv, fs->hash, patched);
    VkShaderModule m = variant ? create_module(patched, fs->hash.c_str()) : shader_module(fs);
    g_hdr_frag[fs] = {m, variant};
    return m;
}

static VkShaderModule dual_source_module(const ShaderInfo* fs) {
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_dual_src.find(fs);
    if (it != g_dual_src.end()) return it->second;
    VkShaderModule m = VK_NULL_HANDLE;
    std::vector<uint32_t> patched;
    if (spirv_dual_source_output(fs->spirv, patched)) {
        m = create_module(patched, fs->hash.c_str());
    } else {
        NXLOG_W(kTag, "%s: dual-source blending is on but the fragment shader has no second output; the unpatched module is used", fs->hash.c_str());
        m = shader_module(fs);
    }
    g_dual_src[fs] = m;
    return m;
}

// The slots `consumer` reads that `producer` does not write: the console gives them undefined values, Vulkan needs an output.
static std::vector<SpvOutputSlot> missing_outputs(const ShaderInfo* producer, const ShaderInfo* consumer) {
    std::vector<SpvOutputSlot> missing;
    if (!producer || !consumer) return missing;
    bool covered[64] = {};
    for (const ShaderIo& o : producer->refl.outputs)
        for (uint32_t e = 0; e < (o.array ? o.array : 1); ++e)
            if (o.location + e < 64) covered[o.location + e] = true;
    for (const ShaderIo& in : consumer->refl.inputs) {
        for (uint32_t e = 0; e < (in.array ? in.array : 1); ++e) {
            const uint32_t loc = in.location + e;
            if (loc >= 64 || covered[loc]) continue;
            covered[loc] = true;
            SpvOutputSlot slot;
            slot.location = loc;
            slot.type = in.type == ScalarType::Sint ? SpvScalar::Sint : in.type == ScalarType::Uint ? SpvScalar::Uint : SpvScalar::Float;
            missing.push_back(slot);
        }
    }
    return missing;
}

// The module of `producer` to use in a pipeline whose next stage is `consumer`: the translated one, or a copy with the outputs the
// consumer reads and the producer lacks. Null on a Vulkan error.
static VkShaderModule producer_module(const ShaderInfo* producer, const ShaderInfo* consumer, const std::vector<SpvIntSlot>& ints = {}) {
    const std::vector<SpvOutputSlot> missing = missing_outputs(producer, consumer);
    if (missing.empty() && ints.empty()) return shader_module(producer);
    std::vector<uint32_t> id;
    for (const SpvOutputSlot& m : missing) id.push_back(m.location * 4 + static_cast<uint32_t>(m.type));
    for (const SpvIntSlot& n : ints) id.push_back(0x80000000u | (n.location * 2 + (n.is_signed ? 1u : 0u)));
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_variants.find({producer, id});
    if (it != g_variants.end()) return it->second;
    VkShaderModule m = VK_NULL_HANDLE;
    std::vector<uint32_t> base_words;
    const std::vector<uint32_t>* words = &producer->spirv;
    if (!ints.empty()) {
        // integer vertex attributes: the shader gets the raw bits the console delivers (nvn_spirv_patch.h)
        if (!spirv_int_attributes(producer->spirv, ints, base_words)) {
            NXLOG_E(kTag, "%s: the integer vertex attributes could not be patched in; draws with this shader are skipped", producer->hash.c_str());
            g_variants[{producer, id}] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        words = &base_words;
        if (missing.empty()) {
            m = create_module(base_words, producer->hash.c_str());
            g_variants[{producer, id}] = m;
            return m;
        }
    }
    std::vector<uint32_t> patched;
    // a passthrough geometry shader: the hardware forwards the attributes of the input vertices, the translation has no code for it
    const bool pass = producer->stage == ShaderStage::Geometry && spirv_gs_passthrough(*words, missing, patched);
    if (pass || spirv_add_outputs(*words, missing, patched)) {
        m = create_module(patched, producer->hash.c_str());
        NXLOG_D(kTag, "%s: %zu outputs added for the next stage%s", producer->hash.c_str(), missing.size(), pass ? " (copied from the input vertices)" : "");
    } else {
        NXLOG_W(kTag, "%s: the %zu varyings the next stage reads could not be added; the unpatched module is used", producer->hash.c_str(), missing.size());
        m = shader_module(producer);
    }
    g_variants[{producer, id}] = m;
    return m;
}

const ProgLayout* prog_layout(const ShaderInfo* const stages[6]) {
    LayoutKey key;
    for (int i = 0; i < 6; ++i) key.s[i] = stages[i];
    std::lock_guard<std::mutex> lk(g_layout_mu);
    auto it = g_layouts.find(key);
    if (it != g_layouts.end()) return &it->second;

    ProgLayout pl;
    std::map<std::pair<uint32_t, uint32_t>, size_t> index;
    for (int st = 0; st < 6; ++st) {
        const ShaderInfo* s = stages[st];
        if (!s) continue;
        for (size_t i = 0; i < s->refl.bindings.size(); ++i) {
            const DescriptorBinding& b = s->refl.bindings[i];
            if (!b.used || b.set > 3) continue;
            const auto k = std::make_pair(b.set, b.binding);
            auto f = index.find(k);
            if (f == index.end()) {
                BindingDesc d;
                d.set = b.set;
                d.binding = b.binding;
                d.type = static_cast<VkDescriptorType>(b.type);
                d.stages = stage_bit(s->stage);
                d.shader = s;
                d.index = static_cast<int>(i);
                d.nvn_stage = st;
                index[k] = pl.bindings.size();
                pl.bindings.push_back(d);
            } else {
                pl.bindings[f->second].stages |= stage_bit(s->stage);   // shared binding (the support buffer)
            }
        }
    }
    for (const BindingDesc& d : pl.bindings)
        pl.c0_graphics |= d.nvn_stage != 5 && d.shader->refl.bindings[d.index].nvn.source == NvnSource::DriverConstants;
    // by (set, binding), as ProgLayout promises (build_set and the set_reuse memo walk this list in that order)
    std::sort(pl.bindings.begin(), pl.bindings.end(), [](const BindingDesc& a, const BindingDesc& b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    });
    std::vector<VkDescriptorSetLayoutBinding> lbs[4];
    std::string sig;                                   // the descriptors of the program, as bytes: the key of the shared layout
    for (int set = 0; set < 4; ++set) {
        for (const BindingDesc& d : pl.bindings) {
            if (d.set != static_cast<uint32_t>(set)) continue;
            VkDescriptorSetLayoutBinding b = {};
            b.binding = d.binding;
            b.descriptorType = d.type;
            b.descriptorCount = std::max<uint32_t>(1, d.shader->refl.bindings[d.index].count);
            b.stageFlags = d.stages;
            lbs[set].push_back(b);
            pl.has_set[set] = true;
            const uint32_t w[4] = {b.binding, static_cast<uint32_t>(b.descriptorType), b.descriptorCount, b.stageFlags};
            sig.append(reinterpret_cast<const char*>(w), sizeof w);
        }
        sig.append("|", 1);
    }
    // push constants of generated stages (the pass-through tessellation control shader): one range from offset 0 over those stages
    VkPushConstantRange pcr = {0, 0, 0};
    for (int st = 0; st < 6; ++st)
        if (stages[st] && stages[st]->push_bytes) {
            pcr.stageFlags |= stage_bit(stages[st]->stage);
            pcr.size = std::max(pcr.size, stages[st]->push_bytes);
        }
    if (pcr.size) {
        const uint32_t w[2] = {pcr.stageFlags, pcr.size};
        sig.append("P", 1);
        sig.append(reinterpret_cast<const char*>(w), sizeof w);
    }
    auto sh = g_shared_layouts.find(sig);
    if (sh == g_shared_layouts.end()) {
        SharedLayout nl;
        for (int set = 0; set < 4; ++set) {
            VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            ci.bindingCount = static_cast<uint32_t>(lbs[set].size());
            ci.pBindings = lbs[set].data();
            if (vkCreateDescriptorSetLayout(vkdev(), &ci, nullptr, &nl.sets[set]) != VK_SUCCESS) {
                NXLOG_E(kTag, "descriptor set layout %d failed", set);
                return nullptr;
            }
        }
        VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pci.setLayoutCount = 4;
        pci.pSetLayouts = nl.sets;
        pci.pushConstantRangeCount = pcr.size ? 1 : 0;
        pci.pPushConstantRanges = pcr.size ? &pcr : nullptr;
        if (vkCreatePipelineLayout(vkdev(), &pci, nullptr, &nl.layout) != VK_SUCCESS) {
            NXLOG_E(kTag, "pipeline layout failed");
            return nullptr;
        }
        sh = g_shared_layouts.emplace(sig, nl).first;
    }
    for (int set = 0; set < 4; ++set) pl.sets[set] = sh->second.sets[set];
    pl.layout = sh->second.layout;
    pl.push_stages = pcr.stageFlags;
    pl.push_bytes = pcr.size;
    return &g_layouts.emplace(key, std::move(pl)).first->second;
}

std::vector<SpvIntSlot> key_int_slots(const GraphicsKey& key) {
    // vertex inputs the shader declares as floats but the game feeds with integer attributes (key formats UINT / SINT)
    std::vector<SpvIntSlot> int_slots;
    if (key.stage[0] && !key.xfb)
        for (uint32_t k = 0; k < key.attr_count && k < 16; ++k) {
            const VkFormat f = static_cast<VkFormat>(key.attr[k].format);
            if (!vk_format_is_integer(f)) continue;
            for (const ShaderIo& in : key.stage[0]->refl.inputs)
                if (in.type == ScalarType::Float && key.attr[k].location >= in.location && key.attr[k].location < in.location + (in.array ? in.array : 1)) {
                    SpvIntSlot sl;
                    sl.location = key.attr[k].location;
                    sl.is_signed = vk_format_is_signed_int(f);
                    int_slots.push_back(sl);
                    break;
                }
        }
    return int_slots;
}

bool resolve_stage_modules(const GraphicsKey& key, StageModules* out) {
    *out = StageModules{};
    // the stage that feeds the fragment shader gets outputs for the varyings the fragment shader reads and it does not write
    // (geometry, else tessellation evaluation, else vertex)
    const int feeder = key.stage[2] ? 2 : key.tess[1] ? 4 : 0;
    const std::vector<SpvIntSlot> int_slots = key_int_slots(key);
    for (int i = 0; i < 3; ++i) {
        if (!key.stage[i]) continue;
        if (key.xfb && i != 0) continue;     // stream-out draws run the vertex stage only
        if (key.xfb) {
            VkShaderModule m = xfb_variant(key.stage[0]).first;
            if (!m) return false;
            out->m[0] = m;
            continue;
        }
        VkShaderModule m = (i == feeder && key.stage[1]) ? producer_module(key.stage[i], key.stage[1], i == 0 ? int_slots : std::vector<SpvIntSlot>())
                           : (i == 0 && !int_slots.empty()) ? producer_module(key.stage[i], nullptr, int_slots)
                           : (i == 1 && key.no_out)      ? no_output_module(key.stage[i])
                           : (i == 1 && key.dual_src)    ? dual_source_module(key.stage[i])
                           : (i == 1)                    ? hdr_fragment_module(key.stage[i])
                                                         : shader_module(key.stage[i]);
        if (!m) return false;
        out->m[i] = m;
    }
    // tessellation stages (never under stream-out: the replay does not build such keys)
    for (int t = 0; t < 2 && !key.xfb; ++t) {
        const ShaderInfo* s = key.tess[t];
        if (!s) continue;
        VkShaderModule m = (t == 1 && feeder == 4 && key.stage[1]) ? producer_module(s, key.stage[1], std::vector<SpvIntSlot>()) : shader_module(s);
        if (!m) return false;
        out->m[3 + t] = m;
    }
    return true;
}

const VkPipelineDynamicStateCreateInfo* pipeline_dynamic_state() {
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_LINE_WIDTH};
    static const VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0,
                                                        sizeof(dyn) / sizeof(dyn[0]), dyn};
    return &dy;
}

VkPipelineCache pipeline_cache_handle() {
    ensure_cache();
    return g_cache;
}

void note_pipeline_created(double ms, const char* what) {
    counters().pipelines_created.fetch_add(1);
    g_pipes_created.add(1);
    g_cache_dirty.store(true, std::memory_order_relaxed);
    if (ms >= 2.0) g_new_since_save.fetch_add(1, std::memory_order_relaxed);      // a part the cache already had comes back in well under that
    NXLOG_D(kTag, "%s created in %.1f ms", what, ms);
}

VkPipeline build_graphics_pipeline(const GraphicsKey& key, const ProgLayout& layout) {
    ensure_cache();
    const auto t0 = std::chrono::steady_clock::now();

    VkPipelineShaderStageCreateInfo stages[5] = {};
    uint32_t ns = 0;
    const VkShaderStageFlagBits bits[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                           VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT};
    StageModules sm;
    if (!resolve_stage_modules(key, &sm)) return VK_NULL_HANDLE;
    for (int i = 0; i < 5; ++i) {
        if (!sm.m[i]) continue;
        stages[ns] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[ns].pNext = subgroup32_info(bits[i]);
        stages[ns].stage = bits[i];
        stages[ns].module = sm.m[i];
        stages[ns].pName = "main";
        ++ns;
    }

    // vertex input
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t nvb = 0, nva = 0;
    bool binding_seen[16] = {};
    for (uint32_t i = 0; i < key.attr_count && i < 16; ++i) {
        const KeyAttr& a = key.attr[i];
        const VkFormat vf = format_guard().vertex_fetch_format(static_cast<VkFormat>(a.format));   // device without vertex fetch for it: sibling or left out
        if (vf == VK_FORMAT_UNDEFINED) continue;
        va[nva++] = {a.location, a.binding, vf, a.offset};
        if (!binding_seen[a.binding]) {
            binding_seen[a.binding] = true;
            vb[nvb++] = {a.binding, 0, key.binding_instance[a.binding] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        }
    }
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nvb;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = nva;
    vi.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = static_cast<VkPrimitiveTopology>(key.topology);
    VkPipelineTessellationStateCreateInfo ts = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    ts.patchControlPoints = key.patch_control_points;

    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    dcc.negativeOneToOne = key.depth_neg_one ? VK_TRUE : VK_FALSE;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    if (device().features().depth_clip_control) vp.pNext = &dcc;   // viewport and scissor counts are dynamic

    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = key.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = static_cast<VkPolygonMode>(key.polygon_mode);
    rs.lineWidth = 1.0f;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(key.samples ? key.samples : 1);
    ms.alphaToCoverageEnable = key.alpha_to_coverage ? VK_TRUE : VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < key.color_count && i < 8; ++i) {
        cba[i] = {};
        cba[i].blendEnable = key.blend_enable[i] && format_guard().blend_allowed(static_cast<VkFormat>(key.color_format[i])) ? VK_TRUE : VK_FALSE;
        cba[i].srcColorBlendFactor = static_cast<VkBlendFactor>(key.blend_src_rgb[i]);
        cba[i].dstColorBlendFactor = static_cast<VkBlendFactor>(key.blend_dst_rgb[i]);
        cba[i].colorBlendOp = static_cast<VkBlendOp>(key.blend_op_rgb[i]);
        cba[i].srcAlphaBlendFactor = static_cast<VkBlendFactor>(key.blend_src_a[i]);
        cba[i].dstAlphaBlendFactor = static_cast<VkBlendFactor>(key.blend_dst_a[i]);
        cba[i].alphaBlendOp = static_cast<VkBlendOp>(key.blend_op_a[i]);
        cba[i].colorWriteMask = key.write_mask[i];
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.logicOpEnable = key.logic_op_enable ? VK_TRUE : VK_FALSE;
    cb.logicOp = static_cast<VkLogicOp>(key.logic_op);
    cb.attachmentCount = key.color_count;
    cb.pAttachments = cba;

    VkFormat cf[8];
    for (uint32_t i = 0; i < key.color_count && i < 8; ++i) cf[i] = static_cast<VkFormat>(key.color_format[i]);
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = key.color_count;
    ri.pColorAttachmentFormats = cf;
    ri.depthAttachmentFormat = static_cast<VkFormat>(key.depth_format);
    ri.stencilAttachmentFormat = static_cast<VkFormat>(key.stencil_format);

    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    gi.stageCount = ns;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pTessellationState = key.patch_control_points ? &ts : nullptr;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = pipeline_dynamic_state();
    gi.layout = layout.layout;
    if (key.fb_loop) gi.flags |= VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;   // the draw samples its own colour attachment
    VkPipeline pipe = VK_NULL_HANDLE;
    const VkResult r = create_graphics_pipeline_tracked(vkdev(), g_cache, &gi, &pipe);
    const double ms_taken = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "graphics pipeline failed (%d): vs %s fs %s, %u colour targets", static_cast<int>(r), key.stage[0] ? key.stage[0]->hash.c_str() : "-",
                key.stage[1] ? key.stage[1]->hash.c_str() : "-", key.color_count);
        pipe = VK_NULL_HANDLE;
    } else {
        counters().pipelines_created.fetch_add(1);
        g_cache_dirty.store(true, std::memory_order_relaxed);
    g_new_since_save.fetch_add(1, std::memory_order_relaxed);
        g_pipes_created.add(1);
        NXLOG_D(kTag, "graphics pipeline created in %.1f ms (vs %s fs %s)", ms_taken, key.stage[0] ? key.stage[0]->hash.c_str() : "-",
                key.stage[1] ? key.stage[1]->hash.c_str() : "-");
    }
    return pipe;
}

// The entry of the previous call (same key as the last draw: most state changes only re-ask for the same pipeline): skips hashing and the map.
// Render thread only like g_gfx; `gen` drops it when g_gfx is cleared. Node pointers of unordered_map are stable; the vector inside is addressed by index.
static uint32_t g_gfx_gen = 1;
struct LastGfx {
    GraphicsKey key;
    std::vector<GfxEntry>* bucket = nullptr;
    size_t idx = 0;
    uint32_t gen = 0;
};

static VkPipeline take_entry(GfxEntry& e) {
    counters().pipeline_cache_hits.fetch_add(1, std::memory_order_relaxed);
    if (e.up && e.up->state.load(std::memory_order_acquire) != 0) gpl_take_upgrade(e.second, e.alt, e.up);
    if (e.alt && gpl_ab_whole()) return e.alt;
    if (!e.second && e.up) g_aw_skipped.fetch_add(1, std::memory_order_relaxed);   // still being built in the background: skipped
    return e.second;
}

VkPipeline graphics_pipeline(const GraphicsKey& key, const ProgLayout& layout) {
    ensure_cache();
    static const bool use_last = nxrt::config_get_bool("nvn", "pipe_last", true);
    static thread_local LastGfx last;
    if (use_last && last.gen == g_gfx_gen && last.bucket && last.idx < last.bucket->size() && last.key == key)
        return take_entry((*last.bucket)[last.idx]);
    const uint64_t h = key.hash();
    auto& bucket = g_gfx[h];
    for (size_t i = 0; i < bucket.size(); ++i)
        if (bucket[i].first == key) {
            if (use_last) { last.key = key; last.bucket = &bucket; last.idx = i; last.gen = g_gfx_gen; }
            return take_entry(bucket[i]);
        }
    prewarm_init();
    VkPipeline pipe = VK_NULL_HANDLE;
    std::shared_ptr<PipeUpgrade> later;          // the key file's whole pipeline, still being built: it replaces the linked one
    // [nvn] async_whole in play (the loading gate, gpl_gate_open): nothing is built whole or waited for here (stream-out draws excepted)
    const bool no_stall = async_whole_on() && !key.xfb && gpl_gate_open();
    // compiled ahead on a worker; one being compiled is linked from its parts meanwhile when they are built (else waited for, or with
    // no_stall left to the worker while the draw is skipped)
    if (prewarm_take(key, &pipe, [&] { return gpl_ready(key, layout); }, &later, no_stall)) {
        ++g_pipeline_count;
        g_gfx_keys.add(1);
        bucket.push_back({key, pipe, VK_NULL_HANDLE, nullptr});
        return pipe;
    }
    std::shared_ptr<PipeUpgrade> up;
    bool pending = false;
    pipe = gpl_link(key, layout, later ? nullptr : &up, &pending); // linked from shader parts compiled ahead (null: not eligible or a part failed)
    if (later) up = later;
    prewarm_record(key);                         // every key a draw asked for: the next run builds it from boot
    if (pending && !later) return VK_NULL_HANDLE;   // its parts are compiling in the background: the draw is skipped, the next one asks again
    g_gfx_keys.add(1);                           // every path below adds one entry to the bucket
    if (pipe) {
        ++g_pipeline_count;
        bucket.push_back({key, pipe, VK_NULL_HANDLE, up});
        return pipe;
    }
    if (pending) {                               // (rare) parts gone missing between the check and the link: skipped until the whole pipeline is in
        bucket.push_back({key, VK_NULL_HANDLE, VK_NULL_HANDLE, up});
        return VK_NULL_HANDLE;
    }
    if (no_stall) {                              // [nvn] async_whole: the key file's build, or one of our own, replaces the empty entry
        if (!up) up = aw_queue(key, layout);
        if (up) {
            ++g_pipeline_count;                  // counted now: the swap in gpl_take_upgrade replaces, it does not add
            bucket.push_back({key, VK_NULL_HANDLE, VK_NULL_HANDLE, up});
            g_aw_skipped.fetch_add(1, std::memory_order_relaxed);
            return VK_NULL_HANDLE;
        }
    }
    NXFT("pipe.create");
    pipe = build_graphics_pipeline(key, layout);
    if (pipe) {
        ++g_pipeline_count;
        g_built_at_draw.fetch_add(1, std::memory_order_relaxed);
    }
    bucket.push_back({key, pipe, VK_NULL_HANDLE, up});   // up: the key file's build, when one is under way, replaces this one (same pipeline)
    return pipe;
}

// ---- compute pipelines ----------------------------------------------------------------------------------------------------------
namespace {
uint64_t compute_code_id(const ShaderInfo* cs) {
    uint64_t v = 0;
    for (char ch : cs->hash) {
        uint64_t d;
        if (ch >= '0' && ch <= '9') d = static_cast<uint64_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint64_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = static_cast<uint64_t>(ch - 'A' + 10);
        else { v = 0; break; }
        v = (v << 4) | d;
    }
    return v ? v : reinterpret_cast<uint64_t>(cs);
}

// Any thread, no lock held. Counters as before; the hitch phase only on the render thread.
VkPipeline create_compute(const ShaderInfo* cs, const ProgLayout& layout) {
    ensure_cache();
    VkShaderModule m = shader_module(cs);
    if (!m) return VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.pNext = subgroup32_info(VK_SHADER_STAGE_COMPUTE_BIT,
                                     uint64_t{cs->local_size[0]} * cs->local_size[1] * cs->local_size[2]);
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = layout.layout;
    const uint64_t tp0 = nxrt::hitch_ticks();
    const VkResult cr = vkCreateComputePipelines(vkdev(), g_cache, 1, &ci, nullptr, &pipe);
    if (nxrt::hitch_on_render_thread()) nxrt::hitch_phase_add(nxrt::kHpRenderPipeline, nxrt::hitch_ticks() - tp0);
    if (cr != VK_SUCCESS) {
        NXLOG_E(kTag, "compute pipeline failed for %s", cs->hash.c_str());
        return VK_NULL_HANDLE;
    }
    counters().pipelines_created.fetch_add(1);
    ++g_pipeline_count;
    g_pipes_created.add(1);
    g_cache_dirty.store(true, std::memory_order_relaxed);
    g_new_since_save.fetch_add(1, std::memory_order_relaxed);
    return pipe;
}

// The entry of (cs, layout), made (state 0) when absent. g_compute_mu held.
ComputeEntry& compute_entry_locked(const ShaderInfo* cs, const ProgLayout& layout) {
    auto [it, fresh] = g_compute.try_emplace(ComputeKey{compute_code_id(cs), layout.layout});
    if (fresh) g_compute_keys.add(1);
    return it->second;
}

// A background build (async thread or prewarm worker): only when nobody built it or is building it. True: this call built it.
bool compute_build_bg(const ShaderInfo* cs, const ProgLayout& layout, bool prewarm) {
    ComputeEntry* e;
    {
        std::lock_guard<std::mutex> lk(g_compute_mu);
        e = &compute_entry_locked(cs, layout);
        if (e->state != 0) return false;
        e->state = 1;
    }
    VkPipeline p;
    {
        NXFT("cs.build");
        p = create_compute(cs, layout);
    }
    {
        std::lock_guard<std::mutex> lk(g_compute_mu);
        e->pipe = p;
        e->state = 2;
    }
    g_compute_cv.notify_all();
    (prewarm ? g_cs_prewarmed : g_cs_background).fetch_add(1, std::memory_order_relaxed);
    if (p && !prewarm) prewarm_record_compute(cs, compute_layout_key(layout));     // the key file's record: built at boot next time
    return true;
}
}  // namespace

uint64_t compute_layout_key(const ProgLayout& layout) {
    uint64_t h = kFnvBasisShort;
    auto mix = [&h](uint64_t v) { for (int i = 0; i < 8; ++i) h = fnv_step(h, (v >> (8 * i)) & 0xff); };
    for (const BindingDesc& d : layout.bindings) {
        mix(d.set);
        mix(d.binding);
        mix(static_cast<uint64_t>(d.type));
        mix(d.stages);
    }
    mix(layout.push_stages);
    mix(layout.push_bytes);
    return h;
}

// Render thread: the pipeline, built here when nobody has it. One a background thread is building is waited for (`cs.wait`; the old path
// built it here, which takes at least as long); one still queued is taken over.
VkPipeline compute_pipeline(const ShaderInfo* cs, const ProgLayout& layout) {
    std::unique_lock<std::mutex> lk(g_compute_mu);
    ComputeEntry& e = compute_entry_locked(cs, layout);
    if (e.state == 2) return e.pipe;
    if (e.state == 1) {
        NXFT("cs.wait");
        g_compute_cv.wait(lk, [&] { return e.state == 2; });
        return e.pipe;
    }
    e.state = 1;
    lk.unlock();
    const auto t0 = std::chrono::steady_clock::now();
    VkPipeline p = create_compute(cs, layout);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    lk.lock();
    e.pipe = p;
    e.state = 2;
    lk.unlock();
    g_compute_cv.notify_all();
    g_cs_at_dispatch.fetch_add(1, std::memory_order_relaxed);
    NXLOG_I(kTag, "compute built at the dispatch %s (%.1f ms%s)", cs->hash.c_str(), ms, gpl_gate_open() ? "" : ", loading");
    if (p) prewarm_record_compute(cs, compute_layout_key(layout));
    return p;
}

bool compute_pipeline_done(const ShaderInfo* cs, const ProgLayout& layout) {
    std::lock_guard<std::mutex> lk(g_compute_mu);
    auto it = g_compute.find(ComputeKey{compute_code_id(cs), layout.layout});
    return it != g_compute.end() && it->second.state == 2;
}

bool compute_queue_front(const ShaderInfo* cs, const ProgLayout& layout) {
    {
        std::lock_guard<std::mutex> lk(g_compute_mu);
        auto [it, fresh] = g_compute.try_emplace(ComputeKey{compute_code_id(cs), layout.layout});
        if (!fresh) return true;                  // queued by an earlier dispatch, being built, or done meanwhile
        g_compute_keys.add(1);
    }
    AsyncJob j;
    j.cs = cs;
    j.layout = &layout;
    return aw_push(std::move(j), true);
}

void compute_program_set(const ShaderInfo* cs) {
    if (!cs || !async_compute_on()) return;
    static std::mutex mu;
    static std::unordered_map<const ShaderInfo*, bool> seen;           // queued once per shader
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!seen.emplace(cs, true).second) return;
    }
    AsyncJob j;
    j.cs = cs;                                    // layout derived on the async thread (prog_layout takes its own lock)
    aw_push(std::move(j), false);
}

bool compute_build_ahead(const ShaderInfo* cs, uint64_t layout_key) {
    const ShaderInfo* st6[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, cs};
    const ProgLayout* pl = cs ? prog_layout(st6) : nullptr;
    if (!pl || compute_layout_key(*pl) != layout_key) return false;    // the shader's descriptors changed since it was recorded
    compute_build_bg(cs, *pl, true);
    return true;
}

ComputeStats compute_stats() {
    ComputeStats s;
    s.skipped = g_cs_skipped.load(std::memory_order_relaxed);
    s.at_dispatch = g_cs_at_dispatch.load(std::memory_order_relaxed);
    s.prewarmed = g_cs_prewarmed.load(std::memory_order_relaxed);
    s.background = g_cs_background.load(std::memory_order_relaxed);
    return s;
}

void compute_note_skipped() { g_cs_skipped.fetch_add(1, std::memory_order_relaxed); }

bool async_compute_on() {
    static const bool on = [] {
        const bool v = nxrt::config_get_bool("nvn", "async_compute", true);
        NXLOG_I(kTag, "async_compute %s", v ? "on: compute pipelines are built in the background when a program is set, a dispatch that "
                                              "only GPU memory sees is skipped while its pipeline builds" : "off");
        return v;
    }();
    return on;
}

bool compute_prewarm_on() {
    static const bool on = nxrt::config_get_bool("nvn", "compute_prewarm", true);
    return on;
}

VkBuffer constant_data_buffer(const ShaderInfo* s, VkDeviceSize* range) {
    auto it = g_const_bufs.find(s);
    if (it == g_const_bufs.end()) {
        const VkDeviceSize size = std::max<VkDeviceSize>(256, (s->constants.size() + 15) & ~15ull);
        gfx::Buffer b;
        if (device().create_buffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryUse::Upload, &b, "nvn c1") != VK_SUCCESS) {
            *range = 0;
            return VK_NULL_HANDLE;
        }
        std::memset(b.mapped, 0, size);
        if (!s->constants.empty()) std::memcpy(b.mapped, s->constants.data(), s->constants.size());
        it = g_const_bufs.emplace(s, std::make_pair(b, size)).first;
        g_const_n.add(1);
        g_const_bytes.add(static_cast<int64_t>(size));
    }
    *range = it->second.second;
    return it->second.first.buffer;
}

// The shared c[0] for the stages the replay does not fill per draw / dispatch (driver_bank_for in nvn_replay.cpp, nvn_driver_bank.h).
VkBuffer driver_constants_buffer(VkDeviceSize* range) {
    if (!g_driver_const_made) {
        g_driver_const_made = true;
        if (device().create_buffer(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryUse::Upload, &g_driver_const, "nvn c0") == VK_SUCCESS) {
            std::memset(g_driver_const.mapped, 0, 65536);
            // c[0]+0x610: maximum point size read by the agl vertex shaders (gl_PointSize = min(size, c0[0x610]))
            const float max_point = 64.0f;
            std::memcpy(static_cast<uint8_t*>(g_driver_const.mapped) + 0x610, &max_point, 4);
        }
    }
    *range = 65536;
    return g_driver_const.buffer;
}

size_t pipeline_count() { return static_cast<size_t>(g_pipeline_count.get()); }

void module_cache_clear() {
    VkDevice d = vkdev();
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    for (auto& [k, m] : g_modules) if (m) vkDestroyShaderModule(d, m, nullptr);
    g_modules.clear();
    for (auto& [k, m] : g_variants) if (m) vkDestroyShaderModule(d, m, nullptr);
    g_variants.clear();
    for (auto& [k, m] : g_dual_src) if (m) vkDestroyShaderModule(d, m, nullptr);
    g_dual_src.clear();
    for (auto& [k, e] : g_no_out) if (e.first && e.second) vkDestroyShaderModule(d, e.first, nullptr);
    g_no_out.clear();
    for (auto& [k, e] : g_hdr_frag) if (e.first && e.second) vkDestroyShaderModule(d, e.first, nullptr);
    g_hdr_frag.clear();                 // its other entries are g_modules' (destroyed above): none may outlive this
    for (auto& [k, v] : g_xfb_mod) if (v.first) vkDestroyShaderModule(d, v.first, nullptr);
    g_xfb_mod.clear();
    std::lock_guard<std::mutex> lk2(g_module_id_mu);
    g_module_ids.clear();
}

uint64_t module_content_id(VkShaderModule m) {
    std::lock_guard<std::mutex> lk(g_module_id_mu);
    auto it = g_module_ids.find(m);
    return it != g_module_ids.end() ? it->second : reinterpret_cast<uint64_t>(m);   // not made by create_module: the handle stands for itself
}

uint64_t pipelines_built_at_draw() { return g_built_at_draw.load(std::memory_order_relaxed); }
uint64_t pipelines_built_async() { return g_aw_built.load(std::memory_order_relaxed); }
uint64_t draws_skipped_async() { return g_aw_skipped.load(std::memory_order_relaxed); }

bool async_whole_on() {
    static const bool on = [] {
        // On every platform (first-run pop-in instead of hitches on Windows too, #120); `auto` (the old default) means the same.
        const std::string v = nxrt::config_get_str("nvn", "async_whole", "true");
        bool r = v == "auto" || v == "on" || v == "1" || v == "true";
        // [nvn] gpl_ab keeps a finished build as the measured alternative instead of swapping it in: an empty entry would stay empty
        const bool ab = r && nxrt::config_get_int("nvn", "gpl_ab", 0) > 0;
        if (ab) r = false;
        NXLOG_I(kTag, "async_whole %s (%s%s, %s)", r ? "on" : "off", v.c_str(), ab ? ", off with gpl_ab" : "", nxrt::host_is_wine() ? "Wine" : "not Wine");
        return r;
    }();
    return on;
}

void pipeline_shutdown() {
    VkDevice d = vkdev();
    prewarm_shutdown();
    aw_stop();
    gpl_stop();
    if (!g_cache_big || g_new_since_save.load() >= 300) save_cache();
    for (auto& [h, bucket] : g_gfx)
        for (auto& e : bucket) {
            if (e.second) vkDestroyPipeline(d, e.second, nullptr);
            if (e.alt) vkDestroyPipeline(d, e.alt, nullptr);
            if (e.up && e.up->state.load() == 1 && e.up->pipe) vkDestroyPipeline(d, e.up->pipe, nullptr);     // built and never taken
        }
    g_gfx.clear();
    ++g_gfx_gen;
    g_gfx_keys.sub(g_gfx_keys.get());
    gpl_shutdown();                              // the parts the pipelines were linked from
    {
        std::lock_guard<std::mutex> lk(g_compute_mu);   // the async and prewarm threads are stopped: nothing else holds an entry
        for (auto& [k, e] : g_compute)
            if (e.pipe) vkDestroyPipeline(d, e.pipe, nullptr);
        g_compute.clear();
    }
    g_compute_keys.sub(g_compute_keys.get());
    g_layouts.clear();
    for (auto& [k, l] : g_shared_layouts) {          // the handles in g_layouts were copies of these
        if (l.layout) vkDestroyPipelineLayout(d, l.layout, nullptr);
        for (VkDescriptorSetLayout s : l.sets)
            if (s) vkDestroyDescriptorSetLayout(d, s, nullptr);
    }
    g_shared_layouts.clear();
    module_cache_clear();                        // the threads are stopped: its locks are free
    for (auto& [k, b] : g_const_bufs) device().destroy_buffer(b.first);
    g_const_bufs.clear();
    g_const_n.sub(g_const_n.get());
    g_const_bytes.sub(g_const_bytes.get());
    if (g_driver_const_made) device().destroy_buffer(g_driver_const);
    g_driver_const_made = false;
    if (g_cache) vkDestroyPipelineCache(d, g_cache, nullptr);
    g_cache = VK_NULL_HANDLE;
    g_cache_tried = false;
    g_cache_ready.store(false);
    g_cache_dirty.store(false);
    g_last_save_ms.store(-1);
    g_pipeline_count.sub(g_pipeline_count.get());
}

// Gauges (nxrt/gauge.h). The callbacks read the layout and module maps under the locks that guard them; they are removed before those maps
// are destroyed.
namespace {
struct PipelineGauges {
    PipelineGauges() {
        nxrt::gauge_register("nvn.pipeline_layouts", [] {
            std::lock_guard<std::mutex> lk(g_layout_mu);
            return static_cast<int64_t>(g_layouts.size());
        });
        nxrt::gauge_register("nvn.shared_layouts", [] {
            std::lock_guard<std::mutex> lk(g_layout_mu);
            return static_cast<int64_t>(g_shared_layouts.size());
        });
        nxrt::gauge_register("nvn.shader_modules", [] {
            std::lock_guard<std::recursive_mutex> lk(g_module_mu);
            return static_cast<int64_t>(g_modules.size());
        });
        nxrt::gauge_register("nvn.shader_modules.variants", [] {
            std::lock_guard<std::recursive_mutex> lk(g_module_mu);
            return static_cast<int64_t>(g_variants.size());
        });
        nxrt::gauge_register("nvn.shader_modules.dual_src", [] {
            std::lock_guard<std::recursive_mutex> lk(g_module_mu);
            return static_cast<int64_t>(g_dual_src.size());
        });
        nxrt::gauge_register("nvn.shader_modules.no_out", [] {
            std::lock_guard<std::recursive_mutex> lk(g_module_mu);
            return static_cast<int64_t>(g_no_out.size());
        });
        nxrt::gauge_register("nvn.shader_modules.xfb", [] {
            std::lock_guard<std::recursive_mutex> lk(g_module_mu);
            return static_cast<int64_t>(g_xfb_mod.size());
        });
    }
    ~PipelineGauges() {
        for (const char* n : {"nvn.pipeline_layouts", "nvn.shared_layouts", "nvn.shader_modules", "nvn.shader_modules.variants",
                              "nvn.shader_modules.dual_src", "nvn.shader_modules.no_out", "nvn.shader_modules.xfb"})
            nxrt::gauge_unregister(n);
    }
};
PipelineGauges g_pipeline_gauges;
}  // namespace

const void* subgroup32_info(VkShaderStageFlagBits stage, uint64_t compute_invocations) {
    static const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo k32 = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, nullptr, 32};
    // [nvn] subgroup32 = 0: never ask (the driver's own width)
    static const bool on = config_get_bool("nvn", "subgroup32", true);
    const gfx::DeviceLimitsExt& l = device().limits_ext();
    const bool needed = on && l.subgroup_size != 32;
    const bool can = l.subgroup_size_control && l.min_subgroup_size <= 32 && 32 <= l.max_subgroup_size;
    static std::once_flag logged;
    std::call_once(logged, [&] {
        if (!needed) return;
        const uint32_t game = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                              VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        const uint32_t pinned = can ? (l.required_subgroup_stages & game) : 0;
        NXLOG_W("nvn", "subgroups are %u wide here, the game's shaders assume 32: asking for 32 in stages 0x%x%s", l.subgroup_size, pinned,
                (game & ~pinned) ? "; the other stages run at the driver's width (Mesa RADV: RADV_PERFTEST=cswave32,pswave32,gewave32 makes them 32 wide)" : "");
    });
    if (!needed || !can || !(l.required_subgroup_stages & stage)) return nullptr;
    // VUID-VkPipelineShaderStageCreateInfo-pNext-02756: the workgroup must fit in maxComputeWorkgroupSubgroups subgroups of 32. A bigger
    // one asked to run 32 wide is invalid usage, and AMD's Windows driver can end the process on it instead of failing the call (#487)
    if (stage == VK_SHADER_STAGE_COMPUTE_BIT && compute_invocations > uint64_t{l.max_compute_workgroup_subgroups} * 32) {
        static std::atomic<uint32_t> skipped{0};
        if (skipped.fetch_add(1, std::memory_order_relaxed) < 8)
            NXLOG_W("nvn", "compute workgroup of %llu invocations is over %u subgroups of 32: left at the driver's width",
                    static_cast<unsigned long long>(compute_invocations), l.max_compute_workgroup_subgroups);
        return nullptr;
    }
    return &k32;
}

}  // namespace nxrt::nvn::be::gpu
