// SPDX-License-Identifier: GPL-3.0-or-later
// Crash handler (declared in nxrt/diag.h): an unhandled-exception filter that writes the "HOST EXCEPTION" report, plus a
// vectored handler that catches stack overflow on a fresh helper thread. Guest traps (0xE0000001 / 0xE0000002) arrive at
// the unhandled filter like any other exception. The layout of the report follows crash_build_report() in diag.h.
// Idea from smo-recomp runtime/core/hostrt.c crash_filter; written here from scratch.
#include "nxrt/diag.h"

#include <windows.h>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/symbols.h"
#include "stackwalk.h"

namespace nxrt {
namespace {

#if defined(__APPLE__)
extern "C" uintptr_t nxos_fault_pthread();   // os_posix.cpp: pthread_self() of the thread that faulted (0 if not captured)
// Reporter progress on fd 2: if the reporter itself dies, the last marker says which section killed it.
void rep_mark(const char* s) { (void)::write(2, s, std::strlen(s)); }
#else
void rep_mark(const char*) {}
#endif

extern DWORD g_rep_thread;           // defined with the reporter below; second_exception() needs these first
extern std::atomic<int> g_rep_state;
#if defined(__APPLE__)
extern "C" int nxos_recent_faults(uint64_t* pcs, uint64_t* sps, uint32_t* sigs, uint32_t* guards, int max);
extern "C" int nxos_recent_fault_details(uint64_t* guard_ptrs, uint64_t* slots, const char** names, uint64_t* tps, int max);
#endif

constexpr DWORD kTrapCode = 0xE0000001u;
constexpr DWORD kUntranslatedCode = 0xE0000002u;
constexpr DWORD kGuestBreakCode = 0xE0000003u;
constexpr DWORD kAbortCode = 0xE0000004u;      // os_posix.cpp: SIGABRT (the guest's abort()/std::terminate)   // svcBreak / svcReturnFromException (svc_thread.cpp)
constexpr size_t kMaxHostFrames = 32;
constexpr int kMaxGuestFrames = 24;
constexpr DWORD kOverflowWaitMs = 30000;

std::mutex g_install_mu;
bool g_installed = false;

// Options copied at install time and read by the handler.
bool g_path_given = false;  // false: default "<exe>_crash.txt" next to the exe; true with "" : no file
std::string g_path;
bool g_terminate = true;
uint32_t g_exit_code = 4;
void (*g_on_report)(const char*, void*) = nullptr;
void* g_user = nullptr;

std::atomic<bool> g_in_handler{false};          // set while the handler runs: a second exception terminates
std::atomic<bool> g_overflow_reported{false};   // the vectored handler already reported this stack overflow
std::atomic<uint32_t> g_reports{0};

// Stack overflow: the faulting thread has no stack left, so its record and context are copied into these statics and a
// helper thread builds the report from them.
EXCEPTION_RECORD g_ov_rec{};
CONTEXT g_ov_ctx{};
DWORD g_ov_tid = 0;

#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void appendf(std::string& out, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    if (n >= 0 && static_cast<size_t>(n) < sizeof buf) {
        out.append(buf, static_cast<size_t>(n));
    } else if (n > 0) {
        std::string big(static_cast<size_t>(n) + 1, '\0');
        std::vsnprintf(big.data(), big.size(), fmt, ap2);
        out.append(big.data(), static_cast<size_t>(n));
    }
    va_end(ap);
    va_end(ap2);
}

// True when every byte of [addr, addr+n) is committed, readable and not a guard page. Never faults.
bool readable(uint64_t addr, uint64_t n) {
    if (n == 0) return true;
    uint64_t p = addr;
    const uint64_t end = addr + n;
    if (end < addr) return false;
    const DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(p)), &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(mbi.Protect & kReadable)) return false;
        const uint64_t next = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) + mbi.RegionSize;
        if (next <= p) return false;
        p = next;
    }
    return true;
}

// <exe path without extension>_crash.txt
std::string default_report_path() {
    char exe[1024] = {};
    DWORD n = GetModuleFileNameA(nullptr, exe, sizeof exe);
    std::string p(exe, n < sizeof exe ? n : sizeof exe - 1);
    size_t slash = p.find_last_of("\\/");
    size_t dot = p.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) p.erase(dot);
    return p + "_crash.txt";
}

bool write_file(const std::string& path, const std::string& text) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD done = 0;
    BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &done, nullptr);
    CloseHandle(h);
    return ok && done == text.size();
}

// The register file the report prints. Win32's CONTEXT names the x64 registers Rip/Rsp/Rbp and has no link register; the POSIX
// shim's arm64-shaped CONTEXT (src/os/nx_os.h) calls them pc/sp/fp and carries lr. Two accessors keep the report code shared.
#if defined(_WIN32)
static inline uintptr_t ctx_pc(const CONTEXT* cx) { return static_cast<uintptr_t>(cx->Rip); }
static inline uintptr_t ctx_sp(const CONTEXT* cx) { return static_cast<uintptr_t>(cx->Rsp); }
static inline uintptr_t ctx_fp(const CONTEXT* cx) { return static_cast<uintptr_t>(cx->Rbp); }
static inline uintptr_t ctx_lr(const CONTEXT*) { return 0; }   // x64 returns through the stack
#else
static inline uintptr_t ctx_pc(const CONTEXT* cx) { return static_cast<uintptr_t>(cx->pc); }
static inline uintptr_t ctx_sp(const CONTEXT* cx) { return static_cast<uintptr_t>(cx->sp); }
static inline uintptr_t ctx_fp(const CONTEXT* cx) { return static_cast<uintptr_t>(cx->fp); }
static inline uintptr_t ctx_lr(const CONTEXT* cx) { return static_cast<uintptr_t>(cx->lr); }
#endif

// Builds the whole report text for an exception raised on thread `tid`.
std::string build_report(const EXCEPTION_RECORD* rec, const CONTEXT* cx, DWORD tid) {
    g_reports.fetch_add(1, std::memory_order_relaxed);
    std::string out;
    const uint64_t rip = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress));
    rep_mark("crash: rep: threads\n");
    const std::vector<ThreadRecord> threads = diag_threads();
    rep_mark("crash: rep: header\n");

    // The faulting thread: its registered name and Ctx. The current thread uses its thread-local Ctx; another thread
    // (stack-overflow helper) uses the Ctx its registry record holds.
    std::string tname = "unregistered";
    bool named = false;
    ::Ctx* gctx = nullptr;
    for (const auto& t : threads) {
        if (t.host_tid != tid) continue;
        if (!named) {
            tname = t.name.empty() ? std::string("(unnamed)") : t.name;
            named = true;
        }
        if (!gctx && t.ctx) gctx = t.ctx;
    }
    if (tid == GetCurrentThreadId()) gctx = diag_thread_ctx();
    if (gctx && !readable(reinterpret_cast<uint64_t>(gctx), sizeof(::Ctx))) gctx = nullptr;

    // Header.
    const char* code_name = nullptr;
    if (rec->ExceptionCode == kAbortCode) code_name = "ABORT";
    else if (rec->ExceptionCode == kTrapCode) code_name = "GUEST TRAP";
    else if (rec->ExceptionCode == kUntranslatedCode) code_name = "UNTRANSLATED INSTRUCTION";
    else if (rec->ExceptionCode == kGuestBreakCode) code_name = "GUEST BREAK (svcBreak)";
    if (code_name) {
        appendf(out, "HOST EXCEPTION 0x%08lX (%s) at host address 0x%llx on thread %s (tid %lu)\n",
                static_cast<unsigned long>(rec->ExceptionCode), code_name, static_cast<unsigned long long>(rip),
                tname.c_str(), static_cast<unsigned long>(tid));
    } else {
        appendf(out, "HOST EXCEPTION 0x%08lX at host address 0x%llx on thread %s (tid %lu)\n",
                static_cast<unsigned long>(rec->ExceptionCode), static_cast<unsigned long long>(rip), tname.c_str(),
                static_cast<unsigned long>(tid));
    }
    appendf(out, "  in %s\n", symbols_describe(rip).c_str());
    appendf(out, "  pc 0x%llx, sp 0x%llx, fp 0x%llx, lr 0x%llx\n", static_cast<unsigned long long>(ctx_pc(cx)),
            static_cast<unsigned long long>(ctx_sp(cx)), static_cast<unsigned long long>(ctx_fp(cx)),
            static_cast<unsigned long long>(ctx_lr(cx)));
    // The page the fault address lives on. On macOS a write to a read-only or PROT_NONE page is SIGBUS (0xC0000006), not an
    // access violation, so the protection is the thing to look at first.
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(rip), &mbi, sizeof mbi) != 0)
            appendf(out, "  fault page: state 0x%lx, protect 0x%lx, region 0x%llx (+0x%llx)\n", static_cast<unsigned long>(mbi.State),
                    static_cast<unsigned long>(mbi.Protect), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)),
                    static_cast<unsigned long long>(mbi.RegionSize));
    }
    // The stack pointer's own region: on a stack overflow the fault address is the guard page and sp has already left the stack,
    // so this shows which region sp is really in (a stack, the guard, or nothing mapped).
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(ctx_sp(cx)), &mbi, sizeof mbi) != 0)
            appendf(out, "  sp page: state 0x%lx, protect 0x%lx, region 0x%llx (+0x%llx)\n", static_cast<unsigned long>(mbi.State),
                    static_cast<unsigned long>(mbi.Protect), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)),
                    static_cast<unsigned long long>(mbi.RegionSize));
    }
    // The faulting thread's own stack bounds, captured by the handler: pthread_from_mach_thread_np misses many threads, and
    // pthread_getname_np on a foreign pthread_t can trap, so the faulting thread is the only one queried.
#if defined(__APPLE__)
    {
        pthread_t pt = reinterpret_cast<pthread_t>(nxos_fault_pthread());
        if (pt) {
            char nm[64] = {};
            pthread_getname_np(pt, nm, sizeof nm);
            const uintptr_t top = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pt));
            const size_t size = pthread_get_stacksize_np(pt);
            appendf(out, "  faulting thread: %s, stack 0x%llx..0x%llx (%zu KiB)\n", nm[0] ? nm : "(unnamed)",
                    static_cast<unsigned long long>(top - size), static_cast<unsigned long long>(top), size >> 10);
        }
    }
    {
        const uint64_t base = symbols_main_base();
        if (base) {
            appendf(out, "  image base 0x%llx", static_cast<unsigned long long>(base));
            if (ctx_pc(cx) >= base) appendf(out, " (pc RVA 0x%llx)", static_cast<unsigned long long>(ctx_pc(cx) - base));
            if (ctx_lr(cx) >= base) appendf(out, ", lr RVA 0x%llx", static_cast<unsigned long long>(ctx_lr(cx) - base));
            out += "\n";
        }
    }
#endif
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        const char* what = kind == 0 ? "reading" : kind == 1 ? "writing" : kind == 8 ? "executing" : "accessing";
        appendf(out, "  %s address 0x%llx\n", what, static_cast<unsigned long long>(rec->ExceptionInformation[1]));
    }

    // Last guest function: the lifted function whose host code contains the faulting instruction.
    if (const uint64_t g = guest_function_of(rip))
        appendf(out, "last guest function entered: f_%llx\n", static_cast<unsigned long long>(g));
    else
        out += "last guest function entered: (unknown: fault address is not inside lifted code)\n";

    // Guest registers.
    if (!gctx) {
        out += "guest registers: (no guest context on this thread)\n";
    } else {
        out += "guest registers:\n";
        for (int n = 0; n <= 30; ++n) {
            out += (n % 3 == 0) ? (n == 0 ? "  " : "\n  ") : "   ";
            appendf(out, "x%-2d %016llx", n, static_cast<unsigned long long>(reg(gctx, n)));
        }
        out += "\n";
        appendf(out, "  sp %016llx   tpidr %016llx   tpidrro %016llx\n", static_cast<unsigned long long>(sp(gctx)),
                static_cast<unsigned long long>(tpidr_el0(gctx)), static_cast<unsigned long long>(tpidrro_el0(gctx)));
    }

    // Guest frame chain: lr of the Ctx, then the return address of every frame reachable through x29.
    if (!gctx) {
        out += "guest frame chain (x29): (none)\n";
    } else {
        appendf(out, "guest frame chain (x29): lr=0x%llx", static_cast<unsigned long long>(lr(gctx)));
        uint64_t f = fp(gctx);
        for (int i = 0; i < kMaxGuestFrames && f && readable(f, 16); ++i) {
            const uint64_t next = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f));
            const uint64_t ret = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f + 8));
            if (ret) appendf(out, " <- 0x%llx", static_cast<unsigned long long>(ret));
            if (next <= f) break;  // frames must go up the stack; anything else is a broken chain
            f = next;
        }
        out += "\n";
    }

    // The faults that came before this one: a guarded copy faults by design, so the fatal fault alone does not say whether it
    // was the first or whether a guard was armed (recorded by the signal handler, oldest first).
#if defined(__APPLE__)
    {
        uint64_t pcs[8] = {}, sps[8] = {};
        uint32_t sigs[8] = {}, guards[8] = {};
        const int n = nxos_recent_faults(pcs, sps, sigs, guards, 8);
        uint64_t gptrs[8] = {}, slots[8] = {};
        const char* names[8] = {};
        uint64_t tps[16] = {};
        nxos_recent_fault_details(gptrs, slots, names, tps, 8);
        out += "recent faults (oldest first):\n";
        if (!n) out += "  (none)\n";
        for (int i = 0; i < n; ++i)
            appendf(out, "  %s: signal %u at pc %s, sp 0x%llx, guard %s (t_guard 0x%llx at 0x%llx, tp 0x%llx/0x%llx)\n",
                    names[i] && *names[i] ? names[i] : "(unnamed)", sigs[i], symbols_describe(pcs[i]).c_str(),
                    static_cast<unsigned long long>(sps[i]), guards[i] ? "armed" : "none",
                    static_cast<unsigned long long>(gptrs[i]), static_cast<unsigned long long>(slots[i]),
                    static_cast<unsigned long long>(tps[2 * i]), static_cast<unsigned long long>(tps[2 * i + 1]));
    }
#endif
    // Host call stack, symbolised.
    uint64_t frames[kMaxHostFrames] = {};
    rep_mark("crash: rep: stack\n");
    const size_t nframes = detail::capture_stack(cx, frames, kMaxHostFrames);
    out += "host call stack (innermost first):\n";
    if (!nframes) out += "  (none)\n";
    for (size_t i = 0; i < nframes; ++i) appendf(out, "  %s\n", symbols_describe(frames[i]).c_str());
    rep_mark("crash: rep: gueststack\n");

    // A frame-pointer chain that breaks early (a leaf fault, a corrupted record) leaves the caller unknown. Scan the faulting
    // thread's stack for words that land inside a loaded image: the return addresses of the frames the chain lost.
    if (nframes < 4) {
        MEMORY_BASIC_INFORMATION mbi{};
        const uintptr_t sp = ctx_sp(cx);
        if (VirtualQuery(reinterpret_cast<LPCVOID>(sp), &mbi, sizeof mbi) != 0 && (mbi.Protect & 0x4)) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t end = base + mbi.RegionSize;
            const uintptr_t stop = std::min<uintptr_t>(end, sp + (256u << 10));
            int shown = 0;
            out += "stack scan (words that point into an image, innermost first):\n";
            for (uintptr_t a = sp & ~static_cast<uintptr_t>(7); a + 8 <= stop && shown < 16; a += 8) {
                const uint64_t v = *reinterpret_cast<const uint64_t*>(a);
                if (symbols_lookup(v).module == "?") continue;
                appendf(out, "  0x%llx: %s\n", static_cast<unsigned long long>(a), symbols_describe(v).c_str());
                ++shown;
            }
            if (!shown) out += "  (none)\n";
        }
    }

    // Guest call stack: the lifted functions among the host frames, consecutive repeats dropped.
    out += "guest call stack (innermost first):";
    uint64_t last = 0;
    bool any = false;
    for (size_t i = 0; i < nframes; ++i) {
        const uint64_t g = guest_function_of(frames[i]);
        if (!g || g == last) continue;
        appendf(out, " f_%llx", static_cast<unsigned long long>(g));
        last = g;
        any = true;
    }
    if (!any) out += " (none)";
    out += "\n";

    // Threads of the registry.
    out += "threads:\n";
    if (threads.empty()) out += "  (none)\n";
    for (const auto& t : threads) {
        char lrbuf[32] = "-";
        if (t.ctx && readable(reinterpret_cast<uint64_t>(t.ctx), sizeof(::Ctx)))
            std::snprintf(lrbuf, sizeof lrbuf, "0x%llx", static_cast<unsigned long long>(lr(t.ctx)));
        appendf(out, "  %s tid=%lu guest_tid=%llu lr=%s\n", t.name.empty() ? "(unnamed)" : t.name.c_str(),
                static_cast<unsigned long>(t.host_tid), static_cast<unsigned long long>(t.guest_tid), lrbuf);
    }

    // Recent files, oldest first.
    rep_mark("crash: rep: files\n");
    const std::vector<std::string> files = diag_recent_files();
    out += "last files opened (oldest first):\n";
    if (files.empty()) out += "  (none)\n";
    for (const auto& f : files) appendf(out, "  %s\n", f.c_str());

    // The last 30 s of frame times: what the game was doing before it went wrong.
    rep_mark("crash: rep: frames\n");
    out += frame_times_summary(30.0);
    // The sections other parts register for the freeze dump (diag_add_report): the GPU's state, the graphics pools' fill.
    rep_mark("crash: rep: extra\n");
    out += diag_crash_extra_reports();
    rep_mark("crash: rep: built\n");
    return out;
}

// Log block, report file, stderr, then the on_report callback. Terminating is the caller's decision.
void emit(const std::string& text) {
    for (size_t pos = 0; pos < text.size();) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        NXLOG_E("crash", "%.*s", static_cast<int>(eol - pos), text.data() + pos);
        pos = eol + 1;
    }
    log_flush();
    const std::string path = g_path_given ? g_path : default_report_path();
    if (!path.empty()) write_file(path, text);
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
    if (g_on_report) g_on_report(text.c_str(), g_user);
}

[[noreturn]] void second_exception() {
    // While the reporter is building the report, a fault on another thread must not take the process down with it: park that
    // thread and let the report finish (the first fault is the one worth reading). Only a fault on the reporter itself, or a
    // fault after the report is written, ends the process.
    if (g_rep_thread != 0 && GetCurrentThreadId() != g_rep_thread && g_rep_state.load(std::memory_order_acquire) == 1) {
        std::fputs("nxrt: second exception on another thread while the report is being written; parking the thread\n", stderr);
        std::fflush(stderr);
        for (;;) Sleep(1000);
    }
    std::fputs("nxrt: second exception inside the crash handler, terminating\n", stderr);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), g_exit_code);
    for (;;) Sleep(1000);
}

// ---- the reporter thread ---------------------------------------------------------------------------------------------
// The report must not be built on the faulting thread: a stack overflow leaves no stack for it (the handler runs on the
// same thread), and that is exactly the crash a report is needed for. A thread created at crash_install waits for a
// request; the handler copies the exception record and the context here and spins until the report is written, or 2 s
// pass. Plain atomics and Sleep only, so it is safe from the signal handler.
EXCEPTION_RECORD g_rep_rec{};
CONTEXT g_rep_cx{};
DWORD g_rep_tid = 0;
DWORD g_rep_thread = 0;              // the reporter's own tid (0: not started): a fault there is not a second exception
std::atomic<int> g_rep_state{0};     // 0 idle, 1 requested, 2 written
std::atomic<bool> g_rep_started{false};

DWORD WINAPI reporter_main(LPVOID) {
    g_rep_thread = GetCurrentThreadId();
#if defined(__APPLE__)
    nxos_set_thread_name("crash-reporter");   // the fault note reads this name without a libc call
#endif
    for (;;) {
        if (g_rep_state.load(std::memory_order_acquire) == 1) {
            emit(build_report(&g_rep_rec, &g_rep_cx, g_rep_tid));
            g_rep_state.store(2, std::memory_order_release);
        }
        Sleep(1);
    }
}

// The faulting thread: hand the report over and wait (bounded: a report that cannot be built must not keep the process alive).
void report_via_helper(const EXCEPTION_RECORD* rec, const CONTEXT* cx, DWORD tid) {
    if (!g_rep_started.load(std::memory_order_acquire)) {
        emit(build_report(rec, cx, tid));
        return;
    }
    g_rep_rec = *rec;
    g_rep_cx = *cx;
    g_rep_tid = tid;
    g_rep_state.store(1, std::memory_order_release);
    for (int i = 0; i < 8000 && g_rep_state.load(std::memory_order_acquire) != 2; ++i) Sleep(1);
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW &&
        g_overflow_reported.exchange(false))
        return EXCEPTION_CONTINUE_SEARCH;  // the vectored handler already wrote this report
    if (g_in_handler.exchange(true)) second_exception();
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        g_in_handler = false;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    report_via_helper(ep->ExceptionRecord, ep->ContextRecord, GetCurrentThreadId());
    if (g_terminate) TerminateProcess(GetCurrentProcess(), g_exit_code);
    g_in_handler = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI overflow_thread(LPVOID) {
    emit(build_report(&g_ov_rec, &g_ov_ctx, g_ov_tid));
    if (g_terminate) TerminateProcess(GetCurrentProcess(), g_exit_code);
    return 0;
}

LONG WINAPI crash_vectored(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    if (g_in_handler.exchange(true)) second_exception();
    g_ov_rec = *ep->ExceptionRecord;
    g_ov_ctx = *ep->ContextRecord;
    g_ov_tid = GetCurrentThreadId();
    HANDLE th = CreateThread(nullptr, 0, overflow_thread, nullptr, 0, nullptr);
    if (th) {
        WaitForSingleObject(th, kOverflowWaitMs);
        CloseHandle(th);
        g_overflow_reported = true;
    }
    g_in_handler = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void crash_install(const CrashOptions& opt) {
    std::lock_guard<std::mutex> lk(g_install_mu);
    g_path_given = opt.report_path != nullptr;
    g_path = opt.report_path ? opt.report_path : "";
    g_terminate = opt.terminate;
    g_exit_code = opt.exit_code;
    g_on_report = opt.on_report;
    g_user = opt.user;
    if (g_installed) return;
#if !defined(_WIN32)
    symbols_snapshot_images();   // before the reporter starts: the report's symbolizer reads this table, never dyld
#endif
    SetUnhandledExceptionFilter(crash_filter);
    ULONG guarantee = 64 * 1024;  // stack reserved for the handlers if the thread overflows
    SetThreadStackGuarantee(&guarantee);
    AddVectoredExceptionHandler(0, crash_vectored);
    // The reporter: 1 MiB of its own stack, so the report survives a faulting thread with nothing left.
    if (HANDLE th = CreateThread(nullptr, 1u << 20, reporter_main, nullptr, 0, nullptr)) {
        CloseHandle(th);   // detached; the process is going away after the report anyway
        g_rep_started.store(true, std::memory_order_release);
    }
    g_installed = true;
}

std::string crash_build_report(void* exception_pointers) {
    auto* ep = static_cast<EXCEPTION_POINTERS*>(exception_pointers);
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return "HOST EXCEPTION: no exception information\n";
    return build_report(ep->ExceptionRecord, ep->ContextRecord, GetCurrentThreadId());
}

std::string crash_report_path() { return g_path_given ? g_path : default_report_path(); }

}  // namespace nxrt
