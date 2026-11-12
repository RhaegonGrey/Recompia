// SPDX-License-Identifier: GPL-3.0-or-later
//! poll() on std sockets: WSAPoll on Windows, poll(2) elsewhere (coop_relay.c's rpoll). One thread waits on every link at once.

pub const IN: i16 = 1;
pub const OUT: i16 = 2;
pub const ERR: i16 = 4;

#[cfg(windows)]
pub type Raw = u64;
#[cfg(not(windows))]
pub type Raw = i32;

#[cfg(windows)]
pub fn raw_tcp(s: &std::net::TcpStream) -> Raw {
    std::os::windows::io::AsRawSocket::as_raw_socket(s)
}
#[cfg(windows)]
pub fn raw_listener(s: &std::net::TcpListener) -> Raw {
    std::os::windows::io::AsRawSocket::as_raw_socket(s)
}
#[cfg(windows)]
pub fn raw_udp(s: &std::net::UdpSocket) -> Raw {
    std::os::windows::io::AsRawSocket::as_raw_socket(s)
}
#[cfg(not(windows))]
pub fn raw_tcp(s: &std::net::TcpStream) -> Raw {
    std::os::unix::io::AsRawFd::as_raw_fd(s)
}
#[cfg(not(windows))]
pub fn raw_listener(s: &std::net::TcpListener) -> Raw {
    std::os::unix::io::AsRawFd::as_raw_fd(s)
}
#[cfg(not(windows))]
pub fn raw_udp(s: &std::net::UdpSocket) -> Raw {
    std::os::unix::io::AsRawFd::as_raw_fd(s)
}

/// One socket to wait on: `want` IN / OUT, `got` IN / OUT / ERR after `poll`.
#[derive(Clone, Copy)]
pub struct Fd {
    pub s: Raw,
    pub want: i16,
    pub got: i16,
}

#[cfg(windows)]
#[repr(C)]
struct WsaPollFd {
    fd: usize,
    events: i16,
    revents: i16,
}

#[cfg(windows)]
#[link(name = "ws2_32")]
extern "system" {
    fn WSAPoll(fds: *mut WsaPollFd, n: u32, timeout: i32) -> i32;
}

/// Waits up to `ms` for any of `fds`. The count of ready sockets, or -1 when polling failed.
#[cfg(windows)]
pub fn poll(fds: &mut [Fd], ms: i32) -> i32 {
    const POLLRDNORM: i16 = 0x0100;
    const POLLRDBAND: i16 = 0x0200;
    const POLLWRNORM: i16 = 0x0010;
    const POLLERR: i16 = 0x0001;
    const POLLHUP: i16 = 0x0002;
    const POLLNVAL: i16 = 0x0004;
    if fds.is_empty() {
        std::thread::sleep(std::time::Duration::from_millis(ms.max(0) as u64));
        return 0;
    }
    let mut v: Vec<WsaPollFd> = fds
        .iter()
        .map(|f| WsaPollFd {
            fd: f.s as usize,
            events: (if f.want & IN != 0 { POLLRDNORM | POLLRDBAND } else { 0 }) | (if f.want & OUT != 0 { POLLWRNORM } else { 0 }),
            revents: 0,
        })
        .collect();
    let r = unsafe { WSAPoll(v.as_mut_ptr(), v.len() as u32, ms) };
    if r < 0 {
        return -1;
    }
    for (f, p) in fds.iter_mut().zip(v.iter()) {
        let e = p.revents;
        f.got = (if e & (POLLRDNORM | POLLRDBAND) != 0 { IN } else { 0 })
            | (if e & POLLWRNORM != 0 { OUT } else { 0 })
            | (if e & (POLLERR | POLLHUP | POLLNVAL) != 0 { ERR } else { 0 });
    }
    r
}

#[cfg(not(windows))]
pub fn poll(fds: &mut [Fd], ms: i32) -> i32 {
    let mut v: Vec<libc::pollfd> = fds
        .iter()
        .map(|f| libc::pollfd {
            fd: f.s,
            events: (if f.want & IN != 0 { libc::POLLIN } else { 0 }) | (if f.want & OUT != 0 { libc::POLLOUT } else { 0 }),
            revents: 0,
        })
        .collect();
    let r = unsafe { libc::poll(v.as_mut_ptr(), v.len() as libc::nfds_t, ms) };
    if r < 0 {
        return if std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted { 0 } else { -1 };
    }
    for (f, p) in fds.iter_mut().zip(v.iter()) {
        let e = p.revents;
        f.got = (if e & libc::POLLIN != 0 { IN } else { 0 })
            | (if e & libc::POLLOUT != 0 { OUT } else { 0 })
            | (if e & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 { ERR } else { 0 });
    }
    r
}
