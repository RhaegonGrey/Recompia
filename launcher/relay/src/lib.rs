// SPDX-License-Identifier: GPL-3.0-or-later
//! The relay crate: the launcher's "Run a relay here" and `--host`, and the standalone `pokopia-relay` server binary (src/main.rs).
//!
//! Based on pokopia-native (MIT) windows/tools/coop-relay/coop_relay.h @4a72d23: the Link Play relay ("pkp host"), a Rust port of
//! pokopia-native's coop-relay, the meeting point for internet play when the host cannot forward a port. Game builds with `[net] relay` set
//! (ours and pokopia-native's) all dial out to it.
//!
//! It keeps rooms keyed by a 16-byte room id the players derive from their room code (coop_auth.c croom_keys): it never sees the code or any
//! key. A host keeps a control link open for its room; a visitor asks for the room, the relay tells the host, the host dials one more link for
//! that visitor, and the relay pairs the two and copies bytes between them without reading them. Everything inside is the players' own
//! authenticated protocol (coop protocol 3), so the relay cannot forge or change game messages; it CAN read them (nothing is encrypted).
//!
//! # Protocol (PKR1, TCP, port 47610, little-endian; the same values as runtime/third_party/pokopia-coop/coop_int.h RLY_*)
//!
//! A link's first 32 bytes: u32 magic "PKR1", u8 kind (1 host, 2 join, 3 attach), u8 version 1, 2 reserved, room id[16], u32 visitor id,
//! u32 cookie. The relay answers in 16-byte messages: u32 magic, u8 type (1 ok, 2 visitor, 3 gone, 4 error, 5 pong), u8 error code
//! (1 no room, 2 taken, 3 full, 4 limit, 5 timeout, 6 bad, 7 expired), 2 reserved, u32 arg (a visitor id), u32 cookie. A host's control link
//! may send pings (type 6).
//!
//! Limits (engine::Config, coop_relay_defaults): 64 rooms, 7 visitors a room, 256 links, 24 links and 10 new links a second per address,
//! 60 failed lookups a minute per address, hello within 5 s, a visitor waits 10 s for its host, a host's control link silent 30 s closes its
//! room, a room lives 12 h and carries 4 GiB, a link sends 256 KiB/s (bursts twice that).
//!
//! # Relay UDP (pokopia-recomp extension, opt-in, coop_int.h "pokopia-recomp extension: relay UDP")
//!
//! pokopia-native sends game datagrams over UDP on direct links but over the TCP link through a relay (one lost segment stalls every datagram
//! behind it). This relay can also carry them over UDP, only between two links that both ask for it, so their builds and their relay see
//! exactly the PKR1 above:
//!
//! - Hello byte 6 = 0xC5: this link wants relay UDP (their clients send 0, their relay ignores the byte).
//! - At pairing, when the visitor's join and the host's attach both asked (and the UDP socket is open): message type 0xC5 to each link, arg =
//!   the low and cookie = the high 32 bits of that link's random u64 UDP token; the host's link gets it before any byte of the visitor's,
//!   the visitor before its OK. Otherwise nothing extra is sent and both stay on TCP.
//! - UDP on the relay's port, packets of u32 magic "PRU1", u8 type, u8 flags, 2 reserved, u64 token, payload:
//!   1 BIND (client to relay, no payload): this token's endpoint is the sender. Answered with 2 BOUND (as long as the bind, flags bit 0: the
//!   other link of the pair has bound too). 3 DATA: payload for the other link of the pair, sent on to its endpoint with its own token.
//! - A packet counts only when its token is a paired link's and it comes from that link's TCP address; data shares the link's byte rate and
//!   the room's byte budget. The payload is the players' own tagged datagram (PCU3: token, sequence number, HMAC tag), checked end to end.
//! - The clients also agree end to end, over their tagged link (frame type 0xC5), before either sends game datagrams this way.

pub mod cli;
mod engine;
mod poll;
pub mod room_code;
mod server;

pub use engine::Config;
pub use server::{RelayServer, Status};

/// Default port of the relay (TCP, and UDP for relay UDP): pokopia-native's COOP_RELAY_PORT.
pub const DEFAULT_PORT: u16 = 47610;
