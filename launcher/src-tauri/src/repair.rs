// SPDX-License-Identifier: GPL-3.0-or-later
//! Repair and reset: puts settings back to the defaults the runtime already has, clears caches the game rebuilds, and removes what old
//! versions left behind. Every action is planned first (what exactly would change), the small text files it changes are copied to
//! `reset-backup/<date-time>/` before anything is touched, and `undo` puts them back.
//!
//! What it never touches, whatever the action: the saves, the profile list, the shader cache, the game build, the dump, the update's own
//! folder. Paths come only from the fixed lists below or from the install folder's own file names, never from the caller.

use crate::ini::Ini;
use crate::settings::{write_atomic, CONTROLS_INI, PKP_INI};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};

pub const BACKUP_DIR: &str = "reset-backup";
const MANIFEST: &str = "manifest.json";
const KEEP_BACKUPS: usize = 3;
const LAUNCHER_JSON: &str = "launcher.json";

pub const ACTIONS: [&str; 5] = ["game_settings", "controls", "launcher_prefs", "caches", "leftovers"];

/// What "Reset game settings" can reset on its own: the pkp.ini sections that belong together, as the player thinks of them. The labels
/// live in the launcher (`repair-model.js`, which a test keeps in step with these ids). A key whose section is in no list belongs to
/// `OTHER_GROUP`, so every key is in exactly one group and "all groups" is exactly the old full reset. This table is the only place that
/// decides which setting belongs where (the game's own menu can use the same split).
pub const GAME_GROUPS: [(&str, &[&str]); 5] = [
    ("graphics", &["nvn", "gfx", "upscale", "display", "picture", "hdr", "fps", "latency", "memory", "sched", "media"]),
    ("network", &["net"]),
    ("gameplay", &["pkp", "play"]),
    ("clock", &["time"]),
    ("backups", &["backup"]),
];
/// Every pkp.ini section not named above.
pub const OTHER_GROUP: &str = "other";

/// All group ids, in the order the launcher lists them.
pub fn group_ids() -> Vec<String> {
    GAME_GROUPS.iter().map(|(id, _)| id.to_string()).chain(std::iter::once(OTHER_GROUP.to_string())).collect()
}

fn group_of(key: &str) -> &'static str {
    let section = key.split_once('.').map_or("", |(s, _)| s);
    GAME_GROUPS.iter().find(|(_, sections)| sections.contains(&section)).map_or(OTHER_GROUP, |(id, _)| id)
}

fn check_groups(groups: &[String]) -> Result<(), String> {
    if groups.is_empty() {
        return Err("Pick at least one group of game settings to reset.".into());
    }
    let known = group_ids();
    for g in groups {
        if !known.contains(g) {
            return Err(format!("Unknown group of game settings '{}'.", g));
        }
    }
    Ok(())
}

/// Rebuilt by the game on its next start (the first run may stutter once).
const CACHE_FILES: [&str; 4] = ["pipeline_cache.bin", "pipeline_keys.bin", "shader_order.txt", "pkp_freeze.txt"];
/// launcher.json keeps the places the launcher needs; everything else is a preference and goes back to its default.
const LAUNCHER_KEEP: [&str; 3] = ["dump_dir", "game_dir", "channel"];
/// Files whose old copies updates set aside (`pkp_rt.dll.0.7.6`); the file with the plain name is the current one and is never listed.
const VERSIONED: [&str; 7] = ["pkp_rt.dll", "libpkp_rt.so", "libpkp_rt.dylib", "PokopiaLauncher.AppImage", "pkp.exe", "pkp", "PokopiaLauncher.exe"];
/// Never a target of any action (a unit test checks that no plan names one).
pub const PROTECTED: [&str; 14] = ["save", "save-backups", "rollback-backups", "profiles.json", "shader_cache", "build", "nup_dump", ".update", "tools", "pkp.exe", "pkp", "pkp_rt.dll", "libpkp_rt.so", "libpkp_rt.dylib"];

#[derive(Clone, Copy, PartialEq, Debug)]
enum Root {
    Game,
    Config,
}

impl Root {
    fn name(self) -> &'static str {
        match self {
            Root::Game => "game",
            Root::Config => "config",
        }
    }
}

/// One file the plan changes. `text` None removes it, Some rewrites it.
struct Step {
    action: &'static str,
    root: Root,
    rel: String,
    text: Option<String>,
    backup: bool,
    bytes: u64,
    /// game_settings only: the groups reset, empty when all of them (the full reset)
    groups: Vec<String>,
}

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Change {
    pub action: String,
    pub file: String,
    /// "reset" (rewritten with defaults) or "remove"
    pub what: String,
    pub bytes: u64,
    /// a copy is kept in reset-backup, so Undo brings it back
    pub backup: bool,
    /// game_settings: the groups that reset when the player picked only some of them; empty means everything
    pub groups: Vec<String>,
}

#[derive(Serialize, Default, Debug)]
pub struct Plan {
    pub changes: Vec<Change>,
}

#[derive(Serialize, Default, Debug)]
pub struct Report {
    pub done: Vec<String>,
    pub backup: Option<String>,
}

#[derive(Serialize, Debug, PartialEq)]
pub struct BackupInfo {
    pub name: String,
    pub files: Vec<String>,
}

#[derive(Serialize, Deserialize)]
struct Entry {
    root: String,
    rel: String,
}

#[derive(Serialize, Deserialize)]
struct Manifest {
    entries: Vec<Entry>,
}

/// A plain file name: no folder, no `..`, not empty.
fn safe_name(name: &str) -> bool {
    !name.is_empty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\\') && !name.contains('\0')
}

fn base_path(root: Root, game: &Path, cfg: &Path) -> PathBuf {
    match root {
        Root::Game => game.to_path_buf(),
        Root::Config => cfg.to_path_buf(),
    }
}

/// Which pkp.ini values survive a reset: the places the launcher put (the game files and shader folders), the profile's identity and the
/// Link Play uid. Everything else is a tuning choice and goes back to the runtime's default by being absent.
fn keep_key(key: &str) -> bool {
    key.starts_with("fs.") || key.starts_with("account.") || matches!(key, "net.uid" | "nvn.shader_cache" | "nvn.dump_missing")
}

pub fn reset_ini_text(old: &str) -> String {
    reset_ini_groups(old, &group_ids())
}

/// pkp.ini after resetting only `groups`: the kept places and identity always stay, and so does every key of a group that was not picked.
/// With every group this is exactly the full reset. (Comment lines are not carried over, as before; the backup has them.)
pub fn reset_ini_groups(old: &str, groups: &[String]) -> String {
    let mut fresh = Ini::empty();
    for (key, value) in Ini::parse(old).all() {
        if keep_key(&key) || !groups.iter().any(|g| g == group_of(&key)) {
            if let Some((section, name)) = key.split_once('.') {
                fresh.set(section, name, &value);
            }
        }
    }
    let text = fresh.to_text();
    if old.contains("\r\n") {
        text
    } else {
        text.replace("\r\n", "\n")
    }
}

/// launcher.json after a reset: only the kept places, as text. A file that does not parse (damaged) gives an empty object, so the
/// launcher starts from its defaults and the damaged original is in the backup.
pub fn reset_launcher_text(old: &str) -> String {
    let mut out = serde_json::Map::new();
    if let Ok(serde_json::Value::Object(map)) = serde_json::from_str::<serde_json::Value>(old) {
        for key in LAUNCHER_KEEP {
            if let Some(v @ serde_json::Value::String(_)) = map.get(key) {
                out.insert(key.to_string(), v.clone());
            }
        }
    }
    serde_json::to_string_pretty(&serde_json::Value::Object(out)).unwrap_or_else(|_| "{}".into())
}

/// `pkp_rt.dll.0.7.6` yes; `pkp_rt.dll`, `pkp_rt.dll.bak` and `pkp.log.prev` no.
pub fn is_old_copy(name: &str) -> bool {
    VERSIONED.iter().any(|base| {
        name.strip_prefix(base).and_then(|rest| rest.strip_prefix('.')).is_some_and(|v| v.chars().next().is_some_and(|c| c.is_ascii_digit()))
    })
}

fn size_of(path: &Path) -> u64 {
    fs::metadata(path).map(|m| m.len()).unwrap_or(0)
}

fn steps(game: &Path, cfg: &Path, actions: &[String], groups: Option<&[String]>) -> Result<Vec<Step>, String> {
    for a in actions {
        if !ACTIONS.contains(&a.as_str()) {
            return Err(format!("Unknown repair action '{}'.", a));
        }
    }
    if let Some(g) = groups {
        check_groups(g)?;
    }
    // None, or every group, is the full reset; a smaller list resets only those groups
    let partial: Vec<String> = match groups {
        Some(g) if !group_ids().iter().all(|id| g.contains(id)) => group_ids().into_iter().filter(|id| g.contains(id)).collect(),
        _ => Vec::new(),
    };
    let want = |name: &str| actions.iter().any(|a| a == name);
    let mut out: Vec<Step> = Vec::new();
    if want("game_settings") {
        let path = game.join(PKP_INI);
        if let Ok(bytes) = fs::read(&path) {
            let old = String::from_utf8_lossy(&bytes).into_owned();
            let text = if partial.is_empty() { reset_ini_text(&old) } else { reset_ini_groups(&old, &partial) };
            // the full reset keeps its old test (any rewrite counts); a partial one counts only when a picked key is really there
            let changed = if partial.is_empty() { text != old } else { Ini::parse(&text).all() != Ini::parse(&old).all() };
            if changed {
                out.push(Step { action: "game_settings", root: Root::Game, rel: PKP_INI.into(), text: Some(text), backup: true, bytes: bytes.len() as u64, groups: partial.clone() });
            }
        }
    }
    if want("controls") {
        let path = game.join(CONTROLS_INI);
        if path.is_file() {
            out.push(Step { action: "controls", root: Root::Game, rel: CONTROLS_INI.into(), text: None, backup: true, bytes: size_of(&path), groups: Vec::new() });
        }
    }
    if want("launcher_prefs") {
        let path = cfg.join(LAUNCHER_JSON);
        if let Ok(bytes) = fs::read(&path) {
            let old = String::from_utf8_lossy(&bytes).into_owned();
            let text = reset_launcher_text(&old);
            if text != old {
                out.push(Step { action: "launcher_prefs", root: Root::Config, rel: LAUNCHER_JSON.into(), text: Some(text), backup: true, bytes: bytes.len() as u64, groups: Vec::new() });
            }
        }
    }
    if want("caches") {
        for name in CACHE_FILES {
            let path = game.join(name);
            if path.is_file() {
                out.push(Step { action: "caches", root: Root::Game, rel: name.into(), text: None, backup: false, bytes: size_of(&path), groups: Vec::new() });
            }
        }
    }
    if want("leftovers") {
        let mut names: Vec<String> = fs::read_dir(game)
            .map_err(|e| format!("Cannot read {}: {}", game.display(), e))?
            .filter_map(|e| e.ok())
            .filter(|e| e.path().is_file())
            .filter_map(|e| e.file_name().into_string().ok())
            .filter(|n| is_old_copy(n))
            .collect();
        names.sort();
        for name in names {
            let bytes = size_of(&game.join(&name));
            out.push(Step { action: "leftovers", root: Root::Game, rel: name, text: None, backup: false, bytes, groups: Vec::new() });
        }
    }
    for s in &out {
        if !safe_name(&s.rel) || PROTECTED.contains(&s.rel.as_str()) {
            return Err(format!("Refusing to touch {}.", s.rel));
        }
    }
    Ok(out)
}

#[cfg(test)]
pub fn plan(game: &Path, cfg: &Path, actions: &[String]) -> Result<Plan, String> {
    plan_with(game, cfg, actions, None)
}

/// `groups`: which game settings groups to reset when "game_settings" is picked; None is all of them.
pub fn plan_with(game: &Path, cfg: &Path, actions: &[String], groups: Option<&[String]>) -> Result<Plan, String> {
    let changes = steps(game, cfg, actions, groups)?
        .into_iter()
        .map(|s| Change {
            action: s.action.into(),
            file: s.rel,
            what: if s.text.is_some() { "reset" } else { "remove" }.into(),
            bytes: s.bytes,
            backup: s.backup,
            groups: s.groups,
        })
        .collect();
    Ok(Plan { changes })
}

fn backups(game: &Path) -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir(game.join(BACKUP_DIR))
        .map(|rd| rd.filter_map(|e| e.ok()).filter(|e| e.path().join(MANIFEST).is_file()).filter_map(|e| e.file_name().into_string().ok()).collect())
        .unwrap_or_default();
    names.sort();
    names
}

fn read_manifest(dir: &Path) -> Result<Manifest, String> {
    let text = fs::read_to_string(dir.join(MANIFEST)).map_err(|e| format!("The backup list is missing: {}", e))?;
    serde_json::from_str(&text).map_err(|e| format!("The backup list is damaged: {}", e))
}

pub fn last_backup(game: &Path) -> Option<BackupInfo> {
    let name = backups(game).pop()?;
    let manifest = read_manifest(&game.join(BACKUP_DIR).join(&name)).ok()?;
    Some(BackupInfo { name, files: manifest.entries.into_iter().map(|e| e.rel).collect() })
}

fn stamp(game: &Path) -> String {
    let base = crate::fsutil::file_stamp();
    let taken = |n: &str| game.join(BACKUP_DIR).join(n).exists();
    if !taken(&base) {
        return base;
    }
    (2..1000).map(|i| format!("{}-{}", base, i)).find(|n| !taken(n)).unwrap_or(base)
}

fn copy_into(from: &Path, to: &Path) -> Result<(), String> {
    if let Some(parent) = to.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("Cannot create {}: {}", parent.display(), e))?;
    }
    let tmp = PathBuf::from(format!("{}.tmp", to.display()));
    fs::copy(from, &tmp).map_err(|e| format!("Cannot copy {}: {}", from.display(), e))?;
    fs::rename(&tmp, to).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        format!("Cannot place {}: {}", to.display(), e)
    })
}

fn describe(step: &Step) -> String {
    match step.action {
        "game_settings" if !step.groups.is_empty() => "The chosen game settings (pkp.ini) are back to their defaults; the other settings, your game files, profile and paths are kept".to_string(),
        "game_settings" => "Game settings (pkp.ini) are back to their defaults; your game files, profile and paths are kept".to_string(),
        "controls" => "Controls (controls.ini) removed; the game creates it again with its default bindings".to_string(),
        "launcher_prefs" => "Launcher settings are back to their defaults; the game folder, dump folder and update channel are kept".to_string(),
        "caches" => format!("Cache file {} removed; the game builds it again", step.rel),
        _ => format!("Old copy {} removed", step.rel),
    }
}

#[cfg(test)]
pub fn apply(game: &Path, cfg: &Path, actions: &[String]) -> Result<Report, String> {
    apply_with(game, cfg, actions, None)
}

pub fn apply_with(game: &Path, cfg: &Path, actions: &[String], groups: Option<&[String]>) -> Result<Report, String> {
    let steps = steps(game, cfg, actions, groups)?;
    if steps.is_empty() {
        return Ok(Report { done: vec!["Nothing needed changing.".into()], backup: None });
    }
    // 1. copy what is worth keeping, and check the copies, before anything is changed
    let kept: Vec<&Step> = steps.iter().filter(|s| s.backup).collect();
    let mut backup_name = None;
    if !kept.is_empty() {
        let name = stamp(game);
        let dir = game.join(BACKUP_DIR).join(&name);
        let result = (|| -> Result<(), String> {
            for s in &kept {
                let from = base_path(s.root, game, cfg).join(&s.rel);
                let to = dir.join(s.root.name()).join(&s.rel);
                copy_into(&from, &to)?;
                if size_of(&from) != size_of(&to) {
                    return Err(format!("The backup copy of {} is not complete.", s.rel));
                }
            }
            let manifest = Manifest { entries: kept.iter().map(|s| Entry { root: s.root.name().into(), rel: s.rel.clone() }).collect() };
            let text = serde_json::to_string_pretty(&manifest).map_err(|e| e.to_string())?;
            write_atomic(&dir.join(MANIFEST), &text)
        })();
        if let Err(e) = result {
            let _ = fs::remove_dir_all(&dir);
            return Err(format!("{} Nothing was changed.", e));
        }
        backup_name = Some(name);
    }
    // 2. the changes
    let mut done = Vec::new();
    for s in &steps {
        let path = base_path(s.root, game, cfg).join(&s.rel);
        let result = match &s.text {
            Some(text) => write_atomic(&path, text),
            None => fs::remove_file(&path).map_err(|e| format!("cannot remove {}: {}", path.display(), e)),
        };
        if let Err(e) = result {
            let kept_note = if backup_name.is_some() { " Use Undo last repair to put the earlier files back." } else { "" };
            return Err(format!("Stopped at {}: {}. {} change(s) were made before it.{}", s.rel, e, done.len(), kept_note));
        }
        done.push(describe(s));
    }
    done.dedup();
    // 3. old backups go
    let all = backups(game);
    for old in all.iter().take(all.len().saturating_sub(KEEP_BACKUPS)) {
        let _ = fs::remove_dir_all(game.join(BACKUP_DIR).join(old));
    }
    Ok(Report { done, backup: backup_name })
}

pub fn undo(game: &Path, cfg: &Path) -> Result<Report, String> {
    let name = backups(game).pop().ok_or("There is nothing to undo.")?;
    let dir = game.join(BACKUP_DIR).join(&name);
    let manifest = read_manifest(&dir)?;
    let mut done = Vec::new();
    for e in &manifest.entries {
        let root = match e.root.as_str() {
            "game" => Root::Game,
            "config" => Root::Config,
            other => return Err(format!("The backup lists an unknown place '{}'.", other)),
        };
        if !safe_name(&e.rel) || PROTECTED.contains(&e.rel.as_str()) {
            return Err(format!("The backup lists a file that is not allowed: {}.", e.rel));
        }
        copy_into(&dir.join(root.name()).join(&e.rel), &base_path(root, game, cfg).join(&e.rel))?;
        done.push(format!("{} restored", e.rel));
    }
    let _ = fs::remove_dir_all(&dir);
    Ok(Report { done, backup: None })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    fn tmp() -> PathBuf {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let dir = std::env::temp_dir().join(format!("pkp-repair-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::SeqCst)));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    const INI: &str = "[fs]\nexefs = /g/exefs\nromfs = /g/romfs\nsave_dir = save\nromfs_mod = \n\n[nvn]\nshader_cache = shader_cache/spirv\ndump_missing = shader_cache/missing\n; note\npool_mirror = off\ntex_write_watch = false\n\n[net]\nserver = x\nuid = aaaa\n\n[time]\nclock_offset = 60\nevent_preset = copycat\n\n[account]\nnickname = Koi\nuid = bbbb\n\n[pkp]\njump_momentum = 1\n";

    /// An install with everything in it: settings, caches, leftovers and the files no repair may touch.
    fn install() -> (PathBuf, PathBuf) {
        let game = tmp();
        let cfg = tmp();
        fs::write(game.join("pkp.ini"), INI).unwrap();
        fs::write(game.join("controls.ini"), "[meta]\ndefaults_rev = 5\n[keys]\nx = y\n").unwrap();
        fs::write(cfg.join("launcher.json"), r#"{"dump_dir":"D:/dump","game_dir":"C:/g","channel":"testing","minimize_on_play":true,"gentle_build":true,"window":{"x":1.0,"y":2.0,"width":3.0,"height":4.0}}"#).unwrap();
        for f in CACHE_FILES {
            fs::write(game.join(f), b"cache").unwrap();
        }
        for f in ["pkp_rt.dll.0.7.6", "PokopiaLauncher.AppImage.0.7.6", "pkp_rt.dll", "libpkp_rt.so", "libpkp_rt.dylib", "pkp.exe", "pkp", "PokopiaLauncher.AppImage", "pkp.log.prev", "pkp_rt.dll.bak"] {
            fs::write(game.join(f), f.as_bytes()).unwrap();
        }
        fs::write(game.join("profiles.json"), "{\"active\":\"default\"}").unwrap();
        for d in ["save", "shader_cache", "build", "nup_dump", ".update", "tools"] {
            fs::create_dir_all(game.join(d).join("inner")).unwrap();
            fs::write(game.join(d).join("inner").join("data.bin"), d.as_bytes()).unwrap();
        }
        (game, cfg)
    }

    fn all(_unused: &str) -> Vec<String> {
        ACTIONS.iter().map(|a| a.to_string()).collect()
    }

    /// The bytes of every file under a folder, to prove nothing moved.
    fn snapshot(root: &Path) -> Vec<(String, Vec<u8>)> {
        fn walk(dir: &Path, base: &Path, out: &mut Vec<(String, Vec<u8>)>) {
            let mut entries: Vec<_> = fs::read_dir(dir).unwrap().filter_map(|e| e.ok()).collect();
            entries.sort_by_key(|e| e.file_name());
            for e in entries {
                let p = e.path();
                if p.is_dir() {
                    walk(&p, base, out);
                } else {
                    out.push((p.strip_prefix(base).unwrap().display().to_string(), fs::read(&p).unwrap()));
                }
            }
        }
        let mut out = Vec::new();
        walk(root, root, &mut out);
        out
    }

    #[test]
    fn the_ini_reset_keeps_paths_and_identity_and_drops_the_rest() {
        let text = reset_ini_text(INI);
        let ini = Ini::parse(&text);
        for (k, v) in [("fs.exefs", "/g/exefs"), ("fs.romfs", "/g/romfs"), ("fs.save_dir", "save"), ("net.uid", "aaaa"), ("account.nickname", "Koi"), ("account.uid", "bbbb"), ("nvn.shader_cache", "shader_cache/spirv"), ("nvn.dump_missing", "shader_cache/missing")] {
            assert_eq!(ini.get(k.split('.').next().unwrap(), k.split('.').nth(1).unwrap()).as_deref(), Some(v), "{}", k);
        }
        for (s, k) in [("nvn", "pool_mirror"), ("nvn", "tex_write_watch"), ("net", "server"), ("time", "clock_offset"), ("time", "event_preset"), ("pkp", "jump_momentum")] {
            assert_eq!(ini.get(s, k), None, "{}.{}", s, k);
        }
        assert_eq!(reset_ini_text(&text), text, "a reset file resets to itself");
    }

    #[test]
    fn the_ini_reset_keeps_the_line_ending_style() {
        assert!(!reset_ini_text("[fs]\nexefs = a\n[nvn]\npool_mirror = on\n").contains('\r'));
        assert!(reset_ini_text("[fs]\r\nexefs = a\r\n[nvn]\r\npool_mirror = on\r\n").contains("\r\n"));
    }

    #[test]
    fn the_launcher_reset_keeps_the_places_and_survives_a_damaged_file() {
        let kept: serde_json::Value = serde_json::from_str(&reset_launcher_text(r#"{"dump_dir":"D:/d","game_dir":"C:/g","channel":"testing","gentle_build":true,"mods_auto_update":true,"window":{"x":1}}"#)).unwrap();
        assert_eq!(kept, serde_json::json!({"dump_dir":"D:/d","game_dir":"C:/g","channel":"testing"}));
        let bad: serde_json::Value = serde_json::from_str(&reset_launcher_text("{ this is not json")).unwrap();
        assert_eq!(bad, serde_json::json!({}));
        let wrong_type: serde_json::Value = serde_json::from_str(&reset_launcher_text(r#"{"dump_dir":5}"#)).unwrap();
        assert_eq!(wrong_type, serde_json::json!({}));
    }

    #[test]
    fn only_old_versioned_copies_count_as_leftovers() {
        for yes in ["pkp_rt.dll.0.7.6", "libpkp_rt.so.0.7.6", "libpkp_rt.dylib.0.7.6", "PokopiaLauncher.AppImage.0.7.9", "pkp.exe.0.7.5", "pkp.0.7.5", "PokopiaLauncher.exe.1.0"] {
            assert!(is_old_copy(yes), "{}", yes);
        }
        for no in ["pkp_rt.dll", "libpkp_rt.so", "libpkp_rt.dylib", "PokopiaLauncher.AppImage", "pkp.exe", "pkp", "pkp_rt.dll.bak", "pkp.log.prev", "pkp_rt.dll.", "save", "other.dll.0.7.6", ""] {
            assert!(!is_old_copy(no), "{}", no);
        }
    }

    #[test]
    fn the_plan_lists_only_what_exists_and_never_a_protected_name() {
        let (game, cfg) = install();
        let plan = plan(&game, &cfg, &all("")).unwrap();
        let files: Vec<&str> = plan.changes.iter().map(|c| c.file.as_str()).collect();
        for want in ["pkp.ini", "controls.ini", "launcher.json", "pipeline_cache.bin", "pipeline_keys.bin", "shader_order.txt", "pkp_freeze.txt", "pkp_rt.dll.0.7.6", "PokopiaLauncher.AppImage.0.7.6"] {
            assert!(files.contains(&want), "{} missing from {:?}", want, files);
        }
        assert_eq!(files.len(), 9, "{:?}", files);
        for c in &plan.changes {
            assert!(!PROTECTED.contains(&c.file.as_str()), "{}", c.file);
        }
        let only: Vec<String> = vec!["caches".into()];
        assert_eq!(super::plan(&game, &cfg, &only).unwrap().changes.len(), 4);
        assert!(super::plan(&game, &cfg, &["nonsense".to_string()]).is_err());
    }

    #[test]
    fn apply_changes_only_its_files_and_leaves_everything_protected_untouched() {
        let (game, cfg) = install();
        let guarded = ["save", "shader_cache", "build", "nup_dump", ".update", "tools"];
        let before: Vec<_> = guarded.iter().map(|d| snapshot(&game.join(d))).collect();
        let profiles = fs::read(game.join("profiles.json")).unwrap();
        let report = apply(&game, &cfg, &all("")).unwrap();
        assert!(report.backup.is_some());
        for (d, snap) in guarded.iter().zip(before) {
            assert_eq!(snapshot(&game.join(d)), snap, "{} changed", d);
        }
        assert_eq!(fs::read(game.join("profiles.json")).unwrap(), profiles);
        for keep in ["pkp.exe", "pkp", "pkp_rt.dll", "libpkp_rt.so", "libpkp_rt.dylib", "PokopiaLauncher.AppImage", "pkp.log.prev", "pkp_rt.dll.bak"] {
            assert!(game.join(keep).is_file(), "{} must stay", keep);
        }
        for gone in ["controls.ini", "pipeline_cache.bin", "pkp_rt.dll.0.7.6", "PokopiaLauncher.AppImage.0.7.6"] {
            assert!(!game.join(gone).exists(), "{} should be gone", gone);
        }
        assert_eq!(fs::read_to_string(game.join("pkp.ini")).unwrap(), reset_ini_text(INI));
        let launcher: serde_json::Value = serde_json::from_str(&fs::read_to_string(cfg.join("launcher.json")).unwrap()).unwrap();
        assert_eq!(launcher, serde_json::json!({"dump_dir":"D:/dump","game_dir":"C:/g","channel":"testing"}));
        assert_eq!(super::plan(&game, &cfg, &all("")).unwrap().changes.len(), 0, "a second run has nothing left to do");
    }

    #[test]
    fn undo_puts_the_settings_back_byte_for_byte() {
        let (game, cfg) = install();
        let ini = fs::read(game.join("pkp.ini")).unwrap();
        let controls = fs::read(game.join("controls.ini")).unwrap();
        let launcher = fs::read(cfg.join("launcher.json")).unwrap();
        apply(&game, &cfg, &all("")).unwrap();
        let info = last_backup(&game).unwrap();
        assert!(info.files.contains(&"pkp.ini".to_string()) && info.files.contains(&"launcher.json".to_string()));
        assert!(!info.files.contains(&"pipeline_cache.bin".to_string()), "caches are not copied");
        undo(&game, &cfg).unwrap();
        assert_eq!(fs::read(game.join("pkp.ini")).unwrap(), ini);
        assert_eq!(fs::read(game.join("controls.ini")).unwrap(), controls);
        assert_eq!(fs::read(cfg.join("launcher.json")).unwrap(), launcher);
        assert!(last_backup(&game).is_none(), "an undone backup is used up");
        assert!(undo(&game, &cfg).is_err());
    }

    #[test]
    fn a_damaged_launcher_file_is_backed_up_then_replaced() {
        let (game, cfg) = install();
        fs::write(cfg.join("launcher.json"), "{ cut off").unwrap();
        apply(&game, &cfg, &["launcher_prefs".to_string()]).unwrap();
        assert_eq!(fs::read_to_string(cfg.join("launcher.json")).unwrap(), "{}");
        undo(&game, &cfg).unwrap();
        assert_eq!(fs::read_to_string(cfg.join("launcher.json")).unwrap(), "{ cut off");
    }

    #[test]
    fn caches_and_leftovers_alone_make_no_backup() {
        let (game, cfg) = install();
        let report = apply(&game, &cfg, &["caches".to_string(), "leftovers".to_string()]).unwrap();
        assert!(report.backup.is_none());
        assert!(!game.join(BACKUP_DIR).exists());
    }

    #[test]
    fn a_missing_file_is_nothing_to_do_not_an_error() {
        let game = tmp();
        let cfg = tmp();
        let report = apply(&game, &cfg, &all("")).unwrap();
        assert_eq!(report.done, vec!["Nothing needed changing.".to_string()]);
    }

    #[test]
    fn only_the_newest_three_backups_stay() {
        let (game, cfg) = install();
        for i in 0..5 {
            fs::write(game.join("pkp.ini"), format!("{}[pkp]\nx = {}\n", INI, i)).unwrap();
            apply(&game, &cfg, &["game_settings".to_string()]).unwrap();
        }
        assert_eq!(backups(&game).len(), KEEP_BACKUPS);
    }

    #[test]
    fn names_that_leave_the_folder_are_refused() {
        for bad in ["", ".", "..", "a/b", "a\\b", "../pkp.ini"] {
            assert!(!safe_name(bad), "{}", bad);
        }
        assert!(safe_name("pkp.ini"));
    }

    /// A pkp.ini with a key in every kind of group, including sections no group names.
    const INI_GROUPS: &str = "[fs]\nexefs = /g/exefs\nsave_dir = save\n\n[nvn]\nshader_cache = shader_cache/spirv\ndump_missing = shader_cache/missing\npool_mirror = off\nview_scale = 2\n\n[net]\nserver = x\nuid = aaaa\n\n[time]\nclock_offset = 60\nevent_preset = copycat\n\n[account]\nnickname = Koi\nuid = bbbb\n\n[pkp]\njump_momentum = 1\ncamera_aim = 1\n\n[backup]\ninterval_minutes = 10\nkeep = 5\n\n[boot]\nfast = 1\n\n[title]\nskip = 1\n";

    /// The reset as it was before the groups existed: keep only the kept keys.
    fn old_full_reset(old: &str) -> String {
        let mut fresh = Ini::empty();
        for (key, value) in Ini::parse(old).all() {
            if keep_key(&key) {
                if let Some((section, name)) = key.split_once('.') {
                    fresh.set(section, name, &value);
                }
            }
        }
        let text = fresh.to_text();
        if old.contains("\r\n") { text } else { text.replace("\r\n", "\n") }
    }

    /// `section.key` lookup on a parsed file.
    fn at(ini: &Ini, key: &str) -> Option<String> {
        let (section, name) = key.split_once('.').unwrap();
        ini.get(section, name)
    }

    fn ids(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn all_groups_selected_gives_exactly_the_old_full_reset() {
        for src in [INI, INI_GROUPS, "", "[nvn]\na = 1\r\n"] {
            assert_eq!(reset_ini_groups(src, &group_ids()), old_full_reset(src));
            assert_eq!(reset_ini_text(src), old_full_reset(src));
        }
        // through the whole plan and apply path: "no list" and "every group" write the same bytes
        let (g1, c1) = install();
        let (g2, c2) = install();
        fs::write(g1.join("pkp.ini"), INI_GROUPS).unwrap();
        fs::write(g2.join("pkp.ini"), INI_GROUPS).unwrap();
        let act = vec!["game_settings".to_string()];
        apply_with(&g1, &c1, &act, None).unwrap();
        apply_with(&g2, &c2, &act, Some(&group_ids())).unwrap();
        assert_eq!(fs::read(g1.join("pkp.ini")).unwrap(), fs::read(g2.join("pkp.ini")).unwrap());
        assert_eq!(fs::read_to_string(g1.join("pkp.ini")).unwrap(), old_full_reset(INI_GROUPS));
    }

    #[test]
    fn a_partial_reset_only_drops_the_chosen_groups_and_keeps_the_rest() {
        let after = Ini::parse(&reset_ini_groups(INI_GROUPS, &ids(&["network", "clock"])));
        // the chosen groups are gone, except the identity key that is always kept
        assert_eq!(at(&after, "net.server"), None);
        assert_eq!(at(&after, "time.clock_offset"), None);
        assert_eq!(at(&after, "time.event_preset"), None);
        assert_eq!(at(&after, "net.uid").as_deref(), Some("aaaa"));
        // everything else is untouched
        for (k, v) in [("nvn.pool_mirror", "off"), ("nvn.view_scale", "2"), ("pkp.jump_momentum", "1"), ("pkp.camera_aim", "1"), ("backup.interval_minutes", "10"), ("backup.keep", "5"), ("boot.fast", "1"), ("title.skip", "1")] {
            assert_eq!(at(&after, k).as_deref(), Some(v), "{}", k);
        }
        // "other" reaches the sections no group names
        let after = Ini::parse(&reset_ini_groups(INI_GROUPS, &ids(&["other"])));
        assert_eq!(at(&after, "boot.fast"), None);
        assert_eq!(at(&after, "title.skip"), None);
        assert_eq!(at(&after, "nvn.pool_mirror").as_deref(), Some("off"));
        let after = Ini::parse(&reset_ini_groups(INI_GROUPS, &ids(&["gameplay"])));
        assert_eq!(at(&after, "pkp.jump_momentum"), None);
        assert_eq!(at(&after, "backup.keep").as_deref(), Some("5"));
    }

    #[test]
    fn the_paths_and_identity_survive_every_group_choice() {
        let mut choices: Vec<Vec<String>> = group_ids().into_iter().map(|g| vec![g]).collect();
        choices.push(group_ids());
        for groups in choices {
            let after = Ini::parse(&reset_ini_groups(INI_GROUPS, &groups));
            for (k, v) in [("fs.exefs", "/g/exefs"), ("fs.save_dir", "save"), ("net.uid", "aaaa"), ("account.nickname", "Koi"), ("account.uid", "bbbb"), ("nvn.shader_cache", "shader_cache/spirv"), ("nvn.dump_missing", "shader_cache/missing")] {
                assert_eq!(at(&after, k).as_deref(), Some(v), "{} with {:?}", k, groups);
            }
        }
    }

    #[test]
    fn an_unknown_group_is_refused_and_an_empty_choice_changes_nothing() {
        let (game, cfg) = install();
        fs::write(game.join("pkp.ini"), INI_GROUPS).unwrap();
        let before = snapshot(&game);
        let act = vec!["game_settings".to_string()];
        for bad in [ids(&[]), ids(&["bogus"]), ids(&["network", "../pkp.ini"])] {
            assert!(plan_with(&game, &cfg, &act, Some(&bad)).is_err(), "{:?}", bad);
            assert!(apply_with(&game, &cfg, &act, Some(&bad)).is_err(), "{:?}", bad);
        }
        assert_eq!(snapshot(&game), before);
        assert!(!game.join(BACKUP_DIR).exists());
    }

    #[test]
    fn every_ini_section_belongs_to_exactly_one_group() {
        let mut seen: Vec<&str> = Vec::new();
        for (_, sections) in GAME_GROUPS {
            for s in sections {
                assert!(!seen.contains(s), "section {} is in two groups", s);
                seen.push(s);
            }
        }
        let ids = group_ids();
        assert_eq!(ids.len(), GAME_GROUPS.len() + 1);
        assert!(ids.contains(&OTHER_GROUP.to_string()));
        let mut sorted = ids.clone();
        sorted.sort();
        sorted.dedup();
        assert_eq!(sorted.len(), ids.len(), "group ids must be unique");
        // every key of a real file lands in a known group; unnamed sections land in "other"
        for (key, _) in Ini::parse(INI_GROUPS).all() {
            assert!(ids.contains(&group_of(&key).to_string()), "{}", key);
        }
        assert_eq!(group_of("boot.fast"), OTHER_GROUP);
        assert_eq!(group_of("net.server"), "network");
        assert_eq!(group_of("nokey"), OTHER_GROUP);
    }

    #[test]
    fn a_partial_reset_is_undone_byte_for_byte() {
        let (game, cfg) = install();
        fs::write(game.join("pkp.ini"), INI_GROUPS).unwrap();
        let act = vec!["game_settings".to_string()];
        let report = apply_with(&game, &cfg, &act, Some(&ids(&["graphics", "gameplay"]))).unwrap();
        assert!(report.backup.is_some());
        let now = fs::read_to_string(game.join("pkp.ini")).unwrap();
        assert!(now.contains("server = x") && !now.contains("pool_mirror") && !now.contains("jump_momentum"));
        undo(&game, &cfg).unwrap();
        assert_eq!(fs::read_to_string(game.join("pkp.ini")).unwrap(), INI_GROUPS);
    }

    #[test]
    fn the_plan_names_the_chosen_groups() {
        let (game, cfg) = install();
        fs::write(game.join("pkp.ini"), INI_GROUPS).unwrap();
        let act = vec!["game_settings".to_string()];
        let partial = plan_with(&game, &cfg, &act, Some(&ids(&["clock", "network"]))).unwrap();
        assert_eq!(partial.changes.len(), 1);
        assert_eq!(partial.changes[0].groups, ids(&["network", "clock"]), "listed in the table's order");
        for full in [plan_with(&game, &cfg, &act, None).unwrap(), plan_with(&game, &cfg, &act, Some(&group_ids())).unwrap(), plan(&game, &cfg, &act).unwrap()] {
            assert!(full.changes[0].groups.is_empty(), "the full reset carries no group list");
        }
        // nothing to reset in the picked groups means no change at all
        fs::write(game.join("pkp.ini"), "[fs]\nexefs = /g\n[net]\nuid = a\n").unwrap();
        assert!(plan_with(&game, &cfg, &act, Some(&ids(&["clock"]))).unwrap().changes.is_empty());
    }
}
