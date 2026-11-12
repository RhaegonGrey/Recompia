// SPDX-License-Identifier: GPL-3.0-or-later
//! Where the launcher lives and which folder is the game folder.

use std::path::{Path, PathBuf};

use crate::config::Config;

/// The launcher's folder, where `tools/` is. An AppImage runs from a temporary mount: there it is the folder of the
/// .AppImage file itself ($APPIMAGE, set by the AppImage runtime).
pub fn exe_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("APPIMAGE").map(PathBuf::from).and_then(|p| p.parent().map(Path::to_path_buf)) {
        return dir;
    }
    std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())).unwrap_or_else(|| PathBuf::from("."))
}

/// The game folder a fresh install starts from: the launcher's own folder, or -- inside a macOS `.app` -- the folder the bundle
/// sits in (the bundle carries the launcher only; pkp, pkp.ini and shader_cache stay beside it, where a 26 GB cache belongs).
pub fn default_game_dir() -> PathBuf {
    let exe = exe_dir();
    if exe.file_name().is_some_and(|n| n == "MacOS") && exe.parent().is_some_and(|p| p.file_name().is_some_and(|n| n == "Contents")) {
        let beside = exe.parent().and_then(std::path::Path::parent).and_then(std::path::Path::parent);
        // a game folder kept beside the bundle (development) wins over the installed one
        if let Some(dir) = &beside {
            if dir.join(crate::settings::PKP_INI).is_file() {
                return dir.to_path_buf();
            }
        }
        // Installed into /Applications: the bundle is read only, so the game folder (pkp, pkp.ini, shader_cache, saves)
        // lives in Application Support, which Set up creates.
        if let Some(home) = std::env::var_os("HOME").map(PathBuf::from) {
            return home.join("Library").join("Application Support").join("Recompia").join("game");
        }
        if let Some(dir) = beside {
            return dir.to_path_buf();
        }
    }
    exe
}

pub fn game_dir_of(cfg: &Config) -> PathBuf {
    let chosen = cfg.game_dir.trim();
    // A game folder inside an .app is never the real one: an earlier build offered exe_dir() (the bundle's Contents) as the
    // default, and saving the settings wrote it back. Ignore it, so the next save heals the file.
    if chosen.is_empty() || chosen.contains(".app/") {
        default_game_dir()
    } else {
        PathBuf::from(chosen)
    }
}

/// The launcher file itself: the AppImage, not the program inside its mount.
pub fn launcher_path() -> Option<PathBuf> {
    std::env::var_os("APPIMAGE").map(PathBuf::from).or_else(|| std::env::current_exe().ok())
}
