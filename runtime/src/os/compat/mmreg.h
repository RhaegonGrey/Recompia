// SPDX-License-Identifier: GPL-3.0-or-later
// <mmreg.h> for the POSIX build: the wave format the XAudio2 sink describes. That sink is Windows-only; the layout is
// repeated here because include/nxrt/audio_sink.h names it.
#pragma once
#include "nx_os.h"

typedef struct tWAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *LPWAVEFORMATEX;
typedef const WAVEFORMATEX* LPCWAVEFORMATEX;
#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_IEEE_FLOAT 3
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
