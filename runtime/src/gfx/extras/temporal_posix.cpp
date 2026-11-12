// SPDX-License-Identifier: GPL-3.0-or-later
// Linux and macOS: the two temporal upscalers are Windows SDKs. DLSS goes through Streamline's sl.*.dll and FSR 3 through
// the FidelityFX DLLs, so temporal_dlss.cpp and temporal_ffx.cpp are not in these builds (docs/macos-port.md M2, D2). This
// file is what the rest of the graphics shell sees instead: neither backend is available or starts, the settings page shows
// both as unavailable, and volk initialises the normal loader. FSR 1 and NIS are plain compute shaders and do run.
#include "temporal.h"

namespace nxrt::extras {

bool temporal_dlss_available() { return false; }
bool temporal_dlss_started() { return false; }
TemporalBackend* temporal_dlss(void*, bool) { return nullptr; }

bool temporal_ffx_available() { return false; }
TemporalBackend* temporal_ffx(void*) { return nullptr; }

PFN_vkGetInstanceProcAddr streamline_preinit() { return nullptr; }

}  // namespace nxrt::extras
