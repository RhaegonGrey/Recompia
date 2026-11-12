// SPDX-License-Identifier: GPL-3.0-or-later
// <mmsystem.h> for the POSIX build: the timer-resolution pair the audio thread boost raises
// (services/audren/Integration/ThreadBoost.h). macOS has no 15.6 ms tick to shorten, so both are no-ops that succeed;
// they are declared in src/os/nx_os.h.
#pragma once
#include "nx_os.h"

typedef struct timecaps_tag {
    UINT wPeriodMin;
    UINT wPeriodMax;
} TIMECAPS, *PTIMECAPS, *LPTIMECAPS;

#define TIMERR_NOERROR 0
#define TIMERR_NOCANDO 97

#if defined(__cplusplus)
extern "C" {
#endif
UINT timeGetDevCaps(TIMECAPS* caps, UINT size);
#if defined(__cplusplus)
}
#endif
