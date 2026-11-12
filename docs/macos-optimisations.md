# macOS optimisations — handoff for a macOS agent

What this is: the 16 improvements found by reviewing [Chaos192/PokoMac](https://github.com/Chaos192/PokoMac)
(a rival Apple-silicon port that forks from the same commit our port started at, `5f98748`), with what is
already in this tree, what is deliberately not, and exactly how to pick the rest up.

Read `docs/macos-port.md` first: it is the port itself (D1–D10, M1–M6). This file is only the tuning layer
on top of a working port, plus the traps that cost us a debugging session each.

Nothing here needs a Windows box. Where an item is open, the reason is "only a Mac can run it", and the
section says what to run.

---

## 1. Status

| # | PokoMac item | State here |
|---|---|---|
| 1 | LDAR is a full barrier plus a seq_cst load, STLR a seq_cst store plus a barrier after it | **done** — `recomp/include/nxrecomp/ctx.h:186-209`, `cxx.h:68-93`, `lift/a64.py:379`, `lift/tier2/misc.py:270` |
| 2 | `VK_KHR_portability_enumeration` + `VK_KHR_portability_subset` | **done** — `runtime/src/gfx/vk_device.cpp:430-431,468,677-679` |
| 3 | SHA-1 with the ARMv8 instructions | **done** — `runtime/src/gfx/nvn/nvn_shaders.cpp:29-38,109-169,202` |
| 4 | Write tracking at 16 KiB host pages | **done** — `runtime/src/mem/write_watch.cpp:99-129` and its callers |
| 5 | No `VkPipelineCache` with MoltenVK, 8 build threads | **knob only** — `[nvn] pipeline_cache_moltenvk` (`nvn_pipeline.cpp:233-245`); the default is unchanged on purpose (§3) |
| 6 | `shouldMaximizeConcurrentCompilation` | **open** — needs Obj-C/Metal (§4.1) |
| 7 | Pool mirrors off on Apple | **done** — `runtime/src/gfx/nvn/nvn_gpu_pool.cpp:995-1010` |
| 8 | Metal shader-cache identity | **done** — `game-build/release_b/pkp_build.py` `write_identity` + `IDENTITY`; link inspected on a Mac only *(never run — §3)* |
| 9 | `pkp --prepare-pipelines` | **open** — pkp has no CLI at all (§4.2) |
| 10 | Real-time policy for the render thread | **already ours** — `kernel/sched.cpp:335` → `os_posix.cpp:733-741` |
| 11 | Lazy barriers + sticky read-only depth | **half ours** — the lazy-barrier half already exists (`nvn_replay_pass.cpp:146-155`); sticky depth is open (§4.3) |
| 12 | vsync on by default on macOS, once | **done** — `[meta] display_rev` (`play_config.cpp:189-205`, `play_config_table.cpp:216`) |
| 13 | A frame limit holds through loading | **done** — `frame_limiter_set_player_cap` (`play_frame_cap.cpp:105-110`) |
| 14 | MetalFX temporal upscaling | **open** — large (§4.4) |
| 15 | UDOT/SDOT, ZBIC-compressed NSOs | **open** — for games we do not ship (§4.5) |
| 16 | KosmicKrisp as a second driver | **open** — needs a Mac (§4.6) |

Do not "fix" the done ones back. Each has a comment at the site saying why it is the way it is; the ones
that look like bugs (5, 8, 10, 11) are the ones with a history.

---

## 2. Where the pieces are

- **Lifter**: `recomp/` (Python). Emitted C++ primitives live in `recomp/include/nxrecomp/cxx.h`; the
  ordering helpers in `ctx.h`; the decoder is `lift/a64.py` (A64) and `lift/tier2/*.py` (Advanced SIMD).
  A lifter change makes Set up rebuild the game's code once (the build stamp in `pkp_build.py`).
- **Runtime**: `runtime/src/gfx/nvn/` is the NVN→Vulkan replay (passes, pipelines, pools, prewarm),
  `runtime/src/gfx/` the device and swapchain, `runtime/src/os/` the POSIX shim (a Win32-shaped core,
  see `docs/macos-port.md` M4).
- **Packaging**: `launcher/scripts/make-payload.sh` (any host) + `make-mac.sh` (macOS only);
  `game-build/release_b/pkp_build.py` builds the player's `pkp`.

---

## 3. Implemented, with the mechanism (so it survives a refactor)

**1 — LDAR/STLR.** AArch64 lets an `STLR` pass a later `LDAR`; a plain store followed by `LDAR` of another
word is unordered on every host (store buffer, x86 too). So `ldar*` = full barrier + seq_cst load and
`stlr*` = seq_cst store + barrier after it (barrier only on arm64; x86 keeps the TSO store and a compiler
fence). Do **not** map LDAR back to `__ATOMIC_ACQUIRE`: clang turns that into `LDAPR` for Apple cores,
which is a *weaker* guarantee than LDAR. Litmus: `build/litmus/ldar.cpp`, 300k rounds, up to 37 % lost
before, 0 after.

**2 — Vulkan portability.** `VK_KHR_portability_enumeration` on the instance (plus
`VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`) and `VK_KHR_portability_subset` on the device, or
`vkCreateDevice` fails on MoltenVK. The macros for the subset live in `<vulkan/vulkan_beta.h>` behind
`VK_ENABLE_BETA_EXTENSIONS`, so the name is passed as a string literal.

**3 — arm64 SHA-1.** The NEON block needs the crypto extension: `__attribute__((target("sha2")))` (clang)
or `target("arch=armv8-a+crypto")` (gcc) — without it clang refuses the intrinsics. `have_shani()` is true
on Apple and asks `getauxval(AT_HWCAP) & HWCAP_SHA1` on Linux.

**4 — host pages.** The shim applies a host page the **union** of the protections of the 4 KiB guest pages
on it (`os_posix_mem.cpp:176-183`, `host_page_protect`). Arming one guest page of a 16 KiB page therefore
changed nothing on the host: no fault, and the owner's copy went stale (invisible on Windows, where a host
page *is* a guest page). `to_host_pages()` rounds every range `arm`/`guard`/`release_page` touches out to
the host page and moves the bits with it. `host_pages()` is 1 on Windows, so the Windows behaviour is
byte-identical — keep it that way.

**7 — pool mirrors.** One memory for CPU and GPU on Apple silicon makes a device-local copy a copy of the
same memory. Default off there, `[nvn] pool_mirror = on` still works.

**8 — Metal shader-cache identity.** Metal keeps compiled shaders per *program identity*, in numbered files
under `DARWIN_USER_CACHE_DIR`; a process takes the first file no other running process of that identity
holds and looks only there. A binary without an identity shares its folder with every bundle-less program,
so a start can get a different file than the one a previous run (or a preparation pass) filled, and every
pipeline is compiled again. `pkp_build.py` writes `Info.plist`
(`CFBundleIdentifier org.pkp-recomp.game.<sha256 of the game folder>[:12]`) and links it with
`-Wl,-sectcreate,__TEXT,__info_plist,<file>`. `IDENTITY` is in the build stamp, so bumping it relinks.

> **Never run.** Zig's ld64 rejects `-sectcreate` (`unsupported linker arg`), so Linux cannot test it; the
> macOS link uses Apple clang. First check on a Mac:
> `otool -s __TEXT __info_plist <game>/pkp | head`, and the log should show Metal's cache folder names
> the game, not a shared one.

**12 — vsync.** Without it MoltenVK presents immediately and Metal shows frames mid-refresh (tore and
flashed partly drawn pictures on a MacBook). `[meta] display_rev = 1` switches vsync on once, for the
factory text and for an existing `controls.ini`; a player who turns it off keeps it off. Only compiled in
under `__APPLE__`.

**13 — frame limit through loading.** Loading runs unpaced by design (the loading code advances once per
frame). A *number* in `[display] fps_limit` now wins over that (`frame_limiter_set_player_cap`); `auto` and
`unlimited` keep the faster unpaced loading.

---

## 4. Open items, with how to do them

Each one: what to change, and what proves it. All of them need a Mac with the dump and Xcode Command Line
Tools (`docs/macos-port.md` M1–M3).

### 4.1 — 6: `shouldMaximizeConcurrentCompilation`

Metal's compiler service runs as many compilations at once as the GPU allows. The property lives on
`MTLDevice`/`NSProcessInfo` on macOS 14+, so it needs an Obj-C message send; this runtime has no Obj-C
(the whole POSIX layer is C).

Options, cheapest first:
1. One tiny `Obj-C++` translation unit for Apple (`runtime/src/gfx/extras/metal_mac.mm`) added only in the
   `APPLE` branch of `runtime/CMakeLists.txt`, containing
   `[MTLCreateSystemDefaultDevice() setShouldMaximizeConcurrentCompilation:YES]` guarded by
   `@available(macOS 14.0, *)`, exposed as `nxrt::gfx::metal_maximize_compilation()`. Called from the
   device-creation path in `runtime/src/gfx/vk_device.cpp` (after the instance, before the pipelines).
2. Or `objc_msgSend` casts from C. Works, but the selector/ABI dance is harder to keep right than one `.mm`.
- *Proves it*: `/usr/bin/log stream --predicate 'subsystem == "com.apple.MTLCompilerService"'` shows
  parallel compiles during prewarm; our own `[nvn.pipe]` line shows background builds per 30 s.
- *Watch out*: never link the `.mm` into the Windows/Linux build; the runtime must stay Obj-C-free there.

### 4.2 — 9: `pkp --prepare-pipelines`

Goal: a headless run that builds every pipeline in `pipeline_keys.bin` and exits, so a start finds them in
Metal's cache (see §3 item 8 — the identity is what makes the cache file stable).

State of the ground: `runtime/src/gfx/nvn/nvn_prewarm.cpp` already records every key
(`[nvn] pipeline_keys`, default `pipeline_keys.bin`), queues them at start in first-use order and builds
them on worker threads (`build_record` and friends). `pkp` itself has **no argument parsing anywhere**
(`game-build/src/pkp_stub.cpp` takes nothing).

Sketch:
1. `pkp_stub.cpp` (and `tools/build/recomp/games/pkp/...` if the stub is generated): scan `argv` for
   `--prepare-pipelines [--if-needed]`, publish it through `nxrt::config` (a `[boot] prepare_pipelines`
   key is enough; the runtime already reads its settings from there).
2. In `nvn_prewarm.cpp`, when that key is set: skip the guest, create the device headless (the
   `nxrt_gfx_demo --headless` path is the model), and instead of queueing keys for later, build every
   record of the key file synchronously, logging `built/total`.
3. Exit 0 when the queue is empty and the pipeline cache file is written; log the counts a state file
   needs (`pipelines-N.state`: driver, macOS build, identity, folder creation time — an update of any of
   those means "build again").
4. Wire it into the launcher: Set up after the build, and once per Play with `--if-needed`
   (`launcher/src-tauri/src/gamebuild.rs` is where the launcher runs `pkp_build.py`).
- *Proves it*: the second Play's own `[nvn.pipe]` line reports `keys known N from the file, 0 still to
  build`, and `slow graphics pipeline` lines disappear from the first minute of play.
- *Watch out*: do not make `--prepare-pipelines` a mode the game can enter by accident; a headless device
  with no window must never fall through into the guest's boot.

### 4.3 — 11: sticky read-only depth

The lazy-barrier half is ours already (`g_bar.pending` + `pending_conflict`,
`nvn_replay_pass.cpp:146-155`, plus `pass_unsafe` for storage writes). What is missing is the pass
end/begin pair that flips whenever `depth_ro` changes.

Rule to implement: a pass that is already in `DEPTH_STENCIL_READ_ONLY_OPTIMAL` keeps serving draws that do
**not write depth** (colour writes and depth *tests* are unaffected); only a draw that writes depth needs
the writable layout and must end the pass.

Site: `nvn_replay_draw.cpp:423-431`. Today `g_cmd.want_depth_ro = samples_depth_target(*pl)` goes straight
into `ensure_pass(...)`, and `ensure_pass` (`nvn_replay_pass.cpp:143-145`) ends the pass whenever
`P.depth_ro != depth_ro`. The draw's depth write is `S.ds.depth_test && S.ds.depth_write` (the pipeline
state already forces the host `depthWriteEnable` to false in a read-only pass, `nvn_replay_draw.cpp:272`).

```cpp
bool want_ro = g_cmd.want_depth_ro;
if (!want_ro && P.open && P.depth_ro && !(S.ds.depth_test && S.ds.depth_write)) want_ro = true;   // stay read-only
pass_ok = ensure_pass(want_ro, g_copies.fbl_want);
```
- *Proves it*: `[prof] render_split` logs `passes` and `end_pass` per frame (see §5). It only logs while
  frames are **presented** — the headless demo never presents, so this cannot be measured from
  `nxrt_gfx_demo`. Run the game for 30 s before and after.
- *Watch out*: a read-only depth attachment must never receive a depth write; if validation starts
  reporting `VUID-vkCmdDraw-format-…`/depth-write complaints, the condition above is wrong for some
  stencil path (`S.ds.stencil_wmask` is force-masked in a read-only pass, `nvn_replay_draw.cpp:291`).

### 4.4 — 14: MetalFX temporal upscaling

The largest item by far and the reason PokoMac forked Metal in: the frame's Vulkan recording is split
around MetalFX's own Metal queue with a timeline semaphore (GPU-side through MoltenVK's `MTLSharedEvent`;
a host round trip on KosmicKrisp), with FSR 1 as the fallback if MetalFX cannot start. FSR 1 and SMAA
already live in `runtime/src/gfx/extras/` (`fsr1_spatial.cpp`, `smaa_scene.cpp`) and `extras/temporal.cpp`
holds the history; the launcher and the in-game F3 screen each have an upscaler choice.

Do not start this without §4.1 and §4.2 in place: MetalFX is only worth its complexity once pipelines are
ready before play.

### 4.5 — 15: UDOT/SDOT and ZBIC-compressed NSOs

Neither is needed by the game this project ships: the lift of its main NSO reports **0 untranslated
instructions**, so adding these only helps another title (or a future dump).

- UDOT/SDOT: Advanced SIMD three-same, `0 Q 0 01110 size 1 Rm 100101 Rn Rd` (`size` 10 = SDOT on signed
  bytes, 11 = UDOT on unsigned bytes), 4 or 16 bytes per group. Add the pattern to
  `recomp/nxrecomp/lift/tier2/neon.py` next to the other dot-ish reductions, emit a `nx::udot4x8` /
  `nx::sdot4x8` helper from `cxx.h`, and add the scalar host form there too (the lifted code runs on x86).
  **A wrong pattern silently mistranslates guest code** — the lifter reports unknown opcodes, which is
  safe; guessing is not. Test with a tiny fixture NSO, not by eyeballing the game.
- ZBIC: a compressed NSO code/data section. If a dump needs it, the loader (`runtime/src/loader/module.cpp`
  or the lifter's NSO reader in `recomp/`) has to decompress before lifting, and the dump hash list
  (`game-build/release_b/dump_hashes.json`) has to record it.

### 4.6 — 16: KosmicKrisp as a second driver

The bundle ships MoltenVK as `libvulkan.1.dylib` next to the executable (`make-mac.sh`), and volk
`dlopen`s that name; anything that installs a Vulkan ICD works already. What is missing is (a) shipping
KosmicKrisp as an alternative and (b) the log line that says which driver is running and whether Metal's
shader cache is in the right folder (§3 item 8). Exercise it on a Mac with both ICDs present; nothing in
this tree needs to change to *load* it.

---

## 5. Measurement rigs (use these before believing any of the above)

| What | How |
|---|---|
| Passes / pass ends per frame | `[prof] render_split = 1` in the game folder's `pkp.ini`; the log prints `per frame: … passes %.1f` and `epoch bumps: … end_pass %.0f, begin pass %.0f` every 10 s **while frames present** |
| Pipeline builds | `[nvn.pipe]` every 30 s: `keys known N (A from the file, B new), M still to build`; `slow graphics pipeline` for anything ≥ 3 s |
| Whether the VkPipelineCache helped | `[nvn] pipeline_cache_moltenvk = auto` vs `off` (§3 item 5) — the decision needs one cold start each, same key file |
| Whether Metal's cache found it | the log line for the cache file (§3 item 8) and the `in the pipeline cache, so Metal compiled it again` note PokoMac added — port it if you keep the cache on |
| Write tracker | `[diag] ww_stats = 1` (faults per region), `mem.ww.faults` gauge |
| sgx/driver-level | `VK_LOADER_DEBUG=error,warn`, MoltenVK's `MVK_CONFIG_LOG_LEVEL=2` |

---

## 6. Traps found during this port

- **A host page is not a guest page** on Apple silicon (16 KiB vs 4 KiB) and the POSIX shim unions
  protections per host page. Any code that protects, guards or tracks 4 KiB at a time must round out
  (§3 item 4). `MAP_FIXED_NOREPLACE`, not `MAP_FIXED`, for guest views (`os_posix_mem.cpp`).
- **The pipeline cache is not the problem it looked like.** It was off on macOS for a while because the
  game stopped after NVN init; the real cause was the pool shadow copy writing past its buffer. Turning the
  cache off now costs a Metal compile of every pipeline at start. See the comment at
  `nvn_pipeline.cpp:242-247` before changing it.
- **`-sectcreate` is an Apple-ld flag.** Zig's lld refuses it, so a Linux cross-link cannot validate the
  identity (§3 item 8).
- **Set up relinks `pkp` when the build stamp changes**; `IDENTITY`, `FORMAT` and the tool hashes are in
  `tool_stamp()`/`lib_stamp()`. Changing the stamp shape or `FORMAT` throws away the player's compiled
  objects (minutes), so bump only what must be rebuilt.
- **`spctl` only waves through an Apple-issued Developer ID.** Without the secrets the bundle is ad-hoc
  signed and the zip's note walks the player through System Settings → Privacy & Security → Open Anyway,
  or `xattr -dr com.apple.quarantine`. `make-mac.sh` re-signs after notarization because stapling writes
  into the bundle.
- **CI**: `release.yml` runs `macos-payload` on **ubuntu** (harness, MoltenVK, lifter, icons) and the
  `macos` job only compiles the launcher and `libpkp_rt.dylib` and signs. Do not add `brew install` back to
  the macOS job; MoltenVK comes from the pinned release tarball (`MOLTENVK_VERSION`).
