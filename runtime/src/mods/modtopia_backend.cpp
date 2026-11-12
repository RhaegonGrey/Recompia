// SPDX-License-Identifier: GPL-3.0-or-later
// The mods host's MtBackend (Modtopia docs/HOST.md section 2): what the Modtopia core asks of the running game. Guest memory, the registers
// of a hook stop, guest calls, module bases, which hook points the game build has, the game's own RomFS files and a note on those that hold
// shaders (file_note), the save folder, messages over the picture (notify, notify_image), screenshots without our menus, the players' pads
// (pad_read, pad_hold, pad_block: modtopia_input.cpp), sound under the game's (audio_*), which keyboard keys are held (key_down), a colour
// matrix over the final picture (color_matrix), the game's 2D shown or hidden (ui_visible) and HTTP(S) requests (net_request). ui_visible
// stays NULL when the game's rendering cannot hide its 2D (ui_layer::supported): the core then tells a mod that asks for it that this host
// has none (MT_ERR_VERSION).
#include <windows.h>

#include <cstring>
#include <string>

#include "../net/http_client.h"
#include "mods_internal.h"
#include "nxrt/audio_sink.h"
#include "nxrt/color_matrix.h"
#include "nxrt/dispatch.h"
#include "nxrt/fault_guard.h"
#include "nxrt/game.h"
#include "nxrt/kernel.h"
#include "nxrt/key_state.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/notices.h"
#include "nxrt/screenshot.h"
#include "nxrt/services_api.h"
#include "nxrt/text.h"
#include "nxrt/ui_layer.h"

namespace nxrt::mods::detail {

namespace {

constexpr const char* kTag = "mods";

// Register numbers of MtBackend::reg_get / reg_set (modtopia_host.h).
constexpr int kRegSp = 31, kRegPc = 32, kRegNzcv = 33, kRegFpcr = 34, kRegFpsr = 35;

thread_local uint64_t t_skip_site = 0;   // call_original: the entry stop at this site is skipped once

// Every byte of [addr, addr + n) is the guest's own memory (its memory map, svcQueryMemory's view) and readable, or also writable.
// Guest addresses are host addresses: this is what keeps a mod's guest address from reaching the host's own memory.
bool guest_range(uint64_t addr, size_t n, bool write) {
    if (n == 0) return true;
    const uint64_t end = addr + n;
    if (end < addr) return false;
    for (uint64_t a = addr; a < end;) {
        const mem::VmInfo v = mem::vm_query(a);
        if (v.state == mem::kMemFree || v.state == mem::kMemInaccessible || !(v.perm & (write ? 2u : 1u)) || v.addr + v.size <= a) return false;
        a = v.addr + v.size;
    }
    return true;
}

// The copy itself, guarded: a page the map calls mapped may still fault (a race with an unmap). The write tracker's vectored handler serves
// its guarded pages before this handler is asked (nxrt/write_watch.h).
bool guarded_copy(void* dst, const void* src, size_t n) {
    return nxrt::fault_guarded([&] { std::memcpy(dst, src, n); }, nxrt::FaultCatch::Any);
}

void be_log(void*, int level, const char* line) {
    switch (level) {
        case MT_LOG_DEBUG: NXLOG_D(kTag, "%s", line); break;
        case MT_LOG_INFO: NXLOG_I(kTag, "%s", line); break;
        case MT_LOG_WARN: NXLOG_W(kTag, "%s", line); break;
        default: NXLOG_E(kTag, "%s", line); break;
    }
}

int be_mem_read(void*, uint64_t addr, void* out, size_t n) {
    if (!guest_range(addr, n, false)) return MT_ERR_FAULT;
    return guarded_copy(out, reinterpret_cast<const void*>(addr), n) ? MT_OK : MT_ERR_FAULT;
}
int be_mem_write(void*, uint64_t addr, const void* in, size_t n) {
    if (!guest_range(addr, n, true)) return MT_ERR_FAULT;
    return guarded_copy(reinterpret_cast<void*>(addr), in, n) ? MT_OK : MT_ERR_FAULT;
}

uint64_t be_module_base(void*, const char* name) {
    const Module* m = name ? module_by_name(name) : nullptr;
    return m ? m->base() : 0;
}

int be_reg_get(void*, void* host_ctx, int reg, uint64_t* out) {
    const Stop* s = static_cast<const Stop*>(host_ctx);
    if (!s || !out) return MT_ERR_UNKNOWN;
    const ::Ctx* c = s->c;
    if (reg >= 0 && reg <= kRegSp) *out = c->x[reg];
    else if (reg == kRegPc) *out = s->site;
    else if (reg == kRegNzcv) *out = (uint64_t{c->N & 1} << 31) | (uint64_t{c->Z & 1} << 30) | (uint64_t{c->C & 1} << 29) | (uint64_t{c->V & 1} << 28);
    else if (reg == kRegFpcr) *out = c->fpcr;
    else if (reg == kRegFpsr) *out = c->fpsr;
    else return MT_ERR_UNKNOWN;
    return MT_OK;
}
// NZCV is not writable: the lifted code keeps the flags in a local across a site callout and does not read them back from the Ctx there
// (recomp/nxrecomp/locals.py, 'lean': nx_site_* are FLAT_RUNTIME), so a write could not reach the game's code. pc is read only (HOST.md).
int be_reg_set(void*, void* host_ctx, int reg, uint64_t value) {
    Stop* s = static_cast<Stop*>(host_ctx);
    if (!s) return MT_ERR_UNKNOWN;
    ::Ctx* c = s->c;
    if (reg >= 0 && reg <= kRegSp) c->x[reg] = value;
    else if (reg == kRegFpcr) c->fpcr = value;
    else if (reg == kRegFpsr) c->fpsr = value;
    else return MT_ERR_UNKNOWN;
    return MT_OK;
}
int be_vreg_get(void*, void* host_ctx, int vreg, void* out16) {
    const Stop* s = static_cast<const Stop*>(host_ctx);
    if (!s || !out16 || vreg < 0 || vreg > 31) return MT_ERR_UNKNOWN;
    std::memcpy(out16, s->c->v[vreg].b, 16);
    return MT_OK;
}
int be_vreg_set(void*, void* host_ctx, int vreg, const void* in16) {
    Stop* s = static_cast<Stop*>(host_ctx);
    if (!s || !in16 || vreg < 0 || vreg > 31) return MT_ERR_UNKNOWN;
    std::memcpy(s->c->v[vreg].b, in16, 16);
    return MT_OK;
}

// A game function on the calling game thread, on that thread's own registers (the stop's, or the thread's when an event calls): x0..x7 and
// v0..v7 from regs, everything else as the caller has it, and the whole register file put back afterwards (as the game layer's guest calls
// do), so the stop's registers come back unchanged. An address that is neither a lifted function nor a host function is refused instead
// of trapping.
int be_call_guest(void*, void* host_ctx, uint64_t addr, MtCall* regs) {
    ::Ctx* own = kernel::current_ctx();
    if (!own || !regs) return MT_ERR_STATE;
    ::Ctx* c = host_ctx ? static_cast<Stop*>(host_ctx)->c : own;
    if (!fn_lookup(addr) && !hle_is_thunk(addr)) return MT_ERR_UNKNOWN;
    const ::Ctx saved = *c;
    for (int i = 0; i < 8; ++i) {
        c->x[i] = regs->x[i];
        std::memcpy(c->v[i].b, regs->v[i], 16);
    }
    dispatch_call(c, addr);
    for (int i = 0; i < 8; ++i) {
        regs->x[i] = c->x[i];
        std::memcpy(regs->v[i], c->v[i].b, 16);
    }
    *c = saved;
    return MT_OK;
}

// replace points: the game's function at site on the stop's registers, its entry stop skipped this once (take_skip in the entry hook);
// its results stay in the stop's registers.
int be_call_original(void*, void* host_ctx, uint64_t site) {
    Stop* s = static_cast<Stop*>(host_ctx);
    if (!s || !kernel::current_ctx() || !fn_lookup(site)) return MT_ERR_STATE;
    t_skip_site = site;
    dispatch_call(s->c, site);
    t_skip_site = 0;
    return MT_OK;
}

int be_hook_built_in(void*, uint64_t addr, int kind) {
    const auto it = host().built.find(addr);
    return it != host().built.end() && it->second.plan_kind >= kind ? 1 : 0;
}

int be_game_has_file(void*, const char* path) { return path && services::fsapi::romfs_game_file_size(path) >= 0 ? 1 : 0; }
// A data mod's file over one of the game's shader files (game::ModHost::shader_files): the shader cache has no translation for the shaders
// it changes; the launcher translates them after the game closes ([nvn] dump_missing). The core puts the file's path before the note.
const char* be_file_note(void*, const char* path) {
    const game::ModHost* mh = game::info().mods;
    if (!path || !mh) return nullptr;
    const std::string p = text::lower(path);
    for (uint32_t i = 0; i < mh->shader_file_count; ++i)
        if (p == text::lower(mh->shader_files[i]))
            return "holds shaders: the ones this mod changes are translated after the game closes, so its first run may miss some graphics";
    return nullptr;
}
int64_t be_game_file_size(void*, const char* path) { return path ? services::fsapi::romfs_game_file_size(path) : -1; }
int64_t be_game_read(void*, const char* path, uint64_t offset, void* buf, uint64_t n) {
    return path && buf ? services::fsapi::romfs_game_read(path, offset, buf, n) : -1;
}

const char* be_save_dir(void*) { return host().save_dir.c_str(); }

int be_pad_read(void*, int p, MtPad* out) { return pad_read(p, out); }
int be_pad_hold(void*, int p, const MtPad* add) { return pad_hold(p, add); }
int be_pad_block(void*, int p, const MtPad* mask) { return pad_block(p, mask); }

// A mod's sound: the core gives each mod that plays a voice (0..MT_AUDIO_VOICES - 1), mixed under the game's output (nxrt/audio_sink.h).
static_assert(MT_AUDIO_VOICES <= audio::kExtraVoices, "a voice for every voice the core gives out");
int be_audio_write(void*, int voice, const int16_t* stereo, int frames) {
    return frames > 0 ? static_cast<int>(audio::extra_voice_write(voice, stereo, static_cast<uint32_t>(frames))) : 0;
}
int be_audio_queued(void*, int voice) { return static_cast<int>(audio::extra_voice_queued(voice)); }
int be_audio_clear(void*, int voice) {
    audio::extra_voice_clear(voice);
    return MT_OK;
}

// A mod's message over the picture (nxrt/notices.h); its newer one replaces its earlier one.
int be_notify(void*, const char* mod_id, const char* text, double seconds) {
    if (!mod_id || !text) return MT_ERR_UNKNOWN;
    notices::post(std::string("mod:") + mod_id, text, seconds);
    return MT_OK;
}
// The same with a picture beside the text: the core has already read the mod's PNG and fitted it to MT_NOTIFY_IMAGE x MT_NOTIFY_IMAGE
// straight RGBA (modtopia_host.h), valid during the call only, so the notice keeps its own copy.
static_assert(MT_NOTIFY_IMAGE <= notices::kMaxImageSide, "the core's picture is shown at its own size");
int be_notify_image(void*, const char* mod_id, const char* text, double seconds, const uint8_t* rgba) {
    if (!mod_id || !text) return MT_ERR_UNKNOWN;
    notices::post(std::string("mod:") + mod_id, text, seconds, rgba, MT_NOTIFY_IMAGE, MT_NOTIFY_IMAGE);
    return MT_OK;
}

// The next presented picture as a PNG at path, without our menus. The request is taken now and the file is written when that frame ends
// (nxrt/screenshot.h), so MT_OK means requested, as the host API says. No renderer (a headless run), or another capture not taken yet
// (one at a time; the mod may ask again next frame): MT_ERR_STATE.
int be_screenshot(void*, const char* path) {
    if (!path || !*path) return MT_ERR_UNKNOWN;
    return screenshot::request(path, true) ? MT_OK : MT_ERR_STATE;
}

// 1 while the keyboard key is held, by the name key settings store. The state only holds keys that reached the game's window while it had
// focus (cleared on focus loss), so an unfocused window or an unknown name reads 0.
int be_key_down(void*, const char* key) { return key && key_state::down_by_name(key) ? 1 : 0; }

// The final picture's colour matrix: the core passes the last loaded mod's (null when no mod has one); the host keeps the latest and the
// output pass applies it from the next presented frame (nxrt/color_matrix.h).
int be_color_matrix(void*, const float* m12) {
    color_matrix::set(m12);
    return MT_OK;
}

// The game's 2D (HUD, markers, menus, text) over its picture: the core passes 0 while any mod hides it, 1 when none does
// (modtopia_host.h). The replay leaves it out from the next draw on (nxrt/ui_layer.h). Offered only when the game supports it
// (make_backend), so elsewhere a mod that asks hears MT_ERR_VERSION as before.
int be_ui_visible(void*, int visible) {
    ui_layer::set_visible(visible != 0);
    return MT_OK;
}

// One HTTP(S) request off the calling thread (src/net/http_client.h: https only but for localhost, no redirects, no cookies or sign-in). The
// core has checked the URL against the mod's hosts and keeps at most 4 of a mod's waiting; the client caps the whole process. The sizes are
// modtopia.h's: a body of at most 64 KiB, an answer of at most 256 KiB (longer: no answer, status 0). done runs once, on the request's worker.
constexpr size_t kNetBodyMax = size_t{64} << 10, kNetAnswerMax = size_t{256} << 10;
int be_net_request(void*, const char* method, const char* url, const char* body, size_t n, void (*done)(void*, int, const char*, size_t),
                   void* token) {
    if (!method || !url || !done) return MT_ERR_UNKNOWN;
    if (n > kNetBodyMax || (n && !body)) return MT_ERR;
    http::Request r;
    r.method = method;
    r.url = url;
    if (n) r.body.assign(body, n);
    r.user_agent = host().host_name;
    r.max_answer = kNetAnswerMax;
    const bool started = http::start(std::move(r), [done, token](int status, std::string answer) {
        done(token, status, answer.data(), answer.size());
    });
    return started ? MT_OK : MT_ERR_STATE;
}

}  // namespace

bool take_skip(uint64_t site) {
    if (t_skip_site != site) return false;
    t_skip_site = 0;
    return true;
}

MtBackend make_backend() {
    Host& h = host();
    MtBackend be{};
    be.size = sizeof be;
    be.host_name = h.host_name.c_str();
    be.game_version = h.game_version.c_str();
    be.game_build_id = h.build_id.empty() ? nullptr : h.build_id.c_str();
    be.platform = nullptr;   // mt_platform(): this build's own tag
    be.log = &be_log;
    be.mem_read = &be_mem_read;
    be.mem_write = &be_mem_write;
    be.module_base = &be_module_base;
    be.reg_get = &be_reg_get;
    be.reg_set = &be_reg_set;
    be.vreg_get = &be_vreg_get;
    be.vreg_set = &be_vreg_set;
    be.call_guest = &be_call_guest;
    be.call_original = &be_call_original;
    be.hook_built_in = &be_hook_built_in;
    be.game_has_file = &be_game_has_file;
    be.file_note = &be_file_note;
    be.game_file_size = &be_game_file_size;
    be.game_read = &be_game_read;
    be.save_dir = &be_save_dir;
    be.notify = &be_notify;
    be.notify_image = &be_notify_image;
    be.screenshot = &be_screenshot;
    be.key_down = &be_key_down;
    be.color_matrix = &be_color_matrix;
    if (ui_layer::supported()) be.ui_visible = &be_ui_visible;
    be.net_request = &be_net_request;
    be.pad_read = &be_pad_read;
    be.pad_hold = &be_pad_hold;
    be.pad_block = &be_pad_block;
    be.audio_write = &be_audio_write;
    be.audio_queued = &be_audio_queued;
    be.audio_clear = &be_audio_clear;
    return be;
}

}  // namespace nxrt::mods::detail
