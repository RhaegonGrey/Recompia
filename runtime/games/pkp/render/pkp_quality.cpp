// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, PC picture quality (pkp_quality.h). Offsets in `main` of the 2.0.1 update; the RE notes are work/audit/quality_params.md.
//
// View distance is the console's and cannot be changed (#249). The game's shadow projection and the pictures in frames and photo
// displays depend on the scene camera's far clip: a longer one made the shadows follow the camera, seamed the block textures and blanked
// framed pictures until the player walked up to them. The cameras keep the game's own near and far; nothing here touches them.
//
// The kids::placeable::camera parameter object (property table 0x71009d3010: FXAA* +0x1dc.., ShadowCascade*Start +0xafc..,
// ShadowmapKeyLightResolution +0xb98, SSAO* +0x12c4.., RenderResolutionRatio +0x1a5c ...) is committed by 0x71022a9f60 (vtable slot 159, entries
// 0x710327f770 / 0x710343ee90). A full new-game run never reached the mid hook after that commit (0x71022aced8): the game
// loads these objects from data (0x71008877a0 -> 0x7100887800, then 0x71022b3700 copies live -> staged; the data blend 0x71022adba0 ->
// 0x71022ae680 copies the quality fields) and does not commit them during play. The hook stays as a probe: the first call logs, and it
// logs the quality fields once per distinct set. None of them is changed here (their renderer readers are not confirmed).
//
// Texture sharpness: the renderer's own sampler tuning ([display] lod_bias, anisotropy, play_display.cpp) on every NVN sampler. A
// [pkp] quality_preset in the ini or the environment wins over the F1 page's values at start and after every settings change (texture
// values only: a device preset's render scale and frame limit come from the F1 page's [display] values).
//
// Fog strength ([pkp] fog_strength, 0 .. 1.5, 1 = the console). The height-fog pass (fragment shader 7c8c05da24a1f5a2, a full-screen pass
// after lighting) blends three fog layers over the scene: per layer i, factor = (1 - exp(-ramp^2)) * clamp(c3[i].w, 0, 1) * distance ramp,
// colour c3[i].xyz, where c3 is its uniform block 3 (73 vec4s; c3[3..5] the height ramps, c3[6..65] four cos fluctuations per layer,
// c3[66] mask-map transform, c3[67] flags, c3[68] depth linearisation, c3[69..72] the inverse view). The NVN back end scales the three
// layer amounts c3[0..2].w (byte offsets 0x0c, 0x1c, 0x2c) by the strength when it binds that block (nxrt/gfx_tuning.h): 0 removes the
// fog, values above 1 raise only the layers whose amount is below 1 (the shader clamps it). No guest hook: the CPU writer of the block was not
// found (the env parameter table 0x71009d3010 has HeightFogLayer0/1 Start, End and Color at +0x8d8..+0x908, but no 3-layer writer was traced).
#include "render/pkp_quality.h"
#include "core/pkp_guest.h"
#include "core/pkp_settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/gfx_tuning.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kCamCommitEnd = 0x22aced8;   // mid: kids camera commit 0x71022a9f60 after the lock release, x19 = the camera (probe)
constexpr uint64_t kLodJobs[] = {0x2371280, 0x23719ac, 0x2371d80};   // entry: model LOD job builders

// camera fields (property table 0x71009d3010)
constexpr size_t kNear = 0x10, kFar = 0x14, kFxaaEnable = 0x1ec, kFxaaType = 0x1f0, kFxaaLevel = 0x200, kCascade1 = 0xafc, kCascade2 = 0xb00,
                 kCascade3 = 0xb04, kKeyLightRes = 0xb98, kSsaoQuality = 0x12e4, kSsaoRes = 0x12e8, kRenderRatio = 0x1a5c, kSsRatio = 0x1a9c,
                 kSsEnable = 0x1aa0, kCamSize = 0x1f30;

struct Preset {
    const char* name;
    double lod_bias;
    int anisotropy;
    bool frame;          // a device preset: also sets the render scale and the frame limit
    int render_scale;    // [display] render_scale (0 = auto: the window's size, never below 1080p off a handheld)
    int fps_cap;         // [display] fps_limit (-1 = the display's refresh rate)
    gfx::ShadowQuality shadows;   // [display] shadow_quality
    extras::AaMode aa;            // [upscale] aa
};
// switch2 = the console's picture; the PC presets raise the texture sharpness. The device presets (#122) also pick a render scale and a
// frame limit for the machine (100 = 1920 x 1080). Upscalers stay off; fog ([pkp] fog_strength) and the view distance are the console's.
constexpr Preset kPresets[] = {
    {"switch2", 0.0, 0, false, 0, 0, gfx::ShadowQuality::Default, extras::AaMode::Off},
    {"high", -0.5, 8, false, 0, 0, gfx::ShadowQuality::High, extras::AaMode::SmaaHigh},
    {"ultra", -1.0, 16, false, 0, 0, gfx::ShadowQuality::VeryHigh, extras::AaMode::SmaaUltra},
    {"steamdeck", 0.0, 4, true, 75, 40, gfx::ShadowQuality::Default, extras::AaMode::Off},    // 1440 x 810 for the 1280 x 800 screen; 40 fps. The preset does not set vsync: with vsync off (the default) the limiter runs at 40 on its own clock whatever the refresh rate. Only with vsync on does the cap snap ([display] fps_snap): 40 where the refresh is a multiple (80 Hz OLED / 40 Hz LCD in Steam's Performance menu), else the next divisor down (30 at 90 Hz and at 60 Hz)
    {"legion_go_s", 0.0, 8, true, 80, 60, gfx::ShadowQuality::Default, extras::AaMode::Off},  // 1536 x 864 for the 1920 x 1200 screen, 60 fps
    {"laptop", 0.0, 4, true, 75, 60, gfx::ShadowQuality::Default, extras::AaMode::Off},       // integrated / entry graphics: fewer pixels, 60 fps
    {"desktop", -0.5, 16, true, 0, -1, gfx::ShadowQuality::VeryHigh, extras::AaMode::SmaaHigh},     // a dedicated GPU: the window's size (at least 1080p), the display's refresh rate
};

const Preset* find_preset(const std::string& n) {
    for (const Preset& p : kPresets)
        if (n == p.name) return &p;
    return nullptr;
}

struct State {
    uint64_t base = 0x7100000000ull;
    bool probe = false;
    float lod_scale = 1.0f;
    bool ini_lod_set = false;
    std::atomic<uint64_t> commits{0};
    std::atomic<bool> commit_seen{false};
    std::mutex log_mu;
    std::vector<std::string> logged;   // distinct probe lines already written (capped)
    bool installed = false;
    // [pkp] quality_preset from the ini or the environment (wins over the F1 page)
    const Preset* ini_preset = nullptr;
};

State& st() {
    static State s;
    return s;
}

using guest::rd;
using guest::wr;

void probe(uint64_t cam, float game_far) {
    char line[512];
    std::snprintf(line, sizeof line,
                  "near=%g far=%g fxaa=%d type=%d level=%d cascades=%g/%g/%g keylight_res=%d ssao_quality=%d ssao_res=%d render_ratio=%g "
                  "supersampling=%d ratio=%g",
                  static_cast<double>(rd<float>(cam + kNear)), static_cast<double>(game_far), rd<int32_t>(cam + kFxaaEnable),
                  rd<int32_t>(cam + kFxaaType), rd<int32_t>(cam + kFxaaLevel), static_cast<double>(rd<float>(cam + kCascade1)),
                  static_cast<double>(rd<float>(cam + kCascade2)), static_cast<double>(rd<float>(cam + kCascade3)), rd<int32_t>(cam + kKeyLightRes),
                  rd<int32_t>(cam + kSsaoQuality), rd<int32_t>(cam + kSsaoRes), static_cast<double>(rd<float>(cam + kRenderRatio)),
                  rd<uint8_t>(cam + kSsEnable), static_cast<double>(rd<float>(cam + kSsRatio)));
    State& s = st();
    std::lock_guard<std::mutex> lk(s.log_mu);
    if (s.logged.size() >= 24) return;
    const std::string l(line);
    if (std::find(s.logged.begin(), s.logged.end(), l) != s.logged.end()) return;
    s.logged.push_back(l);
    NXLOG_I(kTag, "quality: camera %llx commit: %s", static_cast<unsigned long long>(cam), line);
}

int hook_cam_commit(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const uint64_t cam = c->x[19];
    s.commits.fetch_add(1, std::memory_order_relaxed);
    if (!s.commit_seen.exchange(true)) NXLOG_I(kTag, "quality: first kids camera commit (camera %llx)", static_cast<unsigned long long>(cam));
    if (!cam || !mem::is_mapped(cam, kCamSize)) return 0;
    probe(cam, rd<float>(cam + kFar));
    return 0;
}

// Model LOD job builders 0x7102371280 / 0x71023719ac / 0x7102371d80 (x0 = the model's LOD data: LodStartDistanceArray floats at +0xe8,
// PartsLodDistance0/1 at +0x174/+0x178, EnablePartsLod +0x154, EnableLod +0x158). [pkp] lod_distance scales the distances (1 = the game's).
// Only the leading run of positive, increasing array entries (at most 8, ending before +0x108) counts as distances. The data persists, so
// the game's values are kept per object and rewritten only when memory still holds them or our last write. [pkp] quality_probe = 1 logs
// the first 24 objects.
constexpr uint64_t kLodArr = 0xe8, kLodD0 = 0x174, kLodD1 = 0x178, kLodPartsOn = 0x154, kLodOn = 0x158;
constexpr int kLodArrMax = 8;
int hook_parts_lod(::Ctx* c, uint64_t at_fn, void*) {
    struct Orig { float g[kLodArrMax + 2], w[kLodArrMax + 2]; int n; };
    static std::mutex mu;
    static std::vector<std::pair<uint64_t, Orig>> objs;
    static int logged = 0;
    // lod_distance 1 and no probe (the defaults; both are set once, before these hooks are added): nothing below writes or logs, so the
    // object need not be tracked (the scan of up to 8192 entries under the lock is skipped)
    if (st().lod_scale == 1.0f && !st().probe) return 0;
    const uint64_t o = c->x[0];
    if (!o || !mem::is_mapped(o, kLodD1 + 4)) return 0;
    State& s = st();
    const float k = s.lod_scale;
    float m[kLodArrMax + 2];
    for (int i = 0; i < kLodArrMax; ++i) m[i] = rd<float>(o + kLodArr + 4 * i);
    m[kLodArrMax] = rd<float>(o + kLodD0);
    m[kLodArrMax + 1] = rd<float>(o + kLodD1);
    std::lock_guard<std::mutex> lk(mu);
    Orig* e = nullptr;
    for (auto& p : objs) if (p.first == o) { e = &p.second; break; }
    bool fresh = false;
    if (!e) {
        if (objs.size() >= 8192) return 0;
        objs.push_back({o, {}});
        e = &objs.back().second;
        fresh = true;
    }
    bool changed = fresh;
    for (int i = 0; i < kLodArrMax + 2 && !changed; ++i) changed = m[i] != e->w[i];
    if (changed) {   // the game's values (first sight, or the game rewrote them)
        std::memcpy(e->g, m, sizeof m);
        int n = 0;
        float prev = 0.f;
        while (n < kLodArrMax && std::isfinite(m[n]) && m[n] >= 0.01f && m[n] > prev && m[n] < 1.0e6f) prev = m[n++];
        e->n = n;
    }
    std::memcpy(e->w, e->g, sizeof e->g);
    for (int i = 0; i < e->n; ++i) e->w[i] = e->g[i] * k;
    for (int i = kLodArrMax; i < kLodArrMax + 2; ++i) if (e->g[i] > 0.f && std::isfinite(e->g[i])) e->w[i] = e->g[i] * k;
    if (k != 1.0f) {
        for (int i = 0; i < e->n; ++i) wr<float>(o + kLodArr + 4 * i, e->w[i]);
        wr<float>(o + kLodD0, e->w[kLodArrMax]);
        wr<float>(o + kLodD1, e->w[kLodArrMax + 1]);
    }
    if (s.probe && changed && logged < 24) {
        ++logged;
        char arr[160] = {};
        int at = 0;
        for (int i = 0; i < kLodArrMax && at < 150; ++i) at += std::snprintf(arr + at, sizeof arr - at, "%s%g", i ? " " : "", static_cast<double>(e->g[i]));
        NXLOG_I(kTag, "quality probe: LOD data %llx (fn %llx) distances [%s] (%d scaled) parts %g / %g, EnableLod %d EnablePartsLod %d, x%.2f",
                static_cast<unsigned long long>(o), static_cast<unsigned long long>(at_fn), arr, e->n, static_cast<double>(e->g[kLodArrMax]),
                static_cast<double>(e->g[kLodArrMax + 1]), rd<int32_t>(o + kLodOn), rd<int32_t>(o + kLodPartsOn), static_cast<double>(k));
    }
    return 0;
}

// LOD selection candidates (work/audit/quality_params.md, "LOD selection"). Each site counts its calls (status line `lod=`); with
// [pkp] quality_probe = 1 it logs its first call and its first distinct values. [pkp] lod_distance k (> 1 = more detail) changes only
// the sites whose direction is known, and only in registers, so nothing persists in game memory:
//  inst  mid 0x7102350520  placement instance LOD (0x710234fc20): s0 = camera-to-instance distance, compared (>=) against the
//                          type's LOD1..9 switch distances (168-byte records, floats at +64 + 12*i; [sp+0xa8] = &LOD9). s0 / k.
//  cell  mid 0x71023501bc  same function, grid cell cull: s2 = camera-to-cell distance, w0 = the int limit. Probe only (draw distance).
//  errA  mid 0x71023b7c68  model screen-error LOD (0x71023b7b60, "ResolvedLodLevel is unenable."): s0 = pixel threshold from
//                          0x71022a0168 (camera +0x2398[group], default 2.0); coarsest level with error*scale/dist <= s0 wins. s0 / k.
//  errB  mid 0x71023b80a4  the 3-level parts variant (0x71023b8004), same threshold in s0. s0 / k.
//  sel   entry 0x71023be60c the model's LOD select (vtable slot +0x460, run by the LOD job 0x71023701c0). Probe only.
//  set   entry 0x71008eb6e0 SetLodLevel(model, ., s0 = level): every LOD change; logs the distinct callers (x30). Probe only.
//  obj   mid 0x71008f3a20  placed-object update 0x71008f3660 (the path that runs in the first area): s8 = squared camera-to-object
//                          distance, s11 = the object's squared LOD switch distance (+0x184 = (type byte * 100)^2, set in 0x71008ea460)
//                          times max(FOV zoom factor, 1); s8 > s11 sets flag 0x400 << view in +0x104, and 0x71008f19b8 then calls
//                          SetLodLevel(1) (else 0). s11 * k^2 (the compare is squared), x22 = the object, s0 = the FOV factor.
//  objb  mid 0x71008f3fa0  same function, batched objects: s0 = +0x184 handed to 0x71008f4c20, whose per-instance dist^2 <= s0 bits
//                          (+0x350) keep the batch's models at LOD 0 (0x71008f2ca0). s0 * k^2.
constexpr uint64_t kLodInst = 0x2350520, kLodCell = 0x23501bc, kLodErrA = 0x23b7c68, kLodErrB = 0x23b80a4, kLodSel = 0x23be60c,
                   kLodSet = 0x8eb6e0, kLodObj = 0x8f3a20, kLodObjB = 0x8f3fa0;
struct LodSite {
    const char* name;
    std::atomic<uint64_t> calls{0};
    std::mutex mu;
    std::vector<std::string> seen;
};
LodSite g_lod_sites[] = {{"inst"}, {"cell"}, {"errA"}, {"errB"}, {"sel"}, {"set"}, {"obj"}, {"objb"}};
enum { kSiteInst, kSiteCell, kSiteErrA, kSiteErrB, kSiteSel, kSiteSet, kSiteObj, kSiteObjB, kSiteCount };

// Counts the call; true when the caller should build a probe line (probe on and the site still logging).
bool lod_count(int i) {
    LodSite& s = g_lod_sites[i];
    const uint64_t n = s.calls.fetch_add(1, std::memory_order_relaxed);
    if (!st().probe) return false;
    if (n == 0) NXLOG_I(kTag, "quality probe: lod %s first call (lod_distance x%.2f)", s.name, static_cast<double>(st().lod_scale));
    std::lock_guard<std::mutex> lk(s.mu);
    return s.seen.size() < 8;
}
void lod_log(int i, const char* line) {
    LodSite& s = g_lod_sites[i];
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.seen.size() >= 8) return;
    const std::string l(line);
    if (std::find(s.seen.begin(), s.seen.end(), l) != s.seen.end()) return;
    s.seen.push_back(l);
    NXLOG_I(kTag, "quality probe: lod %s #%llu: %s", s.name, static_cast<unsigned long long>(s.calls.load(std::memory_order_relaxed)), line);
}
float r3(float v) {   // 3 significant digits, so the distinct-value lists stay short
    if (!std::isfinite(v) || v == 0.f) return v;
    const float p = std::pow(10.f, 2.f - std::floor(std::log10(std::fabs(v))));
    return std::round(v * p) / p;
}

int hook_lod_inst(::Ctx* c, uint64_t, void*) {
    const float d = c->v[0].f32[0], k = st().lod_scale;
    if (lod_count(kSiteInst)) {
        const uint64_t p9 = rd<uint64_t>(c->x[31] + 0xa8);
        char line[256];
        int at = std::snprintf(line, sizeof line, "switch distances (LOD1..9)");
        if (p9 >= 96 && mem::is_mapped(p9 - 96, 100))
            for (int i = 0; i < 9 && at < 240; ++i) at += std::snprintf(line + at, sizeof line - at, " %g", static_cast<double>(r3(rd<float>(p9 - 96 + 12 * i))));
        lod_log(kSiteInst, line);
    }
    if (k != 1.0f && std::isfinite(d)) c->v[0].f32[0] = d / k;
    return 0;
}

int hook_lod_cell(::Ctx* c, uint64_t, void*) {
    if (lod_count(kSiteCell)) {
        char line[96];
        std::snprintf(line, sizeof line, "cell limit %d", static_cast<int32_t>(c->x[0]));
        lod_log(kSiteCell, line);
    }
    return 0;
}

int lod_err(::Ctx* c, int site) {
    const float t = c->v[0].f32[0], k = st().lod_scale;
    if (lod_count(site)) {
        char line[96];
        std::snprintf(line, sizeof line, "pixel threshold %g", static_cast<double>(r3(t)));
        lod_log(site, line);
    }
    if (k != 1.0f && std::isfinite(t) && t > 0.f) c->v[0].f32[0] = t / k;
    return 0;
}
int hook_lod_err_a(::Ctx* c, uint64_t, void*) { return lod_err(c, kSiteErrA); }
int hook_lod_err_b(::Ctx* c, uint64_t, void*) { return lod_err(c, kSiteErrB); }

int hook_lod_sel(::Ctx* c, uint64_t, void*) {
    if (lod_count(kSiteSel)) {
        char line[96];
        std::snprintf(line, sizeof line, "model vtable %llx", static_cast<unsigned long long>(c->x[0] && mem::is_mapped(c->x[0], 8) ? rd<uint64_t>(c->x[0]) : 0));
        lod_log(kSiteSel, line);
    }
    return 0;
}

int hook_lod_set(::Ctx* c, uint64_t, void*) {
    if (lod_count(kSiteSet)) {
        char line[96];
        std::snprintf(line, sizeof line, "caller %llx", static_cast<unsigned long long>(c->x[30]));
        lod_log(kSiteSet, line);
    }
    return 0;
}

// Placed-object LOD switch (see "obj" / "objb" above). The thresholds are squared distances, so lod_distance k scales them by k^2.
int lod_obj(::Ctx* c, int site, int reg) {
    const float t = c->v[reg].f32[0], k = st().lod_scale;
    if (lod_count(site)) {
        char line[128];
        if (site == kSiteObj)
            std::snprintf(line, sizeof line, "switch distance %g (fov factor %g)", static_cast<double>(r3(std::sqrt(std::max(t, 0.f)))),
                          static_cast<double>(r3(c->v[0].f32[0])));
        else
            std::snprintf(line, sizeof line, "batch switch distance %g", static_cast<double>(r3(std::sqrt(std::max(t, 0.f)))));
        lod_log(site, line);
    }
    if (k != 1.0f && std::isfinite(t) && t > 0.f) c->v[reg].f32[0] = t * k * k;
    return 0;
}
int hook_lod_obj(::Ctx* c, uint64_t, void*) { return lod_obj(c, kSiteObj, 11); }
int hook_lod_obj_b(::Ctx* c, uint64_t, void*) { return lod_obj(c, kSiteObjB, 0); }

void apply() {
    State& s = st();
    if (!s.ini_preset) return;
    // after play_display's sampler tuning (play.cpp: display_tuning runs before the game's settings_changed)
    gfx::tuning().lod_bias.store(static_cast<float>(s.ini_preset->lod_bias));
    gfx::tuning().anisotropy.store(s.ini_preset->anisotropy);
    gfx::tuning().shadow_scale.store(gfx::shadow_scale_of(s.ini_preset->shadows));
    extras::Settings x = extras::settings();   // the live anti-aliasing: the settings file keeps what the page set, the preset in the ini wins
    x.upscale.aa = s.ini_preset->aa;
    extras::set_settings(x);
}

}  // namespace

bool quality_preset(const char* name, play::Settings& s) {
    const Preset* p = name ? find_preset(name) : nullptr;
    if (!p) return false;
    play::DisplaySettings& d = s.display;
    s.extras.upscale.aa = p->aa;
    d.lod_bias = p->lod_bias;
    d.anisotropy = p->anisotropy;
    d.shadow_quality = p->shadows;
    if (p->frame) {
        d.render_scale = p->render_scale;
        d.fps_cap = p->fps_cap;
    }
    d.quality_preset = p->name;
    return true;
}

namespace {

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.installed) return;
    const uint64_t base = in.base;
    const play::Settings& set = in.settings;
    s.base = base;
    if (settings::is_set(settings::Key::QualityPreset)) {
        const std::string n = settings::get_str(settings::Key::QualityPreset);
        s.ini_preset = find_preset(n);
        if (!s.ini_preset) NXLOG_W(kTag, "quality: [pkp] quality_preset '%s' is not switch2, high, ultra, steamdeck, legion_go_s, laptop or desktop: ignored", n.c_str());
    }
    apply();
    {
        const double fog = settings::get_double(settings::Key::FogStrength);
        gfx::Tuning& t = gfx::tuning();
        t.ufs_block.store(3);
        t.ufs_mask.store((1u << 3) | (1u << 7) | (1u << 11));   // c3[0].w, c3[1].w, c3[2].w
        t.ufs_scale.store(static_cast<float>(fog));
        t.ufs_hash.store(0x7c8c05da24a1f5a2ull);
        if (fog != 1.0) NXLOG_I(kTag, "quality: fog strength x%.2f", fog);
    }
    in.hooks.add_mid(base + kCamCommitEnd, &hook_cam_commit);
    s.probe = settings::get_bool(settings::Key::QualityProbe);
    s.ini_lod_set = settings::is_set(settings::Key::LodDistance);
    if (s.ini_lod_set) s.lod_scale = static_cast<float>(settings::get_double(settings::Key::LodDistance));
    for (uint64_t off : kLodJobs) in.hooks.add_entry(base + off, &hook_parts_lod);
    in.hooks.add_mid(base + kLodInst, &hook_lod_inst);
    in.hooks.add_mid(base + kLodCell, &hook_lod_cell);
    in.hooks.add_mid(base + kLodErrA, &hook_lod_err_a);
    in.hooks.add_mid(base + kLodErrB, &hook_lod_err_b);
    in.hooks.add_entry(base + kLodSel, &hook_lod_sel);
    in.hooks.add_entry(base + kLodSet, &hook_lod_set);
    in.hooks.add_mid(base + kLodObj, &hook_lod_obj);
    in.hooks.add_mid(base + kLodObjB, &hook_lod_obj_b);
    s.installed = true;
    NXLOG_I(kTag, "quality: texture LOD bias %.2f, anisotropy %d (preset %s%s)", static_cast<double>(gfx::tuning().lod_bias.load()), gfx::tuning().anisotropy.load(),
            s.ini_preset ? s.ini_preset->name : set.display.quality_preset.c_str(), s.ini_preset ? ", from [pkp] quality_preset" : "");
}

void remove() { st().installed = false; }   // the registry has removed the hooks

void settings_changed(const play::Settings&) { apply(); }

void status_text(char* buf, size_t n) {
    const State& s = st();
    const int k = std::snprintf(buf, n, " cam_commits=%llu", static_cast<unsigned long long>(s.commits.load()));
    if (k < 0 || static_cast<size_t>(k) + 1 >= n) return;
    auto cnt = [](int i) { return static_cast<unsigned long long>(g_lod_sites[i].calls.load(std::memory_order_relaxed)); };
    std::snprintf(buf + k, n - k, " lod=inst:%llu,cell:%llu,errA:%llu,errB:%llu,sel:%llu,set:%llu,obj:%llu,objb:%llu", cnt(kSiteInst), cnt(kSiteCell),
                  cnt(kSiteErrA), cnt(kSiteErrB), cnt(kSiteSel), cnt(kSiteSet), cnt(kSiteObj), cnt(kSiteObjB));
}

}  // namespace

const Feature kQualityFeature = {
    .name = "quality",
    .install = &install,
    .remove = &remove,
    .settings_changed = &settings_changed,
    .status_text = &status_text,
};

}  // namespace nxrt::games::pkp
