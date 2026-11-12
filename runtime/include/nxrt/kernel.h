// SPDX-License-Identifier: GPL-3.0-or-later
// A small Horizon kernel: result codes, kernel objects and the handle table, guest threads (one Win32 thread per guest thread,
// console core model), and the host sleeps. The SVC dispatcher lives in svc.h, IPC sessions in ipc.h, the system tick in clock.h.
// Pattern from SuperMarioWonderRecomp runtime/src/kernel.cpp + kernel_state.h (MIT) and smo-recomp runtime/os/os_threads.c (GPL-3.0).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/game.h"

namespace nxrt::kernel {

// ---- result codes (module | description << 9; the low 9 bits are the module, kernel = 1) ----------------------------------
constexpr uint32_t make_result(uint32_t module, uint32_t desc) { return module | (desc << 9); }
// Values from Atmosphere libvapours svc_results.hpp (module 1): description numbers below.
constexpr uint32_t kOk = 0;
constexpr uint32_t kNotImplemented = 0x4201;     // 33: what a kernel answers for an SVC number it does not implement
constexpr uint32_t kInvalidSize = 0xCA01;        // 101
constexpr uint32_t kInvalidAddress = 0xCC01;     // 102
constexpr uint32_t kOutOfResource = 0xCE01;      // 103
constexpr uint32_t kOutOfMemory = 0xD001;        // 104
constexpr uint32_t kOutOfHandles = 0xD201;       // 105
constexpr uint32_t kInvalidMemoryState = 0xD401; // 106 (InvalidCurrentMemory)
constexpr uint32_t kInvalidPermission = 0xD801;  // 108
constexpr uint32_t kInvalidMemoryRegion = 0xDC01;// 110
constexpr uint32_t kInvalidPriority = 0xE001;    // 112
constexpr uint32_t kInvalidCoreId = 0xE201;      // 113
constexpr uint32_t kInvalidHandle = 0xE401;      // 114
constexpr uint32_t kInvalidPointer = 0xE601;     // 115
constexpr uint32_t kInvalidCombination = 0xE801; // 116
constexpr uint32_t kTimedOut = 0xEA01;           // 117
constexpr uint32_t kCancelled = 0xEC01;          // 118
constexpr uint32_t kOutOfRange = 0xEE01;         // 119
constexpr uint32_t kInvalidEnum = 0xF001;        // 120
constexpr uint32_t kNotFound = 0xF201;           // 121
constexpr uint32_t kSessionClosed = 0xF601;      // 123
constexpr uint32_t kInvalidState = 0xFA01;       // 125
constexpr uint32_t kStubService = make_result(0x1D5, 0x5E5);   // 0x0BCBD5: "an nxrt stub service answered": never a real Horizon result

constexpr uint32_t kPseudoCurrentThread = 0xFFFF8000;
constexpr uint32_t kPseudoCurrentProcess = 0xFFFF8001;
constexpr uint32_t kHandleWaitMask = 0x40000000;   // set in a guest mutex word while there are waiters

// ---- objects ---------------------------------------------------------------------------------------------------------------
enum class ObjType { Unknown, Thread, Event, Session, SharedMemory, TransferMemory, Process, Port };

struct KObject {
    virtual ~KObject() = default;
    virtual ObjType type() const { return ObjType::Unknown; }
    virtual bool signaled() const { return false; }        // for WaitSynchronization
    virtual void consume() {}                              // called on the object WaitSynchronization returned
};

struct EventState {
    bool signaled = false;
};
struct KEvent : KObject {                                  // readable and writable handles share `state`
    std::shared_ptr<EventState> state = std::make_shared<EventState>();
    bool writable = false;
    ObjType type() const override { return ObjType::Event; }
    bool signaled() const override { return state->signaled; }
};

struct KSharedMemory : KObject {
    uint64_t size = 0;
    uint64_t host = 0;           // backing memory (kernel region) the service writes to
    std::string name;
    ObjType type() const override { return ObjType::SharedMemory; }
};

struct KTransferMemory : KObject {
    uint64_t addr = 0, size = 0;
    ObjType type() const override { return ObjType::TransferMemory; }
};

// ---- handle table ------------------------------------------------------------------------------------------------------------
uint32_t handle_add(std::shared_ptr<KObject> o);          // 0 when the table is full; handles start at 0x10000 (Horizon-like opaque values)
std::shared_ptr<KObject> handle_get(uint32_t h);          // resolves kPseudoCurrentThread to the calling guest thread
void handle_close(uint32_t h);
size_t handle_count();
template <class T>
std::shared_ptr<T> handle_as(uint32_t h) { return std::dynamic_pointer_cast<T>(handle_get(h)); }
void handles_reset();                                      // tests

// ---- yield back-off ([sched] yield_backoff; svc_thread.cpp) ----------------------------------------------------------------
// A run of yields is SleepThread(0/-1/-2) calls of one thread with no other SVC of that thread between them and no progress: the caller's
// callee-saved x19..x28 hash equals one of the run's last two yields'. Such a loop waits for another thread, and every host yield of it
// is a busy spin (on a handheld that shares one power budget between CPU and GPU, spin watts come out of the GPU clock). The step:
// the run's first `plain` yields hand the processor over (yield_host); then Sleep(0) until the run has lasted start_ns; then each yield
// sleeps an eighth of the run so far, clamped to [min_ns, cap_ns]. Any other SVC, or changed registers, starts a new run.
// Run shape from pokopia-native host/hle_thread.c yield_backoff_ns / thr_yield (MIT) @7c3655d; the Sleep(0) step is ours.
struct YieldBackoffCfg {
    uint32_t plain = 64;                 // the run's first yields: SwitchToThread
    uint64_t start_ns = 2000000;         // until the run has lasted this long: Sleep(0)
    uint64_t min_ns = 10000, cap_ns = 250000;
};
struct YieldRun {
    uint64_t svc = 0;                    // the thread's SVC count at the run's last yield
    uint64_t t0 = 0;                     // when the run started
    uint32_t n = 0;                      // yields in the run
    uint64_t sig = 0, sig2 = 0;          // register hashes of the last two yields
};
enum class YieldAct : uint8_t { Yield, Sleep0, Sleep };
struct YieldStep {
    YieldAct act;
    uint64_t ns;                         // Sleep only
};
// nsvc: the thread's SVC count including this yield; now: host ns; sig: yield_regs_sig of the caller. Pure (tests).
YieldStep yield_backoff_step(YieldRun& r, const YieldBackoffCfg& c, uint64_t nsvc, uint64_t now, uint64_t sig);
uint64_t yield_regs_sig(const ::Ctx& c); // FNV-1a over x19..x28

// ---- guest threads ---------------------------------------------------------------------------------------------------------
struct KThread : KObject {
    uint64_t id = 0;                    // svcGetThreadId value
    uint32_t handle = 0;
    std::string name;
    alignas(16) ::Ctx ctx{};            // guest registers of this thread
    uint64_t tls = 0;                   // TLS region (TPIDRRO_EL0), 0x200 bytes; +0x1F0 holds the thread pointer (nn::os convention)
    uint64_t tp_block = 0;              // static TLS block (TPIDR_EL0 points here)
    uint64_t entry = 0, arg = 0, stack_top = 0;
    int prio = 44;                      // HOS priority 0 (highest) .. 63; applications use 28..59 (nn::os 0..31 = prio - 28)
    int ideal_core = 0;                 // console core 0..2 the thread prefers (-1 none)
    uint64_t core_mask = 1;             // console cores the thread may run on (bit n = core n)
    void* host_handle = nullptr;        // Win32 thread handle (owned by the thread object)
    uint32_t host_tid = 0;
    bool started = false, cancel_sync = false;
    std::atomic<bool> exited{false};    // written under the kernel lock (thread_mark_exited), read under the thread-list lock and elsewhere
    std::atomic<bool> aged{false};      // starvation guard (starvation_guard.cpp): the host leaves this thread ready, so its yields keep the processor
    std::atomic<uint64_t> host_affinity{0};   // the host affinity mask sched_apply (or a starvation boost) last gave the host thread
    std::atomic<int> last_proc{-1};     // host processor the thread was on at one of its last 16 SVCs (svc_dispatch samples it), for the starvation line
    uint64_t nsvc = 0;                  // SVCs the thread made (svc_dispatch; only the thread itself writes it)
    YieldRun yrun;                      // [sched] yield_backoff state (svc_thread.cpp; own thread only)
    std::atomic<uint64_t> nyield{0}, nbackoff{0};   // yield SVCs, and of them the ones that slept (written by the thread, read by the census)
    uint64_t census_yield = 0, census_backoff = 0;  // [diag] yield_census: the counts at the previous summary (under the census lock)
    // wait state, protected by the kernel lock (kernel_sync.cpp)
    std::atomic<bool> wake{false};      // written under the kernel lock; with [kernel] sync_wait = address the woken thread reads it without the lock
    uint32_t wait_result = 0;           // written before wake is set
    uint64_t wake_at_ns = 0;            // [diag] sync_stats: when wake was set (qpc), written before it
    uint64_t wait_mutex = 0, wait_key = 0, wait_addr = 0;
    uint32_t wait_tag = 0;
    void* cv_storage[1] = {nullptr};    // CONDITION_VARIABLE is pointer sized: this thread's own wake-up (kernel_sync.cpp)
    std::atomic<uint32_t> park{0};      // [kernel] sync_wait = address: the word the thread sleeps on; every wake-up adds one under the kernel lock
    void* precise_event = nullptr;      // [sched] precise_waits: the event the thread waits on in a wait's last stretch, set too on a wake
    std::weak_ptr<KThread> self;
    ~KThread() override;                // closes host_handle
    ObjType type() const override { return ObjType::Thread; }
    bool signaled() const override { return exited; }
};

struct ThreadParams {
    uint64_t entry = 0, arg = 0, stack_top = 0;
    int prio = 44;
    int core = -2;                       // -2: the process default core (0)
    std::string name;
    uint64_t host_stack_bytes = 0;       // 0: [threads] host_stack_mb (default 64) MiB: lifted code nests deeply
};

// Creates the thread object (TLS region and static TLS block included) without starting it. Returns its handle, 0 on failure.
uint32_t thread_create(const ThreadParams& p);
// Starts the host thread: it registers with diag, applies the console core/priority model, runs entry(arg) through dispatch_call
// with sp = stack_top and exits when the function returns (like svcExitThread).
uint32_t thread_start(uint32_t handle);                    // result code
[[noreturn]] void thread_exit();                           // svcExitThread for the calling guest thread
// Turns the calling host thread into the main guest thread (stack allocated in the kernel region). Returns its handle.
uint32_t main_thread_adopt(uint64_t stack_size, const char* name = "main");
void main_thread_release();                                // the adopted main thread is done: unregisters it from diag and marks it exited
KThread* current_thread();                                 // nullptr on a host thread that is not a guest thread
::Ctx* current_ctx();                                      // current_thread()->ctx or nullptr
std::vector<std::shared_ptr<KThread>> threads();           // every thread ever created and not yet destroyed
void kernel_reset();                                       // tests: join/forget threads, clear handles, sync state (no thread may run guest code)
uint64_t thread_count_running();                           // started and not yet exited
bool thread_set_priority(KThread& t, int hos_prio);       // updates the object and the host thread
int app_core_count();                 // application cores: 3 (Switch 1) or 6 (Switch 2)
bool thread_set_core_mask(KThread& t, int ideal_core, uint64_t mask);   // console cores; false when mask has no usable core

// ---- scheduling model (console cores) ----------------------------------------------------------------------------------------
// The console runs the highest-priority ready thread on each of its three application cores. The model gives every console core one host
// processor and maps HOS priorities onto six Windows priority bands. [sched] affinity = soft (default): that processor is the thread's ideal
// processor and its affinity is the whole pool, so a ready thread runs on any idle processor; strict: the affinity is pinned to it.
struct SchedInfo {
    bool active = false;                 // false: fewer usable host processors than console cores, or [sched] console_cores = 0
    bool soft = false;                   // [sched] affinity = soft: affinity `pool`, ideal processor = the core's processor
    int cores = 3;                       // application cores of the console: 3 (Switch 1), 6 (Switch 2)
    uint64_t host_masks[6] = {};         // host processor per console core (the first `cores` are used): the affinity (strict) or ideal processor (soft)
    uint64_t pool = 0;                   // host processors a guest thread may use under soft (and during a starvation boost under strict)
    int slot = -1;                       // processor slot taken (-1: shared / explicit mask)
};
SchedInfo sched_info();                  // initialises on first use
int sched_host_priority(int hos_prio);   // THREAD_PRIORITY_* for a HOS priority under the model (nn::os prio = hos - 28)
// Which console core the calling thread is on right now: the core of its host processor (strict), or its ideal core (soft, or off-model processor).
int sched_current_core();
// Process priority class. [sched] priority_class = auto | high | above_normal | normal (default auto: high when `interactive`, i.e. a person
// is playing in a window, otherwise left alone). The guest threads are pinned and run with Windows' wake-up boost off, so under
// background CPU load a woken thread waits a full scheduler quantum for its processor (debug 180, S10 measurements: 100 to 200 ms
// stalls and half the frame rate with every core busy); a class above the background keeps the hand-overs immediate.
void sched_set_process_priority(bool interactive);
void sched_apply(KThread& t);            // (re)applies affinity + priority of t to its host thread
std::string sync_wait_report();          // the guest threads blocked in WaitForAddress / process-wide keys / mutex hand-overs, with the word each waits on (watchdog dump; never blocks)
void yield_host();                       // the calling thread offers its processor to another ready thread (SwitchToThread), unless the starvation guard aged it
void starvation_guard_start();           // ages guest threads the host leaves ready but never runs (starvation_guard.cpp, [sched] starvation_guard)
void starvation_guard_stop();
uint64_t starvation_boosts();            // boosts given so far
void install_spin_hint();                // WFE / YIELD of the lifted code hand the host processor over (busy_mutex.cpp, [sched] spin_yield)
void install_busy_mutex_hle();           // nn::os::LockBusyMutex as host code that yields while it waits (busy_mutex.cpp, [sched] busy_mutex_hle)

// ---- what the kernel asks of the layers above it (set once by nxrt::init, src/core/runtime.cpp, before the guest runs) ---------
struct ProcessHooks {
    uint64_t (*program_id)() = nullptr;             // GetInfo ProgramId (services: [process] program_id, else the title's)
    uint64_t (*system_resource_size)() = nullptr;   // GetInfo SystemResourceSizeTotal default (the title profile's NPDM value)
    std::string (*state_report)() = nullptr;        // the runtime's state summary, logged at svcExitProcess and svcBreak
};
void set_process_hooks(const ProcessHooks& h);     // unset members answer 0 / an empty report

// ---- time (the console tick and its rate: nxrt/clock.h) ----------------------------------------------------------------------
void sleep_ns(int64_t ns);               // high-resolution sleep; <= 0 yields
// SleepThread with the game's polling sleeps shortened: the sleep a call from `lr` gets (`ns` unchanged when lr is not in the table, the
// request is under 1 ms, or the listed sleep is not shorter). Table entries are offsets from main_base (game.h PollSleep).
int64_t poll_sleep_ns(const game::PollSleep* table, uint32_t count, uint64_t main_base, uint64_t lr, int64_t ns);
// Precise waits (precise_wait.cpp). [sched] precise_sleep: SleepThread over 200 us ends on its deadline (timer lateness corrected, the
// last kPreciseSpinMaxNs or less spun through). [sched] precise_waits: timed kernel waits end on the deadline instead of being rounded
// up to the next whole millisecond. Both off by default, read once.
constexpr int64_t kPreciseSpinMaxNs = 200000;   // the most a precise wait spins (pause loop) instead of sleeping on the timer
// Pure: what to ask the timer for with rem_ns left and the timer waking late_ns late; 0 = spin the rest (rem_ns <= kPreciseSpinMaxNs).
int64_t precise_timer_request(int64_t rem_ns, int64_t late_ns);
// Pure: the lateness estimate after one timer wait asked for asked_ns that took took_ns (moving average 1/16, 0..2 ms; samples that came
// back early or 2 ms or more late are ignored).
int64_t precise_late_update(int64_t late_ns, int64_t asked_ns, int64_t took_ns);
bool precise_sleep_enabled();
bool precise_waits_enabled();
// Waits ns on the calling thread's waitable timer, never ending before ns have passed unless wake_event (a HANDLE, may be null) is set;
// true when the event ended it.
bool precise_wait_ns(int64_t ns, void* wake_event);

// ---- kernel-level synchronisation (the SVC semantics; kernel_sync.cpp) -------------------------------------------------------
// ArbitrateLock / WaitProcessWideKeyAtomic / WaitForAddress ... operate on guest memory words. Exposed for tests and for HLE code.
uint32_t arbitrate_lock(uint32_t owner_handle, uint64_t addr, uint32_t tag);
uint32_t arbitrate_unlock(uint64_t addr);
uint32_t wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout_ns);
uint32_t signal_process_wide_key(uint64_t key, int32_t count);
uint32_t wait_for_address(uint64_t addr, uint32_t arb_type, int32_t value, int64_t timeout_ns);     // 0 less, 1 dec+less, 2 equal
uint32_t signal_to_address(uint64_t addr, uint32_t signal_type, int32_t value, int32_t count);     // 0 signal, 1 +1 if equal, 2 modify by waiting
uint32_t wait_synchronization(const uint32_t* handles, int32_t count, int64_t timeout_ns, int32_t* index_out);
uint32_t cancel_synchronization(uint32_t thread_handle);
void notify_waitable_changed();          // wake threads blocked in wait_synchronization (call after changing an object's signaled state)
// A host function that one host thread runs while it is blocked in a kernel wait (WaitSynchronization, mutex, condition variable and address
// waits, and SleepThread longer than 10 ms): its wait is cut into 10 ms slices and fn runs between them, without the kernel lock. The play layer gives the window's thread one,
// so the window keeps working while the game waits on something the player must do (the software keyboard applet). fn null: off.
void set_blocked_hook(uint32_t host_tid, void (*fn)());

}  // namespace nxrt::kernel
