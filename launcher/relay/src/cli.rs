// SPDX-License-Identifier: GPL-3.0-or-later
//! The relay's command line (the launcher's `--host` and the standalone `pokopia-relay`): one parser, so both take the same options.
//! The limits are pokopia-native's coop-relay options.

use crate::engine::Config;
use std::net::{Ipv4Addr, SocketAddrV4};

pub const OPTIONS: &str = "[--listen <IPv4>[:<port>]] [--port N] [--no-udp] [--rooms N] [--peers N] [--links N] [--per-ip N] [--new-per-ip N] [--nofind-per-min N] [--room-hours H] [--room-mib M] [--link-kib K] [--wait-s S] [--idle-s S] [--stats S] [--quiet]";

/// What the command line asked for. `stats_s`: a counters line every so many seconds (0: never). `quiet`: no log lines.
pub struct Args {
    pub addr: SocketAddrV4,
    pub cfg: Config,
    pub stats_s: u64,
    pub quiet: bool,
}

/// Parses `args` (no program name; a leading `--host` is skipped). Err: the message to print, the usage included where it helps.
pub fn parse(args: &[String]) -> Result<Args, String> {
    let mut ip = Ipv4Addr::UNSPECIFIED;
    let mut port = crate::DEFAULT_PORT;
    let mut cfg = Config::default();
    let mut stats_s: u64 = 60;
    let mut quiet = false;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--host" => continue,
            "--no-udp" => {
                cfg.udp = false;
                continue;
            }
            "--quiet" => {
                quiet = true;
                continue;
            }
            _ => {}
        }
        let Some(v) = it.next() else {
            return Err(format!("{} needs a value. Options: {}", a, OPTIONS));
        };
        if a == "--listen" {
            let (h, p) = match v.split_once(':') {
                Some((h, p)) => (h, Some(p)),
                None => (v.as_str(), None),
            };
            match (h.parse::<Ipv4Addr>(), p.map(|p| p.parse::<u16>())) {
                (Ok(h), None) => ip = h,
                (Ok(h), Some(Ok(p))) => {
                    ip = h;
                    port = p;
                }
                _ => return Err(format!("--listen {}: <IPv4>[:<port>] wanted", v)),
            }
            continue;
        }
        let Ok(x) = v.parse::<u64>() else {
            return Err(format!("{} {}: a number >= 0 wanted", a, v));
        };
        match a.as_str() {
            "--port" if x <= 65535 => port = x as u16,
            "--rooms" => cfg.max_rooms = x as usize,
            "--peers" => cfg.max_peers = x as usize,
            "--links" => cfg.max_conns = x as usize,
            "--per-ip" => cfg.max_per_ip = x.min(i32::MAX as u64) as i32,
            "--new-per-ip" => cfg.new_per_ip = x as i64,
            "--nofind-per-min" => cfg.nofind_per_min = x as i64,
            "--room-hours" => cfg.room_ms = x as i64 * 3600 * 1000,
            "--room-mib" => cfg.room_bytes = x << 20,
            "--link-kib" => cfg.link_bps = (x << 10) as i64,
            "--wait-s" => cfg.wait_ms = x as i64 * 1000,
            "--idle-s" => cfg.idle_ms = x as i64 * 1000,
            "--stats" => stats_s = x,
            _ => return Err(format!("unknown argument {}. Options: {}", a, OPTIONS)),
        }
    }
    Ok(Args { addr: SocketAddrV4::new(ip, port), cfg, stats_s, quiet })
}

/// The lines every relay prints once it listens.
pub fn banner(addr: std::net::SocketAddr) -> [String; 2] {
    [
        format!("Pokopia relay (pokopia-native coop-relay protocol) running on {}.", addr),
        "Anyone who can reach this address can open rooms here; rooms are authenticated end to end by the players, but their traffic is not encrypted."
            .to_string(),
    ]
}
