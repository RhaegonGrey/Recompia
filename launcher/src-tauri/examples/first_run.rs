// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's whole first-run setup without the window (release check of model b):
//!   cargo run --release --example first_run -- <game dir> <exefs> <romfs> <tools dir> [shader limit] [jobs]
//! Builds pkp.exe from the dump (gamebuild.rs) and then the shader cache (shaders.rs), the same code Set up runs; on Linux
//! it fetches the Linux build tools first (host.rs). Prints each phase's time.

// host.rs reaches the launcher's own modules, so an example carries the same set the binary declares (main.rs), by path.
#[allow(dead_code, clippy::all)]
#[path = "../src/config.rs"]
mod config;
#[allow(dead_code)]
#[path = "../src/download.rs"]
mod download;
#[allow(dead_code, clippy::all)]
#[path = "../src/dump.rs"]
mod dump;
#[allow(dead_code, clippy::all)]
#[path = "../src/fsutil.rs"]
mod fsutil;
#[allow(dead_code)]
#[path = "../src/gamebuild.rs"]
mod gamebuild;
#[allow(dead_code)]
#[path = "../src/harness.rs"]
mod harness;
#[allow(dead_code)]
#[path = "../src/host.rs"]
mod host;
#[allow(dead_code, clippy::all)]
#[path = "../src/ini.rs"]
mod ini;
#[allow(dead_code, clippy::all)]
#[path = "../src/paths.rs"]
mod paths;
#[allow(dead_code)]
#[path = "../src/runtime_api.rs"]
mod runtime_api;
#[allow(dead_code, clippy::all)]
#[path = "../src/settings.rs"]
mod settings;
#[allow(dead_code)]
#[path = "../src/shader_archive.rs"]
mod shader_archive;
#[allow(dead_code)]
#[path = "../src/shaders.rs"]
mod shaders;

use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::time::Instant;

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() < 4 {
        eprintln!("usage: first_run <game dir> <exefs> <romfs> <tools dir> [shader limit] [jobs]");
        std::process::exit(2);
    }
    let game = PathBuf::from(&a[0]);
    let tools_dir = PathBuf::from(&a[3]);
    let jobs: usize = a.get(5).and_then(|x| x.parse().ok()).unwrap_or(4);
    let cancel = AtomicBool::new(false);
    let fail = |e: String| -> ! {
        eprintln!("error: {}", e);
        std::process::exit(1)
    };
    let t0 = Instant::now();
    let mut phase = String::new();
    let mut since = Instant::now();
    let mut last = String::new();
    let mut tick = |ph: &str, msg: &str| {
        if ph != phase {
            if !phase.is_empty() {
                println!("== {} took {:.1} s", phase, since.elapsed().as_secs_f64());
            }
            phase = ph.to_string();
            since = Instant::now();
        }
        if msg != last {
            println!("[{}] {}", ph, msg);
            last = msg.to_string();
        }
    };
    match host::prepare_tools(&tools_dir, &cancel, &mut |_, _, msg| tick("tools", &msg)) {
        Ok(true) => {}
        Ok(false) => fail("paused".into()),
        Err(e) => fail(e),
    }
    if !gamebuild::is_built(&game, &tools_dir) {
        let tools = gamebuild::find_tools(&tools_dir, &game).unwrap_or_else(|e| fail(e));
        let job = gamebuild::Job { game_dir: game.clone(), exefs: PathBuf::from(&a[1]), jobs, idle: false, log: None };
        match gamebuild::run(&job, &tools, &cancel, |t| tick(&t.phase, &t.message)) {
            Ok(true) => {}
            Ok(false) => fail("paused".into()),
            Err(e) => fail(e),
        }
    }
    let tools = shaders::find_tools(&tools_dir).unwrap_or_else(|e| fail(e));
    let job = shaders::Job {
        game_dir: game.clone(),
        romfs: PathBuf::from(&a[2]),
        romfs_base: None,
        main: PathBuf::from(&a[1]).join("main"),
        limit: a.get(4).and_then(|x| x.parse().ok()).unwrap_or(0),
        compact: std::env::var_os("PKP_LOOSE_SHADERS").is_none(),
        jobs,
        idle: false,
        log: None,
    };
    if let Err(e) = shaders::run(&job, &tools, &cancel, |p| tick(&p.phase, &p.message)) {
        fail(e)
    }
    tick("end", "");
    println!("first run finished in {:.1} s", t0.elapsed().as_secs_f64());
}
