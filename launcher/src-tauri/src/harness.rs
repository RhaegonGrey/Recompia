//! Running the shader translator (tools/shaders/harness, `spirv_harness.exe`) for `shaders`: a run, and why a failed run failed.
use std::fs;
use std::path::Path;
use std::process::Command;
use std::sync::atomic::AtomicBool;

use crate::shaders::{run_tool, LogFn};

/// What the harness writes in its `--out` folder when a run fails (`ErrorFile` in tools/shaders/harness/Program.cs).
const HARNESS_ERROR: &str = "harness_error.txt";

/// What the harness writes in its `--out` folder as a run goes (`ProgressFile` in tools/shaders/harness/Program.cs).
const HARNESS_PROGRESS: &str = "harness_progress.txt";
/// The translator jobs of a safe run.
const SAFE_JOBS: usize = 2;

/// How the translator runs. Fast: server GC (the harness's own default; with workstation GC each collection stops every worker,
/// about 4x slower) and every job. Safe: workstation GC and at most SAFE_JOBS jobs, for a translator that died under the fast
/// settings without a report (#530: exit status 2 about 3 s after start, every time, on a Steam Deck with 11.5 GB under Proton).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Mode {
    Fast,
    Safe,
}

/// The translator over `input` (a corpus folder with manifest.tsv / unique.tsv) into `stage`.
fn command(exe: &Path, input: &Path, stage: &Path, jobs: usize, mode: Mode) -> Result<Command, String> {
    let jobs = if mode == Mode::Safe { jobs.clamp(1, SAFE_JOBS) } else { jobs.max(1) };
    let mut cmd = Command::new(exe);
    cmd.arg("--batch").arg(input).arg("--out").arg(stage);
    cmd.args(["--jobs", &jobs.to_string(), "--no-val"]);
    cmd.env("DOTNET_gcServer", if mode == Mode::Fast { "1" } else { "0" });
    Ok(cmd)
}

/// A failed run: the message for setup.log, and whether the harness wrote an error report (an error it caught, not a process that
/// went without a word).
struct Failure {
    message: String,
    reported: bool,
}

/// `run_tool` for the shader translator writing to `stage`. Its stdout and stderr never reach the launcher (#366), so a failure is
/// explained from `stage`: the harness's own error file, or, with no `stage` folder at all, that it never started, and the last steps
/// of its progress file.
fn run_once(cmd: Command, stage: &Path, cancel: &AtomicBool, idle: bool, log: Option<&LogFn>, line: impl FnMut(&str)) -> Result<bool, Failure> {
    run_tool(cmd, cancel, idle, log, None, line).map_err(|e| {
        let report = fs::read_to_string(stage.join(HARNESS_ERROR)).ok();
        let mut why = match &report {
            Some(t) => format!("The shader translator reported:\n{}", t.trim()),
            None if !stage.is_dir() => {
                "The shader translator never started (it creates its output folder first): the program failed before it."
                    .to_string()
            }
            None => "The shader translator started but wrote no error report.".to_string(),
        };
        if let Ok(p) = fs::read_to_string(stage.join(HARNESS_PROGRESS)) {
            let steps: Vec<&str> = p.lines().filter(|l| !l.trim().is_empty()).collect();
            why.push_str("\nIts last steps:");
            for l in &steps[steps.len().saturating_sub(4)..] {
                why.push_str("\n  ");
                why.push_str(l);
            }
        }
        if let Some(f) = log {
            f(&why);
        }
        Failure { message: format!("{}\n{}", e, why), reported: report.is_some() }
    })
}

/// One translator run over `input` into `stage` (emptied first). A run that dies without a report under `Mode::Fast` is run again
/// once under `Mode::Safe`, and `mode` stays Safe for the caller's later runs. Ok(false): cancelled.
#[allow(clippy::too_many_arguments)]
pub fn run(
    exe: &Path,
    input: &Path,
    stage: &Path,
    jobs: usize,
    mode: &mut Mode,
    cancel: &AtomicBool,
    idle: bool,
    log: Option<&LogFn>,
    mut line: impl FnMut(&str),
) -> Result<bool, String> {
    let _ = fs::remove_dir_all(stage);
    match run_once(command(exe, input, stage, jobs, *mode)?, stage, cancel, idle, log, &mut line) {
        Err(f) if !f.reported && *mode == Mode::Fast => {
            *mode = Mode::Safe;
            if let Some(l) = log {
                l(&format!("The shader translator stopped without a report: running it again with workstation GC and {} jobs, and keeping that.", jobs.clamp(1, SAFE_JOBS)));
            }
            let _ = fs::remove_dir_all(stage);
            run_once(command(exe, input, stage, jobs, Mode::Safe)?, stage, cancel, idle, log, &mut line).map_err(|f| f.message)
        }
        r => r.map_err(|f| f.message),
    }
}
