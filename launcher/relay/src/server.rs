// SPDX-License-Identifier: GPL-3.0-or-later
//! The relay on a thread of its own (the Link Play page's relay, and `--host`): engine.rs's single poll loop, a stop flag, the stats for the page.
//! After pokopia-native (MIT) windows/tools/coop-relay/main.c @4a72d23 (its stats line).

use crate::engine::{Config, Relay, Stats};
use serde::Serialize;
use std::io;
use std::net::{SocketAddr, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

/// What the Link Play page shows.
#[derive(Serialize, Clone, Debug)]
pub struct Status {
    pub addr: String,
    /// Relay UDP (the pokopia-recomp extension) is open on the same port.
    pub udp: bool,
    /// Links now (host control links, waiting and paired visitors' links).
    pub clients: usize,
    pub stats: Stats,
}

/// A running relay. Dropping it stops it.
pub struct RelayServer {
    addr: SocketAddr,
    udp: bool,
    stop: Arc<AtomicBool>,
    stats: Arc<Mutex<Stats>>,
    thread: Option<JoinHandle<()>>,
    log: Option<crate::engine::LogFn>,
}

impl RelayServer {
    /// Listens on `bind` (TCP, and UDP on the same port unless cfg.udp is off) and runs until stop. `stats_s`: a counters line every so
    /// many seconds when there is a log (0: never).
    pub fn start(bind: impl ToSocketAddrs, cfg: Config, stats_s: u64) -> io::Result<RelayServer> {
        let bind = bind.to_socket_addrs()?.next().ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "no address"))?;
        let log = cfg.log.clone();
        let mut r = Relay::create(bind, cfg)?;
        let addr = r.local_addr();
        let udp = r.udp_on();
        if let Some(l) = &log {
            l(&format!("relay listening on {} (TCP{})", addr, if udp { " and UDP" } else { "" }));
        }
        let stop = Arc::new(AtomicBool::new(false));
        let stats = Arc::new(Mutex::new(r.stats()));
        let (st, sx, lg) = (stop.clone(), stats.clone(), log.clone());
        let thread = std::thread::Builder::new().name("relay".into()).spawn(move || {
            let mut next = Instant::now() + Duration::from_secs(stats_s.max(1));
            while !st.load(Ordering::SeqCst) {
                if r.step(100).is_err() {
                    if let Some(l) = &lg {
                        l("polling failed: the relay stops");
                    }
                    break;
                }
                let s = r.stats();
                *sx.lock().unwrap_or_else(|e| e.into_inner()) = s;
                if stats_s > 0 && Instant::now() >= next {
                    next = Instant::now() + Duration::from_secs(stats_s);
                    if let Some(l) = &lg {
                        l(&format!(
                            "rooms {}, links {} ({} waiting, {} paired); opened {}, paired {}, {} bytes, refused {}, not found {}, bad {}, expired {}; relay UDP pairs {}, packets {}",
                            s.rooms, s.links, s.waiting, s.paired, s.rooms_opened, s.visitors_paired, s.bytes, s.refused, s.not_found, s.bad, s.expired,
                            s.udp_pairs, s.udp_packets
                        ));
                    }
                }
            }
            // dropping the relay closes every link
        })?;
        Ok(RelayServer { addr, udp, stop, stats, thread: Some(thread), log })
    }

    pub fn local_addr(&self) -> SocketAddr {
        self.addr
    }

    /// Closes every link and joins the thread.
    pub fn stop(&mut self) {
        let Some(t) = self.thread.take() else { return };
        self.stop.store(true, Ordering::SeqCst);
        let _ = t.join();
        if let Some(l) = &self.log {
            l("relay stopped");
        }
    }

    pub fn status(&self) -> Status {
        let s = *self.stats.lock().unwrap_or_else(|e| e.into_inner());
        Status { addr: self.addr.to_string(), udp: self.udp, clients: s.links, stats: s }
    }
}

impl Drop for RelayServer {
    fn drop(&mut self) {
        self.stop();
    }
}
