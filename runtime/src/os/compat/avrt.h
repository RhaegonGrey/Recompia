// SPDX-License-Identifier: GPL-3.0-or-later
// <avrt.h> for the POSIX build: MMCSS thread characteristics.
//
// Windows' Multimedia Class Scheduler boosts an audio thread above everything else; macOS reaches the same place through
// a QoS class, which the shim's SetThreadPriority already applies (os_posix.cpp: QOS_CLASS_USER_INTERACTIVE for
// THREAD_PRIORITY_TIME_CRITICAL). So AvSetMmThreadCharacteristicsW reports failure and
// services/audren/Integration/ThreadBoost.h takes its SetThreadPriority fallback, which is the right one here.
#pragma once
#include "nx_os.h"

#if defined(__cplusplus)
extern "C" {
#endif
HANDLE AvSetMmThreadCharacteristicsW(LPCWSTR task, LPDWORD index);
HANDLE AvSetMmThreadCharacteristicsA(LPCSTR task, LPDWORD index);
BOOL AvRevertMmThreadCharacteristics(HANDLE handle);
BOOL AvSetMmThreadPriority(HANDLE handle, int priority);
#if defined(__cplusplus)
}
#endif

#define THREAD_PRIORITY_CRITICAL 15

typedef enum _AVRT_PRIORITY {
    AVRT_PRIORITY_LOW = -1,
    AVRT_PRIORITY_NORMAL = 0,
    AVRT_PRIORITY_HIGH = 1,
    AVRT_PRIORITY_CRITICAL = 2,
} AVRT_PRIORITY;
