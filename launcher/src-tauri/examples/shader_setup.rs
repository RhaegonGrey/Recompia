// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's shader setup without the window (development check):
//!   cargo run --example shader_setup -- <game dir> <romfs> <exefs/main> <tools dir> [limit] [jobs]
//! Builds <game dir>/shader_cache/spirv exactly as the launcher's Set up does; `limit` translates only that many blocks.

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

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() < 4 {
        eprintln!("usage: shader_setup <game dir> <romfs> <exefs/main> <tools dir> [limit] [jobs]");
        std::process::exit(2);
    }
    let job = shaders::Job {
        game_dir: PathBuf::from(&a[0]),
        romfs: PathBuf::from(&a[1]),
        romfs_base: None,
        main: PathBuf::from(&a[2]),
        limit: a.get(4).and_then(|x| x.parse().ok()).unwrap_or(0),
        compact: std::env::var_os("PKP_LOOSE_SHADERS").is_none(),
        jobs: a.get(5).and_then(|x| x.parse().ok()).unwrap_or(2),
        idle: false,
        log: None,
    };
    let tools = shaders::find_tools(&PathBuf::from(&a[3])).unwrap_or_else(|e| {
        eprintln!("{}", e);
        std::process::exit(1)
    });
    let mut last = String::new();
    let r = shaders::run(&job, &tools, &AtomicBool::new(false), |p| {
        if p.message != last {
            println!("[{}] {}", p.phase, p.message);
            last = p.message;
        }
    });
    match r {
        Ok(_) => println!("ok"),
        Err(e) => {
            eprintln!("error: {}", e);
            std::process::exit(1)
        }
    }
}
