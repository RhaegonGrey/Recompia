// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's own options (launcher.json in the user's app config folder).

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;
use tauri::{AppHandle, Manager};

use crate::settings;

/// The launcher's own options, stored in the user's app config folder.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub struct Config {
    /// Root of the decrypted dump (the folder with base/ and updates/).
    pub dump_dir: String,
    /// Folder with pkp.exe. Empty: the folder the launcher is in.
    pub game_dir: String,
    pub minimize_on_play: bool,
    /// Show "Playing Recompia" on the player's Discord profile while the game runs (presence.rs). Off unless the player switches it on.
    pub discord_presence: bool,
    /// The first Play after online presence came asked once whether to switch it on (home.js); never asked again. The switch itself is
    /// pkp.ini [pkp] online_presence (online.rs), shared with the game's own Settings screen.
    pub online_asked: bool,
    /// The name online.rs sends; empty (the default): anonymous. The profile's nickname is never sent.
    pub online_name: String,
    /// Random, made the first time online presence starts; with the profile uid it makes the online ID (online.rs token_of). Only
    /// "Reset my online ID" changes it, by emptying it.
    pub online_salt: String,
    /// Set up with fewer cores at idle priority (preflight.rs jobs_for)
    pub gentle_build: bool,
    /// A new setup keeps its translated shaders as ~99,000 loose files (the old storage) instead of the shader archive. Only read when
    /// a setup starts with no translated shader yet (shaders.rs Job::compact). Loose storage is on its way out.
    pub loose_shaders: bool,
    /// Where the window was last time, on a desktop screen (logical pixels); none: the default size, centred
    pub window: Option<WindowPlace>,
    /// Which releases the launcher offers: "stable" (the default) or "testing" (prereleases built from the testing branch). update.rs Channel.
    pub channel: String,
    /// Once per launcher start, update the installed mods from the mod hub (mods_update.rs). Off unless the player switches it on.
    pub mods_auto_update: bool,
    /// The player unlocked the Community Achievements mod (mods_lock.rs): it may be switched off or removed, and the launcher no longer
    /// installs or switches it on by itself. Locked unless the player unlocks it.
    pub achievements_unlocked: bool,
    /// The player unlocked the mod's sharing setting (mods_lock.rs): it may be switched off. Locked unless the player unlocks it.
    pub achievements_sharing_unlocked: bool,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq)]
pub struct WindowPlace {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}

pub fn config_path(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_config_dir().map_err(|e| e.to_string())?;
    fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
    Ok(dir.join("launcher.json"))
}

/// The game folder the saved settings name (the launcher's own folder when none).
pub fn game_dir(app: &AppHandle) -> PathBuf {
    crate::paths::game_dir_of(&load_config(app))
}

pub fn load_config(app: &AppHandle) -> Config {
    config_path(app)
        .ok()
        .and_then(|p| fs::read_to_string(p).ok())
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

#[tauri::command]
pub fn get_config(app: AppHandle) -> Config {
    load_config(&app)
}

#[tauri::command]
pub fn save_config(app: AppHandle, config: Config) -> Result<(), String> {
    store_config(&app, &config)
}

/// Writes launcher.json (the page's save_config, and a command that changes one option of its own).
pub fn store_config(app: &AppHandle, config: &Config) -> Result<(), String> {
    let text = serde_json::to_string_pretty(config).map_err(|e| e.to_string())?;
    settings::write_atomic(&config_path(app)?, &text).map_err(|e| format!("cannot save launcher settings: {}", e))
}

/// The install's online salt, made and saved the first time it is needed. None: it could not be saved, so no online ID is used (an ID
/// that changed every run would split one player into many).
pub fn online_salt(app: &AppHandle) -> Option<String> {
    let mut cfg = load_config(app);
    if cfg.online_salt.is_empty() {
        let mut bytes = [0u8; 16];
        crate::relay::room_code::os_random(&mut bytes).ok()?;
        cfg.online_salt = bytes.iter().map(|b| format!("{:02x}", b)).collect();
        save_config(app.clone(), cfg.clone()).ok()?;
    }
    Some(cfg.online_salt)
}
