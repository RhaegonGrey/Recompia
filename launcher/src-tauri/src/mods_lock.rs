// SPDX-License-Identifier: GPL-3.0-or-later
//! The mod the launcher keeps on (the repo owner's call, 2026-11-12): Community Achievements (achievements.rs `MOD_ID`) is installed from
//! the mod hub, switched on and kept on, and its `share` setting is kept on, each until the player unlocks it. Two launcher options hold
//! the locks (config.rs `achievements_unlocked`, `achievements_sharing_unlocked`; both off = locked, the default).
//!
//! - Keeping (`keep`): at every launcher start and when the player locks again. With the game set up (pkp.exe in the game folder) and the
//!   mod locked: a missing mod is installed through the Browse install path (mods_install.rs `put_in_place`: the hub's server only, size
//!   and SHA-256 checked) with the player's yes recorded for that one id (the launcher fetched it, as a Browse install), switched on and
//!   its sharing on, with no question (the owner's call). Installed but off: switched on, unless its code is not allowed (a file that did
//!   not come through the launcher stays the player's question). Sharing locked and off: on again. Offline or a failed download: nothing,
//!   the next start tries again; never an error, never in Play's way. Unlocked and removed stays removed ("deleted stays deleted").
//! - Guarding (`check_*`): while locked, the commands refuse to switch the mod off or remove it, and to switch sharing off; the pages
//!   show the rows locked (`mark`) with an Unlock action and its question.
//! - The game: the locks go into pkp.ini `[mods] locked` / `locked_settings` (runtime/docs/CONFIG.md; `mirror`), which the in-game Mods
//!   tab reads to show the rows locked. pkp.ini is only written when it exists (Set up makes it) and the value changed.

use serde::Serialize;
use std::path::Path;

use crate::achievements::MOD_ID;
use crate::config::Config;
use crate::ini::Ini;
use crate::mods::{self, ModView, ModsView};
use crate::mods_catalog::{self, Catalog};
use crate::settings::{self, PKP_INI};

/// The mod's sharing setting (its mod.toml: "Share my achievements with other players").
pub const SHARE: &str = "share";

/// What is locked now.
#[derive(Serialize, Clone, Copy, Debug, PartialEq, Eq)]
pub struct Locks {
    /// the mod: kept installed and on
    pub kept: bool,
    /// its sharing: kept on
    pub sharing: bool,
}

const MOD_LOCKED: &str = "Community Achievements is locked: unlock it first to switch it off or remove it.";
const SHARING_LOCKED: &str = "Sharing of Community Achievements is locked: unlock it first to switch it off.";

impl Locks {
    pub fn of(cfg: &Config) -> Locks {
        Locks { kept: !cfg.achievements_unlocked, sharing: !cfg.achievements_sharing_unlocked }
    }

    fn mod_locked(&self, id: &str) -> bool {
        self.kept && id == MOD_ID
    }

    fn setting_locked(&self, id: &str, key: &str) -> bool {
        self.sharing && id == MOD_ID && key == SHARE
    }

    /// A switch: off is refused while the mod is locked (on is always fine).
    pub fn check_switch(&self, id: &str, on: bool) -> Result<(), String> {
        if !on && self.mod_locked(id) {
            return Err(MOD_LOCKED.into());
        }
        Ok(())
    }

    pub fn check_remove(&self, id: &str) -> Result<(), String> {
        if self.mod_locked(id) {
            return Err(MOD_LOCKED.into());
        }
        Ok(())
    }

    /// A setting's new value: a locked setting takes only on (1).
    pub fn check_setting(&self, id: &str, key: &str, number: Option<f64>) -> Result<(), String> {
        if self.setting_locked(id, key) && number != Some(1.0) {
            return Err(SHARING_LOCKED.into());
        }
        Ok(())
    }

    /// The list with what is locked marked, for the pages.
    pub fn mark(&self, mut view: ModsView) -> ModsView {
        view.locks = Some(*self);
        for m in &mut view.mods {
            m.lockable = m.id == MOD_ID;
            m.locked = self.mod_locked(&m.id);
            for s in &mut m.settings {
                s.lockable = m.lockable && s.key == SHARE;
                s.locked = self.setting_locked(&m.id, &s.key);
            }
        }
        view
    }

    /// pkp.ini's `[mods] locked` (mod ids) and `locked_settings` (`id:key`), comma-separated, empty when nothing is.
    pub fn ini_values(&self) -> [(&'static str, String); 2] {
        let ids = if self.kept { MOD_ID.to_string() } else { String::new() };
        let keys = if self.sharing { format!("{MOD_ID}:{SHARE}") } else { String::new() };
        [("locked", ids), ("locked_settings", keys)]
    }
}

/// Puts the locks into pkp.ini for the game's Mods tab: only when pkp.ini exists (Set up makes it; a pkp.ini made early would read as a
/// finished setup) and a value differs.
pub fn mirror(game_dir: &Path, locks: Locks) -> Result<(), String> {
    let path = game_dir.join(PKP_INI);
    let Ok(text) = std::fs::read(&path) else { return Ok(()) };
    let mut ini = Ini::parse(&String::from_utf8_lossy(&text));
    let mut changed = false;
    for (key, value) in locks.ini_values() {
        if ini.get("mods", key).map(|v| v.trim().to_string()).unwrap_or_default() != value {
            ini.set("mods", key, &value);
            changed = true;
        }
    }
    if changed {
        settings::write_atomic(&path, &ini.to_text())?;
    }
    Ok(())
}

/// One thing `keep` does.
#[derive(Serialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Step {
    /// install from the hub, switch on, sharing on
    Install,
    SwitchOn,
    ShareOn,
}

/// The mod as `steps` looks at it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Have {
    pub enabled: bool,
    /// code the player has not allowed (a file that did not come through the launcher): never switched on without the player
    pub needs_yes: bool,
    pub share_off: bool,
}

impl Have {
    pub fn of(m: &ModView) -> Have {
        Have { enabled: m.enabled, needs_yes: m.needs_yes, share_off: m.settings.iter().any(|s| s.key == SHARE && s.number == 0.0) }
    }
}

/// What keeping the locks needs. `ready`: the game is set up (pkp.exe in the game folder). `have`: the mod, None when it is not installed.
pub fn steps(locks: Locks, ready: bool, have: Option<Have>) -> Vec<Step> {
    if !ready {
        return Vec::new();
    }
    let Some(h) = have else {
        return if locks.kept { vec![Step::Install] } else { Vec::new() };
    };
    let mut out = Vec::new();
    if locks.kept && !h.enabled && !h.needs_yes {
        out.push(Step::SwitchOn);
    }
    if locks.sharing && h.share_off {
        out.push(Step::ShareOn);
    }
    out
}

fn share_on(game_dir: &Path) -> Result<(), String> {
    mods::set_setting(game_dir, MOD_ID, SHARE, Some(1.0), None).map(|_| ())
}

/// Installs the hub's current package, records the yes for it, switches it on and sharing on. False: offline, not on the hub's list for
/// this game, or the download failed (logged; the next launcher start tries again).
fn install(game_dir: &Path, load: impl FnOnce() -> Result<(Catalog, bool), String>) -> Result<bool, String> {
    let (catalog, offline) = match load() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("mods: {MOD_ID} not installed now: {e}");
            return Ok(false);
        }
    };
    if offline || catalog.preview {
        eprintln!("mods: {MOD_ID} not installed now: the mod hub cannot be reached");
        return Ok(false);
    }
    let Some(entry) = catalog.mods.iter().find(|e| e.id == MOD_ID && mods_catalog::fits(e) && e.download.is_some()) else {
        eprintln!("mods: {MOD_ID} is not on the mod hub's list for this game");
        return Ok(false);
    };
    if let Err(e) = crate::mods_install::put_in_place(game_dir, entry, true) {
        eprintln!("mods: {MOD_ID} not installed now: {e}");
        return Ok(false);
    }
    mods::set_enabled(game_dir, MOD_ID, true, true)?;
    if mods::list(game_dir)?.mods.iter().find(|m| m.id == MOD_ID).is_some_and(|m| Have::of(m).share_off) {
        share_on(game_dir)?;
    }
    Ok(true)
}

/// Keeps the locks (see the top): returns what was done. `load` gives the hub's list (only asked for when the mod is missing).
pub fn keep(game_dir: &Path, locks: Locks, load: impl FnOnce() -> Result<(Catalog, bool), String>) -> Result<Vec<Step>, String> {
    let ready = game_dir.join("pkp.exe").is_file();
    if !ready {
        return Ok(Vec::new());
    }
    let have = mods::list(game_dir)?.mods.iter().find(|m| m.id == MOD_ID).map(Have::of);
    let (mut done, mut load) = (Vec::new(), Some(load));
    for step in steps(locks, ready, have) {
        let did = match step {
            Step::Install => install(game_dir, load.take().expect("one install"))?,
            Step::SwitchOn => mods::set_enabled(game_dir, MOD_ID, true, false).map(|_| true)?,
            Step::ShareOn => share_on(game_dir).map(|()| true)?,
        };
        if did {
            done.push(step);
        }
        if step == Step::Install {
            break;
        }
    }
    Ok(done)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mods::testkit::temp_game_dir;
    use std::fs;

    const LOCKED: Locks = Locks { kept: true, sharing: true };
    const OPEN: Locks = Locks { kept: false, sharing: false };
    const ON: Have = Have { enabled: true, needs_yes: false, share_off: false };

    #[test]
    fn a_missing_mod_is_installed_only_while_locked_and_set_up() {
        assert_eq!(steps(LOCKED, true, None), [Step::Install]);
        assert!(steps(Locks { kept: false, sharing: true }, true, None).is_empty(), "unlocked and removed stays removed");
        assert!(steps(LOCKED, false, None).is_empty(), "no game set up: nothing");
        assert!(steps(LOCKED, true, Some(ON)).is_empty(), "on and sharing: nothing to do");
    }

    #[test]
    fn a_locked_mod_is_switched_on_and_its_sharing_too() {
        let off = Have { enabled: false, share_off: true, ..ON };
        assert_eq!(steps(LOCKED, true, Some(off)), [Step::SwitchOn, Step::ShareOn]);
        assert_eq!(steps(Locks { kept: true, sharing: false }, true, Some(off)), [Step::SwitchOn]);
        assert_eq!(steps(Locks { kept: false, sharing: true }, true, Some(off)), [Step::ShareOn]);
        assert!(steps(OPEN, true, Some(off)).is_empty());
        let unallowed = Have { enabled: false, needs_yes: true, share_off: false };
        assert!(steps(LOCKED, true, Some(unallowed)).is_empty(), "code the player did not allow is never switched on");
    }

    #[test]
    fn locked_changes_are_refused() {
        assert!(LOCKED.check_switch(MOD_ID, false).is_err() && LOCKED.check_switch(MOD_ID, true).is_ok());
        assert!(LOCKED.check_remove(MOD_ID).is_err() && LOCKED.check_remove("other").is_ok());
        assert!(LOCKED.check_setting(MOD_ID, SHARE, Some(0.0)).is_err() && LOCKED.check_setting(MOD_ID, SHARE, Some(1.0)).is_ok());
        assert!(LOCKED.check_setting(MOD_ID, "messages", Some(0.0)).is_ok() && LOCKED.check_switch("other", false).is_ok());
        assert!(OPEN.check_switch(MOD_ID, false).is_ok() && OPEN.check_remove(MOD_ID).is_ok() && OPEN.check_setting(MOD_ID, SHARE, Some(0.0)).is_ok());
        let cfg = Config::default();
        assert_eq!(Locks::of(&cfg), LOCKED, "locked unless the player unlocks");
    }

    #[test]
    fn the_game_gets_the_locked_ids() {
        assert_eq!(LOCKED.ini_values(), [("locked", MOD_ID.to_string()), ("locked_settings", format!("{MOD_ID}:share"))]);
        assert_eq!(OPEN.ini_values(), [("locked", String::new()), ("locked_settings", String::new())]);
        let dir = temp_game_dir("lock-ini");
        fs::create_dir_all(&dir).unwrap();
        mirror(&dir, LOCKED).unwrap();
        assert!(!dir.join(PKP_INI).exists(), "no pkp.ini is made before Set up");
        fs::write(dir.join(PKP_INI), "[pkp]\r\njump_momentum = 1\r\n").unwrap();
        mirror(&dir, LOCKED).unwrap();
        let ini = settings::read_pkp(&dir);
        assert_eq!(ini.get("mods.locked").map(String::as_str), Some(MOD_ID));
        assert_eq!(ini.get("pkp.jump_momentum").map(String::as_str), Some("1"));
        mirror(&dir, OPEN).unwrap();
        assert_eq!(settings::read_pkp(&dir).get("mods.locked_settings").map(String::as_str), Some(""));
        let _ = fs::remove_dir_all(&dir);
    }

    /// A game folder with pkp.exe and, when `installed`, the mod as a folder with its two settings.
    fn game(name: &str, installed: bool) -> std::path::PathBuf {
        let dir = temp_game_dir(name);
        let mods_dir = mods::ensure_mods_dir(&dir).unwrap();
        fs::write(dir.join("pkp.exe"), b"MZ").unwrap();
        if installed {
            let m = mods_dir.join(MOD_ID);
            fs::create_dir_all(&m).unwrap();
            fs::write(
                m.join("mod.toml"),
                format!(
                    "id = \"{MOD_ID}\"\nversion = \"0.3.0\"\nname = \"Community Achievements\"\nauthors = [\"Test\"]\nlicense = \"MIT\"\n\n\
                     [[settings]]\nkey = \"messages\"\ntype = \"bool\"\ndefault = true\nlabel = \"Messages\"\n\n\
                     [[settings]]\nkey = \"share\"\ntype = \"bool\"\ndefault = true\nlabel = \"Share\"\n"
                ),
            )
            .unwrap();
        }
        dir
    }

    fn the_mod(dir: &Path) -> Option<ModView> {
        mods::list(dir).unwrap().mods.into_iter().find(|m| m.id == MOD_ID)
    }

    #[test]
    fn offline_the_first_start_installs_nothing_and_is_no_error() {
        let dir = game("lock-offline", false);
        let offline = || parse_empty().map(|c| (c, true));
        assert!(keep(&dir, LOCKED, offline).unwrap().is_empty());
        assert!(keep(&dir, LOCKED, || Err("The mod list could not be loaded.".to_string())).unwrap().is_empty());
        assert!(the_mod(&dir).is_none());
        let never = || -> Result<(Catalog, bool), String> { panic!("unlocked: the hub is not asked") };
        assert!(keep(&dir, OPEN, never).unwrap().is_empty(), "unlocked and missing: nothing");
        assert!(keep(&temp_game_dir("lock-no-game"), LOCKED, never).unwrap().is_empty(), "no game folder set up: nothing");
        let _ = fs::remove_dir_all(&dir);
    }

    fn parse_empty() -> Result<Catalog, String> {
        mods_catalog::parse(br#"{"schema": 1, "preview": false, "mods": []}"#)
    }

    #[test]
    fn keeping_switches_the_installed_mod_and_its_sharing_back_on() {
        let dir = game("lock-keep", true);
        mods::set_enabled(&dir, MOD_ID, false, false).unwrap();
        mods::set_setting(&dir, MOD_ID, SHARE, Some(0.0), None).unwrap();
        let never = || -> Result<(Catalog, bool), String> { panic!("an installed mod needs no hub") };
        assert_eq!(keep(&dir, LOCKED, never).unwrap(), [Step::SwitchOn, Step::ShareOn]);
        let m = the_mod(&dir).unwrap();
        assert!(m.enabled && !Have::of(&m).share_off);
        assert!(keep(&dir, LOCKED, never).unwrap().is_empty(), "nothing more to do");
        mods::set_enabled(&dir, MOD_ID, false, false).unwrap();
        assert!(keep(&dir, OPEN, never).unwrap().is_empty(), "unlocked: the player's off stays");
        let marked = LOCKED.mark(mods::list(&dir).unwrap());
        let m = marked.mods.iter().find(|m| m.id == MOD_ID).unwrap();
        assert!(m.locked && m.lockable && m.settings.iter().all(|s| s.locked == (s.key == SHARE) && s.lockable == s.locked));
        let open = OPEN.mark(mods::list(&dir).unwrap());
        let m = open.mods.iter().find(|m| m.id == MOD_ID).unwrap();
        assert!(!m.locked && m.lockable && m.settings.iter().all(|s| !s.locked && s.lockable == (s.key == SHARE)), "unlocked: Lock is offered");
        let _ = fs::remove_dir_all(&dir);
    }
}
