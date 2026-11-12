// SPDX-License-Identifier: GPL-3.0-or-later
//! The player's Modtopia mods in the game folder: the list the Mods page shows, its changes, and the hook plan Set up builds from.
//!
//! The Modtopia core owns the folders and files (contract 2: `<game>/mods/` with `modtopia.toml` and `.modtopia/`, `<game>/config/<id>.toml`,
//! `<game>/modtopia/mappings/`); this module reaches them only through it (modtopia_ffi.rs), never by editing them itself (the launcher's own
//! `mods/README.txt` is the one file it writes, `ensure_mods_dir`). Each call opens
//! a fresh scan, so a mod dropped into the folder by hand shows at the next look.
//!
//! Code the player has not allowed never runs: a code mod whose file is new or changed since the last yes (the core's MT_MOD_NEW_CODE) is
//! switched off at every scan, and switching it on again is the moment the page asks (mods_cmds.rs `mods_set_enabled` with `allow`).
//! After every change the plan is written again (`refresh_plan`): `<game>/mods/.modtopia/hooks.tsv`, or no file when no mod needs a hook
//! point. Set up builds those points in; `pkp_build.py --check` compares the plan's hash, and readiness.rs says when Set up must run again.

use serde::Serialize;
use std::fs;
use std::path::{Path, PathBuf};

use crate::gamebuild;
use crate::modtopia_ffi::{self as ffi, Core, ModInfo, SettingInfo};

include!(concat!(env!("OUT_DIR"), "/modtopia_mappings.rs"));

pub fn mods_dir(game_dir: &Path) -> PathBuf {
    game_dir.join("mods")
}

/// The one-screen help in `mods/`: what goes in, that code runs with the player's rights, where the switches are. English only; a plain
/// text file for someone who opens the folder in Explorer.
const README_NAME: &str = "README.txt";
const README: &str = concat!(
    "Put Modtopia mods here: .pkmod files, mod folders, or LayeredFS folders and zips.\r\n\r\n",
    "Use the launcher's Mods page to switch them on and off, put them in order, change their settings and find new ones. The game reads this folder each time it starts.\r\n\r\n",
    "A mod with code runs on this PC with your rights, like any program you install: only use mods from people you trust.\r\n",
);

/// The mods folder, made when missing, with README.txt when that is missing (never overwritten: the player may have changed or deleted it
/// on purpose, and a README that cannot be written does not stop the folder from being used).
pub fn ensure_mods_dir(game_dir: &Path) -> Result<PathBuf, String> {
    let dir = mods_dir(game_dir);
    fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    let readme = dir.join(README_NAME);
    if !readme.exists() {
        if let Err(e) = fs::write(&readme, README) {
            eprintln!("mods: {} not written: {}", readme.display(), e);
        }
    }
    Ok(dir)
}

fn mappings_dir(game_dir: &Path) -> PathBuf {
    game_dir.join("modtopia").join("mappings")
}

/// Writes the embedded mappings into `<game>/modtopia/mappings/`, each file only when its content differs (the runtime does the same).
fn write_mappings(game_dir: &Path) -> Result<(), String> {
    let dir = mappings_dir(game_dir);
    fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    for (name, bytes) in MAPPINGS {
        let path = dir.join(name);
        if fs::read(&path).ok().as_deref() != Some(*bytes) {
            crate::fsutil::write_atomic(&path, bytes).map_err(|_| format!("Cannot write {}.", path.display()))?;
        }
    }
    Ok(())
}

/// The host name mods see: "Recompia <version>".
fn host_name() -> String {
    format!("Recompia {}", env!("CARGO_PKG_VERSION"))
}

/// A scan of the game's mods, with code the player has not allowed switched off first (that needs a second scan: the plan and the load
/// order are made by the scan).
fn open(game_dir: &Path) -> Core {
    if let Err(e) = write_mappings(game_dir) {
        eprintln!("mods: {e}");
    }
    let (mods, maps, host) = (mods_dir(game_dir), mappings_dir(game_dir), host_name());
    let core = Core::open(&mods, &maps, &host);
    let unallowed: Vec<String> = core.mods().into_iter().filter(|m| m.enabled && needs_yes(m)).map(|m| m.id).collect();
    if unallowed.is_empty() {
        return core;
    }
    for id in &unallowed {
        if core.set_enabled(id, false).is_err() {
            eprintln!("mods: could not switch off {id}, whose code is not allowed yet");
        }
    }
    drop(core);
    Core::open(&mods, &maps, &host)
}

/// A code mod whose file the player has not said yes to (new, or changed since).
fn needs_yes(m: &ModInfo) -> bool {
    m.flags & ffi::FLAG_CODE != 0 && m.flags & ffi::FLAG_NEW_CODE != 0
}

/// Writes the hook plan of the mods that are on to `<game>/mods/.modtopia/hooks.tsv`, only when it changed (its time is part of Set up's
/// check), or removes it when no mod needs a point. Returns the plan's hash ("" for none).
fn write_plan(core: &Core, game_dir: &Path) -> Result<String, String> {
    let path = game_dir.join(gamebuild::MOD_PLAN);
    if core.plan_count() == 0 {
        match fs::remove_file(&path) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => return Err(format!("Cannot remove {}: {}", path.display(), e)),
        }
        return Ok(String::new());
    }
    let dir = path.parent().unwrap();
    fs::create_dir_all(dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    let tmp = crate::fsutil::temp_beside(&path);
    let written = core.plan_write(&tmp).map_err(|_| format!("Cannot write {}.", tmp.display()));
    let result = written.and_then(|()| {
        let new = fs::read(&tmp).map_err(|e| e.to_string())?;
        if fs::read(&path).ok() == Some(new) {
            return Ok(());
        }
        fs::rename(&tmp, &path).map_err(|e| format!("Cannot write {}: {}", path.display(), e))
    });
    let _ = fs::remove_file(&tmp);
    result.map(|()| core.plan_hash())
}

/// Scans the mods and writes their hook plan: before Play, before Set up and after every change on the Mods page.
pub fn refresh_plan(game_dir: &Path) -> Result<(), String> {
    write_plan(&open(game_dir), game_dir).map(|_| ())
}

/// One setting row of a mod.
#[derive(Serialize, Clone, Debug)]
pub struct SettingView {
    pub key: String,
    pub label: String,
    pub description: String,
    /// "bool", "number", "choice", "text" or "key"
    pub kind: &'static str,
    /// a change takes effect at the next start of the game
    pub restart: bool,
    pub min: f64,
    pub max: f64,
    pub step: f64,
    pub choices: Vec<String>,
    /// the value as the core shows it (On / Off, a number, the choice, the text)
    pub text: String,
    /// bool 0/1, the number, or the choice's index
    pub number: f64,
    /// kept on by the launcher until the player unlocks it (mods_lock.rs): it cannot be switched off
    pub locked: bool,
    /// one the launcher can lock: the page offers Unlock or Lock
    pub lockable: bool,
}

fn setting_view(s: SettingInfo) -> SettingView {
    let kind = match s.kind {
        ffi::SET_BOOL => "bool",
        ffi::SET_NUMBER => "number",
        ffi::SET_CHOICE => "choice",
        ffi::SET_KEY => "key",
        ffi::SET_TEXT => "text",
        _ => "text",
    };
    SettingView { key: s.key, label: s.label, description: s.description, kind, restart: s.restart, min: s.min, max: s.max, step: s.step, choices: s.choices, text: s.text, number: s.number, locked: false, lockable: false }
}

/// One installed mod, as the Mods page shows it.
#[derive(Serialize, Clone, Debug)]
pub struct ModView {
    pub id: String,
    pub name: String,
    pub version: String,
    pub authors: String,
    pub license: String,
    pub description: String,
    /// its file or folder in mods/
    pub source: String,
    /// changes gameplay (everyone in a Link Play session needs it) rather than looks only
    pub gameplay: bool,
    /// a LayeredFS folder or zip without a mod.toml: the core guessed `gameplay` from its files
    pub guessed: bool,
    pub code: bool,
    /// a code mod the player has not said yes to (new or changed): switching it on asks first
    pub needs_yes: bool,
    /// the hosts it may reach, "" when none
    pub network: String,
    /// the player's switch
    pub enabled: bool,
    /// "ok" (loads), "off" (not this time), "error" (broken)
    pub state: &'static str,
    /// why it is off or broken, or a note; "" when there is nothing to say
    pub message: String,
    pub hook_points: i32,
    pub settings: Vec<SettingView>,
    /// kept installed and on by the launcher until the player unlocks it (mods_lock.rs): it cannot be switched off or removed
    pub locked: bool,
    /// one the launcher can lock: the page offers Unlock or Lock
    pub lockable: bool,
}

/// The Mods page's list.
#[derive(Serialize, Clone, Debug)]
pub struct ModsView {
    /// the mods folder, for the page's "Open mods folder"
    pub dir: String,
    /// installed mods: those that load, in load order, then the rest
    pub mods: Vec<ModView>,
    /// "id@version" of every gameplay mod that is on (what the other Link Play players need too)
    pub gameplay: Vec<String>,
    /// hook points the mods that are on need
    pub points: i32,
    /// what the launcher locks now (mods_lock.rs `mark`; None until marked), so a page can offer Lock for a mod that is not installed
    pub locks: Option<crate::mods_lock::Locks>,
}

fn view_of(core: &Core, game_dir: &Path) -> ModsView {
    let mods = core
        .mods()
        .into_iter()
        .enumerate()
        .map(|(i, m)| ModView {
            gameplay: m.flags & ffi::FLAG_GAMEPLAY != 0,
            guessed: m.flags & ffi::FLAG_LAYEREDFS != 0 && m.flags & ffi::FLAG_KIND_GUESSED != 0,
            code: m.flags & ffi::FLAG_CODE != 0,
            needs_yes: needs_yes(&m),
            network: if m.flags & ffi::FLAG_NETWORK != 0 { m.network.clone() } else { String::new() },
            state: match m.state {
                ffi::STATE_OK => "ok",
                ffi::STATE_ERROR => "error",
                _ => "off",
            },
            settings: core.settings(i as i32).into_iter().map(setting_view).collect(),
            id: m.id,
            name: m.name,
            version: m.version,
            authors: m.authors,
            license: m.license,
            description: m.description,
            source: m.source,
            enabled: m.enabled,
            message: m.message,
            hook_points: m.hook_points,
            locked: false,
            lockable: false,
        })
        .collect();
    ModsView { dir: crate::dump::fwd(&mods_dir(game_dir)), mods, gameplay: core.gameplay_mods(), points: core.plan_count(), locks: None }
}

/// The installed mods, after writing their plan (a mod dropped in by hand changes it as much as a click does).
pub fn list(game_dir: &Path) -> Result<ModsView, String> {
    let core = open(game_dir);
    write_plan(&core, game_dir)?;
    Ok(view_of(&core, game_dir))
}

/// Opens a scan, applies `change` to it, then lists again with a fresh scan (the switches, the order and the plan are the scan's).
fn change(game_dir: &Path, change: impl FnOnce(&Core) -> Result<(), String>) -> Result<ModsView, String> {
    {
        let core = open(game_dir);
        change(&core)?;
    }
    list(game_dir)
}

fn known(core: &Core, id: &str) -> Result<i32, String> {
    core.find(id).ok_or_else(|| format!("There is no mod {} in the mods folder.", id))
}

const SAVE_FAILED: &str = "Could not save the mods' list in the mods folder.";

/// The player's switch. A code mod that is not allowed yet is switched on only with `allow` (the player said yes to the question).
pub fn set_enabled(game_dir: &Path, id: &str, on: bool, allow: bool) -> Result<ModsView, String> {
    change(game_dir, |core| {
        known(core, id)?;
        let m = core.mods().into_iter().find(|m| m.id == id).unwrap();
        if on && needs_yes(&m) {
            if !allow {
                return Err("This mod's code is not allowed yet.".into());
            }
            core.trust_mod(id).map_err(|_| format!("Could not record your yes for {}.", m.name))?;
        }
        core.set_enabled(id, on).map_err(|_| SAVE_FAILED.to_string())
    })
}

/// Where a mod is in the list the page shows: (its index, the ids of the list in order).
fn place(core: &Core, id: &str) -> Option<(usize, Vec<String>)> {
    let ids: Vec<String> = core.mods().into_iter().map(|m| m.id).collect();
    ids.iter().position(|m| m == id).map(|i| (i, ids))
}

/// One place up (-1) or down (+1) in the list as the page shows it. The core's order also holds mods that are off or broken, which the
/// list shows after the rest, so a step there may not move the mod past a visible neighbour: it steps on until it does, or reaches an
/// end of the order (then nothing more moves; a dependency can also hold a mod in place).
pub fn move_by(game_dir: &Path, id: &str, delta: i32) -> Result<ModsView, String> {
    let (start, ids) = place(&open(game_dir), id).ok_or_else(|| format!("There is no mod {} in the mods folder.", id))?;
    for _ in 0..ids.len() {
        let core = open(game_dir);
        match core.move_by(id, delta.signum()) {
            Ok(()) => {}
            Err(-4) => break, // MT_ERR_STATE: at an end of the order
            Err(_) => return Err(SAVE_FAILED.into()),
        }
        drop(core);
        if place(&open(game_dir), id).is_some_and(|(now, _)| now != start) {
            break;
        }
    }
    list(game_dir)
}

/// A new value for one of a mod's settings: a number (bools 0/1, choices by index) or a text, as the setting's kind takes it.
pub fn set_setting(game_dir: &Path, id: &str, key: &str, number: Option<f64>, text: Option<&str>) -> Result<ModsView, String> {
    change(game_dir, |core| {
        let m = known(core, id)?;
        let i = core.settings(m).iter().position(|s| s.key == key).ok_or_else(|| format!("{} has no setting {}.", id, key))? as i32;
        let r = match (number, text) {
            (Some(v), _) => core.set_number(m, i, v),
            (None, Some(t)) => core.set_text(m, i, t),
            (None, None) => return Err(format!("No value for {}.", key)),
        };
        r.map_err(|_| format!("Could not change {} of {}.", key, id))
    })
}

/// A mod's file or folder name as found in mods/: one plain name, never a path that leaves the folder.
fn plain_name(source: &str) -> bool {
    !source.is_empty() && source != "." && source != ".." && !source.starts_with('.') && !source.contains(['/', '\\', ':', '\0'])
}

/// Deletes a mod's file or folder from mods/ (the page asks first, like Settings > Profiles' Remove). Its settings in config/ stay, so
/// installing it again keeps them; the core clears its unpacked copy at the next scan.
pub fn remove(game_dir: &Path, id: &str) -> Result<ModsView, String> {
    let source = {
        let core = open(game_dir);
        known(&core, id)?;
        core.mods().into_iter().find(|m| m.id == id).unwrap().source
    };
    delete_source(game_dir, &source)?;
    list(game_dir)
}

/// Deletes one entry of mods/ by its name.
pub fn delete_source(game_dir: &Path, source: &str) -> Result<(), String> {
    if !plain_name(source) {
        return Err(format!("{} is not in the mods folder.", source));
    }
    let path = mods_dir(game_dir).join(source);
    let r = if path.is_dir() { fs::remove_dir_all(&path) } else { fs::remove_file(&path) };
    r.map_err(|e| format!("Could not delete {}: {}", path.display(), e))
}

/// The installed version of a mod and the name of its file in mods/, if it is there.
pub fn installed(game_dir: &Path, id: &str) -> Option<(String, String)> {
    let core = open(game_dir);
    core.mods().into_iter().find(|m| m.id == id).map(|m| (m.version, m.source))
}

/// Every installed mod's id and version (the catalog marks what is installed and what has a newer version).
pub fn installed_versions(game_dir: &Path) -> Vec<(String, String)> {
    open(game_dir).mods().into_iter().map(|m| (m.id, m.version)).collect()
}

/// One installed mod as the hub update looks at it.
#[derive(Clone, Debug, PartialEq)]
pub struct Installed {
    pub id: String,
    pub name: String,
    pub version: String,
    /// no code to run, or code the player has said yes to (its file is the one that was allowed)
    pub allowed: bool,
    /// the installed file has code (with `allowed`: the player said yes to code for this mod)
    pub code: bool,
}

/// Every installed mod with its version and whether its file is allowed.
pub fn installed_all(game_dir: &Path) -> Vec<Installed> {
    open(game_dir)
        .mods()
        .into_iter()
        .map(|m| Installed { allowed: !needs_yes(&m), code: m.flags & ffi::FLAG_CODE != 0, id: m.id, name: m.name, version: m.version })
        .collect()
}

/// Records the player's yes for the file now in mods/ under this id, when it has code that is not allowed yet (an install or update that
/// came through the hub's checked download). Opens the folder without `open`'s hold-back, which would switch the mod off first.
pub fn trust_file(game_dir: &Path, id: &str) -> Result<(), String> {
    let core = Core::open(&mods_dir(game_dir), &mappings_dir(game_dir), &host_name());
    let m = core.mods().into_iter().find(|m| m.id == id).ok_or_else(|| format!("There is no mod {} in the mods folder.", id))?;
    if !needs_yes(&m) {
        return Ok(());
    }
    core.trust_mod(id).map_err(|_| format!("Could not record your yes for {}.", m.name))
}

/// Test helpers: a game folder and .pkmod packages made in memory (stored zips), so the real core can scan them.
#[cfg(test)]
pub(crate) mod testkit {
    use std::path::PathBuf;

    pub fn temp_game_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("recompia-mods-{}-{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        dir
    }

    fn crc32(data: &[u8]) -> u32 {
        let mut crc = !0u32;
        for b in data {
            crc ^= *b as u32;
            for _ in 0..8 {
                crc = if crc & 1 != 0 { crc >> 1 ^ 0xEDB8_8320 } else { crc >> 1 };
            }
        }
        !crc
    }

    /// A zip with every file stored (no compression), which is all the core's reader needs for a test.
    fn zip_stored(files: &[(&str, &[u8])]) -> Vec<u8> {
        let (mut out, mut central) = (Vec::new(), Vec::new());
        let le = |v: &mut Vec<u8>, n: u64, bytes: usize| v.extend_from_slice(&n.to_le_bytes()[..bytes]);
        for (name, data) in files {
            let (crc, offset, len, nlen) = (crc32(data) as u64, out.len() as u64, data.len() as u64, name.len() as u64);
            for (n, w) in [(0x0403_4b50, 4), (20, 2), (0, 2), (0, 2), (0, 2), (0, 2), (crc, 4), (len, 4), (len, 4), (nlen, 2), (0, 2)] {
                le(&mut out, n, w);
            }
            out.extend_from_slice(name.as_bytes());
            out.extend_from_slice(data);
            for (n, w) in [(0x0201_4b50, 4), (20, 2), (20, 2), (0, 2), (0, 2), (0, 2), (0, 2), (crc, 4), (len, 4), (len, 4), (nlen, 2), (0, 2), (0, 2), (0, 2), (0, 2), (0, 4), (offset, 4)] {
                le(&mut central, n, w);
            }
            central.extend_from_slice(name.as_bytes());
        }
        let (cd_off, cd_size, count) = (out.len() as u64, central.len() as u64, files.len() as u64);
        out.extend_from_slice(&central);
        for (n, w) in [(0x0605_4b50, 4), (0, 2), (0, 2), (count, 2), (count, 2), (cd_size, 4), (cd_off, 4), (0, 2)] {
            le(&mut out, n, w);
        }
        out
    }

    /// A .pkmod: a mod.toml with this id and version, and when `code` a Windows library (the core only looks at it, nothing runs).
    pub fn pkmod(id: &str, version: &str, code: bool) -> Vec<u8> {
        let toml = format!("id = \"{id}\"\nversion = \"{version}\"\nname = \"{id}\"\nauthors = [\"Test\"]\nlicense = \"MIT\"\n");
        let mut files: Vec<(&str, &[u8])> = vec![("mod.toml", toml.as_bytes())];
        if code {
            files.push(("bin/windows-x64/mod.dll", b"MZ"));
        }
        zip_stored(&files)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_plain_names_are_deleted() {
        for ok in ["camera.pkmod", "my mod", "a-b_c.zip"] {
            assert!(plain_name(ok), "{ok}");
        }
        for bad in ["", ".", "..", ".modtopia", "a/b", "a\\b", "C:x", "../x"] {
            assert!(!plain_name(bad), "{bad:?}");
        }
    }

    #[test]
    fn an_install_records_the_yes_for_that_one_mod_only() {
        use testkit::{pkmod, temp_game_dir};
        let dir = temp_game_dir("trust");
        let mods = ensure_mods_dir(&dir).unwrap();
        for (file, id, code) in [("cam-1.0.0.pkmod", "cam", true), ("cam2-1.0.0.pkmod", "cam2", true), ("look-1.0.0.pkmod", "look", false)] {
            fs::write(mods.join(file), pkmod(id, "1.0.0", code)).unwrap();
        }
        let ask = |dir: &Path, id: &str| list(dir).unwrap().mods.into_iter().find(|m| m.id == id).unwrap().needs_yes;
        assert!(ask(&dir, "cam") && ask(&dir, "cam2") && !ask(&dir, "look"), "code is new until the player said yes; data has none to ask for");
        trust_file(&dir, "cam").unwrap();
        assert!(!ask(&dir, "cam") && ask(&dir, "cam2"), "the yes is for cam, not for the code beside it");
        trust_file(&dir, "look").unwrap();
        let known = fs::read_to_string(mods.join(".modtopia").join("known-code.toml")).unwrap();
        assert!(known.contains("cam") && !known.contains("cam2") && !known.contains("look"), "{known}");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_mods_folder_gets_a_readme_that_is_never_overwritten() {
        let dir = std::env::temp_dir().join(format!("recompia-mods-readme-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let readme = ensure_mods_dir(&dir).unwrap().join(README_NAME);
        let first = fs::read_to_string(&readme).unwrap();
        assert!(first.contains("Mods page") && first.contains("your rights") && first.contains("\r\n"), "{first:?}");
        fs::write(&readme, "mine").unwrap();
        ensure_mods_dir(&dir).unwrap();
        assert_eq!(fs::read_to_string(&readme).unwrap(), "mine", "an existing README is left as it is");
        fs::remove_file(&readme).unwrap();
        ensure_mods_dir(&dir).unwrap();
        assert_eq!(fs::read_to_string(&readme).unwrap(), first, "a missing one is written again");
        assert!(list(&dir).unwrap().mods.is_empty(), "the README is not a mod");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn an_empty_mods_folder_has_no_plan() {
        let dir = std::env::temp_dir().join(format!("recompia-mods-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(mods_dir(&dir)).unwrap();
        let v = list(&dir).unwrap();
        assert!(v.mods.is_empty() && v.points == 0);
        assert!(!dir.join(gamebuild::MOD_PLAN).exists());
        // the mappings went into the game folder
        assert!(MAPPINGS.iter().all(|(n, b)| fs::read(mappings_dir(&dir).join(n)).ok().as_deref() == Some(*b)));
        let _ = fs::remove_dir_all(&dir);
    }
}
