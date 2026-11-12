# Linux support: port notes

Status: **the game runs natively on x86-64 Linux** — the runtime, the GFX stack, the lifted modules, `libpkp_rt.so`, the
`pkp` executable, the shader translator and the release package are all built by the platform's own toolchain, and the game
renders (4,163 draws, 0 skipped) against a real dump with a translated shader cache. What is left is the launcher's Linux
build (verified by CI only, see §7) and the diagnostics Apple still owns.

The macOS port (`docs/macos-port.md`) is the blueprint this follows: one POSIX shim under `runtime/src/os/`, no rewrite of the
38.5k Win32 call sites. Linux is the *easier* case of the two — the shim's non-Apple branches were already written against
Linux calls — so the work here is making Linux a first-class CMake target and filling the gaps where macOS had an Apple API.

---

## 1. What the port is

Nothing in the recompilation pipeline changed. `nxrecomp` emits portable C++20 against `recomp/include/nxrecomp/ctx.h`, and
the lift is byte-identical to the Windows one. The port is:

| Layer | Change |
|---|---|
| `runtime/CMakeLists.txt` | a `CMAKE_SYSTEM_NAME STREQUAL "Linux"` branch next to the Apple one: x86-64 guard, `NXRT_LINUX`, TLS `initial-exec`, no `-framework` links, `dl`/`pthread`, `nxrt_miniz`, the SDL3 audio sink |
| `runtime/src/os/` (POSIX shim) | the existing non-Apple `#else` branches, made to compile and be correct: timing, memory, files, threads, symbols, altstack, `__rdtsc`/`__cpuid` |
| `runtime/src/services/` | `audio_sdl3.cpp` (a new `AudioSink`), a zlib PNG writer in `album.cpp`, Linux device enumeration |
| `runtime/src/os/os_posix_a2b.cpp` | ASTC decoding through the vendored ARM `astcenc` instead of macOS' Metal sampling, then BC7 through `bc7enc` |
| `game-build/CMakeLists.txt` | model b on ELF: `libpkp_rt.so` with a version script, `pkp` linking it with `$ORIGIN` rpath, PIC modules |
| `launcher/src-tauri/` | the native path (macOS') extended to Linux: `pkp` + `libpkp_rt.so`, no Proton/Wine |
| `game-build/release_b/pkp_build.py` | a `x86_64-linux-gnu` Zig target so a player's Set up builds a native `pkp` |

## 2. Decisions

### L1 — Linux is a third branch of the POSIX shim, not a second shim

The shim's `#else` arms were already Linux-shaped (`clock_gettime`, `MAP_FIXED_NOREPLACE`, `mincore`, `sysinfo`,
`getentropy`, `/proc/self/exe`, `dlopen`, `sched_getcpu`, `ucontext_t.gregs`). Where they were stubs because only macOS
existed, they now do the Linux thing; where an Apple API has no Linux equivalent and nothing needs one, the Linux side
keeps a documented no-op. That keeps one compatibility layer for two POSIX platforms.

### L2 — ASTC decodes through `astcenc`, and BC7 stays `bc7enc`

Windows ships per-ISA `astcdec-*-shared.dll`; macOS samples ASTC on the GPU. Linux gets the decoder statically linked from
the `tools/textures` vendor tree (`ASTCENC_DECOMPRESSOR=ON`, SSE4.1, no CLI, no other ISA), so the package has no extra
`.so` to load and no CPU dispatch to get wrong. `a2b_decode_astc_rgba8` output is **bit-identical** to `astcenc`'s own CLI
decode (verified: 0 of 3,686,400 bytes differ on a 1280×720 6×6 texture).

### L3 — `libpkp_rt.so` exports through a generated version script

An ELF version script filters the dynamic symbol table but does **not** pull archive members in the way a Mach-O export
list does, so `pkp_rt.exports` is compiled into two artefacts: `pkp_rt.map` (`--version-script`) and one
`-Wl,--undefined=<sym>` per export. `--exclude-libs,ALL` must not be used — it would hide the exports.

### L4 — the modules are position-independent

`game-build` sets `CMAKE_POSITION_INDEPENDENT_CODE ON` when `PKP_RT_DLL AND NOT WIN32` (the option has to be declared before
`add_subdirectory(runtime)`). Without it the shared library link fails with `R_X86_64_TPOFF32` relocations against the
lifted code's thread-locals.

### L5 — no guest segment, no resource script

macOS injects a `__GUEST` segment into the Mach-O so the kernel reserves the guest address space before dyld. Linux needs
none: the runtime reserves its ranges with `MAP_FIXED_NOREPLACE` at startup. `pkp.rc`/`pkp.manifest` are Windows resources
and are not shipped or linked.

### L6 — audio is SDL3

`AudioSink` (from the macOS port) gains an SDL3 implementation: a device-bound `SDL_AudioStream` opened at 48 kHz stereo,
queued frames from the stream's byte count, a 250 ms cap, underruns counted on empty transitions. It is the sink `auto`
picks on Linux; XAudio2 and CoreAudio stay the Windows and macOS ones.

### L7 — a player's Set up cross-compiles with the bundled Zig

The package ships one compiler (Zig) on Windows and Linux, so `compile_flags.ZIG_LINUX` is `ZIG_MINGW` with
`-target x86_64-linux-gnu`: same optimisation level, same semantic flags, same `-mcpu=x86_64+cx16`, because the guest code
means the same thing on both hosts. The link step adds `-Wl,-rpath,$ORIGIN -Wl,-z,stack-size=134217728` (the ELF form of
the Windows `/STACK` and the Mach-O `-stack_size`), and the runtime library is copied next to the exe.

### L8 — the launcher runs the native binary

`native_game()` is true on Linux as on macOS, so `play.rs` spawns `pkp` directly instead of building a Proton/umu-run argv;
`readiness`, `repair`, `update`, `runtime_api` and `saveimport` take the executable and runtime-library names from
`host::game_exe[_name]()` / `host::runtime_lib[_name]()`, and the `astcdec-*.dll` copies are gated on `has_decoder_dlls()`
(Windows only). Windows behaviour and wording are unchanged.

### L9 — a mod's HTTPS request is the system's libcurl, loaded at run time

`src/net/http_client.cpp` was WinHTTP only, so the POSIX builds refused every request (`MT_ERR_STATE` to the mod). The
POSIX branch now `dlopen`s `libcurl.so.4` at the first request and declares the ten calls it makes itself: the easy
interface's ABI has been stable since libcurl 7.0, and linking it would make a build-time dependency a run-time one for
every player. `libpkp_rt.so` keeps its five `NEEDED` entries (libm, libstdc++, libgcc_s, libc, the loader); a host without
libcurl refuses the request as before instead of failing the start. Every desktop distribution ships libcurl, Steam itself
links it, and macOS keeps `libcurl.4.dylib` in the dyld shared cache, so the same block serves both ports.

The rules stay the WinHTTP ones, because they are what `http_client.h` promises: https anywhere and plain http only to
localhost / 127.0.0.1, no user name or password in the URL, no control character, no redirect followed, no cookie engine,
TLS 1.2 or later, a 256 KiB answer cap, the connect timeout plus a low-speed bound for a stalled transfer, and the verb
taken from the method so a GET with a body stays a GET. Measured against libcurl 8.5.0: GET and POST to localhost 200 and
201 with the type and body arriving, `https://example.com` 200 over TLS, 302 answered as 302, an answer past the cap and a
stalled transfer both 0, and a stub libcurl missing a call refused up front.

## 3. `__APPLE__` sites: Apple-only or portable

Every `#if defined(__APPLE__)` in `runtime/src` (51 sites, 18 files), and what Linux does instead:

| File:line | What the Apple branch does | Linux |
|---|---|---|
| `os/os_posix.cpp:36,253,265,732,870,884,905,1484,1567,1722,1801,1844,1989,2078` | `mach_absolute_time`/`mach_timebase_info`, `mach_vm_*`, `thread_suspend`/`thread_get_state`, `_NSGetExecutablePath`, `pthread_getname_np`/QoS | Linux arm of the same function: `clock_gettime`, `mmap`/`mincore`, `/proc/self/exe`, `pthread_setname_np` |
| `os/os_posix.cpp:1956,2247` | aarch64 `mrs cntvct_el0` / `tpidrro_el0` | x86-64: `rdtsc` / `%fs:0` |
| `os/os_posix_mem.cpp:33,191,222,400,474,559,584` | `mach_vm` reserve/commit/decommit, `VM_PROT_*`, guard pages | `mmap` + `MAP_FIXED_NOREPLACE` + `madvise`; verified by the boot (§6) |
| `os/os_posix_file.cpp:65,456` | `st_birthtimespec`, `F_FULLFSYNC` | `st_ctim` as the creation time, `fdatasync` |
| `os/os_posix_misc.cpp:27,70` | `arc4random_buf` | `getentropy` (`:14` is the `!__APPLE__` include for it) |
| `os/os_posix_net.cpp:219` | MTU from `getifaddrs`' `if_data` | `ioctl(SIOCGIFMTU)` — Linux keeps the MTU in the kernel |
| `os/compat/intrin.h:33` | `__rdtsc`/`__cpuid` stubs (Apple has no `rdtsc`) | real `rdtsc`/`cpuid` on x86 |
| `os/nx_os.h:45` | the Mach-O/dyld includes | `elf.h`/`link.h` |
| `diag/symbols.cpp:13,353` | `_dyld_image_count`/`_dyld_get_image_*` snapshot | `dl_iterate_phdr` into the same fixed table, slot 0 = the exe |
| `diag/crash.cpp:35,45,207,273,409` | fault-thread name, `nxos_recent_faults` history, reporter thread name | no-ops: the report still carries the fault, the backtrace and the guest context, without the recent-fault list |
| `gfx/platform.cpp:20,385` | `SDL_VULKAN_LIBRARY` pointed at the bundled `libvulkan.1.dylib` | not needed: the system Vulkan loader and its ICDs are used |
| `gfx/vk_device.cpp:29,418` | `dlopen` of the bundled MoltenVK | not needed (same reason) |
| `gfx/nvn/nvn_pipeline.cpp:228` | an empty shader-cache path (MoltenVK has no disk cache) | the normal cache path |
| `gfx/nvn/nvn_pool_copy.cpp:68,120` | pool copy forced on (MoltenVK loses compute writes) | the Wine/integrated-GPU heuristic |
| `gfx/extras/win_display_posix.cpp:17,26` | HDR query through IOKit | returns false (no HDR), as on Windows without DXGI |
| `mem/memory.cpp:182` | guest range from the Mach-O `__GUEST` segment | `MAP_FIXED_NOREPLACE` reservations |
| `services/album.cpp:42,151` | ImageIO PNG writer | zlib PNG writer (`mz_compress2` + `mz_crc32`) |
| `services/audio_sink.cpp:64,105` | CoreAudio sink selection and naming | SDL3 sink |

Conclusion: no Apple branch is load-bearing for Linux correctness. The only silent degradations are the crash reporter's
recent-fault list and the HDR query; both are diagnostics or optional features, and both are listed in §7.

## 4. Build

Prerequisites: CMake ≥ 3.28, a C++20 compiler (GCC 13 or clang 18), Vulkan loader + headers, and the SDL3/volk/VMA deps
that `runtime/cmake/gfx_deps.cmake` fetches (they land in `$XDG_CACHE_HOME/nxrt/deps`).

```sh
cmake -S runtime -B build/linux -DCMAKE_BUILD_TYPE=Release -DNXRT_BUILD_GAMES=OFF \
      -DNXRT_DEPS_DIR=$HOME/.cache/nxrt/deps
cmake --build build/linux -j
```

Headless check without a GPU (Mesa's lavapipe):

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  build/linux/nxrt_gfx_demo --headless --frames 100 --shot-frame 30 --screenshot shot.png
```

Lift and build the game (the release-model-b flow, exactly what a player's Set up runs):

```sh
python tools/build/pkp_build.py --exefs <dump>/exefs --game <game folder> --jobs 12
```

or, from a checkout, the model-b build of the runtime plus the exe:

```sh
cmake -S recomp/games/pkp -B build/pkp -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/recomp/cmake/clang-cl.cmake \
      -DCMAKE_BUILD_TYPE=Release -DNX_ROOT=$PWD -DPKP_EXEFS=<dump>/exefs
cmake --build build/pkp
cmake -S game-build -B build/game -G Ninja -DCMAKE_BUILD_TYPE=Release -DPKP_RT_DLL=ON -DPKP_LIB_DIR=$PWD/build/pkp
cmake --build build/game
```

## 5. Running

`pkp` needs `pkp.ini` (or the same keys as flags) next to it, pointing at the dump; `libpkp_rt.so` must be in the same
folder (`$ORIGIN` rpath). `--headless --max-seconds N` runs without a window; `--log pkp.log` writes the log the launcher
shows. `VK_ICD_FILENAMES` selects a Vulkan driver when more than one is installed.

## 6. Evidence log

Ubuntu 24.04 in WSL, GCC 13.3.0 and clang 18.1.3, Mesa lavapipe (no GPU), real 2.0.1 dump (36 RomFS files, 7,246,052,773
bytes).

| Check | Result |
|---|---|
| `libnxrt.a` (core only) | builds, 8,851,486 B |
| `nxrt_gfx_demo --headless --frames 100` | `frames=100 fps=1104 p99=14.9ms validation_errors=0 warnings=0`, 1280×720 PNG written |
| ASTC → RGBA8 vs `astcenc` CLI | 0 of 3,686,400 bytes differ; 4.92 ms for a 1280×720 6×6 texture |
| modules (284 TUs, clang 18) | `libpkp_main.a` 271,779,544 B, `libpkp_sdk.a` 55,636,980 B, `libpkp_subsdk0.a` 4,174,880 B, `libpkp_rtld.a` 58,928 B |
| `libpkp_rt.so` | 13,354,072 B, all 14 exports present |
| `pkp` (model b, `$ORIGIN` rpath) | 278,626,520 B, runs |
| boot with the real dump | `boot: pkp 0.10.3-linux.1`; RomFS 36 files / 7,246,052,773 B; `svc: 18401522 calls, 25 distinct, 0 unknown`; every IPC service 0 unhandled; NVN back end up (36 slots) |
| frame pacing | `618 frames in 10 s: mean 61.8 fps, 1% low 57.8 fps` on llvmpipe |
| audio | `SDL3 audio does not initialize: No available audio device` → null sink (the WSL host has no audio device; the sink path itself is wired) |
| player build (`pkp_build.py`, Zig `x86_64-linux-gnu`) | 284 TUs, `pkp` 270,664,904 B (Zig's Linux output carries full DWARF, 1,415,832,584 B, so the link passes `-Wl,--strip-debug`; the symbol table stays) |
| shader corpus + harness on Linux | corpus 99,013 unique blocks; `spirv_harness --host desktop --no-val --jobs 6` (linux-x64 publish) translated 99,012 of them into 99,012 `.spv` |
| boot **with** the cache | `nvn back end at the deadline: submits 10714 lists 10694 draws 4163 (skipped 0) presents 975 pipelines 508 textures 18 (failed 0) unsupported 0`, `436 frames in 10.0 s: mean 43.5 fps, 1% low 34.2 fps` on llvmpipe |
| the release package, end to end | staged tar.gz 49,151,117 B (no Python, no Zig, no `astcdec-*.dll`, no `upscalers/`, `libpkp_rt.so` instead of `pkp_rt.dll`); a Set up run **from the package** built `pkp` in 508 s and that exe booted with `draws 3495 (skipped 0) presents 845 pipelines 502 (failed 0) unsupported 0`, `489 frames in 10.1 s: mean 48.5 fps`, 0 error lines |
| runtime rebuild after the last shim changes | `libnxrt.a` 8,872,566 B and `nxrt_gfx_demo` build clean |

## 7. What is left

- **The launcher's Linux build is verified by CI, not locally.** `cargo check --target x86_64-pc-linux-gnu` from Windows
  stops in `webkit2gtk-sys`' pkg-config, and the WSL image has no root to install `libwebkit2gtk-4.1-dev`;
  `.github/workflows/launcher-checks.yml` (ubuntu-22.04: `cargo check`, `clippy`, `cargo test`) is the gate. The AppImage
  itself was not built here either (linuxdeploy needs a desktop toolchain), so the packaged launcher is untested on Linux.
- **Runtime-generated shader variants.** 212 blocks the game asks for are not in the corpus the extractor builds from the
  RomFS (`unique.tsv` has no entry for them), so they log "no translated shader" and the program is not linked; the draws
  that use them still reach the GPU through the runtime's own path (`skipped 0`). Windows has the same gap, which is why
  the launcher can re-run the translator.
- **Crash reporter.** The recent-fault list and the faulting thread's name are Apple-only; a `/proc/<pid>/task/<tid>/comm`
  plus `si_code`/`si_addr` equivalent would restore them.
- **HDR.** `win_display_posix.cpp` reports none on Linux; a DRM/`VK_EXT_hdr_metadata` query would be the Linux analogue.
- **Audio device hot-plug and device selection** are untested (no device in the test host).
- **Upscalers.** `NXRT_UPSCALER_*` are off outside Windows, so the launcher's FSR 3 / DLSS switches have nothing behind
  them on Linux; the Settings page does not hide them yet.
