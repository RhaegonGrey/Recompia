// SPDX-License-Identifier: GPL-3.0-or-later
//! Commands that open a link or a file of the game folder in the system's default program.

use tauri::AppHandle;

use crate::config::game_dir;
use crate::play;

/// Opens an allow-listed link (release pages, issues, Discord invite, mod hub pages) in the default browser.
#[tauri::command]
pub fn open_link(url: String) -> Result<(), String> {
    if !openable_link(&url) {
        return Err("That link cannot be opened from here.".into());
    }
    play::open_url(&url)
}

/// The links the launcher opens in the browser: GitHub (releases, issues), the project's Discord invite (exact host: a look-alike such as discord.recompia.com.evil.example fails) and the Modtopia mod hub's pages (mods_catalog.rs). No spaces, quotes or `&`.
pub fn openable_link(url: &str) -> bool {
    crate::mods_catalog::hub_link(url) || (url.starts_with("https://github.com/") || url == "https://discord.recompia.com" || url.starts_with("https://discord.recompia.com/")) && !url.chars().any(|c| c.is_whitespace() || c == '"' || c == '&')
}

#[cfg(test)]
mod link_tests {
    use super::openable_link;

    #[test]
    fn only_github_and_the_discord_invite_open() {
        assert!(openable_link("https://github.com/featureJosh/Recompia/releases/tag/v0.8.0"));
        assert!(openable_link("https://discord.recompia.com"));
        assert!(openable_link("https://pokopia-native.vercel.app/mods/auto-run"));
        for bad in ["http://github.com/x", "https://github.com.evil.example/x", "https://discord.com/invite/x", "https://discord.gg/x", "https://discord.recompia.com.evil.example/x", "https://discord.recompia.com/ab cd", "https://discord.recompia.com/a&b", "file:///etc/passwd", "javascript:alert(1)", ""] {
            assert!(!openable_link(bad), "{bad:?}");
        }
    }
}

#[tauri::command]
pub fn open_game_file(app: AppHandle, name: String) -> Result<(), String> {
    if !play::REPORT_FILES.contains(&name.as_str()) {
        return Err("That file cannot be opened from here.".into());
    }
    let path = game_dir(&app).join(&name);
    if !path.is_file() {
        return Err(format!("{} does not exist.", name));
    }
    play::open_with_shell(&path)
}

#[tauri::command]
pub fn open_game_dir(app: AppHandle) -> Result<(), String> {
    play::open_with_shell(&game_dir(&app))
}
