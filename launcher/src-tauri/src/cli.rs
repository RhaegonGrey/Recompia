// SPDX-License-Identifier: GPL-3.0-or-later
//! The command line modes: the relay alone (--host) and the game without a window (--play).

use std::path::PathBuf;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use crate::paths::exe_dir;
use crate::{lan, play, relay};

/// `--host [options]`: the relay alone, no window, log lines on stdout, until Ctrl+C. Listens on 0.0.0.0:47610 by default. The options are
/// the relay crate's (pokopia_relay::cli, shared with the standalone `pokopia-relay` server); `--no-udp`: no relay UDP (game datagrams stay
/// on the TCP links, as with pokopia-native's relay).
pub fn run_headless(args: &[String]) -> i32 {
    #[cfg(windows)]
    win_console::attach_and_hook_ctrl_c();
    let mut a = match relay::cli::parse(args) {
        Ok(a) => a,
        Err(e) => {
            eprintln!("{}", e);
            eprintln!("Usage: PokopiaLauncher --host {}", relay::cli::OPTIONS);
            return 2;
        }
    };
    let quiet = a.quiet;
    let log: Arc<dyn Fn(&str) + Send + Sync> = Arc::new(move |line: &str| {
        if !quiet {
            println!("{}", line)
        }
    });
    a.cfg.log = Some(log);
    let mut srv = match relay::RelayServer::start(a.addr, a.cfg, a.stats_s) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("cannot listen on {}: {}", a.addr, e);
            return 1;
        }
    };
    let [running, open] = relay::cli::banner(srv.local_addr());
    println!("{} Ctrl+C stops it.", running);
    println!("{}", open);
    for line in lan::log_lines(&lan::report(), srv.local_addr().port()) {
        println!("{}", line);
    }
    while !win_console::CTRL_C.load(Ordering::SeqCst) {
        std::thread::sleep(std::time::Duration::from_millis(200));
    }
    srv.stop();
    0
}

/// A GUI-subsystem exe has no console: attach to the terminal that started it, and turn Ctrl+C into a clean stop.
mod win_console {
    use std::sync::atomic::AtomicBool;
    pub static CTRL_C: AtomicBool = AtomicBool::new(false);

    #[cfg(windows)]
    pub fn attach_and_hook_ctrl_c() {
        use std::sync::atomic::Ordering;
        const ATTACH_PARENT_PROCESS: u32 = u32::MAX;
        extern "system" {
            fn AttachConsole(pid: u32) -> i32;
            fn SetConsoleCtrlHandler(handler: Option<unsafe extern "system" fn(u32) -> i32>, add: i32) -> i32;
        }
        unsafe extern "system" fn on_ctrl(_kind: u32) -> i32 {
            CTRL_C.store(true, Ordering::SeqCst);
            1 // handled: main() stops the server and returns
        }
        // fails harmlessly when there is no parent console or the exe already has one (debug builds)
        unsafe {
            AttachConsole(ATTACH_PARENT_PROCESS);
            SetConsoleCtrlHandler(Some(on_ctrl), 1);
        }
    }
}

/// The game folder a command line mode works on: `--game <folder>`, else the launcher's own (paths.rs default_game_dir).
fn game_arg(args: &[String]) -> PathBuf {
    args.iter().position(|a| a == "--game").and_then(|i| args.get(i + 1)).map(PathBuf::from).unwrap_or_else(crate::paths::default_game_dir)
}

/// `--play [--game <folder>]`: starts the game as Play does, without the window (a Steam or Sunshine shortcut). The game
/// folder is the launcher's own unless given: the one set in Settings lives in the window's config, not read here.
pub fn run_play(args: &[String]) -> i32 {
    #[cfg(windows)]
    win_console::attach_and_hook_ctrl_c();
    let game = game_arg(args);
    play::run_cli(&game, &exe_dir().join("tools"))
}
