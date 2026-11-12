// SPDX-License-Identifier: GPL-3.0-or-later
//! Play and the game's state.

use tauri::AppHandle;

use crate::states::PlayArg;
use crate::config::load_config;
use crate::guard::{ensure_idle, Need};
use crate::paths::game_dir_of;
use crate::readiness::readiness;
use crate::{play, profiles, settings};

/// `without_mods`: this one start runs without mods (the runtime's [mods] off, passed in the game's environment only).
#[tauri::command]
pub fn play(app: AppHandle, without_mods: Option<bool>) -> Result<(), String> {
    ensure_idle(&app, &[Need::Update, Need::Storage])?;
    let cfg = load_config(&app);
    let probe = readiness(&cfg);
    if probe.readiness.kind != "ready" {
        return Err(probe.readiness.detail);
    }
    let dir = game_dir_of(&cfg);
    // macOS: Set up wrote pkp.ini from the player's dump and there is no dump choice to apply here, so readiness carries no
    // paths. Everywhere else they name the exefs and romfs to write.
    if let Some(paths) = probe.paths {
        settings::ensure_play_paths(&dir, &paths.exefs, &paths.romfs, &paths.romfs_base, &paths.aoc_dir)?;
    }
    profiles::apply_active(&dir)?;
    play::start(&app, &dir, &cfg, without_mods.unwrap_or(false))
}

#[tauri::command]
pub fn game_status(state: PlayArg) -> play::Status {
    state.0.lock().unwrap().clone()
}
