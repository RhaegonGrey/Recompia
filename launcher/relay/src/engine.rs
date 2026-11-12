// SPDX-License-Identifier: MIT
//! Based on pokopia-native (MIT) windows/tools/coop-relay/coop_relay.c @4a72d23 (and coop_relay.h): rooms, pairing and the byte copy (mod.rs
//! has the protocol). Every byte from the network is untrusted: a link's first message must be exactly the 32-byte hello within hello_ms; a
//! host's control link may only send 16-byte messages (pings answered, the rest ignored); paired links are copied as they are, within a byte
//! rate, a buffer bound and the room's byte budget, and never parsed. One thread, one poll() over every socket.
//! pokopia-recomp extension: relay UDP (lib.rs "Relay UDP"): a UDP socket on the same port forwards datagrams between the two links of a pair
//! when both asked for it in their hello; the payload is the players' own tagged datagram, never parsed either.

use crate::poll::{self, Fd};
use std::io::{self, ErrorKind, Read, Write};
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, TcpListener, TcpStream, UdpSocket};
use std::time::Instant;

pub const MAGIC: u32 = 0x3152_4B50; // "PKR1"
const HELLO_LEN: usize = 32;
const MSG_LEN: usize = 16;
const K_HOST: u8 = 1;
const K_JOIN: u8 = 2;
const K_ATTACH: u8 = 3;
const T_OK: u8 = 1;
const T_VISITOR: u8 = 2;
const T_GONE: u8 = 3;
const T_ERR: u8 = 4;
const T_PONG: u8 = 5;
const T_PING: u8 = 6;
const E_NOROOM: u8 = 1;
const E_TAKEN: u8 = 2;
const E_FULL: u8 = 3;
const E_LIMIT: u8 = 4;
const E_TIMEOUT: u8 = 5;
const E_BAD: u8 = 6;
const E_EXPIRED: u8 = 7;
const OUT_MAX: usize = 256 << 10; // bytes waiting for a slow reader; beyond, the other side is not read
const READ_MAX: usize = 16 << 10;
const CLOSE_FLUSH_MS: i64 = 2000;
const IPS: usize = 256;

// pokopia-recomp extension: relay UDP. Hello byte 6 = X_UDP: this link wants relay UDP. Relay message type X_UDP: arg + cookie = the link's
// 64-bit UDP token (low, high), sent to both links of a pair when both asked, before the visitor's OK. UDP packets: u32 X_UMAGIC, u8 type,
// u8 flags, 2 reserved, u64 token, payload.
pub const X_UDP: u8 = 0xC5;
pub const X_UMAGIC: u32 = 0x3155_5250; // "PRU1"
const XU_BIND: u8 = 1; // client -> relay: this token's endpoint is the sender (no payload)
const XU_BOUND: u8 = 2; // relay -> client: the answer to a bind, flags bit 0 = the other link of the pair has bound too
const XU_DATA: u8 = 3; // either way: payload for the other link of the pair (relay -> client: the receiver's own token)
const XU_HDR: usize = 16;
const XU_MAX: usize = 4608; // a tagged game datagram (24 + 12 + 4096 + 16) and the header, with room to spare

pub type LogFn = std::sync::Arc<dyn Fn(&str) + Send + Sync>;

#[derive(Clone)]
pub struct Config {
    pub max_rooms: usize,  // rooms open at once (64)
    pub max_peers: usize,  // visitors in one room, waiting or paired (7: Link Play has at most 8 players)
    pub max_conns: usize,  // links in all (256)
    pub max_per_ip: i32,   // links from one address (24)
    pub new_per_ip: i64,   // new links a second from one address (10; bursts twice that)
    pub nofind_per_min: i64, // requests naming no open room or visitor, per address a minute (60); beyond, refused with "limit"
    pub hello_ms: i64,     // a new link says what it is within this (5 s)
    pub wait_ms: i64,      // a visitor waits this long for its host to dial in (10 s)
    pub idle_ms: i64,      // a host's control link silent this long closes its room (30 s; hosts ping every 5 s)
    pub room_ms: i64,      // a room lives at most this long (12 h), then it closes with "expired"
    pub room_bytes: u64,   // and carries at most this many bytes, both ways, all its links (4 GiB)
    pub link_bps: i64,     // bytes a second one link may send (256 KiB; bursts twice that); beyond, the relay stops reading it for a moment
    pub udp: bool,         // pokopia-recomp extension: relay UDP on the same port (true)
    pub log: Option<LogFn>, // one line without a newline; None: silent
}

impl Default for Config {
    fn default() -> Self {
        Config {
            max_rooms: 64,
            max_peers: 7,
            max_conns: 256,
            max_per_ip: 24,
            new_per_ip: 10,
            nofind_per_min: 60,
            hello_ms: 5000,
            wait_ms: 10000,
            idle_ms: 30000,
            room_ms: 12 * 3600 * 1000,
            room_bytes: 4 << 30,
            link_bps: 256 << 10,
            udp: true,
            log: None,
        }
    }
}

#[derive(Clone, Copy, Default, Debug, serde::Serialize)]
pub struct Stats {
    pub rooms: usize,
    pub links: usize,
    pub waiting: usize,
    pub paired: usize,
    pub rooms_opened: u64,
    pub visitors_paired: u64,
    pub bytes: u64,
    pub refused: u64,   // links refused by a limit (rooms, links, addresses, rates, a full room)
    pub not_found: u64, // joins and attaches naming no open room or waiting visitor
    pub bad: u64,       // malformed or late first messages
    pub expired: u64,   // rooms closed by age, bytes or a silent host
    pub udp_pairs: u64, // pokopia-recomp extension: pairs given relay UDP
    pub udp_packets: u64,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum St {
    Free,
    New,
    Host,
    Wait,
    Pair,
}

struct Conn {
    st: St,
    s: Option<TcpStream>,
    ip: u32,
    port: u16,
    inb: [u8; HELLO_LEN], // the first message; on a host's control link, a partial message
    inn: usize,
    out: Vec<u8>,
    pair: Option<usize>, // Pair: the other link
    room: Option<usize>, // Host / Wait / Pair: the room
    visitor: bool,       // Pair: this is the visitor's side
    vid: u32,
    cookie: u32, // Wait: the visitor's id and the cookie its host must name
    opened: i64,
    last_rx: i64,
    deadline: i64,
    tok: i64,
    tok_at: i64, // bytes it may still send, refilled at link_bps
    closing: bool,
    close_by: i64, // flush what is queued for it, then close
    // pokopia-recomp extension: relay UDP
    x_want: bool,               // its hello asked for relay UDP
    x_tok: u64,                 // Pair: its UDP token (0: none)
    x_ep: Option<SocketAddrV4>, // the endpoint its last bind or datagram came from
}

impl Conn {
    fn free() -> Conn {
        Conn {
            st: St::Free,
            s: None,
            ip: 0,
            port: 0,
            inb: [0; HELLO_LEN],
            inn: 0,
            out: Vec::new(),
            pair: None,
            room: None,
            visitor: false,
            vid: 0,
            cookie: 0,
            opened: 0,
            last_rx: 0,
            deadline: 0,
            tok: 0,
            tok_at: 0,
            closing: false,
            close_by: 0,
            x_want: false,
            x_tok: 0,
            x_ep: None,
        }
    }
}

#[derive(Clone, Copy, Default)]
struct Room {
    used: bool,
    id: [u8; 16],
    host: Option<usize>,
    created: i64,
    bytes: u64,
    next_vid: u32,
}

#[derive(Clone, Copy, Default)]
struct IpRec {
    ip: u32,
    conns: i32,
    ntok: i64,
    nat: i64,
    ftok: i64,
    fat: i64,
    seen: i64,
}

pub struct Relay {
    c: Config,
    ls: TcpListener,
    us: Option<UdpSocket>,
    addr: SocketAddr,
    k: Vec<Conn>,
    rm: Vec<Room>,
    ips: Vec<IpRec>,
    st: Stats,
    t0: Instant,
}

fn put32(p: &mut [u8], v: u32) {
    p[..4].copy_from_slice(&v.to_le_bytes());
}
fn get32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}
fn get64(p: &[u8]) -> u64 {
    u64::from_le_bytes([p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]])
}
fn ipstr(ip: u32) -> String {
    Ipv4Addr::from(ip).to_string()
}
fn roomstr(id: &[u8]) -> String {
    format!("{:02x}{:02x}{:02x}{:02x}", id[0], id[1], id[2], id[3]) // the first 4 bytes, for logs
}
fn rand32() -> u32 {
    let mut b = [0u8; 4];
    let _ = crate::room_code::os_random(&mut b); // a cookie only stops blind attaches; the players' own handshake is what protects a link
    u32::from_le_bytes(b)
}
fn rand64() -> u64 {
    let mut b = [0u8; 8];
    let _ = crate::room_code::os_random(&mut b);
    u64::from_le_bytes(b)
}
fn blocked(e: &io::Error) -> bool {
    matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted)
}

impl Relay {
    /// Listens (non-blocking) on `bind`; with cfg.udp also UDP on the same address and port (a failure there only costs relay UDP).
    pub fn create(bind: SocketAddr, mut c: Config) -> io::Result<Relay> {
        c.max_conns = c.max_conns.max(4);
        c.max_rooms = c.max_rooms.max(1);
        let ls = TcpListener::bind(bind)?;
        ls.set_nonblocking(true)?;
        let addr = ls.local_addr()?;
        let mut r = Relay {
            k: (0..c.max_conns).map(|_| Conn::free()).collect(),
            rm: vec![Room::default(); c.max_rooms],
            ips: vec![IpRec::default(); IPS],
            us: None,
            c,
            ls,
            addr,
            st: Stats::default(),
            t0: Instant::now(),
        };
        if r.c.udp {
            match UdpSocket::bind(addr).and_then(|u| u.set_nonblocking(true).map(|_| u)) {
                Ok(u) => r.us = Some(u),
                Err(e) => r.log(&format!("UDP port {} unavailable ({}): relay UDP off, game datagrams stay on the links", addr.port(), e)),
            }
        }
        Ok(r)
    }

    pub fn local_addr(&self) -> SocketAddr {
        self.addr
    }
    pub fn udp_on(&self) -> bool {
        self.us.is_some()
    }

    fn now(&self) -> i64 {
        self.t0.elapsed().as_millis() as i64 + 1000
    }
    fn log(&self, line: &str) {
        if let Some(f) = &self.c.log {
            f(line);
        }
    }

    // ---------- addresses: links, new links a second and failed lookups a minute per address ----------
    fn ip_get(&mut self, ip: u32, now: i64) -> Option<usize> {
        let mut free: Option<usize> = None;
        for i in 0..IPS {
            let a = &self.ips[i];
            if a.ip == ip && (a.conns != 0 || a.seen != 0) {
                return Some(i);
            }
            if a.conns == 0 && free.map_or(true, |f| a.seen < self.ips[f].seen) {
                free = Some(i);
            }
        }
        let f = free?; // 256 addresses each holding links: refused
        self.ips[f] = IpRec {
            ip,
            seen: now,
            ntok: 2000 * self.c.new_per_ip,
            nat: now,
            ftok: 1000 * self.c.nofind_per_min,
            fat: now,
            conns: 0,
        };
        Some(f)
    }
    fn ip_new_ok(&mut self, a: usize, now: i64) -> bool {
        // thousandths of a link: rate a second, burst twice that
        let rate = self.c.new_per_ip;
        let a = &mut self.ips[a];
        a.ntok += (now - a.nat) * rate;
        a.nat = now;
        if a.ntok > 2000 * rate {
            a.ntok = 2000 * rate;
        }
        if a.ntok < 1000 {
            return false;
        }
        a.ntok -= 1000;
        true
    }
    fn ip_nofind_ok(&mut self, a: Option<usize>, now: i64) -> bool {
        // thousandths of a request: per minute, burst as much
        let Some(a) = a else { return false };
        let rate = self.c.nofind_per_min;
        let a = &mut self.ips[a];
        let add = (now - a.fat) * rate / 60;
        if add > 0 {
            a.ftok += add;
            a.fat = now;
            if a.ftok > 1000 * rate {
                a.ftok = 1000 * rate;
            }
        }
        if a.ftok < 1000 {
            return false;
        }
        a.ftok -= 1000;
        true
    }

    // ---------- links ----------
    fn msg(&mut self, i: usize, ty: u8, code: u8, arg: u32, cookie: u32) {
        let mut m = [0u8; MSG_LEN];
        put32(&mut m, MAGIC);
        m[4] = ty;
        m[5] = code;
        put32(&mut m[8..], arg);
        put32(&mut m[12..], cookie);
        self.k[i].out.extend_from_slice(&m);
    }
    fn closing(&mut self, i: usize, now: i64) {
        let k = &mut self.k[i];
        if k.st != St::Free && !k.closing {
            k.closing = true;
            k.close_by = now + CLOSE_FLUSH_MS;
        }
    }
    fn refuse(&mut self, i: usize, code: u8, now: i64) {
        self.msg(i, T_ERR, code, 0, 0);
        self.closing(i, now);
    }
    /// Closes a link now; what depended on it follows.
    fn conn_free(&mut self, i: usize, now: i64) {
        if self.k[i].st == St::Free {
            return;
        }
        let k = std::mem::replace(&mut self.k[i], Conn::free());
        drop(k.s);
        for a in self.ips.iter_mut() {
            if a.ip == k.ip && a.conns != 0 {
                a.conns -= 1;
                a.seen = now;
                break;
            }
        }
        match (k.st, k.room) {
            (St::Host, Some(room)) if self.rm[room].used && self.rm[room].host == Some(i) => self.room_close(room, "its host left", 0, now),
            (St::Wait, Some(room)) if self.rm[room].used => {
                if let Some(h) = self.rm[room].host {
                    self.msg(h, T_GONE, 0, 0, 0);
                }
            }
            (St::Pair, _) => {
                if let Some(p) = k.pair {
                    if self.k[p].st == St::Pair {
                        self.k[p].pair = None;
                        self.closing(p, now);
                    }
                }
            }
            _ => {}
        }
    }
    fn room_close(&mut self, ri: usize, why: &str, code: u8, now: i64) {
        if !self.rm[ri].used {
            return;
        }
        let m = self.rm[ri];
        self.log(&format!("room {} closed: {} (open {} s, {} bytes)", roomstr(&m.id), why, (now - m.created) / 1000, m.bytes));
        self.rm[ri].used = false;
        for i in 0..self.c.max_conns {
            if self.k[i].st == St::Free || self.k[i].room != Some(ri) {
                continue;
            }
            self.k[i].room = None;
            match self.k[i].st {
                St::Host => {
                    if code != 0 {
                        self.msg(i, T_ERR, code, 0, 0);
                    }
                    self.closing(i, now);
                }
                St::Wait => self.refuse(i, E_NOROOM, now),
                _ => self.closing(i, now), // paired links of this room
            }
        }
        self.rm[ri].host = None;
    }
    fn room_find(&self, id: &[u8]) -> Option<usize> {
        (0..self.c.max_rooms).find(|&i| self.rm[i].used && self.rm[i].id[..] == id[..16])
    }
    fn room_visitors(&self, ri: usize) -> usize {
        self.k.iter().filter(|k| k.room == Some(ri) && (k.st == St::Wait || (k.st == St::Pair && k.visitor))).count()
    }
    /// The 32-byte first message is complete.
    fn hello(&mut self, i: usize, now: i64) {
        let h = self.k[i].inb;
        let a = ipstr(self.k[i].ip);
        let id = roomstr(&h[8..]);
        let ad = self.ip_get(self.k[i].ip, now);
        if get32(&h) != MAGIC || h[5] != 1 || h[4] < K_HOST || h[4] > K_ATTACH {
            self.st.bad += 1;
            self.refuse(i, E_BAD, now);
            return;
        }
        self.k[i].x_want = h[6] == X_UDP; // pokopia-recomp extension: relay UDP (their clients send zeros)
        let ri = self.room_find(&h[8..24]);
        if h[4] == K_HOST {
            if ri.is_some() {
                self.st.refused += 1;
                self.refuse(i, E_TAKEN, now);
                self.log(&format!("{} asked for room {}, which another host holds", a, id));
                return;
            }
            let Some(ri) = (0..self.c.max_rooms).find(|&j| !self.rm[j].used) else {
                self.st.refused += 1;
                self.refuse(i, E_LIMIT, now);
                self.log(&format!("{} refused: {} rooms are open", a, self.c.max_rooms));
                return;
            };
            let mut idb = [0u8; 16];
            idb.copy_from_slice(&h[8..24]);
            self.rm[ri] = Room { used: true, id: idb, host: Some(i), created: now, bytes: 0, next_vid: 0 };
            let k = &mut self.k[i];
            k.st = St::Host;
            k.room = Some(ri);
            k.inn = 0;
            self.msg(i, T_OK, 0, 0, 0);
            self.st.rooms_opened += 1;
            self.log(&format!("room {} open (host {}:{})", id, a, self.k[i].port));
            return;
        }
        let Some(ri) = ri.filter(|&r| self.rm[r].host.is_some()) else {
            self.st.not_found += 1;
            let code = if self.ip_nofind_ok(ad, now) { E_NOROOM } else { E_LIMIT };
            self.refuse(i, code, now);
            return;
        };
        if h[4] == K_JOIN {
            if self.room_visitors(ri) >= self.c.max_peers {
                self.st.refused += 1;
                self.refuse(i, E_FULL, now);
                self.log(&format!("{} refused: room {} has {} visitors", a, id, self.c.max_peers));
                return;
            }
            self.rm[ri].next_vid = self.rm[ri].next_vid.wrapping_add(1);
            let vid = self.rm[ri].next_vid;
            let cookie = rand32();
            let k = &mut self.k[i];
            k.st = St::Wait;
            k.room = Some(ri);
            k.vid = vid;
            k.cookie = cookie;
            k.deadline = now + self.c.wait_ms;
            let host = self.rm[ri].host.unwrap();
            self.msg(host, T_VISITOR, 0, vid, cookie);
            self.log(&format!("visitor {} ({}) waits for room {}", vid, a, id));
            return;
        }
        // attach: the host's link for one waiting visitor
        let (vid, cookie) = (get32(&h[24..]), get32(&h[28..]));
        let w = (0..self.c.max_conns)
            .find(|&j| self.k[j].st == St::Wait && self.k[j].room == Some(ri) && self.k[j].vid == vid && self.k[j].cookie == cookie);
        let Some(w) = w else {
            self.st.not_found += 1;
            let code = if self.ip_nofind_ok(ad, now) { E_NOROOM } else { E_LIMIT };
            self.refuse(i, code, now);
            return;
        };
        let bps = self.c.link_bps;
        {
            let k = &mut self.k[i];
            k.st = St::Pair;
            k.room = Some(ri);
            k.pair = Some(w);
            k.visitor = false;
            k.tok = bps;
            k.tok_at = now;
        }
        {
            let v = &mut self.k[w];
            v.st = St::Pair;
            v.pair = Some(i);
            v.visitor = true;
            v.tok = bps;
            v.tok_at = now;
        }
        // pokopia-recomp extension: relay UDP, only when both links asked (then each gets its token: the host's link before any byte
        // of the visitor's, the visitor before its OK). Anyone else gets exactly what coop_relay.c sends
        let udp = self.us.is_some() && self.k[i].x_want && self.k[w].x_want;
        if udp {
            let (th, tv) = (self.new_token(), self.new_token());
            self.k[i].x_tok = th;
            self.k[w].x_tok = tv;
            self.msg(i, X_UDP, 0, th as u32, (th >> 32) as u32);
            self.msg(w, X_UDP, 0, tv as u32, (tv >> 32) as u32);
            self.st.udp_pairs += 1;
        }
        self.msg(w, T_OK, 0, vid, 0); // the visitor speaks to its host from here on
        self.st.visitors_paired += 1;
        self.log(&format!("visitor {} paired with room {}'s host{}", vid, id, if udp { " (relay UDP)" } else { "" }));
    }
    fn new_token(&self) -> u64 {
        loop {
            let t = rand64();
            if t != 0 && !self.k.iter().any(|k| k.x_tok == t) {
                return t;
            }
        }
    }
    /// A host's control link: 16-byte messages; pings are answered, the rest ignored.
    fn host_ctl(&mut self, i: usize, now: i64) {
        while self.k[i].inn >= MSG_LEN {
            if get32(&self.k[i].inb) != MAGIC {
                self.st.bad += 1;
                self.conn_free(i, now);
                return;
            }
            if self.k[i].inb[4] == T_PING {
                self.msg(i, T_PONG, 0, 0, 0);
            }
            let k = &mut self.k[i];
            k.inb.copy_within(MSG_LEN..k.inn, 0);
            k.inn -= MSG_LEN;
        }
    }
    fn refill(&mut self, i: usize, now: i64) {
        let bps = self.c.link_bps;
        let k = &mut self.k[i];
        k.tok += (now - k.tok_at) * bps / 1000;
        k.tok_at = now;
        if k.tok > 2 * bps {
            k.tok = 2 * bps;
        }
    }
    /// Bytes a paired link moved: the relay's count and its room's budget.
    fn count(&mut self, i: usize, n: usize, now: i64) {
        self.st.bytes += n as u64;
        if let Some(ri) = self.k[i].room {
            if self.rm[ri].used {
                self.rm[ri].bytes += n as u64;
                if self.rm[ri].bytes > self.c.room_bytes {
                    self.st.expired += 1;
                    self.room_close(ri, "its byte budget is spent", E_EXPIRED, now);
                }
            }
        }
    }
    fn conn_read(&mut self, i: usize, now: i64) {
        let st = self.k[i].st;
        if st == St::New || st == St::Host {
            let want = if st == St::New { HELLO_LEN - self.k[i].inn } else { MSG_LEN - self.k[i].inn };
            let k = &mut self.k[i];
            let inn = k.inn;
            let r = k.s.as_mut().unwrap().read(&mut k.inb[inn..inn + want]);
            match r {
                Ok(0) => self.conn_free(i, now),
                Err(e) if !blocked(&e) => self.conn_free(i, now),
                Err(_) => {}
                Ok(n) => {
                    k.inn += n;
                    k.last_rx = now;
                    if st == St::New && k.inn == HELLO_LEN {
                        self.hello(i, now);
                    } else if st == St::Host {
                        self.host_ctl(i, now);
                    }
                }
            }
            return;
        }
        if st == St::Wait {
            // a waiting visitor has nothing to say until it is paired
            let mut b = [0u8; 64];
            match self.k[i].s.as_mut().unwrap().read(&mut b) {
                Ok(0) => self.conn_free(i, now),
                Err(e) if !blocked(&e) => self.conn_free(i, now),
                Err(_) => {}
                Ok(_) => {
                    self.st.bad += 1;
                    self.refuse(i, E_BAD, now);
                }
            }
            return;
        }
        let Some(o) = self.k[i].pair.filter(|_| st == St::Pair) else { return };
        self.refill(i, now);
        let room = OUT_MAX.saturating_sub(self.k[o].out.len());
        let mut want = READ_MAX.min(room);
        let tok = self.k[i].tok;
        if (tok as usize) < want || tok < 0 {
            want = if tok > 0 { tok as usize } else { 0 };
        }
        if want == 0 {
            return;
        }
        let mut buf = vec![0u8; want];
        match self.k[i].s.as_mut().unwrap().read(&mut buf) {
            Ok(0) => self.conn_free(i, now),
            Err(e) if !blocked(&e) => self.conn_free(i, now),
            Err(_) => {}
            Ok(n) => {
                self.k[o].out.extend_from_slice(&buf[..n]);
                self.k[i].tok -= n as i64;
                self.k[i].last_rx = now;
                self.count(i, n, now);
            }
        }
    }
    fn conn_write(&mut self, i: usize, now: i64) {
        while !self.k[i].out.is_empty() {
            let k = &mut self.k[i];
            let n = k.out.len().min(65536);
            match k.s.as_mut().unwrap().write(&k.out[..n]) {
                Err(e) if blocked(&e) => return,
                Ok(0) | Err(_) => {
                    self.conn_free(i, now);
                    return;
                }
                Ok(n) => {
                    k.out.drain(..n);
                }
            }
        }
    }
    fn accept_all(&mut self, now: i64) {
        for _ in 0..64 {
            let (s, sa) = match self.ls.accept() {
                Ok(x) => x,
                Err(_) => return,
            };
            let (ip, port) = match sa {
                SocketAddr::V4(a) => (u32::from(*a.ip()), a.port()),
                SocketAddr::V6(a) => (a.ip().to_ipv4_mapped().map(u32::from).unwrap_or(0), a.port()),
            };
            let slot = (0..self.c.max_conns).find(|&i| self.k[i].st == St::Free);
            let a = self.ip_get(ip, now);
            let why = if slot.is_none() {
                Some("every link slot is taken")
            } else if a.is_none() {
                Some("too many addresses hold links")
            } else if self.ips[a.unwrap()].conns >= self.c.max_per_ip {
                Some("too many links from it")
            } else if !self.ip_new_ok(a.unwrap(), now) {
                Some("too many new links a second from it")
            } else {
                None
            };
            if let Some(why) = why {
                self.st.refused += 1;
                drop(s);
                if self.st.refused <= 8 || self.st.refused % 100 == 0 {
                    self.log(&format!("a link from {} refused: {} ({} refused so far)", ipstr(ip), why, self.st.refused));
                }
                continue;
            }
            if s.set_nonblocking(true).is_err() {
                continue;
            }
            let _ = s.set_nodelay(true);
            let slot = slot.unwrap();
            let mut k = Conn::free();
            k.st = St::New;
            k.s = Some(s);
            k.ip = ip;
            k.port = port;
            k.opened = now;
            k.last_rx = now;
            self.k[slot] = k;
            let a = &mut self.ips[a.unwrap()];
            a.conns += 1;
            a.seen = now;
        }
    }
    fn timers(&mut self, now: i64) {
        for i in 0..self.c.max_conns {
            let k = &self.k[i];
            if k.st == St::Free {
                continue;
            }
            if k.closing {
                if k.out.is_empty() || now >= k.close_by {
                    self.conn_free(i, now);
                }
                continue;
            }
            if k.st == St::New && now - k.opened >= self.c.hello_ms {
                self.st.bad += 1;
                self.conn_free(i, now);
            } else if k.st == St::Wait && now >= k.deadline {
                self.st.expired += 1;
                self.refuse(i, E_TIMEOUT, now);
            } else if k.st == St::Host && now - k.last_rx >= self.c.idle_ms {
                if let Some(r) = k.room {
                    self.st.expired += 1;
                    self.room_close(r, "its host went silent", 0, now);
                }
            }
        }
        for i in 0..self.c.max_rooms {
            if self.rm[i].used && now - self.rm[i].created >= self.c.room_ms {
                self.st.expired += 1;
                self.room_close(i, "it reached its age limit", E_EXPIRED, now);
            }
        }
    }

    // ---------- pokopia-recomp extension: relay UDP ----------
    // A packet counts only when it is well formed, its token belongs to a paired link and it comes from that link's own address (any port:
    // NATs pick one). A bind makes its source the link's endpoint and is answered with a packet as long as itself; data goes to the other
    // link's endpoint, which that link proved the same way, within the link's byte rate and the room's budget (the same as its TCP bytes).
    fn udp_read(&mut self, now: i64) {
        let mut b = [0u8; XU_MAX + 1];
        for _ in 0..512 {
            let r = match &self.us {
                Some(us) => us.recv_from(&mut b),
                None => return,
            };
            let (n, src) = match r {
                Ok(x) => x,
                Err(e) if e.kind() == ErrorKind::WouldBlock => return,
                Err(_) => continue, // an ICMP report (Windows: WSAECONNRESET) or an over-long packet
            };
            let SocketAddr::V4(src) = src else { continue };
            if n < XU_HDR || n > XU_MAX || get32(&b) != X_UMAGIC {
                continue;
            }
            let tok = get64(&b[8..]);
            if tok == 0 {
                continue;
            }
            let Some(i) = (0..self.c.max_conns).find(|&i| self.k[i].st == St::Pair && self.k[i].x_tok == tok && !self.k[i].closing) else {
                continue;
            };
            if u32::from(*src.ip()) != self.k[i].ip {
                continue;
            }
            match b[4] {
                XU_BIND if n == XU_HDR => {
                    self.k[i].x_ep = Some(src);
                    let other = self.k[i].pair.map_or(false, |p| self.k[p].x_ep.is_some());
                    let mut a = [0u8; XU_HDR];
                    put32(&mut a, X_UMAGIC);
                    a[4] = XU_BOUND;
                    a[5] = other as u8;
                    a[8..16].copy_from_slice(&tok.to_le_bytes());
                    self.send_udp(&a, src);
                }
                XU_DATA if n > XU_HDR => {
                    let Some(o) = self.k[i].pair else { continue };
                    self.k[i].x_ep = Some(src);
                    self.refill(i, now);
                    if self.k[i].tok < n as i64 {
                        continue; // over its rate: dropped (the game resends)
                    }
                    let Some(dst) = self.k[o].x_ep else { continue };
                    self.k[i].tok -= n as i64;
                    self.k[i].last_rx = now;
                    b[5] = 0;
                    b[6] = 0;
                    b[7] = 0;
                    b[8..16].copy_from_slice(&self.k[o].x_tok.to_le_bytes());
                    self.send_udp(&b[..n], dst);
                    self.st.udp_packets += 1;
                    self.count(i, n, now);
                }
                _ => {}
            }
        }
    }

    fn send_udp(&self, p: &[u8], to: SocketAddrV4) {
        if let Some(u) = &self.us {
            let _ = u.send_to(p, to); // a full buffer drops it, as UDP does
        }
    }

    /// One round: waits up to ms for traffic, moves bytes, runs the timers. Err when polling failed.
    pub fn step(&mut self, ms: i32) -> Result<(), ()> {
        let mut fds: Vec<Fd> = Vec::with_capacity(self.c.max_conns + 2);
        let mut idx: Vec<isize> = Vec::with_capacity(self.c.max_conns + 2);
        fds.push(Fd { s: poll::raw_listener(&self.ls), want: poll::IN, got: 0 });
        idx.push(-1);
        if let Some(u) = &self.us {
            fds.push(Fd { s: poll::raw_udp(u), want: poll::IN, got: 0 });
            idx.push(-2);
        }
        let mut throttled = false;
        let now = self.now();
        for i in 0..self.c.max_conns {
            let k = &self.k[i];
            if k.st == St::Free {
                continue;
            }
            let mut ev = 0;
            if !k.closing {
                if k.st != St::Pair {
                    // a host link answers every ping into `out`: while the host does not read those pongs it is not read either (as
                    // a pair is not), so `out` stays bounded; it then goes silent and the idle timer closes it
                    if k.out.len() < OUT_MAX {
                        ev |= poll::IN;
                    }
                } else if let Some(p) = k.pair {
                    if self.k[p].out.len() < OUT_MAX {
                        let tok = k.tok + (now - k.tok_at) * self.c.link_bps / 1000;
                        if tok > 0 {
                            ev |= poll::IN;
                        } else {
                            throttled = true; // out of bytes for now: not read until the rate refills
                        }
                    }
                }
            }
            if !k.out.is_empty() {
                ev |= poll::OUT;
            }
            if ev == 0 {
                continue; // WSAPoll wants something to wait for; the timers below still see it
            }
            fds.push(Fd { s: poll::raw_tcp(k.s.as_ref().unwrap()), want: ev, got: 0 });
            idx.push(i as isize);
        }
        let got = poll::poll(&mut fds, if throttled && ms > 10 { 10 } else { ms });
        if got < 0 {
            return Err(());
        }
        let now = self.now();
        for (f, &i) in fds.iter().zip(idx.iter()) {
            if f.got == 0 {
                continue;
            }
            if i == -1 {
                self.accept_all(now);
                continue;
            }
            if i == -2 {
                self.udp_read(now);
                continue;
            }
            let i = i as usize;
            let same = self.k[i].s.as_ref().map_or(false, |s| poll::raw_tcp(s) == f.s);
            if self.k[i].st == St::Free || !same {
                continue;
            }
            if f.got & (poll::IN | poll::ERR) != 0 {
                self.conn_read(i, now);
            }
            if self.k[i].st != St::Free && f.got & poll::OUT != 0 {
                self.conn_write(i, now);
            }
        }
        for i in 0..self.c.max_conns {
            if self.k[i].st != St::Free && !self.k[i].out.is_empty() {
                self.conn_write(i, now); // what this round queued goes out now
            }
        }
        self.timers(now);
        Ok(())
    }

    pub fn stats(&self) -> Stats {
        let mut s = self.st;
        s.rooms = self.rm.iter().filter(|r| r.used).count();
        s.links = self.k.iter().filter(|k| k.st != St::Free).count();
        s.waiting = self.k.iter().filter(|k| k.st == St::Wait).count();
        s.paired = self.k.iter().filter(|k| k.st == St::Pair).count();
        s
    }
}
