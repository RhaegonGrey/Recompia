// SPDX-License-Identifier: GPL-3.0-or-later
// Leveled logging to a file (and optionally stderr). Thread safe; never allocates unbounded memory.
// Line format:  [  12.345678 I t1234 name] tag: message
//   seconds since log_init, level letter (E W I D T), host thread id, guest/host thread name (log_set_thread_name).
#pragma once
#include <cstdarg>
#include <cstdint>

namespace nxrt {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

struct LogOptions {
    const char* path = nullptr;      // null: "nxrt.log" in the current directory ("" = no file)
    LogLevel level = LogLevel::Info;
    bool to_stderr = true;           // mirror lines at Warn and above to stderr (everything with stderr_all)
    bool stderr_all = false;
    bool keep_previous = true;       // rename an existing log to <path>.prev first
};

// Initialise (or re-initialise: closes the old file). Reads NXRT_LOG (path) and NXRT_LOG_LEVEL (error|warn|info|debug|trace
// or 0..4) from the environment over `opt`, then the [log] level/path keys of the config (see config.h) if loaded before.
// Calling logf before log_init initialises with defaults.
void log_init(const LogOptions& opt = {});
void log_shutdown();                   // flush and close the file
void log_flush();
LogLevel log_level();
bool log_enabled(LogLevel l);          // l <= current level
const char* log_path();                // path of the file actually open ("" when none)
void log_set_thread_name(const char* name);   // copied; shown in the line prefix of this thread
const char* log_thread_name();
// Every name given with log_set_thread_name, for a reader on another thread: fn(host thread id, the thread's creation time as GetThreadTimes
// reports it (FILETIME as 64 bits, 0 when unknown), name, user). A thread that ended stays listed until its id is named again, so a caller
// that holds a thread by handle compares the creation time before it trusts the name. fn runs under the table's lock: it must not log.
void log_each_thread_name(void (*fn)(uint32_t host_tid, uint64_t created, const char* name, void* user), void* user);
LogLevel log_parse_level(const char* s, LogLevel fallback);

void logv(LogLevel l, const char* tag, const char* fmt, va_list ap);
void logf(LogLevel l, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

// Every level tests the filter first, so the arguments (strings built for the line, locks taken by a getter) are not evaluated for a level
// that is off. A line printed before log_init starts the log (the level is Info until then, so E, W and I always reach logf).
#define NXLOG_AT(lvl, tag, ...) do { if (::nxrt::log_enabled(lvl)) ::nxrt::logf(lvl, tag, __VA_ARGS__); } while (0)
#define NXLOG_E(tag, ...) NXLOG_AT(::nxrt::LogLevel::Error, tag, __VA_ARGS__)
#define NXLOG_W(tag, ...) NXLOG_AT(::nxrt::LogLevel::Warn, tag, __VA_ARGS__)
#define NXLOG_I(tag, ...) NXLOG_AT(::nxrt::LogLevel::Info, tag, __VA_ARGS__)
#define NXLOG_D(tag, ...) NXLOG_AT(::nxrt::LogLevel::Debug, tag, __VA_ARGS__)
#define NXLOG_T(tag, ...) NXLOG_AT(::nxrt::LogLevel::Trace, tag, __VA_ARGS__)

}  // namespace nxrt
