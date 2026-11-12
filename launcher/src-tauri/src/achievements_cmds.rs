// SPDX-License-Identifier: GPL-3.0-or-later
//! The Achievements page's command (achievements.rs holds the logic). It reads the disk and the network, so it runs off the UI thread.
//! It changes nothing and sends nothing: the page's mod switch and sharing setting go through the Mods page's commands.

use tauri::AppHandle;

use crate::achievements::{self, AchievementsView, Kept};
use crate::config::{config_path, game_dir};
use crate::states::States;

/// The page: the hub's list with everyone's counts and the pictures, and what the player earned (the mod's `earned.tsv`). `refresh` reads
/// the list and the counts again instead of using this session's copies.
#[tauri::command]
pub async fn achievements_view(app: AppHandle, refresh: Option<bool>) -> Result<AchievementsView, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let beside = |name: &str| config_path(&app).map(|p| p.with_file_name(name)).unwrap_or_default();
        let kept = Kept { catalog: beside("mods-catalog.json"), stats: beside("achievement-counts.json"), pictures: beside("achievement-pictures") };
        achievements::view(&game_dir(&app), &app.mod_catalog(), &app.achievement_stats(), &kept, refresh.unwrap_or(false))
    })
    .await
    .map_err(|e| e.to_string())?
}
