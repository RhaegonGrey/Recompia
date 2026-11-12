// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_misc.cpp: the two SDK entry points that are not part of the Win32 kernel surface but are used by the
// runtime -- dbghelp's UnDecorateSymbolName and bcrypt's BCryptGenRandom.
#include "dbghelp.h"
#include "bcrypt.h"

#if !defined(_WIN32)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <cstring>

#if !defined(__APPLE__)
#include <sys/random.h>
#endif

#include <algorithm>
#include <string>

#if defined(__has_include)
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#endif
#endif

#if defined(__APPLE__)
#include <stdlib.h>   // arc4random_buf
#endif

namespace {

// Demangles one Itanium ABI name. Returns an empty string when the input is not mangled.
std::string demangle(const char* name) {
    if (!name || name[0] != '_' || name[1] != 'Z') return {};
#if defined(__has_include)
#if __has_include(<cxxabi.h>)
    int status = 0;
    char* out = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status == 0 && out) {
        std::string s(out);
        ::free(out);
        return s;
    }
    ::free(out);
#endif
#endif
    return {};
}

}  // namespace

extern "C" {

DWORD UnDecorateSymbolName(LPCSTR name, LPSTR out, DWORD cap, DWORD flags) {
    (void)flags;
    if (!name || !out || cap == 0) return 0;
    std::string s = demangle(name);
    if (s.empty()) s = name;   // not mangled: Windows' UnDecorateSymbolName also returns the input unchanged
    const size_t n = std::min<size_t>(s.size(), cap - 1);
    memcpy(out, s.data(), n);
    out[n] = 0;
    return static_cast<DWORD>(n);
}

NTSTATUS BCryptGenRandom(void* alg, PUCHAR buf, ULONG len, ULONG flags) {
    (void)alg;
    (void)flags;   // BCRYPT_USE_SYSTEM_PREFERRED_RNG is the only flag the runtime passes
    if (!buf || len == 0) return 0;
#if defined(__APPLE__)
    ::arc4random_buf(buf, len);
#else
    if (::getentropy(buf, len) != 0) return -1;   // STATUS_UNSUCCESSFUL: the caller reports no randomness
#endif
    return 0;
}

}  // extern "C"

#endif  // !_WIN32
