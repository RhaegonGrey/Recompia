// SPDX-License-Identifier: GPL-3.0-or-later
// <dbghelp.h> for the POSIX build: UnDecorateSymbolName.
//
// Windows symbol files hold MSVC-decorated names; the port has no .map to read (diag/symbols.cpp falls back to the
// embedded table), but the undecoration entry point is kept so that file compiles, and it demangles Itanium ABI
// names through __cxa_demangle -- which is what a macOS binary's symbols actually are.
#pragma once
#include "nx_os.h"

#define UNDNAME_COMPLETE 0x0000
#define UNDNAME_NO_LEADING_UNDERSCORES 0x0001
#define UNDNAME_NO_MS_KEYWORDS 0x0002
#define UNDNAME_NO_FUNCTION_RETURNS 0x0004
#define UNDNAME_NO_ALLOCATION_MODEL 0x0008
#define UNDNAME_NO_ALLOCATION_LANGUAGE 0x0010
#define UNDNAME_NO_MS_THISTYPE 0x0020
#define UNDNAME_NO_CV_THISTYPE 0x0040
#define UNDNAME_NO_THISTYPE 0x0060
#define UNDNAME_NO_ACCESS_SPECIFIERS 0x0080
#define UNDNAME_NO_THROW_SIGNATURES 0x0100
#define UNDNAME_NO_MEMBER_TYPE 0x0200
#define UNDNAME_NO_RETURN_UDT_MODEL 0x0400

#if defined(__cplusplus)
extern "C" {
#endif
DWORD UnDecorateSymbolName(LPCSTR name, LPSTR out, DWORD cap, DWORD flags);
#if defined(__cplusplus)
}
#endif
