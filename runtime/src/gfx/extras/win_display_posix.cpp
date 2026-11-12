// SPDX-License-Identifier: GPL-3.0-or-later
// macOS display HDR query (win_display_posix.cpp), the counterpart of win_display.cpp. Same contract, same struct: the
// caller (src/gfx/vk_renderer.cpp) asks whether the monitor under the window has HDR on and what its luminance numbers
// are, and only logs the answer.
//
// Windows answers through the CCD database and DXGI_OUTPUT_DESC1. macOS has the same numbers through CoreGraphics:
// a display's EDR headroom is what the OS allows the layer to exceed SDR white by, and CGDisplayMode gives the pixel
// encoding. What is missing is the *user's* "SDR content brightness" slider, which Windows reports directly; on macOS
// that is not exposed, so sdr_white_nits stays at its default and the caller's tone mapping uses the headroom ratio.
#include "extras/win_display.h"

#if !defined(_WIN32)

#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace nxrt::extras {

bool query_display_hdr(void* hwnd, DisplayHdrInfo* out) {
    if (!out) return false;
    *out = DisplayHdrInfo{};
#if defined(__APPLE__)
    (void)hwnd;
    // The main display is the one a single-window game is on; a second monitor's headroom would need the NSScreen the
    // window is on, which SDL exposes only as the window's display index.
    CGDirectDisplayID display = CGMainDisplayID();
    out->valid = true;

    if (CGDisplayIsBuiltin(display)) {
        snprintf(out->monitor, sizeof out->monitor, "builtin");
    } else {
        snprintf(out->monitor, sizeof out->monitor, "display-%u", static_cast<unsigned>(display));
    }

    // A display whose colour space is extended-range is one the compositor lets exceed SDR white, which is what "HDR
    // on" means on macOS. Plain Display P3 is bounded (SDR wide gamut), so it does not read as extended.
    // CGDisplayModeCopyPixelEncoding, the obvious query, was deprecated and stops answering in 10.11; the EDR headroom
    // itself (NSScreen maximumPotentialExtendedDynamicRangeColorComponentValue) needs AppKit and the window's screen.
    CGColorSpaceRef space = CGDisplayCopyColorSpace(display);
    const bool wide = space && CGColorSpaceUsesExtendedRange(space);
    if (space) CGColorSpaceRelease(space);
    out->hdr_enabled = wide;
    if (wide) {
        // Extended range: the panel can exceed SDR white. The exact headroom is not exposed through CoreGraphics, so
        // max_nits stays 0 ("unknown") and hdr_resolve_levels falls back to its 1000-nit default. vk_renderer.cpp
        // logs these values and uses hdr_enabled for the Auto decision.
        snprintf(out->how, sizeof out->how, "CoreGraphics: extended-range colour space");
    } else {
        snprintf(out->how, sizeof out->how, "CoreGraphics: %s colour space", space ? "bounded" : "no");
    }
    out->min_nits = 0.0f;
    out->sdr_white_nits = 80.0f;
    return true;
#else
    (void)hwnd;
    return false;
#endif
}

}  // namespace nxrt::extras

#endif  // !_WIN32
