// SPDX-License-Identifier: GPL-3.0-or-later
//! `pokopia-relay [options]`: the Link Play relay alone, for a server (no launcher, no window). Log lines on stdout; runs until it is
//! killed (SIGTERM from systemd, or Ctrl+C): dropping the process closes every link, which is all a stop needs to do.

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "--help" || a == "-h") {
        println!("Usage: pokopia-relay {}", pokopia_relay::cli::OPTIONS);
        return;
    }
    let mut a = match pokopia_relay::cli::parse(&args) {
        Ok(a) => a,
        Err(e) => {
            eprintln!("{}", e);
            std::process::exit(2);
        }
    };
    let quiet = a.quiet;
    a.cfg.log = Some(std::sync::Arc::new(move |line: &str| {
        if !quiet {
            println!("{}", line)
        }
    }));
    let srv = match pokopia_relay::RelayServer::start(a.addr, a.cfg, a.stats_s) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("cannot listen on {}: {}", a.addr, e);
            std::process::exit(1);
        }
    };
    for line in pokopia_relay::cli::banner(srv.local_addr()) {
        println!("{}", line);
    }
    loop {
        std::thread::park();
    }
}
