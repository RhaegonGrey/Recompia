// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's shader setup without the window (development check):
//!   cargo run --example shader_setup -- <game dir> <romfs> <exefs/main> <tools dir> [limit] [jobs]
//! Builds <game dir>/shader_cache/spirv exactly as the launcher's Set up does; `limit` translates only that many blocks.

#[path = "../src/harness.rs"]
mod harness;
#[allow(dead_code)]
#[path = "../src/host.rs"]
mod host;
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
