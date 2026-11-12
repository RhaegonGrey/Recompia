// SPDX-License-Identifier: GPL-3.0-or-later
//! Optional DLSS download. The release carries FSR 3 and the Streamline DLLs (upscalers/), but not NVIDIA's nvngx_dlss.dll: its
//! licence (NVIDIA RTX SDKs LICENSE) does not fit a GPL-3.0 package. This runs the bundled `tools/build/fetch_dlss.py`, which
//! downloads the file from NVIDIA's own GitHub repository at a fixed tag and checks its sha256, into `<game>/upscalers/streamline`.

use serde::Serialize;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::Stdio;

#[derive(Serialize, Clone, Default)]
pub struct Progress {
    pub done: u64,
    pub total: u64,
    pub message: String,
}

#[derive(Serialize)]
pub struct Status {
    /// this package can fetch the file (it carries the bundled Python and fetch_dlss.py)
    pub available: bool,
    /// nvngx_dlss.dll is in place
    pub present: bool,
    /// the folder it goes into
    pub dir: String,
}

pub fn target_dir(game_dir: &Path) -> PathBuf {
    game_dir.join("upscalers").join("streamline")
}

fn tools(tools_dir: &Path) -> (PathBuf, PathBuf) {
    // Linux: the Python that Set up fetches into tools/linux, so the download is there once setup has started
    (crate::host::python(tools_dir), tools_dir.join("build").join("fetch_dlss.py"))
}

pub fn status(game_dir: &Path, tools_dir: &Path) -> Status {
    let dir = target_dir(game_dir);
    // Windows only: the DLSS upscaler is off in the Linux and macOS builds (its SDK is Windows only, and the runtime turns it off
    // there), so there is nothing to download.
    #[cfg(not(windows))]
    {
        return Status { available: false, present: dir.join("nvngx_dlss.dll").is_file(), dir: crate::dump::fwd(&dir) };
    }
    #[cfg(windows)]
    {
        let (py, script) = tools(tools_dir);
        Status { available: py.is_file() && script.is_file(), present: dir.join("nvngx_dlss.dll").is_file(), dir: crate::dump::fwd(&dir) }
    }
}

/// Runs the download. Progress lines are `@progress download <done> <total> <text>`, a failure is `@error <text>`.
pub fn fetch(game_dir: &Path, tools_dir: &Path, mut report: impl FnMut(Progress)) -> Result<(), String> {
    // Windows only: status() reports it unavailable on Linux and macOS, so there is nothing to fetch.
    if cfg!(not(windows)) {
        return Err("DLSS is not available on this system: its SDK is Windows only.".into());
    }
    let (py, script) = tools(tools_dir);
    if !script.is_file() {
        return Err("This package has no DLSS download tool. Extract the release zip again so tools/ is complete.".into());
    }
    if !py.is_file() {
        return Err(if cfg!(windows) {
            "This package has no DLSS download tool. Extract the release zip again so tools/ is complete."
        } else {
            "The download needs the Linux build tools that Set up fetches: run Set up first."
        }
        .into());
    }
    let mut cmd = crate::host::command(&py);
    cmd.arg("-I").arg("-B").arg(&script).arg("--dir").arg(target_dir(game_dir));
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    let mut child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| format!("could not start the download ({}): {}", py.display(), e))?;
    let err = child.stderr.take().unwrap();
    let err_tail = std::thread::spawn(move || BufReader::new(err).lines().map_while(Result::ok).last().unwrap_or_default());
    let mut error: Option<String> = None;
    for l in BufReader::new(child.stdout.take().unwrap()).lines().map_while(Result::ok) {
        if let Some(rest) = l.strip_prefix("@progress ") {
            let mut it = rest.splitn(4, ' ');
            let _phase = it.next();
            let done = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
            let total = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
            report(Progress { done, total, message: it.next().unwrap_or("").to_string() });
        } else if let Some(rest) = l.strip_prefix("@error ") {
            error = Some(rest.to_string());
        }
    }
    let st = child.wait().map_err(|e| e.to_string())?;
    if st.success() {
        return Ok(());
    }
    let tail = err_tail.join().unwrap_or_default();
    Err(error.unwrap_or_else(|| format!("The DLSS download failed ({}). {}", st, tail.trim())))
}
