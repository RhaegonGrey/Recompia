// SPDX-License-Identifier: GPL-3.0-or-later
//! First-run game build (release model b): the release ships no game code, so pkp.exe is built here, on the player's PC,
//! from their own dump. The work is done by `tools/build/pkp_build.py` (bundled Python, lifter and Zig; see that file):
//! verify the ExeFS by hash, lift the four modules, compile them, link pkp.exe against pkp_rt.dll (the runtime).
//!
//! Resumable: the script keeps finished modules and objects in `<game>/build`; Pause writes `<game>/build/cancel`, which
//! stops it and its compilers. `<game>/build/done.json` plus pkp.exe means a build finished; it is this release's build
//! only if `pkp_build.py --check` says done.json matches the tools this release ships (lifter, main.sites, pkp_rt.lib, ...).
//! An update extracted over an install leaves the old pkp.exe and done.json behind: the check calls that stale, so Play
//! asks for Set up again and Set up rebuilds (the old pkp.exe stays until the new one is linked over it).
//! The stamp cannot see pkp_rt.dll. Before it, runtime_api.rs reads the runtime API number from pkp.exe, pkp_rt.dll and the
//! tools' header: an exe from another release than the dll is Stale however current its stamp, and a dll from another release
//! than the tools is `Built::Mixed` (an update with only some files put in place), which no build fixes.
//! On Linux the script runs with the Linux Python and Zig that host.rs fetches into `tools/linux`.
//! Its crate-local imports are host.rs (with download.rs) and runtime_api.rs, so `examples/first_run.rs` can build the files alone.

use std::fs;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

pub struct Tools {
    pub python: PathBuf,
    pub script: PathBuf,
}

/// Where setup writes its log lines (the launcher's setup.log); a closure so this file needs no crate-local import.
pub type LogFn = std::sync::Arc<dyn Fn(&str) + Send + Sync>;

pub struct Job {
    pub game_dir: PathBuf,
    pub exefs: PathBuf,
    pub jobs: usize,
    /// "Gentle build": the script and its compilers run at idle priority (the default is below normal).
    pub idle: bool,
    pub log: Option<LogFn>,
}

/// One progress line of the script: phase ("verify", "lift", "compile", "link", "finish"), done, total, text.
pub struct Tick {
    pub phase: String,
    pub done: u64,
    pub total: u64,
    pub message: String,
}

/// The package builds pkp.exe itself (model b) when it carries the build tools.
pub fn is_package_b(tools_dir: &Path) -> bool {
    // The release carries pkp_build.py: on Windows and Linux it links pkp.exe against pkp_rt.dll, on macOS pkp against
    // libpkp_rt.dylib. A development tree (no tools/ next to the launcher) builds the game with CMake instead.
    tools_dir.join("build").join("pkp_build.py").is_file()
}

/// What Play says when pkp.exe is from an older release (Built::Stale). The time is the compile's, as in the README.
pub const REBUILD_MESSAGE: &str = "This update changes the game code. Press Finish update to rebuild it (about 7 minutes). Your shaders and saves are kept.";
/// What Play says for Built::Relink: the game code is kept, only the link runs again.
pub const RELINK_MESSAGE: &str = "This update changes the runtime only. Press Finish update to link the game again (about a minute). Your shaders and saves are kept.";

/// How far the pkp.exe in a game folder is from this release.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Built {
    /// no finished build (no pkp.exe or no done.json)
    No,
    /// a finished build from other tools: an older release, or one whose tools could not be checked; or an exe built for another
    /// runtime API than the pkp_rt.dll beside the launcher (runtime_api.rs)
    Stale,
    /// Stale, but the module libraries kept in `<game>/build/lib` match this release (`--check` exit 4): Set up only links again
    Relink,
    /// built with this release's tools
    Current,
    /// pkp_rt.dll beside the launcher and the tools come from different releases (their runtime API numbers; None is a release
    /// before the number was readable): no build with these tools runs with this dll
    Mixed { dll: Option<u32>, sdk: Option<u32> },
}

/// What Play and Set up say for Built::Mixed: the player has to put the whole release in place.
pub fn mixed_message(dll: Option<u32>, sdk: Option<u32>) -> String {
    let api = |a: Option<u32>| a.map_or("an API before 2".to_string(), |n| format!("API {}", n));
    format!(
        "pkp_rt.dll next to the launcher is from another release than the build tools in the tools folder ({} and {}), so Set up would build a pkp.exe that does not run with it. Only part of an update is in place: extract the whole release zip over this folder, every file, then press Set up again.",
        api(dll),
        api(sdk)
    )
}

/// The mods' hook plan the launcher writes for Set up (mods.rs; Modtopia contract 2): the script builds a stop at each point and its
/// `--check` compares the plan's hash with the one it built from.
pub const MOD_PLAN: &str = "mods/.modtopia/hooks.tsv";
/// What Set up built from that plan: the same format, with the `# hash` of the plan it was built from.
pub const MOD_HOOKS_BUILT: &str = "build/mod-hooks-built.tsv";

/// What Play says when the build is stale because the mods' hook points changed (mod_plan_changed): only the game code is built again.
pub const MOD_REBUILD_MESSAGE: &str = "Your mods need new hook points: Set up again (a few minutes; shaders and saves are kept).";

/// The `# hash <hex>` line of a hooks file; "" when the file or the line is missing (no points).
fn plan_hash_of(path: &Path) -> String {
    let Ok(body) = fs::read_to_string(path) else {
        return String::new();
    };
    body.lines().find_map(|l| l.strip_prefix("# hash ")).map(|h| h.trim().to_string()).unwrap_or_default()
}

/// The mods' plan differs from the one pkp.exe was built with: a stale build is then (at least) the mods' doing.
pub fn mod_plan_changed(game_dir: &Path) -> bool {
    plan_hash_of(&game_dir.join(MOD_PLAN)) != plan_hash_of(&game_dir.join(MOD_HOOKS_BUILT))
}

/// The last check: (done.json path, its mtime and size, pkp.exe's mtime, exe and dll APIs differ, the mods' plan's mtime and size) ->
/// result. The tools do not change while the launcher runs, and a finished build rewrites done.json, so one python run per finished
/// build and plan is enough.
type CheckKey = (PathBuf, Option<std::time::SystemTime>, u64, Option<std::time::SystemTime>, bool, Option<(Option<std::time::SystemTime>, u64)>);
static CHECKED: std::sync::Mutex<Option<(CheckKey, Built)>> = std::sync::Mutex::new(None);

/// Whether pkp.exe was built by this release's tools (`pkp_build.py --check`, the stamp's only implementation).
pub fn built_state(game_dir: &Path, tools_dir: &Path) -> Built {
    // before the stamp: a dll from another release than the tools makes every build useless, whatever pkp.exe is.
    // macOS links the runtime into pkp (libpkp_rt.dylib) and has no pkp_rt.dll to compare, so the check does not apply.
    #[cfg(target_os = "macos")]
    let api = crate::runtime_api::Agreement::Agree;
    #[cfg(not(target_os = "macos"))]
    let api = crate::runtime_api::check(game_dir, tools_dir);
    if let crate::runtime_api::Agreement::Mixed { dll, sdk } = api {
        return Built::Mixed { dll, sdk };
    }
    let exe = crate::host::game_exe(game_dir);   // pkp on macOS, pkp.exe elsewhere
    let done = game_dir.join("build").join("done.json");
    let (Ok(em), Ok(dm)) = (fs::metadata(&exe), fs::metadata(&done)) else {
        return Built::No;
    };
    if !em.is_file() || !dm.is_file() {
        return Built::No;
    }
    // built for another API than the dll, however current its stamp: the link gives the dll's, so it is at least Stale
    let exe_stale = api == crate::runtime_api::Agreement::ExeStale;
    let plan = fs::metadata(game_dir.join(MOD_PLAN)).ok().map(|m| (m.modified().ok(), m.len()));
    let key: CheckKey = (done.clone(), dm.modified().ok(), dm.len(), em.modified().ok(), exe_stale, plan);
    if let Some((k, b)) = CHECKED.lock().unwrap().as_ref() {
        if *k == key {
            return *b;
        }
    }
    // missing tools: Stale, so Set up runs and names what is missing
    let Ok(tools) = find_tools(tools_dir, game_dir) else {
        return Built::Stale;
    };
    let mut cmd = crate::host::command(&tools.python);
    cmd.arg("-I").arg("-B").arg(&tools.script).arg("--check").arg("--game").arg(game_dir);
    quiet(&mut cmd, false);
    let Ok(out) = cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).status() else {
        return Built::Stale; // not cached: the next look tries again
    };
    // exit 0 current, 4 only the link is out of date, anything else (3) a full build
    let b = match out.code() {
        Some(4) => Built::Relink,
        Some(0) if !exe_stale => Built::Current,
        _ => Built::Stale,
    };
    *CHECKED.lock().unwrap() = Some((key, b));
    b
}

/// pkp.exe is in place and was built with this release's tools.
pub fn is_built(game_dir: &Path, tools_dir: &Path) -> bool {
    built_state(game_dir, tools_dir) == Built::Current
}

pub fn find_tools(tools_dir: &Path, game_dir: &Path) -> Result<Tools, String> {
    // The Python is fetched into the writable tools folder (the game folder on macOS, next to the AppImage on Linux); the
    // script, the lifter and the runtime library ship inside the package.
    let fetched = crate::host::python(&crate::host::tools_dir_for(game_dir));
    let python = if fetched.is_file() { fetched } else { crate::host::python(tools_dir) };
    let script = tools_dir.join("build").join("pkp_build.py");
    let mut required = vec![python.clone(), script.clone()];
    // macOS compiles with Apple clang and links libpkp_rt.dylib; the Windows and Linux packages carry Zig and the runtime as
    // pkp_rt.lib (Windows, linked in) or libpkp_rt.so (Linux, found next to the exe at run time).
    if !cfg!(target_os = "macos") {
        required.push(crate::host::zig(tools_dir));
    }
    required.extend([crate::host::runtime_lib_rel(), "build/recomp/nxrecomp/__main__.py"].iter().map(|p| tools_dir.join(p)));
    let mut missing = Vec::new();
    for p in required {
        if !p.is_file() {
            missing.push(p.display().to_string());
        }
    }
    if !missing.is_empty() {
        return Err(format!("The game build cannot run, missing: {}. Extract the release zip again so tools/ is complete.", missing.join("; ")));
    }
    Ok(Tools { python, script })
}

fn quiet(cmd: &mut Command, idle: bool) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, or IDLE_PRIORITY_CLASS when idle (the compilers it starts inherit it)
        cmd.creation_flags(0x0800_0000 | if idle { 0x0000_0040 } else { 0x0000_4000 });
    }
    #[cfg(not(windows))]
    crate::host::lower_priority(cmd, idle);
}

/// Runs the build. Ok(true) = pkp.exe built, Ok(false) = paused, Err = what went wrong, in the script's words.
pub fn run(job: &Job, tools: &Tools, cancel: &AtomicBool, mut report: impl FnMut(Tick)) -> Result<bool, String> {
    let build = job.game_dir.join("build");
    fs::create_dir_all(&build).map_err(|e| format!("cannot create {}: {}", build.display(), e))?;
    let cancel_file = build.join("cancel");
    let _ = fs::remove_file(&cancel_file);
    let mut cmd = crate::host::command(&tools.python);
    cmd.arg("-I").arg("-B").arg(&tools.script).arg("--exefs").arg(&job.exefs).arg("--game").arg(&job.game_dir);
    cmd.args(["--jobs", &job.jobs.max(1).to_string()]);
    quiet(&mut cmd, job.idle);
    let say = |t: &str| {
        if let Some(f) = &job.log {
            f(t)
        }
    };
    say(&format!("run: {} {}", tools.python.display(), cmd.get_args().map(|a| a.to_string_lossy().to_string()).collect::<Vec<_>>().join(" ")));
    let mut child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| format!("could not start the game build ({}): {}", tools.python.display(), e))?;
    let out = child.stdout.take().unwrap();
    let err = child.stderr.take().unwrap();
    let (tx, rx) = std::sync::mpsc::channel::<String>();
    std::thread::spawn(move || BufReader::new(out).lines().map_while(Result::ok).for_each(|l| drop(tx.send(l))));
    let elog = job.log.clone();
    let err_tail = std::thread::spawn(move || {
        let mut tail: Vec<String> = Vec::new();
        for l in BufReader::new(err).lines().map_while(Result::ok) {
            if let Some(f) = &elog {
                f(&format!("stderr: {}", l));
            }
            tail.push(l);
            if tail.len() > 12 {
                tail.remove(0);
            }
        }
        tail.join("\n")
    });
    let mut error: Option<String> = None;
    let mut paused = false;
    let mut asked = false;
    loop {
        if cancel.load(Ordering::SeqCst) && !asked {
            // the script stops its compilers and exits 2; nothing is killed from here
            let _ = fs::write(&cancel_file, "pause\n");
            asked = true;
        }
        match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(l) => {
                if let Some(rest) = l.strip_prefix("@progress ") {
                    let mut it = rest.splitn(4, ' ');
                    let phase = it.next().unwrap_or("").to_string();
                    let done = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
                    let total = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
                    let message = it.next().unwrap_or("").to_string();
                    report(Tick { phase, done, total, message });
                } else if let Some(rest) = l.strip_prefix("@error ") {
                    error = Some(rest.replace("\\n", "\n"));
                    say(&format!("error: {}", rest.replace("\\n", "\n")));
                } else if l.starts_with("@cancelled") {
                    paused = true;
                    say("the script reported: paused");
                } else {
                    say(&format!("out: {}", l));
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }
    let st = child.wait().map_err(|e| e.to_string())?;
    let _ = fs::remove_file(&cancel_file);
    if paused || (asked && st.code() == Some(2)) {
        say("game build paused by the player");
        return Ok(false);
    }
    if st.success() {
        say("game build finished");
        return Ok(true);
    }
    let tail = err_tail.join().unwrap_or_default();
    say(&format!("FAILED: game build ({}). Last error output:\n{}", st, tail));
    Err(error.unwrap_or_else(|| format!("The game build failed ({}). {}", st, tail.trim())))
}
