// SPDX-License-Identifier: GPL-3.0-or-later
// Host probe (WP01, [diag] stutter_logs): on Linux only, once, 60 s after the first present, logs whether the heap's pages are backed by
// transparent huge pages (AnonHugePages / Rss of the mapping holding mem::kHeapBase from /proc/self/smaps, and the THP mode from
// /sys/kernel/mm/transparent_hugepage/enabled). Nothing happens elsewhere.
#pragma once

namespace nxrt::diag {

// Called from the hitch monitor's summary tick (one thread); `presented` = at least one frame has been presented.
void host_probe_tick(bool presented);

}  // namespace nxrt::diag
