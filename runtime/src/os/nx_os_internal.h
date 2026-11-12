// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_internal.h: the object model shared by the three files of the POSIX shim (os_posix.cpp, os_posix_file.cpp,
// os_posix_mem.cpp). Internal to src/os -- the rest of the runtime only sees src/os/nx_os.h.
//
// One definition of Obj and one Kind enum, so that a HANDLE made by one file (a file handle from CreateFileW) can be
// passed to a function in another (CloseHandle). A per-file copy of these structs would compile and then be undefined
// behaviour, which is exactly the kind of bug that only shows up as a crash on a player's machine.
#pragma once

#include "nx_os.h"

#if !defined(_WIN32)

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nxos {

constexpr uint32_t kObjMagic = 0x4E78506Fu;   // "NxPo"

enum class Kind : uint32_t {
    File = 1,
    Thread,
    Process,
    Event,
    Mutex,
    Semaphore,
    Timer,
    Section,
    Find,
    Snapshot,
    Module,
};

struct Obj {
    uint32_t magic = kObjMagic;
    Kind kind;
    std::atomic<int> refs{1};
    explicit Obj(Kind k) : kind(k) {}
    virtual ~Obj() = default;
};

// Anything WaitForSingleObject can wait on: an atomic predicate plus an epoch that every state change bumps.
struct Waitable : Obj {
    std::atomic<bool> signaled{false};
    std::atomic<uint64_t> epoch{0};
    explicit Waitable(Kind k) : Obj(k) {}
};

struct FileObj : Obj {
    int fd = -1;
    bool is_dir = false;
    explicit FileObj(int f, bool dir = false) : Obj(Kind::File), fd(f), is_dir(dir) {}
    ~FileObj() override {
        if (fd >= 0) ::close(fd);
    }
};

struct ThreadObj : Obj {
    pthread_t th{};
    DWORD tid = 0;
    std::atomic<bool> exited{false};
    std::atomic<bool> started{false};
    std::atomic<int> suspend{0};
    std::atomic<int> priority{0};
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    uint64_t cpu_ns = 0;
    explicit ThreadObj() : Obj(Kind::Thread) {}
    ~ThreadObj() override {
        pthread_mutex_destroy(&mu);
        pthread_cond_destroy(&cv);
    }
};

struct ProcessObj : Obj {
    DWORD pid = 0;
    explicit ProcessObj(DWORD p) : Obj(Kind::Process), pid(p) {}
};

struct EventObj : Waitable {
    bool manual = false;
    explicit EventObj(bool m) : Waitable(Kind::Event), manual(m) {}
};

struct MutexObj : Waitable {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    DWORD owner = 0;
    int depth = 0;
    std::string name;
    MutexObj() : Waitable(Kind::Mutex) {}
    ~MutexObj() override { pthread_mutex_destroy(&mu); }
};

struct SemaphoreObj : Waitable {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    long count = 0, max = 0;
    SemaphoreObj() : Waitable(Kind::Semaphore) {}
    ~SemaphoreObj() override { pthread_mutex_destroy(&mu); }
};

struct TimerObj : Waitable {
    bool manual = false;
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    pthread_t worker{};
    bool worker_started = false;
    bool stop = false;
    uint64_t due_ns = 0;
    uint64_t period_ns = 0;
    uint64_t generation = 0;
    TimerObj() : Waitable(Kind::Timer) {}
    // The worker owns the mutex and the condition variable while it waits on them, so it has to be joined before they
    // are destroyed: CloseHandle on a timer stops the worker first (os_posix.cpp).
    ~TimerObj() override {
        if (worker_started) {
            pthread_mutex_lock(&mu);
            stop = true;
            due_ns = 0;
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&mu);
            pthread_join(worker, nullptr);
        }
        pthread_mutex_destroy(&mu);
        pthread_cond_destroy(&cv);
    }
};

struct FindObj : Obj {
    std::string dir;
    std::vector<std::string> names;
    size_t at = 0;
    explicit FindObj() : Obj(Kind::Find) {}
};

struct SnapshotObj : Obj {
    std::vector<DWORD> tids;
    size_t at = 0;
    explicit SnapshotObj() : Obj(Kind::Snapshot) {}
};

struct ModuleObj : Obj {
    void* dl = nullptr;
    std::string path;
    explicit ModuleObj() : Obj(Kind::Module) {}
};

// ---- handle plumbing (os_posix.cpp) -------------------------------------------------------------------------------
Obj* as_obj(HANDLE h);

template <typename T>
T* as(HANDLE h, Kind k) {
    Obj* o = as_obj(h);
    return o && o->kind == k ? static_cast<T*>(o) : nullptr;
}

void retain(Obj* o);
void release(Obj* o);

// ---- shared wait plumbing ----------------------------------------------------------------------------------------
// One process-wide condition variable and one mutex: see the comment at the top of os_posix.cpp.
extern pthread_mutex_t g_wait_mu;
extern pthread_cond_t g_wait_cv;
void wait_note();
void make_deadline(struct timespec* ts, DWORD ms);
bool past(const struct timespec& ts);
bool signaled_of(Obj* o);
void consume(Obj* o);

// ---- shared identity and conversion helpers ----------------------------------------------------------------------
DWORD self_tid();
ThreadObj* self_thread();   // the calling thread's object, created on first use
uint64_t filetime_of(const struct timespec& ts);
void set_filetime(FILETIME* ft, uint64_t v);
std::string native_path(const char* p);   // backslashes to slashes

}  // namespace nxos

#endif  // !_WIN32
