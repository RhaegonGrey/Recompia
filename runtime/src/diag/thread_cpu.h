// SPDX-License-Identifier: GPL-3.0-or-later
// Thread CPU table ([diag] thread_cpu): with every frame summary of the hitch monitor, one log line that says which threads of this process
// used the processor since the previous summary. Threads are taken from the system's list of this process, so driver, audio-engine and
// other threads the runtime never named are counted too; the names are those of log_set_thread_name. Nothing runs between two summaries.
#pragma once

namespace nxrt::diag {

// Called from the hitch monitor's summary tick (one thread at a time).
void thread_cpu_tick();

}  // namespace nxrt::diag
