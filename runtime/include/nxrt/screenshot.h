// SPDX-License-Identifier: GPL-3.0-or-later
// The one way to ask for a screenshot of the next presented picture, for the code that has no renderer to call: the test tooling (@shot) and
// the mods host. The graphics layer (play mode) registers the provider that owns the renderer; with none registered a request fails.
// "bare" leaves the runtime's own menus out of that one capture (the F1 menu, the on-screen keyboard, the quit prompt, the touch button and
// the notices); a request that is not bare keeps whatever the overlay draws for a screenshot today. Any thread.
#pragma once

namespace nxrt::screenshot {

// Writes the next presented frame as a PNG at png_path (absolute; its folder exists) and returns once it is requested, not once the file
// exists. false: the capture could not be requested.
using ProviderFn = bool (*)(const char* png_path, bool bare, void* user);
void set_provider(ProviderFn fn, void* user);   // fn null: take it out

bool request(const char* png_path, bool bare);   // false: no provider, or it failed

}  // namespace nxrt::screenshot
