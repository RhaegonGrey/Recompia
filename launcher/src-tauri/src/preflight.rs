// SPDX-License-Identifier: GPL-3.0-or-later
//! Checks before Set up starts: free disk space, write access to the game folder (writecheck.rs), Vulkan, RAM, and a warning
//! for the Intel Core CPUs that crash under long heavy loads. Every check except the disk space and the write access is a
//! warning the player can continue past.
//! `evaluate` is a pure function of `Facts` (tests below); `probe` fills `Facts` from Windows, Linux or macOS.
//!
//! The Intel check and the "gentle" job count follow pokopia-native (MIT), nxs2.py `intel_microcode_warning` and
//! `gentle_jobs` @7c3655d: 13th/14th generation desktop CPUs need Intel's 0x12B microcode (a BIOS update) or sustained
//! all-core loads such as the shader translation can crash Windows.

use crate::{shaders, writecheck};
use serde::Serialize;
use std::path::Path;

pub const GB: u64 = 1 << 30;
/// shader_cache/corpus while translating: about 5 GB in pack files (launcher README), deleted when setup finishes.
pub const CORPUS_BYTES: u64 = 5 * GB;
/// shader_cache/spirv when complete: about 26 GB for the 99,012 blocks of 2.0.1 (launcher README).
pub const SPIRV_BYTES: u64 = 26 * GB;
/// The same shaders packed (shader_archive.rs): SPIRV_BYTES / RATIO_GUESS, the 3.8x the preload pack measured at a lower zstd level
/// than the archive uses, so a ceiling. Replace with the measured size once the first full caches are packed (setup.log says).
pub const PACKED_BYTES: u64 = 7 * GB;
/// A packed setup still writes each translated chunk as loose files before it packs them: the largest chunk (shaders.rs CHUNK, 5000
/// blocks on Linux, about 1.3 GB) and its packed copy are on disk together for a moment.
pub const CHUNK_BYTES: u64 = 2 * GB;
/// The default block count when setup.json has none yet (2.0.1).
pub const DEFAULT_BLOCKS: u64 = 99_012;
/// The game build's generated sources and objects, deleted when it links (pkp_build.py). Not measured: "a few GB" in the
/// Set up text. It is freed before the shaders start, so only the larger of the two counts.
pub const BUILD_BYTES: u64 = 6 * GB;
/// Less physical memory than this: warn (README: 16 GB; Windows reports about 15.7 GiB on a 16 GB PC).
pub const RAM_WARN_BYTES: u64 = 15 * GB;
/// Intel's microcode with the Vmin-shift fix for 13th/14th generation CPUs.
pub const FIXED_MICROCODE: u32 = 0x12B;

#[derive(Serialize, Clone, Copy, PartialEq, Eq, Debug)]
#[serde(rename_all = "lowercase")]
pub enum Level {
    Ok,
    Warn,
    /// Setup cannot start.
    Fail,
}

#[derive(Serialize, Clone, Debug)]
pub struct Check {
    pub id: &'static str,
    pub level: Level,
    pub title: String,
    pub detail: String,
}

#[derive(Serialize, Clone, Debug)]
pub struct Report {
    pub checks: Vec<Check>,
    /// a check failed: Set up must not start
    pub blocked: bool,
    pub warnings: usize,
    /// bytes setup still needs on the game drive
    pub need_bytes: u64,
    /// the CPU is one the gentle build helps
    pub suggest_gentle: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Gpu {
    pub name: String,
    /// (major, minor)
    pub api: (u32, u32),
    /// a software renderer (llvmpipe, WARP): not a GPU for the game
    pub software: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Vulkan {
    /// vulkan-1.dll (libvulkan.so.1) did not load
    NoLoader(String),
    /// the loader is there but made no instance or found no device
    Failed(String),
    Gpus(Vec<Gpu>),
}

#[derive(Clone, Debug)]
pub struct Facts {
    pub free_bytes: Option<u64>,
    pub need_bytes: u64,
    pub drive: String,
    /// the game folder: the write test of it and its setup subfolders
    pub game_dir: String,
    pub write: Result<(), writecheck::WriteError>,
    pub vulkan: Vulkan,
    pub ram_total: Option<u64>,
    pub cpu_brand: Option<String>,
    pub microcode: Option<u32>,
    /// the translated shaders will be packed (Remaining::compact): what the disk message says stays
    pub compact: bool,
    /// macOS: the game runs natively on Metal, so there is no Vulkan driver to check and no /proc to read the RAM from. Linux runs
    /// the game natively too, but on Vulkan and with /proc: it keeps both checks.
    pub macos: bool,
}

/// What of setup is still to do, as far as the disk goes.
#[derive(Clone, Copy, Debug, Default)]
pub struct Remaining {
    pub build_pending: bool,
    pub shaders_complete: bool,
    /// the corpus is extracted and still on disk
    pub corpus_present: bool,
    pub shaders_done: u64,
    pub shaders_total: u64,
    /// the translated shaders go into the shader archive (shaders.rs storage_of)
    pub compact: bool,
}

/// Bytes setup still needs free. The game build's temporary files are gone before the shaders start, so the larger counts.
/// Resumed shader work counts only for what is left: the finished blocks are already on disk.
pub fn disk_needed(r: &Remaining) -> u64 {
    let build = if r.build_pending { BUILD_BYTES } else { 0 };
    let shaders = if r.shaders_complete {
        0
    } else {
        let total = if r.shaders_total == 0 { DEFAULT_BLOCKS } else { r.shaders_total };
        let left = total.saturating_sub(r.shaders_done);
        let kept = if r.compact { PACKED_BYTES } else { SPIRV_BYTES };
        let spirv = (kept as u128 * left as u128 / total as u128) as u64 + if r.compact && left > 0 { CHUNK_BYTES } else { 0 };
        spirv + if r.corpus_present { 0 } else { CORPUS_BYTES }
    };
    build.max(shaders)
}

/// Reads the shader state of a game folder (the marker and the finished rows) into `Remaining`. `compact`: the setup's choice for a
/// cache with nothing translated yet (config loose_shaders off).
pub fn remaining_of(game_dir: &Path, build_pending: bool, compact: bool) -> Remaining {
    let m = shaders::read_marker(game_dir);
    let results = game_dir.join(shaders::SPIRV_REL).join("results.tsv");
    // header + one row per translated block
    let done = std::fs::read(&results).map(|b| b.iter().filter(|&&c| c == b'\n').count().saturating_sub(1) as u64).unwrap_or(0);
    Remaining {
        build_pending,
        shaders_complete: shaders::is_complete(game_dir),
        corpus_present: m.extracted && game_dir.join("shader_cache").join("corpus").join("unique_all.tsv").is_file(),
        shaders_done: done,
        shaders_total: m.blocks as u64,
        compact: shaders::storage_of(game_dir, &m, compact) == "compact",
    }
}

/// (shader jobs, build jobs). Normal: every thread but one (the build gets one more, as before). Gentle: a quarter of the
/// threads, 2 to 8 (pokopia-native's default), so a laptop stays cool and the PC usable.
pub fn jobs_for(cpus: usize, gentle: bool) -> (usize, usize) {
    if gentle {
        let j = (cpus / 4).clamp(2, 8);
        return (j, j);
    }
    let j = cpus.saturating_sub(1).max(1);
    (j, j + 1)
}

/// `Core(TM) i9-13900K`, `14th Gen Intel(R) Core(TM) i7-14700KF`: a 13th or 14th generation Core desktop part. Laptop
/// parts (H, HX, U, P ...) are not on Intel's list; a desktop part has no suffix or K, KF, KS, F, T.
pub fn raptor_lake_desktop(brand: &str) -> bool {
    if !brand.contains("Intel") {
        return false;
    }
    let Some(i) = brand.find("-1") else { return false };
    let model: String = brand[i + 1..].chars().take_while(|c| c.is_ascii_alphanumeric()).collect();
    let digits: String = model.chars().take_while(|c| c.is_ascii_digit()).collect();
    let suffix = &model[digits.len()..];
    let gen_ok = digits.len() == 5 && (digits.starts_with("13") || digits.starts_with("14"));
    // "i9-" must precede: "-1" also appears in other strings
    let tier_ok = brand[..i].trim_end().ends_with(|c: char| c.is_ascii_digit()) && brand[..i].contains(" i");
    gen_ok && tier_ok && matches!(suffix, "" | "K" | "KF" | "KS" | "F" | "T")
}

fn gb(b: u64) -> String {
    format!("{:.1} GB", b as f64 / GB as f64)
}

/// The checks, in the order they are shown. Pure.
pub fn evaluate(f: &Facts) -> Report {
    let mut checks = Vec::new();
    let mut push = |id: &'static str, level: Level, title: &str, detail: String| checks.push(Check { id, level, title: title.into(), detail });

    match f.free_bytes {
        Some(free) if free < f.need_bytes => push(
            "disk",
            Level::Fail,
            "Not enough free disk space",
            format!(
                "{} has {} free and setup needs about {} (the game build, then about 5 GB of shader sources that are deleted at the end and about {} of translated shaders that stay). Free some space, or move the game folder to a bigger drive (Settings, game folder).",
                f.drive,
                gb(free),
                gb(f.need_bytes),
                if f.compact { "7 GB (packed)" } else { "26 GB" }
            ),
        ),
        Some(free) => push("disk", Level::Ok, "Free disk space", format!("{} free on {}; setup needs about {}.", gb(free), f.drive, gb(f.need_bytes))),
        None => push("disk", Level::Warn, "Free disk space unknown", format!("Could not read the free space of {}. Setup needs about {}.", f.drive, gb(f.need_bytes))),
    }

    match &f.write {
        Ok(()) => push("write", Level::Ok, "Game folder", format!("Setup can write in {}.", f.game_dir)),
        Err(e) => push("write", Level::Fail, e.title(), e.message()),
    }

    let mut suggest_gentle = false;
    if f.macos {
        push(
            "native",
            Level::Ok,
            "Native build",
            "The game runs natively on this Mac, on Metal through MoltenVK: there is no Vulkan driver to install and no Proton to run it with.".into(),
        );
    } else {
    match &f.vulkan {
        Vulkan::NoLoader(e) => push(
            "vulkan",
            Level::Warn,
            "Vulkan not found",
            format!("{} did not load ({}). It comes with the graphics driver: install the current driver from NVIDIA, AMD or Intel. Setup can finish without it, but the game cannot start.", sys::VULKAN_LOADER, e),
        ),
        Vulkan::Failed(e) => push(
            "vulkan",
            Level::Warn,
            "No Vulkan GPU found",
            format!("{}. Update the graphics driver. Setup can finish without it, but the game cannot start.", e),
        ),
        Vulkan::Gpus(g) => {
            let real: Vec<&Gpu> = g.iter().filter(|x| !x.software).collect();
            if real.is_empty() {
                push("vulkan", Level::Warn, "No Vulkan GPU found", "Vulkan only offers a software renderer. Update the graphics driver. Setup can finish without it, but the game cannot start.".into());
            } else if real.iter().all(|x| x.api < (1, 3)) {
                let names: Vec<String> = real.iter().map(|x| format!("{} (Vulkan {}.{})", x.name, x.api.0, x.api.1)).collect();
                push("vulkan", Level::Warn, "Vulkan 1.3 needed", format!("{}. The game needs Vulkan 1.3: update the graphics driver.", names.join("; ")));
            } else {
                let names: Vec<String> = real.iter().map(|x| format!("{} (Vulkan {}.{})", x.name, x.api.0, x.api.1)).collect();
                push("vulkan", Level::Ok, "Vulkan", names.join("; "));
            }
        }
    }

    match f.ram_total {
        Some(r) if r < RAM_WARN_BYTES => push(
            "ram",
            Level::Warn,
            "Less than 16 GB of RAM",
            format!("This PC has {}. The game wants 16 GB. Setup still works, but the build runs fewer compiles at once and takes longer.", gb(r)),
        ),
        Some(r) => push("ram", Level::Ok, "Memory", format!("{} of RAM.", gb(r))),
        None => push("ram", Level::Warn, "Memory unknown", "Could not read the amount of RAM.".into()),
    }

    if let Some(brand) = f.cpu_brand.as_deref().filter(|b| raptor_lake_desktop(b)) {
        match f.microcode {
            Some(m) if m >= FIXED_MICROCODE => push("cpu", Level::Ok, "Processor", format!("{} runs microcode {:#x}, which has Intel's fix.", brand.trim(), m)),
            known => {
                suggest_gentle = true;
                let have = known.map(|m| format!("runs microcode {:#x}", m)).unwrap_or_else(|| "has microcode that could not be read".into());
                push(
                    "cpu",
                    Level::Warn,
                    "Intel 13th/14th generation CPU: update the BIOS",
                    format!(
                        "{} {}. These CPUs can become unstable, and crash Windows under long heavy loads like the shader translation (1 to 2 hours on every core), until the motherboard's BIOS carries Intel's microcode fix (0x12B or newer). Update the BIOS from your motherboard maker first, and turn on \"Use fewer cores\" below.",
                        brand.trim(),
                        have
                    ),
                );
            }
        }
    }

    }
    let blocked = checks.iter().any(|c| c.level == Level::Fail);
    let warnings = checks.iter().filter(|c| c.level == Level::Warn).count();
    Report { checks, blocked, warnings, need_bytes: f.need_bytes, suggest_gentle }
}

/// Reads one Vulkan device's `VkPhysicalDeviceProperties` (apiVersion at 0, deviceType at 16, deviceName at 20, 256 bytes).
pub fn parse_gpu(props: &[u8]) -> Option<Gpu> {
    if props.len() < 276 {
        return None;
    }
    let u = |o: usize| u32::from_le_bytes([props[o], props[o + 1], props[o + 2], props[o + 3]]);
    let api = u(0);
    let name_bytes = &props[20..276];
    let end = name_bytes.iter().position(|&b| b == 0).unwrap_or(name_bytes.len());
    Some(Gpu {
        name: String::from_utf8_lossy(&name_bytes[..end]).trim().to_string(),
        api: ((api >> 22) & 0x7F, (api >> 12) & 0x3FF),
        software: u(16) == 4, // VK_PHYSICAL_DEVICE_TYPE_CPU
    })
}

/// "Update Revision" of the registry's CentralProcessor key: eight bytes, the revision in the second dword (the first on
/// some systems that give four).
#[cfg_attr(not(windows), allow(dead_code))]
pub fn microcode_of(raw: &[u8]) -> Option<u32> {
    let at = |o: usize| u32::from_le_bytes([raw[o], raw[o + 1], raw[o + 2], raw[o + 3]]);
    match raw.len() {
        n if n >= 8 => Some(at(4)),
        4..=7 => Some(at(0)),
        _ => None,
    }
}

/// Everything `evaluate` needs, from this PC.
pub fn probe(game_dir: &Path, r: &Remaining) -> Facts {
    let need = disk_needed(r);
    // macOS runs the game natively on Metal: there is no Vulkan driver to find and no /proc to read the RAM from. Linux runs it
    // natively too, but on Vulkan and with /proc, so it keeps both checks.
    let macos = cfg!(target_os = "macos");
    let (brand, microcode) = sys::cpu();
    Facts {
        free_bytes: sys::free_bytes(game_dir),
        need_bytes: need,
        compact: r.compact,
        drive: drive_of(game_dir),
        game_dir: game_dir.to_string_lossy().into_owned(),
        write: writecheck::check(game_dir),
        vulkan: if macos { Vulkan::Gpus(Vec::new()) } else { sys::vulkan() },
        ram_total: if macos { None } else { sys::ram_total() },
        cpu_brand: brand,
        microcode,
        macos,
    }
}

/// Free bytes on the drive of `dir` (or its nearest existing parent).
pub fn free_bytes(dir: &Path) -> Option<u64> {
    sys::free_bytes(dir)
}

/// "C:" of `C:\games\pokopia`, else the path itself.
pub fn drive_of(p: &Path) -> String {
    let s = p.to_string_lossy();
    let s = s.strip_prefix(r"\\?\").unwrap_or(&s);
    match s.as_bytes() {
        [d, b':', ..] if d.is_ascii_alphabetic() => s[..2].to_string(),
        _ => s.to_string(),
    }
}

#[cfg(windows)]
mod sys {
    use super::Vulkan;
    use std::ffi::c_void;
    use std::os::windows::ffi::OsStrExt;
    use std::path::Path;

    #[repr(C)]
    struct MemStatus {
        len: u32,
        load: u32,
        total: u64,
        avail: u64,
        page_total: u64,
        page_avail: u64,
        virt_total: u64,
        virt_avail: u64,
        ext: u64,
    }

    extern "system" {
        fn LoadLibraryExW(name: *const u16, file: *mut c_void, flags: u32) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
        fn FreeLibrary(module: *mut c_void) -> i32;
        fn GetDiskFreeSpaceExW(dir: *const u16, avail: *mut u64, total: *mut u64, free: *mut u64) -> i32;
        fn GlobalMemoryStatusEx(buf: *mut MemStatus) -> i32;
    }
    #[link(name = "advapi32")]
    extern "system" {
        fn RegOpenKeyExW(key: isize, sub: *const u16, opts: u32, sam: u32, out: *mut isize) -> i32;
        fn RegQueryValueExW(key: isize, name: *const u16, res: *mut u32, ty: *mut u32, data: *mut u8, len: *mut u32) -> i32;
        fn RegCloseKey(key: isize) -> i32;
    }

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    pub fn free_bytes(dir: &Path) -> Option<u64> {
        // the folder may not exist yet: ask about the nearest one that does
        let mut d = dir;
        while !d.exists() {
            d = d.parent()?;
        }
        let w: Vec<u16> = d.as_os_str().encode_wide().chain(std::iter::once(0)).collect();
        let mut avail = 0u64;
        let ok = unsafe { GetDiskFreeSpaceExW(w.as_ptr(), &mut avail, std::ptr::null_mut(), std::ptr::null_mut()) };
        (ok != 0).then_some(avail)
    }

    pub fn ram_total() -> Option<u64> {
        let mut m = MemStatus { len: std::mem::size_of::<MemStatus>() as u32, load: 0, total: 0, avail: 0, page_total: 0, page_avail: 0, virt_total: 0, virt_avail: 0, ext: 0 };
        (unsafe { GlobalMemoryStatusEx(&mut m) } != 0).then_some(m.total)
    }

    /// (brand string, microcode revision) from HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0
    pub fn cpu() -> (Option<String>, Option<u32>) {
        const HKLM: isize = 0x8000_0002u32 as i32 as isize;
        const KEY_READ: u32 = 0x20019;
        let mut key = 0isize;
        let sub = wide(r"HARDWARE\DESCRIPTION\System\CentralProcessor\0");
        if unsafe { RegOpenKeyExW(HKLM, sub.as_ptr(), 0, KEY_READ, &mut key) } != 0 {
            return (None, None);
        }
        let read = |name: &str| -> Option<Vec<u8>> {
            let n = wide(name);
            let mut buf = vec![0u8; 512];
            let mut len = buf.len() as u32;
            let mut ty = 0u32;
            let rc = unsafe { RegQueryValueExW(key, n.as_ptr(), std::ptr::null_mut(), &mut ty, buf.as_mut_ptr(), &mut len) };
            (rc == 0).then(|| {
                buf.truncate(len as usize);
                buf
            })
        };
        let brand = read("ProcessorNameString").map(|b| {
            let u: Vec<u16> = b.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).take_while(|&c| c != 0).collect();
            String::from_utf16_lossy(&u)
        });
        let micro = read("Update Revision").and_then(|b| super::microcode_of(&b));
        unsafe { RegCloseKey(key) };
        (brand, micro)
    }

    pub const VULKAN_LOADER: &str = "vulkan-1.dll";

    /// Loads vulkan-1.dll from System32 and lists its devices (super::list_gpus).
    pub fn vulkan() -> Vulkan {
        const LOAD_LIBRARY_SEARCH_SYSTEM32: u32 = 0x800;
        unsafe {
            let dll = LoadLibraryExW(wide("vulkan-1.dll").as_ptr(), std::ptr::null_mut(), LOAD_LIBRARY_SEARCH_SYSTEM32);
            if dll.is_null() {
                return Vulkan::NoLoader(std::io::Error::last_os_error().to_string());
            }
            let gpa = GetProcAddress(dll, b"vkGetInstanceProcAddr\0".as_ptr());
            let out = if gpa.is_null() {
                Vulkan::Failed("vulkan-1.dll has no vkGetInstanceProcAddr".into())
            } else {
                super::list_gpus(std::mem::transmute::<*mut c_void, super::GetProc>(gpa))
            };
            FreeLibrary(dll);
            out
        }
    }
}

/// vkGetInstanceProcAddr
type GetProc = unsafe extern "system" fn(*mut std::ffi::c_void, *const u8) -> *mut std::ffi::c_void;

/// Makes an instance through the loader's vkGetInstanceProcAddr and lists its devices. The loader runs the driver in this
/// process. The Windows and the Linux probe share it.
unsafe fn list_gpus(gpa: GetProc) -> Vulkan {
    use std::ffi::c_void;
    #[repr(C)]
    struct InstanceInfo {
        s_type: u32,
        next: *const c_void,
        flags: u32,
        app: *const c_void,
        layers: u32,
        layer_names: *const *const u8,
        exts: u32,
        ext_names: *const *const u8,
    }
    type Create = unsafe extern "system" fn(*const InstanceInfo, *const c_void, *mut *mut c_void) -> i32;
    type Enum = unsafe extern "system" fn(*mut c_void, *mut u32, *mut *mut c_void) -> i32;
    type Props = unsafe extern "system" fn(*mut c_void, *mut u64);
    type Destroy = unsafe extern "system" fn(*mut c_void, *const c_void);

    let create = gpa(std::ptr::null_mut(), b"vkCreateInstance\0".as_ptr());
    if create.is_null() {
        return Vulkan::Failed("the Vulkan loader has no vkCreateInstance".into());
    }
    let create: Create = std::mem::transmute(create);
    let info = InstanceInfo { s_type: 1, next: std::ptr::null(), flags: 0, app: std::ptr::null(), layers: 0, layer_names: std::ptr::null(), exts: 0, ext_names: std::ptr::null() };
    let mut inst: *mut c_void = std::ptr::null_mut();
    let rc = create(&info, std::ptr::null(), &mut inst);
    if rc != 0 || inst.is_null() {
        return Vulkan::Failed(format!("vkCreateInstance failed (VkResult {}): no working Vulkan driver", rc));
    }
    let enumerate = gpa(inst, b"vkEnumeratePhysicalDevices\0".as_ptr());
    let props = gpa(inst, b"vkGetPhysicalDeviceProperties\0".as_ptr());
    let destroy = gpa(inst, b"vkDestroyInstance\0".as_ptr());
    let mut gpus = Vec::new();
    if !enumerate.is_null() && !props.is_null() {
        let enumerate: Enum = std::mem::transmute(enumerate);
        let props: Props = std::mem::transmute(props);
        let mut n = 0u32;
        if enumerate(inst, &mut n, std::ptr::null_mut()) == 0 && n > 0 {
            let mut devs = vec![std::ptr::null_mut::<c_void>(); n as usize];
            if enumerate(inst, &mut n, devs.as_mut_ptr()) >= 0 {
                for d in devs.iter().take(n as usize) {
                    // VkPhysicalDeviceProperties is 824 bytes
                    let mut buf = [0u64; 128];
                    props(*d, buf.as_mut_ptr());
                    let bytes: Vec<u8> = buf.iter().flat_map(|x| x.to_le_bytes()).collect();
                    if let Some(g) = parse_gpu(&bytes) {
                        gpus.push(g);
                    }
                }
            }
        }
    }
    if !destroy.is_null() {
        let destroy: Destroy = std::mem::transmute(destroy);
        destroy(inst, std::ptr::null());
    }
    if gpus.is_empty() {
        Vulkan::Failed("The Vulkan loader found no graphics device".into())
    } else {
        Vulkan::Gpus(gpus)
    }
}

#[cfg(not(windows))]
mod sys {
    use super::Vulkan;
    use std::path::Path;

    pub const VULKAN_LOADER: &str = "libvulkan.so.1";

    pub fn free_bytes(dir: &Path) -> Option<u64> {
        crate::host::free_bytes(dir)
    }

    pub fn ram_total() -> Option<u64> {
        meminfo_kb(&std::fs::read_to_string("/proc/meminfo").ok()?, "MemTotal").map(|kb| kb * 1024)
    }

    pub fn cpu() -> (Option<String>, Option<u32>) {
        cpuinfo(&std::fs::read_to_string("/proc/cpuinfo").unwrap_or_default())
    }

    /// A `/proc/meminfo` value: `MemTotal:  16351904 kB` -> 16351904.
    pub fn meminfo_kb(text: &str, key: &str) -> Option<u64> {
        text.lines().find_map(|l| l.strip_prefix(key)?.strip_prefix(':')?.split_whitespace().next()?.parse().ok())
    }

    /// (brand string, microcode revision) of the first CPU in `/proc/cpuinfo` ("model name", "microcode : 0x12b").
    pub fn cpuinfo(text: &str) -> (Option<String>, Option<u32>) {
        let field = |key: &str| {
            text.lines().find_map(|l| {
                let (k, v) = l.split_once(':')?;
                (k.trim() == key).then(|| v.trim().to_string())
            })
        };
        let micro = field("microcode").and_then(|m| u32::from_str_radix(m.trim_start_matches("0x"), 16).ok());
        (field("model name"), micro)
    }

    /// Loads libvulkan.so.1 and lists its devices (super::list_gpus). The loader stays loaded: unloading it can unload a
    /// driver (NVIDIA's) that still has threads running.
    pub fn vulkan() -> Vulkan {
        unsafe {
            let lib = libc::dlopen(c"libvulkan.so.1".as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL);
            if lib.is_null() {
                let err = libc::dlerror();
                return Vulkan::NoLoader(if err.is_null() { "not found".into() } else { std::ffi::CStr::from_ptr(err).to_string_lossy().into_owned() });
            }
            let gpa = libc::dlsym(lib, c"vkGetInstanceProcAddr".as_ptr());
            if gpa.is_null() {
                return Vulkan::Failed("libvulkan.so.1 has no vkGetInstanceProcAddr".into());
            }
            super::list_gpus(std::mem::transmute::<*mut libc::c_void, super::GetProc>(gpa))
        }
    }
}
