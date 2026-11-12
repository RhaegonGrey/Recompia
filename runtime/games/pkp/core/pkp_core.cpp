// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the core part of the game module (pkp_game.h): the data part plus the title facts and the polling loops the kernel
// treats specially. Needs the runtime library, not the graphics one.
#include "core/pkp_game.h"

#include "core/pkp_guest.h"
#include "nxrt/abi.h"
#include "nxrt/game.h"
#include "nxrt/services.h"
#include "nxrt/tt.h"

namespace nxrt::tt::pkp {
const GameProbes& pkp_game_probes();      // pkp_tt_probe.cpp
}

namespace nxrt::games::pkp {

namespace {

// main.npdm of the 2.0.1 update; save sizes are placeholders until control.nacp is known (the boot log shows what the game asks for).
void title_defaults(services::TitleProfile& p) {
    p.program_id = data_info().program_id;
    p.name = data_info().title;
    p.display_version = data_info().version;
    p.supported_languages = data_info().languages;   // control.nacp SupportedLanguage (pkp_data.cpp)
    p.account_save_size = 0x4000000;
    p.account_save_journal = 0x4000000;
    p.device_save_size = 0x4000000;
    p.device_save_journal = 0x4000000;
    p.main_thread_stack = 0x100000;            // main.npdm
    p.main_thread_priority = 44;
    p.main_thread_core = 0;
    p.firmware = "20.5.0";                     // the 2.0.1 update is built with SDK 20.5.25
    p.user_nickname = "Player";
    p.user_uid_hi = 0x0000000000000001ull;
    p.user_uid_lo = 0x41434e4852656370ull;     // "PKPRecmp"
}

// Polling sleeps on the boot path (IDA, 2.0.1; [sched] poll_sleeps, nxrt/game.h). The save manager (the object behind 0x71005ac980 and
// its siblings, run on the loader job thread from 0x710112e2cc) queues each save-file request (SYDAT.BIN and the other save files) on the
// request queue of a worker thread and then waits for it with `while (!request->done) SleepThread(1 s)`, or sets its state word (+0x628)
// and waits `while (state < n) SleepThread(1 s)`. The request takes milliseconds, so every one of them cost a full second: 12 of them on
// a boot to the title screen with a save, ~12 s of the 45 s. The boot state machine at 0x7100230b00 (state 8) sleeps 100 ms and returns
// while what it waits for is not ready (it runs again on the next step), 11 times on that boot. The BCAT polls (500 ms) never run offline
// but are the same kind of loop. All of them only check more often with 1 ms.
constexpr game::PollSleep kPollSleeps[] = {
    {0x5aae44, 1000000}, {0x5abc00, 1000000}, {0x5ac350, 1000000}, {0x5ac9fc, 1000000}, {0x5aca6c, 1000000}, {0x5ad21c, 1000000},
    {0x231580, 1000000},
    {0x545328, 1000000}, {0x545408, 1000000}, {0x583384, 1000000},
};

// Polling sleeps of the frame's hand-overs (2.0.1; [kernel] poll_wait, nxrt/game.h PollWait): the loop sleeps until a value another
// thread writes with a plain store has changed, so the sleep can end there.
//
// The application loop 0x7100882c60 (nnMain runs it once, for the whole game; x19 = the application object) asks 0x7101fdcde4 every round
// for the frame a job thread has finished: 0x7101fdfd60 takes the pointer at application + 0x130 under the spin bits of + 0x1e8 and the
// main thread then runs its part of that frame. The job that ends the game logic's frame (0x7102658b4c) stores the pointer in
// 0x7101fdd700 (at 0x7101fddaf8). With no frame waiting the loop sleeps 1 ms at 0x71008832ac. Its round counter (w28) only counts rounds
// without a frame up to 33, then runs the between-frames calls a finished frame also runs (0x7100883224: the resource pump
// 0x7101ff1d60, the driver's deferred calls) and starts again: a sleep that ends because the pointer is there is followed by the round
// that takes the frame, which resets the counter as before, so no extra round is counted.
constexpr uint64_t kAppLoopSleepRet = 0x8832b0;
constexpr uint64_t kAppPendingFrame = 0x130;   // application: the finished frame the main thread has not taken yet (0: none)
//
// The command queue's fence wait (0x71017c1220, and the same wait in the queue's flush 0x71017c0f80): the main thread puts a fence
// command (0x7d, then the word 0xaaaaaaaa) at the end of what it hands to the command thread and sleeps 100 us at a time until the
// word has changed (0x71017c2b7c); the command thread's handler of that command (0x71017c59a8) stores 0xbbbbbbbb in it. A fence is
// (queue generation << 60) | index of the word; the fence view (x20) holds the queue (+0: its words at +0x18) and the generation (+8).
constexpr uint64_t kFenceWaitSleepRet = 0x17c12b8;    // x20 = fence view, x19 = fence
constexpr uint64_t kFenceFlushSleepRet = 0x17c11ac;   // x20 = fence view, x21 = fence
constexpr uint64_t kFenceGeneration = 8, kFenceQueue = 0, kQueueWords = 0x18;
constexpr uint32_t kFencePending = 0xaaaaaaaa;
constexpr uint64_t kGuestLow = 0x1000000000ull;       // guest pointers are above the low 64 GiB (guest::ok)

bool app_loop_watch(const ::Ctx& c, game::PollWatch* w) {
    const uint64_t app = c.x[19];
    if (app < kGuestLow) return false;
    *w = {app + kAppPendingFrame, 0, 8};
    return true;
}

bool fence_watch(uint64_t view, uint64_t fence, game::PollWatch* w) {
    if (view < kGuestLow || !fence) return false;
    if ((guest::rd<uint32_t>(view + kFenceGeneration) & 0xf) != fence >> 60) return false;   // a fence of an older queue generation counts as passed
    const uint64_t queue = guest::rd<uint64_t>(view + kFenceQueue);
    if (queue < kGuestLow) return false;
    const uint64_t words = guest::rd<uint64_t>(queue + kQueueWords);
    if (words < kGuestLow) return false;
    *w = {words + (fence & 0x0fffffffffffffffull) * 4, kFencePending, 4};
    return true;
}
bool fence_wait_watch(const ::Ctx& c, game::PollWatch* w) { return fence_watch(c.x[20], c.x[19], w); }
bool fence_flush_watch(const ::Ctx& c, game::PollWatch* w) { return fence_watch(c.x[20], c.x[21], w); }

constexpr game::PollWait kPollWaits[] = {
    {kAppLoopSleepRet, &app_loop_watch},
    {kFenceWaitSleepRet, &fence_wait_watch},
    {kFenceFlushSleepRet, &fence_flush_watch},
};

}  // namespace

const game::Info& core_info() {
    static const game::Info info = [] {
        game::Info g = data_info();
        g.title_defaults = &title_defaults;
#if NXRT_TESTTOOLS
        g.probes = &tt::pkp::pkp_game_probes();
#endif
        g.poll_sleeps = kPollSleeps;
        g.poll_sleep_count = sizeof kPollSleeps / sizeof kPollSleeps[0];
        g.poll_waits = kPollWaits;
        g.poll_wait_count = sizeof kPollWaits / sizeof kPollWaits[0];
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::pkp

void nxrt_register_game_pkp_core() { nxrt::game::set_current(&nxrt::games::pkp::core_info()); }
