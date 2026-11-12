// SPDX-License-Identifier: GPL-3.0-or-later
//! First-run shader setup: builds `shader_cache/spirv` next to pkp.exe from the user's own romfs and main.
//!
//! Two existing tools do the work, both shipped in the launcher's `tools/` folder:
//!   1. `tools/shaders/extract_corpus.py` (Python 3 with the `lz4` package) finds every GLSLC output in the romfs and in
//!      exefs/main and writes the distinct shader blocks to `shader_cache/corpus` (pack/<NN>.code, .ctrl, .glslc and
//!      unique.tsv with each block's pack refs: a few large files, issue #194);
//!   2. `tools/spirv_harness/spirv_harness.exe` (self-contained .NET publish) translates them to `<hash>.spv` plus the
//!      `results.tsv` rows the runtime reads.
//!
//! Resumable: the full block list is kept as `corpus/unique_all.tsv`; translation runs in chunks (the harness reads
//! `corpus/unique.tsv`, rewritten per chunk) into `shader_cache/stage`, and a chunk counts only once its `.spv` files are
//! moved into `spirv/` and its rows appended to `spirv/results.tsv`. A block with a row there is never translated again.
//! `shader_cache/setup.json` records what is done. The tools run at below-normal priority.
//! On Linux and macOS the tools run natively: the extractor with the host's Python, the translator as the host's own self-contained
//! publish (host.rs).
//! Compact storage (shader_archive.rs): when the cache has a shader archive, or a new setup asks for one (`Job::compact`), every chunk is
//! moved into the archive as soon as it is committed, so the loose files never pile up. A cache of loose files stays loose until the
//! player makes it smaller (main.rs compact_shaders).
//! Its only crate-local imports are host.rs (with download.rs) and shader_archive.rs, so `examples/shader_setup.rs` can build them alone.

use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::mpsc;
use std::time::Duration;

/// Folder (in the game folder) the runtime opens by default; also what setup writes as `[nvn] shader_cache`.
pub const SPIRV_REL: &str = "shader_cache/spirv";
/// `[nvn] dump_missing`: the runtime writes the blocks the cache has no translation for here (the game builds some while
/// it runs, so the romfs corpus cannot have them). The launcher translates them after the game exits.
pub const MISSING_REL: &str = "shader_cache/missing";
/// Every run-time block seen so far, kept with its code (manifest.tsv, code_<hash>.bin, ctrl_<hash>.bin), so each Play
/// can check that all of them are translated without running the game.
pub const RUNTIME_REL: &str = "shader_cache/runtime";
/// Run-time blocks a translator fix changes whose old translation is in the shader archive (it cannot be deleted one by one there):
/// one hash per line. translate_missing translates them again, as if they had no translation, and empties the list.
const RETRANSLATE_REL: &str = "shader_cache/runtime/retranslate.txt";
/// Blocks per translator run. Windows runs more, smaller chunks so a pause or a crash loses less; the POSIX hosts run fewer, larger
/// ones (a native process start is cheap, and each chunk re-reads the corpus index).
const CHUNK: usize = if cfg!(windows) { 1000 } else { 5000 };
/// Workers for the extractor. It maps each asset file into memory and decompresses it, so one worker per CPU thread can
/// thrash a machine's memory and disk (issue #194: 15 workers on a hard drive); a few already saturate the disk. The
/// translator (spirv_harness) keeps the full `job.jobs`.
const EXTRACT_JOBS: usize = 4;
/// Translator revision. Raised when a translator fix changes shaders that are already translated: a finished cache of a lower
/// revision counts as unfinished, and Set up translates those shaders again (`stale_blocks`), the others are kept.
/// 1: an atomic whose result is read once ran twice (every such counter added its amount twice; spirv-call-args.patch).
/// 2: LOP3.LUT with a predicate destination read its sources one operand off and left the predicate false (#242).
pub const TRANSLATOR_REV: u32 = 2;
/// The blocks whose translation revision 2 changes: every block of the 2.0.1 corpus whose SPIR-V differs between the
/// translator before and after the #242 fix (tools/shaders/ampere/frontend/AmpereEmit.cs EmitLop3Ampere), one hash a line.
const REV2_BLOCKS: &str = include_str!("rev2_blocks.txt");
/// Threads for the stale-shader check (`stale_blocks`): it only reads small files, so the disk sets the pace, not the CPU.
const STALE_SCAN_THREADS: usize = 8;

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub struct Marker {
    /// romfs the corpus was extracted from
    pub romfs: String,
    pub extracted: bool,
    pub blocks: usize,
    pub complete: bool,
    /// TRANSLATOR_REV of the finished cache (0: before revisions were recorded)
    pub translator: u32,
    /// "compact" (shader archive) or "loose": decided once, when this cache's first setup starts (Job::compact), so a setup that stops
    /// before its first chunk is packed keeps the choice. Empty: a cache from before the archive (it is loose). Packing and unpacking
    /// in Settings change it (set_storage).
    pub storage: String,
}

/// Where a setup job stands. The page gets the lowercase word: "idle", "running", "done", "error", "cancelled".
#[derive(Serialize, Clone, Copy, Default, PartialEq, Eq, Debug)]
#[serde(rename_all = "lowercase")]
#[allow(dead_code)] // the launcher builds every variant; the shader_setup example includes this file and builds only Running
pub enum RunState {
    #[default]
    Idle,
    Running,
    Done,
    Error,
    Cancelled,
}

impl std::fmt::Display for RunState {
    /// The same word the page gets (the setup log prints it).
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            RunState::Idle => "idle",
            RunState::Running => "running",
            RunState::Done => "done",
            RunState::Error => "error",
            RunState::Cancelled => "cancelled",
        })
    }
}

#[derive(Serialize, Clone, Default)]
pub struct Progress {
    pub state: RunState,
    /// "check", "extract", "translate", "finish"
    pub phase: String,
    pub done: u64,
    pub total: u64,
    pub message: String,
}

pub struct Tools {
    pub python: PathBuf,
    pub extract: PathBuf,
    pub harness: PathBuf,
}

/// Where setup writes its log lines (the launcher's setup.log); a closure so this file needs no crate-local import.
pub type LogFn = std::sync::Arc<dyn Fn(&str) + Send + Sync>;

pub struct Job {
    pub game_dir: PathBuf,
    pub romfs: PathBuf,
    /// The base game's romfs layered under `romfs` (an update dumped without the base game's files under it); None: romfs alone.
    pub romfs_base: Option<PathBuf>,
    pub main: PathBuf,
    pub jobs: usize,
    /// "Gentle build": the tools run at idle priority (the default is below normal).
    pub idle: bool,
    pub log: Option<LogFn>,
    /// Test runs: translate at most this many blocks (0 = all); the setup is then never marked complete.
    pub limit: usize,
    /// A new cache goes into the shader archive (shader_archive.rs). Only decides for a cache with no translated shader yet: an existing
    /// cache keeps the storage it has (one with an archive stays compact, one of loose files stays loose).
    pub compact: bool,
}

impl Job {
    /// What the marker records as the extracted romfs: the romfs, and the base romfs under it when there is one.
    pub fn romfs_key(&self) -> String {
        let f = |p: &Path| p.to_string_lossy().replace('\\', "/");
        match &self.romfs_base {
            Some(b) => format!("{} over {}", f(&self.romfs), f(b)),
            None => f(&self.romfs),
        }
    }
}

pub fn cache_dir(game_dir: &Path) -> PathBuf {
    game_dir.join("shader_cache")
}

pub fn read_marker(game_dir: &Path) -> Marker {
    fs::read_to_string(cache_dir(game_dir).join("setup.json")).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

fn write_marker(game_dir: &Path, m: &Marker) -> Result<(), String> {
    // through a temporary file: a marker cut short by a crash would read back as no progress and start the hours-long setup over
    let p = cache_dir(game_dir).join("setup.json");
    let tmp = cache_dir(game_dir).join("setup.json.tmp");
    fs::write(&tmp, serde_json::to_string_pretty(m).unwrap_or_default()).map_err(|e| format!("cannot write {}: {}", tmp.display(), e))?;
    fs::rename(&tmp, &p).map_err(|e| format!("cannot replace {}: {}", p.display(), e))
}

/// How this cache is (or will be) stored, with `choice` the setup's Job::compact: a cache with an archive is compact, otherwise the
/// storage setup.json recorded, otherwise (nothing decided yet) the choice when no shader is translated yet, else loose. Setup and
/// the preflight's disk estimate both ask this.
pub fn storage_of(game_dir: &Path, marker: &Marker, choice: bool) -> &'static str {
    if crate::shader_archive::exists(game_dir) || marker.storage == "compact" {
        "compact"
    } else if marker.storage.is_empty() && choice && crate::shader_archive::loose_files(game_dir).is_empty() {
        "compact"
    } else {
        "loose"
    }
}

/// Records how the cache is stored ("compact" or "loose"), after Settings packed or unpacked it.
pub fn set_storage(game_dir: &Path, storage: &str) -> Result<(), String> {
    let mut m = read_marker(game_dir);
    m.storage = storage.to_string();
    write_marker(game_dir, &m)
}

/// True when a finished setup's cache is in place.
pub fn is_complete(game_dir: &Path) -> bool {
    let m = read_marker(game_dir);
    m.complete && m.translator >= TRANSLATOR_REV && game_dir.join(SPIRV_REL).join("results.tsv").is_file()
}

/// Records a finished cache (the macOS setup translates it itself, outside shaders::run): `is_complete` then says so and
/// Set up does not run again until the RomFS or the translator changes.
pub fn mark_complete(game_dir: &Path, romfs: &str, blocks: usize) -> Result<(), String> {
    let m = Marker { romfs: romfs.to_string(), extracted: true, blocks, complete: true, translator: TRANSLATOR_REV, storage: "loose".into() };
    write_marker(game_dir, &m)
}

/// True when the cache was finished by an older translator that a later fix changes: Set up has to run once more.
pub fn needs_retranslation(game_dir: &Path) -> bool {
    let m = read_marker(game_dir);
    m.complete && m.translator < TRANSLATOR_REV
}

/// The blocks of `rows` (results.tsv rows) whose translation by revision `rev` the later revisions change. Before 1: every
/// module with an atomic instruction (OpAtomicExchange..OpAtomicXor, opcodes 229-242). Before 2: the blocks of REV2_BLOCKS,
/// and every block the game built while it ran (not in `corpus`, so not in the list; a few hundred, among them the lighting
/// shader that draws the #242 outline).
/// The rev < 1 check reads every translated shader (~99,000 files): on a hard drive with an antivirus scanning each file that
/// took longer than players waited, with no progress shown (#269). So it runs on several threads, reports `progress(done, total)`
/// and stops on `cancel` (None).
fn stale_blocks(
    spirv: &Path,
    archive: &crate::shader_archive::Archive,
    rows: &[String],
    corpus: &HashSet<&str>,
    rev: u32,
    cancel: &AtomicBool,
    mut progress: impl FnMut(u64, u64),
) -> Option<HashSet<String>> {
    let mut out = HashSet::new();
    if rev >= TRANSLATOR_REV {
        return Some(out);
    }
    if rev < 2 {
        let list: HashSet<&str> = REV2_BLOCKS.lines().map(str::trim).filter(|l| !l.is_empty()).collect();
        out.extend(rows.iter().map(|r| first_col(r)).filter(|h| list.contains(h) || !corpus.contains(h)).map(str::to_string));
    }
    if rev >= 1 {
        return Some(out);
    }
    let todo: Vec<&str> = rows.iter().map(|r| first_col(r)).filter(|h| !out.contains(*h)).collect();
    let total = todo.len() as u64;
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4).clamp(1, STALE_SCAN_THREADS);
    let per = todo.len().div_ceil(threads).max(1);
    let (checked, running) = (AtomicU64::new(0), AtomicUsize::new(0));
    let found: Vec<String> = std::thread::scope(|s| {
        let parts: Vec<_> = todo
            .chunks(per)
            .map(|part| {
                running.fetch_add(1, Ordering::SeqCst);
                let (checked, running) = (&checked, &running);
                s.spawn(move || {
                    let mut hits = Vec::new();
                    for h in part {
                        if cancel.load(Ordering::SeqCst) {
                            break;
                        }
                        if crate::shader_archive::read_translation(spirv, archive, h).is_some_and(|b| has_atomic(&b)) {
                            hits.push(h.to_string());
                        }
                        checked.fetch_add(1, Ordering::Relaxed);
                    }
                    running.fetch_sub(1, Ordering::SeqCst);
                    hits
                })
            })
            .collect();
        while running.load(Ordering::SeqCst) > 0 {
            progress(checked.load(Ordering::Relaxed), total);
            std::thread::sleep(Duration::from_millis(250));
        }
        parts.into_iter().flat_map(|p| p.join().unwrap_or_default()).collect()
    });
    if cancel.load(Ordering::SeqCst) {
        return None;
    }
    progress(total, total);
    out.extend(found);
    Some(out)
}

/// True when the SPIR-V module has an atomic instruction (OpAtomicExchange..OpAtomicXor, opcodes 229-242).
fn has_atomic(b: &[u8]) -> bool {
    let w: Vec<u32> = b.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
    let mut i = 5;
    while i < w.len() {
        let (wc, op) = ((w[i] >> 16) as usize, w[i] & 0xffff);
        if wc == 0 {
            return false;
        }
        if (229..=242).contains(&op) {
            return true;
        }
        i += wc;
    }
    false
}

fn quiet(cmd: &mut Command, idle: bool) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, or IDLE_PRIORITY_CLASS when idle (the tools' own child processes inherit it)
        cmd.creation_flags(0x0800_0000 | if idle { 0x0000_0040 } else { 0x0000_4000 });
    }
    #[cfg(not(windows))]
    crate::host::lower_priority(cmd, idle);
}

fn python_has_lz4(py: &Path) -> bool {
    let mut c = crate::host::command(py);
    c.args(["-I", "-B", "-c", "import lz4.block"]).stdout(Stdio::null()).stderr(Stdio::null());
    quiet(&mut c, false);
    c.status().map(|s| s.success()).unwrap_or(false)
}

fn on_path(exe: &str) -> Vec<PathBuf> {
    std::env::var_os("PATH").map(|p| std::env::split_paths(&p).map(|d| d.join(exe)).filter(|f| f.is_file()).collect()).unwrap_or_default()
}

/// The files of tools/spirv_harness without which the translator cannot start.
const HARNESS_FILES: &[&str] = &[
    "spirv_harness.exe",
    "spirv_harness.dll",
    "spirv_harness.deps.json",
    "spirv_harness.runtimeconfig.json",
    "hostfxr.dll",
    "hostpolicy.dll",
    "coreclr.dll",
];

/// Finds the tools in `tools_dir` (the launcher's `tools/` folder). Python: `tools/python/python.exe` (Linux:
/// `tools/linux/python/bin/python3`), else the first python.exe (python3) on PATH that has `lz4`. Every missing piece is named
/// in the error.
pub fn find_tools(tools_dir: &Path) -> Result<Tools, String> {
    let mut missing = Vec::new();
    let harness_dir = tools_dir.join("spirv_harness");
    let harness = harness_dir.join("spirv_harness.exe");
    // what the self-contained .NET publish needs to start at all; an empty file is a cut-off copy or extraction (#366)
    let broken: Vec<&str> = HARNESS_FILES.iter().copied().filter(|f| fs::metadata(harness_dir.join(f)).map_or(true, |m| m.len() == 0)).collect();
    if !broken.is_empty() {
        missing.push(format!("the shader translator ({}: {} missing or empty)", harness_dir.display(), broken.join(", ")));
    }
    let extract = tools_dir.join("shaders").join("extract_corpus.py");
    if !extract.is_file() {
        missing.push(format!("the shader extractor ({})", extract.display()));
    }
    let bundled = crate::host::python(tools_dir);
    let mut candidates = vec![bundled.clone()];
    candidates.extend(on_path(if cfg!(windows) { "python.exe" } else { "python3" }));
    let python = candidates.into_iter().filter(|p| p.is_file()).find(|p| python_has_lz4(p));
    if python.is_none() {
        missing.push(format!(
            "Python 3 with the lz4 package ({}, or python.exe on PATH with `pip install lz4`)",
            bundled.display()
        ));
    }
    if !missing.is_empty() {
        return Err(format!("Shader setup cannot run, missing: {}. Re-extract the launcher's release folder so tools/ is complete.", missing.join("; ")));
    }
    Ok(Tools { python: python.unwrap(), extract, harness })
}

/// A progress line of a tool ("scan: 12 of 99", "  500/1000  3.2 s"): counted by the progress bar, not worth a log line.
fn is_progress_line(l: &str) -> bool {
    l.starts_with("scan: ") || l.trim().split('/').next().map_or(false, |x| !x.is_empty() && x.bytes().all(|b| b.is_ascii_digit()) && l.contains('/'))
}

/// Runs a tool, handing each stdout line to `line`; kills it when `cancel` is set. Ok(true) = exit 0, Ok(false) = cancelled.
/// `log` gets the command line, the tool's stderr and non-progress stdout, and on failure the end of its output.
/// `limit`: killed after that long, an error.
pub(crate) fn run_tool(mut cmd: Command, cancel: &AtomicBool, idle: bool, log: Option<&LogFn>, limit: Option<Duration>, mut line: impl FnMut(&str)) -> Result<bool, String> {
    quiet(&mut cmd, idle);
    let name = cmd.get_program().to_string_lossy().to_string();
    let say = |t: &str| {
        if let Some(f) = log {
            f(t)
        }
    };
    say(&format!("run: {} {}", name, cmd.get_args().map(|a| a.to_string_lossy().to_string()).collect::<Vec<_>>().join(" ")));
    let mut child: Child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| format!("could not start {}: {}", name, e))?;
    let (tx, rx) = mpsc::channel::<String>();
    let out = child.stdout.take().unwrap();
    let tx2 = tx.clone();
    std::thread::spawn(move || BufReader::new(out).lines().map_while(Result::ok).for_each(|l| drop(tx2.send(l))));
    let err = child.stderr.take().unwrap();
    let (etx, erx) = mpsc::channel::<String>();
    let elog = log.cloned();
    std::thread::spawn(move || {
        let mut tail: Vec<String> = Vec::new();
        for l in BufReader::new(err).lines().map_while(Result::ok) {
            if let Some(f) = &elog {
                f(&format!("stderr: {}", l));
            }
            tail.push(l);
            if tail.len() > 12 {
                tail.remove(0);
            }
        }
        let _ = etx.send(tail.join("\n"));
    });
    drop(tx);
    let deadline = limit.map(|d| std::time::Instant::now() + d);
    let mut out_tail: Vec<String> = Vec::new();
    loop {
        match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(l) => {
                if !is_progress_line(&l) {
                    say(&format!("out: {}", l));
                }
                out_tail.push(l.clone());
                if out_tail.len() > 12 {
                    out_tail.remove(0);
                }
                line(&l)
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                // stdout closed: the process is ending. Polled, so Pause and the time limit still apply while it does
                if let Some(st) = child.try_wait().map_err(|e| e.to_string())? {
                    if cancel.load(Ordering::SeqCst) {
                        say(&format!("stopped: {} (paused by the player)", name));
                        return Ok(false);
                    }
                    if st.success() {
                        say(&format!("done: {}", name));
                        return Ok(true);
                    }
                    let tail = erx.recv_timeout(Duration::from_secs(5)).unwrap_or_default();
                    say(&format!("FAILED: {} ({}). Last output:\n{}\nLast error output:\n{}", name, st, out_tail.join("\n"), tail));
                    return Err(format!("{} failed ({}). {}", name, st, tail.trim()));
                }
                std::thread::sleep(Duration::from_millis(200));
            }
        }
        if cancel.load(Ordering::SeqCst) {
            crate::host::kill(&mut child);
            let _ = child.wait();
            say(&format!("stopped: {} (paused by the player)", name));
            return Ok(false);
        }
        if deadline.is_some_and(|d| std::time::Instant::now() >= d) {
            crate::host::kill(&mut child);
            let _ = child.wait();
            say(&format!("stopped: {} (still running after {} s)", name, limit.unwrap_or_default().as_secs()));
            return Err(format!("{} did not end in time", name));
        }
    }
}

/// Header + rows of a tsv file (rows without the trailing newline).
fn read_tsv(p: &Path) -> Result<(String, Vec<String>), String> {
    let text = fs::read_to_string(p).map_err(|e| format!("cannot read {}: {}", p.display(), e))?;
    let mut it = text.lines();
    let header = it.next().unwrap_or("").to_string();
    Ok((header, it.filter(|l| !l.is_empty()).map(|l| l.to_string()).collect()))
}

fn first_col(row: &str) -> &str {
    row.split('\t').next().unwrap_or("")
}

/// Moves a harness run's .spv files from `stage` into `spirv`, then appends its rows to spirv/results.tsv (the rows are
/// what make a block count as done, so they go last). Returns the number of rows.
fn commit_stage(stage: &Path, spirv: &Path) -> Result<usize, String> {
    let results = spirv.join("results.tsv");
    let (rheader, rrows) = read_tsv(&stage.join("results.tsv"))?;
    for r in &rrows {
        let h = first_col(r);
        let src = stage.join(format!("{}.spv", h));
        if src.is_file() {
            let dst = spirv.join(format!("{}.spv", h));
            let _ = fs::remove_file(&dst);
            fs::rename(&src, &dst).map_err(|e| format!("cannot move {}: {}", src.display(), e))?;
        }
    }
    let new_file = !results.is_file();
    let mut out = fs::OpenOptions::new().create(true).append(true).open(&results).map_err(|e| format!("cannot write {}: {}", results.display(), e))?;
    let mut text = String::new();
    if new_file {
        text.push_str(&rheader);
        text.push('\n');
    }
    for r in &rrows {
        text.push_str(r);
        text.push('\n');
    }
    out.write_all(text.as_bytes()).map_err(|e| format!("cannot write {}: {}", results.display(), e))?;
    Ok(rrows.len())
}

fn col(row: &str, k: usize) -> &str {
    row.split('\t').nth(k).unwrap_or("")
}

/// True when no run-time block was ever collected for this cache: the warm-up run has not happened yet.
pub fn needs_warm_up(game_dir: &Path) -> bool {
    !game_dir.join(RUNTIME_REL).join("manifest.tsv").is_file()
}

/// Before every start: moves what the game dumped to MISSING_REL (by any run, also one started without the launcher)
/// into RUNTIME_REL, then translates every block listed there that has no .spv in the cache. Ok(n) = blocks translated.
pub fn translate_missing(game_dir: &Path, tools_dir: &Path) -> Result<usize, String> {
    let missing = game_dir.join(MISSING_REL);
    let runtime = game_dir.join(RUNTIME_REL);
    let list = runtime.join("manifest.tsv");
    // manifest columns (runtime nvn_shaders.cpp dump_missing_block): 10 = hash, 11 = code file, 12 = control file
    if let Ok((header, rows)) = read_tsv(&missing.join("manifest.tsv")) {
        fs::create_dir_all(&runtime).map_err(|e| format!("cannot create {}: {}", runtime.display(), e))?;
        let mut known: HashSet<String> = read_tsv(&list).map(|(_, r)| r.iter().map(|x| col(x, 10).to_string()).collect()).unwrap_or_default();
        let mut text = if fs::metadata(&list).map(|m| m.len() == 0).unwrap_or(true) { format!("{}\n", header) } else { String::new() };
        for r in &rows {
            // the same block twice (several runs appended to one manifest before it was merged): the first one counts
            if !known.insert(col(r, 10).to_string()) {
                continue;
            }
            for f in [col(r, 11), col(r, 12)] {
                fs::rename(missing.join(f), runtime.join(f)).map_err(|e| format!("cannot move {}: {}", f, e))?;
            }
            text.push_str(r);
            text.push('\n');
        }
        let mut out = fs::OpenOptions::new().create(true).append(true).open(&list).map_err(|e| format!("cannot write {}: {}", list.display(), e))?;
        out.write_all(text.as_bytes()).map_err(|e| format!("cannot write {}: {}", list.display(), e))?;
        let _ = fs::remove_dir_all(&missing);
    }

    let spirv = game_dir.join(SPIRV_REL);
    let Ok((header, rows)) = read_tsv(&list) else { return Ok(0) };
    let archive = crate::shader_archive::Archive::open(game_dir);
    let again: HashSet<String> = fs::read_to_string(game_dir.join(RETRANSLATE_REL)).map(|t| t.lines().map(|l| l.trim().to_string()).collect()).unwrap_or_default();
    // ponytail: a block the translator fails on has no .spv and is tried again at every start (a second or two)
    let todo: Vec<&String> = rows
        .iter()
        .filter(|r| again.contains(col(r, 10)) || !crate::shader_archive::has_translation(&spirv, &archive, col(r, 10)))
        .collect();
    if todo.is_empty() {
        let _ = fs::remove_file(game_dir.join(RETRANSLATE_REL));
        return Ok(0);
    }
    // the harness reads the code and control files relative to its manifest: absolute paths (as the translator sees them)
    // point it at RUNTIME_REL
    let input = cache_dir(game_dir).join("runtime_todo");
    let stage = cache_dir(game_dir).join("stage_missing");
    let _ = fs::remove_dir_all(&input);
    let _ = fs::remove_dir_all(&stage);
    fs::create_dir_all(&input).map_err(|e| format!("cannot create {}: {}", input.display(), e))?;
    let mut text = format!("{}\n", header);
    for r in &todo {
        let mut f: Vec<String> = r.split('\t').map(String::from).collect();
        for k in [11, 12] {
            if let Some(v) = f.get_mut(k) {
                *v = runtime.join(&*v).to_string_lossy().replace('\\', "/");
            }
        }
        text.push_str(&f.join("\t"));
        text.push('\n');
    }
    fs::write(input.join("manifest.tsv"), text).map_err(|e| format!("cannot write {}: {}", input.display(), e))?;
    let jobs = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2);
    let exe = tools_dir.join("spirv_harness").join("spirv_harness.exe");
    let mut mode = crate::harness::Mode::Fast;
    if !crate::harness::run(&exe, &input, &stage, jobs, &mut mode, &AtomicBool::new(false), false, None, |_| {})? {
        return Err("the shader translator stopped".into());
    }
    let n = commit_stage(&stage, &spirv)?;
    let _ = fs::remove_dir_all(&stage);
    let _ = fs::remove_dir_all(&input);
    let _ = fs::remove_file(game_dir.join(RETRANSLATE_REL));
    // a compact cache stays compact: the new translations join the archive (a small segment)
    if crate::shader_archive::exists(game_dir) {
        let hashes: Vec<String> = todo.iter().map(|r| col(r, 10).to_string()).collect();
        crate::shader_archive::compact(game_dir, Some(&hashes), &AtomicBool::new(false), |_, _| {})?;
    }
    Ok(n)
}

/// Seconds the warm-up run plays. The opening asks for its run-time blocks about 3 to 25 s after start.
pub const WARMUP_SECONDS: u32 = 60;

/// Plays the game once without a window or sound, for WARMUP_SECONDS, so the blocks it builds while it runs (the romfs
/// corpus cannot have them) are collected and translated before a real start. A fresh save folder in the cache, so the
/// player's own save is never touched. Ok(None) = cancelled, else how many blocks were translated.
pub fn warm_up(game_dir: &Path, tools_dir: &Path, cancel: &AtomicBool) -> Result<Option<usize>, String> {
    let scratch = cache_dir(game_dir).join("warmup");
    let _ = fs::remove_dir_all(&scratch);
    fs::create_dir_all(scratch.join("save")).map_err(|e| format!("cannot create {}: {}", scratch.display(), e))?;
    if let Some(install) = tools_dir.parent() {
        crate::host::sync_runtime_files(install, game_dir)?;
    }
    let mut cmd = std::process::Command::new(game_dir.join("pkp.exe"));
    cmd.current_dir(game_dir).arg("--headless").args(["--max-seconds", &WARMUP_SECONDS.to_string()]).arg("--log").arg(scratch.join("warmup.log"));
    // environment variables win over pkp.ini
    cmd.env("NXRT_FS_SAVE_DIR", scratch.join("save")).env("NXRT_NVN_DUMP_MISSING", MISSING_REL).env("NXRT_AUDIO_SINK", "null");
    // it ends with exit code 124 when the time is up: any end is fine, a crash only means fewer blocks were collected. One
    // still running 4 minutes later (a first start also translates the first blocks) is stopped.
    if let Ok(false) = run_tool(cmd, cancel, false, None, Some(Duration::from_secs(WARMUP_SECONDS as u64 + 240)), |_| {}) {
        return Ok(None);
    }
    let n = translate_missing(game_dir, tools_dir)?;
    // A run that collected nothing is retried at the next Play/Set up, but only twice, so a game that never dumps a run-time
    // block does not warm up on every start. The retries are counted in a small file next to the manifest.
    let list = game_dir.join(RUNTIME_REL).join("manifest.tsv");
    if list.is_file() {
        let _ = fs::remove_file(game_dir.join(RUNTIME_REL).join("warmup_empty"));
    } else {
        let counter = game_dir.join(RUNTIME_REL).join("warmup_empty");
        let tried = fs::read_to_string(&counter).ok().and_then(|s| s.trim().parse::<u32>().ok()).unwrap_or(0) + 1;
        let _ = fs::create_dir_all(game_dir.join(RUNTIME_REL));
        if tried >= 3 {
            fs::write(&list, "").map_err(|e| format!("cannot write {}: {}", list.display(), e))?;
            let _ = fs::remove_file(&counter);
        } else {
            fs::write(&counter, tried.to_string()).map_err(|e| format!("cannot write {}: {}", counter.display(), e))?;
        }
    }
    let _ = fs::remove_dir_all(&scratch);
    Ok(Some(n))
}

/// What every start and every setup does: the warm-up run once, afterwards only the check of the collected blocks.
pub fn prepare(game_dir: &Path, tools_dir: &Path, cancel: &AtomicBool) -> Result<Option<usize>, String> {
    if needs_warm_up(game_dir) {
        warm_up(game_dir, tools_dir, cancel)
    } else {
        translate_missing(game_dir, tools_dir).map(Some)
    }
}

/// The whole setup. `report` gets every progress change. Ok(true) = finished (or the limit reached), Ok(false) = cancelled.
pub fn run(job: &Job, tools: &Tools, cancel: &AtomicBool, mut report: impl FnMut(Progress)) -> Result<bool, String> {
    let cache = cache_dir(&job.game_dir);
    let corpus = cache.join("corpus");
    let stage = cache.join("stage");
    let spirv = job.game_dir.join(SPIRV_REL);
    fs::create_dir_all(&spirv).map_err(|e| format!("cannot create {}: {}", spirv.display(), e))?;
    crate::shader_archive::clean(&job.game_dir);
    let romfs_s = job.romfs_key();
    let mut marker = read_marker(&job.game_dir);
    if marker.romfs != romfs_s {
        marker = Marker { romfs: romfs_s.clone(), ..Default::default() };
    }
    // the storage this cache keeps (Job::compact): decided once, when nothing is translated yet; a cache with an archive is compact
    marker.storage = storage_of(&job.game_dir, &marker, job.compact).into();
    let compact = marker.storage == "compact";
    if let Some(l) = &job.log {
        l(&format!("shader storage: {}", if compact { "compact (shader archive)" } else { "loose .spv files" }));
    }
    marker.complete = false;
    write_marker(&job.game_dir, &marker)?;
    let extract_jobs = job.jobs.clamp(1, EXTRACT_JOBS).to_string();
    let p = |phase: &str, done: u64, total: u64, message: String| Progress { state: RunState::Running, phase: phase.into(), done, total, message };

    // 1. extract (skipped when this romfs was already extracted)
    let all = corpus.join("unique_all.tsv");
    if !(marker.extracted && all.is_file()) {
        report(p("extract", 0, 0, "Finding the game's shaders in your game files...".into()));
        let _ = fs::remove_file(corpus.join("unique.tsv"));
        let mut cmd = crate::host::command(&tools.python);
        cmd.arg("-I").arg("-B").arg(&tools.extract).arg("--romfs").arg(&job.romfs).arg("--main").arg(&job.main).arg("--out").arg(&corpus);
        if let Some(b) = &job.romfs_base {
            cmd.arg("--romfs-base").arg(b);
        }
        cmd.args(["--jobs", &extract_jobs, "--progress"]);
        let ok = run_tool(cmd, cancel, job.idle, job.log.as_ref(), None, |l| {
            if let Some(rest) = l.strip_prefix("scan: ") {
                let mut it = rest.split_whitespace().filter_map(|x| x.parse::<u64>().ok());
                if let (Some(d), Some(t)) = (it.next(), it.next()) {
                    report(p("extract", d, t, format!("Scanning game files: {} of {}", d, t)));
                }
            }
        })?;
        if !ok {
            return Ok(false);
        }
        fs::rename(corpus.join("unique.tsv"), &all).map_err(|e| format!("the extractor wrote no block list: {}", e))?;
        marker.extracted = true;
        write_marker(&job.game_dir, &marker)?;
    }

    // 2. translate what spirv/results.tsv does not list yet
    let (uheader, urows) = read_tsv(&all)?;
    marker.blocks = urows.len();
    write_marker(&job.game_dir, &marker)?;
    let results = spirv.join("results.tsv");
    let (_, rrows) = if results.is_file() { read_tsv(&results)? } else { (String::new(), Vec::new()) };
    let mut done: HashSet<String> = rrows.iter().map(|r| first_col(r).to_string()).collect();
    // a cache of an older translator: the blocks a later fix changes are translated again. Those of the corpus here; those the
    // game built while it ran (not in the corpus) lose their .spv, so translate_missing (prepare, after setup) does them.
    let archive = crate::shader_archive::Archive::open(&job.game_dir);
    let corpus_rows: HashSet<&str> = urows.iter().map(|r| first_col(r)).collect();
    if marker.translator < TRANSLATOR_REV && !rrows.is_empty() {
        report(p("recheck", 0, 0, "Checking the shaders an earlier version translated...".into()));
        if let Some(l) = &job.log {
            l(&format!("checking {} translated shaders for translator revision {} -> {}", rrows.len(), marker.translator, TRANSLATOR_REV));
        }
    }
    // setup.log gets a line per 5% from the reports (main.rs), so a slow disk shows up there too
    let Some(stale) = stale_blocks(&spirv, &archive, &rrows, &corpus_rows, marker.translator, cancel, |d, t| {
        report(p("recheck", d, t, format!("Checking the shaders an earlier version translated: {} of {}", d, t)))
    }) else {
        return Ok(false);
    };
    if !stale.is_empty() {
        let mut again = String::new();
        for h in &stale {
            done.remove(h);
            if !corpus_rows.contains(h.as_str()) {
                let _ = fs::remove_file(spirv.join(format!("{}.spv", h)));
                if archive.contains(h) {
                    again.push_str(h);
                    again.push('\n');
                }
            }
        }
        if !again.is_empty() {
            let p = job.game_dir.join(RETRANSLATE_REL);
            let _ = fs::create_dir_all(job.game_dir.join(RUNTIME_REL));
            let mut f = fs::OpenOptions::new().create(true).append(true).open(&p).map_err(|e| format!("cannot write {}: {}", p.display(), e))?;
            f.write_all(again.as_bytes()).map_err(|e| format!("cannot write {}: {}", p.display(), e))?;
        }
        if let Some(l) = &job.log {
            l(&format!("translator revision {} -> {}: {} shaders translated again", marker.translator, TRANSLATOR_REV, stale.len()));
        }
    }
    let mut todo: Vec<&String> = urows.iter().filter(|r| !done.contains(first_col(r))).collect();
    let limited = job.limit > 0 && todo.len() > job.limit;
    if job.limit > 0 {
        todo.truncate(job.limit);
    }
    let total = urows.len() as u64;
    let mut finished = (urows.len() - urows.iter().filter(|r| !done.contains(first_col(r))).count()) as u64;
    report(p("translate", finished, total, format!("Translating shaders: {} of {}", finished, total)));
    let mut mode = crate::harness::Mode::Fast;   // Safe from the first batch that needed it on (harness::run)
    for chunk in todo.chunks(CHUNK) {
        let mut f = fs::File::create(corpus.join("unique.tsv")).map_err(|e| e.to_string())?;
        writeln!(f, "{}", uheader).map_err(|e| e.to_string())?;
        for r in chunk {
            writeln!(f, "{}", r).map_err(|e| e.to_string())?;
        }
        drop(f);
        let base = finished;
        let ok = crate::harness::run(&tools.harness, &corpus, &stage, job.jobs, &mut mode, cancel, job.idle, job.log.as_ref(), |l| {
            // "  <n>/<count>  <s> s" every 500 blocks
            if let Some(n) = l.trim().split('/').next().and_then(|x| x.parse::<u64>().ok()) {
                report(p("translate", base + n, total, format!("Translating shaders: {} of {}", base + n, total)));
            }
        })?;
        if !ok {
            return Ok(false);
        }
        commit_stage(&stage, &spirv)?;
        if compact {
            // into the archive at once: the loose files of a chunk never pile up (peak disk use stays near the compressed size)
            let hashes: Vec<String> = chunk.iter().map(|r| first_col(r).to_string()).collect();
            crate::shader_archive::compact(&job.game_dir, Some(&hashes), &AtomicBool::new(false), |_, _| {})?;
        }
        finished += chunk.len() as u64;
        report(p("translate", finished, total, format!("Translating shaders: {} of {}", finished, total)));
    }
    let _ = fs::remove_dir_all(&stage);
    let _ = fs::remove_file(corpus.join("unique.tsv"));
    if limited {
        return Ok(true);
    }

    // 3. done: the corpus is only needed to translate, drop it (several GB)
    report(p("finish", total, total, "Cleaning up...".into()));
    let _ = fs::remove_dir_all(&corpus);
    marker.extracted = false;
    marker.complete = true;
    marker.translator = TRANSLATOR_REV;
    write_marker(&job.game_dir, &marker)?;
    Ok(true)
}
