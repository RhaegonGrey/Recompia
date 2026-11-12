//! Add to Steam: puts the launcher into Steam as a Non-Steam Game ("Recompia", or "Recompia (Testing)" on the Testing channel) with the
//! launch option `--play`, and copies the artwork into Steam's `grid` folder, so Game Mode and Big Picture show it without a search.
//!
//! Files (the app id rule and the file layout are the ones Steam ROM Manager uses):
//!  * `<Steam>/userdata/<account>/config/shortcuts.vdf`: Steam's binary key-value file, one numbered entry per shortcut. Steam keeps it in
//!    memory and writes it when it closes, so it is only edited while Steam is not running.
//!  * `<Steam>/userdata/<account>/config/grid/<id>.png` (wide), `<id>p.png` (portrait), `<id>_hero.png`, `<id>_logo.png`, `<id>_icon.png`,
//!    where `<id>` is the short app id: the CRC-32 of the quoted exe path followed by the name, with the top bit set.
//! The old file is copied to `shortcuts.vdf.recompia-backup` before the first change and that copy is kept (never overwritten, so it stays the
//! file the user had). Only our own entry and our own artwork are added,
//! replaced or removed; other shortcuts are kept as they are (unknown fields too).
use std::fs;
use std::path::{Path, PathBuf};

use crate::fsutil::{self, AtomicFail};

use serde::Serialize;

// ---- the artwork, embedded (the grids and hero are a plain meadow placeholder with the wordmark; the icons are the launcher's: the R, and the R with a T badge on Testing) ----
const WIDE: &[u8] = include_bytes!("../assets/steam/grid_wide.png");
const PORTRAIT: &[u8] = include_bytes!("../assets/steam/grid_portrait.png");
const HERO: &[u8] = include_bytes!("../assets/steam/hero.png");
const LOGO: &[u8] = include_bytes!("../assets/steam/logo.png");
const ICON: &[u8] = include_bytes!("../assets/steam/icon.png");
const ICON_TESTING: &[u8] = include_bytes!("../assets/steam/icon_testing.png");

// ---- Steam's binary key-value format ----
// A map is a run of entries ended by 0x08. An entry is a type byte (0 map, 1 string, 2 u32 little endian), the key and a 0 byte, then the
// value (a map's value is the map itself; a string ends with a 0 byte). The file is the root map, which holds one entry: "shortcuts".
#[derive(Clone, Debug, PartialEq)]
pub enum Val {
    Map(Vec<(String, Val)>),
    Str(String),
    Int(u32),
}

fn cstr(b: &[u8], p: &mut usize) -> Result<String, String> {
    let start = *p;
    while *p < b.len() && b[*p] != 0 {
        *p += 1;
    }
    if *p >= b.len() {
        return Err("a text in the file is not finished".into());
    }
    let s = String::from_utf8_lossy(&b[start..*p]).into_owned();
    *p += 1;
    Ok(s)
}

fn parse_map(b: &[u8], p: &mut usize, depth: u32) -> Result<Vec<(String, Val)>, String> {
    if depth > 8 {
        return Err("the file nests too deep".into());
    }
    let mut out = Vec::new();
    loop {
        let t = *b.get(*p).ok_or("the file ends inside a list")?;
        *p += 1;
        if t == 8 {
            return Ok(out);
        }
        let key = cstr(b, p)?;
        let v = match t {
            0 => Val::Map(parse_map(b, p, depth + 1)?),
            1 => Val::Str(cstr(b, p)?),
            2 => {
                let w = b.get(*p..*p + 4).ok_or("the file ends inside a number")?;
                *p += 4;
                Val::Int(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
            }
            other => return Err(format!("unknown entry type {other} in the file")),
        };
        out.push((key, v));
    }
}

/// The root map of a shortcuts.vdf. An empty or missing file is an empty root.
pub fn parse(b: &[u8]) -> Result<Vec<(String, Val)>, String> {
    if b.is_empty() {
        return Ok(Vec::new());
    }
    let mut p = 0;
    let root = parse_map(b, &mut p, 0)?;
    Ok(root)
}

fn write_map(out: &mut Vec<u8>, m: &[(String, Val)]) {
    for (k, v) in m {
        match v {
            Val::Map(inner) => {
                out.push(0);
                out.extend_from_slice(k.as_bytes());
                out.push(0);
                write_map(out, inner);
            }
            Val::Str(s) => {
                out.push(1);
                out.extend_from_slice(k.as_bytes());
                out.push(0);
                out.extend_from_slice(s.as_bytes());
                out.push(0);
            }
            Val::Int(n) => {
                out.push(2);
                out.extend_from_slice(k.as_bytes());
                out.push(0);
                out.extend_from_slice(&n.to_le_bytes());
            }
        }
    }
    out.push(8);
}

pub fn write(root: &[(String, Val)]) -> Vec<u8> {
    let mut out = Vec::new();
    write_map(&mut out, root);
    out
}

fn key_is(k: &str, name: &str) -> bool {
    k.eq_ignore_ascii_case(name)
}

fn get_str<'a>(entry: &'a [(String, Val)], name: &str) -> Option<&'a str> {
    entry.iter().find_map(|(k, v)| match v {
        Val::Str(s) if key_is(k, name) => Some(s.as_str()),
        _ => None,
    })
}

/// The list of shortcuts inside the root map, created when the file has none.
fn shortcuts_mut(root: &mut Vec<(String, Val)>) -> &mut Vec<(String, Val)> {
    let i = match root.iter().position(|(k, v)| key_is(k, "shortcuts") && matches!(v, Val::Map(_))) {
        Some(i) => i,
        None => {
            root.push(("shortcuts".into(), Val::Map(Vec::new())));
            root.len() - 1
        }
    };
    match &mut root[i].1 {
        Val::Map(m) => m,
        _ => unreachable!(),
    }
}

// ---- the app id ----
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xffff_ffffu32;
    for &byte in data {
        crc ^= byte as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 { 0xedb8_8320 ^ (crc >> 1) } else { crc >> 1 };
        }
    }
    !crc
}

/// Steam's short app id of a Non-Steam shortcut: the CRC-32 of the exe text (as written in shortcuts.vdf, with its quotes) followed by
/// the name, top bit set. It names the artwork files, and is stored in the entry as the same 32 bits.
pub fn short_id(exe: &str, name: &str) -> u32 {
    crc32(format!("{exe}{name}").as_bytes()) | 0x8000_0000
}

// ---- what is added ----
#[derive(Clone, Debug)]
pub struct Spec {
    pub name: String,
    pub exe: String,        // quoted, as Steam writes it
    pub start_dir: String,  // quoted
    pub launch: String,
    pub testing: bool,
}

fn entry_for(spec: &Spec, icon_path: &str) -> Vec<(String, Val)> {
    let s = |k: &str, v: &str| (k.to_string(), Val::Str(v.to_string()));
    let n = |k: &str, v: u32| (k.to_string(), Val::Int(v));
    vec![
        n("appid", short_id(&spec.exe, &spec.name)),
        s("AppName", &spec.name),
        s("Exe", &spec.exe),
        s("StartDir", &spec.start_dir),
        s("icon", icon_path),
        s("ShortcutPath", ""),
        s("LaunchOptions", &spec.launch),
        n("IsHidden", 0),
        n("AllowDesktopConfig", 1),
        n("AllowOverlay", 1),
        n("OpenVR", 0),
        n("Devkit", 0),
        s("DevkitGameID", ""),
        n("DevkitOverrideAppID", 0),
        n("LastPlayTime", 0),
        s("FlatpakAppID", ""),
        ("tags".to_string(), Val::Map(Vec::new())),
    ]
}

fn is_ours(entry: &[(String, Val)], spec: &Spec) -> bool {
    get_str(entry, "AppName") == Some(spec.name.as_str()) && get_str(entry, "Exe") == Some(spec.exe.as_str())
}

/// Adds our entry, or replaces the one we added before. Other entries are untouched. Returns the new list of the file.
pub fn add_entry(root: &mut Vec<(String, Val)>, spec: &Spec, icon_path: &str) {
    let list = shortcuts_mut(root);
    let fresh = entry_for(spec, icon_path);
    if let Some(at) = list.iter().position(|(_, v)| matches!(v, Val::Map(e) if is_ours(e, spec))) {
        list[at].1 = Val::Map(fresh);
        return;
    }
    let next = list.iter().filter_map(|(k, _)| k.parse::<u32>().ok()).max().map_or(0, |m| m + 1);
    list.push((next.to_string(), Val::Map(fresh)));
}

/// Takes our entry out; the numbers of the others are kept (Steam does not need them without gaps). True when there was one.
pub fn remove_entry(root: &mut Vec<(String, Val)>, spec: &Spec) -> bool {
    let list = shortcuts_mut(root);
    let before = list.len();
    list.retain(|(_, v)| !matches!(v, Val::Map(e) if is_ours(e, spec)));
    list.len() != before
}

pub fn has_entry(root: &[(String, Val)], spec: &Spec) -> bool {
    root.iter().any(|(k, v)| match v {
        Val::Map(list) if key_is(k, "shortcuts") => list.iter().any(|(_, e)| matches!(e, Val::Map(e) if is_ours(e, spec))),
        _ => false,
    })
}

// ---- Steam's folders ----
fn roots() -> Vec<PathBuf> {
    roots_in(std::env::var_os("HOME").map(PathBuf::from))
}

/// The Steam installs under `home`: Linux keeps them under `.local/share`, macOS in `Library/Application Support`, and the Flatpak
/// in `.var`. Split from `roots()` so a test can point it at a folder of its own.
fn roots_in(home: Option<PathBuf>) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = Vec::new();
    #[cfg(unix)]
    if let Some(home) = home {
        #[cfg(target_os = "macos")]
        v.push(home.join("Library/Application Support/Steam"));
        #[cfg(not(target_os = "macos"))]
        {
            v.push(home.join(".local/share/Steam"));
            v.push(home.join(".steam/steam"));
            v.push(home.join(".var/app/com.valvesoftware.Steam/.local/share/Steam"));
        }
    }
    #[cfg(not(unix))]
    let _ = home;
    #[cfg(windows)]
    {
        for var in ["ProgramFiles(x86)", "ProgramFiles"] {
            if let Some(p) = std::env::var_os(var) {
                v.push(PathBuf::from(p).join("Steam"));
            }
        }
        v.push(PathBuf::from(r"C:\Program Files (x86)\Steam"));
    }
    let mut seen: Vec<PathBuf> = Vec::new();
    let mut out = Vec::new();
    for p in v {
        if !p.join("userdata").is_dir() {
            continue;
        }
        let real = fs::canonicalize(&p).unwrap_or_else(|_| p.clone());
        if seen.contains(&real) {
            continue;
        }
        seen.push(real);
        out.push(p);
    }
    out
}

/// The account folders of a Steam install (the numeric ones, not the shared 0).
fn accounts(root: &Path) -> Vec<PathBuf> {
    let Ok(rd) = fs::read_dir(root.join("userdata")) else { return Vec::new() };
    let mut v: Vec<PathBuf> = rd
        .flatten()
        .filter(|e| e.path().is_dir())
        .filter(|e| {
            let n = e.file_name().to_string_lossy().into_owned();
            n != "0" && !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit())
        })
        .map(|e| e.path())
        .collect();
    v.sort();
    v
}

fn steam_running() -> bool {
    #[cfg(target_os = "linux")]
    {
        let Ok(rd) = fs::read_dir("/proc") else { return false };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if !n.bytes().all(|b| b.is_ascii_digit()) {
                continue;
            }
            if let Ok(c) = fs::read_to_string(e.path().join("comm")) {
                if c.trim() == "steam" {
                    return true;
                }
            }
        }
        false
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        std::process::Command::new("tasklist")
            .args(["/FI", "IMAGENAME eq steam.exe", "/NH"])
            .creation_flags(0x0800_0000)
            .output()
            .map(|o| String::from_utf8_lossy(&o.stdout).to_ascii_lowercase().contains("steam.exe"))
            .unwrap_or(false)
    }
    #[cfg(target_os = "macos")]
    {
        // Steam runs as steam_osx. Its own folder is checked too, because pgrep is not guaranteed on a Mac without the developer tools.
        if std::process::Command::new("pgrep").arg("-x").arg("steam_osx").output().map(|o| o.status.success()).unwrap_or(false) {
            return true;
        }
        std::env::var_os("HOME")
            .map(PathBuf::from)
            .map(|h| h.join("Library/Application Support/Steam/logs/connection_log.txt"))
            .is_some_and(|log| fs::metadata(log).and_then(|m| m.modified()).map(|t| t.elapsed().map(|e| e.as_secs() < 120).unwrap_or(false)).unwrap_or(false))
    }
    #[cfg(not(any(target_os = "linux", windows, target_os = "macos")))]
    {
        false
    }
}

// ---- reading and writing one account ----
fn art_files(grid: &Path, id: u32) -> [(PathBuf, &'static str); 5] {
    [
        (grid.join(format!("{id}.png")), "wide"),
        (grid.join(format!("{id}p.png")), "portrait"),
        (grid.join(format!("{id}_hero.png")), "hero"),
        (grid.join(format!("{id}_logo.png")), "logo"),
        (grid.join(format!("{id}_icon.png")), "icon"),
    ]
}

fn art_bytes(kind: &str, testing: bool) -> &'static [u8] {
    match kind {
        "wide" => WIDE,
        "portrait" => PORTRAIT,
        "hero" => HERO,
        "logo" => LOGO,
        _ => {
            if testing {
                ICON_TESTING
            } else {
                ICON
            }
        }
    }
}

fn write_atomic(path: &Path, data: &[u8]) -> Result<(), String> {
    fsutil::write_atomic(path, data).map_err(|fail| match fail {
        AtomicFail::Write { error, .. } => format!("could not write {}: {error}", path.display()),
        AtomicFail::Replace { error } => format!("could not replace {}: {error}", path.display()),
    })
}

fn load_root(file: &Path) -> Result<Vec<(String, Val)>, String> {
    match fs::read(file) {
        Ok(b) => parse(&b).map_err(|e| format!("Steam's shortcut list could not be read ({e}). Nothing was changed.")),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Vec::new()),
        Err(e) => Err(format!("could not read {}: {e}", file.display())),
    }
}

/// Keeps a copy of Steam's shortcut list as the user had it: made once, the first time we change an existing file, and never overwritten
/// (a later add or remove would otherwise replace it with a file that already has our entries).
fn save_backup(cfg: &Path, file: &Path) -> Result<(), String> {
    let backup = cfg.join("shortcuts.vdf.recompia-backup");
    if file.exists() && !backup.exists() {
        fs::copy(file, &backup).map_err(|e| format!("could not save a backup of Steam's shortcut list: {e}"))?;
    }
    Ok(())
}

/// Adds the shortcut and the artwork for one account folder. The file is only rewritten after it parsed and the old one is saved.
pub fn add_to_account(account: &Path, spec: &Spec) -> Result<(), String> {
    let cfg = account.join("config");
    let grid = cfg.join("grid");
    fs::create_dir_all(&grid).map_err(|e| format!("could not create {}: {e}", grid.display()))?;
    let file = cfg.join("shortcuts.vdf");
    let mut root = load_root(&file)?;
    let id = short_id(&spec.exe, &spec.name);
    for (path, kind) in art_files(&grid, id) {
        write_atomic(&path, art_bytes(kind, spec.testing))?;
    }
    let icon_path = grid.join(format!("{id}_icon.png")).to_string_lossy().into_owned();
    add_entry(&mut root, spec, &icon_path);
    save_backup(&cfg, &file)?;
    write_atomic(&file, &write(&root))
}

/// Takes our shortcut out and deletes our artwork files (only those that are still exactly ours). True when the shortcut was there.
pub fn remove_from_account(account: &Path, spec: &Spec) -> Result<bool, String> {
    let cfg = account.join("config");
    let file = cfg.join("shortcuts.vdf");
    let mut root = load_root(&file)?;
    let had = remove_entry(&mut root, spec);
    if had {
        save_backup(&cfg, &file)?;
        write_atomic(&file, &write(&root))?;
    }
    let id = short_id(&spec.exe, &spec.name);
    for (path, kind) in art_files(&cfg.join("grid"), id) {
        if let Ok(cur) = fs::read(&path) {
            if cur == art_bytes(kind, spec.testing) {
                let _ = fs::remove_file(&path);
            }
        }
    }
    Ok(had)
}

// ---- what the launcher calls ----
#[derive(Serialize, Default, Clone, Debug)]
pub struct Status {
    pub found: bool,
    pub running: bool,
    pub accounts: usize,
    /// Accounts that have the shortcut that starts the game straight away (`--play`).
    pub game_added: usize,
    /// Accounts that have the shortcut that opens the launcher window.
    pub launcher_added: usize,
    pub game_name: String,
    pub launcher_name: String,
}

/// Which shortcut: the game itself (starts it at once, `--play`) or the launcher window.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Target {
    Game,
    Launcher,
}

impl Target {
    pub fn parse(s: &str) -> Result<Target, String> {
        match s {
            "game" => Ok(Target::Game),
            "launcher" => Ok(Target::Launcher),
            other => Err(format!("unknown Steam target \"{other}\"")),
        }
    }
}

/// The shortcut for this launcher's own file (the AppImage when run from one). The game shortcut is named "Recompia" (the name the game
/// goes by, so a SteamGridDB search for it finds the right title) and starts the game with `--play`; the launcher shortcut is "Recompia Launcher" and has no launch option.
pub fn spec_here(testing: bool, target: Target) -> Result<Spec, String> {
    let exe = std::env::var_os("APPIMAGE")
        .map(PathBuf::from)
        .filter(|p| p.is_file())
        .or_else(|| std::env::current_exe().ok())
        .ok_or("could not find this launcher's own path")?;
    let dir = exe.parent().map(Path::to_path_buf).unwrap_or_default();
    Ok(Spec {
        name: shortcut_name(testing, target),
        exe: format!("\"{}\"", exe.display()),
        start_dir: format!("\"{}\"", dir.display()),
        launch: if target == Target::Game { "--play".into() } else { String::new() },
        testing,
    })
}

pub fn shortcut_name(testing: bool, target: Target) -> String {
    match (target, testing) {
        (Target::Game, false) => "Recompia".into(),
        (Target::Game, true) => "Recompia (Testing)".into(),
        (Target::Launcher, false) => "Recompia Launcher".into(),
        (Target::Launcher, true) => "Recompia Launcher (Testing)".into(),
    }
}

fn all_accounts() -> Vec<PathBuf> {
    roots().iter().flat_map(|r| accounts(r)).collect()
}

fn count_added(accs: &[PathBuf], spec: &Result<Spec, String>) -> usize {
    match spec {
        Ok(s) => accs.iter().filter(|a| load_root(&a.join("config").join("shortcuts.vdf")).map(|r| has_entry(&r, s)).unwrap_or(false)).count(),
        Err(_) => 0,
    }
}

pub fn status(testing: bool) -> Status {
    let accs = all_accounts();
    Status {
        found: !roots().is_empty() && !accs.is_empty(),
        running: steam_running(),
        accounts: accs.len(),
        game_added: count_added(&accs, &spec_here(testing, Target::Game)),
        launcher_added: count_added(&accs, &spec_here(testing, Target::Launcher)),
        game_name: shortcut_name(testing, Target::Game),
        launcher_name: shortcut_name(testing, Target::Launcher),
    }
}

pub fn add(testing: bool, target: Target) -> Result<String, String> {
    if steam_running() {
        return Err("Close Steam first (Steam, Exit), then press the button again. Steam only saves its shortcut list when it closes.".into());
    }
    let accs = all_accounts();
    if accs.is_empty() {
        return Err("Steam was not found on this PC, or nobody has signed in to it yet. Start Steam once, sign in, close it, and try again.".into());
    }
    let spec = spec_here(testing, target)?;
    for a in &accs {
        add_to_account(a, &spec)?;
    }
    Ok(format!("Added \"{}\" to Steam for {} account{}. Start Steam again to see it, with its artwork.", spec.name, accs.len(), if accs.len() == 1 { "" } else { "s" }))
}

pub fn remove(testing: bool, target: Target) -> Result<String, String> {
    if steam_running() {
        return Err("Close Steam first (Steam, Exit), then press the button again.".into());
    }
    let spec = spec_here(testing, target)?;
    let mut n = 0;
    for a in all_accounts() {
        if remove_from_account(&a, &spec)? {
            n += 1;
        }
    }
    Ok(if n == 0 { "It was not in Steam.".into() } else { format!("Removed \"{}\" from Steam. Start Steam again.", spec.name) })
}

#[cfg(test)]
mod tests {
    use super::*;

    // unix only: Windows finds Steam under Program Files, not under a home folder a test can point at
    #[cfg(unix)]
    #[test]
    fn roots_find_the_platform_install() {
        let base = std::env::temp_dir().join(format!("steam_roots_{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        #[cfg(target_os = "macos")]
        let steam = base.join("Library/Application Support/Steam");
        #[cfg(not(target_os = "macos"))]
        let steam = base.join(".local/share/Steam");
        fs::create_dir_all(steam.join("userdata").join("123").join("config")).unwrap();
        let roots = roots_in(Some(base.clone()));
        assert_eq!(roots, vec![steam.clone()]);
        assert_eq!(accounts(&steam).len(), 1);
        let _ = fs::remove_dir_all(&base);
    }

    fn spec(testing: bool) -> Spec {
        Spec {
            name: if testing { "Recompia (Testing)" } else { "Recompia" }.into(),
            exe: "\"/home/deck/Games/PokopiaRecomp/PokopiaLauncher.AppImage\"".into(),
            start_dir: "\"/home/deck/Games/PokopiaRecomp\"".into(),
            launch: "--play".into(),
            testing,
        }
    }

    fn tmp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("recompia-steam-test-{}-{tag}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn crc32_known_values() {
        assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
        assert_eq!(crc32(b""), 0);
        // the rule: CRC-32 of exe + name, top bit set
        assert_eq!(short_id("a", "b") & 0x8000_0000, 0x8000_0000);
        assert_eq!(short_id("a", "b"), crc32(b"ab") | 0x8000_0000);
    }

    #[test]
    fn vdf_round_trip_keeps_everything() {
        let mut root = Vec::new();
        add_entry(&mut root, &spec(false), "/x/icon.png");
        let other = Spec { name: "Other Game".into(), exe: "\"/bin/other\"".into(), start_dir: "\"/bin\"".into(), launch: "-x".into(), testing: false };
        add_entry(&mut root, &other, "");
        let bytes = write(&root);
        assert_eq!(bytes[0], 0);
        assert_eq!(*bytes.last().unwrap(), 8);
        let back = parse(&bytes).unwrap();
        assert_eq!(back, root);
        assert_eq!(write(&back), bytes);
    }

    #[test]
    fn add_replaces_ours_and_keeps_others() {
        let mut root = Vec::new();
        let other = Spec { name: "Other".into(), exe: "\"/bin/o\"".into(), start_dir: "\"/bin\"".into(), launch: "".into(), testing: false };
        add_entry(&mut root, &other, "");
        add_entry(&mut root, &spec(false), "a");
        add_entry(&mut root, &spec(false), "b");   // the second add updates the entry, it does not make a second one
        let list = match &root[0].1 { Val::Map(m) => m.clone(), _ => panic!() };
        assert_eq!(list.len(), 2);
        assert!(has_entry(&root, &spec(false)));
        assert!(!has_entry(&root, &spec(true)));   // the Testing shortcut is a different one
        assert!(remove_entry(&mut root, &spec(false)));
        assert!(!remove_entry(&mut root, &spec(false)));
        assert!(has_entry(&root, &other));
    }

    #[test]
    fn game_and_launcher_are_two_shortcuts() {
        let g = shortcut_name(false, Target::Game);
        let l = shortcut_name(false, Target::Launcher);
        assert_eq!(g, "Recompia");
        assert_eq!(l, "Recompia Launcher");
        assert_eq!(shortcut_name(true, Target::Game), "Recompia (Testing)");
        assert_eq!(shortcut_name(true, Target::Launcher), "Recompia Launcher (Testing)");
        assert!(Target::parse("game").is_ok() && Target::parse("launcher").is_ok() && Target::parse("x").is_err());
        let acc = tmp("two");
        let exe = "\"/h/PokopiaLauncher.AppImage\"".to_string();
        let game = Spec { name: g, exe: exe.clone(), start_dir: "\"/h\"".into(), launch: "--play".into(), testing: false };
        let launcher = Spec { name: l, exe, start_dir: "\"/h\"".into(), launch: String::new(), testing: false };
        assert_ne!(short_id(&game.exe, &game.name), short_id(&launcher.exe, &launcher.name));
        add_to_account(&acc, &game).unwrap();
        add_to_account(&acc, &launcher).unwrap();
        let root = parse(&fs::read(acc.join("config/shortcuts.vdf")).unwrap()).unwrap();
        assert!(has_entry(&root, &game) && has_entry(&root, &launcher));
        // removing one leaves the other
        assert!(remove_from_account(&acc, &game).unwrap());
        let root = parse(&fs::read(acc.join("config/shortcuts.vdf")).unwrap()).unwrap();
        assert!(!has_entry(&root, &game) && has_entry(&root, &launcher));
        let _ = fs::remove_dir_all(&acc);
    }

    #[test]
    fn parse_refuses_garbage_and_reads_empty() {
        assert!(parse(&[]).unwrap().is_empty());
        assert!(parse(&[9, b'x', 0]).is_err());
        assert!(parse(&[1, b'k', 0, b'v']).is_err());      // string not finished
        assert!(parse(&[2, b'k', 0, 1, 2]).is_err());      // number cut short
        assert!(parse(&[0, b'a', 0]).is_err());            // map not closed
    }

    #[test]
    fn the_backup_stays_the_file_the_user_had() {
        let acc = tmp("backup");
        let cfg = acc.join("config");
        fs::create_dir_all(&cfg).unwrap();
        let mut b = Vec::new();
        b.extend_from_slice(&[0]); b.extend_from_slice(b"shortcuts\0");
        b.extend_from_slice(&[0]); b.extend_from_slice(b"0\0");
        b.push(1); b.extend_from_slice(b"AppName\0Mine\0");
        b.extend_from_slice(&[8, 8, 8]);
        fs::write(cfg.join("shortcuts.vdf"), &b).unwrap();
        let mut launcher = spec(false);
        launcher.name = "Recompia Launcher".into();
        launcher.launch = String::new();
        add_to_account(&acc, &spec(false)).unwrap();
        add_to_account(&acc, &launcher).unwrap();
        assert!(remove_from_account(&acc, &spec(false)).unwrap());
        assert_eq!(fs::read(cfg.join("shortcuts.vdf.recompia-backup")).unwrap(), b);
        let _ = fs::remove_dir_all(&acc);
    }

    #[test]
    fn unknown_fields_and_case_survive() {
        // an entry written by Steam with lower-case names and an extra field
        let mut b = Vec::new();
        b.extend_from_slice(&[0]); b.extend_from_slice(b"shortcuts\0");
        b.extend_from_slice(&[0]); b.extend_from_slice(b"0\0");
        b.push(1); b.extend_from_slice(b"appname\0Mine\0");
        b.push(1); b.extend_from_slice(b"exe\0\"/m\"\0");
        b.push(2); b.extend_from_slice(b"Extra\0"); b.extend_from_slice(&7u32.to_le_bytes());
        b.extend_from_slice(&[8, 8, 8]);
        let mut root = parse(&b).unwrap();
        add_entry(&mut root, &spec(false), "");
        let out = write(&root);
        let back = parse(&out).unwrap();
        let s = String::from_utf8_lossy(&out).into_owned();
        assert!(s.contains("appname") && s.contains("Extra") && s.contains("Recompia"));
        assert_eq!(back, root);
    }

    #[test]
    fn account_add_and_remove_end_to_end() {
        let acc = tmp("e2e");
        let s = spec(false);
        add_to_account(&acc, &s).unwrap();
        let cfg = acc.join("config");
        let id = short_id(&s.exe, &s.name);
        for n in [format!("{id}.png"), format!("{id}p.png"), format!("{id}_hero.png"), format!("{id}_logo.png"), format!("{id}_icon.png")] {
            assert!(cfg.join("grid").join(&n).is_file(), "{n} missing");
        }
        let root = parse(&fs::read(cfg.join("shortcuts.vdf")).unwrap()).unwrap();
        assert!(has_entry(&root, &s));
        // the second add changes nothing but keeps a backup of the first file
        let first = fs::read(cfg.join("shortcuts.vdf")).unwrap();
        add_to_account(&acc, &s).unwrap();
        assert_eq!(fs::read(cfg.join("shortcuts.vdf")).unwrap(), first);
        assert_eq!(fs::read(cfg.join("shortcuts.vdf.recompia-backup")).unwrap(), first);
        // a file the user changed is not deleted on remove
        fs::write(cfg.join("grid").join(format!("{id}_hero.png")), b"mine").unwrap();
        assert!(remove_from_account(&acc, &s).unwrap());
        assert!(!cfg.join("grid").join(format!("{id}.png")).exists());
        assert!(cfg.join("grid").join(format!("{id}_hero.png")).exists());
        assert!(!has_entry(&parse(&fs::read(cfg.join("shortcuts.vdf")).unwrap()).unwrap(), &s));
        assert!(!remove_from_account(&acc, &s).unwrap());
        let _ = fs::remove_dir_all(&acc);
    }

    #[test]
    fn unreadable_file_is_left_alone() {
        let acc = tmp("bad");
        let cfg = acc.join("config");
        fs::create_dir_all(&cfg).unwrap();
        fs::write(cfg.join("shortcuts.vdf"), [9u8, 1, 2, 3]).unwrap();
        let err = add_to_account(&acc, &spec(false)).unwrap_err();
        assert!(err.contains("Nothing was changed"), "{err}");
        assert_eq!(fs::read(cfg.join("shortcuts.vdf")).unwrap(), vec![9u8, 1, 2, 3]);
        let _ = fs::remove_dir_all(&acc);
    }

    #[test]
    fn accounts_skip_the_shared_folder() {
        let root = tmp("acc");
        for n in ["0", "93249718", "notanumber"] {
            fs::create_dir_all(root.join("userdata").join(n)).unwrap();
        }
        let a = accounts(&root);
        assert_eq!(a.len(), 1);
        assert!(a[0].ends_with("93249718"));
        let _ = fs::remove_dir_all(&root);
    }
}
