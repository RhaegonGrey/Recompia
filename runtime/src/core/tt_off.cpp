// SPDX-License-Identifier: GPL-3.0-or-later
// The test tooling's entry points when it is not built (NXRT_TESTTOOLS=OFF, release packages): everything is off and asks nothing of the
// config. Only the functions the rest of the runtime calls are here; src/testtools has the real ones.
#include "nxrt/tt.h"

namespace nxrt::tt {

bool init() { return false; }
void shutdown() {}
bool enabled() { return false; }
bool finished() { return false; }
void end_run(int, const char*, const std::string&) {}
void set_screenshot_flush(void (*)()) {}
void set_mouse_sink(MouseSinkFn) {}
void set_key_sink(KeySinkFn) {}

}  // namespace nxrt::tt
