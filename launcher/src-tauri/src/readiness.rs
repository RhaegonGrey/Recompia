// SPDX-License-Identifier: GPL-3.0-or-later
//! Whether Play can run and what the page's main button does about it.

use serde::Serialize;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use tauri::AppHandle;

use crate::config::{load_config, Config};
use crate::paths::{exe_dir, game_dir_of};
use crate::{dump, gamebuild, mods, shaders};

/// This run was started by a self-update (--updated). launcher_state tells the page once.
pub static UPDATED: AtomicBool = AtomicBool::new(false);

#[derive(Serialize)]
pub struct Overview {
    pub game_dir: String,
    pub default_game_dir: String,
    pub exe_exists: bool,
    /// this release builds pkp.exe itself (Set up), so a missing one only means setup has not run yet
    pub builds_exe: bool,
}

pub fn overview_of(cfg: &Config) -> Overview {
    let dir = game_dir_of(cfg);
    Overview {
        game_dir: dump::fwd(&dir),
        // the folder a fresh install starts from, not the launcher's own: inside a .app that is the bundle's Contents, and
        // the settings page offered it as the Install folder (a save then wrote it back, and Set up and Play worked in it)
        default_game_dir: dump::fwd(&crate::paths::default_game_dir()),
        exe_exists: crate::host::game_exe(&dir).is_file(),
        builds_exe: gamebuild::is_package_b(&exe_dir().join("tools")),
    }
}

#[tauri::command]
pub fn get_overview(app: AppHandle) -> Overview {
    overview_of(&load_config(&app))
}

/// Whether Play can run, and if not, what the main button does about it. `kind`: no_files, bad_files, no_exe, no_runtime,
/// mixed_release, setup, resume, rebuild, mods, retranslate, rebase, ready. `detail`: the sentence for the player (empty when ready).
#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Readiness {
    pub kind: &'static str,
    pub detail: String,
}

impl Readiness {
    fn of(kind: &'static str, detail: impl Into<String>) -> Readiness {
        Readiness { kind, detail: detail.into() }
    }
}

/// The game folder's state, once usable game files are chosen.
pub struct ReadyFacts {
    pub game_dir: String,
    pub exe_exists: bool,
    /// pkp.exe is there but pkp_rt.dll is neither beside it nor in the launcher's folder to copy from: it would stop at once (exit 53)
    pub runtime_missing: bool,
    /// the release builds pkp.exe itself
    pub builds_exe: bool,
    /// Current when the release does not build it
    pub built: gamebuild::Built,
    /// the mods' hook plan differs from the one pkp.exe was built with (gamebuild::mod_plan_changed)
    pub mod_plan_changed: bool,
    pub needs_retranslation: bool,
    pub shaders_complete: bool,
    /// an earlier setup started
    pub started: bool,
    /// the base romfs now layered under the update is not the one the shaders were made from (#128)
    pub base_changed: bool,
}

pub fn readiness_of(f: &ReadyFacts) -> Readiness {
    if !f.builds_exe && !f.exe_exists {
        return Readiness::of("no_exe", format!("{} was not found in {}. Put the launcher next to {}.", crate::host::game_exe_name(), f.game_dir, crate::host::game_exe_name()));
    }
    if f.exe_exists && f.runtime_missing {
        return Readiness::of("no_runtime", format!("{} was not found next to {} in {} nor next to the launcher. Extract the release again so the launcher's folder has it, or set the game folder in Settings.", crate::host::runtime_lib_name(), crate::host::game_exe_name(), f.game_dir));
    }
    let unfinished = || {
        if f.started {
            Readiness::of("resume", "Setup has not finished. Press Continue setup: it picks up where it stopped.")
        } else {
            Readiness::of("setup", "Press Set up to build the game and prepare its shaders.")
        }
    };
    match f.built {
        gamebuild::Built::Current => {}
        // the same Set up as after an update, said in the mods' words when they are what changed
        gamebuild::Built::Stale if f.mod_plan_changed => return Readiness::of("mods", gamebuild::MOD_REBUILD_MESSAGE),
        gamebuild::Built::Stale => return Readiness::of("rebuild", gamebuild::REBUILD_MESSAGE),
        gamebuild::Built::Relink => return Readiness::of("rebuild", gamebuild::RELINK_MESSAGE),
        gamebuild::Built::Mixed { dll, sdk } => return Readiness::of("mixed_release", gamebuild::mixed_message(dll, sdk)),
        gamebuild::Built::No => return unfinished(),
    }
    if f.needs_retranslation {
        return Readiness::of("retranslate", "This version fixes how some shaders are translated. Press Finish update: it translates only those shaders again.");
    }
    if !f.shaders_complete {
        return unfinished();
    }
    if f.base_changed {
        return Readiness::of("rebase", "Your update holds only the files it changes, so the launcher now adds the missing ones from the base game. Press Finish update so their shaders are prepared too.");
    }
    Readiness::of("ready", "")
}

/// What Play writes into pkp.ini.
pub struct GamePaths {
    pub exefs: String,
    pub romfs: String,
    pub romfs_base: String,
    pub aoc_dir: String,
}

#[derive(Serialize, Clone)]
pub struct DumpChoice {
    pub path: String,
    /// "base", "update", "folder" or "nup"
    pub kind: String,
    /// the update's version folder; empty for the rest
    pub version: String,
}

pub struct Probe {
    pub readiness: Readiness,
    pub paths: Option<GamePaths>,
    pub dump: Option<DumpChoice>,
}

/// A setup has started in this game folder (Set up continues it).
pub fn setup_started(dir: &Path) -> bool {
    !shaders::read_marker(dir).romfs.is_empty() || dir.join("build").join("state.json").is_file()
}

/// The one answer to "can Play run": the Play command and the page's main button both come here, so they cannot disagree.
pub fn readiness(cfg: &Config) -> Probe {
    // macOS: the game is a native build and reads its own pkp.ini. When the package carries the builder, pkp does not exist
    // until Set up has built it from the player's dump, so a missing pkp is "press Set up", not "no executable".
    if crate::host::native_game() {
        let dir = game_dir_of(cfg);
        let refuse = |r: Readiness| Probe { readiness: r, paths: None, dump: None };
        let tools = crate::paths::exe_dir().join("tools");
        let package_b = crate::gamebuild::is_package_b(&tools);
        let exe = crate::host::game_exe(&dir);
        if package_b {
            if cfg.dump_dir.trim().is_empty() {
                return refuse(Readiness::of("no_files", "Choose your game files first."));
            }
            if !exe.is_file() || !crate::gamebuild::is_built(&dir, &tools) {
                return refuse(Readiness::of("setup", "Press Set up to build the game from your files and prepare its shaders."));
            }
        } else if !exe.is_file() {
            return refuse(Readiness::of(
                "no_exe",
                format!("{} was not found in {}. Put it there or set the game folder in Settings.", crate::host::game_exe_name(), dump::fwd(&dir)),
            ));
        }
        if !dir.join(crate::settings::PKP_INI).is_file() {
            return refuse(Readiness::of("no_files", format!("pkp.ini was not found in {}. It needs [fs] exefs and [fs] romfs.", dump::fwd(&dir))));
        }
        if !crate::shaders::is_complete(&dir) {
            return refuse(Readiness::of("setup", "Press Set up to extract and translate the game's shaders."));
        }
        return Probe { readiness: Readiness::of("ready", ""), paths: None, dump: None };
    }
    let refuse = |r: Readiness| Probe { readiness: r, paths: None, dump: None };
    let picked = cfg.dump_dir.trim();
    if picked.is_empty() {
        return refuse(Readiness::of("no_files", "Choose your game files first."));
    }
    // a .nup package is played as it is: pkp reads exefs and romfs straight from it
    let (paths, dump) = if picked.ends_with(".nup") && Path::new(picked).is_file() {
        let dump = DumpChoice { path: dump::fwd(Path::new(picked)), kind: "nup".into(), version: String::new() };
        (GamePaths { exefs: picked.to_string(), romfs: picked.to_string(), romfs_base: String::new(), aoc_dir: String::new() }, dump)
    } else {
        let report = dump::scan(&cfg.dump_dir);
        let Some(chosen) = report.chosen else {
            return refuse(Readiness::of("bad_files", format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" "))));
        };
        let dump = DumpChoice { path: chosen.path, kind: chosen.kind, version: chosen.version };
        (GamePaths { exefs: chosen.exefs, romfs: chosen.romfs, romfs_base: chosen.romfs_base, aoc_dir: report.aoc_dir }, dump)
    };
    let dir = game_dir_of(cfg);
    let tools_dir = exe_dir().join("tools");
    let builds_exe = gamebuild::is_package_b(&tools_dir);
    // the mods' hook plan is part of what the build is checked against: written from the mods folder as it is now
    if builds_exe {
        if let Err(e) = mods::refresh_plan(&dir) {
            eprintln!("mods plan: {e}");
        }
    }
    // the base game's files now fill in an update that lacks them (#128): its shaders were never extracted, so Set up runs once more
    let base_changed = !paths.romfs_base.is_empty() && {
        let job = shaders::Job { game_dir: dir.clone(), romfs: PathBuf::from(&paths.romfs), romfs_base: Some(PathBuf::from(&paths.romfs_base)), main: PathBuf::new(), jobs: 1, idle: false, log: None, limit: 0, compact: false };
        shaders::read_marker(&dir).romfs != job.romfs_key()
    };
    let readiness = readiness_of(&ReadyFacts {
        game_dir: dump::fwd(&dir),
        exe_exists: crate::host::game_exe(&dir).is_file(),
        runtime_missing: !crate::host::runtime_lib(&dir).is_file() && !crate::host::runtime_lib(&exe_dir()).is_file(),
        builds_exe,
        built: if builds_exe { gamebuild::built_state(&dir, &tools_dir) } else { gamebuild::Built::Current },
        mod_plan_changed: gamebuild::mod_plan_changed(&dir),
        needs_retranslation: shaders::needs_retranslation(&dir),
        shaders_complete: shaders::is_complete(&dir),
        started: setup_started(&dir),
        base_changed,
    });
    Probe { readiness, paths: Some(paths), dump: Some(dump) }
}

#[derive(Serialize)]
pub struct LauncherState {
    pub readiness: Readiness,
    #[serde(flatten)]
    pub overview: Overview,
    pub dump: Option<DumpChoice>,
    pub version: String,
    /// this is the first look after a self-update restart
    pub updated: bool,
}

/// What the page's main button is built from. The same checks as Play, off the UI thread: they read the disk and can start Python.
#[tauri::command]
pub async fn launcher_state(app: AppHandle) -> Result<LauncherState, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let cfg = load_config(&app);
        let probe = readiness(&cfg);
        LauncherState { readiness: probe.readiness, overview: overview_of(&cfg), dump: probe.dump, version: app.package_info().version.to_string(), updated: UPDATED.swap(false, Ordering::SeqCst) }
    })
    .await
    .map_err(|e| e.to_string())
}
