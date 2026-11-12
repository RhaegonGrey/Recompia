// SPDX-License-Identifier: GPL-3.0-or-later
// <shellapi.h> for the POSIX build: the shell is absent. src/host_device.cpp opens the store page for a Steam Deck
// helper and probes the "SteamDeck" environment variable; neither path exists on macOS.
#pragma once
#include "nx_os.h"

#define SW_SHOWNORMAL 1
#define SEE_MASK_NOCLOSEPROCESS 0x00000040

typedef struct _SHELLEXECUTEINFOA {
    DWORD cbSize;
    DWORD fMask;
    HWND hwnd;
    LPCSTR lpVerb;
    LPCSTR lpFile;
    LPCSTR lpParameters;
    LPCSTR lpDirectory;
    int nShow;
    HINSTANCE hInstApp;
    void* lpIDList;
    LPCSTR lpClass;
    HKEY hkeyClass;
    DWORD dwHotKey;
    HANDLE hIcon;
    HANDLE hProcess;
} SHELLEXECUTEINFOA, *LPSHELLEXECUTEINFOA;
typedef SHELLEXECUTEINFOA SHELLEXECUTEINFO;
typedef SHELLEXECUTEINFO* LPSHELLEXECUTEINFO;

#if defined(__cplusplus)
extern "C" {
#endif
BOOL ShellExecuteExA(SHELLEXECUTEINFOA* info);
HINSTANCE ShellExecuteA(HWND hwnd, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, int show);
#if defined(__cplusplus)
}
#endif
