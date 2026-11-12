// SPDX-License-Identifier: GPL-3.0-or-later
// Short messages over the game's picture (nxrt/notices.h): the list the overlay draws from.
#include "nxrt/notices.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace nxrt::notices {

namespace {

struct Entry {
    Notice n;
    double seconds = 0;
    double until_s = -1;   // < 0: not shown yet, its time starts at the first frame that draws it
};

struct List {
    std::mutex mu;
    std::vector<Entry> items;
    std::atomic<bool> any{false};
};
List& list() {
    static List l;
    return l;
}

constexpr size_t kMaxShown = 6;   // more sources at once: the oldest go first

// The picture as Image keeps it: a side over kMaxImageSide is scaled down (aspect kept) by averaging each output pixel's source box.
std::shared_ptr<const Image> make_image(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return nullptr;
    auto img = std::make_shared<Image>();
    const double k = std::min(1.0, static_cast<double>(kMaxImageSide) / std::max(w, h));
    img->w = std::max(1, static_cast<int>(w * k));
    img->h = std::max(1, static_cast<int>(h * k));
    img->rgba.resize(static_cast<size_t>(img->w) * img->h * 4);
    if (img->w == w && img->h == h) {
        std::memcpy(img->rgba.data(), rgba, img->rgba.size());
        return img;
    }
    for (int y = 0; y < img->h; ++y) {
        const int y0 = y * h / img->h, y1 = std::max(y0 + 1, (y + 1) * h / img->h);
        for (int x = 0; x < img->w; ++x) {
            const int x0 = x * w / img->w, x1 = std::max(x0 + 1, (x + 1) * w / img->w);
            uint32_t sum[4] = {};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                    for (int c = 0; c < 4; ++c) sum[c] += rgba[(static_cast<size_t>(sy) * w + sx) * 4 + c];
            const uint32_t n = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
            for (int c = 0; c < 4; ++c) img->rgba[(static_cast<size_t>(y) * img->w + x) * 4 + c] = static_cast<uint8_t>(sum[c] / n);
        }
    }
    return img;
}

}  // namespace

void post(const std::string& source, const std::string& text, double seconds, const uint8_t* rgba, int w, int h) {
    std::shared_ptr<const Image> img = make_image(rgba, w, h);   // outside the lock: the overlay reads the list every frame
    List& l = list();
    std::lock_guard<std::mutex> lk(l.mu);
    std::erase_if(l.items, [&](const Entry& e) { return e.n.source == source; });
    if ((text.empty() && !img) || !(seconds > 0)) {
        l.any.store(!l.items.empty(), std::memory_order_relaxed);
        return;
    }
    l.items.push_back(Entry{Notice{source, text, std::move(img)}, std::min(seconds, 60.0), -1});
    if (l.items.size() > kMaxShown) l.items.erase(l.items.begin());
    l.any.store(true, std::memory_order_relaxed);
}

std::vector<Notice> shown(double now_s) {
    List& l = list();
    std::lock_guard<std::mutex> lk(l.mu);
    std::vector<Notice> out;
    for (Entry& e : l.items)
        if (e.until_s < 0) e.until_s = now_s + e.seconds;
    std::erase_if(l.items, [now_s](const Entry& e) { return now_s >= e.until_s; });
    for (const Entry& e : l.items) out.push_back(e.n);
    l.any.store(!l.items.empty(), std::memory_order_relaxed);
    return out;
}

bool any() { return list().any.load(std::memory_order_relaxed); }

}  // namespace nxrt::notices
