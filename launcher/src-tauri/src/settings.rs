// SPDX-License-Identifier: GPL-3.0-or-later
//! Reads and edits the runtime's ini files next to pkp.exe. The key schema lives in the frontend;
//! this side only moves `section.key` values in and out, with the careful editor in ini.rs.

use crate::fsutil::{self, AtomicFail};
use crate::ini::Ini;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

pub const PKP_INI: &str = "pkp.ini";
pub const CONTROLS_INI: &str = "controls.ini";

#[derive(Serialize)]
pub struct SettingsDump {
    pub game_dir: String,
    pub pkp_exists: bool,
    pub controls_exists: bool,
    pub pkp: HashMap<String, String>,
    pub controls: HashMap<String, String>,
}

#[derive(Deserialize)]
pub struct Edit {
    /// "pkp" or "controls"
    pub file: String,
    pub section: String,
    pub key: String,
    pub value: String,
}

fn load(path: &Path) -> Option<Ini> {
    let bytes = fs::read(path).ok()?;
    // the files are UTF-8 (controls.ini has a non-ASCII header); a stray bad byte must not lose the file
    Some(Ini::parse(&String::from_utf8_lossy(&bytes)))
}

fn map_of(ini: &Option<Ini>) -> HashMap<String, String> {
    ini.as_ref().map(|i| i.all().into_iter().collect()).unwrap_or_default()
}

pub fn file_for(dir: &Path, which: &str) -> Result<PathBuf, String> {
    match which {
        "pkp" => Ok(dir.join(PKP_INI)),
        "controls" => Ok(dir.join(CONTROLS_INI)),
        other => Err(format!("unknown settings file '{}'", other)),
    }
}

/// pkp.ini as `section.key` -> value. A key that appears twice keeps its last value, which is what the runtime does (diag/config.cpp
/// parse_ini overwrites), so every launcher reading of pkp.ini that must agree with the game goes through here.
pub fn read_pkp(dir: &Path) -> HashMap<String, String> {
    map_of(&load(&dir.join(PKP_INI)))
}

pub fn read(dir: &Path) -> SettingsDump {
    let pkp = load(&dir.join(PKP_INI));
    let controls = load(&dir.join(CONTROLS_INI));
    SettingsDump {
        game_dir: crate::dump::fwd(dir),
        pkp_exists: pkp.is_some(),
        controls_exists: controls.is_some(),
        pkp: map_of(&pkp),
        controls: map_of(&controls),
    }
}

/// Writes `text` over `path` through a temporary file (fsutil::write_atomic), with this module's wording for a failure.
pub fn write_atomic(path: &Path, text: &str) -> Result<(), String> {
    fsutil::write_atomic(path, text.as_bytes()).map_err(|fail| match fail {
        AtomicFail::Write { tmp, error } => format!("cannot write {}: {}", tmp.display(), error),
        AtomicFail::Replace { error } => format!("cannot replace {}: {}", path.display(), error),
    })
}

/// Applies edits grouped per file. controls.ini is never created: the game writes it with its factory
/// bindings on first run, and a file with only a few keys would lose them.
pub fn apply(dir: &Path, edits: &[Edit]) -> Result<(), String> {
    for which in ["pkp", "controls"] {
        let mine: Vec<&Edit> = edits.iter().filter(|e| e.file == which).collect();
        if mine.is_empty() {
            continue;
        }
        let path = file_for(dir, which)?;
        let mut ini = match load(&path) {
            Some(i) => i,
            None if which == "pkp" => Ini::empty(),
            None => return Err(format!("{} does not exist yet. Start the game once so it creates it.", CONTROLS_INI)),
        };
        for e in mine {
            ini.set(&e.section, &e.key, &e.value);
        }
        write_atomic(&path, &ini.to_text())?;
    }
    Ok(())
}

/// Called once when the launcher window starts (not by `--play` or `--host`): puts Link Play's per-session [net] keys back to their
/// defaults, so nothing left from an earlier session or an older version (a `join:<ip>`, a relay that is no longer running, an old room
/// code, `protocol = linkplay`) decides how the next one runs (#239). The Link Play page remembers the player's own addresses and codes in
/// its storage and offers them again. Off stays off; `lan_sweep` and `uid` are preferences and are kept. A missing pkp.ini is left
/// missing, and the file is only written when a value changes. True: something was reset.
pub fn reset_link_play(dir: &Path) -> Result<bool, String> {
    let path = dir.join(PKP_INI);
    let Some(mut ini) = load(&path) else { return Ok(false) };
    let cur = |ini: &Ini, key: &str| ini.get("net", key).map(|v| v.trim().to_ascii_lowercase());
    let off = matches!(cur(&ini, "mode").as_deref(), Some("off") | Some("0") | Some(""));
    let mut changed = false;
    let mut put = |ini: &mut Ini, key: &str, value: &str, keep: &dyn Fn(&str) -> bool| {
        if let Some(v) = cur(ini, key) {
            if !keep(&v) {
                ini.set("net", key, value);
                changed = true;
            }
        }
    };
    put(&mut ini, "protocol", "native", &|v| v == "native");
    if !off {
        put(&mut ini, "mode", "auto", &|v| v == "auto");
    }
    put(&mut ini, "internet", "false", &|v| matches!(v, "" | "false" | "0" | "no" | "off"));
    put(&mut ini, "relay", "", &|v| v.is_empty());
    put(&mut ini, "room_code", "", &|v| v.is_empty());
    if changed {
        write_atomic(&path, &ini.to_text())?;
    }
    Ok(changed)
}

/// Used by Play and setup: the ini keys the launcher owns. pkp.exe reads every path from pkp.ini (a pkp.ini next to it makes
/// the game folder its working directory), so it needs no command-line paths.
/// A path for pkp.ini: relative to the game folder (forward slashes) when it lies inside it, so the folder can be moved or copied to
/// another PC or to a Steam Deck with its dump and saves; absolute otherwise. The game resolves relative paths against its folder.
pub fn portable(dir: &Path, p: &str) -> String {
    let full = Path::new(p.trim());
    match (fs::canonicalize(dir), fs::canonicalize(full)) {
        (Ok(d), Ok(f)) => match f.strip_prefix(&d) {
            Ok(rel) if !rel.as_os_str().is_empty() => crate::dump::fwd(rel),
            _ => p.to_string(),
        },
        _ => p.to_string(),
    }
}

/// `romfs_base`: the base game's romfs to layer under `romfs` ([fs] romfs_base; empty: none). It follows the launcher's choice while
/// [fs] romfs is the launcher's own value; a romfs the user pointed elsewhere keeps the romfs_base the user set with it.
/// `aoc_dir`: the dump's add-on content folder ([fs] aoc_dir; empty: the dump has none). Written once; a value the user set is kept.
pub fn ensure_play_paths(dir: &Path, exefs: &str, romfs: &str, romfs_base: &str, aoc_dir: &str) -> Result<(), String> {
    let path = dir.join(PKP_INI);
    let mut ini = load(&path).unwrap_or_else(Ini::empty);
    let mut changed = !path.exists();
    let resolve = |v: &str| {
        let p = Path::new(v.trim());
        let full = if p.is_absolute() { p.to_path_buf() } else { dir.join(p) };
        fs::canonicalize(&full).unwrap_or(full)
    };
    let romfs_was_ours = ini.get("fs", "romfs").is_none_or(|cur| cur.trim().is_empty() || resolve(&cur) == resolve(romfs));

    // [fs] exefs and [fs] romfs: kept while they name an existing folder or .nup (the user may have pointed them elsewhere)
    for (key, value) in [("exefs", exefs), ("romfs", romfs)] {
        let cur = ini.get("fs", key).unwrap_or_default();
        let cur_path = Path::new(cur.trim());
        // relative paths in the ini are relative to the game folder, the working directory of the game
        let cur_full = if cur_path.is_absolute() { cur_path.to_path_buf() } else { dir.join(cur_path) };
        if cur.trim().is_empty() || !(cur_full.is_dir() || cur_full.is_file()) {
            ini.set("fs", key, &portable(dir, value));
            changed = true;
        }
    }
    let romfs_is_ours = ini.get("fs", "romfs").is_some_and(|cur| resolve(&cur) == resolve(romfs));
    if romfs_was_ours && romfs_is_ours {
        let want = if romfs_base.is_empty() { String::new() } else { portable(dir, romfs_base) };
        if ini.get("fs", "romfs_base").unwrap_or_default().trim() != want {
            ini.set("fs", "romfs_base", &want);
            changed = true;
        }
    }
    if ini.get("nvn", "shader_cache").unwrap_or_default().trim() != crate::shaders::SPIRV_REL {
        ini.set("nvn", "shader_cache", crate::shaders::SPIRV_REL);
        changed = true;
    }
    // shaders the game builds while it runs, for play.rs to translate after it exits (kept if the user set it, even empty)
    if ini.get("nvn", "dump_missing").is_none() {
        ini.set("nvn", "dump_missing", crate::shaders::MISSING_REL);
        changed = true;
    }
    // the add-on content (DLC) of the dump, for pkp's aoc:u and OpenDataStorageByDataId (kept if the user set it, even empty)
    if !aoc_dir.is_empty() && ini.get("fs", "aoc_dir").is_none() {
        ini.set("fs", "aoc_dir", &portable(dir, aoc_dir));
        changed = true;
    }
    let cur_save = ini.get("fs", "save_dir").unwrap_or_default();
    if cur_save.trim().is_empty() {
        let _ = fs::create_dir_all(dir.join("save"));
        ini.set("fs", "save_dir", "save");   // relative: the saves move with the folder
        changed = true;
    }
    if changed {
        write_atomic(&path, &ini.to_text())?;
    }
    Ok(())
}

/// The folder add-ons (DLC) are unpacked into: the one `[fs] aoc_dir` names, else `<dir>/aoc`, which is then written to pkp.ini.
/// A dump's own aoc folder is therefore filled, not left behind, and pkp serves one folder only.
pub fn aoc_target(dir: &Path) -> Result<PathBuf, String> {
    let path = dir.join(PKP_INI);
    let mut ini = load(&path).unwrap_or_else(Ini::empty);
    let cur = ini.get("fs", "aoc_dir").unwrap_or_default();
    if !cur.trim().is_empty() {
        let p = Path::new(cur.trim());
        return Ok(if p.is_absolute() { p.to_path_buf() } else { dir.join(p) });
    }
    let aoc = dir.join("aoc");
    fs::create_dir_all(&aoc).map_err(|e| format!("Cannot create {}: {e}", aoc.display()))?;
    ini.set("fs", "aoc_dir", &portable(dir, &crate::dump::fwd(&aoc)));
    write_atomic(&path, &ini.to_text())?;
    Ok(aoc)
}

#[cfg(test)]
mod reset_tests {
    use super::*;

    fn run(text: &str) -> (bool, String) {
        static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        let dir = std::env::temp_dir().join(format!("pkp-reset-{}-{}", std::process::id(), n));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        if !text.is_empty() {
            fs::write(dir.join(PKP_INI), text).unwrap();
        }
        let changed = reset_link_play(&dir).unwrap();
        let out = fs::read_to_string(dir.join(PKP_INI)).unwrap_or_default();
        let _ = fs::remove_dir_all(&dir);
        (changed, out)
    }

    fn net(text: &str, key: &str) -> Option<String> {
        Ini::parse(text).get("net", key)
    }

    #[test]
    fn leftovers_go_back_to_defaults() {
        let (changed, out) = run("[net]\nprotocol = linkplay\nmode = join:192.168.0.20\ninternet = true\nrelay = 127.0.0.1:47610\n\
                                  room_code = ABCD-ABCD-ABCD-ABCD\nlan_sweep = off\nuid = 0123456789abcdef0123456789abcdef\n[pkp]\nlink_story_coop = true\n");
        assert!(changed);
        assert_eq!(net(&out, "protocol").as_deref(), Some("native"));
        assert_eq!(net(&out, "mode").as_deref(), Some("auto"));
        assert_eq!(net(&out, "internet").as_deref(), Some("false"));
        assert_eq!(net(&out, "relay").as_deref(), Some(""));
        assert_eq!(net(&out, "room_code").as_deref(), Some(""));
        assert_eq!(net(&out, "lan_sweep").as_deref(), Some("off"));
        assert_eq!(net(&out, "uid").as_deref(), Some("0123456789abcdef0123456789abcdef"));
        assert_eq!(Ini::parse(&out).get("pkp", "link_story_coop").as_deref(), Some("true"));
    }

    #[test]
    fn a_bare_address_in_mode_is_reset() {
        let (_, out) = run("[net]\nmode = 192.168.0.20\n");
        assert_eq!(net(&out, "mode").as_deref(), Some("auto"));
    }

    #[test]
    fn off_stays_off() {
        let (changed, out) = run("[net]\nmode = off\ninternet = true\n");
        assert!(changed);
        assert_eq!(net(&out, "mode").as_deref(), Some("off"));
        assert_eq!(net(&out, "internet").as_deref(), Some("false"));
    }

    #[test]
    fn defaults_and_missing_keys_are_not_written() {
        let text = "[video]\nwidth = 1920\n[net]\nmode = auto\ninternet = false\n";
        let (changed, out) = run(text);
        assert!(!changed);
        assert_eq!(out, text);
    }

    #[test]
    fn a_missing_file_stays_missing() {
        let (changed, out) = run("");
        assert!(!changed);
        assert!(out.is_empty());
    }

    #[test]
    fn writers_of_one_file_do_not_share_a_temporary_name() {
        let target = Path::new("some").join("x.ini");
        let names: std::collections::HashSet<PathBuf> = (0..100).map(|_| fsutil::temp_beside(&target)).collect();
        assert_eq!(names.len(), 100);
        assert!(names.iter().all(|n| n.parent() == target.parent() && n.to_string_lossy().ends_with(".tmp")), "beside the target, so the rename stays on one volume");
    }
}

#[cfg(test)]
mod aoc_tests {
    use super::*;

    #[test]
    fn add_ons_go_to_the_named_folder_or_to_aoc_next_to_the_game() {
        let dir = std::env::temp_dir().join(format!("pkp-aoc-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        // no pkp.ini: <dir>/aoc, and the key is written for the game
        assert_eq!(aoc_target(&dir).unwrap(), dir.join("aoc"));
        assert_eq!(load(&dir.join(PKP_INI)).unwrap().get("fs", "aoc_dir").as_deref(), Some("aoc"));
        // a key the user (or the dump) set is followed, relative to the game folder
        fs::write(dir.join(PKP_INI), "[fs]\naoc_dir = dump/aoc\n").unwrap();
        assert_eq!(aoc_target(&dir).unwrap(), dir.join("dump/aoc"));
        let _ = fs::remove_dir_all(&dir);
    }
}
