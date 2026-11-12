// SPDX-License-Identifier: GPL-3.0-or-later
//! Installing a mod from the hub's list, after the player's one yes (the page asks): the package is downloaded from the hub's download
//! server only (mods_catalog.rs `download_ok`; no redirect is followed), checked against the size and SHA-256 the list gives (a mismatch
//! stops the install), written as `<file>.part` in mods/ and renamed (download.rs), and nothing in it runs. Then the yes is recorded for
//! that exact file (the core's `mt_loader_trust_mod`: a later change to it asks again). `put_in_place` is that shared path: the page's Install
//! then switches the mod on; the update by itself (mods_update.rs) leaves the switch and the order as they were. A newer version replaces the
//! older file.

use std::path::Path;
use std::sync::atomic::AtomicBool;

use crate::download::{self, Pinned};
use crate::mods::{self, ModsView};
use crate::mods_catalog::{self, CatalogStore, Entry};

/// The one install path: downloads the entry's package (the hub's server only, size and SHA-256 checked, never half a file in mods/),
/// removes an older version's file, and, when `trust` (the player said yes to this mod's code), records the yes for the new file when it
/// has code. The mod's switch and place are the core's (by id), so they stay as they were. Nothing is switched on here.
pub fn put_in_place(game_dir: &Path, entry: &Entry, trust: bool) -> Result<(), String> {
    let dl = entry.download.as_ref().ok_or_else(|| format!("{} is not out yet.", entry.name))?;
    if !mods_catalog::download_ok(dl) {
        return Err(format!("{} cannot be installed: its download is not on the mod hub's server.", entry.name));
    }
    let dir = mods::ensure_mods_dir(game_dir)?;
    let before = mods::installed(game_dir, &entry.id);
    let pin = Pinned { file: &dl.file, url: &dl.url, size: dl.size, sha256: &dl.sha256 };
    download::fetch(&download::agent(false), &pin, &dir, &AtomicBool::new(false), &mut |_| {})?;
    drop_older(game_dir, before, &dl.file)?;
    if trust {
        mods::trust_file(game_dir, &entry.id)?;
    }
    Ok(())
}

/// The older version's file goes once the new one is in place (two copies of one id: the core loads only one). `before` is what the core
/// found for the id before the download: its file is found by the id inside it, never by its name, and is kept when it is the new file.
fn drop_older(game_dir: &Path, before: Option<(String, String)>, new_file: &str) -> Result<(), String> {
    match before {
        Some((_, source)) if source != new_file => mods::delete_source(game_dir, &source),
        _ => Ok(()),
    }
}

pub fn install(game_dir: &Path, store: &CatalogStore, cache: &Path, id: &str) -> Result<ModsView, String> {
    let (catalog, offline) = mods_catalog::load(store, cache, false)?;
    if offline {
        return Err("Installing needs the mod hub, which cannot be reached now. Check the internet connection and try again.".into());
    }
    if catalog.preview {
        return Err("The mod hub is not open for installs yet.".into());
    }
    let entry = catalog.mods.iter().find(|e| e.id == id && mods_catalog::fits(e)).ok_or_else(|| format!("{} is not in the mod list for this game.", id))?;
    // the page asked the install question (code runs with the player's rights) before this call
    put_in_place(game_dir, entry, true)?;
    mods::set_enabled(game_dir, id, true, true)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mods::testkit::{pkmod, temp_game_dir};
    use std::fs;

    fn pkmods(game_dir: &Path) -> Vec<String> {
        let mut v: Vec<String> = fs::read_dir(mods::mods_dir(game_dir)).unwrap().filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).filter(|n| n.ends_with(".pkmod")).collect();
        v.sort();
        v
    }

    #[test]
    fn an_update_replaces_the_older_file_by_the_id_inside_it_not_its_name() {
        let dir = temp_game_dir("update");
        let mods_dir = mods::ensure_mods_dir(&dir).unwrap();
        // the older version is under a name that says nothing about its id; a mod whose id starts the same and a broken package sit beside it
        fs::write(mods_dir.join("my-copy.pkmod"), pkmod("auto_run", "1.0.0", false)).unwrap();
        fs::write(mods_dir.join("auto_run_plus-1.0.0.pkmod"), pkmod("auto_run_plus", "1.0.0", false)).unwrap();
        fs::write(mods_dir.join("broken.pkmod"), b"not a zip").unwrap();
        let before = mods::installed(&dir, "auto_run");
        assert_eq!(before, Some(("1.0.0".into(), "my-copy.pkmod".into())));
        fs::write(mods_dir.join("auto_run-1.1.0.pkmod"), pkmod("auto_run", "1.1.0", false)).unwrap();
        drop_older(&dir, before, "auto_run-1.1.0.pkmod").unwrap();
        assert_eq!(pkmods(&dir), ["auto_run-1.1.0.pkmod", "auto_run_plus-1.0.0.pkmod", "broken.pkmod"]);
        // a new package that replaced the older file under the same name is not deleted with it
        drop_older(&dir, Some(("1.1.0".into(), "auto_run-1.1.0.pkmod".into())), "auto_run-1.1.0.pkmod").unwrap();
        assert_eq!(pkmods(&dir).len(), 3);
        drop_older(&dir, None, "x.pkmod").unwrap();
        let _ = fs::remove_dir_all(&dir);
    }
}
