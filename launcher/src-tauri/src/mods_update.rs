// SPDX-License-Identifier: GPL-3.0-or-later
//! "Update mods by themselves" (launcher.json `mods_auto_update`, off unless the player switches it on): once per launcher session the
//! installed mods that the mod hub's list has a newer version of are replaced by it, with no question. The hub's list is the session copy
//! Browse uses (mods_catalog.rs `load`, the kept copy offline); a download is taken only from the hub's server with the size and SHA-256
//! the list gives (mods_install.rs `put_in_place`, the same path as the page's Install). The player's earlier yes carries over: only a mod
//! whose installed file is allowed (or has no code) is updated, and its new file is allowed after the checks only when the old file had
//! code the player allowed. A data-only mod whose new version brings code was never a yes to code: that code waits for the player (the
//! hold-back of mods.rs `open`, the page asks when it is switched on). A mod whose file was dropped in by hand or changed since is left
//! alone. The mod's switch and place stay as they were (the core keeps both by id).
//! Each mod is its own step: one that fails is logged and the others go on.

use serde::Serialize;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::modtopia_ffi as ffi;
use crate::mods::{self, Installed};
use crate::mods_catalog::{self, Catalog, CatalogStore, Entry};
use crate::mods_install;

/// Whether this launcher session already ran the update (a second call does nothing).
#[derive(Default)]
pub struct Session(AtomicBool);

impl Session {
    /// True for the first caller of the session only.
    pub fn claim(&self) -> bool {
        !self.0.swap(true, Ordering::SeqCst)
    }
}

/// One mod that was updated, for the player's one line.
#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Updated {
    pub name: String,
    pub from: String,
    pub to: String,
}

/// The list's entries to install over what is installed: a version newer than the installed one, made for this game and core, with a
/// download the launcher takes, for a mod whose installed file is allowed or data-only. Nothing while the hub is a preview or the list
/// is the kept copy (offline).
pub fn plan<'a>(catalog: &'a Catalog, offline: bool, installed: &'a [Installed]) -> Vec<(&'a Entry, &'a Installed)> {
    if offline || catalog.preview {
        return Vec::new();
    }
    let mut out = Vec::new();
    for entry in &catalog.mods {
        let Some(have) = installed.iter().find(|m| m.id == entry.id) else { continue };
        let takes = entry.download.as_ref().is_some_and(mods_catalog::download_ok);
        if have.allowed && takes && mods_catalog::fits(entry) && ffi::version_compare(&entry.version, &have.version) == std::cmp::Ordering::Greater {
            out.push((entry, have));
        }
    }
    out
}

/// Updates what `plan` names, then writes the hook plan once (a new version may need other points). Returns what was updated.
pub fn run(game_dir: &Path, store: &CatalogStore, cache: &Path) -> Result<Vec<Updated>, String> {
    let (catalog, offline) = mods_catalog::load(store, cache, false)?;
    let installed = mods::installed_all(game_dir);
    let mut done = Vec::new();
    for (entry, have) in plan(&catalog, offline, &installed) {
        match mods_install::put_in_place(game_dir, entry, have.code) {
            Ok(()) => done.push(Updated { name: have.name.clone(), from: have.version.clone(), to: entry.version.clone() }),
            Err(e) => eprintln!("mods: update of {} to {} failed: {e}", entry.id, entry.version),
        }
    }
    if !done.is_empty() {
        mods::refresh_plan(game_dir)?;
    }
    Ok(done)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mods_catalog::{parse, DOWNLOAD_HOST};

    fn entry(id: &str, version: &str, extra: &str) -> String {
        format!(
            r#"{{"id": "{id}", "name": "{id}", "version": "{version}", "game": ["2.0.1"], "needs": ">=0.3.0 <1.0.0", {extra}
             "download": {{"url": "{DOWNLOAD_HOST}mods/{id}/{version}/{id}-{version}.pkmod", "file": "{id}-{version}.pkmod", "size": 10, "sha256": "{}"}}}}"#,
            "a".repeat(64)
        )
    }

    fn list(preview: bool, mods: &[String]) -> Catalog {
        parse(format!(r#"{{"schema": 1, "preview": {preview}, "mods": [{}]}}"#, mods.join(",")).as_bytes()).unwrap()
    }

    fn have(id: &str, version: &str, allowed: bool) -> Installed {
        Installed { id: id.into(), name: id.into(), version: version.into(), allowed, code: true }
    }

    fn ids(c: &Catalog, offline: bool, installed: &[Installed]) -> Vec<String> {
        plan(c, offline, installed).iter().map(|(e, _)| e.id.clone()).collect()
    }

    #[test]
    fn only_a_newer_version_of_an_installed_mod_is_updated() {
        let c = list(false, &[entry("a", "1.1.0", ""), entry("b", "1.0.0", ""), entry("c", "0.9.0", ""), entry("d", "2.0.0", "")]);
        let installed = [have("a", "1.0.0", true), have("b", "1.0.0", true), have("c", "1.0.0", true)];
        assert_eq!(ids(&c, false, &installed), ["a"], "same or older version, and a mod that is not installed, stay as they are");
        assert_eq!(plan(&c, false, &installed)[0].1.version, "1.0.0");
    }

    #[test]
    fn a_file_the_player_did_not_allow_is_never_updated() {
        let c = list(false, &[entry("a", "1.1.0", ""), entry("b", "1.1.0", "")]);
        assert_eq!(ids(&c, false, &[have("a", "1.0.0", false), have("b", "1.0.0", true)]), ["b"]);
    }

    #[test]
    fn nothing_while_offline_or_in_preview() {
        let installed = [have("a", "1.0.0", true)];
        assert!(ids(&list(false, &[entry("a", "1.1.0", "")]), true, &installed).is_empty());
        assert!(ids(&list(true, &[entry("a", "1.1.0", "")]), false, &installed).is_empty());
    }

    #[test]
    fn an_entry_that_does_not_fit_or_cannot_be_downloaded_is_skipped() {
        let other_game = entry("a", "1.1.0", "").replace(r#""game": ["2.0.1"]"#, r#""game": ["1.0.5"]"#);
        let too_new = entry("b", "1.1.0", "").replace(">=0.3.0 <1.0.0", ">=0.7.0");
        let foreign = entry("c", "1.1.0", "").replace(DOWNLOAD_HOST, "https://example.invalid/");
        let soon = r#"{"id": "d", "name": "d", "version": "1.1.0", "game": ["2.0.1"], "needs": ">=0.3.0 <1.0.0", "download": null}"#.to_string();
        let c = list(false, &[other_game, too_new, foreign, soon]);
        let installed: Vec<Installed> = ["a", "b", "c", "d"].iter().map(|id| have(id, "1.0.0", true)).collect();
        assert!(ids(&c, false, &installed).is_empty());
    }

    #[test]
    fn the_session_runs_once() {
        let s = Session::default();
        assert!(s.claim());
        assert!(!s.claim());
    }
}
