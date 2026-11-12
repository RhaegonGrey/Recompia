// SPDX-License-Identifier: GPL-3.0-or-later
// <windows.h> for the POSIX build: the runtime is written against Win32 (docs/macos-port.md, D6), and this header is
// what makes it compile unchanged. It is on the include path only for the non-Windows target (runtime/CMakeLists.txt),
// so the Windows build still gets the real SDK header.
//
// Everything it declares lives in src/os/nx_os.h; this file exists so that `#include <windows.h>` keeps working.
#pragma once

#include "nx_os.h"

// winnt.h pulls in <intrin.h> on x86/x64, which is how include/nxrt/ftrace.h gets __rdtsc without including it. Do the
// same, so a header that relied on that transitively keeps compiling.
#include "intrin.h"
