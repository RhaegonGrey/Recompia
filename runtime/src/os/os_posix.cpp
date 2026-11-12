// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix.cpp: the POSIX side of src/os/nx_os.h -- handles, threads, synchronisation, waitable objects, time,
// string conversion, module loading, structured exceptions, console.
//
// A HANDLE is the object pointer, so CloseHandle is a dereference rather than a table lookup and DuplicateHandle is
// a refcount bump (diag/registry.cpp and kernel/thread.cpp both duplicate the current thread handle).
// GetCurrentProcess()/GetCurrentThread() are the two Win32 pseudo-handles ((HANDLE)-1 / (HANDLE)-2) and are
// recognised before the tag check; (HANDLE)-1 is also INVALID_HANDLE_VALUE, exactly as on Windows.
//
// Waiting follows one rule: every waitable holds an atomic predicate, and every state change broadcasts a single
// process-wide condition variable that every waiter re-checks after each wakeup. One condition variable for the whole
// process costs a broadcast per signal, and the runtime signals tens of times per frame, not millions; a condition
// variable per object plus a hand-rolled multi-object wait buys nothing here and gets the lost-wakeup cases wrong.
// The fast path -- an already-signalled object -- takes no lock at all.
#include "nx_os_internal.h"

// The two SDK-shaped headers whose entry points live in this file.
#include "mmsystem.h"
#include "shellapi.h"

#if !defined(_WIN32)

#include <dlfcn.h>
#include <errno.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/task_info.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#else
#include <ucontext.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace nxos;

namespace {

constexpr DWORD kPseudoProcess = static_cast<DWORD>(-1);
constexpr DWORD kPseudoThread = static_cast<DWORD>(-2);

std::atomic<DWORD> g_next_tid{1};
thread_local DWORD t_tid = 0;
thread_local ThreadObj* t_self = nullptr;
thread_local DWORD t_last_error = 0;

pthread_mutex_t g_mod_mu = PTHREAD_MUTEX_INITIALIZER;
ModuleObj* g_main_module = nullptr;

}  // namespace

namespace nxos {

pthread_mutex_t g_wait_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_wait_cv = PTHREAD_COND_INITIALIZER;

void wait_note() {
    pthread_mutex_lock(&g_wait_mu);
    pthread_cond_broadcast(&g_wait_cv);
    pthread_mutex_unlock(&g_wait_mu);
}

// A condition variable deadline. pthread_cond_timedwait reads the condition variable's clock, which is CLOCK_REALTIME
// unless pthread_condattr_setclock said otherwise -- and Darwin has no pthread_condattr_setclock at all, while several
// of the objects here are PTHREAD_COND_INITIALIZER statics that cannot take an attribute. One clock for every condvar in
// the process therefore: the realtime clock. The cost is that a wall-clock jump moves a pending timeout; the runtime
// never sets the clock, and Win32's own absolute-timeout path has the same property.
void make_deadline(struct timespec* ts, DWORD ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += static_cast<time_t>(ms / 1000);
    ts->tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

bool past(const struct timespec& ts) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec > ts.tv_sec || (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec);
}

Obj* as_obj(HANDLE h) {
    if (!h) return nullptr;
    if (h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoProcess)) ||
        h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread)))
        return nullptr;
    auto* o = static_cast<Obj*>(h);
    return o->magic == kObjMagic ? o : nullptr;
}

void retain(Obj* o) { o->refs.fetch_add(1, std::memory_order_relaxed); }

void release(Obj* o) {
    if (o && o->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete o;
}

DWORD self_tid() {
    if (t_tid == 0) t_tid = g_next_tid.fetch_add(1, std::memory_order_relaxed);
    return t_tid;
}

ThreadObj* self_thread() {
    if (!t_self) {
        auto* t = new ThreadObj();
        t->th = pthread_self();
        t->tid = self_tid();
        t->started.store(true, std::memory_order_relaxed);
        t_self = t;
    }
    return t_self;
}

uint64_t filetime_of(const struct timespec& ts) {
    return 11644473600ull * 10000000ull + static_cast<uint64_t>(ts.tv_sec) * 10000000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 100ull;
}

void set_filetime(FILETIME* ft, uint64_t v) {
    ft->dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFu);
    ft->dwHighDateTime = static_cast<DWORD>(v >> 32);
}

std::string native_path(const char* p) {
    std::string s = p ? p : "";
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

bool signaled_of(Obj* o) {
    switch (o->kind) {
        case Kind::Thread: return static_cast<ThreadObj*>(o)->exited.load(std::memory_order_acquire);
        case Kind::Process: return false;   // a foreign pid cannot be waited on; see OpenProcess
        case Kind::Semaphore: {
            auto* s = static_cast<SemaphoreObj*>(o);
            pthread_mutex_lock(&s->mu);
            const bool free_slot = s->count > 0;
            pthread_mutex_unlock(&s->mu);
            return free_slot;
        }
        case Kind::Event:
        case Kind::Timer: return static_cast<Waitable*>(o)->signaled.load(std::memory_order_acquire);
        case Kind::Mutex: {
            // A mutex is available when nobody owns it, and a thread that already owns it re-acquires it (Win32 mutexes are
            // recursive). Without the owner test the shim's mutexes started out "not signaled" and no wait ever completed:
            // the fs save root lock blocked the main thread for the whole run.
            auto* m = static_cast<MutexObj*>(o);
            return m->owner == self_tid() || m->signaled.load(std::memory_order_acquire);
        }
        default: return false;
    }
}

void consume(Obj* o) {
    if (o->kind == Kind::Event) {
        auto* e = static_cast<EventObj*>(o);
        if (!e->manual) e->signaled.store(false, std::memory_order_release);
        e->epoch.fetch_add(1);
    } else if (o->kind == Kind::Semaphore) {
        auto* s = static_cast<SemaphoreObj*>(o);
        pthread_mutex_lock(&s->mu);
        if (s->count > 0) --s->count;
        pthread_mutex_unlock(&s->mu);
        s->epoch.fetch_add(1);
    } else if (o->kind == Kind::Mutex) {
        auto* m = static_cast<MutexObj*>(o);
        if (m->owner == self_tid()) {
            ++m->depth;   // recursive: the owner takes it again
            return;
        }
        if (pthread_mutex_trylock(&m->mu) != 0) pthread_mutex_lock(&m->mu);   // lost the race: wait for the owner
        m->owner = self_tid();
        m->depth = 1;
        m->signaled.store(false, std::memory_order_release);
        m->epoch.fetch_add(1);
    }
}

}  // namespace nxos

namespace {

ModuleObj* main_module() {
    pthread_mutex_lock(&g_mod_mu);
    if (!g_main_module) {
        g_main_module = new ModuleObj();
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&main_module), &info) && info.dli_fname) g_main_module->path = info.dli_fname;
    }
    pthread_mutex_unlock(&g_mod_mu);
    return g_main_module;
}

}  // namespace

// ==================================================================================================================
// errors
// ==================================================================================================================
DWORD GetLastError(void) { return t_last_error; }
void SetLastError(DWORD err) { t_last_error = err; }

void nxos_set_error_from_errno(int err) {
    switch (err) {
        case 0: t_last_error = ERROR_SUCCESS; break;
        case ENOENT: t_last_error = ERROR_FILE_NOT_FOUND; break;
        case ENOTDIR: t_last_error = ERROR_PATH_NOT_FOUND; break;
        case EACCES:
        case EPERM: t_last_error = ERROR_ACCESS_DENIED; break;
        case EEXIST: t_last_error = ERROR_ALREADY_EXISTS; break;
        case EBADF: t_last_error = ERROR_INVALID_HANDLE; break;
        case EINVAL: t_last_error = ERROR_INVALID_PARAMETER; break;
        case ENOMEM: t_last_error = ERROR_NOT_ENOUGH_MEMORY; break;
        case ENOSPC: t_last_error = ERROR_DISK_FULL; break;
        case ENOTSUP: t_last_error = ERROR_NOT_SUPPORTED; break;
        case EINTR: t_last_error = ERROR_OPERATION_ABORTED; break;
        case ETIMEDOUT: t_last_error = ERROR_TIMEOUT; break;
        case EISDIR: t_last_error = ERROR_ACCESS_DENIED; break;
        case ENAMETOOLONG:
        case ELOOP: t_last_error = ERROR_INVALID_NAME; break;
        default: t_last_error = ERROR_GEN_FAILURE; break;
    }
}

// ==================================================================================================================
// time
// ==================================================================================================================
BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
    if (!out) return FALSE;
#if defined(__APPLE__)
    out->QuadPart = static_cast<LONGLONG>(mach_absolute_time());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    out->QuadPart = static_cast<LONGLONG>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
#endif
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* out) {
    if (!out) return FALSE;
#if defined(__APPLE__)
    // mach_absolute_time counts timebase ticks; report the frequency in the same unit so ns = ticks * 1e9 / freq.
    static const double ticks_per_ns = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        return static_cast<double>(tb.denom) / static_cast<double>(tb.numer);
    }();
    out->QuadPart = static_cast<LONGLONG>(ticks_per_ns * 1e9);
#else
    out->QuadPart = 1000000000LL;
#endif
    return TRUE;
}

static uint64_t monotonic_ns() {
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    if (f.QuadPart <= 0) return 0;
    return static_cast<uint64_t>((static_cast<double>(c.QuadPart) * 1e9) / static_cast<double>(f.QuadPart));
}

DWORD GetTickCount(void) { return static_cast<DWORD>(GetTickCount64() & 0xFFFFFFFFu); }

ULONGLONG GetTickCount64(void) { return monotonic_ns() / 1000000ull; }

static void fill_systemtime(LPSYSTEMTIME st, const struct tm& t) {
    st->wYear = static_cast<WORD>(t.tm_year + 1900);
    st->wMonth = static_cast<WORD>(t.tm_mon + 1);
    st->wDayOfWeek = static_cast<WORD>(t.tm_wday);
    st->wDay = static_cast<WORD>(t.tm_mday);
    st->wHour = static_cast<WORD>(t.tm_hour);
    st->wMinute = static_cast<WORD>(t.tm_min);
    st->wSecond = static_cast<WORD>(t.tm_sec);
    st->wMilliseconds = 0;
}

void GetSystemTime(LPSYSTEMTIME st) {
    if (!st) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t{};
    gmtime_r(&ts.tv_sec, &t);
    fill_systemtime(st, t);
    st->wMilliseconds = static_cast<WORD>(ts.tv_nsec / 1000000);
}

void GetLocalTime(LPSYSTEMTIME st) {
    if (!st) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t{};
    localtime_r(&ts.tv_sec, &t);
    fill_systemtime(st, t);
    st->wMilliseconds = static_cast<WORD>(ts.tv_nsec / 1000000);
}

void GetSystemTimeAsFileTime(LPFILETIME ft) {
    if (!ft) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    set_filetime(ft, filetime_of(ts));
}

ULONGLONG GetSystemTimeAsUnixTime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<ULONGLONG>(ts.tv_sec);
}

BOOL SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft) {
    if (!st || !ft) return FALSE;
    struct tm t{};
    t.tm_year = st->wYear - 1900;
    t.tm_mon = st->wMonth - 1;
    t.tm_mday = st->wDay;
    t.tm_hour = st->wHour;
    t.tm_min = st->wMinute;
    t.tm_sec = st->wSecond;
    const time_t secs = timegm(&t);
    set_filetime(ft, 11644473600ull * 10000000ull + static_cast<uint64_t>(secs) * 10000000ull +
                         static_cast<uint64_t>(st->wMilliseconds) * 10000ull);
    return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st) {
    if (!ft || !st) return FALSE;
    const uint64_t v = (static_cast<uint64_t>(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
    const uint64_t unix_100ns = v - 11644473600ull * 10000000ull;
    time_t secs = static_cast<time_t>(unix_100ns / 10000000ull);
    struct tm t{};
    gmtime_r(&secs, &t);
    fill_systemtime(st, t);
    st->wMilliseconds = static_cast<WORD>((unix_100ns % 10000000ull) / 10000ull);
    return TRUE;
}

BOOL FileTimeToLocalFileTime(const FILETIME* ft, LPFILETIME local) {
    if (!ft || !local) return FALSE;
    const uint64_t v = (static_cast<uint64_t>(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
    const time_t secs = static_cast<time_t>((v - 11644473600ull * 10000000ull) / 10000000ull);
    struct tm t{};
    localtime_r(&secs, &t);
    set_filetime(local, 11644473600ull * 10000000ull + static_cast<uint64_t>(timegm(&t)) * 10000000ull +
                            (v - 11644473600ull * 10000000ull) % 10000000ull);
    return TRUE;
}

BOOL LocalFileTimeToFileTime(const FILETIME* ft, LPFILETIME utc) {
    if (!ft || !utc) return FALSE;
    const uint64_t v = (static_cast<uint64_t>(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
    const time_t secs = static_cast<time_t>((v - 11644473600ull * 10000000ull) / 10000000ull);
    struct tm t{};
    gmtime_r(&secs, &t);
    const time_t utc_secs = timegm(&t);
    struct tm local{};
    localtime_r(&utc_secs, &local);
    set_filetime(utc, 11644473600ull * 10000000ull + static_cast<uint64_t>(timegm(&local)) * 10000000ull +
                          (v - 11644473600ull * 10000000ull) % 10000000ull);
    return TRUE;
}

// macOS has no timer-resolution knob: the scheduler is not on a 15.6 ms tick, so these are no-ops that succeed.
DWORD timeBeginPeriod(UINT) { return 0; }
DWORD timeEndPeriod(UINT) { return 0; }

UINT timeGetDevCaps(TIMECAPS* caps, UINT size) {
    if (!caps || size < sizeof(TIMECAPS)) return 97;   // TIMERR_NOCANDO
    caps->wPeriodMin = 1;
    caps->wPeriodMax = 1000;
    return 0;   // TIMERR_NOERROR
}

// ==================================================================================================================
// process and thread identity
// ==================================================================================================================
HANDLE GetCurrentProcess(void) { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoProcess)); }
DWORD GetCurrentProcessId(void) { return static_cast<DWORD>(getpid()); }
HANDLE GetCurrentThread(void) { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread)); }
DWORD GetCurrentThreadId(void) { return self_tid(); }

void Sleep(DWORD ms) {
    if (ms == 0) {
        sched_yield();
        return;
    }
    struct timespec ts{static_cast<time_t>(ms / 1000), static_cast<long>(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

DWORD SleepEx(DWORD ms, BOOL) {
    Sleep(ms);
    return 0;
}

BOOL SwitchToThread(void) { return sched_yield() == 0 ? TRUE : FALSE; }

// ==================================================================================================================
// handles
// ==================================================================================================================
BOOL CloseHandle(HANDLE h) {
    if (!h || h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoProcess)) ||
        h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread))) {
        return TRUE;   // pseudo handles are not ours to close (thread.cpp:184 duplicates before it keeps one)
    }
    Obj* o = as_obj(h);
    if (!o) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    release(o);
    return TRUE;
}

BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, HANDLE* out, DWORD, BOOL, DWORD) {
    if (!out) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    if (src == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread))) {
        ThreadObj* t = self_thread();
        retain(t);
        *out = t;
        return TRUE;
    }
    if (src == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoProcess))) {
        *out = new ProcessObj(static_cast<DWORD>(getpid()));
        return TRUE;
    }
    Obj* o = as_obj(src);
    if (!o) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    retain(o);
    *out = o;
    return TRUE;
}

HANDLE OpenProcess(DWORD, BOOL, DWORD pid) {
    if (pid == 0) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    // A foreign pid can only be probed, not waited on: fs_volume.cpp uses this for "is the writer still alive".
    if (pid != static_cast<DWORD>(getpid()) && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    return new ProcessObj(pid);
}

HANDLE OpenThread(DWORD, BOOL, DWORD tid) {
    if (tid == self_tid()) {
        ThreadObj* t = self_thread();
        retain(t);
        return t;
    }
    t_last_error = ERROR_INVALID_PARAMETER;   // no cross-thread pthread_t lookup without a mach thread port
    return nullptr;
}

BOOL TerminateProcess(HANDLE, UINT code) {
    std::fflush(nullptr);
    _exit(static_cast<int>(code));
}

void ExitProcess(UINT code) {
    std::fflush(nullptr);
    _exit(static_cast<int>(code));
}

BOOL GetExitCodeProcess(HANDLE h, PDWORD code) {
    if (!code) return FALSE;
    auto* p = as<ProcessObj>(h, Kind::Process);
    if (!p) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    *code = STILL_ACTIVE;
    return TRUE;
}

DWORD GetProcessHandleCount(HANDLE, PDWORD count) {
    if (count) *count = 0;
    return TRUE;
}

BOOL GetExitCodeThread(HANDLE h, PDWORD code) {
    if (!code) return FALSE;
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    // The shim keeps no per-thread exit code (a pthread returns void*), so a thread that has ended reports 0 and a running one
    // STILL_ACTIVE: the caller asks whether it ended (diag/hle_census.cpp), not what it returned.
    *code = t->exited.load(std::memory_order_acquire) ? 0u : STILL_ACTIVE;
    return TRUE;
}

// ==================================================================================================================
// SRWLOCK: a real reader/writer lock over one pthread mutex plus one condition variable
//
// `readers` is the state: > 0 = that many shared holders, 0 = free, -1 = one exclusive holder. The exclusive holder
// does NOT keep the mutex: SleepConditionVariableSRW needs it held to hand to pthread_cond_wait, and it releases the
// write lock atomically by clearing `readers` and broadcasting just before the wait. A thread that wants the exclusive
// lock always takes the mutex first, sees `readers == 0`, sets -1 and lets go; a waker that needs the exclusive lock
// to signal cannot slip into the window between the clear and the cond_wait, because it blocks on the mutex that the
// waiter still holds there. That is exactly the Win32 contract (a condition variable wait on an SRWLOCK must hold it
// exclusively), and it is why no wakeup can be lost.
//
// SRWLOCK and CONDITION_VARIABLE are one pointer each because static storage zeroed by the loader must be a valid
// unlocked lock (os/sync.cpp:110 says so, and KThread::cv_storage is one void*), while a Darwin pthread_mutex_t is 64
// bytes whose zeroed state is not initialised. The pthread objects are allocated on first use instead.
// ==================================================================================================================
namespace {

struct SrwImpl {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    int readers = 0;
};

pthread_mutex_t g_srw_init_mu = PTHREAD_MUTEX_INITIALIZER;

SrwImpl* srw_get(SRWLOCK* lock) {
    SrwImpl* impl = static_cast<SrwImpl*>(__atomic_load_n(&lock->impl, __ATOMIC_ACQUIRE));
    if (impl) return impl;
    pthread_mutex_lock(&g_srw_init_mu);
    impl = static_cast<SrwImpl*>(lock->impl);
    if (!impl) {
        impl = new SrwImpl();
        __atomic_store_n(&lock->impl, impl, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_srw_init_mu);
    return impl;
}

pthread_mutex_t g_cv_init_mu = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t* cv_get(CONDITION_VARIABLE* cv) {
    pthread_cond_t* impl = static_cast<pthread_cond_t*>(__atomic_load_n(&cv->impl, __ATOMIC_ACQUIRE));
    if (impl) return impl;
    pthread_mutex_lock(&g_cv_init_mu);
    impl = static_cast<pthread_cond_t*>(cv->impl);
    if (!impl) {
        impl = new pthread_cond_t();
        pthread_cond_init(impl, nullptr);
        __atomic_store_n(&cv->impl, impl, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_cv_init_mu);
    return impl;
}

struct CsImpl {
    pthread_mutex_t mu;
};

pthread_mutex_t g_cs_init_mu = PTHREAD_MUTEX_INITIALIZER;

CsImpl* cs_get(CRITICAL_SECTION* cs) {
    CsImpl* impl = static_cast<CsImpl*>(__atomic_load_n(&cs->impl, __ATOMIC_ACQUIRE));
    if (impl) return impl;
    pthread_mutex_lock(&g_cs_init_mu);
    impl = static_cast<CsImpl*>(cs->impl);
    if (!impl) {
        impl = new CsImpl();
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&impl->mu, &attr);
        pthread_mutexattr_destroy(&attr);
        __atomic_store_n(&cs->impl, impl, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_cs_init_mu);
    return impl;
}

}  // namespace

void InitializeSRWLock(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    impl->readers = 0;
    pthread_cond_broadcast(&impl->cv);
    pthread_mutex_unlock(&impl->mu);
}

void AcquireSRWLockExclusive(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    while (impl->readers != 0) pthread_cond_wait(&impl->cv, &impl->mu);
    impl->readers = -1;
    pthread_mutex_unlock(&impl->mu);
}

void ReleaseSRWLockExclusive(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    impl->readers = 0;
    pthread_cond_broadcast(&impl->cv);
    pthread_mutex_unlock(&impl->mu);
}

void AcquireSRWLockShared(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    while (impl->readers < 0) pthread_cond_wait(&impl->cv, &impl->mu);
    impl->readers++;
    pthread_mutex_unlock(&impl->mu);
}

void ReleaseSRWLockShared(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    if (--impl->readers == 0) pthread_cond_broadcast(&impl->cv);
    pthread_mutex_unlock(&impl->mu);
}

BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    if (impl->readers != 0) {
        pthread_mutex_unlock(&impl->mu);
        return 0;
    }
    impl->readers = -1;
    pthread_mutex_unlock(&impl->mu);
    return 1;
}

BOOLEAN TryAcquireSRWLockShared(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    if (impl->readers < 0) {
        pthread_mutex_unlock(&impl->mu);
        return 0;
    }
    impl->readers++;
    pthread_mutex_unlock(&impl->mu);
    return 1;
}

void InitializeConditionVariable(PCONDITION_VARIABLE cv) { (void)cv_get(cv); }

BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG) {
    pthread_cond_t* cond = cv_get(cv);
    SrwImpl* impl = srw_get(lock);

    pthread_mutex_lock(&impl->mu);
    if (impl->readers != -1) {
        // Not held exclusively: the Win32 contract is broken at the call site. Releasing `readers` would corrupt the
        // lock, so report failure instead of pretending.
        pthread_mutex_unlock(&impl->mu);
        t_last_error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    impl->readers = 0;   // release the write lock, still holding mu: atomic with the wait below
    pthread_cond_broadcast(&impl->cv);

    int rc;
    if (ms == INFINITE) {
        rc = pthread_cond_wait(cond, &impl->mu);
    } else {
        struct timespec dl;
        make_deadline(&dl, ms);
        rc = pthread_cond_timedwait(cond, &impl->mu, &dl);
    }
    // Reacquire the write lock while still holding mu.
    while (impl->readers != 0) pthread_cond_wait(&impl->cv, &impl->mu);
    impl->readers = -1;
    pthread_mutex_unlock(&impl->mu);

    if (rc == ETIMEDOUT) {
        t_last_error = ERROR_TIMEOUT;
        return FALSE;
    }
    return TRUE;
}

BOOL SleepConditionVariableCS(PCONDITION_VARIABLE, PCRITICAL_SECTION, DWORD) {
    t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
    return FALSE;
}

void WakeConditionVariable(PCONDITION_VARIABLE cv) {
    // A never-used condition variable has no waiter: signalling it is a no-op, and every caller re-checks a flag
    // under the lock before waiting (kernel_sync.cpp block_on_wake, os/sync.cpp wait_step), so nothing is lost.
    pthread_cond_t* impl = static_cast<pthread_cond_t*>(__atomic_load_n(&cv->impl, __ATOMIC_ACQUIRE));
    if (impl) pthread_cond_signal(impl);
}

void WakeAllConditionVariable(PCONDITION_VARIABLE cv) {
    pthread_cond_t* impl = static_cast<pthread_cond_t*>(__atomic_load_n(&cv->impl, __ATOMIC_ACQUIRE));
    if (impl) pthread_cond_broadcast(impl);
}

// ==================================================================================================================
// CRITICAL_SECTION (the vendored Link Play module only)
// ==================================================================================================================
void InitializeCriticalSection(PCRITICAL_SECTION cs) { (void)cs_get(cs); }
void EnterCriticalSection(PCRITICAL_SECTION cs) { pthread_mutex_lock(&cs_get(cs)->mu); }
void LeaveCriticalSection(PCRITICAL_SECTION cs) { pthread_mutex_unlock(&cs_get(cs)->mu); }
BOOL TryEnterCriticalSection(PCRITICAL_SECTION cs) { return pthread_mutex_trylock(&cs_get(cs)->mu) == 0 ? TRUE : FALSE; }
void DeleteCriticalSection(PCRITICAL_SECTION) {}

// ==================================================================================================================
// threads
// ==================================================================================================================
namespace {

#if defined(__linux__)
// The main thread's stack, the Linux side of pkp.exe's /STACK:134217728: the host main thread runs the guest's main thread and
// lifted code nests deeply. Linux grows the main stack up to RLIMIT_STACK (8 MiB on SteamOS and most desktops) and ignores the
// PT_GNU_STACK size the link writes; it keeps at least 128 MiB free below the stack for that growth, so raising the soft limit
// before main() is enough. glibc sized its default thread stack from the limit before this runs, so other threads keep theirs.
// A hard limit below 128 MiB caps it there. macOS takes the size from the executable's LC_MAIN (-stack_size).
// From Dittobyte/Recompia#546 (game-build/src/pkp_host_stack_linux.cpp there).
[[gnu::constructor]] void raise_main_stack() {
    constexpr rlim_t kMainStack = 134217728;
    rlimit rl{};
    if (getrlimit(RLIMIT_STACK, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= kMainStack) return;
    rl.rlim_cur = rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= kMainStack ? kMainStack : rl.rlim_max;
    setrlimit(RLIMIT_STACK, &rl);
}
#endif

struct StartBox {
    PTHREAD_START_ROUTINE fn = nullptr;
    LPVOID arg = nullptr;
    ThreadObj* self = nullptr;
};

void apply_priority(ThreadObj* t) {
#if defined(__APPLE__)
    if (t->th != pthread_self()) return;   // QoS is set from the thread itself
    const int p = t->priority.load(std::memory_order_relaxed);
    qos_class_t q = QOS_CLASS_DEFAULT;
    if (p >= 15) q = QOS_CLASS_USER_INTERACTIVE;
    else if (p >= 2) q = QOS_CLASS_USER_INITIATED;
    else if (p >= 0) q = QOS_CLASS_DEFAULT;
    else if (p >= -2) q = QOS_CLASS_UTILITY;
    else q = QOS_CLASS_BACKGROUND;
    pthread_set_qos_class_self_np(q, 0);
#else
    (void)t;
#endif
}

void* thread_trampoline(void* p) {
    StartBox box = *static_cast<StartBox*>(p);
    delete static_cast<StartBox*>(p);
    nxos_install_altstack();   // so a fault on this thread is reportable even when its stack is exhausted
    t_tid = box.self->tid;
    t_self = box.self;
    box.self->started.store(true, std::memory_order_release);

    // CREATE_SUSPENDED: the creator fills the guest Ctx before the thread may run (kernel/thread.cpp:146).
    if (box.self->suspend.load(std::memory_order_acquire) > 0) {
        pthread_mutex_lock(&box.self->mu);
        while (box.self->suspend.load(std::memory_order_relaxed) > 0)
            pthread_cond_wait(&box.self->cv, &box.self->mu);
        pthread_mutex_unlock(&box.self->mu);
    }
    apply_priority(box.self);

    struct timespec t0;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
    if (box.fn) box.fn(box.arg);
    struct timespec t1;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);

    box.self->cpu_ns = static_cast<uint64_t>(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                       static_cast<uint64_t>(t1.tv_nsec - t0.tv_nsec);
    box.self->exited.store(true, std::memory_order_release);
    wait_note();
    t_self = nullptr;
    release(box.self);
    return nullptr;
}

}  // namespace

HANDLE CreateThread(void*, SIZE_T stack, PTHREAD_START_ROUTINE start, LPVOID arg, DWORD flags, DWORD* tid) {
    auto* t = new ThreadObj();
    t->tid = g_next_tid.fetch_add(1, std::memory_order_relaxed);
    if (flags & CREATE_SUSPENDED) t->suspend.store(1, std::memory_order_relaxed);
    if (tid) *tid = t->tid;

    // Two references: the handle the caller gets, and the one the running thread holds until its trampoline returns.
    // Without the second, a CloseHandle right after CreateThread would free the object the thread is still writing to.
    retain(t);
    auto* box = new StartBox{start, arg, t};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (stack) pthread_attr_setstacksize(&attr, std::max<size_t>(stack, 64u << 10));
    const int rc = pthread_create(&t->th, &attr, thread_trampoline, box);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete box;
        release(t);   // the running-thread reference
        release(t);   // the handle reference
        t_last_error = ERROR_NOT_ENOUGH_MEMORY;
        if (tid) *tid = 0;
        return nullptr;
    }
    return t;
}

BOOL SetThreadPriority(HANDLE h, int priority) {
    if (h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread))) {
        ThreadObj* t = self_thread();
        t->priority.store(priority, std::memory_order_relaxed);
        apply_priority(t);
        return TRUE;
    }
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    t->priority.store(priority, std::memory_order_relaxed);
    apply_priority(t);
    return TRUE;
}

int GetThreadPriority(HANDLE h) {
    if (h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread))) return self_thread()->priority.load();
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        t_last_error = ERROR_INVALID_HANDLE;
        return THREAD_PRIORITY_ERROR_RETURN_VALUE;
    }
    return t->priority.load();
}

BOOL SetThreadPriorityBoost(HANDLE, BOOL) { return TRUE; }   // no dynamic priority boost to turn off

DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR) {
    // macOS has no affinity API: thread_policy_set(THREAD_AFFINITY_POLICY) is a hint at best and has been reported to
    // hurt on Apple Silicon, where the scheduler already migrates between P and E cores. Returning the mask the caller
    // "had" keeps sched_apply on its happy path; the guest's core pinning degrades to a preference the host ignores.
    return 1;
}

BOOL GetProcessAffinityMask(HANDLE, PDWORD_PTR proc_mask, PDWORD_PTR sys_mask) {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    const DWORD_PTR all = (n > 0 && n < 64) ? ((static_cast<DWORD_PTR>(1) << n) - 1) : ~static_cast<DWORD_PTR>(0);
    if (proc_mask) *proc_mask = all;
    if (sys_mask) *sys_mask = all;
    return TRUE;
}

namespace {
DWORD g_priority_class = NORMAL_PRIORITY_CLASS;
}

BOOL SetPriorityClass(HANDLE, DWORD cls) {
    g_priority_class = cls;
    return TRUE;
}

DWORD GetPriorityClass(HANDLE) { return g_priority_class; }

BOOL SetProcessInformation(HANDLE, int, LPVOID, DWORD) {
    // Windows 11's EcoQoS and its timer-resolution floor have no macOS counterpart: there is no per-process power state
    // to set, and the scheduler is not on a tick. sched.cpp logs the failure once and moves on, which is the truth.
    t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
    return FALSE;
}

#if defined(__APPLE__)
// The mach port of a thread the shim knows, for thread_suspend/thread_get_state. MACH_PORT_NULL when it is gone.
mach_port_t mach_thread_of(ThreadObj* t) {
    if (!t || !t->started.load(std::memory_order_acquire) || t->exited.load(std::memory_order_acquire)) return MACH_PORT_NULL;
    return pthread_mach_thread_np(t->th);
}
#endif

DWORD SuspendThread(HANDLE h) {
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        t_last_error = ERROR_INVALID_HANDLE;
        return static_cast<DWORD>(-1);
    }
#if defined(__APPLE__)
    // thread_suspend on the mach port: the diag sampler and the freeze watchdog then read a stopped register set, which is
    // what GetThreadContext needs (thread_get_state fails on a running thread).
    const mach_port_t mt = mach_thread_of(t);
    if (mt == MACH_PORT_NULL || thread_suspend(mt) != KERN_SUCCESS) {
        t_last_error = ERROR_INVALID_HANDLE;
        return static_cast<DWORD>(-1);
    }
    return static_cast<DWORD>(t->suspend.fetch_add(1, std::memory_order_relaxed));
#else
    t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
    return static_cast<DWORD>(-1);
#endif
}

DWORD ResumeThread(HANDLE h) {
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        t_last_error = ERROR_INVALID_HANDLE;
        return static_cast<DWORD>(-1);
    }
#if defined(__APPLE__)
    if (const int n = t->suspend.load(std::memory_order_relaxed); n > 0) {
        if (const mach_port_t mt = mach_thread_of(t); mt != MACH_PORT_NULL) thread_resume(mt);
        return static_cast<DWORD>(t->suspend.fetch_sub(1, std::memory_order_relaxed) - 1);
    }
    return 0;
#else
    const int prev = t->suspend.load(std::memory_order_relaxed);
    if (prev > 0) {
        pthread_mutex_lock(&t->mu);
        t->suspend.store(prev - 1, std::memory_order_relaxed);
        pthread_cond_broadcast(&t->cv);
        pthread_mutex_unlock(&t->mu);
    }
    return static_cast<DWORD>(prev);
#endif
}

void ExitThread(DWORD) {
    if (t_self) {
        t_self->exited.store(true, std::memory_order_release);
        wait_note();
    }
    pthread_exit(nullptr);
}

// ==================================================================================================================
// waitable objects
// ==================================================================================================================
namespace {

DWORD wait_any(const HANDLE* handles, DWORD count, BOOL all, DWORD ms) {
    if (!handles || count == 0 || count > MAXIMUM_WAIT_OBJECTS) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return WAIT_FAILED;
    }
    Obj* objs[MAXIMUM_WAIT_OBJECTS];
    for (DWORD i = 0; i < count; i++) {
        objs[i] = as_obj(handles[i]);
        if (!objs[i]) {
            t_last_error = ERROR_INVALID_HANDLE;
            return WAIT_FAILED;
        }
    }

    struct timespec dl{};
    const bool timed = ms != INFINITE;
    if (timed) make_deadline(&dl, ms);

    for (;;) {
        bool ready = all != FALSE;
        DWORD hit = 0;
        bool any = false;
        for (DWORD i = 0; i < count; i++) {
            const bool s = signaled_of(objs[i]);
            if (s) {
                any = true;
                hit = i;
            }
            if (all) ready = ready && s;
        }
        if (all ? ready : any) {
            if (all) {
                for (DWORD i = 0; i < count; i++) consume(objs[i]);
                return WAIT_OBJECT_0;
            }
            consume(objs[hit]);
            return WAIT_OBJECT_0 + hit;
        }
        if (timed && past(dl)) return WAIT_TIMEOUT;

        pthread_mutex_lock(&g_wait_mu);
        // Re-check under the mutex: a signal that landed between the check above and this lock broadcast before we
        // took it, so the predicate read here is the one that broadcast belongs to.
        ready = all != FALSE;
        any = false;
        for (DWORD i = 0; i < count; i++) {
            const bool s = signaled_of(objs[i]);
            if (all) ready = ready && s;
            else if (s) any = true;
        }
        if (!(all ? ready : any)) {
            if (timed) pthread_cond_timedwait(&g_wait_cv, &g_wait_mu, &dl);
            else pthread_cond_wait(&g_wait_cv, &g_wait_mu);
        }
        pthread_mutex_unlock(&g_wait_mu);
    }
}

}  // namespace

DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return wait_any(&h, 1, FALSE, ms); }
DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL) { return wait_any(&h, 1, FALSE, ms); }
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL all, DWORD ms) {
    return wait_any(handles, count, all, ms);
}
DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL all, DWORD ms, BOOL) {
    return wait_any(handles, count, all, ms);
}

HANDLE nxos_create_event(const char* name, BOOL manual_reset, BOOL initial) {
    (void)name;   // the two named uses are single-process slot mutexes; see nxos_create_mutex
    auto* e = new EventObj(manual_reset != FALSE);
    e->signaled.store(initial != FALSE, std::memory_order_relaxed);
    return e;
}

HANDLE CreateEventA(void*, BOOL manual_reset, BOOL initial, LPCSTR name) {
    return nxos_create_event(name, manual_reset, initial);
}

BOOL SetEvent(HANDLE h) {
    auto* w = as<Waitable>(h, Kind::Event);
    if (!w) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    w->signaled.store(true, std::memory_order_release);
    w->epoch.fetch_add(1);
    wait_note();
    return TRUE;
}

BOOL ResetEvent(HANDLE h) {
    auto* w = as<Waitable>(h, Kind::Event);
    if (!w) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    w->signaled.store(false, std::memory_order_release);
    w->epoch.fetch_add(1);
    return TRUE;
}

// Named mutexes: kernel/sched.cpp picks a scheduler slot through one and tests for ERROR_ALREADY_EXISTS.
namespace {
pthread_mutex_t g_named_mu = PTHREAD_MUTEX_INITIALIZER;
std::vector<std::pair<std::string, MutexObj*>> g_named_mutexes;
}  // namespace

HANDLE nxos_create_mutex(const char* name, BOOL initial_owner) {
    auto* m = new MutexObj();
    if (name && *name) {
        m->name = name;
        pthread_mutex_lock(&g_named_mu);
        for (auto& kv : g_named_mutexes) {
            if (kv.first == m->name) {
                retain(kv.second);
                delete m;
                pthread_mutex_unlock(&g_named_mu);
                t_last_error = ERROR_ALREADY_EXISTS;
                return kv.second;
            }
        }
        g_named_mutexes.emplace_back(m->name, m);
        pthread_mutex_unlock(&g_named_mu);
    }
    if (initial_owner) {
        pthread_mutex_lock(&m->mu);
        m->owner = self_tid();
        m->depth = 1;
        m->signaled.store(false, std::memory_order_release);
    } else {
        m->signaled.store(true, std::memory_order_release);   // unowned: the first wait takes it
    }
    return m;
}

BOOL ReleaseMutex(HANDLE h) {
    auto* m = as<MutexObj>(h, Kind::Mutex);
    if (!m) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (m->owner == self_tid()) {
        if (m->depth > 1) {
            --m->depth;   // one of the recursive acquisitions
            return TRUE;
        }
        m->owner = 0;
        m->depth = 0;
        pthread_mutex_unlock(&m->mu);
        m->signaled.store(true, std::memory_order_release);
        m->epoch.fetch_add(1);
        wait_note();   // wake the threads blocked on it: the cv is the only wake-up path
    }
    return TRUE;
}

HANDLE CreateSemaphoreW(void*, LONG initial, LONG max, LPCWSTR) {
    auto* s = new SemaphoreObj();
    s->count = initial;
    s->max = max;
    return s;
}

BOOL ReleaseSemaphore(HANDLE h, LONG release, PLONG previous) {
    auto* s = as<SemaphoreObj>(h, Kind::Semaphore);
    if (!s) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    pthread_mutex_lock(&s->mu);
    if (previous) *previous = s->count;
    s->count = std::min<long>(s->count + release, s->max);
    pthread_mutex_unlock(&s->mu);
    wait_note();
    return TRUE;
}

// ---- waitable timers ----------------------------------------------------------------------------------------------
namespace {

void* timer_worker(void* p) {
    auto* t = static_cast<TimerObj*>(p);
    pthread_mutex_lock(&t->mu);
    while (!t->stop) {
        const uint64_t gen = t->generation;
        const uint64_t due = t->due_ns;
        if (due == 0) {
            pthread_cond_wait(&t->cv, &t->mu);
            continue;
        }
        const uint64_t now = monotonic_ns();
        if (due > now) {
            const uint64_t delta = due - now;
            struct timespec dl{};
            clock_gettime(CLOCK_REALTIME, &dl);   // see make_deadline: every condvar here is on the realtime clock
            dl.tv_sec += static_cast<time_t>(delta / 1000000000ull);
            dl.tv_nsec += static_cast<long>(delta % 1000000000ull);
            if (dl.tv_nsec >= 1000000000L) {
                dl.tv_sec += 1;
                dl.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&t->cv, &t->mu, &dl);
            continue;   // re-read the generation: a re-arm wins over the deadline
        }
        if (t->generation != gen) continue;
        t->signaled.store(true, std::memory_order_release);
        t->epoch.fetch_add(1);
        wait_note();
        if (t->period_ns) {
            t->due_ns = now + t->period_ns;
        } else {
            t->due_ns = 0;   // one-shot: disarmed until the next SetWaitableTimer
            pthread_cond_wait(&t->cv, &t->mu);
        }
    }
    pthread_mutex_unlock(&t->mu);
    return nullptr;
}

}  // namespace

HANDLE nxos_create_waitable_timer(const char* name, DWORD flags, DWORD) {
    (void)name;
    auto* t = new TimerObj();
    t->manual = (flags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0;
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION asks for a timer that is not quantised to the scheduler tick. On macOS
    // pthread_cond_timedwait on CLOCK_MONOTONIC is already sub-millisecond, so the flag needs no separate path.
    // Joinable, not detached: ~TimerObj joins it so the mutex and the condition variable outlive the wait (see
    // nx_os_internal.h). A detached worker would be waiting on destroyed objects the moment CloseHandle ran.
    if (pthread_create(&t->worker, nullptr, timer_worker, t) != 0) {
        delete t;
        t_last_error = ERROR_NOT_ENOUGH_MEMORY;
        return nullptr;
    }
    t->worker_started = true;
    return t;
}

BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period, void*, void*, BOOL) {
    auto* t = as<TimerObj>(h, Kind::Timer);
    if (!t || !due) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    const uint64_t now = monotonic_ns();
    // due->QuadPart is 100 ns units; negative means relative to now (frame_limiter.cpp:78, timestep.cpp:202).
    const int64_t q = due->QuadPart;
    const uint64_t target = q < 0 ? now + static_cast<uint64_t>(-q) * 100ull : static_cast<uint64_t>(q) * 100ull;

    pthread_mutex_lock(&t->mu);
    t->due_ns = target;
    t->period_ns = period > 0 ? static_cast<uint64_t>(period) * 1000000ull : 0;
    t->generation++;
    if (!t->manual) t->signaled.store(false, std::memory_order_release);
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    return TRUE;
}

BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER* due, LONG period, void* routine, void* arg, void* wake_context,
                        ULONG flags) {
    (void)wake_context;   // no suspend/resume power management to attach a timer to
    (void)flags;
    return SetWaitableTimer(h, due, period, routine, arg, FALSE);
}

BOOL CancelWaitableTimer(HANDLE h) {
    auto* t = as<TimerObj>(h, Kind::Timer);
    if (!t) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    pthread_mutex_lock(&t->mu);
    t->due_ns = 0;
    t->period_ns = 0;
    t->generation++;
    t->signaled.store(false, std::memory_order_release);
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    return TRUE;
}

// ==================================================================================================================
// string conversion (UTF-8 <-> UTF-32: wchar_t is 4 bytes here, 2 on Windows)
// ==================================================================================================================
namespace {

uint32_t utf8_next(const unsigned char*& p, const unsigned char* end) {
    const unsigned char b = *p;
    if (b < 0x80) {
        ++p;
        return b;
    }
    int extra;
    uint32_t cp;
    if ((b & 0xE0) == 0xC0) {
        extra = 1;
        cp = b & 0x1F;
    } else if ((b & 0xF0) == 0xE0) {
        extra = 2;
        cp = b & 0x0F;
    } else if ((b & 0xF8) == 0xF0) {
        extra = 3;
        cp = b & 0x07;
    } else {
        ++p;
        return 0xFFFD;
    }
    if (p + extra >= end) {
        ++p;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++) {
        const unsigned char c = p[i];
        if ((c & 0xC0) != 0x80) {
            ++p;
            return 0xFFFD;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    p += extra + 1;
    return cp;
}

size_t utf8_put(uint32_t cp, char* out) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

int wide_len(const wchar_t* s, int len) {
    if (len >= 0) return len;
    int n = 0;
    while (s[n]) ++n;
    return n + 1;   // a length of -1 includes the terminator, like Win32
}

int narrow_len(const char* s, int len) {
    if (len >= 0) return len;
    int n = 0;
    while (s[n]) ++n;
    return n + 1;
}

int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

}  // namespace

int MultiByteToWideChar(UINT cp, DWORD, LPCSTR src, int src_len, LPWSTR dst, int dst_len) {
    if (!src) return 0;
    if (cp != CP_UTF8 && cp != CP_ACP && cp != CP_OEMCP) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return 0;
    }
    const int n = narrow_len(src, src_len);
    const auto* p = reinterpret_cast<const unsigned char*>(src);
    const auto* end = p + n;
    if (!dst || dst_len == 0) {
        int count = 0;
        while (p < end) {
            if (*p == 0 && src_len < 0) {
                ++count;
                break;
            }
            utf8_next(p, end);
            ++count;
        }
        return count;
    }
    int written = 0;
    while (p < end && written < dst_len) {
        if (*p == 0 && src_len < 0) {
            dst[written++] = 0;
            break;
        }
        dst[written++] = static_cast<wchar_t>(utf8_next(p, end));
    }
    return written;
}

int WideCharToMultiByte(UINT cp, DWORD, LPCWSTR src, int src_len, LPSTR dst, int dst_len, LPCSTR, LPBOOL) {
    if (!src) return 0;
    if (cp != CP_UTF8 && cp != CP_ACP && cp != CP_OEMCP) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return 0;
    }
    const int n = wide_len(src, src_len);
    if (!dst || dst_len == 0) {
        int count = 0;
        char tmp[4];
        for (int i = 0; i < n; i++) {
            const wchar_t c = src[i];
            if (c == 0 && src_len < 0) {
                ++count;
                break;
            }
            count += static_cast<int>(utf8_put(static_cast<uint32_t>(c), tmp));
        }
        return count;
    }
    int written = 0;
    for (int i = 0; i < n && written < dst_len; i++) {
        const wchar_t c = src[i];
        if (c == 0 && src_len < 0) {
            dst[written++] = 0;
            break;
        }
        char tmp[4];
        const size_t k = utf8_put(static_cast<uint32_t>(c), tmp);
        if (written + static_cast<int>(k) > dst_len) break;
        for (size_t j = 0; j < k; j++) dst[written++] = tmp[j];
    }
    return written;
}

int lstrlenW(LPCWSTR s) {
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

int lstrlenA(LPCSTR s) {
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

LPWSTR lstrcpyW(LPWSTR dst, LPCWSTR src) {
    LPWSTR d = dst;
    while (src && (*d++ = *src++)) {
    }
    return dst;
}

int lstrcmpiW(LPCWSTR a, LPCWSTR b) {
    if (!a || !b) return a ? 1 : (b ? -1 : 0);
    for (;; ++a, ++b) {
        const int x = ascii_lower(static_cast<int>(*a));
        const int y = ascii_lower(static_cast<int>(*b));
        if (x != y || x == 0) return x - y;
    }
}

int lstrcmpiA(LPCSTR a, LPCSTR b) {
    if (!a || !b) return a ? 1 : (b ? -1 : 0);
    for (;; ++a, ++b) {
        const int x = ascii_lower(static_cast<unsigned char>(*a));
        const int y = ascii_lower(static_cast<unsigned char>(*b));
        if (x != y || x == 0) return x - y;
    }
}

LPWSTR CharUpperW(LPWSTR s) {
    for (LPWSTR p = s; p && *p; ++p)
        if (*p >= L'a' && *p <= L'z') *p = static_cast<wchar_t>(*p - 32);
    return s;
}

BOOL CompareStringOrdinal(LPCWSTR a, int a_len, LPCWSTR b, int b_len, BOOL ignore_case) {
    const int na = a_len < 0 ? lstrlenW(a) : a_len;
    const int nb = b_len < 0 ? lstrlenW(b) : b_len;
    const int n = std::min(na, nb);
    for (int i = 0; i < n; i++) {
        int x = static_cast<int>(a[i]), y = static_cast<int>(b[i]);
        if (ignore_case) {
            x = ascii_lower(x);
            y = ascii_lower(y);
        }
        if (x != y) return 1;   // CSTR_LESS_THAN
    }
    if (na == nb) return 2;     // CSTR_EQUAL
    return na < nb ? 1 : 3;
}

DWORD FormatMessageW(DWORD, LPCVOID, DWORD msg_id, DWORD, LPWSTR out, DWORD cap, void*) {
    if (!out || cap == 0) return 0;
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "error %u", static_cast<unsigned>(msg_id));
    const int w = MultiByteToWideChar(CP_UTF8, 0, buf, n, out, static_cast<int>(cap) - 1);
    if (w > 0) out[w] = 0;
    return static_cast<DWORD>(w);
}

void OutputDebugStringA(LPCSTR s) {
    if (!s) return;
    std::fputs(s, stderr);
    std::fflush(stderr);
}

void OutputDebugStringW(LPCWSTR s) {
    if (!s) return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return;
    std::string buf(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, buf.data(), n, nullptr, nullptr);
    std::fputs(buf.c_str(), stderr);
    std::fflush(stderr);
}

#if defined(__cplusplus)
std::string nxos_utf8(const char* s) { return s ? std::string(s) : std::string(); }
std::string nxos_utf8(const wchar_t* s) {
    if (!s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}
#endif

// ==================================================================================================================
// modules
// ==================================================================================================================
HMODULE nxos_load_library(const char* path, DWORD) {
    if (!path || !*path) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    std::string p = native_path(path);
    void* dl = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        // Win32 module names carry a .dll suffix; a POSIX build's plugins are .dylib/.so. Try the obvious rewrite
        // before giving up, so one configured name works on both platforms (temporal_ffx.cpp:90 loads "ffx_fsr3").
        std::string alt = p;
        const size_t dot = alt.find_last_of('.');
        const size_t slash = alt.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) alt.erase(dot);
#if defined(__APPLE__)
        alt += ".dylib";
#else
        alt += ".so";
#endif
        dl = dlopen(alt.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (dl) p = alt;
    }
    if (!dl) {
        t_last_error = ERROR_MOD_NOT_FOUND;
        return nullptr;
    }
    auto* m = new ModuleObj();
    m->dl = dl;
    m->path = p;
    return m;
}

FARPROC GetProcAddress(HMODULE mod, LPCSTR name) {
    if (!mod || !name) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    auto* m = as<ModuleObj>(mod, Kind::Module);
    if (!m || !m->dl) {
        t_last_error = ERROR_INVALID_HANDLE;
        return nullptr;
    }
    void* sym = dlsym(m->dl, name);
    if (!sym) t_last_error = ERROR_PROC_NOT_FOUND;
    return reinterpret_cast<FARPROC>(sym);
}

BOOL FreeLibrary(HMODULE mod) {
    auto* m = as<ModuleObj>(mod, Kind::Module);
    if (!m) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (m->dl && m != g_main_module) dlclose(m->dl);
    m->dl = nullptr;
    release(m);
    return TRUE;
}

HMODULE GetModuleHandleA(LPCSTR name) {
    if (!name || !*name) return main_module();
    // "ntdll.dll" and friends are probed for Wine detection (host_probe.cpp, nvn_gpu.cpp, sched.cpp): report absence.
    t_last_error = ERROR_MOD_NOT_FOUND;
    return nullptr;
}

BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out) {
    if (!out) return FALSE;
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(name)), &info) && info.dli_fname) {
            *out = main_module();
            return TRUE;
        }
        t_last_error = ERROR_MOD_NOT_FOUND;
        return FALSE;
    }
    HMODULE m = GetModuleHandleA(name);
    if (!m) return FALSE;
    *out = m;
    return TRUE;
}

DWORD nxos_get_module_file_name(HMODULE mod, char* out, DWORD cap) {
    if (!out || cap == 0) return 0;
    std::string path;
    if (!mod) {
        path = main_module()->path;
    } else {
        auto* m = as<ModuleObj>(mod, Kind::Module);
        if (!m) {
            t_last_error = ERROR_INVALID_HANDLE;
            return 0;
        }
        path = m->path;
    }
    if (path.empty()) {
#if defined(__APPLE__)
        char buf[4096];
        uint32_t n = static_cast<uint32_t>(sizeof buf);
        if (_NSGetExecutablePath(buf, &n) == 0) path = buf;
#else
        char buf[4096];
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            path = buf;
        }
#endif
    }
    const size_t n = std::min<size_t>(path.size(), cap - 1);
    std::memcpy(out, path.data(), n);
    out[n] = 0;
    return static_cast<DWORD>(n);
}

DWORD GetModuleFileNameA(HMODULE mod, LPSTR out, DWORD cap) { return nxos_get_module_file_name(mod, out, cap); }

// ==================================================================================================================
// registry: macOS has none. Every key reports "not found" so the callers take their fallback path.
// ==================================================================================================================
LONG RegOpenKeyExA(HKEY, LPCSTR, DWORD, DWORD, PHANDLE out) {
    if (out) *out = nullptr;
    return ERROR_FILE_NOT_FOUND;
}
LONG RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
LONG RegCloseKey(HKEY) { return ERROR_SUCCESS; }
LONG RegSetValueExA(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD) { return ERROR_SUCCESS; }
LONG RegCreateKeyExA(HKEY, LPCSTR, DWORD, LPSTR, DWORD, DWORD, void*, PHANDLE, LPDWORD) { return ERROR_SUCCESS; }

// ==================================================================================================================
// environment, process creation, console
// ==================================================================================================================
DWORD nxos_get_environment(const char* name, char* out, DWORD cap) {
    const char* v = name ? getenv(name) : nullptr;
    if (!v) {
        t_last_error = ERROR_ENVVAR_NOT_FOUND;
        return 0;
    }
    const size_t n = std::strlen(v);
    if (!out || cap == 0) return static_cast<DWORD>(n + 1);
    if (n + 1 > cap) {
        t_last_error = ERROR_INSUFFICIENT_BUFFER;
        return static_cast<DWORD>(n + 1);
    }
    std::memcpy(out, v, n + 1);
    return static_cast<DWORD>(n);
}

BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value) {
    if (!name) return FALSE;
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0 ? TRUE : FALSE;
}

DWORD ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD cap) {
    if (!src) return 0;
    std::string out;
    for (const char* p = src; *p;) {
        if (*p == '%') {
            const char* close = std::strchr(p + 1, '%');
            if (close) {
                const std::string name(p + 1, static_cast<size_t>(close - p - 1));
                if (const char* v = getenv(name.c_str())) out += v;
                p = close + 1;
                continue;
            }
        }
        out += *p++;
    }
    if (!dst || cap == 0) return static_cast<DWORD>(out.size() + 1);
    const size_t n = std::min<size_t>(out.size(), cap - 1);
    std::memcpy(dst, out.data(), n);
    dst[n] = 0;
    return static_cast<DWORD>(n);
}

BOOL CreateProcessW(LPCWSTR, LPWSTR, void*, void*, BOOL, DWORD, void*, LPCWSTR, STARTUPINFOW*, PROCESS_INFORMATION* pi) {
    // The one caller (host_device.cpp) probes a Steam Deck helper; there is none on macOS.
    if (pi) std::memset(pi, 0, sizeof *pi);
    t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
    return FALSE;
}

BOOL GetComputerNameA(LPSTR out, LPDWORD cap) {
    char buf[256] = {0};
    if (gethostname(buf, sizeof buf - 1) != 0) return FALSE;
    const size_t n = std::strlen(buf);
    if (!out || !cap || *cap < n + 1) {
        if (cap) *cap = static_cast<DWORD>(n + 1);
        return FALSE;
    }
    std::memcpy(out, buf, n + 1);
    *cap = static_cast<DWORD>(n);
    return TRUE;
}

BOOL GetUserNameA(LPSTR out, LPDWORD cap) {
    const char* u = getenv("USER");
    if (!u) u = getenv("LOGNAME");
    if (!u) return FALSE;
    const size_t n = std::strlen(u);
    if (!out || !cap || *cap < n + 1) {
        if (cap) *cap = static_cast<DWORD>(n + 1);
        return FALSE;
    }
    std::memcpy(out, u, n + 1);
    *cap = static_cast<DWORD>(n);
    return TRUE;
}

HANDLE GetStdHandle(DWORD which) {
    int fd = STDIN_FILENO;
    if (which == STD_OUTPUT_HANDLE) fd = STDOUT_FILENO;
    else if (which == STD_ERROR_HANDLE) fd = STDERR_FILENO;
    const int dup_fd = ::dup(fd);
    return dup_fd >= 0 ? new FileObj(dup_fd) : nullptr;
}

BOOL GetConsoleMode(HANDLE, PDWORD mode) {
    if (mode) *mode = 0;
    return FALSE;
}

BOOL SetConsoleMode(HANDLE, DWORD) { return FALSE; }

int MessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT) {
    const std::string t = nxos_utf8(text), c = nxos_utf8(caption);
    std::fprintf(stderr, "[%s] %s\n", c.c_str(), t.c_str());
    std::fflush(stderr);
    return 1;   // IDOK
}

int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT) {
    std::fprintf(stderr, "[%s] %s\n", caption ? caption : "", text ? text : "");
    std::fflush(stderr);
    return 1;   // IDOK
}

// ==================================================================================================================
// COM: none. The launcher, the Media Foundation movie path and the XAudio2 sink are Windows-only and not built.
// ==================================================================================================================
HRESULT CoInitializeEx(void*, DWORD) { return S_OK; }
void CoUninitialize(void) {}
HRESULT CoCreateInstance(REFCLSID, void*, DWORD, REFIID, void** out) {
    if (out) *out = nullptr;
    return E_NOINTERFACE;
}

// ==================================================================================================================
// diagnostics
// ==================================================================================================================
BOOL IsDebuggerPresent(void) {
#if defined(__APPLE__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(getpid())};
    struct kinfo_proc info{};
    size_t size = sizeof info;
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) return FALSE;
    return (info.kp_proc.p_flag & P_TRACED) ? TRUE : FALSE;
#elif defined(__linux__)
    // /proc/self/status "TracerPid": non-zero while a debugger is attached.
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char line[256];
    BOOL traced = FALSE;
    while (std::fgets(line, sizeof line, f)) {
        long pid = 0;
        if (std::sscanf(line, "TracerPid: %ld", &pid) == 1) {
            traced = pid != 0 ? TRUE : FALSE;
            break;
        }
    }
    std::fclose(f);
    return traced;
#else
    return FALSE;
#endif
}

void DebugBreak(void) { __builtin_trap(); }

BOOL QueryThreadCycleTime(HANDLE h, PULONGLONG cycles) {
    if (!cycles) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    ThreadObj* t = (h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread))) ? self_thread()
                                                                                        : as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (t->th == pthread_self()) {
        struct timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        *cycles = static_cast<ULONGLONG>(ts.tv_sec) * 1000000000ull + static_cast<ULONGLONG>(ts.tv_nsec);
        return TRUE;
    }
    *cycles = t->cpu_ns;   // written when the thread exits; a running thread reports its last known value
    return TRUE;
}

BOOL GetThreadTimes(HANDLE, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel_t, LPFILETIME user_t) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    const uint64_t cpu = static_cast<uint64_t>(ts.tv_sec) * 10000000ull + static_cast<uint64_t>(ts.tv_nsec) / 100ull;
    if (created) set_filetime(created, 0);
    if (exited) set_filetime(exited, 0);
    if (kernel_t) set_filetime(kernel_t, 0);
    if (user_t) set_filetime(user_t, cpu);
    return TRUE;
}

BOOL GetProcessTimes(HANDLE, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel_t, LPFILETIME user_t) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (created) set_filetime(created, filetime_of(ts));
    if (exited) set_filetime(exited, 0);
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        if (kernel_t)
            set_filetime(kernel_t, static_cast<uint64_t>(ru.ru_stime.tv_sec) * 10000000ull +
                                       static_cast<uint64_t>(ru.ru_stime.tv_usec) * 10ull);
        if (user_t)
            set_filetime(user_t, static_cast<uint64_t>(ru.ru_utime.tv_sec) * 10000000ull +
                                     static_cast<uint64_t>(ru.ru_utime.tv_usec) * 10ull);
    }
    return TRUE;
}

BOOL GetProcessMemoryInfo(HANDLE, PROCESS_MEMORY_COUNTERS* pmc, DWORD) {
    if (!pmc) return FALSE;
#if defined(__APPLE__)
    // phys_footprint is what the OS charges the process (Activity Monitor's "Memory"): resident pages plus compressed and the
    // IOKit/Metal memory the GPU driver holds for it. getrusage's ru_maxrss is a *peak* of resident pages only, so a guard
    // built on it sees neither the current size nor the video memory -- on a 16 GiB host the pools alone are ~5 GiB.
    task_vm_info_data_t vi{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vi), &count) == KERN_SUCCESS) {
        const SIZE_T fp = static_cast<SIZE_T>(vi.phys_footprint);
        pmc->WorkingSetSize = static_cast<SIZE_T>(vi.resident_size);
        pmc->PeakWorkingSetSize = static_cast<SIZE_T>(vi.resident_size_peak);
        pmc->PagefileUsage = fp;
        pmc->PeakPagefileUsage = fp;   // task_vm_info has no peak footprint; memguard keeps its own peak
        if (pmc->cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX))
            static_cast<PROCESS_MEMORY_COUNTERS_EX*>(static_cast<void*>(pmc))->PrivateUsage = fp;
        return TRUE;
    }
#elif defined(__linux__)
    // /proc/self/status has both numbers the guard needs: VmRSS (now) and VmHWM (the peak). getrusage's ru_maxrss is the
    // peak only, so a guard built on it would report a high-water mark that never comes back down.
    if (std::FILE* f = std::fopen("/proc/self/status", "r")) {
        SIZE_T rss = 0, peak = 0;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            unsigned long long kb = 0;
            if (std::sscanf(line, "VmRSS: %llu kB", &kb) == 1) rss = static_cast<SIZE_T>(kb) * 1024;
            else if (std::sscanf(line, "VmHWM: %llu kB", &kb) == 1) peak = static_cast<SIZE_T>(kb) * 1024;
        }
        std::fclose(f);
        if (rss || peak) {
            if (!peak) peak = rss;
            pmc->WorkingSetSize = rss;
            pmc->PeakWorkingSetSize = peak;
            pmc->PagefileUsage = rss;
            pmc->PeakPagefileUsage = peak;
            if (pmc->cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX))
                static_cast<PROCESS_MEMORY_COUNTERS_EX*>(static_cast<void*>(pmc))->PrivateUsage = rss;
            return TRUE;
        }
    }
#endif
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const SIZE_T rss = static_cast<SIZE_T>(ru.ru_maxrss);   // bytes on Darwin, KiB on Linux
#if defined(__APPLE__)
    const SIZE_T rss_bytes = rss;
#else
    const SIZE_T rss_bytes = rss * 1024;
#endif
    pmc->WorkingSetSize = rss_bytes;
    pmc->PeakWorkingSetSize = rss_bytes;
    pmc->PagefileUsage = rss_bytes;
    pmc->PeakPagefileUsage = rss_bytes;
    if (pmc->cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) {
        static_cast<PROCESS_MEMORY_COUNTERS_EX*>(static_cast<void*>(pmc))->PrivateUsage = rss_bytes;
    }
    return TRUE;
}

DWORD GetGuiResources(HANDLE, DWORD) { return 0; }

// ==================================================================================================================
// thread snapshots (diag/gauge.cpp counts threads)
// ==================================================================================================================
HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD) {
    if (!(flags & TH32CS_SNAPTHREAD)) {
        t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
        return nullptr;
    }
    auto* s = new SnapshotObj();
    s->tids.push_back(self_tid());
    return s;
}

BOOL Thread32First(HANDLE h, PTHREADENTRY32 te) {
    auto* s = as<SnapshotObj>(h, Kind::Snapshot);
    if (!s || !te) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    s->at = 0;
    te->dwSize = sizeof(THREADENTRY32);
    te->th32OwnerProcessID = GetCurrentProcessId();
    te->th32ThreadID = s->tids[0];
    return TRUE;
}

BOOL Thread32Next(HANDLE h, PTHREADENTRY32 te) {
    auto* s = as<SnapshotObj>(h, Kind::Snapshot);
    if (!s || !te) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (++s->at >= s->tids.size()) {
        t_last_error = ERROR_NO_MORE_FILES;
        return FALSE;
    }
    te->th32ThreadID = s->tids[s->at];
    return TRUE;
}

// <avrt.h> declares these inside extern "C": the definitions must match, or the call sites link to C++-mangled names.
extern "C" {
HANDLE AvSetMmThreadCharacteristicsW(LPCWSTR, LPDWORD index) {
    // No MMCSS on macOS: the caller (services/audren/Integration/ThreadBoost.h) falls back to SetThreadPriority, which
    // maps to a QoS class and is what actually raises an audio thread's priority here.
    if (index) *index = 0;
    t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
    return nullptr;
}

HANDLE AvSetMmThreadCharacteristicsA(LPCSTR, LPDWORD index) { return AvSetMmThreadCharacteristicsW(nullptr, index); }
BOOL AvRevertMmThreadCharacteristics(HANDLE) { return TRUE; }
BOOL AvSetMmThreadPriority(HANDLE, int) { return TRUE; }
}

HINSTANCE ShellExecuteA(HWND, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, int) {
    (void)verb;
    (void)params;
    (void)dir;
    // src/host_device.cpp hands a URL to the Steam keyboard; there is no shell to hand it to here, and the caller
    // logs a warning and carries on. Returning 0 is Win32's "the call failed".
    if (file) t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
    return nullptr;
}

BOOL ShellExecuteExA(SHELLEXECUTEINFOA* info) {
    // Opening a URL or a folder through the shell. macOS has "open", but the only caller is the Steam Deck helper path
    // in src/host_device.cpp, which does not exist here: report failure so the caller logs and moves on.
    if (info) info->hProcess = nullptr;
    t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
    return FALSE;
}

// ==================================================================================================================
// processor topology and the odds and ends the diag layer reaches for
// ==================================================================================================================
BOOL GetThreadIdealProcessorEx(HANDLE, PPROCESSOR_NUMBER out) {
    // There is no "ideal processor" to read back on macOS: the scheduler decides. A caller that only wants a label
    // gets failure and prints nothing, which is better than a fabricated number in a diagnostics line.
    if (out) std::memset(out, 0, sizeof *out);
    t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
    return FALSE;
}

BOOL GetThreadContext(HANDLE h, LPCONTEXT ctx) {
    if (!ctx) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    std::memset(ctx, 0, sizeof *ctx);
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
#if defined(__APPLE__) && defined(__aarch64__)
    // The caller suspended the thread (SuspendThread): thread_get_state fails on a running one.
    const mach_port_t mt = mach_thread_of(t);
    if (mt == MACH_PORT_NULL) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    arm_thread_state64_t st{};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(mt, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&st), &count) != KERN_SUCCESS) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    for (int i = 0; i < 29; i++) ctx->x[i] = st.__x[i];
    ctx->fp = st.__fp;
    ctx->lr = st.__lr;
    ctx->sp = st.__sp;
    ctx->pc = st.__pc;
    ctx->Cpsr = st.__cpsr;
    ctx->ContextFlags = 0x00100007u;   // CONTEXT_ARM64 | CONTROL | INTEGER | FLOATING_POINT: the whole file is there
    return TRUE;
#else
    t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
    return FALSE;
#endif
}

BOOL SetThreadContext(HANDLE, const CONTEXT*) {
    t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
    return FALSE;
}

DWORD GetCurrentProcessorNumber(void) {
#if defined(__APPLE__)
    // thread_info(THREAD_BASIC_INFO) carries cpu_usage, not the cpu id; the portable answer is the mach thread's
    // last run-on processor through thread_get_assignment, which needs a thread port. Report 0 (the only CPU group).
    return 0;
#else
    const int cpu = sched_getcpu();
    return cpu > 0 ? static_cast<DWORD>(cpu) : 0;
#endif
}

DWORD GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER out) {
    if (out) {
        out->Group = 0;
        out->Number = static_cast<BYTE>(GetCurrentProcessorNumber());
        out->Reserved = 0;
    }
    return GetCurrentProcessorNumber();
}

BOOL SetThreadStackGuarantee(PULONG guarantee) {
    // The reserved stack the exception handlers may use on Windows. pthread stacks have no equivalent reservation, and
    // the crash path here runs on a BSD signal handler with its own altstack, so report success and leave the value.
    if (guarantee) *guarantee = 0;
    return TRUE;
}

BOOL IsBadReadPtr(LPCVOID addr, UINT_PTR size) {
    // kernel/svc_thread.cpp validates a guest stack pointer before walking it: walk the page table the same way
    // ReadProcessMemory does, but without copying.
    if (!addr) return TRUE;
    uintptr_t page = reinterpret_cast<uintptr_t>(addr);
    const uintptr_t end = page + (size ? size : 1);
    while (page < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(page), &mbi, sizeof mbi) != sizeof mbi) return TRUE;
        if (mbi.State != MEM_COMMIT) return TRUE;
        const DWORD p = mbi.Protect & 0xFF;
        if (p == PAGE_NOACCESS || p == 0) return TRUE;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= page) return TRUE;
        page = next;
    }
    return FALSE;
}

BOOL IsBadWritePtr(LPVOID addr, UINT_PTR size) {
    if (!addr) return TRUE;
    uintptr_t page = reinterpret_cast<uintptr_t>(addr);
    const uintptr_t end = page + (size ? size : 1);
    while (page < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(page), &mbi, sizeof mbi) != sizeof mbi) return TRUE;
        if (mbi.State != MEM_COMMIT) return TRUE;
        const DWORD p = mbi.Protect & 0xFF;
        if (p != PAGE_READWRITE && p != PAGE_WRITECOPY && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY)
            return TRUE;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= page) return TRUE;
        page = next;
    }
    return FALSE;
}

// The host's cores, one entry per core, in the shape kernel/sched.cpp reads. Windows groups processors above 64 per
// group; the shim keeps one group, which is what a Mac has.
BOOL GetLogicalProcessorInformationEx(int relationship, SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* buffer, PDWORD len) {
    if (!len) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    if (relationship != RelationProcessorCore) {
        *len = 0;
        t_last_error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores <= 0) cores = 1;
    if (cores > 64) cores = 64;   // one GROUP_AFFINITY word

    const DWORD entry = sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
    const DWORD need = entry * static_cast<DWORD>(cores);
    if (!buffer || *len < need) {
        *len = need;
        t_last_error = ERROR_INSUFFICIENT_BUFFER;
        return FALSE;
    }
    // macOS numbers the performance cores first (hw.perflevel0) and the efficiency cores after them (hw.perflevel1),
    // which is the order kernel/sched.cpp wants: a higher EfficiencyClass is a faster core.
    long performance = 0;
#if defined(__APPLE__)
    size_t perf_len = sizeof performance;
    if (sysctlbyname("hw.perflevel0.logicalcpu", &performance, &perf_len, nullptr, 0) != 0) performance = 0;
    if (performance <= 0 || performance > cores) performance = cores;
#endif
    auto* out = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer);
    for (long i = 0; i < cores; ++i) {
        std::memset(out, 0, entry);
        out->Relationship = RelationProcessorCore;
        out->Size = entry;
        out->Processor.EfficiencyClass = i < performance ? 1 : 0;
        out->Processor.GroupCount = 1;
        out->Processor.GroupMask[0].Mask = static_cast<DWORD_PTR>(1) << i;
        out = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(reinterpret_cast<unsigned char*>(out) + entry);
    }
    *len = need;
    return TRUE;
}

DWORD SetThreadIdealProcessorEx(HANDLE, const PROCESSOR_NUMBER* ideal, PDWORD previous) {
    // The hint is advisory on Windows and absent on macOS: the scheduler decides. Report the current processor as both
    // the previous and the new ideal, which is what the caller (kernel/sched.cpp) treats as "no change".
    const DWORD current = GetCurrentProcessorNumber();
    if (previous) *previous = current;
    (void)ideal;
    return current;
}

LANGID GetUserDefaultUILanguage(void) {
    // The UI language, from the locale the process was started with. services/common.cpp only asks which language the
    // system menu strings should be in, and the locale is what decides that on macOS.
    const char* lang = getenv("LC_ALL");
    if (!lang || !*lang) lang = getenv("LC_MESSAGES");
    if (!lang || !*lang) lang = getenv("LANG");
    if (!lang) return MAKELANGID(LANG_ENGLISH, 0);
    const auto starts = [&](const char* p) { return std::strncmp(lang, p, std::strlen(p)) == 0; };
    if (starts("ja")) return MAKELANGID(LANG_JAPANESE, 0);
    if (starts("fr")) return MAKELANGID(LANG_FRENCH, 0);
    if (starts("de")) return MAKELANGID(LANG_GERMAN, 0);
    if (starts("it")) return MAKELANGID(LANG_ITALIAN, 0);
    if (starts("es")) return MAKELANGID(LANG_SPANISH, 0);
    if (starts("ko")) return MAKELANGID(LANG_KOREAN, 0);
    if (starts("zh")) return MAKELANGID(LANG_CHINESE, 0);
    if (starts("pt")) return MAKELANGID(LANG_PORTUGUESE, 0);
    if (starts("ru")) return MAKELANGID(LANG_RUSSIAN, 0);
    if (starts("nl")) return MAKELANGID(LANG_DUTCH, 0);
    if (starts("pl")) return MAKELANGID(LANG_POLISH, 0);
    return MAKELANGID(LANG_ENGLISH, 0);
}

LANGID GetUserDefaultLangID(void) { return GetUserDefaultUILanguage(); }
LANGID GetSystemDefaultUILanguage(void) { return GetUserDefaultUILanguage(); }

void GetSystemTimePreciseAsFileTime(LPFILETIME ft) { GetSystemTimeAsFileTime(ft); }

FILE* nxos_wfopen(const wchar_t* path, const char* mode) {
    if (!path || !mode) return nullptr;
    const std::string p = nxos_utf8(path);
    return std::fopen(p.c_str(), mode);
}

FILE* nxos_wfopen(const char* path, const char* mode) { return path && mode ? std::fopen(path, mode) : nullptr; }
FILE* nxos_wfreopen(const char* path, const char* mode, FILE* stream) {
    return path && mode ? std::freopen(path, mode, stream) : nullptr;
}
FILE* nxos_wfopen(const char* path, const wchar_t* mode) {
    const std::string m = nxos_utf8(mode);
    return nxos_wfopen(path, m.c_str());
}
FILE* nxos_wfreopen(const char* path, const wchar_t* mode, FILE* stream) {
    const std::string m = nxos_utf8(mode);
    return nxos_wfreopen(path, m.c_str(), stream);
}
FILE* nxos_wfopen(const wchar_t* path, const wchar_t* mode) {
    const std::string m = nxos_utf8(mode);
    return nxos_wfopen(path, m.c_str());
}
FILE* nxos_wfreopen(const wchar_t* path, const wchar_t* mode, FILE* stream) {
    const std::string m = nxos_utf8(mode);
    return nxos_wfreopen(path, m.c_str(), stream);
}

FILE* nxos_wfreopen(const wchar_t* path, const char* mode, FILE* stream) {
    if (!path || !mode) return nullptr;
    const std::string p = nxos_utf8(path);
    return std::freopen(p.c_str(), mode, stream);
}

// ==================================================================================================================
// structured exceptions: BSD signals, not mach ports
//
// A BSD signal handler runs on the faulting thread and returning from it resumes the faulting instruction, which is
// what write_watch.cpp needs (un-protect the page, return, the store retries). A mach exception port would hand the
// fault to a helper thread and need thread_set_state to resume: more machinery for less. The CONTEXT is built from
// the ucontext_t register file, so a crash report carries the real pc/lr/sp/fp.
// ==================================================================================================================
namespace {

// The vectored handlers, in a fixed table. The signal handler runs on the faulting thread and MUST NOT allocate or take a
// lock: the first version copied a std::vector under a std::mutex, the allocation itself faulted (a guarded memcpy over a
// decommitted page), and the nested handler deadlocked on the mutex it already held. Registration is rare and takes the
// mutex; readers only load the published count (release) and the slots.
constexpr int kVehMax = 32;
std::atomic<PVECTORED_EXCEPTION_HANDLER> g_veh[kVehMax] = {};
std::atomic<int> g_veh_count{0};
std::atomic<uintptr_t> g_fault_pthread{0};   // pthread_self() of the thread that faulted, for the report's stack bounds
std::atomic<uintptr_t> g_image_base{0};      // the main image's load address, so a fault pc can be turned into an RVA
std::mutex g_veh_mu;   // registration only: never taken by the signal handler
LPTOP_LEVEL_EXCEPTION_FILTER g_top_filter = nullptr;
std::atomic<bool> g_signals_installed{false};

// The __try/__except stand-in (nx_os.h): a fault inside nxos_guarded_call jumps back to its sigsetjmp instead of
// reaching the top-level filter. One frame per nesting level, per thread.
struct GuardFrame {
    sigjmp_buf env;
    GuardFrame* prev;
};
thread_local GuardFrame* t_guard = nullptr;
// The name the runtime gave this thread (log_set_thread_name), read by the fault note and the report. A thread-local pointer,
// not pthread_getname_np: the handler may run on an exhausted stack, where a libc call can trap.
thread_local const char* t_thread_name = nullptr;

// The last faults, in a fixed ring written without locks or allocation from the signal handler. The report prints them: the
// fatal fault alone does not say whether it was the first, nor whether a guard was armed at the time.
struct FaultRec {
    uint64_t pc;
    uint64_t sp;
    uint64_t guard_ptr;   // t_guard at the time of the fault
    uint64_t slot;        // &t_guard, to tell a TLS mix-up from a plain null
    const char* name;     // the thread's name (t_thread_name)
    uint64_t tp_ro;       // tpidrro_el0 at the fault: the thread pointer the TLS access uses
    uint64_t tp;
    uint32_t sig;
    uint32_t guard;
};
FaultRec g_fault_ring[8] = {};
std::atomic<uint32_t> g_fault_seq{0};

void record_fault(int sig, uint64_t pc, uint64_t sp, bool guard) {
    const uint32_t i = g_fault_seq.fetch_add(1, std::memory_order_relaxed);
    uint64_t tp_ro = 0, tp = 0;
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp_ro));
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
#elif defined(__x86_64__)
    // x86-64 has a single thread pointer: %fs:0 is the TCB's own address, which is what a TLS access adds to. Both fields carry
    // it so the record keeps its shape (the Apple report reads them side by side).
    __asm__ volatile("movq %%fs:0, %0" : "=r"(tp));
    tp_ro = tp;
#endif
    g_fault_ring[i % 8] = FaultRec{pc, sp, reinterpret_cast<uint64_t>(t_guard), reinterpret_cast<uint64_t>(&t_guard),
                                   t_thread_name, tp_ro, tp, static_cast<uint32_t>(sig), guard ? 1u : 0u};
}

DWORD code_of_signal(int sig) {
    switch (sig) {
        case SIGSEGV: return EXCEPTION_ACCESS_VIOLATION;
        case SIGBUS: return EXCEPTION_IN_PAGE_ERROR;
        case SIGILL: return EXCEPTION_ILLEGAL_INSTRUCTION;
        case SIGFPE: return EXCEPTION_PRIV_INSTRUCTION;
        case SIGTRAP: return EXCEPTION_BREAKPOINT;
        case SIGABRT: return 0xE0000004u;   // "ABORT": the guest's abort()/std::terminate, reported like a fault
        default: return EXCEPTION_ACCESS_VIOLATION;
    }
}

void fill_context(CONTEXT* cx, void* uctx) {
    std::memset(cx, 0, sizeof *cx);
    auto* uc = static_cast<ucontext_t*>(uctx);
#if defined(__APPLE__) && defined(__aarch64__)
    const auto& ss = uc->uc_mcontext->__ss;
    for (int i = 0; i < 29; i++) cx->x[i] = ss.__x[i];
    cx->fp = ss.__fp;
    cx->lr = ss.__lr;
    cx->sp = ss.__sp;
    cx->pc = ss.__pc;
    cx->Cpsr = ss.__cpsr;
#elif defined(__x86_64__)
    const auto& g = uc->uc_mcontext.gregs;
    cx->pc = static_cast<ULONGLONG>(g[REG_RIP]);
    cx->sp = static_cast<ULONGLONG>(g[REG_RSP]);
    cx->fp = static_cast<ULONGLONG>(g[REG_RBP]);
#elif defined(__aarch64__)
    for (int i = 0; i < 29; i++) cx->x[i] = uc->uc_mcontext.regs[i];
    cx->sp = uc->uc_mcontext.sp;
    cx->pc = uc->uc_mcontext.pc;
#else
    (void)uc;
#endif
}

// Runs the registered handlers in order. No allocation and no lock: safe from the signal handler. True when a handler
// continued execution (the faulting instruction is resumed).
bool run_veh(EXCEPTION_POINTERS* ep) {
    const int n = std::min<int>(g_veh_count.load(std::memory_order_acquire), kVehMax);
    for (int i = 0; i < n; ++i) {
        PVECTORED_EXCEPTION_HANDLER h = g_veh[i].load(std::memory_order_acquire);
        if (h && h(ep) == EXCEPTION_CONTINUE_EXECUTION) return true;
    }
    return false;
}

// One line to fd 2 from the signal handler: allocation-free, no locks, no stdio (the crash report below allocates, and if it
// faults too the original fault would leave no trace).
void note_fault(int sig, void* addr, uint64_t pc, uint64_t sp) {
    char b[160];
    size_t n = 0;
    auto put = [&](const char* s) {
        while (*s && n < sizeof b - 1) b[n++] = *s++;
    };
    auto hex = [&](uint64_t v) {
        put("0x");
        char t[16];
        int k = 0;
        do {
            t[k++] = "0123456789abcdef"[v & 15u];
            v >>= 4;
        } while (v);
        while (k && n < sizeof b - 1) b[n++] = t[--k];
    };
    auto dec = [&](int v) {
        char t[12];
        int k = 0;
        if (v <= 0) {
            put("0");
            return;
        }
        while (v > 0) {
            t[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        }
        while (k && n < sizeof b - 1) b[n++] = t[--k];
    };
    put("nxrt: unhandled signal ");
    dec(sig);
    put(" at ");
    hex(reinterpret_cast<uintptr_t>(addr));
    put(", pc ");
    hex(pc);
    put(", sp ");
    hex(sp);
    put(" on ");
    put(t_thread_name && *t_thread_name ? t_thread_name : "(unnamed)");
    put(", base ");
    hex(g_image_base.load(std::memory_order_relaxed));
    put("\n");
    (void)::write(2, b, n);
}

void handler(int sig, siginfo_t* info, void* uctx) {
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = code_of_signal(sig);
    rec.ExceptionAddress = info ? info->si_addr : nullptr;
    // The Win32 access parameters the vectored handlers read (mem::ww::on_fault, the GPU guards): [0] 1 = write (a fault on a
    // page this process made read-only is a write; Darwin reports protection faults as SIGBUS and gives no direction), [1] the
    // address. Without them every handler saw NumberParameters 0 and let the fault through (write-watch, 2026-11-10).
    if (sig == SIGSEGV || sig == SIGBUS) {
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = 1;
        rec.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(info ? info->si_addr : nullptr);
    }
    CONTEXT cx;
    fill_context(&cx, uctx);
    EXCEPTION_POINTERS ep{&rec, &cx};
    record_fault(sig, cx.pc, cx.sp, t_guard != nullptr);

    // SIGILL and SIGTRAP are never part of the guarded-call machinery (write watch, GPU page faults), so note them before
    // anything can swallow them: an illegal instruction that a vectored handler continues from would otherwise leave no trace.
    if (sig == SIGILL || sig == SIGTRAP) note_fault(sig, info ? info->si_addr : nullptr, cx.pc, cx.sp);
    if (run_veh(&ep)) return;   // resumes the faulting instruction
    // A guarded call is the SEH frame: on Windows the frame handler runs after the vectored handlers, so this does too.
    if (t_guard) siglongjmp(t_guard->env, 1);
    // Nothing handled it. Say where, with write(2) and hand-rolled hex: the crash report below allocates, and a fault inside
    // it (or a second one on another thread) would otherwise leave no trace of the original one.
    note_fault(sig, info ? info->si_addr : nullptr, cx.pc, cx.sp);
    if (g_top_filter && g_top_filter(&ep) != EXCEPTION_CONTINUE_SEARCH) return;
    // Nothing handled it: restore the default action and return, so the fault kills the process and the kernel writes
    // a core instead of the thread spinning on a re-faulting instruction.
    signal(sig, SIG_DFL);
}

void install_signals() {
    if (g_signals_installed.exchange(true)) return;
    nxos_install_altstack();   // the main thread; every other thread installs its own when it starts
    // Bind every libc stub the signal path calls, now, on a normal stack. The first call through an unbound PLT stub goes to
    // dyld's binder, and a binder call made from inside the handler (write/pthread_self in note_fault, siglongjmp for a
    // guarded copy) faults again and recurses through the handler until the altstack is gone: that was the nvn-pool-land
    // SIGILL with the handler/note_fault stack on 2026-11-10.
    {
        // Volatile sinks: the compiler drops calls whose result is unused (it did), and the stubs stayed unbound.
        static volatile intptr_t bind_sink = 0;
        // pthread_t is a pointer on Darwin and an integer on glibc: through uintptr_t both are one cast away.
        bind_sink = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(::pthread_self()));
        bind_sink = static_cast<intptr_t>(::write(-1, "x", 1));   // fd -1: fails with EBADF, binds the stub
        sigjmp_buf jb;
        const int jumped = sigsetjmp(jb, 1);
        bind_sink = jumped;
        if (!jumped) siglongjmp(jb, 1);
    }
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    // SIGABRT too: the guest's abort()/std::terminate is where a failed init ends, and without it the run just stops.
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT}) sigaction(sig, &sa, nullptr);
}

}  // namespace

extern "C" uintptr_t nxos_fault_pthread() { return g_fault_pthread.load(std::memory_order_relaxed); }

extern "C" int nxos_recent_faults(uint64_t* pcs, uint64_t* sps, uint32_t* sigs, uint32_t* guards, int max) {
    const uint32_t seq = g_fault_seq.load(std::memory_order_relaxed);
    const int n = static_cast<int>(seq < 8 ? seq : 8);
    const int k = n < max ? n : max;
    for (int i = 0; i < k; ++i) {
        const FaultRec& r = g_fault_ring[(seq - static_cast<uint32_t>(k) + static_cast<uint32_t>(i)) % 8];
        pcs[i] = r.pc;
        sps[i] = r.sp;
        sigs[i] = r.sig;
        guards[i] = r.guard;
    }
    return k;
}

extern "C" int nxos_recent_fault_details(uint64_t* guard_ptrs, uint64_t* slots, const char** names, uint64_t* tps, int max) {
    const uint32_t seq = g_fault_seq.load(std::memory_order_relaxed);
    const int n = static_cast<int>(seq < 8 ? seq : 8);
    const int k = n < max ? n : max;
    for (int i = 0; i < k; ++i) {
        const FaultRec& r = g_fault_ring[(seq - static_cast<uint32_t>(k) + static_cast<uint32_t>(i)) % 8];
        guard_ptrs[i] = r.guard_ptr;
        slots[i] = r.slot;
        names[i] = r.name;
        if (tps) { tps[2 * i] = r.tp_ro; tps[2 * i + 1] = r.tp; }
    }
    return k;
}

namespace {
// The main image's load address, once, before any thread exists: the note line can then print a fault pc as an RVA too.
struct ImageBaseInit {
    ImageBaseInit() {
        Dl_info dl{};
        if (dladdr(reinterpret_cast<void*>(&nxos_fault_pthread), &dl) && dl.dli_fbase)
            g_image_base.store(reinterpret_cast<uintptr_t>(dl.dli_fbase), std::memory_order_relaxed);
    }
} g_image_base_init;
}  // namespace

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler_fn) {
    if (!handler_fn) return nullptr;
    install_signals();
    std::lock_guard<std::mutex> g(g_veh_mu);
    const int n = g_veh_count.load(std::memory_order_relaxed);
    if (n >= kVehMax) {
        std::fprintf(stderr, "nxos: AddVectoredExceptionHandler: the table is full (%d handlers)\n", kVehMax);
        return nullptr;
    }
    if (first) {
        for (int i = n; i > 0; --i) g_veh[i].store(g_veh[i - 1].load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_veh[0].store(handler_fn, std::memory_order_relaxed);
    } else {
        g_veh[n].store(handler_fn, std::memory_order_relaxed);
    }
    g_veh_count.store(n + 1, std::memory_order_release);
    return reinterpret_cast<PVOID>(handler_fn);
}

ULONG RemoveVectoredExceptionHandler(PVOID handle) {
    std::lock_guard<std::mutex> g(g_veh_mu);
    const int n = g_veh_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        if (g_veh[i].load(std::memory_order_relaxed) == reinterpret_cast<PVECTORED_EXCEPTION_HANDLER>(handle)) {
            for (int k = i; k + 1 < n; ++k) g_veh[k].store(g_veh[k + 1].load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_veh[n - 1].store(nullptr, std::memory_order_relaxed);
            g_veh_count.store(n - 1, std::memory_order_release);
            return 1;
        }
    }
    return 0;
}

LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    install_signals();
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_top_filter;
    g_top_filter = filter;
    return prev;
}

void RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args) {
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags;
    rec.ExceptionAddress = __builtin_return_address(0);
    rec.NumberParameters = std::min<DWORD>(nargs, EXCEPTION_MAXIMUM_PARAMETERS);
    for (DWORD i = 0; i < rec.NumberParameters; i++) rec.ExceptionInformation[i] = args ? args[i] : 0;
    CONTEXT cx;
    std::memset(&cx, 0, sizeof cx);
    cx.pc = reinterpret_cast<ULONGLONG>(__builtin_return_address(0));
    cx.sp = reinterpret_cast<ULONGLONG>(__builtin_frame_address(0));
    EXCEPTION_POINTERS ep{&rec, &cx};

    if (run_veh(&ep)) return;
    if (t_guard) siglongjmp(t_guard->env, 1);
    if (g_top_filter && g_top_filter(&ep) != EXCEPTION_CONTINUE_SEARCH) return;
    // Every caller in the runtime raises one of its own trap codes and terminates on the next line; if control ever
    // comes back here the process state is unknown, so stop.
    std::fflush(nullptr);
    abort();
}

void nxos_install_altstack(void) {
    stack_t cur{};
    if (sigaltstack(nullptr, &cur) == 0 && (cur.ss_flags & SS_DISABLE) == 0) return;   // already installed
    // A separate mapping, NOT a thread_local buffer: macOS puts the TLS block at the bottom of the thread's stack, so a static
    // altstack sat exactly where a stack overflow lands and the handler then overwrote the thread's deepest live frames (the
    // land thread's overflow turned into handler->note_fault recursion on 2026-11-10). 64 KiB of stack over a 64 KiB guard.
    // Linux has the same requirement for the same reason: a guest thread that overflows must be reported, not lost.
    static thread_local char* alt = nullptr;
    if (!alt) {
        void* p = ::mmap(nullptr, 2 * 64 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) return;
        ::mprotect(p, 64 * 1024, PROT_NONE);
        alt = static_cast<char*>(p);
    }
    stack_t ss{};
    ss.ss_sp = alt + 64 * 1024;
    ss.ss_size = 64 * 1024;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) != 0)
        std::fprintf(stderr, "nxrt: sigaltstack failed: %s\n", std::strerror(errno));
}

void nxos_set_thread_name(const char* name) { t_thread_name = name; }

int nxos_guarded_call(void (*fn)(void*), void* arg) {
    if (!fn) return 1;
    install_signals();
    nxos_install_altstack();   // a guarded copy can fault on a thread that has not installed one yet
    GuardFrame frame;
    frame.prev = t_guard;
    t_guard = &frame;
    if (sigsetjmp(frame.env, 1) == 0) {
        fn(arg);
        t_guard = frame.prev;
        return 1;
    }
    t_guard = frame.prev;   // the signal handler's siglongjmp lands here
    return 0;
}

#endif  // !_WIN32
