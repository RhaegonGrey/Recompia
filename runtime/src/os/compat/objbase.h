// SPDX-License-Identifier: GPL-3.0-or-later
// <objbase.h> for the POSIX build: COM is absent. The callers (album.cpp, audio_xaudio2.cpp, movie_mf.cpp) are
// Windows-only and are not built for macOS; CoInitializeEx/CoCreateInstance here only keep the shared headers compiling.
#pragma once
#include "nx_os.h"
