// SPDX-License-Identifier: GPL-3.0-or-later
//! What the launcher does differently on each system: the game is a native program everywhere now (pkp.exe on Windows, pkp on Linux
//! and macOS), so every host decision below has one branch for Windows and one for the POSIX builds.
//!
//! Windows: the release's Windows tools run and pkp.exe starts directly. Linux and macOS: the launcher is a native program (Tauri
//! draws it with WebKitGTK, or with WKWebView on macOS), and the tools that only prepare files run as native programs from `tools/`:
//! Python 3.13 with lz4 and capstone (pkp_build.py, the lifter, extract_corpus.py), and Zig 0.17.0 on Linux, which links `pkp`
//! against the runtime library. `prepare_tools` fetches them once, each pinned by size and SHA-256; macOS compiles with Apple clang
//! and needs no Zig. Nothing runs through Proton or Wine any more: the shader translator, the harness and the game are host programs.
//! Its crate-local imports are download.rs (which has none) and the modules the native Set up walks (paths, settings, gamebuild, dump,
//! config, shaders); the two examples declare the same module set the binary does, so they compile the same code.

use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::sync::atomic::AtomicBool;

use crate::download::{self, Pinned};

/// macOS and Linux run the game as the native binary built from this tree: there is no pkp.exe, no Proton and none of the
/// release's Windows tools (Python, Zig, the shader translator), so every host decision below has this one branch.
pub fn native_game() -> bool {
    cfg!(any(target_os = "macos", target_os = "linux"))
}

/// The game executable in `dir`: `pkp` on macOS and Linux, `pkp.exe` everywhere else.
pub fn game_exe(dir: &Path) -> PathBuf {
    dir.join(if native_game() { "pkp" } else { "pkp.exe" })
}

/// The runtime library the game loads from its own folder (release model (b)): pkp_rt.dll, libpkp_rt.dylib, libpkp_rt.so.
pub fn runtime_lib_name() -> &'static str {
    if cfg!(windows) {
        "pkp_rt.dll"
    } else if cfg!(target_os = "macos") {
        "libpkp_rt.dylib"
    } else {
        "libpkp_rt.so"
    }
}

pub fn runtime_lib(dir: &Path) -> PathBuf {
    dir.join(runtime_lib_name())
}

/// The GPU model the shader translator is asked for: MoltenVK on macOS (its limits and precision), the desktop Vulkan driver
/// everywhere else. Same values the release's Windows setup passes.
pub fn harness_host() -> &'static str {
    if cfg!(target_os = "macos") {
        "moltenvk"
    } else {
        "desktop"
    }
}

/// The file the game build's link step takes the runtime from, relative to the package's tools folder.
pub fn runtime_lib_rel() -> &'static str {
    if cfg!(windows) {
        "build/sdk/pkp_rt.lib"
    } else if cfg!(target_os = "macos") {
        "build/sdk/libpkp_rt.dylib"
    } else {
        "build/sdk/libpkp_rt.so"
    }
}

/// The decoder libraries pkp_rt.dll loads at run time next to the game (astcdec-*-shared.dll). macOS samples ASTC in Metal and
/// Linux decodes with the astcenc that is linked into the runtime, so only Windows has them.
pub fn has_decoder_dlls() -> bool {
    cfg!(windows)
}

/// Linux and macOS: the hidden run Windows does at Set up (shaders::warm_up). The romfs corpus cannot hold the blocks the game builds
/// while it runs, so one headless run collects them and the translator merges them: without it the first real start draws
/// with unlinked programs (models that never appear). A scratch save folder, so the player's own save is never touched.
pub fn native_warm_up(game_dir: &Path, cancel: &std::sync::atomic::AtomicBool) -> Result<Option<String>, String> {
    use std::sync::atomic::Ordering;
    let exe = game_exe(game_dir);
    if !exe.is_file() {
        return Ok(None);
    }
    let scratch = crate::shaders::cache_dir(game_dir).join("warmup");
    let _ = std::fs::remove_dir_all(&scratch);
    std::fs::create_dir_all(scratch.join("save")).map_err(|e| format!("cannot create {}: {}", scratch.display(), e))?;
    let mut child = command(&exe)
        .current_dir(game_dir)
        .arg("--headless")
        .args(["--max-seconds", &crate::shaders::WARMUP_SECONDS.to_string()])
        .arg("--log")
        .arg(scratch.join("warmup.log"))
        // environment variables win over pkp.ini, as in the Windows warm-up
        .env("NXRT_FS_SAVE_DIR", scratch.join("save"))
        .env("NXRT_NVN_DUMP_MISSING", crate::shaders::MISSING_REL)
        .env("NXRT_AUDIO_SINK", "null")
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .map_err(|e| format!("could not start the warm-up run: {}", e))?;
    // the run ends with the exit code 124 when --max-seconds is up; a crash only means fewer blocks were collected
    let limit = std::time::Duration::from_secs(crate::shaders::WARMUP_SECONDS as u64 + 240);
    let start = std::time::Instant::now();
    loop {
        if cancel.load(Ordering::SeqCst) {
            let _ = child.kill();
            let _ = child.wait();
            let _ = std::fs::remove_dir_all(&scratch);
            return Err("cancelled".into());
        }
        if let Ok(Some(_)) = child.try_wait() {
            break;
        }
        if start.elapsed() > limit {
            let _ = child.kill();
            let _ = child.wait();
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
    let _ = std::fs::remove_dir_all(&scratch);
    native_translate_shaders(game_dir)
}

/// Linux and macOS: the shader cache is built by the C# harness in `tools/shaders`, not by the release's Windows tools. Runs it over
/// the corpus the game dumped (`[nvn] dump_missing`, shaders::MISSING_REL), writing `<game>/shader_cache/spirv`.
/// Returns the harness's last line, or None when there is nothing to translate or no harness to run.
pub fn native_translate_shaders(game_dir: &Path) -> Result<Option<String>, String> {
    // The game writes the blocks it has no translation for to [nvn] dump_missing, which Set up sets to shader_cache/missing
    // (shaders::MISSING_REL). This used to read shader_corpus, a folder only the development ini ever used, so the blocks the
    // game dumped were never translated and every one of them stayed an unlinked program (black geometry, missing prompts).
    let corpus = game_dir.join(crate::shaders::MISSING_REL);
    if !corpus.join("manifest.tsv").is_file() {
        return Ok(None);
    }
    let cache = game_dir.join("shader_cache").join("spirv");
    let dll = std::env::var_os("PKP_SPIRV_HARNESS")
        .map(PathBuf::from)
        .or_else(|| {
            // the game folder's tools/, then the launcher's own (the package ships tools/harness)
            [game_dir.join("tools"), crate::paths::exe_dir().join("tools")]
                .into_iter()
                .flat_map(|t| [t.join("harness").join("spirv_harness.dll"), t.join("spirv_harness.dll")])
                .find(|p| p.is_file())
        })
        .or_else(|| {
            // a development tree: the launcher sits in work/play, the harness in tools/shaders/build
            let mut dir = std::env::current_exe().ok()?.parent()?.to_path_buf();
            for _ in 0..6 {
                let p = dir.join("tools").join("shaders").join("build").join("bin").join("Release").join("net10.0").join("spirv_harness.dll");
                if p.is_file() {
                    return Some(p);
                }
                if !dir.pop() {
                    break;
                }
            }
            None
        });
    let Some(dll) = dll else {
        return Err("the shader translator (spirv_harness.dll) was not found: set PKP_SPIRV_HARNESS or put it in <game>/tools".into());
    };
    // Two jobs and the workstation collector: the harness allocates heavily per worker and the server GC grows its heap with
    // the worker count (six workers measured 751 MiB; the point is to stay far below the game's own footprint on a 16 GB host).
    // Into a scratch folder, never straight into the cache: the harness writes results.tsv for the blocks it was given, so a
    // translation of the dumped shaders alone would replace the cache's table with just those (it did: a full 99,012-row
    // table became 6,064 after one Play). The new rows are merged below.
    let scratch = game_dir.join("shader_cache").join(".translate");
    let _ = std::fs::remove_dir_all(&scratch);
    std::fs::create_dir_all(&scratch).map_err(|e| format!("cannot create {}: {}", scratch.display(), e))?;
    // A self-contained publish ships its own runtime: run its apphost next to the dll instead of the system's dotnet.
    let apphost = dll.with_file_name("spirv_harness");
    let mut runner = if apphost.is_file() { command(&apphost) } else { command("dotnet") };
    if !apphost.is_file() {
        runner.arg(&dll);
    }
    let out = runner
        .arg("--batch")
        .arg(&corpus)
        .arg("--out")
        .arg(&scratch)
        .args(["--jobs", "2", "--host", harness_host(), "--no-val"])
        .env("DOTNET_gcServer", "0")
        .env("DOTNET_GCHeapHardLimit", "0x60000000")   // 1.5 GiB ceiling for the translation
        .output()
        .map_err(|e| format!("could not run the shader translator: {}", e))?;
    let text = String::from_utf8_lossy(&out.stdout);
    let last = text.lines().rev().find(|l| !l.trim().is_empty()).unwrap_or("").to_string();
    if !out.status.success() {
        let _ = std::fs::remove_dir_all(&scratch);
        return Err(format!("the shader translator failed: {}", last));
    }
    let merged = merge_translation(&scratch, &cache);
    // The scratch's results.tsv says which blocks failed: keep the folder for the next run only if some did.
    let failed = std::fs::read_to_string(scratch.join("results.tsv"))
        .map(|t| t.lines().skip(1).filter(|l| l.split('\t').nth(4).is_some_and(|s| s != "ok")).count())
        .unwrap_or(1);
    let _ = std::fs::remove_dir_all(&scratch);
    if failed == 0 {
        let _ = std::fs::remove_dir_all(&corpus);
    }
    merged.map(|n| Some(if n > 0 { format!("{}: {} new shaders merged into the cache", last, n) } else { last }))
}

/// Moves what `scratch` translated into `cache` without losing the rows the cache already had: the .spv files are copied
/// (a hash already there is left alone) and the new rows are appended to results.tsv.
fn merge_translation(scratch: &Path, cache: &Path) -> Result<usize, String> {
    use std::collections::HashSet;
    let table = cache.join("results.tsv");
    let header = "hash\tgroup\tstage\tstages_using\tstatus\tms\tspv_bytes\tresources\ttranslator_messages\terror\tdraw_time_queries\tglslc_reflection";
    let existing_text = std::fs::read_to_string(&table).unwrap_or_default();
    let mut known: HashSet<String> = HashSet::new();
    for line in existing_text.lines().skip(1) {
        if let Some(h) = line.split('\t').next() {
            if !h.is_empty() {
                known.insert(h.to_string());
            }
        }
    }
    let fresh = std::fs::read_to_string(scratch.join("results.tsv")).map_err(|e| format!("the translator wrote no results.tsv: {}", e))?;
    let mut added_rows = String::new();
    let mut n = 0usize;
    for line in fresh.lines().skip(1) {
        let h = line.split('\t').next().unwrap_or("");
        if h.is_empty() || !known.insert(h.to_string()) {
            continue;
        }
        let src = scratch.join(format!("{}.spv", h));
        if src.is_file() {
            let _ = std::fs::copy(&src, cache.join(format!("{}.spv", h)));
        }
        added_rows.push_str(line);
        added_rows.push('\n');
        n += 1;
    }
    if n > 0 {
        use std::io::Write;
        let mut f = std::fs::OpenOptions::new().create(true).append(true).open(&table).map_err(|e| format!("cannot write {}: {}", table.display(), e))?;
        if existing_text.is_empty() {
            f.write_all(format!("{}\n", header).as_bytes()).map_err(|e| e.to_string())?;
        }
        f.write_all(added_rows.as_bytes()).map_err(|e| format!("cannot write {}: {}", table.display(), e))?;
    }
    Ok(n)
}

/// The tools the Linux and macOS setup runs: the repository's Python (with lz4) and the C# shader harness.
pub struct SetupTools {
    pub python: PathBuf,
    pub extract: PathBuf,
    pub harness: PathBuf,
}

/// A path relative to the development tree, searched upward from the launcher (the release layout puts the same files
/// under <game>/tools instead).
fn find_up(rel: &str) -> Option<PathBuf> {
    let mut dir = std::env::current_exe().ok()?.parent()?.to_path_buf();
    for _ in 0..6 {
        let p = dir.join(rel);
        if p.exists() {
            return Some(p);
        }
        if !dir.pop() {
            break;
        }
    }
    None
}

pub fn native_setup_tools(game_dir: &Path) -> Result<SetupTools, String> {
    // The game folder's tools/ first (a tree that keeps them there), then the launcher's own (the package ships them in
    // Contents/MacOS/tools), then upward from the exe (a development checkout).
    let local = |rel: &str| {
        [game_dir.join("tools").join(rel), crate::paths::exe_dir().join("tools").join(rel)].into_iter().find(|p| p.exists())
    };
    let python = std::env::var_os("PKP_SETUP_PYTHON")
        .map(PathBuf::from)
        .or_else(|| {
            // the Python prepare_tools fetched: into the game folder on macOS (the bundle is read only), next to the
            // AppImage on Linux. The bundle's copy is checked second, for a package that still carries one.
            [crate::host::tools_dir_for(game_dir), crate::paths::exe_dir().join("tools")]
                .into_iter()
                .map(|t| python(&t))
                .find(|p| p.is_file())
        })
        .or_else(|| local("python3"))
        .or_else(|| find_up("tools/.venv/bin/python3"))
        .unwrap_or_else(|| PathBuf::from("python3"));
    let extract = std::env::var_os("PKP_EXTRACT_CORPUS")
        .map(PathBuf::from)
        // tools/shaders/ is the Linux package's layout (make-linux.sh, checked by release.yml) and the Windows one;
        // tools/ itself is the macOS payload's (make-payload.sh)
        .or_else(|| local("shaders/extract_corpus.py"))
        .or_else(|| local("extract_corpus.py"))
        .or_else(|| find_up("tools/shaders/ampere/extract_corpus.py"))
        .ok_or("extract_corpus.py was not found (set PKP_EXTRACT_CORPUS, or put it in <game>/tools/shaders)")?;
    let harness = std::env::var_os("PKP_SPIRV_HARNESS")
        .map(PathBuf::from)
        .or_else(|| local("harness/spirv_harness.dll"))
        .or_else(|| local("spirv_harness.dll"))
        .or_else(|| find_up("tools/shaders/build/bin/Release/net10.0/spirv_harness.dll"))
        .ok_or("spirv_harness.dll was not found (set PKP_SPIRV_HARNESS, or put it in <game>/tools)")?;
    Ok(SetupTools { python, extract, harness })
}

/// Everything Set up does on Linux and macOS, for the window and for `--setup`: write pkp.ini from the chosen dump, fetch the
/// build tools, build pkp from the dump (release model b), then extract and translate the shader corpus. Progress is
/// (phase, done, total, message), as the shader job's own.
pub fn native_set_up_install(
    cfg: &crate::config::Config,
    cancel: &std::sync::atomic::AtomicBool,
    progress: &mut dyn FnMut(&str, u64, u64, &str),
) -> Result<usize, String> {
    let report = crate::dump::scan(&cfg.dump_dir);
    let chosen = report.chosen.ok_or_else(|| "Choose usable game files first.".to_string())?;
    let dir = crate::paths::game_dir_of(cfg);
    std::fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
    crate::settings::ensure_play_paths(&dir, &chosen.exefs, &chosen.romfs, &chosen.romfs_base, &report.aoc_dir)?;
    let tools_dir = crate::paths::exe_dir().join("tools");
    let writable = tools_dir_for(&dir);
    if crate::gamebuild::is_package_b(&tools_dir) {
        // Linux and macOS fetch their native tools once (Python with lz4 and capstone; macOS compiles with Apple clang)
        let ready = prepare_tools(&writable, cancel, &mut |done, total, message| progress("tools", done, total, &message))?;
        if !ready {
            return Err("Setup paused.".into());
        }
        if !crate::gamebuild::is_built(&dir, &tools_dir) {
            let tools = crate::gamebuild::find_tools(&tools_dir, &dir)?;
            let exefs = {
                let ini = crate::settings::read(&dir);
                let p = ini.pkp.get("fs.exefs").cloned().unwrap_or_default();
                let pb = std::path::PathBuf::from(&p);
                if pb.is_absolute() { pb } else { dir.join(pb) }
            };
            let jobs = std::thread::available_parallelism().map(|n| n.get().saturating_sub(1).max(1)).unwrap_or(2);
            let job = crate::gamebuild::Job { game_dir: dir.clone(), exefs, jobs, idle: false, log: None };
            let built = crate::gamebuild::run(&job, &tools, cancel, |t| progress(&t.phase, t.done, t.total, &t.message))?;
            if !built {
                return Err("Setup paused.".into());
            }
        }
    }
    native_set_up(&dir, cancel, progress)
}

/// Linux and macOS: extract the game's shader corpus from its RomFS (extract_corpus.py) and translate every block
/// (spirv_harness), then mark the cache complete. The game build is `native_set_up_install`'s step, before this one.
pub fn native_set_up(
    game_dir: &Path,
    cancel: &std::sync::atomic::AtomicBool,
    progress: &mut dyn FnMut(&str, u64, u64, &str),
) -> Result<usize, String> {
    use std::sync::atomic::Ordering;
    let cfg = crate::settings::read(game_dir);
    let get = |k: &str| cfg.pkp.get(k).map(String::as_str).filter(|v| !v.is_empty()).map(str::to_string);
    // The ini's paths may be relative to the game folder (the runtime resolves them itself); the extractor and the harness
    // are separate processes started from the launcher's own folder, so they need absolute ones.
    let resolve = |p: String| {
        let pb = std::path::PathBuf::from(&p);
        if pb.is_absolute() {
            pb
        } else {
            game_dir.join(pb)
        }
    };
    let romfs = resolve(get("fs.romfs").ok_or("pkp.ini has no [fs] romfs")?);
    let exefs = resolve(get("fs.exefs").ok_or("pkp.ini has no [fs] exefs")?);
    let t = native_setup_tools(game_dir)?;
    let cache = crate::shaders::cache_dir(game_dir);
    let corpus = cache.join("corpus");
    let spirv = cache.join("spirv");
    std::fs::create_dir_all(&corpus).map_err(|e| format!("cannot make {}: {}", corpus.display(), e))?;
    std::fs::create_dir_all(&spirv).map_err(|e| format!("cannot make {}: {}", spirv.display(), e))?;

    progress("extract", 0, 0, "Finding the game's shaders in its RomFS...");
    let out = command(&t.python)
        .arg("-I").arg("-B")
        .arg(&t.extract)
        .arg("--romfs")
        .arg(&romfs)
        .arg("--main")
        .arg(Path::new(&exefs).join("main"))
        .arg("--out")
        .arg(&corpus)
        .args(["--jobs", "4"])
        .output()
        .map_err(|e| format!("could not run the corpus extractor: {}", e))?;
    if !out.status.success() {
        let err = String::from_utf8_lossy(&out.stderr);
        return Err(format!("the corpus extractor failed ({}): {}", t.python.display(), err.lines().last().unwrap_or("")));
    }
    let blocks = std::fs::read_to_string(corpus.join("unique.tsv"))
        .map(|t| t.lines().count().saturating_sub(1))
        .unwrap_or(0);
    if blocks == 0 {
        return Err("the corpus extractor found no shader blocks".into());
    }

    // A cache that already holds a translation for every block (a finished run, or one made before the marker was written)
    // is not translated again: Set up is then the extraction plus the marker.
    let have = std::fs::read_to_string(spirv.join("results.tsv")).map(|t| t.lines().count().saturating_sub(1)).unwrap_or(0);
    if have >= blocks {
        progress("translate", blocks as u64, blocks as u64, "Every shader is translated already.");
        warm_up_run(game_dir, cancel, progress);
        crate::shaders::mark_complete(game_dir, &romfs.to_string_lossy(), blocks)?;
        return Ok(blocks);
    }

    progress("translate", 0, blocks as u64, &format!("Translating {} shaders...", blocks));
    // The package ships a self-contained harness: run its apphost, not the system's dotnet, which a launcher started from
    // Finder does not have on its PATH (that was "could not run the shader translator: No such file or directory").
    let apphost = t.harness.with_file_name("spirv_harness");
    let mut runner = if apphost.is_file() { command(&apphost) } else { command("dotnet") };
    if !apphost.is_file() {
        runner.arg(&t.harness);
    }
    let mut child = runner
        .arg("--batch")
        .arg(&corpus)
        .arg("--out")
        .arg(&spirv)
        .args(["--jobs", "6", "--host", harness_host(), "--no-val"])   // as the Windows setup
        .env("DOTNET_gcServer", "0")
        .env("DOTNET_GCHeapHardLimit", "0x60000000")
        .stdout(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| format!("could not run the shader translator: {}", e))?;
    // The harness prints "  <n>/<total>  <s> s" every 500 blocks: that is the progress (counting .spv files stalls while a
    // second run overwrites the ones a stopped run left behind).
    let now = std::sync::Arc::new(std::sync::atomic::AtomicU64::new(0));
    let reader = {
        let now = now.clone();
        let out = child.stdout.take();
        std::thread::spawn(move || {
            use std::io::BufRead;
            if let Some(out) = out {
                for line in std::io::BufReader::new(out).lines().map_while(Result::ok) {
                    if let Some((n, _)) = line.trim().split_once('/') {
                        if let Ok(v) = n.trim().parse::<u64>() {
                            now.store(v, Ordering::Relaxed);
                        }
                    }
                }
            }
        })
    };
    loop {
        if cancel.load(Ordering::SeqCst) {
            let _ = child.kill();
            let _ = child.wait();
            return Err("cancelled".into());
        }
        match child.try_wait() {
            Ok(Some(st)) => {
                if !st.success() {
                    // The harness appends what went wrong to harness_error.txt in --out: name it, not just the exit status.
                    let detail = std::fs::read_to_string(spirv.join("harness_error.txt")).unwrap_or_default();
                    let first = detail.lines().next().unwrap_or("");
                    return Err(format!("the shader translator failed ({}): {}", st, first));
                }
                break;
            }
            Ok(None) => {}
            Err(e) => return Err(format!("lost track of the shader translator: {}", e)),
        }
        let done = now.load(Ordering::Relaxed).min(blocks as u64);
        progress("translate", done, blocks as u64, &format!("Translating shaders: {} of {}...", done, blocks));
        std::thread::sleep(std::time::Duration::from_secs(2));
    }
    let _ = reader.join();
    warm_up_run(game_dir, cancel, progress);
    crate::shaders::mark_complete(game_dir, &romfs.to_string_lossy(), blocks)?;
    Ok(blocks)
}

/// The warm-up as Set up reports it: a phase of its own, and a failure is only a message (the first Play then collects the
/// blocks and the launcher translates them when it exits, as it always did).
fn warm_up_run(game_dir: &Path, cancel: &std::sync::atomic::AtomicBool, progress: &mut dyn FnMut(&str, u64, u64, &str)) {
    progress("warmup", 0, 0, "Playing the opening once, to collect the shaders it builds...");
    match native_warm_up(game_dir, cancel) {
        Ok(Some(line)) => progress("warmup", 1, 1, &line),
        Ok(None) => progress("warmup", 1, 1, "No shader was missing."),
        Err(e) if e == "cancelled" => {}
        Err(e) => progress("warmup", 1, 1, &format!("The warm-up run did not finish: {}", e)),
    }
}

/// macOS: the share of physical memory that is free (free + inactive + speculative pages), 0..1. None elsewhere. The
/// launcher stops the game below 10 %: on a 16 GB host the game's world load reaches ~7 GB and macOS must keep room to
/// compress and swap, or the whole machine stops responding (a hard reboot, twice).
#[cfg(target_os = "macos")]
pub fn free_memory_ratio() -> Option<f64> {
    unsafe {
        let mut memsize: u64 = 0;
        let mut len = std::mem::size_of::<u64>();
        if libc::sysctlbyname(c"hw.memsize".as_ptr(), &mut memsize as *mut _ as *mut _, &mut len, std::ptr::null_mut(), 0) != 0
            || memsize == 0
        {
            return None;
        }
        let mut page_size: u64 = 0;
        let mut plen = std::mem::size_of::<u64>();
        if libc::sysctlbyname(c"hw.pagesize".as_ptr(), &mut page_size as *mut _ as *mut _, &mut plen, std::ptr::null_mut(), 0) != 0 {
            page_size = 16384;
        }
        let mut stats: libc::vm_statistics64 = std::mem::zeroed();
        let mut count = libc::HOST_VM_INFO64_COUNT;
        if libc::host_statistics64(libc::mach_host_self(), libc::HOST_VM_INFO64, &mut stats as *mut _ as *mut _, &mut count)
            != libc::KERN_SUCCESS
        {
            return None;
        }
        let pages = stats.free_count as u64 + stats.inactive_count as u64 + stats.speculative_count as u64;
        Some((pages.saturating_mul(page_size)) as f64 / memsize as f64)
    }
}

/// The kernel's memory-pressure level on macOS: 1 normal, 2 warning, 4 critical (kern.memorystatus_vm_pressure_level).
/// The free ratio alone missed a crash: the game sat at 7.8 GB with a few GB of inactive file cache (so the ratio read ~25 %)
/// while the kernel was already refusing to fault pages in, and the GPU driver took a SIGBUS inside vkQueueSubmit.
#[cfg(target_os = "macos")]
pub fn memory_pressure_level() -> Option<i32> {
    let mut level: i32 = 0;
    let mut len = std::mem::size_of::<i32>();
    let rc = unsafe {
        libc::sysctlbyname(c"kern.memorystatus_vm_pressure_level".as_ptr(), &mut level as *mut _ as *mut _, &mut len, std::ptr::null_mut(), 0)
    };
    (rc == 0).then_some(level)
}

/// True when the game must be stopped to keep the machine alive. Critical pressure, or under 6 % free: the game needs
/// ~9 GB and a 16 GB host has to compress/swap the rest, so a warning-level stop would kill it every time it loads the
/// world (the disk has room for swap; the kernel's critical level is the real danger sign).
#[cfg(target_os = "macos")]
pub fn memory_is_short() -> bool {
    let ratio = free_memory_ratio();
    let level = memory_pressure_level();
    level.is_some_and(|l| l >= 4) || ratio.is_some_and(|f| f < 0.06)
}

/// Windows and Linux: nothing to watch here -- the game's own memguard ([memory] limit_gb) is the guard.
#[cfg(not(target_os = "macos"))]
pub fn memory_is_short() -> bool {
    false
}

/// What to call this system in messages and logs.
pub fn os_name() -> &'static str {
    if cfg!(windows) {
        "Windows"
    } else if cfg!(target_os = "macos") {
        "macOS"
    } else {
        "Linux"
    }
}

/// What to call the game in messages.
pub fn game_exe_name() -> &'static str {
    if native_game() {
        "pkp"
    } else {
        "pkp.exe"
    }
}

/// The Python that runs the release's scripts (pkp_build.py, extract_corpus.py, fetch_dlss.py).
pub fn python(tools_dir: &Path) -> PathBuf {
    if cfg!(windows) {
        tools_dir.join("python").join("python.exe")
    } else if cfg!(target_os = "macos") {
        // Fetched by prepare_tools at first Set up, as on Linux (there is no bundled Python in the macOS package).
        tools_dir.join("macos").join("python").join("bin").join("python3")
    } else {
        tools_dir.join("linux").join("python").join("bin").join("python3")
    }
}

/// The Zig pkp_build.py compiles and links with (the script finds the same path on its own).
pub fn zig(tools_dir: &Path) -> PathBuf {
    if cfg!(windows) {
        tools_dir.join("build").join("zig").join("zig.exe")
    } else {
        tools_dir.join("linux").join("zig").join("zig")
    }
}

/// A command for a program of this system (Python, tar, xdg-open). In an AppImage, whose runtime points
/// variables such as GTK_PATH, GIO_EXTRA_MODULES or XDG_DATA_DIRS into the AppImage for the launcher's own libraries, the
/// program gets the environment without those entries (and without APPDIR, APPIMAGE, ARGV0, OWD); elsewhere nothing
/// changes. After tinfoil-chewer/pokopia-recomp-linux (GPL-3.0) launcher/src-tauri/src/linux.rs `host_env`.
pub fn command<S: AsRef<std::ffi::OsStr>>(program: S) -> Command {
    let mut cmd = Command::new(program);
    // No .pyc files anywhere: the lifter and the extractor run from inside the .app on macOS, and a __pycache__ written into the
    // bundle breaks its code signature (codesign --verify then reports "file added", and macOS refuses to open the app).
    cmd.env("PYTHONDONTWRITEBYTECODE", "1");
    let appdir = std::env::var("APPDIR").unwrap_or_default();
    let vars = std::env::vars_os().filter_map(|(k, v)| Some((k.into_string().ok()?, v.into_string().ok()?)));
    for (k, v) in appimage_env_fixes(&appdir, vars) {
        match v {
            Some(v) => cmd.env(k, v),
            None => cmd.env_remove(k),
        };
    }
    cmd
}

/// What `command` changes for `appdir` (empty: not an AppImage, nothing): (variable, Some(new value) or None = removed).
fn appimage_env_fixes(appdir: &str, vars: impl Iterator<Item = (String, String)>) -> Vec<(String, Option<String>)> {
    let appdir = appdir.trim_end_matches('/');
    if appdir.is_empty() {
        return Vec::new();
    }
    let mut out = Vec::new();
    for (k, v) in vars {
        if matches!(k.as_str(), "APPDIR" | "APPIMAGE" | "ARGV0" | "OWD") {
            out.push((k, None));
        } else if v.contains(appdir) {
            // a list keeps its other entries; a single path into the AppImage goes
            let kept: Vec<&str> = v.split(':').filter(|e| !e.is_empty() && !e.contains(appdir)).collect();
            out.push((k, (v.contains(':') && !kept.is_empty()).then(|| kept.join(":"))));
        }
    }
    out
}

/// Stops a tool and what it started: on Linux the compilers, which `lower_priority` made the leaders of their own process group.
pub fn kill(child: &mut Child) {
    #[cfg(not(windows))]
    unsafe {
        libc::kill(-(child.id() as i32), libc::SIGKILL);
    }
    let _ = child.kill();
}

/// Linux: the tool runs niced (10, idle 19; its children inherit it) in a process group of its own, for `kill`.
#[cfg(not(windows))]
pub fn lower_priority(cmd: &mut Command, idle: bool) {
    use std::os::unix::process::CommandExt;
    let n = if idle { 19 } else { 10 };
    cmd.process_group(0);
    // SAFETY: nice(2) is async-signal-safe, which is all pre_exec asks of the closure
    unsafe {
        cmd.pre_exec(move || {
            libc::nice(n);
            Ok(())
        });
    }
}

/// Free bytes on the file system that holds `dir` (or its nearest existing parent).
#[cfg(not(windows))]
pub fn free_bytes(dir: &Path) -> Option<u64> {
    use std::os::unix::ffi::OsStrExt;
    let mut d = dir;
    while !d.exists() {
        d = d.parent()?;
    }
    let c = std::ffi::CString::new(d.as_os_str().as_bytes()).ok()?;
    // SAFETY: c is NUL-terminated and s is a valid out pointer for the call
    let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
    (unsafe { libc::statvfs(c.as_ptr(), &mut s) } == 0).then(|| s.f_bavail as u64 * s.f_frsize as u64)
}

/// Opens a file in its default app, a folder in the file manager, or a link in the browser.
pub fn open(target: &std::ffi::OsStr) -> Result<(), String> {
    let opener = if cfg!(windows) {
        "explorer.exe"
    } else if cfg!(target_os = "macos") {
        "open"   // xdg-open does not exist on macOS, so the Setup log and folder buttons did nothing
    } else {
        "xdg-open"
    };
    let mut child = command(opener).arg(target).spawn().map_err(|e| format!("Could not open {}: {}", target.to_string_lossy(), e))?;
    // reaped, so no exited opener lingers as a zombie while the launcher runs
    std::thread::spawn(move || child.wait());
    Ok(())
}

// python-build-standalone 20261013 (its SHA256SUMS), the PyPI wheels the Windows release bundles, Zig 0.17.0 (ziglang.org
// download index). Bump together with the Windows release's tools/python and tools/build/zig.
const PYTHON: Pinned<'static> = Pinned {
    file: "cpython-3.13.16+20261013-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
    url: "https://github.com/astral-sh/python-build-standalone/releases/download/20261013/cpython-3.13.16%2B20261013-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
    size: 35_099_487,
    sha256: "d0080766b6b74aeb81e1949ef0d9a20e6c17f7b160fb295a69b936ccb17660be",
};
const WHEELS: [Pinned<'static>; 2] = [
    Pinned {
        file: "lz4-4.4.5-cp313-cp313-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl",
        url: "https://files.pythonhosted.org/packages/d2/fb/ba9256c48266a09012ed1d9b0253b9aa4fe9cdff094f8febf5b26a4aa2a2/lz4-4.4.5-cp313-cp313-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl",
        size: 1_368_257,
        sha256: "451039b609b9a88a934800b5fc6ee401c89ad9c175abf2f4d9f8b2e4ef1afc64",
    },
    Pinned {
        file: "capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
        url: "https://files.pythonhosted.org/packages/d1/39/17747862222bb062e86b501f1f148d5ff589b77908b080d30f7f085cbfb7/capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
        size: 1_485_316,
        sha256: "273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e",
    },
];
const PYTHON_MACOS: Pinned<'static> = Pinned {
    file: "cpython-3.13.16+20261013-aarch64-apple-darwin-install_only_stripped.tar.gz",
    url: "https://github.com/astral-sh/python-build-standalone/releases/download/20261013/cpython-3.13.16%2B20261013-aarch64-apple-darwin-install_only_stripped.tar.gz",
    size: 25_268_458,
    sha256: "16a5332fb0dcb77f163fc8f8cbccd95be1644cc53e68e5d2a79f50afd6680a0e",
};
const WHEELS_MACOS: [Pinned<'static>; 2] = [
    Pinned {
        file: "lz4-4.4.5-cp313-cp313-macosx_11_0_arm64.whl",
        url: "https://files.pythonhosted.org/packages/8f/3f/ea3334e59de30871d773963997ecdba96c4584c5f8007fd83cfc8f1ee935/lz4-4.4.5-cp313-cp313-macosx_11_0_arm64.whl",
        size: 207_163,
        sha256: "b424df1076e40d4e884cfcc4c77d815368b7fb9ebcd7e634f937725cd9a8a72a",
    },
    Pinned {
        file: "capstone-5.0.9-py3-none-macosx_11_0_arm64.whl",
        url: "https://files.pythonhosted.org/packages/42/9a/4eecfbc94961cda70301fd13de69042be1943bee456e37bc1369840601b1/capstone-5.0.9-py3-none-macosx_11_0_arm64.whl",
        size: 1_202_361,
        sha256: "fc269c5082e17d7f9f265cd6980e2fdc6ab572df913951d76db0357e06b8001d",
    },
];
const ZIG: Pinned<'static> = Pinned {
    file: "zig-x86_64-linux-0.17.0.tar.xz",
    url: "https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz",
    size: 57_332_648,
    sha256: "1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026",
};

/// Where the fetched tools go: next to the AppImage on Linux (exe_dir is the .AppImage's folder), and in the game folder on
/// macOS, because the launcher runs from inside a bundle that must not be written to (a signature covers it, and
/// /Applications may be owned by another account).
pub fn tools_dir_for(game_dir: &Path) -> PathBuf {
    if cfg!(target_os = "macos") {
        game_dir.join("tools")
    } else {
        crate::paths::exe_dir().join("tools")
    }
}

/// The Linux Python can run the release's scripts.
fn python_ready(py: &Path) -> bool {
    let mut c = command(py);
    c.args(["-I", "-B", "-c", "import lz4.block, capstone"]).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null());
    c.status().map(|s| s.success()).unwrap_or(false)
}

/// Linux and macOS: fetches the native build tools into `tools/linux` or `tools/macos` (python/ with lz4 and capstone, and
/// zig/ on Linux) unless they are there. `report(done, total, text)` gets the download progress. Ok(false) = cancelled.
/// Windows: nothing to do, the package bundles them.
pub fn prepare_tools(tools_dir: &Path, cancel: &AtomicBool, report: &mut dyn FnMut(u64, u64, String)) -> Result<bool, String> {
    if cfg!(windows) {
        return Ok(true);
    }
    let macos = cfg!(target_os = "macos");
    let what = if macos { "macOS" } else { "Linux" };
    let (python_pin, wheels): (&Pinned, &[Pinned]) = if macos { (&PYTHON_MACOS, &WHEELS_MACOS) } else { (&PYTHON, &WHEELS) };
    let platform = tools_dir.join(if macos { "macos" } else { "linux" });
    let (py, zig) = (python(tools_dir), zig(tools_dir));
    let py_ok = python_ready(&py);
    let zig_ok = macos || zig.is_file();   // macOS compiles with Apple clang
    if py_ok && zig_ok {
        return Ok(true);
    }
    let dl = platform.join("downloads");
    std::fs::create_dir_all(&dl).map_err(|e| format!("cannot create {}: {}", dl.display(), e))?;
    let mut need: Vec<&Pinned> = Vec::new();
    if !py_ok {
        need.push(python_pin);
        need.extend(wheels.iter());
    }
    if !zig_ok {
        need.push(&ZIG);
    }
    let total: u64 = need.iter().map(|p| p.size).sum();
    let (agent, mut done) = (download::agent(true), 0u64);
    for p in &need {
        let mb = |b: u64| b as f64 / (1u64 << 20) as f64;
        let ok = download::fetch(&agent, p, &dl, cancel, &mut |n| {
            report(done + n, total, format!("Downloading the {} build tools: {:.0} of {:.0} MB", what, mb(done + n), mb(total)))
        })?;
        if !ok {
            return Ok(false);
        }
        done += p.size;
    }
    if !py_ok {
        report(total, total, format!("Installing the {} build tools: Python...", what));
        unpack(&dl.join(python_pin.file), &platform, "python")?;
        let mut cmd = command(&py);
        cmd.args(["-I", "-B", "-m", "pip", "install", "-q", "--no-index", "--no-deps", "--no-compile", "--disable-pip-version-check", "--no-warn-script-location"]);
        cmd.args(wheels.iter().map(|w| dl.join(w.file)));
        let out = cmd.output().map_err(|e| format!("could not start {}: {}", py.display(), e))?;
        if !out.status.success() || !python_ready(&py) {
            return Err(format!("Installing lz4 and capstone into {} failed: {}", py.display(), String::from_utf8_lossy(&out.stderr).trim()));
        }
    }
    if !zig_ok {
        report(total, total, format!("Installing the {} build tools: Zig...", what));
        unpack(&dl.join(ZIG.file), &platform, "zig")?;
    }
    let _ = std::fs::remove_dir_all(&dl);
    Ok(true)
}

/// Unpacks a tarball with one top-level folder (python/, zig-x86_64-linux-0.17.0/) as `parent/name`, replacing an older one.
fn unpack(archive: &Path, parent: &Path, name: &str) -> Result<(), String> {
    let tmp = parent.join(format!(".unpack-{}", name));
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(&tmp).map_err(|e| format!("cannot create {}: {}", tmp.display(), e))?;
    let out = command("tar").arg("-xf").arg(archive).arg("-C").arg(&tmp).output().map_err(|e| format!("could not start tar: {}", e))?;
    if !out.status.success() {
        return Err(format!("Unpacking {} failed: {}", archive.display(), String::from_utf8_lossy(&out.stderr).trim()));
    }
    let top: Vec<PathBuf> = std::fs::read_dir(&tmp).map_err(|e| e.to_string())?.flatten().map(|e| e.path()).collect();
    let [only] = top.as_slice() else {
        return Err(format!("{} does not hold one folder", archive.display()));
    };
    let dst = parent.join(name);
    let _ = std::fs::remove_dir_all(&dst);
    std::fs::rename(only, &dst).map_err(|e| format!("cannot move {}: {}", dst.display(), e))?;
    let _ = std::fs::remove_dir_all(&tmp);
    Ok(())
}

/// The game loads its runtime library and the ASTC decoders from its own folder. Set up builds the game into the game folder,
/// which can be another folder than the release's: without these files Windows ends it at once with STATUS_DLL_NOT_FOUND,
/// shown as exit code 53 (#221). Copies what is missing or different from the launcher's folder; nothing when they are one.
pub fn sync_runtime_files(src: &Path, game_dir: &Path) -> Result<(), String> {
    let canon = |d: &Path| std::fs::canonicalize(d).unwrap_or_else(|_| d.to_path_buf());
    if canon(src) == canon(game_dir) {
        return Ok(());
    }
    let lib = runtime_lib_name();
    let decoders = has_decoder_dlls();
    let wanted = |n: &str| n == lib || (decoders && n.starts_with("astcdec-") && n.ends_with(".dll"));
    let Ok(rd) = std::fs::read_dir(src) else { return Ok(()) };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        if !wanted(&name) {
            continue;
        }
        let dst = game_dir.join(&name);
        let same = match (std::fs::metadata(e.path()), std::fs::metadata(&dst)) {
            (Ok(a), Ok(b)) => a.len() == b.len() && b.modified().ok() >= a.modified().ok(),
            _ => false,
        };
        if !same {
            std::fs::copy(e.path(), &dst).map_err(|err| format!("cannot copy {} into {}: {}", name, game_dir.display(), err))?;
        }
    }
    Ok(())
}

/// macOS: the files the game loads from its own folder that the package carries: the runtime library and MoltenVK. A launcher
/// update ships newer ones (a runtime fix, the pipeline cache) and they must reach the game without a Set up; the same rule as
/// pkp_rt.dll on Windows. Copies when the size or the content differs.
#[cfg(target_os = "macos")]
pub fn sync_macos_runtime(game_dir: &Path) -> Result<(), String> {
    use std::os::unix::fs::PermissionsExt;
    let sdk = crate::paths::exe_dir().join("tools").join("build").join("sdk");
    for name in ["libpkp_rt.dylib", "libvulkan.1.dylib"] {
        let src = sdk.join(name);
        let dst = game_dir.join(name);
        if !src.is_file() {
            continue;
        }
        let same = match (std::fs::read(&src), std::fs::read(&dst)) {
            (Ok(a), Ok(b)) => a == b,
            _ => false,
        };
        if same {
            continue;
        }
        let _ = std::fs::remove_file(&dst);   // the package's copy can be read only
        std::fs::copy(&src, &dst).map_err(|e| format!("cannot copy {} into {}: {}", name, game_dir.display(), e))?;
        let _ = std::fs::set_permissions(&dst, std::fs::Permissions::from_mode(0o755));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[ignore = "needs PKP_GAME_DIR; plays the opening headless for a minute and translates what it built"]
    fn native_warm_up_collects_the_opening() {
        let dir = std::path::PathBuf::from(std::env::var("PKP_GAME_DIR").expect("PKP_GAME_DIR"));
        let line = native_warm_up(&dir, &std::sync::atomic::AtomicBool::new(false)).expect("the warm-up run ends");
        println!("{:?}", line);
    }

    #[test]
    #[ignore = "needs PKP_GAME_DIR and PKP_SPIRV_HARNESS; translates the game folder's dumped blocks into its cache"]
    fn macos_translation_merges_the_dumped_blocks() {
        let dir = std::path::PathBuf::from(std::env::var("PKP_GAME_DIR").expect("PKP_GAME_DIR"));
        let line = native_translate_shaders(&dir).expect("the dumped blocks translate");
        println!("{:?}", line);
        assert!(!dir.join("shader_cache").join("missing").join("manifest.tsv").exists(), "the dumped corpus is cleared once translated");
    }

    #[test]
    #[ignore = "downloads about 27 MB from python-build-standalone and PyPI"]
    fn prepare_tools_fetches_a_working_python() {
        let dir = std::env::temp_dir().join(format!("pkp_tools_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let cancel = std::sync::atomic::AtomicBool::new(false);
        let mut last = String::new();
        let ok = prepare_tools(&dir, &cancel, &mut |_, _, m| last = m).unwrap();
        assert!(ok);
        assert!(python_ready(&python(&dir)), "the fetched Python cannot import lz4 and capstone: {last}");
        let _ = std::fs::remove_dir_all(&dir);
    }
    use std::fs;

    #[test]
    fn merge_keeps_the_rows_the_cache_had() {
        let base = std::env::temp_dir().join(format!("pkp_merge_{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        let scratch = base.join("scratch");
        let cache = base.join("cache");
        fs::create_dir_all(&scratch).unwrap();
        fs::create_dir_all(&cache).unwrap();
        fs::write(cache.join("results.tsv"), "hash\tgroup\nold\t.\n").unwrap();
        fs::write(cache.join("old.spv"), b"OLD").unwrap();
        // the scratch table repeats "old" (it was in the dumped corpus too): only the new hashes are merged
        fs::write(scratch.join("results.tsv"), "hash\tgroup\nold\t.\nnew\t.\nnewer\t.\n").unwrap();
        fs::write(scratch.join("new.spv"), b"NEW").unwrap();
        fs::write(scratch.join("newer.spv"), b"NEWER").unwrap();

        let n = merge_translation(&scratch, &cache).unwrap();

        assert_eq!(n, 2);
        let table = fs::read_to_string(cache.join("results.tsv")).unwrap();
        assert_eq!(table.lines().count(), 4, "header, old, new, newer: {}", table);
        assert_eq!(fs::read(cache.join("old.spv")).unwrap(), b"OLD", "an existing translation must not be touched");
        assert_eq!(fs::read(cache.join("new.spv")).unwrap(), b"NEW");
        let _ = fs::remove_dir_all(&base);
    }
}

#[cfg(all(test, target_os = "macos"))]
mod pressure_tests {
    use super::*;

    #[test]
    fn short_memory_follows_pressure_and_ratio() {
        // the real machine: these read live sysctls, so only sanity bounds are asserted
        if let Some(level) = memory_pressure_level() {
            assert!([1, 2, 4].contains(&level) || level >= 1, "level {level} outside the documented values");
        }
        if let Some(ratio) = free_memory_ratio() {
            assert!((0.0..=1.0).contains(&ratio), "ratio {ratio} outside 0..1");
        }
    }
}
