// SPDX-License-Identifier: GPL-3.0-or-later
//! Starts pkp.exe, watches it, and reports how it ended.

use serde::Serialize;
use std::path::Path;
use std::sync::Mutex;
use std::time::{Duration, SystemTime};
use tauri::{AppHandle, Emitter, Manager};

use crate::config::Config;
use crate::states::States;

/// Files the launcher may open for the user, by name, inside the game folder.
pub const REPORT_FILES: [&str; 4] = ["pkp_crash.txt", "pkp_freeze.txt", "pkp_fatal.txt", "pkp.log"];

#[derive(Serialize, Clone, Default)]
pub struct ReportFile {
    pub name: String,
    /// Written since this run started.
    pub fresh: bool,
}

/// What the game is doing. The page gets the lowercase word: "idle", "running" or "exited".
#[derive(Serialize, Clone, Copy, PartialEq, Eq, Debug)]
#[serde(rename_all = "lowercase")]
pub enum GameState {
    Idle,
    Running,
    Exited,
}

#[derive(Serialize, Clone)]
pub struct Status {
    pub state: GameState,
    pub code: Option<i32>,
    pub crashed: bool,
    pub files: Vec<ReportFile>,
    pub message: String,
}

impl Default for Status {
    fn default() -> Self {
        Status { state: GameState::Idle, code: None, crashed: false, files: vec![], message: String::new() }
    }
}

#[derive(Default)]
pub struct PlayState(pub Mutex<Status>);

fn set_status(app: &AppHandle, s: Status) {
    *app.play().0.lock().unwrap() = s.clone();
    let _ = app.emit("game-status", s);
}

fn report_files(dir: &Path, started: SystemTime) -> Vec<ReportFile> {
    REPORT_FILES
        .iter()
        .filter_map(|n| {
            let md = std::fs::metadata(dir.join(n)).ok()?;
            let fresh = md.modified().map(|m| m + Duration::from_secs(2) >= started).unwrap_or(false);
            Some(ReportFile { name: n.to_string(), fresh })
        })
        .collect()
}

pub fn start(app: &AppHandle, game_dir: &Path, cfg: &Config, without_mods: bool) -> Result<(), String> {
    let (minimize, discord) = (cfg.minimize_on_play, cfg.discord_presence);
    {
        let st = app.play();
        if st.0.lock().unwrap().state == GameState::Running {
            return Err("The game is already running.".into());
        }
    }
    let exe = crate::host::game_exe(game_dir);
    if !exe.is_file() {
        return Err(format!(
            "{} was not found in {}. Put the launcher next to it or set the game folder in Settings.",
            crate::host::game_exe_name(),
            crate::dump::fwd(game_dir)
        ));
    }
    // the in-game Mods tab shows the launcher's locks (pkp.ini [mods]); a pkp.ini that cannot be written does not stop the start
    if let Err(e) = crate::mods_lock::mirror(game_dir, crate::mods_lock::Locks::of(cfg)) {
        eprintln!("mods: the locks were not written to pkp.ini: {e}");
    }

    let app2 = app.clone();
    let dir = game_dir.to_path_buf();
    let tools = crate::paths::exe_dir().join("tools");
    let first = if crate::host::native_game() {
        "Starting the game..."
    } else if crate::shaders::needs_warm_up(game_dir) {
        "Collecting the shaders the game builds while it runs (once, about a minute)..."
    } else {
        "Checking the shaders the game builds while it runs..."
    };
    set_status(app, Status { state: GameState::Running, message: first.into(), ..Default::default() });
    std::thread::spawn(move || {
        // every start: the run-time shaders seen so far must all be translated (the warm-up run collects them the first
        // time, e.g. for a setup finished before it existed). A failure here is logged in the status, the game still starts.
        // macOS builds and translates its own shaders: the shader tools are Windows programs, so there is nothing to prepare
        let prep = if crate::host::native_game() {
            String::new()
        } else {
            match crate::shaders::prepare(&dir, &tools, &std::sync::atomic::AtomicBool::new(false)) {
                Ok(Some(n)) if n > 0 => format!(" {} shaders were translated before the start.", n),
                Err(e) => format!(" Shaders were not prepared: {}.", e),
                _ => String::new(),
            }
        };
        // Calculate after shader preparation, immediately before the process starts, so a long preparation cannot age the offset.
        let mut cmd = match crate::events::override_for(&dir).and_then(|clock| game_command(&exe, &dir, clock, without_mods)) {
            Ok(c) => c,
            Err(e) => {
                set_status(&app2, Status { state: GameState::Exited, crashed: true, message: format!("Could not start pkp.exe: {}", e), ..Default::default() });
                return;
            }
        };
        let started = SystemTime::now();
        let mut child = match cmd.spawn() {
            Ok(c) => c,
            Err(e) => {
                set_status(&app2, Status { state: GameState::Exited, crashed: true, message: format!("Could not start pkp.exe: {}", e), ..Default::default() });
                return;
            }
        };
        set_status(&app2, Status { state: GameState::Running, message: format!("pkp.exe started (process {})", child.id()), ..Default::default() });
        if minimize {
            if let Some(w) = app2.get_webview_window("main") {
                let _ = w.minimize();
            }
        }
        // the player's save backups: a timer while the game runs (when switched on), one more copy once it has ended
        let backups = crate::savebackup::Watch::start(&dir);
        let presence = crate::presence::Watch::start(discord);
        let online = crate::online::Watch::start(&app2, &dir);
        let result = wait_with_memory_guard(&mut child);
        online.finish();
        presence.finish();
        backups.finish();
        let (code, wait_err) = match result {
            Ok(s) => (s.code(), None),
            Err(e) => (None, Some(e.to_string())),
        };
        // shaders the game built while it ran: translated now, so the next run has them (the state stays "running" meanwhile)
        set_status(&app2, Status { state: GameState::Running, message: "Translating the shaders the game built while it ran...".into(), ..Default::default() });
        let shaders = if crate::host::native_game() {
            match crate::host::native_translate_shaders(&dir) {
                Ok(Some(line)) => format!(" {}.", line),
                Ok(None) => String::new(),
                Err(e) => format!(" New shaders were not translated: {}.", e),
            }
        } else {
            match crate::shaders::translate_missing(&dir, &tools) {
                Ok(0) => String::new(),
                Ok(n) => format!(" {} new shaders translated.", n),
                Err(e) => format!(" New shaders were not translated: {}.", e),
            }
        };
        let files = report_files(&dir, started);
        let crash_file = files.iter().any(|f| f.fresh && f.name != "pkp.log");
        let crashed = crash_file || (code != Some(0));
        let message = match (&wait_err, code) {
            (Some(e), _) => format!("Lost track of pkp.exe: {}", e),
            (None, Some(0)) => "pkp.exe exited normally.".to_string(),
            (None, Some(c)) => format!("pkp.exe exited with code {} (0x{:08X}).", c, c as u32),
            (None, None) => "pkp.exe was ended by the system.".to_string(),
        };
        set_status(&app2, Status { state: GameState::Exited, code, crashed, files, message: message + &prep + &shaders });
        if minimize {
            if let Some(w) = app2.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.set_focus();
            }
        }
    });
    Ok(())
}

/// How Play starts the game: `pkp` in its folder on macOS and Linux, `pkp.exe` on Windows (host.rs). Every path (exefs, romfs, shader
/// cache) comes from pkp.ini next to it. `without_mods`: this start runs without mods.
fn game_command(exe: &Path, dir: &Path, clock: Option<crate::events::ClockOverride>, without_mods: bool) -> Result<std::process::Command, String> {
    if !crate::host::native_game() {
        crate::host::sync_runtime_files(&crate::paths::exe_dir(), dir)?;   // pkp_rt.dll and the ASTC decoders: Windows files
    }
    #[cfg(target_os = "macos")]
    crate::host::sync_macos_runtime(dir)?;   // libpkp_rt.dylib and MoltenVK: a launcher update brings newer ones
    let mut cmd = std::process::Command::new(exe);
    cmd.current_dir(dir).args(["--log", "pkp.log"]);
    // macOS: the folder is not the release's installed layout (no exefs/main next to the binary), so name the settings file;
    // the game reads exefs and romfs from it.
    if crate::host::native_game() && dir.join(crate::settings::PKP_INI).is_file() {
        cmd.args(["--ini", crate::settings::PKP_INI]);
    }
    if crate::host::native_game() {
        // The game writes its log to pkp.log and tees it to stdout. Windows spawns it with CREATE_NO_WINDOW and a GUI-subsystem
        // exe, so nothing shows; on macOS it would inherit the launcher's terminal (or the .app's stderr). Nothing is lost: the
        // log file and the crash/freeze reports are next to the game.
        cmd.stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null());
    }
    if let Some(clock) = clock {
        clock.apply_to(&mut cmd);
    }
    if without_mods {
        leave_mods_out(&mut cmd);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no stray console if pkp.exe is a console build
    }
    Ok(cmd)
}

/// One start without mods: the runtime's `[mods] off` through its environment override (runtime/docs/CONFIG.md), so pkp.ini and
/// the mods folder are not touched and the next start has its mods again.
fn leave_mods_out(cmd: &mut std::process::Command) {
    cmd.env("NXRT_MODS_OFF", "1");
}

/// `--play`: what Play does, without the window, for a shortcut that starts the game directly (Steam, Sunshine). Checks
/// that setup finished, prepares the run-time shaders, runs pkp.exe until it exits, then translates the shaders it built.
/// Returns the game's exit code (1 when it could not start).
pub fn run_cli(game_dir: &Path, tools: &Path) -> i32 {
    let fail = |m: String| {
        eprintln!("{}", m);
        1
    };
    let exe = crate::host::game_exe(game_dir);
    if !exe.is_file() || (!crate::host::native_game() && crate::gamebuild::is_package_b(tools) && !crate::gamebuild::is_built(game_dir, tools)) {
        return fail(format!(
            "{} in {} is missing or from an older release: open the launcher and press Set up.",
            crate::host::game_exe_name(),
            crate::dump::fwd(game_dir)
        ));
    }
    if !game_dir.join(crate::settings::PKP_INI).is_file() || (!crate::host::native_game() && !crate::shaders::is_complete(game_dir)) {
        return fail("Setup has not finished: open the launcher and press Set up.".into());
    }
    if !crate::host::native_game() {
        match crate::shaders::prepare(game_dir, tools, &std::sync::atomic::AtomicBool::new(false)) {
            Ok(Some(n)) if n > 0 => println!("{} shaders were translated before the start.", n),
            Err(e) => eprintln!("Shaders were not prepared: {}.", e),
            _ => {}
        }
    }
    // the same rule as Play in the window: without the active profile in pkp.ini the game could start on another player's account and save
    if let Err(e) = crate::profiles::apply_active(game_dir) {
        return fail(format!("The profile could not be applied, so the game was not started: {}", e));
    }
    let clock = match crate::events::override_for(game_dir) {
        Ok(clock) => clock,
        Err(e) => return fail(e),
    };
    let backups = crate::savebackup::Watch::start(game_dir);   // the same save backups as Play in the window
    let status = game_command(&exe, game_dir, clock, false).and_then(|mut c| {
        let mut child = c.spawn().map_err(|e| format!("Could not start {}: {}", crate::host::game_exe_name(), e))?;
        wait_with_memory_guard(&mut child).map_err(|e| format!("Lost track of {}: {}", crate::host::game_exe_name(), e))
    });
    backups.finish();
    let code = match status {
        Ok(st) => st.code().unwrap_or(1),
        Err(e) => return fail(e),
    };
    if crate::host::native_game() {
        match crate::host::native_translate_shaders(game_dir) {
            Ok(Some(line)) => println!("shaders: {}", line),
            Ok(None) => {}
            Err(e) => eprintln!("shaders were not translated: {}", e),
        }
    } else {
        match crate::shaders::translate_missing(game_dir, tools) {
            Ok(n) if n > 0 => println!("{} new shaders translated.", n),
            Err(e) => eprintln!("New shaders were not translated: {}.", e),
            _ => {}
        }
    }
    code
}

/// Waits for the game, stopping it if the machine runs out of memory. macOS: the world load needs ~9 GB on a 16 GB host, and
/// the kernel's critical pressure level is the last warning before the machine stops responding (host.rs memory_is_short).
fn wait_with_memory_guard(child: &mut std::process::Child) -> std::io::Result<std::process::ExitStatus> {
    if !crate::host::native_game() {
        return child.wait();
    }
    loop {
        if let Some(s) = child.try_wait()? {
            return Ok(s);
        }
        if crate::host::memory_is_short() {
            eprintln!("the system is out of memory: stopping the game");
            let _ = child.kill();
            return child.wait();
        }
        std::thread::sleep(Duration::from_secs(2));
    }
}

pub fn open_with_shell(path: &Path) -> Result<(), String> {
    // explorer (Linux: xdg-open) opens a file in its default app and a folder in a window
    crate::host::open(path.as_os_str())
}

/// Opens an http(s) link in the default browser (same route as open_with_shell).
pub fn open_url(url: &str) -> Result<(), String> {
    crate::host::open(std::ffi::OsStr::new(url))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn without_mods_sets_the_runtime_switch() {
        let mut cmd = std::process::Command::new("pkp-test");
        leave_mods_out(&mut cmd);
        let off = cmd.get_envs().find(|(k, _)| *k == std::ffi::OsStr::new("NXRT_MODS_OFF")).and_then(|(_, v)| v);
        assert_eq!(off, Some(std::ffi::OsStr::new("1")));
    }
}
