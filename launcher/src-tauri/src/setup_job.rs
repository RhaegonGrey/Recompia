// SPDX-License-Identifier: GPL-3.0-or-later
//! First-run setup: the build and shader job on a worker thread, its progress and the preflight checks.

use serde::Serialize;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tauri::Emitter;
use tauri::AppHandle;

use crate::states::SetupArg;
use crate::config::{game_dir, load_config};
use crate::guard::{ensure_idle, Need};
use crate::paths::{exe_dir, game_dir_of};
use crate::readiness::setup_started;
use crate::shaders::RunState;
use crate::{dump, gamebuild, host, mods, play, preflight, settings, setuplog, shaders, writecheck};

/// Shader setup: the running job's progress and its cancel flag.
#[derive(Default)]
pub struct SetupState {
    pub progress: Mutex<shaders::Progress>,
    pub cancel: AtomicBool,
    /// Cancel flag of a running .nup unpack.
    pub nup_cancel: AtomicBool,
}

impl SetupState {
    pub fn running(&self) -> bool {
        self.progress.lock().unwrap().state == RunState::Running
    }
}

#[derive(Serialize)]
pub struct SetupStatus {
    pub progress: shaders::Progress,
    /// setup.json says finished and the cache is there
    pub complete: bool,
    /// an earlier setup started (Set up continues it)
    pub started: bool,
    /// pkp.exe is from an older release whose game code this one changes: Set up rebuilds it
    pub rebuild: bool,
}

#[tauri::command]
pub fn setup_status(app: AppHandle, state: SetupArg) -> SetupStatus {
    let dir = game_dir(&app);
    let progress = state.progress.lock().unwrap().clone();
    let tools_dir = exe_dir().join("tools");
    let state = if gamebuild::is_package_b(&tools_dir) { gamebuild::built_state(&dir, &tools_dir) } else { gamebuild::Built::Current };
    let started = setup_started(&dir);
    SetupStatus {
        progress,
        complete: state == gamebuild::Built::Current && shaders::is_complete(&dir),
        started,
        rebuild: matches!(state, gamebuild::Built::Stale | gamebuild::Built::Relink),
    }
}

/// First-run setup: (a) the pkp.ini paths, (b) in a model (b) release, pkp.exe built from the player's dump
/// (gamebuild.rs), (c) the shader cache in shader_cache/spirv, on a worker thread with "setup-status" events.
#[tauri::command]
pub fn start_setup(app: AppHandle, state: SetupArg, gentle: Option<bool>) -> Result<(), String> {
    ensure_idle(&app, &[Need::Update, Need::Storage])?;
    let cfg = load_config(&app);
    // Linux and macOS: the player's dump is the source of everything. The same flow as the Windows Set up, from
    // host.rs native_set_up_install: pkp.ini from the chosen dump, the tools, the game build, then the shader corpus.
    if host::native_game() {
        let dir = game_dir_of(&cfg);
        fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
        writecheck::check(&dir).map_err(|e| e.message())?;
        {
            let mut p = state.progress.lock().unwrap();
            if p.state == RunState::Running {
                return Err("Setup is already running.".into());
            }
            *p = shaders::Progress { state: RunState::Running, phase: "check".into(), message: "Checking the setup tools...".into(), ..Default::default() };
        }
        let st = state.inner().clone();
        st.cancel.store(false, Ordering::SeqCst);
        let slog = Arc::new(setuplog::SetupLog::open(&dir));
        let log: shaders::LogFn = {
            let l = slog.clone();
            Arc::new(move |t: &str| l.line(t))
        };
        log(&format!("=== Set up started: launcher {}, {}", env!("CARGO_PKG_VERSION"), host::os_name()));
        std::thread::spawn(move || {
            let mut last = (String::new(), 0u64, String::new());
            let mut emit = |p: shaders::Progress| {
                if setuplog::progress_worth_logging(&mut last, &p.phase, p.done, p.total, &p.message) && p.state == RunState::Running {
                    log(&format!("[{}] {}", p.phase, p.message));
                }
                *st.progress.lock().unwrap() = p.clone();
                let _ = app.emit("setup-status", p);
            };
            let result = host::native_set_up_install(&cfg, &st.cancel, &mut |phase, done, total, message| {
                emit(shaders::Progress { state: RunState::Running, phase: phase.to_string(), done, total, message: message.to_string() })
            });
            let end = match result {
                Ok(n) => shaders::Progress { state: RunState::Done, phase: "finish".into(), message: format!("Setup finished: {} shaders translated.", n), ..Default::default() },
                Err(e) => shaders::Progress { state: RunState::Error, message: e, ..Default::default() },
            };
            slog.line(&format!("=== Set up ended: {}{}", end.state, if end.message.is_empty() { String::new() } else { format!(": {}", end.message) }));
            emit(end);
        });
        return Ok(());
    }
    let gentle = gentle.unwrap_or(cfg.gentle_build);
    let report = dump::scan(&cfg.dump_dir);
    let chosen = report.chosen.ok_or_else(|| "Choose usable game files first.".to_string())?;
    let dir = game_dir_of(&cfg);
    let tools_dir = exe_dir().join("tools");
    let package_b = gamebuild::is_package_b(&tools_dir);
    if !package_b && !host::game_exe(&dir).is_file() {
        return Err(format!("{} was not found in {}. Put the launcher next to {} or set the game folder in Settings.", host::game_exe_name(), dump::fwd(&dir), host::game_exe_name()));
    }
    // a pkp_rt.dll from another release than the tools: the build would finish and the game still refuse to start
    if package_b {
        if let gamebuild::Built::Mixed { dll, sdk } = gamebuild::built_state(&dir, &tools_dir) {
            return Err(gamebuild::mixed_message(dll, sdk));
        }
    }
    // the build, the shader tools and pkp.ini all write in the game folder: a blocked one fails here, in words (writecheck.rs)
    writecheck::check(&dir).map_err(|e| e.message())?;
    // the build puts in the hook points of the mods as they are now (Modtopia's hooks.tsv)
    if package_b {
        mods::refresh_plan(&dir)?;
    }
    {
        let mut p = state.progress.lock().unwrap();
        if p.state == RunState::Running {
            return Err("Setup is already running.".into());
        }
        *p = shaders::Progress { state: RunState::Running, phase: "check".into(), message: "Checking the setup tools...".into(), ..Default::default() };
    }
    settings::ensure_play_paths(&dir, &chosen.exefs, &chosen.romfs, &chosen.romfs_base, &report.aoc_dir)?;
    let st = state.inner().clone();
    st.cancel.store(false, Ordering::SeqCst);
    let cpus = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(3);
    let (jobs, build_jobs) = preflight::jobs_for(cpus, gentle);
    let slog = Arc::new(setuplog::SetupLog::open(&dir));
    let log: shaders::LogFn = {
        let l = slog.clone();
        Arc::new(move |t: &str| l.line(t))
    };
    log(&format!(
        "=== Set up started: launcher {}, {} threads, {} jobs{}",
        env!("CARGO_PKG_VERSION"),
        cpus,
        jobs,
        if gentle { ", gentle (idle priority)" } else { "" }
    ));
    log(&format!("game files: {}", dump::fwd(Path::new(&chosen.exefs))));
    let build_job = gamebuild::Job { game_dir: dir.clone(), exefs: PathBuf::from(&chosen.exefs), jobs: build_jobs, idle: gentle, log: Some(log.clone()) };
    let romfs_base = (!chosen.romfs_base.is_empty()).then(|| PathBuf::from(&chosen.romfs_base));
    let job = shaders::Job { game_dir: dir, romfs: PathBuf::from(&chosen.romfs), romfs_base, main: PathBuf::from(&chosen.exefs).join("main"), jobs, idle: gentle, log: Some(log.clone()), limit: 0, compact: !cfg.loose_shaders };
    std::thread::spawn(move || {
        let mut last = (String::new(), 0u64, String::new());
        let mut emit = |p: shaders::Progress| {
            if setuplog::progress_worth_logging(&mut last, &p.phase, p.done, p.total, &p.message) && p.state == RunState::Running {
                log(&format!("[{}] {}", p.phase, p.message));
            }
            *st.progress.lock().unwrap() = p.clone();
            let _ = app.emit("setup-status", p);
        };
        let result = (|| {
            // no build yet, or one from an older release (stale tools stamp): build pkp.exe (again)
            if package_b && !gamebuild::is_built(&build_job.game_dir, &tools_dir) {
                let tools = gamebuild::find_tools(&tools_dir, &build_job.game_dir)?;
                let built = gamebuild::run(&build_job, &tools, &st.cancel, |t| {
                    emit(shaders::Progress { state: RunState::Running, phase: t.phase, done: t.done, total: t.total, message: t.message })
                })?;
                if !built {
                    return Ok(false);
                }
            }
            let tools = shaders::find_tools(&tools_dir)?;
            // a finished cache of the same romfs has nothing left to translate (re-extracting would only find that out)
            let same = shaders::read_marker(&job.game_dir).romfs == job.romfs_key();
            let done = same && shaders::is_complete(&job.game_dir);
            if !done && !shaders::run(&job, &tools, &st.cancel, &mut emit)? {
                return Ok(false);
            }
            // the shaders the game builds while it runs: one hidden run collects those of the opening; later ones are
            // collected by every normal run, and every Play checks them all (play.rs)
            emit(shaders::Progress {
                state: RunState::Running,
                phase: "warmup".into(),
                message: format!("Running the game once in the background to collect the shaders it builds while it runs (about {} s)...", shaders::WARMUP_SECONDS),
                ..Default::default()
            });
            Ok(shaders::prepare(&job.game_dir, &tools_dir, &st.cancel)?.is_some())
        })();
        let end = match result {
            Ok(true) => shaders::Progress { state: RunState::Done, phase: "finish".into(), message: "Setup finished.".into(), ..Default::default() },
            Ok(false) => shaders::Progress { state: RunState::Cancelled, message: "Setup paused. Set up again continues where it stopped.".into(), ..Default::default() },
            Err(e) => shaders::Progress { state: RunState::Error, message: e, ..Default::default() },
        };
        slog.line(&format!("=== Set up ended: {}{}", end.state, if end.message.is_empty() { String::new() } else { format!(": {}", end.message) }));
        emit(end);
    });
    Ok(())
}

/// The checks shown before Set up starts (preflight.rs). Reads the disk and loads Vulkan, so it runs off the UI thread.
#[tauri::command]
pub async fn preflight(app: AppHandle) -> Result<preflight::Report, String> {
    let cfg = load_config(&app);
    let dir = game_dir_of(&cfg);
    let compact = !cfg.loose_shaders;
    let tools_dir = exe_dir().join("tools");
    tauri::async_runtime::spawn_blocking(move || {
        let build_pending = gamebuild::is_package_b(&tools_dir) && !gamebuild::is_built(&dir, &tools_dir);
        let facts = preflight::probe(&dir, &preflight::remaining_of(&dir, build_pending, compact));
        let report = preflight::evaluate(&facts);
        // into the setup log too: a failed setup report then shows what the PC looked like
        let log = setuplog::SetupLog::open(&dir);
        for c in &report.checks {
            log.line(&format!("preflight {:?}: {}: {}", c.level, c.title, c.detail));
        }
        report
    })
    .await
    .map_err(|e| e.to_string())
}

/// Opens setup.log (next to pkp.exe) in the default editor.
#[tauri::command]
pub fn open_setup_log(app: AppHandle) -> Result<(), String> {
    let path = setuplog::path_of(&game_dir(&app));
    if !path.is_file() {
        return Err("There is no setup log yet: it is written when Set up starts.".into());
    }
    play::open_with_shell(&path)
}

#[tauri::command]
pub fn cancel_setup(state: SetupArg) {
    state.cancel.store(true, Ordering::SeqCst);
}
