// SPDX-License-Identifier: GPL-3.0-or-later
// The screenshot provider registration (nxrt/screenshot.h).
#include "nxrt/screenshot.h"

#include <mutex>

namespace nxrt::screenshot {

namespace {
std::mutex g_mu;
ProviderFn g_fn = nullptr;
void* g_user = nullptr;
}  // namespace

void set_provider(ProviderFn fn, void* user) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_fn = fn;
    g_user = user;
}

bool request(const char* png_path, bool bare) {
    ProviderFn fn;
    void* user;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        fn = g_fn;
        user = g_user;
    }
    return png_path && fn && fn(png_path, bare, user);
}

}  // namespace nxrt::screenshot
