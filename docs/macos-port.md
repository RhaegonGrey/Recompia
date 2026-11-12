# macOS support: port plan

Status: **the game runs on Apple Silicon** — M0–M3 done, M4 done except Link Play against a peer and a real gamepad,
M5 done, M6 partial (launcher, the flat release zip, CI, the shader pipeline and the release pipeline done; movies untested,
and no signing by decision — the zip ships unsigned and its note covers the one-time Open Anyway).
§7 is the evidence log; §8 is the operational guide (build, lift, run, launcher, shader cache, memory, traps).
Tuning beyond the port itself — the 16 items a review of [Chaos192/PokoMac](https://github.com/Chaos192/PokoMac) turned up,
what is in the tree and how to pick up the rest — is `docs/macos-optimisations.md`.

Target: a native `arm64` macOS build of `pkp` (Apple Silicon), running the same lifted game libraries the Windows
build links.

The design sections were written against `main` at `5f98748`; the file/line references are from that tree and the counts
are `wc -l` on it. The port has since been rebased onto `main` at `d6e240e` (see §8), where the launcher was split into
modules and the texture-edit hook was removed.

---

## 1. What the port actually is

`nxrecomp` emits plain C++20 against one ABI header (`recomp/include/nxrecomp/ctx.h`). That header is already
portable: 2,090 lines, one x86-only construct (a `PAUSE` hint, guarded by `__x86_64__`), all atomics through
`__atomic_*` builtins, all float behaviour through bit manipulation (`fpexact.h`, `neon.h` have no intrinsics at
all). The recompiler itself is 6,438 lines of Python and has no host-architecture dependency.

So the recompilation pipeline needs **no** port. The work is entirely in `runtime/` (113,090 lines in `src/`,
4,829 in `include/`) plus build and packaging.

Measured coupling to Win32 (`#include <windows.h>`/`<winsock2.h>` or a Win32-only SDK header):

| Area | Files | Lines | What the Win32 dependency is |
|---|---|---|---|
| `runtime/src` + `runtime/games` + `runtime/apps` + `game-build/src` | 80 | 38,520 | 101 distinct Win32 APIs |
| of which, graphics (`src/gfx`) | 21 | 12,000 | SDL3 already; the rest is DXGI HDR query, XAudio2, `Sleep`, `_mm_pause`, `__cpuid` |
| of which, `src/mem` | 2 | 1,435 | `VirtualAlloc`/sections/VEH page faults |
| of which, `src/diag` | 11 | 2,500 | SEH crash handler, `__rdtsc`, registry, DbgHelp |
| of which, `src/net` | 2 | 2,110 | Winsock |
| of which, `src/media` | 1 | 1,539 | Media Foundation |
| of which, `apps/launcher` | 7 | 4,352 | raw Win32 UI (`CreateWindowExW`, `wWinMain`) |

The single most important measurement: **the 101 Win32 APIs are almost all boring portability surface** —
`SRWLOCK`, `CONDITION_VARIABLE`, `CreateThread`, `WaitForSingleObject`, `CreateWaitableTimerExW`,
`QueryPerformanceCounter`, `CreateFileW`/`ReadFile`, `WideCharToMultiByte`, `GetLastError`. Only about ten
things need genuine design work. That is the shape of this plan.

---

## 2. Decisions

### D1 — Target `arm64` only, floor macOS 13 (Ventura)

The guest is AArch64; the host being AArch64 removes a whole class of mismatches and every `-mcx16` /
`cmpxchg16b` / `_mm_pause` / `__rdtsc` / `__cpuid` problem at once (clang lowers `__atomic_compare_exchange_n` on
`unsigned __int128` to `CASP` on Apple Silicon). An `x86_64` macOS build is only interesting as an optional
staging step (see M2a) and is not a deliverable.

Rationale for the floor: MoltenVK 1.4.2 raised its minimum to macOS 12.0, SDL3 requires 10.15+, and Metal 3
(macOS 13) is what the swapchain path wants. Recommend macOS 14+ for the test matrix.

### D2 — Vulkan stays the only graphics API. MoltenVK ≥ 1.4.2 supplies it.

`runtime/src/gfx/vk_device.cpp:365` hard-fails below Vulkan 1.3, and `:481` additionally requires
`dynamicRendering` and `synchronization2`. MoltenVK added Vulkan 1.3 in **v1.3.0** (2025-04-27), Vulkan 1.4 in
v1.4.0, and the current 1.4.x line also adds `VK_EXT_depth_clip_control` (which `vk_device.cpp:583` probes) and
`VK_EXT_external_memory_metal`. So the requirement is met. Writing a Metal backend instead would be a rewrite of
12,000 lines of NVN interception; rejected.

Concrete change: `runtime/cmake/gfx_deps.cmake:68` currently pins `VK_USE_PLATFORM_WIN32_KHR` on `nxrt_volk`. On
macOS it becomes `VK_USE_PLATFORM_METAL_EXT` (SDL3's `SDL_Vulkan_GetInstanceExtensions` reports
`VK_EXT_metal_surface`), and the MoltenVK ICD + `libvulkan.1.dylib` are bundled in the app.

### D3 — `[nvn] pool_copy = on` is mandatory on macOS, and `auto_copy()` must say so

`VK_EXT_external_memory_host` does not exist on MoltenVK (it is not in any MoltenVK release note; the Metal-shaped
equivalent is `VK_EXT_external_memory_metal`, which imports `MTLBuffer`, not raw host pointers).
`runtime/src/gfx/nvn/nvn_gpu_pool.cpp:296` already refuses to import in that case and the whole shadow-copy
implementation exists (`nvn_pool_copy.cpp`, 649 lines; `[nvn] pool_copy = auto|on|off`).

The bug to fix: `auto_copy()` at `runtime/src/gfx/nvn/nvn_pool_copy.cpp:71-88` turns the copy path on only for
Wine or an *integrated* GPU with ≤ 8 GiB device-local memory. MoltenVK reports
`VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU` with a unified heap the size of system RAM, so on a 24 GB Mac `auto`
picks **off** and every pool import fails. Add a platform term: on macOS, `auto` ⇒ `true`.

Cost of the copy path: every guest→GPU buffer range is a host-visible shadow, copied on first touch per serial and
written back before a serial is published (`nvn_gpu.cpp:212-214`). That is the same path Steam Deck / Wine users
already run, so it is exercised, not speculative.

### D4 — Pass ASTC through instead of transcoding to BC7

`runtime/src/gfx/nvn/nvn_texdata.cpp` decodes ASTC and re-encodes to `VK_FORMAT_BC7_*` via `tools/textures`
(`astc2bc7` + `bc7e.ispc`, MSVC `.lib`s, ISPC, plus `astcdec-*-shared.dll` loaded at run time —
`runtime/cmake/gfx.cmake:21-28`). Apple GPUs support ASTC natively, so on macOS the right move is
`VK_FORMAT_ASTC_*_BLOCK` straight from the tiled bytes.

This deletes three build dependencies (ISPC, bc7enc_rdo, the MSVC static libs), the runtime DLL loading, the
`bc7_cache` directory, and a per-texture CPU transcode. Keep the `astc2bc7` path compiled on Windows only.

### D5 — Guest threads are pthreads; guest stacks stay emulated

`recomp/include/nxrecomp/ctx.h:47` keeps `x[31]` (SP) **inside** `Ctx`, and `runtime/src/kernel/thread.cpp:115`
sets `t->ctx.x[31] = p.stack_top` — the guest stack at `mem::kStackBase` (`0x7C00000000`) is ordinary mapped
memory that the lifted code addresses explicitly. The host thread's own stack is only used by
`host_thread_main`.

Consequence: **no host stack switching, no `pthread_attr_setstack`, no ucontext trampoline.** `CreateThread` →
`pthread_create` with a default stack, and the `[threads] host_stack_mb = 64` knob becomes a no-op (or a
`pthread_attr_setstacksize`). This was the single biggest suspected blocker and it is not one.

### D6 — A POSIX shim under `runtime/src/os/`, not 80 rewrites

The 101 APIs collapse into ~40 shim functions. Implementing them once (header-only `nx_os.h` plus
`os_posix.cpp`) lets `os/sync.cpp` (595 lines of `SRWLOCK`/`CONDITION_VARIABLE`),
`kernel/kernel_sync.cpp`, `kernel/sched.cpp`, `kernel/precise_wait.cpp`, `diag/*`, `services/*` and
`net/*` compile with only an `#include` change.

The alternative — rewriting every call site — touches 38,520 lines and puts the working Windows build at risk on
every step. Rejected.

The shim provides, on POSIX: `SRWLOCK`, `CONDITION_VARIABLE`, `CRITICAL_SECTION`, `HANDLE` (a fd/`sem_t`/pointer
tagged union), `CreateEventW`, `CreateMutexW`, `WaitForSingleObject`, `SetEvent`/`ResetEvent`,
`CreateWaitableTimerExW`/`SetWaitableTimer`/`CancelWaitableTimer` (→ `kqueue` `EVFILT_TIMER` or
`timerfd`-equivalent via `dispatch_source`), `CreateThread`/`ResumeThread`/`ExitThread`, `Sleep`,
`QueryPerformanceCounter`/`Frequency` (→ `mach_absolute_time`), `GetTickCount64`, `GetLastError`/`SetLastError`
(→ `errno` thread-local), `CreateFileW`/`ReadFile`/`WriteFile`/`SetFilePointerEx`/`GetFileSizeEx` (→ `open`/`pread`,
UTF-16 → UTF-8 at the boundary), `WideCharToMultiByte`/`MultiByteToWideChar` (→
`CFStringConvert`/`iconv`-free hand-rolled UTF-8↔UTF-16), `GetModuleFileNameA` (→ `_NSGetExecutablePath`),
`LoadLibraryW`/`GetProcAddress`/`FreeLibrary` (→ `dlopen`/`dlsym`/`dlclose`), `GetSystemInfo` (→ `sysctl`),
`VirtualAlloc`-family (thin wrappers over `mem::`, see D7).

Everything the shim cannot honestly provide (`SetThreadAffinityMask`, `GetCurrentProcessorNumber`,
`SetThreadPriorityBoost`, `timeBeginPeriod`) gets a no-op or a documented degradation; see R3.

### D7 — `mem::` moves to `mmap` + `mach_vm`, and guest pages stop being 4 KiB on the host

`runtime/src/mem/memory.cpp` (768 lines) is `VirtualAlloc`-based end to end, plus one
`CreateFileMappingW`+`MapViewOfFile` section for the heap (`:479-500`). On macOS:

- `reserve_fixed` → `mmap(PROT_NONE, MAP_PRIVATE|MAP_ANON)` with the address as a *hint*, then verify the
  returned pointer equals the request and fail cleanly if not (`MAP_FIXED` clobbers silently, and
  `MAP_FIXED_NOREPLACE` does not exist). The layout constants in `runtime/include/nxrt/memory.h:14-28`
  (`kMainBase 0x7100000000` … `kKernelBase 0x7E00000000`) all sit inside the 47-bit arm64 user VA space and
  clear of the dyld shared cache, so they are reusable — verify empirically in M1.
- `commit`/`decommit` → `mprotect(PROT_READ|PROT_WRITE)` / `madvise(MADV_FREE_REUSABLE)`.
- `heap_create` → `mmap` a file-backed object (`shm_open` or a temp file) so `heap_alias`/`heap_private_view`
  can `mmap` the same object at several addresses. `heap_has_section()` stays true, which keeps
  `heap_private_view` (the fast GPU-import path, `memory.h:74`) working.
- **Page size.** `kPage = 0x1000` (`memory.h:29`) is guest semantics and must stay 4 KiB. The host page on
  Apple Silicon is **16 KiB**. Every host-facing call therefore needs a host-page rounding
  (`host_page = sysconf(_SC_PAGESIZE)`), and `heap_alias(offset)` with a 4 KiB offset must round down to 16 KiB —
  which changes the meaning of "the alias starts exactly here". Introduce `host_align()` and keep the guest page
  table at 4 KiB (it already is, `g_commit_bits`/`g_heap_cls` index by `kPage`).
- `Prot::RX`/`Prot::RWX` → `PROT_READ` / `PROT_READ|PROT_WRITE`. Guest code is **never executed** — the only
  caller is the ELF segment mapper at `runtime/src/loader/module.cpp:520`, and execution goes through the lifted
  C++. Keeping exec bits off keeps the whole build clear of `MAP_JIT`, `allow-jit`, and W^X entitlement questions.

### D8 — `SIGSEGV` for the two fault-driven features (revised: was "mach exception ports")

> **Revised during implementation.** This section first chose mach exception ports. A BSD signal handler is the
> better fit and is what `src/os/os_posix.cpp` now installs: it runs on the *faulting* thread and returning from it
> resumes the faulting instruction, which is exactly what `write_watch.cpp` needs (un-protect the page, return, the
> store retries). A mach port hands the fault to a helper thread, which then has to find the faulting thread, call
> `thread_get_state`, decide, `thread_set_state` and reply `KERN_SUCCESS` — more moving parts for the same result,
> and the `sigreturn` concern below turned out not to apply: no `sigreturn` is involved when the handler simply
> returns. The `CONTEXT` is built from `ucontext_t` (`uc_mcontext->__ss` on arm64), so a crash report carries the
> real pc/lr/sp/fp, and `RaiseException` runs the same handler list in-process. What still needs the mach work is
> `GetThreadContext`/`SuspendThread` on *another* thread (the diag sampler and the starvation guard), which the shim
> reports as unsupported rather than faking.

`runtime/src/mem/write_watch.cpp` (667 lines) turns guest pages read-only and services the resulting fault to
implement per-page write tracking (`on_fault` at `:269`, a vectored exception handler). `runtime/src/diag/crash.cpp`
(344 lines) is the crash reporter. Both need the same primitive: *see the fault, fix the state, resume the
thread*, and `AddVectoredExceptionHandler`/`RaiseException`/`SetUnhandledExceptionFilter` in the shim give it to
them over `sigaction(SIGSEGV | SIGBUS | SIGILL | SIGFPE | SIGTRAP)`.

`write_watch` is load-bearing: `nvn_gpu_texture.cpp`, `nvn_gpu_pool.cpp`, `nvn_pool_copy.cpp`,
`services/fs_romfs.cpp`, `services/fs_volume.cpp` all consult it. Plan for it to be **deferred, not skipped**:
M5. Until then, a compile-time `NX_WW_POLL` fallback that arms nothing and reports "everything may have been
written" keeps the renderer correct at a cost in upload bandwidth — useful for bringing the game up in M3/M4
before the exception server exists.

### D9 — Audio through a CoreAudio `AudioSink`

`runtime/include/nxrt/audio_sink.h` is a clean 5-method interface and `runtime/src/services/audio_standin.h`
already provides a clock-only sink for tests. `audio_xaudio2.cpp` (611 lines) becomes `audio_coreaudio.cpp`
(AVAudioEngine or a raw `AudioUnit`/`AudioQueue` output unit with the same non-blocking `write()` contract, the
same underrun accounting, and the same device-loss → stand-in → retry ladder). Selectable via `[audio] sink`.

### D10 — Video: the lifted library plus the host VP9 decoder, not AVFoundation

The original plan was an AVFoundation backend mirroring `movie_mf.cpp`. The game's two movies settle it differently:
`movie_asset/ounce/MV01.webm` and `MV02.webm` are VP9 in WebM (370 MB and 136 MB), and AVFoundation has no WebM demuxer.

They do not need one. The lifted `movie::` library in subsdk0 carries the Matroska demuxer (the nandroid port) and calls
`nn::codec` for the VP9 decode, which `src/media/video_codec.cpp` already answers with libvpx (pools, NV12 readback and
fences included). That is the same path Windows calls `[media] movie = off`; on macOS it is the only path, because
Media Foundation does not exist there. What is missing is a test, not a backend: no run has reached a movie yet.

### D11 — Launcher: use the Tauri one; leave `apps/launcher` to the end

> **M5 cross-thread suspend done 2026-11-10.** `SuspendThread` is `thread_suspend` on the thread's mach port
> (`pthread_mach_thread_np`) and `GetThreadContext` is `thread_get_state(ARM_THREAD_STATE64)`, so the sampler and the
> freeze watchdog read a stopped register set of *any* thread and walk its stack. Verified with `[prof] sample_hz = 200`:
> 2,688 samples across main, nvn-render and 40+ guest threads, each with a real host stack.

> **Audio (D9) done 2026-11-10.** `services/audio_coreaudio.cpp`: an AudioUnit (default output, or `[audio] device` by UID) at
> the guest's rate and channel count, fed from an 8192-frame single-producer ring; the render callback applies the master
> volume and mute and counts underruns, `has_device_clock()` is true so the renderer paces by the queue. Measured: 48 kHz
> stereo, 47 ms average queue, zero underruns, zero drops, guest delivering 48,173 frames/s.

> **Done (2026-11-10).** `launcher/` builds with `npm run tauri build` (Rust 1.99 from Homebrew, Tauri CLI 2.x) and runs
> on macOS: a native `pkp` instead of `pkp.exe`, no Proton (`host.rs`), no shader tools or "Set up" (`gamebuild.rs`,
> `play.rs`), and a readiness check that only needs the binary and its `pkp.ini`. Play starts the game windowed.

`runtime/apps/launcher` is a raw Win32 UI (`wWinMain` at `main.cpp:803`, `CreateWindowExW` at `:748`, 4,352
lines). `launcher/` is Tauri 2 (12,626 lines of Rust, 5,206 of JS) and is already cross-platform. For macOS,
M1–M5 run from a hand-written `pkp.ini` and the CLI; the launcher comes last, and the cheapest correct answer is
to make the Tauri app drive the macOS flow rather than port 4,352 lines of Win32.

### D12 — Keep Windows green

Every milestone is additive: `#ifdef`s live in `runtime/src/os/` and the CMake presets, not scattered through the
call sites. `runtime/CMakeLists.txt:18` (`message(FATAL_ERROR "nxrt targets native Windows x64 only")`) becomes a
platform branch, not a deletion. The existing `.github/workflows/release.yml` stays untouched; a new
`macos.yml` builds the macOS artifact.

### D13 — a mod's HTTPS request is the system's libcurl

`src/net/http_client.cpp` was WinHTTP only, so the macOS build refused every request a mod made (`MT_ERR_STATE`). It now
`dlopen`s `/usr/lib/libcurl.4.dylib`, which every macOS keeps in the dyld shared cache, and declares the ten easy-interface
calls itself so curl's headers stay out of the build. Nothing is linked, so `libpkp_rt.dylib` keeps its short `NEEDED`
list and a host without the library refuses the request rather than failing the start. The rules are the WinHTTP ones
(https only but for localhost, no redirect, cookie or sign-in, TLS 1.2 or later, 256 KiB cap, one time limit per step);
the same block runs on Linux (`docs/linux-port.md` §L9), where it was measured against libcurl 8.5.0. **Not yet run on
macOS**: it compiles there, but no macOS run has made a request.

---

## 3. Milestones

Each milestone has a hard acceptance criterion. Nothing advances on "it compiles".

### M0 — Build skeleton (no game, no window)

Work:

1. `runtime/CMakeLists.txt` — replace the `if(NOT WIN32) FATAL_ERROR` with `if(WIN32) … elseif(APPLE) … else()
   FATAL_ERROR`. Guard the MSVC-runtime block, `NOMINMAX`/`_WIN32_WINNT` defines, `/W3` and `psapi` behind `WIN32`.
2. New `runtime/CMakePresets.json` preset `macos-arm64` (Ninja, `CMAKE_OSX_ARCHITECTURES=arm64`,
   `CMAKE_OSX_DEPLOYMENT_TARGET=13.0`, `-O2`, `-fno-strict-aliasing -ffp-contract=off`).
3. `runtime/src/os/nx_os.h` + `os_posix.cpp` — the shim from D6, growing as files are ported.
4. `runtime/cmake/gfx_deps.cmake` — `NXRT_DEPS_DIR` defaults to `$HOME/Library/Caches/nxrt/deps` (currently
   `$ENV{LOCALAPPDATA}`), `VK_USE_PLATFORM_METAL_EXT` on macOS, and MoltenVK as a `FetchContent` pin
   (`MoltenVK-macos.tar`, ≥ 1.4.2).
5. `recomp/cmake/NxRecomp.cmake` — drop `-mcx16` on non-x86 (`:99-102`: both the MSVC and the non-MSVC branch hard-code
   it; make it conditional on `CMAKE_SYSTEM_PROCESSOR` matching `x86_64|AMD64`). `game-build/release_b/pkp_build.py:65,529` needs a `-target arm64-apple-macos` variant.

Acceptance: `cmake --preset macos-arm64 && cmake --build` produces `libnxrt.a` and `libnxrt_gfx.a` on an M-series
Mac with the same warning set as Windows. No behaviour yet.

### M1 — Memory, kernel, timers (headless)

Work:

1. `runtime/src/mem/memory.cpp` — the D7 rewrite: `reserve_fixed`/`release_fixed`/`commit`/`decommit`/`protect`/
   `is_mapped`/`heap_*` over `mmap`/`mprotect`/`madvise`/`mach_vm`, with the 4 KiB guest / 16 KiB host split.
2. `runtime/src/kernel/svc_memory.cpp` (822 lines) — `VirtualAlloc`/`VirtualFree`/`VirtualQuery` call sites move to
   `mem::` (most already should be; the four `VirtualAlloc` uses are the private allocations).
3. `runtime/src/kernel/thread.cpp` — `CreateThread`→`pthread_create`, `DuplicateHandle`→`pthread_t`,
   `ExitThread`→`pthread_exit`, `CloseHandle` on the thread handle dropped.
4. `runtime/src/kernel/{time,precise_wait,busy_mutex,starvation_guard,sched}.cpp` — the shim's waitable timers and
   `mach_absolute_time`; `sched.cpp`'s affinity/priority calls (`:289-335`) degrade to `thread_policy_set`
   `THREAD_AFFINITY_POLICY` (a hint on macOS, and it cannot split performance/efficiency cores) or to nothing.
5. `runtime/src/os/sync.cpp`, `kernel/kernel_sync.cpp` — compile through the shim unchanged.
6. `runtime/src/diag/{log,registry,sampler,host_probe,watchdog,gauge}.cpp` — `GetLastError`→shim, registry reads
   (`host_device.cpp` SMBIOS at `:36-40`) → `sysctl hw.model` / `IOKit`.

Acceptance: the `nxrt` self-tests and `runtime/src/testtools` (`tt_runner`) build and run headless; the memory
unit tests (fixed reservations at `0x7100000000`, heap commit/decommit/alias, `vm_query`) pass. Guest threads
create/join and `svcSleepThread` hits its target within 1 ms.

### M2 — Graphics on MoltenVK

Work:

1. `runtime/src/gfx/platform.cpp` — `native_hwnd()` (`:446`) → `native_nsview()` reading
   `SDL_PROP_WINDOW_COCOA_WINDOW_POINTER`; `set_fullscreen` (`FullscreenMode::Exclusive`) maps to
   borderless-fullscreen on macOS (there is no exclusive mode outside a `CGDisplayCapture` hack, which is not
   worth it); `vulkan_instance_extensions`/`create_vulkan_surface` already delegate to SDL, so they work once
   `VK_USE_PLATFORM_METAL_EXT` is set.
2. `runtime/src/gfx/vk_device.cpp` — expect `external_memory_host == false`, `custom_border_color == false`,
   `push_descriptor == false` (all three are already probed and degrade: `:604`, `:662`, `:606`). Log the
   degradations once, at `INFO`. Add a `VK_MVK_moltenvk` config hook so `MVK_CONFIG_*` can be set from `pkp.ini`.
3. `runtime/src/gfx/nvn/nvn_pool_copy.cpp:71-88` — macOS ⇒ `auto_copy() == true` (D3).
4. `runtime/src/gfx/nvn/nvn_texdata.cpp` — the ASTC passthrough branch (D4), replacing the
   `encode_bc7_banded` path at `:367`.
5. `runtime/src/gfx/extras/win_display.cpp` — either an `IOKit`/`CGDisplay` HDR query or, for M2,
   `query_display_hdr` returning `valid = false` and HDR off. Do not pretend.
6. `runtime/src/gfx/extras/temporal_dlss.cpp`, `temporal_ffx.cpp` — compile out on macOS (Streamline and FFX are
   Windows/Linux; both already runtime-load and fall back). FSR1 (`fsr1_spatial.cpp`) and NIS
   (`nis_spatial.cpp`) are plain compute shaders and stay.
7. `runtime/src/gfx/frame_limiter.cpp:9,105` — `_mm_pause` → `__builtin_arm_yield()` / `asm volatile("yield")`.
8. `runtime/src/gfx/nvn/nvn_shaders.cpp:58-62` — `__cpuid`/`__cpuidex` (CPU feature detection for shader
   preload) → `sysctlbyname("hw.optional.arm.FEAT_*")` or a `#if defined(__aarch64__)` no-op.

Acceptance: `nxrt_gfx_demo` opens a window through SDL3 + MoltenVK, renders its scene, and shows the ImGui F1
overlay. Frame pacing is stable and the log shows the three feature degradations exactly once.

**M2a (optional, cheap de-risk).** Build `x86_64` and run under Rosetta 2. It keeps `cmpxchg16b`, `_mm_pause`,
`__rdtsc` and `__cpuid` working, so it isolates *graphics-stack* problems from *arm64* problems. Not a
deliverable, but a good way to unblock M2 if a MoltenVK issue is ambiguous.

### M3 — Game bring-up

Work:

1. `recomp/scripts/build_game.cmd` equivalent for macOS: lift the four NSOs (`recomp/games/pkp/config.py`
   modules: main `0x7100000000`, sdk `0x7500000000`, subsdk0 `0x7600000000`, rtld `0x7700000000`) with the local
   Python and compile the shards with Apple clang.
2. `game-build/CMakeLists.txt` — the imported `.lib` block becomes platform-conditional (`.a` on macOS); drop
   `/MAP /STACK:134217728 /HIGHENTROPYVA:NO` and `-clang:-fmacro-prefix-map` (use
   `-ffile-prefix-map` directly); the `DejaVuSans.ttf` and `astcdec-*.dll` copies go (D4).
3. `game-build/src/pkp_main.cpp` (542 lines) — the entry point is already `int main(int argc, char** argv)`
   (`:317`), so only the two `MessageBoxA` calls (`:113`, `:309`) need an `NSAlert`/`fprintf(stderr)` path, plus the
   `PKP_GUI_SUBSYSTEM` linker flag (`game-build/CMakeLists.txt:84-85`, `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup`)
   which has no macOS equivalent: on macOS the process is a normal executable, and "no console window" comes from
   starting it outside a terminal, not from the linker.
4. `runtime/src/services/fs_romfs.cpp:537-564` and `fs_volume.cpp` — `CreateFileW`/`ReadFile` with
   `FILE_SHARE_*` and overlapped reads → `open(O_RDONLY)`/`pread` through the shim (the `mem::ww::OsWriteScope`
   around `:556` stays; it is what keeps the write tracker honest).
5. `runtime/src/services/{time,timezone,pl,acc,album,misc,common,logfault}.cpp` — `GetSystemTimePreciseAsFileTime`
   → `clock_gettime(CLOCK_REALTIME)`, `GetDynamicTimeZoneInformation` → `CFTimeZone`/`TZ`, `pl` power state →
   `IOPMrootDomain`, album/screenshot → `NSImage`/`CGImage` write.

Acceptance: the game boots past the title screen and reaches gameplay. This is the milestone that proves D3,
D4, D5 and D7 together; expect it to be where the surprises are.

### M4 — Platform services

> **Status 2026-11-10.** Audio: done (`audio_coreaudio.cpp`). Device list: done (`audio_devices_posix.cpp`). Keyboard and
> mouse: done through SDL3 (`pkp_kbm.cpp` has no Windows calls on the POSIX path). Relay: `relay_server.cpp` binds and
> advertises the LAN address on macOS; the guest side has not been run against a `pokopia-native` peer. Open: a gamepad
> with a real pad, the `fs_volume` XPC check, and a `tt_runner` executable (the testtools code is in the library; no
> target builds a runner).

Work:

1. `runtime/src/services/audio_xaudio2.cpp` → `audio_coreaudio.cpp` (D9), plus
   `runtime/src/services/audio_devices.cpp` (90 lines, device enumeration) → CoreAudio device list.
2. `runtime/games/pkp/input/pkp_kbm.cpp` (777 lines) — keyboard/mouse mapping; it uses `windows.h` for
   `GetKeyboardLayout`/`MapVirtualKey`/`ToUnicode`, all of which become SDL3 scancode/keycode data
   (SDL3 is already linked and already delivers the events via `src/gfx/input.cpp`).
3. `runtime/games/pkp/core/pkp_save_uuid.cpp` (242 lines) — filesystem only; through the shim.
4. `runtime/src/net/relay_server.cpp` (1,095) and `lobby.cpp` (1,015) — Winsock → BSD sockets. Mostly
   `closesocket`→`close`, `WSAGetLastError`→`errno`, `ioctlsocket(FIONBIO)`→`fcntl(O_NONBLOCK)`,
   `SIO_UDP_CONNRESET` dropped, `WSAStartup` dropped.
5. `runtime/src/timestep/timestep.cpp`, `runtime/src/testtools/*` — `QueryPerformanceCounter` → shim.

Acceptance: audio plays through the built-in speakers and a USB interface; keyboard/mouse and a Bluetooth
controller drive the game; Link Play connects to a `pokopia-native` peer; the test-tools runner replays a script.

### M5 — Fault-driven features and diagnostics

> **Status 2026-11-10.** Write tracker and crash reports: done on BSD signal handlers (D8 revised; no exception-port
> server). Cross-thread capture: done — `SuspendThread` is `thread_suspend` on the mach port and `GetThreadContext` is
> `thread_get_state(ARM_THREAD_STATE64)`, so the sampler and the freeze watchdog read stopped register sets and walk
> stacks (2,688 samples over main, nvn-render and 40+ guest threads). Symbols: `tools/symbolize.py` maps a report's
> absolute host addresses through `atos` and its guest frame chain through the `_f_<guest address>` names (the faulting pc
> of a deliberate SIGSEGV reads `f_75003567a0 + 13960`, the chain `f_7101fea580+0x11c <- …`); folded profiles resolve the
> same way.

Work:

1. `runtime/src/os/mach_fault.cpp` — the exception-port server from D8, with a clean `fault_register(range,
   handler)` API that `write_watch` and `crash` both use.
2. `runtime/src/mem/write_watch.cpp` — replace `on_fault` (`:269`) with a registration on the server; replace
   `VirtualProtect` with `mprotect`; the `releasing`/`wsum` bookkeeping is architecture-independent and stays.
3. `runtime/src/diag/crash.cpp` — rewrite against the server: `thread_get_state` for registers, `dladdr` +
   `_Unwind_Backtrace` for stacks, `VM_REGION` walk for the memory map. `runtime/src/diag/symbols.cpp` (371
   lines) — DbgHelp/PDB → Mach-O `LC_SYMTAB` reading, or `atos`/`dSYM` on demand.
4. `runtime/src/diag/ftrace.cpp:113-245` — `__rdtsc` → `mach_absolute_time` (or `cntvct_el0`); the TSC/HPET
   calibration at `:115` disappears.
5. `runtime/src/eh/unwind.cpp:25` — `windows.h` is included only for `TerminateProcess`
   (`:558`, the uncaught-exception path) → `abort()`/`_exit()`. The unwinder itself is already guest-state-only.

Acceptance: the write tracker's unit tests pass; a deliberately crashing build produces a report with a symbolised
native stack; `[diag] ftrace` timings are sane.

### M6 — Media, launcher, packaging

> **Status 2026-11-12.** Launcher: done, including the Steam shortcut (macOS keeps Steam in `Library/Application
> Support`). CI: `.github/workflows/macos.yml`. libvpx: found in Homebrew, so VP9/VP8 decode on the host, which is what the
> movies need (D10). Open: a run that reaches a movie, and an `arm64-apple-macos` target in `pkp_build.py`.
>
> Packaging: **no `.app`, no signature.** `make-mac.sh` builds one zip whose root is `PokopiaRecomp-<version>/` — the launcher
> binary, `tools/` (the lifter, the SDK sources, the shader harness and MoltenVK) and `libpkp_rt.dylib` — the same shape as
> the Windows and Linux packages. The player unpacks it anywhere and starts `PokopiaLauncher` from there; macOS blocks the
> first start, and the zip's `How to open Recompia.txt` walks them through System Settings, Privacy and Security, "Open
> Anyway" (or `xattr -dr com.apple.quarantine <folder>`). A bundle was tried and dropped: signing it needs every nested code
> object signed, and the shader harness is a self-contained .NET publish whose assemblies are PE files `codesign` cannot sign,
> so every signature attempt failed and left the zip unsigned anyway.
>
> In-place updates: the zip is the whole macOS artifact, and `update.rs` takes `PokopiaRecomp-<version>/` as the install root
> (`find_package`, `exe_dir()`) — the launcher and `tools/` are replaced in place, as on Windows and Linux. The runtime dylib
> is in the package twice (beside the launcher, and in `tools/build/sdk`, where Set up copies it from into the game folder);
> after the swap `update.rs` brings the copies already on disk along with the one the update replaced. Nothing is signed, so
> there is no seal to renew and no `codesign` call. An update writes its own files, so no download flag comes back.

Work:

1. Movies: play one. The lifted `movie::` demuxer and the host VP9 decoder are both in place (D10); nothing has reached
   a movie yet.
2. `launcher/` — macOS build of the Tauri app: replace the `pkp.exe`/`vulkan-1.dll`/`.bat` assumptions, point the
   shader-cache pipeline at the bundled `spirv_harness`, keep the disk-space and Vulkan checks
   (`launcher/README.md` steps 1–3).
3. Packaging: decided — no `.app`, no signature (see M6 above). A flat `PokopiaRecomp-<version>/` zip, as on the other
   platforms. If a bundle ever comes back, note that `codesign` cannot sign the harness's .NET assemblies, so `--deep` fails
   and the bundle ships unsigned.
4. Signing: not done on purpose. Only a Developer ID + notarization would make Gatekeeper wave the app through, and both need
   an Apple account; the zip's note covers the one-time approval instead.
5. `game-build/release_b/pkp_build.py` — a `--target arm64-apple-macos` path using the system clang (or a pinned
   LLVM tarball), same content-addressed `ObjCache`, same `@progress`/`@error` protocol.
6. CI: `.github/workflows/macos.yml` on `macos-14` (arm64), build + `nxrt_gfx_demo` smoke test.

Acceptance: a second Mac unpacks the release zip, starts `PokopiaLauncher`, allows it once in System Settings and plays the
game with its own dump.

---

## 4. Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | The address layout (`kMainBase 0x7100000000` … `kKernelBase 0x7E00000000`, `memory.h:14-28`) collides with the dyld shared cache or the arm64 main-thread stack on some macOS version | **Happened** | High | Measured on macOS 27/M3: the kernel's 385 GiB `GUARD` region (`0xfc0000000`–`0x7000000000`) covers the old alias base, and libmalloc's xzone ranges (two ~12 GiB `PROT_NONE` at random addresses in `0x7000000000`–`0x8000000000`) hit the SDK bases in most runs. Fixed by reserving `0x7000000000` + 72 GiB in the executable's own Mach-O header (`runtime/cmake/guest_segment.py`, the `__GUEST` segment) before dyld runs, using `MAP_FIXED` inside it, and moving the alias window to `0x7300000000` and the HLE thunks to `0x7F00000000`. `mem::init` verifies the layout is inside the segment |
| R2 | 16 KiB host pages break `heap_alias` at 4 KiB granularity, and something depends on alias identity | Medium | High | D7's `host_align()`; add an assertion that every alias request is host-aligned and that `heap_offset_of` still resolves; the NVN pool chunker already works in 64 KiB units (`kAllocGranularity`) |
| R3 | macOS gives no core pinning and no priority boost, so the console-core model in `kernel/sched.cpp` degrades; audio/render jitter follows | High | Medium | Accept: this is exactly what the Linux/Proton path already does. Add a `[threads] core_model = off` documented mode and measure frame-time variance against the Windows build |
| R4 | `pool_copy` (the only viable pool path, D3) is slower than import; on unified memory the copy is pure overhead | High | Medium | It is already the Wine path; measure. Long term, `VK_EXT_external_memory_metal` (MoltenVK 1.3+) can wrap an `MTLBuffer` — but the guest heap would have to *be* an `MTLBuffer`, which is a bigger change. Defer |
| R5 | MoltenVK gaps the NVN emulation leans on (`VK_EXT_custom_border_color` absent, no `VK_EXT_external_memory_host`, argument-buffer limits) | Medium | Medium | All are probed and degrade today; the visible cost is a few sampler border colours. Budget one pass of visual diffing against Windows (the repo already has `tools/test/imgdiff.py` and `tools/test/timelines/*.nxs`) |
| R6 | Mach exception ports interfere with the Swift/ObjC runtime or the debugger | Low | High | Isolate in one file behind `fault_register`; make it opt-out (`[diag] fault_server = off`) so a crash under lldb stays debuggable |
| R7 | Rosetta-free arm64 changes FP results enough to break the `recomp` differential oracle | Low | Medium | The oracle runs on Unicorn, not the host — it is unaffected. Only `fpexact.h`'s "host result when not NaN" shortcut (`:179`) depends on host FP; arm64 and x86-64 are both IEEE-754 with the same results here, but re-run the KA suite on arm64 as a check |
| R8 | Codesigning/notarization friction with a bundled Vulkan ICD and a runtime-loaded dylib | Medium | Medium | M6 explicitly; test `codesign --verify --deep --strict` early, and keep an unsigned local build for development |
| R9 | The Windows build regresses from shim-related header changes | Medium | High | D12: CI builds both on every push from M0 onward, and the shim is additive |

---

## 5. Ordering and cost

| Milestone | Depends on | Rough size |
|---|---|---|
| M0 build skeleton | — | small: 5 files, new preset, the shim header |
| M1 memory + kernel | M0 | large: `memory.cpp` (768) + `svc_memory.cpp` (822) + 6 kernel files |
| M2 graphics | M1 | medium: 8 files, mostly feature degradation; the ASTC branch is the only new logic |
| M3 game bring-up | M1, M2 | medium: build glue + 8 service files; the risk is in the debugging, not the writing |
| M4 services | M3 | large: audio (new, ~600), net (~2,100 through a shim), input (~800) |
| M5 faults + diag | M1 | large: the Mach server (new) + `write_watch` (667) + `crash` (344) + `symbols` (371) |
| M6 media + packaging | M4 | large: `movie_avf` (new, ~1,500) + Tauri + signing |

M1–M3 is the critical path; everything after M3 is a feature-completeness question rather than a "will it work".

### The first five commits

Ordered so each one is reviewable and leaves both platforms building.

1. `build: make runtime/CMakeLists.txt platform-aware` — `runtime/CMakeLists.txt:17-18` becomes a branch;
   `WIN32`-only defines, the MSVC runtime block, `/W3`, `psapi` and the `launcher.rc`/`/MANIFEST:NO` block
   (`:61-69`) move inside it. Add `runtime/CMakePresets.json`. No behaviour change on Windows.
2. `build: add runtime/src/os/nx_os.h, the POSIX shim header` — types and inline functions only, no call-site
   edits. `runtime/src/os/` exists but is not in any target yet.
3. `build: os_posix.cpp behind APPLE, port diag/log.cpp and os/sync.cpp` — the first two real consumers; proves
   the shim covers `SRWLOCK`/`CONDITION_VARIABLE`/`GetLastError`/file IO.
4. `mem: split guest page (4 KiB) from host page` — the `host_align()` change plus `mmap`-based
   `reserve_fixed`/`commit`/`decommit`/`protect` behind `APPLE`, with the Windows path untouched. Ship with a unit
   test that reserves `0x7100000000` and round-trips commit/decommit/alias.
5. `gfx: VK_USE_PLATFORM_METAL_EXT and a MoltenVK FetchContent pin` — `runtime/cmake/gfx_deps.cmake:68`, plus the
   macOS term in `auto_copy()`. First commit after which `nxrt_gfx_demo` has a chance of showing a window.

---

## 6. Appendix: Win32 → POSIX mapping

Counts are files that reference the API (of the 80 coupled files).

| Win32 | Uses | macOS |
|---|---|---|
| `GetCurrentProcess`, `GetCurrentProcessId` | 18, 5 | `getpid()` / no-op |
| `Sleep` | 15 | `nanosleep` |
| `TerminateProcess` | 13 | `_exit` / `abort` (shim) |
| `GetCurrentThreadId`, `GetCurrentThread` | 13, 11 | `pthread_mach_thread_np` / `pthread_self` |
| `GetProcAddress`, `LoadLibraryW`, `FreeLibrary` | 11, 2, 1 | `dlsym`, `dlopen`, `dlclose` |
| `GetLastError` | 11 | thread-local `errno` |
| `QueryPerformanceCounter`/`Frequency` | 10, 9 | `mach_absolute_time`, `mach_timebase_info` |
| `WideCharToMultiByte`, `MultiByteToWideChar` | 10, 6 | hand-rolled UTF-8↔UTF-16 (no ICU dependency) |
| `WaitForSingleObject`, `WaitForMultipleObjects` | 9, 1 | `sem_wait`/`pthread_cond_wait`/`kevent` |
| `GetModuleFileNameA/W` | 9, 4 | `_NSGetExecutablePath` |
| `SetThreadPriority`, `GetThreadPriority` | 9, 3 | `pthread_setschedparam` (best effort) |
| `VirtualQuery` | 7 | `mach_vm_region` |
| `AcquireSRWLock*`/`ReleaseSRWLock*` | 5+5+4+4 | `pthread_rwlock_*` |
| `CoInitializeEx`, `CoUninitialize`, `CoCreateInstance` | 5, 5, 3 | delete (COM is only used by Media Foundation and DXGI) |
| `CreateThread`, `ResumeThread`, `SuspendThread`, `ExitThread` | 4, 3, 2, 1 | `pthread_create`, no-op, `pthread_kill(SIGSTOP)` if needed, `pthread_exit` |
| `CreateWaitableTimerExW`, `SetWaitableTimer`, `CancelWaitableTimer` | 4, 3, 1 | `kqueue` `EVFILT_TIMER` or `dispatch_source` |
| `ReadFile`, `WriteFile`, `CreateFileW`, `SetFilePointerEx`, `GetFileSizeEx` | 4, 3, 3, 2, 2 | `pread`, `pwrite`, `open`, `lseek`, `fstat` |
| `GetTickCount64` | 4 | `clock_gettime(CLOCK_MONOTONIC)` |
| `VirtualAlloc`, `VirtualFree`, `VirtualProtect` | 3, 3, 3 | D7 |
| `timeBeginPeriod`/`timeEndPeriod` | 3, 1 | no-op (macOS has no timer resolution knob) |
| `SwitchToThread` | 3 | `sched_yield` |
| `CreateFileMappingW`, `MapViewOfFile`, `UnmapViewOfFile` | 1, 2, 1 | D7 (`mmap` of one file object at N addresses) |
| `SleepConditionVariableSRW`, `WakeConditionVariable`, `WakeAllConditionVariable`, `InitializeConditionVariable` | 2, 2, 1, 1 | `pthread_cond_t` |
| `SetThreadAffinityMask`, `GetCurrentProcessorNumber(Ex)` | 2, 2+1 | `thread_policy_set(THREAD_AFFINITY_POLICY)` hint, else no-op (R3) |
| `SetProcessInformation`, `SetPriorityClass`, `GetPriorityClass` | 1, 2, 1 | `setpriority` on the process, best effort |
| `RegGetValueA` | 2 | `sysctl` / `IOKit` (`host_device.cpp`) |
| `GetSystemInfo`, `GetLogicalProcessorInformationEx` | 1, 1 | `sysctlbyname("hw.ncpu")`, `hw.perflevel*` |
| `MessageBoxA/W` | 3 | `NSAlert` (a small ObjC++ file) or `fprintf(stderr)` |
| `CommandLineToArgvW`, `GetCommandLineW` | 2, 1 | `argv` from `main` |
| `MonitorFromWindow`, `GetMonitorInfoW`, `GetWindowRect`, `SetWindowPos`, `ShowWindow`, `SetForegroundWindow` | 1 each | SDL3 (`SDL_GetDisplayForWindow`, `SDL_SetWindowPosition`, `SDL_RaiseWindow`) |
| `LoadCursorW`, `GetDynamicTimeZoneInformation`, `GetStdHandle`, `AttachConsole` | 1 each | SDL3 cursor, `CFTimeZone`, stdout, dropped |
| `FindFirstFileExW`/`FindNextFileW`, `CreateDirectoryA`, `MoveFileExW`, `DeleteFile*`, `CopyFileA`, `GetFullPathName*` | 1–3 each | `opendir`, `mkdir`, `rename`, `unlink`, `copyfile`, `realpath` |

`winsock2`/`ws2tcpip` (3+3 files): `WSAGetLastError`→`errno`, `closesocket`→`close`, `ioctlsocket`→`fcntl`,
`WSAStartup`/`WSACleanup`→dropped, `SIO_UDP_CONNRESET`→dropped, `SOCKET`→`int`.

`dxgi1_6`/`wingdi` (1 file): dropped with `win_display.cpp`.
`xaudio2`/`mmreg`/`objbase` (1 file): dropped with `audio_xaudio2.cpp`.
`mfapi`/`mfidl`/`mfreadwrite`/`mferror` (1 file): dropped with `movie_mf.cpp`.
`dbghelp` (1 file): Mach-O symbol reader.
`intrin.h` (3 files), `immintrin`/`emmintrin` (2 files): `__rdtsc`→`mach_absolute_time`, `__cpuid`→`sysctl`,
`_mm_pause`→`yield`, `_mm_*` in `frame_limiter.cpp` → dropped.

## 7. Implementation status

What is in the tree and what the evidence for it is. Everything below was run on the target (macOS 27.2, Apple M3,
16 GB, Apple clang 17), not cross-checked.

| what | how it was checked |
|---|---|
| build | `cmake --preset macos-arm64` + `cmake --build` produce `libnxrt.a`, `libnxrt_gfx.a`, `nxrt_gfx_demo`; `game-build/` produces `pkp`. Deps are fetched into `~/Library/Caches/nxrt/deps`, MoltenVK comes from Homebrew. |
| M1, address space | `nxrt_gfx_demo --selftest`: the layout reserves, a 6 GiB heap section commits, the alias at `0x7300000000` is written through, and every page of the four module images (main 0x3d20000, sdk 0xff0000, subsdk0 0x140000, rtld 0x20000 bytes) is written at its real base. 5/5 runs, 0 failures. |
| M2, graphics | `nxrt_gfx_demo --headless --frames 300` → `gpu="Apple M3" frames=300 fps≈4400 … validation_errors=0`; `--overlay --frames 600` presents 600 frames with the ImGui overlay; `--screenshot` writes a 1280x720 PNG. |
| M3, the game | `pkp` loads all four lifted modules (149,924 + 47,943 + 2,730 + 37 functions), runs rtld, starts `sm:` and the services, creates the heap section, brings up NVN on MoltenVK, places the game's five GPU pools, and plays: ~300 fps headless, audio at 48 kHz with no underruns, save data mounted, the world loaded. |
| M5, diagnostics | the crash report is written by a helper thread with host stacks; `SuspendThread`/`GetThreadContext` stop a thread on its mach port (the sampler reads 2,688 stacks across main, the render thread and 40+ guest threads); `tools/symbolize.py` names both host and guest frames. |
| M6, launcher | the Tauri launcher runs natively: Set up extracts and translates the shader corpus, Play starts `pkp` with `--ini`, `--setup`/`--preflight`/`--readiness`/`--play` work without the window, `make-mac.sh` builds the flat release zip, and the update tests pass. |

### What macOS had already taken (measured on the target)

The layout was designed against Windows and collided with system reservations:

- a **385 GiB `GUARD` region at `0xfc0000000`–`0x7000000000`** (tag 31, `VM_MEMORY_GUARD`) exists in every process and
  covers the old `kAliasBase` (`0x6000000000`);
- libmalloc's xzone allocator reserves **two ~12 GiB `PROT_NONE` ranges at random addresses inside
  `0x7000000000`–`0x8000000000`** during dyld/libSystem start-up; over 30 runs they hit the SDK/subsdk0/rtld bases,
  which is why those regions are reserved in the executable's Mach-O header instead of at run time;
- the SDK's stack-guard `mmap` asks for a **512 GiB ASLR region**; the guest gets `0x7000000000`–`0x7100000000` for it.

### The memory budget of a run (16 GB host, measured)

The game commits ~8.4 GB of its 8 GiB heap (it exits at boot with a smaller cap) and its NVN pools reach ~5.2 GB, of
which the port imports only the 80 MiB chunks it touches. At the world load the process footprint is ~9 GB; the
port's own caches (`pool_copy_mb` 1536, `tex_prep_mb` 512) are the only part it can trim — at 384 and 128 the
footprint at the language menu drops from 8.0 GB to 7.5 GB.

### Before an Apple Silicon machine was available

The lifter and the build were exercised on x86-64 Linux first: all five ExeFS files lifted with 0 untranslated
functions, `main` produced 663 MB of C++ across 839 shards, and eight sampled shards plus `module_main.cpp` passed
`g++ -std=c++20 -fsyntax-only`, which is the first evidence that the emitted code is portable C++20 rather than
something only clang-cl accepts. `--target pkp_rtld` lifted, compiled and archived on that host, so `NxRecomp.cmake`'s
non-MSVC branch was exercised, not just written.

### Still open

See §8 "What is left": Link Play against a `pokopia-native` peer, a real gamepad, the `fs_volume` XPC check, a
`tt_runner` executable, a movie played end to end, signing and notarization, an `arm64-apple-macos` target in `pkp_build.py`,
and the GPU-bound frame rate in the world.

## 8. Running the port on a Mac

### Build the runtime

```sh
brew install cmake ninja molten-vk libvpx        # libvpx: the host VP9/VP8 decoders (optional)
cd runtime && cmake --preset macos-arm64 && cmake --build --preset macos-arm64

./build/macos-arm64/nxrt_gfx_demo --headless --frames 300                     # device and frames, no window
./build/macos-arm64/nxrt_gfx_demo --overlay --frames 600                      # window with the F1 overlay
./build/macos-arm64/nxrt_gfx_demo --headless --frames 120 --screenshot s.png --shot-frame 60
./build/macos-arm64/nxrt_memprobe                                            # the guest layout, heap, alias and images
```

Expected on an M3: `gpu="Apple M3" frames=300 … validation_errors=0` and a 1280x720 PNG of the colour grid.

### Lift the game and build the executable

```sh
cmake -S recomp/games/pkp -B work/build/pkp -G Ninja \
      -DNX_ROOT="$PWD" -DPKP_EXEFS="$PWD/work/dump/updates/655360/exefs" \
      -DNX_PYTHON="$PWD/tools/.venv/bin/python"
cmake --build work/build/pkp --target pkp_modules -j 10

cmake -S game-build -B work/build/game -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DPKP_LIB_DIR="$PWD/work/build/pkp" -DPKP_EXEFS_DEFAULT="$PWD/work/dump/updates/655360/exefs"
cmake --build work/build/game -j 6
```

All four modules lift and compile in ~6 minutes on an M3 with the default shard counts.

### Run

`pkp` takes `--exefs <folder> --ini <file>`; `[fs] exefs` and `[fs] romfs` in the ini name the dump. A run that needs no
setup at all:

```ini
[boot]
gfx = window            # or headless
[memory]
app_memory_mb = 8192    # the game exits at boot below this
limit_gb = 11           # the memguard stops the run here, with a report
[nvn]
render_override = 0     # 1080p targets scaled to the window; 1440p targets grow the pools until init backs out
```

### The launcher

The Tauri launcher builds and runs on macOS (`runtime/apps/launcher` is Win32 and is not used).

```sh
cd launcher && npm install          # once
npm run tauri build                 # launcher/src-tauri/target/release/pokopia-launcher
```

`npm run tauri build -- --no-bundle` for the release (only the binary is packaged): a plain `cargo build --release` is a dev binary (no `TAURI_ENV_*`, no embedded front-end) and
opens a white window — it is ~650 KB smaller and has no `index-<hash>.js` in its `strings`.

Put the binary in the game folder; the launcher looks for the game next to itself (`pkp`, `pkp.ini`, `shader_cache`).
There is no `.app`: the release folder holds `PokopiaLauncher` and `tools/`, and the launcher is started from there (a
double click opens Terminal around it, which is where its output and the game's would show; `./PokopiaLauncher` from a
terminal is the same thing). The game's stdout and stderr are discarded
when the launcher starts it (Windows spawns it with `CREATE_NO_WINDOW`); its log, crash report and freeze dump are files in
the game folder either way.

`--play` starts the game without the window, `--setup` runs Set up without it (extract the shader corpus and translate
it), `--preflight` prints the checks and `--readiness` prints what Play would see. The window's Play starts `pkp` in that
folder with `--log pkp.log`; the shader steps of the Windows release are skipped (the game is native, the tools are not).

### Release model

A macOS release follows the Windows one (`PKP_RT_DLL`, release model b): the runtime is a shared library and the game is an
executable that links it, so a release can replace the runtime without rebuilding the game.

```sh
cmake -S game-build -B build/rtb -G Ninja -DCMAKE_BUILD_TYPE=Release -DPKP_RT_DLL=ON \
      -DPKP_LIB_DIR="$PWD/work/build/pkp"
cmake --build build/rtb -j 6
```

The release is a zip (`launcher/scripts/make-mac.sh`): `PokopiaRecomp-<version>/` with the launcher, `tools/` (the lifter,
the SDK sources, the shader harness, MoltenVK) and `libpkp_rt.dylib`, plus the Gatekeeper note. The package carries no game code and
no Python: Set up fetches python-build-standalone and the lz4 and capstone wheels into `tools/macos` at the first run, the
way the Linux package does, and compiles with the Apple clang of the Command Line Tools. The game folder (`pkp`, `pkp.ini`,
`shader_cache`, saves) is created in `~/Library/Application Support/Recompia/game`, because the bundle may sit in
`/Applications`.

Only two files in that bundle need macOS: the launcher (Tauri's bundler) and `libpkp_rt.dylib` (it links the SDK's
frameworks). Everything else — the lifter, the SDK sources, the shader harness as a self-contained `osx-arm64` publish,
MoltenVK and the prompt icons — is staged on a Linux runner by `launcher/scripts/make-payload.sh`, so the macOS runner only
compiles and signs (`macos-payload` -> `macos` in `release.yml`).

That writes `libpkp_rt.dylib` (the runtime and the game module) and `pkp` (`src/pkp_stub.cpp` plus the lifted modules,
linking `@rpath/libpkp_rt.dylib`). The executable carries the `__GUEST` segment, because the main executable's Mach-O
header is what reserves the range before libmalloc runs. `src/pkp_rt.exports` is the counterpart of `pkp_rt.def`: the
same symbols, and an export list is also what makes the linker include the static objects that define them.

Without `PKP_RT_DLL` the build is unchanged: one static `pkp`, which is what development uses.

### The shader cache

The game renders only with a translated shader cache; without one every program stays unlinked and the window is black.
`pkp --prepare-shaders` cannot build it on MoltenVK, so the harness in `tools/shaders` does: dump the blocks the game asks
for (`[nvn] dump_missing = <folder>`), then

```sh
tools/.venv/bin/python3 tools/shaders/ampere/extract_corpus.py --romfs <romfs> --main <exefs>/main --out <corpus>
dotnet tools/shaders/build/bin/Release/net10.0/spirv_harness.dll --batch <corpus> --out <cache>/spirv \
       --jobs 6 --host moltenvk --no-val
```

The full corpus is 99,012 blocks (~23 GB of SPIR-V, ~40 minutes at 6 jobs). Set up does the same from the launcher and
writes `shader_cache/setup.json`; a finished cache is not translated again. Translations the game makes while it runs are
merged into `results.tsv` at exit, never replacing it.

### Reading a report or a profile

Host frames print as `pkp+0x…` (a folded profile) or absolute addresses (a crash report, with its `image base` line);
`tools/symbolize.py` maps them back through `atos`, and the guest frame chain through the `_f_<guest address>` names:

```sh
tools/symbolize.py work/build/game/pkp_crash.txt        # the report, with names
tools/symbolize.py prof_samples.folded --top 20         # the most frequent stacks
```

`[prof] sample_hz = 200` turns on the sampling profiler (it suspends threads with `thread_suspend`, so the stacks are real).

### Memory

The game wants ~9 GB at the world load. The gauges of a limit report show where it goes: `mem.heap_committed` ~8.4 GB
(the game fills the 8 GiB cap) and `nvn.memory_pools.bytes` ~5.2 GB (its own pools, imported lazily in 80 MiB chunks).
The port owns two caches — `[nvn] pool_copy_mb` (shadow cells, 1536 MiB) and `[nvn] tex_prep_mb` (texture staging, 512
MiB); at 384 and 128 the footprint at the language menu drops from 8.0 GB to 7.5 GB. macOS has no swap-size setting: a
run must fit the host, and the launcher stops the game at the kernel's critical memory pressure.

### Traps already paid for

- **The signal handler must not allocate or call into dyld.** The first version deadlocked on its own lock; the fault table
  is lock-free, `note_fault` writes with `write(2)`, and the PLT stubs the handler needs are bound at install time.
- **The altstack must not be thread-local.** macOS puts TLS at the bottom of the stack, so an overflowing thread's guard
  landed in its own altstack and the handler overwrote live frames. It is a separate `mmap` with a guard page.
- **Darwin protection faults are `SIGBUS`**, not `SIGSEGV`; `code_of_signal` maps them to `EXCEPTION_IN_PAGE_ERROR`.
- **`nxos_guarded_call` must call `(*thunk->fn)()`**, not the thunk pointer cast to the callable type: the extra words were
  read as the lambda's captures.
- **`VirtualAlloc` at a fixed address outside `__GUEST`** used an `mmap` hint (macOS ignores hints): `mach_vm_allocate`
  with `VM_FLAGS_FIXED`.
- **The guest heap was the memory hog**, not the images: `[memory] app_memory_mb = 8192` halves RSS at boot.
- **The ASLR region** the SDK maps its stack guard into is 512 GiB; the port gives the guest `0x7000000000`–`0x7100000000`.
- **Named mutexes need a real owner test** (`MountSaveData` hung on the fs root lock).
- **`save_dir()` must build POSIX paths**; a Windows separator in `std::filesystem` silently produced a different folder.
- **Write-watch needs the fault address and direction**: Darwin fills neither for a protection fault, so the handler
  reports `[0] = 1` (write) and `[1] = si_addr` itself.
- **The pool shadow copy breaks the UI**: with `[nvn] pool_copy = on`, a still menu broke for ~30 frames every ~500 (the
  background gone, buttons drawn with garbage) until the game rewrote the buffers. The device reports
  `VK_EXT_external_memory_host`, so the pools are imported from the guest's pages instead (`auto_copy()` asks the device).
- **The prompt atlas patch needs a BC7 encoder**: `astc2bc7` is x86 ISPC, so `a2b_encode_bc7` returned unsupported and every
  keyboard glyph failed to draw. `runtime/third_party/bc7enc` (MIT) encodes there now.
- **The launcher's post-exit shader translation must merge**, not overwrite: the harness writes `results.tsv` for the blocks
  it was given, so translating only the dumped ones replaced a 99,012-row cache with 6,064 rows.

### What is left

- **M4**: Link Play against a `pokopia-native` peer (the relay half is verified), a gamepad with a real pad, the
  `fs_volume` XPC check, and a `tt_runner` executable (the testtools code is in the library; no target builds a runner).
- **M6**: a movie played end to end (the lifted `movie::` demuxer and the host VP9 decoder are in place, D10); an
  `arm64-apple-macos` target in `pkp_build.py` (macOS builds through CMake), and a release design for a static binary. No
  signing by decision (see M6).
- **Performance**: the M3 is GPU-bound at ~25 fps in the world (present blocks 41–49 ms, the render thread idle); the
  render scale is the lever. The pool shadow copy's bookkeeping, if it ever comes back, is the other.
