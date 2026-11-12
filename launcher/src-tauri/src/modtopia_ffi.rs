// SPDX-License-Identifier: GPL-3.0-or-later
//! The Modtopia core (MIT; build.rs compiles runtime/third_party/modtopia into the launcher) behind a safe wrapper. `Core::open` makes a
//! loader for one mods folder that never loads mod code (`no_code`): the launcher lists mods, changes their switches, order and
//! settings, records the player's yes to a mod's code and writes the hook plan, all of which the core owns (modtopia_host.h, contract 1).
//!
//! The backend sets only what a plan needs (modtopia_host.h: size, the names, log). The platform is the host's: the game is a native
//! program (pkp.exe on Windows, pkp on Linux and macOS), so a mod's library is the one built for this system (mt_loader.c
//! `mt_platform`, which returns the same three tags).
//! The core keeps process-wide state (the last loader made, its network lock), so every loader and every call is serialised by one lock
//! held for a `Core`'s whole life. Strings the core returns are copied out at once.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::path::Path;
use std::sync::{Mutex, MutexGuard};

/// The core's version this launcher is built with (MT_CORE_VERSION): what a catalog entry's `needs` range is checked against.
pub const CORE_VERSION: &str = "0.6.1";
/// The game version every mod is planned for (the one game build Recompia runs).
pub const GAME_VERSION: &str = "2.0.1";
/// The tag of the library folder the core plans mods for: the host's, the same three values `mt_platform` (mt_loader.c) returns.
pub fn platform_tag() -> &'static str {
    if cfg!(windows) {
        "windows-x64"
    } else if cfg!(target_os = "macos") {
        "macos-arm64"
    } else {
        "linux-x64"
    }
}

pub const FLAG_CODE: i32 = 1;
pub const FLAG_LAYEREDFS: i32 = 4;
pub const FLAG_GAMEPLAY: i32 = 8;
pub const FLAG_NEW_CODE: i32 = 16;
pub const FLAG_KIND_GUESSED: i32 = 32;
pub const FLAG_NETWORK: i32 = 128;
pub const STATE_OK: i32 = 0;
pub const STATE_ERROR: i32 = 2;
pub const SET_BOOL: i32 = 1;
pub const SET_NUMBER: i32 = 2;
pub const SET_CHOICE: i32 = 3;
pub const SET_TEXT: i32 = 4;
pub const SET_KEY: i32 = 5;

type LogFn = extern "C" fn(user: *mut c_void, level: c_int, line: *const c_char);

/// The head of MtBackend up to `log`: `size` tells the core that every callback after it is NULL.
#[repr(C)]
struct Backend {
    size: u32,
    user: *mut c_void,
    host_name: *const c_char,
    game_version: *const c_char,
    game_build_id: *const c_char,
    platform: *const c_char,
    log: Option<LogFn>,
}

#[repr(C)]
struct Options {
    mods_dir: *const c_char,
    config_dir: *const c_char,
    cache_dir: *const c_char,
    mappings_dir: *const c_char,
    no_code: c_int,
}

#[repr(C)]
struct RawModInfo {
    id: *const c_char,
    name: *const c_char,
    version: *const c_char,
    authors: *const c_char,
    description: *const c_char,
    license: *const c_char,
    source: *const c_char,
    sha256: *const c_char,
    flags: c_int,
    state: c_int,
    enabled: c_int,
    started: c_int,
    message: *const c_char,
    load_index: c_int,
    hook_points: c_int,
    patches: c_int,
    network: *const c_char,
}

#[repr(C)]
struct RawSettingInfo {
    key: *const c_char,
    label: *const c_char,
    description: *const c_char,
    kind: c_int,
    restart: c_int,
    min: f64,
    max: f64,
    step: f64,
    nchoices: c_int,
    choices: *const *const c_char,
}

#[repr(C)]
struct MtLoader {
    _p: [u8; 0],
}

extern "C" {
    fn mt_loader_new(opt: *const Options, be: *const Backend) -> *mut MtLoader;
    fn mt_loader_free(l: *mut MtLoader);
    fn mt_loader_scan(l: *mut MtLoader) -> c_int;
    fn mt_loader_count(l: *mut MtLoader) -> c_int;
    fn mt_loader_info(l: *mut MtLoader, i: c_int) -> *const RawModInfo;
    fn mt_loader_find(l: *mut MtLoader, id: *const c_char) -> c_int;
    fn mt_loader_set_enabled(l: *mut MtLoader, id: *const c_char, on: c_int) -> c_int;
    fn mt_loader_move(l: *mut MtLoader, id: *const c_char, delta: c_int) -> c_int;
    fn mt_loader_trust_mod(l: *mut MtLoader, id: *const c_char) -> c_int;
    fn mt_loader_gameplay_mods(l: *mut MtLoader, f: extern "C" fn(*mut c_void, *const c_char), user: *mut c_void) -> c_int;
    fn mt_setting_count(l: *mut MtLoader, m: c_int) -> c_int;
    fn mt_setting_info(l: *mut MtLoader, m: c_int, i: c_int) -> *const RawSettingInfo;
    fn mt_setting_get_text(l: *mut MtLoader, m: c_int, i: c_int, out: *mut c_char, n: usize) -> c_int;
    fn mt_setting_get_number(l: *mut MtLoader, m: c_int, i: c_int) -> f64;
    fn mt_setting_set_number(l: *mut MtLoader, m: c_int, i: c_int, v: f64) -> c_int;
    fn mt_setting_set_text(l: *mut MtLoader, m: c_int, i: c_int, v: *const c_char) -> c_int;
    fn mt_plan_count(l: *mut MtLoader) -> c_int;
    fn mt_plan_hash(l: *mut MtLoader) -> *const c_char;
    fn mt_plan_write(l: *mut MtLoader, path: *const c_char) -> c_int;
    fn mt_version_satisfies(version: *const c_char, range: *const c_char) -> c_int;
    fn mt_version_compare(a: *const c_char, b: *const c_char) -> c_int;
}

static CORE: Mutex<()> = Mutex::new(());

fn lock() -> MutexGuard<'static, ()> {
    CORE.lock().unwrap_or_else(|e| e.into_inner())
}

extern "C" fn log_line(_user: *mut c_void, _level: c_int, line: *const c_char) {
    eprintln!("{}", text(line));
}

fn text(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    // SAFETY: the core's strings are NUL-terminated and valid until the next call that changes the same mod; this copies at once
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

fn c(s: &str) -> CString {
    CString::new(s.replace('\0', "")).unwrap_or_default()
}

fn c_path(p: &Path) -> CString {
    c(&p.to_string_lossy())
}

/// One mod as the core sees it (MtModInfo), copied.
#[derive(Clone, Debug)]
pub struct ModInfo {
    pub id: String,
    pub name: String,
    pub version: String,
    pub authors: String,
    pub description: String,
    pub license: String,
    /// its file or folder name in mods/
    pub source: String,
    pub flags: i32,
    pub state: i32,
    pub enabled: bool,
    pub message: String,
    pub hook_points: i32,
    pub network: String,
}

/// One setting of a mod (MtSettingInfo) with its value now, copied.
#[derive(Clone, Debug)]
pub struct SettingInfo {
    pub key: String,
    pub label: String,
    pub description: String,
    pub kind: i32,
    pub restart: bool,
    pub min: f64,
    pub max: f64,
    pub step: f64,
    pub choices: Vec<String>,
    /// the value as shown (On / Off, a number, the choice, the text)
    pub text: String,
    /// bool 0/1, the number, or the choice's index
    pub number: f64,
}

/// A loader over one mods folder, scanned once when opened. Holds the core's lock until dropped.
pub struct Core {
    l: *mut MtLoader,
    // the loader keeps pointers to these (modtopia_host.h: "their strings must outlive the loader")
    _strings: Vec<CString>,
    _guard: MutexGuard<'static, ()>,
}

impl Core {
    /// mt_loader_new + mt_loader_scan for `mods_dir`, with the mappings in `mappings_dir`. A missing mods folder scans as no mods.
    pub fn open(mods_dir: &Path, mappings_dir: &Path, host_name: &str) -> Core {
        let guard = lock();
        let strings = vec![c_path(mods_dir), c_path(mappings_dir), c(host_name), c(GAME_VERSION), c(platform_tag())];
        let opt = Options { mods_dir: strings[0].as_ptr(), config_dir: std::ptr::null(), cache_dir: std::ptr::null(), mappings_dir: strings[1].as_ptr(), no_code: 1 };
        let be = Backend {
            size: std::mem::size_of::<Backend>() as u32,
            user: std::ptr::null_mut(),
            host_name: strings[2].as_ptr(),
            game_version: strings[3].as_ptr(),
            game_build_id: std::ptr::null(),
            platform: strings[4].as_ptr(),
            log: Some(log_line),
        };
        // SAFETY: opt and be are valid for the call (the core copies them); the strings they point at live as long as the Core
        let l = unsafe { mt_loader_new(&opt, &be) };
        assert!(!l.is_null(), "mt_loader_new");
        // SAFETY: l is a live loader; -1 (no mods folder) leaves it with no mods, which is what the page shows
        unsafe { mt_loader_scan(l) };
        Core { l, _strings: strings, _guard: guard }
    }

    /// Every mod found: those that load, in load order, then the rest.
    pub fn mods(&self) -> Vec<ModInfo> {
        // SAFETY: l is live; indexes below mt_loader_count are valid; each info is copied before the next call
        let n = unsafe { mt_loader_count(self.l) };
        (0..n)
            .filter_map(|i| {
                let p = unsafe { mt_loader_info(self.l, i) };
                // SAFETY: non-null infos point at the core's MtModInfo for mod i
                let m = unsafe { p.as_ref() }?;
                Some(ModInfo {
                    id: text(m.id),
                    name: text(m.name),
                    version: text(m.version),
                    authors: text(m.authors),
                    description: text(m.description),
                    license: text(m.license),
                    source: text(m.source),
                    flags: m.flags,
                    state: m.state,
                    enabled: m.enabled != 0,
                    message: text(m.message),
                    hook_points: m.hook_points,
                    network: text(m.network),
                })
            })
            .collect()
    }

    /// The mod list's index of a mod, or None.
    pub fn find(&self, id: &str) -> Option<i32> {
        let id = c(id);
        // SAFETY: l is live, id is a valid C string for the call
        let i = unsafe { mt_loader_find(self.l, id.as_ptr()) };
        (i >= 0).then_some(i)
    }

    /// The player's switch, saved to mods/modtopia.toml. Err(code) when it cannot be written.
    pub fn set_enabled(&self, id: &str, on: bool) -> Result<(), i32> {
        let id = c(id);
        ok(unsafe { mt_loader_set_enabled(self.l, id.as_ptr(), on as c_int) })
    }

    /// One place earlier (-1) or later (+1) in the player's order, saved.
    pub fn move_by(&self, id: &str, delta: i32) -> Result<(), i32> {
        let id = c(id);
        ok(unsafe { mt_loader_move(self.l, id.as_ptr(), delta) })
    }

    /// Records the player's yes for this mod's code as it is now (known-code.toml). Err(MT_ERR_UNKNOWN) when it has no code.
    pub fn trust_mod(&self, id: &str) -> Result<(), i32> {
        let id = c(id);
        ok(unsafe { mt_loader_trust_mod(self.l, id.as_ptr()) })
    }

    /// "id@version" of every gameplay mod that is on, in load order.
    pub fn gameplay_mods(&self) -> Vec<String> {
        extern "C" fn add(user: *mut c_void, s: *const c_char) {
            // SAFETY: user is the &mut Vec passed below, alive for the call
            unsafe { &mut *(user as *mut Vec<String>) }.push(text(s));
        }
        let mut out: Vec<String> = Vec::new();
        unsafe { mt_loader_gameplay_mods(self.l, add, &mut out as *mut Vec<String> as *mut c_void) };
        out
    }

    /// The settings of mod `m` (its mod list index), with their values now.
    pub fn settings(&self, m: i32) -> Vec<SettingInfo> {
        let n = unsafe { mt_setting_count(self.l, m) };
        (0..n)
            .filter_map(|i| {
                // SAFETY: i below mt_setting_count; the info and its strings are copied before the next call
                let s = unsafe { mt_setting_info(self.l, m, i).as_ref() }?;
                let choices = (0..s.nchoices.max(0) as usize).map(|k| text(unsafe { *s.choices.add(k) })).collect();
                let mut buf = vec![0 as c_char; 512];
                unsafe { mt_setting_get_text(self.l, m, i, buf.as_mut_ptr(), buf.len()) };
                Some(SettingInfo {
                    key: text(s.key),
                    label: text(s.label),
                    description: text(s.description),
                    kind: s.kind,
                    restart: s.restart != 0,
                    min: s.min,
                    max: s.max,
                    step: s.step,
                    choices,
                    text: text(buf.as_ptr()),
                    number: unsafe { mt_setting_get_number(self.l, m, i) },
                })
            })
            .collect()
    }

    /// A number, bool (0/1) or choice index for setting `i` of mod `m`: clamped by the core and saved to config/<id>.toml.
    pub fn set_number(&self, m: i32, i: i32, v: f64) -> Result<(), i32> {
        ok(unsafe { mt_setting_set_number(self.l, m, i, v) })
    }

    /// A text, key, or choice by its name, saved.
    pub fn set_text(&self, m: i32, i: i32, v: &str) -> Result<(), i32> {
        let v = c(v);
        ok(unsafe { mt_setting_set_text(self.l, m, i, v.as_ptr()) })
    }

    /// How many hook points the mods that are on need for this game version.
    pub fn plan_count(&self) -> i32 {
        unsafe { mt_plan_count(self.l) }
    }

    /// The plan's SHA-256 hex; "" when there are no points.
    pub fn plan_hash(&self) -> String {
        text(unsafe { mt_plan_hash(self.l) })
    }

    /// Writes the hooks file ("modtopia hooks 1"). Err(-1) when it cannot be written.
    pub fn plan_write(&self, path: &Path) -> Result<(), i32> {
        let p = c_path(path);
        ok(unsafe { mt_plan_write(self.l, p.as_ptr()) })
    }
}

impl Drop for Core {
    fn drop(&mut self) {
        // SAFETY: l came from mt_loader_new and is freed once; no code was loaded (no_code), so nothing outlives it
        unsafe { mt_loader_free(self.l) };
    }
}

fn ok(r: c_int) -> Result<(), i32> {
    if r == 0 {
        Ok(())
    } else {
        Err(r)
    }
}

// The version functions are pure (mt_version.c keeps no state): no lock, so they work while a Core is open.

/// mt_version_satisfies: Some(true/false), None for a malformed version or range.
pub fn version_satisfies(version: &str, range: &str) -> Option<bool> {
    let (v, r) = (c(version), c(range));
    match unsafe { mt_version_satisfies(v.as_ptr(), r.as_ptr()) } {
        1 => Some(true),
        0 => Some(false),
        _ => None,
    }
}

/// mt_version_compare: semantic version precedence.
pub fn version_compare(a: &str, b: &str) -> std::cmp::Ordering {
    let (a, b) = (c(a), c(b));
    unsafe { mt_version_compare(a.as_ptr(), b.as_ptr()) }.cmp(&0)
}
