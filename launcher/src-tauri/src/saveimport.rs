// SPDX-License-Identifier: GPL-3.0-or-later
// Save import: brings a Pokopia save from Eden, Ryujinx or any folder holding the game's save layout into the save the runtime plays.
// The finder and the layout test follow pokopia-native (MIT), nxs2.py eden_saves() / import_save() @7c3655d; the Ryujinx scan, the
// committed-volume placement and the staging/rollback are new.
//
// The runtime keeps an account save at <save_dir>/account/<uid bytes as 32 hex>/ (the COMMITTED folder of a journalled volume,
// runtime/src/services/fs_volume.cpp) with <same>.meta beside it (size/journal/owner/attr, services/fs.cpp). A running game keeps
// work folders (<root>.work.<pid>), journals (<root>.journal.<pid>) and, for a commit in flight, <root>.commit next to it.
// Import: stage a copy of B00/G00/S00 beside the root, move the root and its leftovers to <save_dir>.before-<time>/ (never deleted),
// rename the staged copy into place. The folder is then a plain committed state with no marker: the runtime opens it as is.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use serde::Serialize;

/// Program id of the game (little-endian in the save attribute).
const PROGRAM_ID: u64 = 0x0400_A3A0_1EE4_C000;
const TITLE_DIR: &str = "0400A3A01EE4C000";
/// The runtime's built-in local user (games/pkp/core/pkp_core.cpp): [account] uid is hi then lo; the save folder is each u64 little-endian.
const DEFAULT_UID: &str = "000000000000000141434e4852656370";
const SAVE_PARTS: [&str; 3] = ["B00", "G00", "S00"];
/// Size and journal size the runtime gives the account save (games/pkp/core/pkp_core.cpp account_save_size).
const ACCOUNT_SAVE_SIZE: u64 = 0x400_0000;

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Source {
    /// "Eden", "Ryujinx"
    pub label: String,
    pub path: String,
}

#[derive(Serialize, Clone, Debug)]
pub struct Report {
    pub source: String,
    pub files: u64,
    /// Where the previous save went (None when there was none).
    pub backup: Option<String>,
    pub target: String,
}

fn fwd(p: &Path) -> String {
    p.to_string_lossy().replace('\\', "/")
}

/// A folder is a Pokopia save when it holds the game's G00 and S00 folders (nxs2.py import_save).
pub fn is_save_folder(d: &Path) -> bool {
    d.join("G00").is_dir() && d.join("S00").is_dir()
}

/// The game's own file names, to tell its save from another title's in an emulator's folders (nxs2.py eden_saves).
fn has_game_files(d: &Path) -> bool {
    d.join("G00").join("CMNDAT.BIN").is_file() && d.join("S00").join("SYDAT.BIN").is_file()
}

/// A picked folder, or the save/ folder inside it.
pub fn resolve_picked(p: &Path) -> Option<PathBuf> {
    if is_save_folder(p) {
        Some(p.to_path_buf())
    } else if is_save_folder(&p.join("save")) {
        Some(p.join("save"))
    } else {
        None
    }
}

fn newest_mtime(d: &Path) -> SystemTime {
    let mut best = SystemTime::UNIX_EPOCH;
    let mut stack = vec![d.to_path_buf()];
    while let Some(x) = stack.pop() {
        let Ok(rd) = fs::read_dir(&x) else { continue };
        for e in rd.flatten() {
            let Ok(md) = e.metadata() else { continue };
            if md.is_dir() {
                stack.push(e.path());
            } else if let Ok(m) = md.modified() {
                best = best.max(m);
            }
        }
    }
    best
}

fn subdirs(d: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = fs::read_dir(d).map(|rd| rd.flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect()).unwrap_or_default();
    v.sort();
    v
}

/// Eden's saves of the game under <data>/nand/user/save/0000000000000000/<user>/<title id> (or the title-0 folder it uses when it cannot
/// read the title id from an extracted dump). Only read.
pub fn eden_saves(data: &Path) -> Vec<PathBuf> {
    let root = data.join("nand").join("user").join("save").join("0000000000000000");
    let mut out = vec![];
    for user in subdirs(&root) {
        for name in [TITLE_DIR, "0000000000000000"] {
            let d = user.join(name);
            if has_game_files(&d) {
                out.push(d);
            }
        }
    }
    out
}

/// Ryujinx keeps a save at <data>/bis/user/save/<save id>/0 (and a second copy in 1). The save id comes from its imkvdb.arc index, which
/// is not parsed here: every save folder is tested for the game's own files instead (CMNDAT.BIN and SYDAT.BIN do not occur in other titles).
pub fn ryujinx_saves(data: &Path) -> Vec<PathBuf> {
    let root = data.join("bis").join("user").join("save");
    let mut out = vec![];
    for id in subdirs(&root) {
        for slot in ["0", "1"] {
            let d = id.join(slot);
            if has_game_files(&d) {
                out.push(d);
            }
        }
    }
    out
}

/// Everything found automatically, newest first.
pub fn find_sources(appdata: &Path, eden_dir: Option<&Path>, ryujinx_dir: Option<&Path>) -> Vec<Source> {
    let eden = eden_dir.map(Path::to_path_buf).unwrap_or_else(|| appdata.join("eden"));
    let ryu = ryujinx_dir.map(Path::to_path_buf).unwrap_or_else(|| appdata.join("Ryujinx"));
    let mut all: Vec<(SystemTime, Source)> = vec![];
    for (label, list) in [("Eden", eden_saves(&eden)), ("Ryujinx", ryujinx_saves(&ryu))] {
        for d in list {
            all.push((newest_mtime(&d), Source { label: label.into(), path: fwd(&d) }));
        }
    }
    all.sort_by(|a, b| b.0.cmp(&a.0));
    all.into_iter().map(|(_, s)| s).collect()
}

pub fn find_sources_here() -> Vec<Source> {
    let env_dir = |k: &str| std::env::var_os(k).filter(|v| !v.is_empty()).map(PathBuf::from);
    #[cfg(windows)]
    let (appdata, eden, ryujinx) = (PathBuf::from(std::env::var_os("APPDATA").unwrap_or_default()), env_dir("EDEN_DIR"), env_dir("RYUJINX_DIR"));
    // Linux: Eden keeps its data in $XDG_DATA_HOME/eden, Ryujinx in $XDG_CONFIG_HOME/Ryujinx
    #[cfg(not(windows))]
    let (appdata, eden, ryujinx) = {
        let home = PathBuf::from(std::env::var_os("HOME").unwrap_or_default());
        let data = env_dir("XDG_DATA_HOME").unwrap_or_else(|| home.join(".local").join("share"));
        let config = env_dir("XDG_CONFIG_HOME").unwrap_or_else(|| home.join(".config"));
        (PathBuf::new(), env_dir("EDEN_DIR").or(Some(data.join("eden"))), env_dir("RYUJINX_DIR").or(Some(config.join("Ryujinx"))))
    };
    find_sources(&appdata, eden.as_deref(), ryujinx.as_deref())
}

// ---- where the runtime keeps the save ------------------------------------------------------------------------------------------------

/// NXRT_FS_SAVE_DIR, which overrides [fs] save_dir in the runtime and so here too; empty when unset. The tests never see it: they
/// work in folders of their own, and following the variable would point them at the real save of the PC they run on.
pub fn env_save_dir() -> String {
    if cfg!(test) {
        return String::new();
    }
    std::env::var("NXRT_FS_SAVE_DIR").unwrap_or_default()
}

/// The runtime's [fs] save_dir: relative to the game folder; empty is the launcher's default `save`.
pub fn save_dir_of(game_dir: &Path, ini_value: &str) -> PathBuf {
    let v = ini_value.trim();
    if v.is_empty() {
        return game_dir.join("save");
    }
    let p = Path::new(v);
    if p.is_absolute() {
        p.to_path_buf()
    } else {
        game_dir.join(p)
    }
}

/// [account] uid (32 hex digits: hi then lo) to the folder name the runtime uses: each u64 as 8 little-endian bytes in hex.
pub fn uid_key(uid_hex: &str) -> Option<String> {
    let h = uid_hex.trim();
    if h.len() != 32 || !h.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    let le = |s: &str| -> String {
        let v = u64::from_str_radix(s, 16).unwrap_or(0);
        v.to_le_bytes().iter().map(|b| format!("{:02x}", b)).collect()
    };
    Some(format!("{}{}", le(&h[..16]), le(&h[16..])))
}

fn app_hex() -> String {
    PROGRAM_ID.to_le_bytes().iter().map(|b| format!("{:02x}", b)).collect()
}

fn meta_attr_matches(meta: &Path) -> bool {
    let Ok(text) = fs::read_to_string(meta) else { return false };
    let Some(attr) = text.lines().find_map(|l| l.strip_prefix("attr=")) else { return false };
    // attr: application id (8 bytes), uid (16), system id (8), type (1 byte at 32; 1 = account)
    attr.len() >= 66 && attr.get(..16).is_some_and(|a| a.eq_ignore_ascii_case(&app_hex())) && attr.get(64..66) == Some("01")   // get(): a non-ASCII byte there is no match, not a panic
}

/// The runtime's save folder and the account folder the game uses for the active profile, from pkp.ini (settings::read_pkp, the
/// runtime's own rule for repeated keys). Import, backup, restore and rollback all ask here.
pub fn locate(game_dir: &Path) -> (PathBuf, String) {
    let pkp = crate::settings::read_pkp(game_dir);
    let get = |k: &str| pkp.get(k).cloned().unwrap_or_default();
    let env = env_save_dir();
    let sd = save_dir_of(game_dir, &if env.trim().is_empty() { get("fs.save_dir") } else { env });
    // with profiles, a profile other than the first plays its own account folder, not the newest one
    let key = crate::profiles::strict_save_key(game_dir).unwrap_or_else(|| target_key(&sd, &get("account.uid")));
    (sd, key)
}

/// The account save folder name to import into: the game's existing one (its .meta names the game and the account type), else the
/// folder of [account] uid, else the runtime's built-in user.
pub fn target_key(save_dir: &Path, ini_uid: &str) -> String {
    let acc = save_dir.join("account");
    let mut metas: Vec<PathBuf> = fs::read_dir(&acc)
        .map(|rd| rd.flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|x| x == "meta") && meta_attr_matches(p)).collect())
        .unwrap_or_default();
    metas.sort_by_key(|m| std::cmp::Reverse(fs::metadata(m).and_then(|x| x.modified()).ok()));
    if let Some(stem) = metas.first().and_then(|m| m.file_stem()).and_then(|s| s.to_str()) {
        return stem.to_string();
    }
    uid_key(ini_uid).or_else(|| uid_key(DEFAULT_UID)).unwrap()
}

fn meta_text(key: &str) -> String {
    // attr = app id, uid, system id 0, type 1, rank 0, index 0, padding: 64 bytes, as the runtime's create_save writes it
    let attr = format!("{}{}{}01{}", app_hex(), key, "00".repeat(8), "00".repeat(31));
    format!("size={0}\njournal={0}\nowner={1:x}\nattr={2}\n", ACCOUNT_SAVE_SIZE, PROGRAM_ID, attr)
}

// ---- the import ----------------------------------------------------------------------------------------------------------------------

fn copy_tree(src: &Path, dst: &Path, n: &mut u64) -> io::Result<()> {
    fs::create_dir_all(dst)?;
    for e in fs::read_dir(src)? {
        let e = e?;
        let t = e.file_type()?;
        let to = dst.join(e.file_name());
        if t.is_dir() {
            copy_tree(&e.path(), &to, n)?;
        } else if t.is_file() {
            fs::copy(e.path(), &to)?;
            *n += 1;
        }
    }
    Ok(())
}

fn put_back(old: &[PathBuf], bacc: &Path) {
    for p in old {
        let back = bacc.join(p.file_name().unwrap());
        if back.exists() && !p.exists() {
            let _ = fs::rename(&back, p);
        }
    }
}

/// Brings `src` (a folder holding G00 and S00, optionally B00) in as the game's committed account save. The source is only read. The
/// caller has checked that the game is not running.
pub fn import(save_dir: &Path, key: &str, src: &Path) -> Result<Report, String> {
    if !is_save_folder(src) {
        return Err(format!("{} holds no Pokopia save (no G00 and S00 folders).", fwd(src)));
    }
    let acc = save_dir.join("account");
    let root = acc.join(key);
    let canon = |p: &Path| fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf());
    if canon(src).starts_with(canon(save_dir)) {
        return Err(format!("{} is inside the save folder the game plays. Pick a copy outside it.", fwd(src)));
    }
    fs::create_dir_all(&acc).map_err(|e| format!("Cannot create {}: {}", fwd(&acc), e))?;
    let t = crate::fsutil::file_stamp();

    // 1. stage the copy beside the root; a failure here changes nothing
    let stage = acc.join(format!("{}.import-{}", key, t));
    let _ = fs::remove_dir_all(&stage);
    let mut files = 0u64;
    let staged = (|| -> io::Result<()> {
        fs::create_dir_all(&stage)?;
        for part in SAVE_PARTS {
            if src.join(part).is_dir() {
                copy_tree(&src.join(part), &stage.join(part), &mut files)?;
            }
        }
        Ok(())
    })();
    if let Err(e) = staged {
        let _ = fs::remove_dir_all(&stage);
        return Err(format!("Copying the save failed ({}). Your current save was not touched.", e));
    }
    if files == 0 {
        let _ = fs::remove_dir_all(&stage);
        return Err("The folder holds no save files.".into());
    }

    // 2. move the old save and its commit leftovers (.work.<pid>, .journal.<pid>, .commit) aside; never deleted
    let mut old: Vec<PathBuf> = vec![];
    let stem = format!("{}.", key);
    if let Ok(rd) = fs::read_dir(&acc) {
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().to_string();
            if ["work.", "journal."].iter().any(|k| name.strip_prefix(&stem).is_some_and(|r| r.starts_with(k))) || name == format!("{}commit", stem) {
                old.push(e.path());
            }
        }
    }
    if root.exists() {
        old.push(root.clone());
    }
    let mut backup: Option<PathBuf> = None;
    if !old.is_empty() {
        let base = save_dir.to_string_lossy().trim_end_matches(['/', '\\']).to_string();
        let mut b = PathBuf::from(format!("{}.before-{}", base, t));
        let mut i = 2;
        while b.exists() {
            b = PathBuf::from(format!("{}.before-{}-{}", base, t, i));
            i += 1;
        }
        let bacc = b.join("account");
        let moved = (|| -> io::Result<()> {
            fs::create_dir_all(&bacc)?;
            for p in &old {
                fs::rename(p, bacc.join(p.file_name().unwrap()))?;
            }
            let meta = acc.join(format!("{}.meta", key));
            if meta.is_file() {
                fs::copy(&meta, bacc.join(format!("{}.meta", key)))?;
            }
            Ok(())
        })();
        if let Err(e) = moved {
            put_back(&old, &bacc);
            let _ = fs::remove_dir_all(&stage);
            return Err(format!("Could not move the old save aside ({}). Nothing was changed.", e));
        }
        backup = Some(b);
    }

    // 3. the staged copy becomes the committed folder
    if let Err(e) = fs::rename(&stage, &root) {
        if let Some(b) = &backup {
            put_back(&old, &b.join("account"));
        }
        let _ = fs::remove_dir_all(&stage);
        return Err(format!("Could not put the imported save in place ({}). The old save was put back.", e));
    }
    // 4. the .meta the runtime needs to see the save (an existing one is kept: it carries the game's size and owner)
    let meta = acc.join(format!("{}.meta", key));
    if !meta.is_file() {
        let tmp = acc.join(format!("{}.meta.tmp", key));
        if fs::write(&tmp, meta_text(key)).and_then(|_| fs::rename(&tmp, &meta)).is_err() {
            let _ = fs::remove_file(&tmp);
            return Err(format!("The save was copied to {} but its .meta could not be written. Start the game once to let it create one.", fwd(&root)));
        }
    }
    Ok(Report { source: fwd(src), files, backup: backup.map(|b| fwd(&b)), target: fwd(&root) })
}

/// True while a pkp.exe process runs (the runtime holds the save open and would overwrite the import at its next commit).
#[cfg(windows)]
pub fn game_running() -> bool {
    use std::os::windows::process::CommandExt;
    std::process::Command::new("tasklist")
        .args(["/FI", "IMAGENAME eq pkp.exe", "/NH"])
        .creation_flags(0x0800_0000)
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).to_lowercase().contains("pkp.exe"))
        .unwrap_or(false)
}
/// Linux: a process named `pkp` (from `host::game_exe_name`).
#[cfg(not(windows))]
pub fn game_running() -> bool {
    let name = crate::host::game_exe_name();
    fs::read_dir("/proc")
        .map(|rd| rd.flatten().any(|e| fs::read_to_string(e.path().join("comm")).map(|c| c.trim() == name).unwrap_or(false)))
        .unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn meta_with(attr: &str) -> bool {
        static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let p = std::env::temp_dir().join(format!("pkp-meta-{}-{}.meta", std::process::id(), N.fetch_add(1, std::sync::atomic::Ordering::SeqCst)));
        fs::write(&p, format!("size=1\nattr={}\n", attr)).unwrap();
        let r = meta_attr_matches(&p);
        let _ = fs::remove_file(&p);
        r
    }

    #[test]
    fn a_repeated_save_dir_key_takes_the_last_value_like_the_runtime() {
        let dir = std::env::temp_dir().join(format!("pkp-locate-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("pkp.ini"), "[fs]
save_dir = first
save_dir = second
").unwrap();
        let (sd, _) = locate(&dir);
        let _ = fs::remove_dir_all(&dir);
        assert_eq!(sd, dir.join("second"));
    }

    #[test]
    fn an_account_meta_of_the_game_matches() {
        let attr = format!("{}{}{}01{}", app_hex(), "ab".repeat(16), "00".repeat(8), "00".repeat(31));
        assert!(meta_with(&attr));
        assert!(!meta_with(&attr.replace("01", "00")));
    }

    #[test]
    fn a_non_ascii_attr_is_no_match_and_no_panic() {
        // a multi-byte character straddling offsets 16 and 64
        let mut attr = "é".repeat(8);
        attr.push_str(&"0".repeat(60));
        assert!(attr.len() >= 66);
        assert!(!meta_with(&attr));
        let mut late = "0".repeat(63);
        late.push_str("éé");
        assert!(late.len() >= 66 && !meta_with(&late));
    }
}
