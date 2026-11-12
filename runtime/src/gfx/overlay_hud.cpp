// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay: the frame rate counter, the pointer arrow, the "Preparing shaders" note, the save-failure notice and the short
// messages of nxrt/notices.h.
#include "overlay_impl.h"

#include <cstring>

#include <imgui_internal.h>   // RegisterUserTexture, UnregisterUserTexture

namespace nxrt::gfx {
using namespace overlay_detail;

void Overlay::set_pointer(bool show, float x, float y, bool in_screenshots) {
    Impl& s = *impl_;
    s.ptr_x.store(x, std::memory_order_relaxed);
    s.ptr_y.store(y, std::memory_order_relaxed);
    s.ptr_in_shots.store(in_screenshots, std::memory_order_relaxed);
    s.ptr_show.store(show, std::memory_order_relaxed);
}

void Overlay::set_counter(const CounterConfig& c) {
    Impl& s = *impl_;
    s.counter_corner.store(std::clamp(c.corner, 0, 3), std::memory_order_relaxed);
    s.counter_detail.store(std::clamp(c.detail, 0, 2), std::memory_order_relaxed);
    s.counter_in_shots.store(c.in_screenshots, std::memory_order_relaxed);
    s.counter_show.store(c.show, std::memory_order_relaxed);   // last: the other settings are in place when it turns on
}

void Overlay::Impl::refresh_counter_text(double now_s) {
    if (now_s - text_s < 0.25) return;    // the text is rebuilt at most every 250 ms
    text_s = now_s;
    snap = stats.snapshot(now_s);
    const int detail = counter_detail.load(std::memory_order_relaxed);
    char buf[64];
    counter_n = 0;
    if (snap.valid) std::snprintf(buf, sizeof buf, "%.0f FPS", snap.fps);
    else std::snprintf(buf, sizeof buf, "-- FPS");
    counter_lines[counter_n++] = buf;
    if (detail >= 1) {
        if (snap.valid) std::snprintf(buf, sizeof buf, "%.1f ms", snap.avg_ms);
        else std::snprintf(buf, sizeof buf, "-- ms");
        counter_lines[counter_n++] = buf;
    }
    if (detail >= 2) {
        if (snap.valid) std::snprintf(buf, sizeof buf, "1%% low %.0f", snap.low1_fps);
        else std::snprintf(buf, sizeof buf, "1%% low --");
        counter_lines[counter_n++] = buf;
        if (snap.valid) std::snprintf(buf, sizeof buf, "max %.1f ms", snap.max_ms);
        else std::snprintf(buf, sizeof buf, "max -- ms");
        counter_lines[counter_n++] = buf;
    }
}

// Draws the counter with the foreground draw list: no window, so it never takes mouse or keyboard input.
void Overlay::Impl::draw_counter(const FrameInfo& f, double now_s) {
    refresh_counter_text(now_s);
    ImFont* font = ImGui::GetFont();
    const float size = std::clamp(std::round(static_cast<float>(f.extent.height) / 45.f), 14.f, 40.f);
    const float pad = size * 0.4f;
    const float gap = size * 0.15f;
    const float margin = size * 0.8f;
    const int n = counter_n;
    float lines_w[4] = {};
    float line_h = 0, text_w = 0;
    for (int i = 0; i < n; ++i) {
        const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, counter_lines[i].c_str());
        lines_w[i] = sz.x;
        line_h = std::max(line_h, sz.y);
        text_w = std::max(text_w, sz.x);
    }
    const float box_w = text_w + 2.f * pad;
    const float box_h = static_cast<float>(n) * line_h + static_cast<float>(n - 1) * gap + 2.f * pad;
    const int corner = counter_corner.load(std::memory_order_relaxed);
    const bool right = corner == 1 || corner == 3;
    const bool bottom = corner >= 2;
    const float W = static_cast<float>(f.extent.width), H = static_cast<float>(f.extent.height);
    const float x0 = right ? W - margin - box_w : margin;
    const float y0 = bottom ? H - margin - box_h : margin;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + box_w, y0 + box_h), IM_COL32(0, 0, 0, 140), size * 0.3f);
    for (int i = 0; i < n; ++i) {
        const float x = right ? x0 + box_w - pad - lines_w[i] : x0 + pad;
        const float y = y0 + pad + static_cast<float>(i) * (line_h + gap);
        dl->AddText(font, size, ImVec2(x, y), IM_COL32(255, 255, 255, 255), counter_lines[i].c_str());
    }
}

bool Overlay::Impl::note_visible(double now_s) {
    if (!note_read) {
        note_read = true;
        note_enabled = nxrt::config_get_bool("nvn", "compile_note", true);
    }
    const int pending = shader_backlog();
    if (!note_enabled) return false;
    if (pending > 0) {
        if (note_since_s < 0) { note_since_s = now_s; note_peak = 0; }
        note_empty_s = -1.0;
        note_peak = std::max(note_peak, pending);
        return now_s - note_since_s >= 1.5;
    }
    if (note_since_s < 0) return false;
    if (note_empty_s < 0) note_empty_s = now_s;
    if (now_s - note_empty_s >= 1.0) { note_since_s = -1.0; return false; }
    return now_s - note_since_s >= 1.5;
}

void Overlay::Impl::draw_note(const FrameInfo& f) {
    char text[96];
    const int pending = shader_backlog();
    if (pending > 0) std::snprintf(text, sizeof text, "Preparing shaders (%d left)", pending);
    else std::snprintf(text, sizeof text, "Preparing shaders (done)");
    ImFont* font = ImGui::GetFont();
    const float size = std::clamp(std::round(static_cast<float>(f.extent.height) / 60.f), 12.f, 28.f);
    const float pad = size * 0.4f;
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float W = static_cast<float>(f.extent.width), H = static_cast<float>(f.extent.height);
    const float x0 = (W - sz.x) * 0.5f - pad, y0 = H - size * 0.8f - sz.y - 2.f * pad;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + sz.x + 2.f * pad, y0 + sz.y + 2.f * pad), IM_COL32(0, 0, 0, 120), size * 0.3f);
    dl->AddText(font, size, ImVec2(x0 + pad, y0 + pad), IM_COL32(255, 255, 255, 220), text);
}

bool Overlay::Impl::save_notice_visible(double now_s) {
    const uint32_t n = services::fsapi::save_failures().count;
    if (n != save_seen) {
        save_seen = n;
        save_until_s = now_s + 10.0;
    }
    return now_s < save_until_s;
}

void Overlay::Impl::draw_save_notice(const FrameInfo& f) {
    char text[160];
    std::snprintf(text, sizeof text, "Save failed: %s. The last good save is kept.", services::fsapi::save_failure_reason(services::fsapi::save_failures().host_error));
    ImFont* font = ImGui::GetFont();
    const float size = std::clamp(std::round(static_cast<float>(f.extent.height) / 45.f), 14.f, 36.f);
    const float pad = size * 0.5f;
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float W = static_cast<float>(f.extent.width);
    const float x0 = (W - sz.x) * 0.5f - pad, y0 = size;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + sz.x + 2.f * pad, y0 + sz.y + 2.f * pad), IM_COL32(120, 0, 0, 200), size * 0.3f);
    dl->AddText(font, size, ImVec2(x0 + pad, y0 + pad), IM_COL32(255, 255, 255, 255), text);
}

void Overlay::Impl::draw_notices(const FrameInfo& f, const std::vector<notices::Notice>& list) {
    if (list.empty()) return;
    ImFont* font = ImGui::GetFont();
    const float size = std::clamp(std::round(static_cast<float>(f.extent.height) / 50.f), 13.f, 32.f);
    const float pad = size * 0.45f, gap = size * 0.3f;
    const float pic_h = size * 3.f;  // a message's picture: about three text lines high, its own aspect
    const float W = static_cast<float>(f.extent.width), H = static_cast<float>(f.extent.height);
    const float wrap = W * 0.45f;   // long messages wrap at a little under half the picture's width
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float y = H - size * 1.5f;      // from the bottom up: the newest lowest
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        ImTextureData* tex = it->image ? notice_texture(it->image) : nullptr;
        const float pic_w = tex ? pic_h * static_cast<float>(it->image->w) / static_cast<float>(it->image->h) : 0.f;
        const float lead = tex ? pic_w + (it->text.empty() ? 0.f : pad) : 0.f;   // the text starts right of the picture
        const ImVec2 sz = it->text.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(size, FLT_MAX, wrap, it->text.c_str());
        const float body_h = std::max(sz.y, tex ? pic_h : 0.f);
        const float x0 = size, y0 = y - body_h - 2.f * pad;
        if (y0 < 0) break;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + lead + sz.x + 2.f * pad, y), IM_COL32(0, 0, 0, 170), size * 0.3f);
        if (tex) {
            const float py = y0 + pad + (body_h - pic_h) * 0.5f;
            dl->AddImage(tex->GetTexRef(), ImVec2(x0 + pad, py), ImVec2(x0 + pad + pic_w, py + pic_h));
        }
        if (!it->text.empty())
            dl->AddText(font, size, ImVec2(x0 + pad + lead, y0 + pad + (body_h - sz.y) * 0.5f), IM_COL32(255, 255, 255, 240), it->text.c_str(), nullptr, wrap);
        y = y0 - gap;
    }
}

ImTextureData* Overlay::Impl::notice_texture(const std::shared_ptr<const notices::Image>& img) {
    for (const NoticeTex& n : notice_tex)
        if (n.img == img) return n.tex;
    if (img->w <= 0 || img->h <= 0 || img->rgba.size() != static_cast<size_t>(img->w) * img->h * 4) return nullptr;
    auto* tex = IM_NEW(ImTextureData)();
    tex->Create(ImTextureFormat_RGBA32, img->w, img->h);
    std::memcpy(tex->GetPixels(), img->rgba.data(), img->rgba.size());
    ImGui::RegisterUserTexture(tex);
    notice_tex.push_back(NoticeTex{img, tex});
    return tex;
}

void Overlay::Impl::retire_notice_textures(const std::vector<notices::Notice>* live) {
    std::erase_if(notice_tex, [&](NoticeTex& n) {
        if (n.tex->WantDestroyNextFrame) {
            if (n.tex->Status != ImTextureStatus_Destroyed) return false;   // the backend still holds it (frames in flight)
            ImGui::UnregisterUserTexture(n.tex);
            IM_DELETE(n.tex);
            return true;
        }
        if (live && std::none_of(live->begin(), live->end(), [&](const notices::Notice& m) { return m.image == n.img; }))
            n.tex->WantDestroyNextFrame = true;   // its message is gone: not drawn again from here on
        return false;
    });
}

void Overlay::Impl::free_notice_textures() {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(imgui);
    for (NoticeTex& n : notice_tex) {
        ImGui::UnregisterUserTexture(n.tex);
        IM_DELETE(n.tex);
    }
    notice_tex.clear();
    ImGui::SetCurrentContext(prev);
}


}  // namespace nxrt::gfx
