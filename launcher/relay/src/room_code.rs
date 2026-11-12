// SPDX-License-Identifier: GPL-3.0-or-later
//! Room codes (lib.rs "Room codes"): 16 symbols of Crockford base32, 80 bits, shown as XXXX-XXXX-XXXX-XXXX. The relay never sees
//! a code (clients send a key derived from it); the launcher only makes, checks and shows codes.
//! From pokopia-native (MIT), windows/src/coop/coop_auth.c @7c3655d: the alphabet, the input rules and the display format.

/// Symbols in a code.
pub const LEN: usize = 16;
const B32: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford's base32

/// The code's 16 symbols, or None. Case, '-' and spaces do not matter; O reads as 0, I and L as 1.
pub fn normalize(input: &str) -> Option<String> {
    let mut out = String::with_capacity(LEN);
    for ch in input.chars() {
        if ch == '-' || ch == ' ' {
            continue;
        }
        let c = match ch.to_ascii_uppercase() {
            'O' => '0',
            'I' | 'L' => '1',
            c => c,
        };
        if !c.is_ascii() || !B32.contains(&(c as u8)) || out.len() == LEN {
            return None;
        }
        out.push(c);
    }
    (out.len() == LEN).then_some(out)
}

/// XXXX-XXXX-XXXX-XXXX of a normalized code.
pub fn format(norm: &str) -> String {
    let mut s = String::with_capacity(LEN + 3);
    for (i, c) in norm.chars().enumerate() {
        if i > 0 && i % 4 == 0 {
            s.push('-');
        }
        s.push(c);
    }
    s
}

/// A new code (normalized) from the OS random source. 256 is a multiple of 32: masking a byte has no bias.
pub fn new_code() -> std::io::Result<String> {
    let mut r = [0u8; LEN];
    os_random(&mut r)?;
    Ok(r.iter().map(|b| B32[(b & 31) as usize] as char).collect())
}

#[cfg(windows)]
pub fn os_random(buf: &mut [u8]) -> std::io::Result<()> {
    const BCRYPT_USE_SYSTEM_PREFERRED_RNG: u32 = 2;
    #[link(name = "bcrypt")]
    extern "system" {
        fn BCryptGenRandom(alg: *mut std::ffi::c_void, buf: *mut u8, len: u32, flags: u32) -> i32;
    }
    let st = unsafe { BCryptGenRandom(std::ptr::null_mut(), buf.as_mut_ptr(), buf.len() as u32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) };
    if st < 0 {
        return Err(std::io::Error::other(format!("BCryptGenRandom failed: 0x{:08x}", st as u32)));
    }
    Ok(())
}

#[cfg(not(windows))]
pub fn os_random(buf: &mut [u8]) -> std::io::Result<()> {
    use std::io::Read;
    std::fs::File::open("/dev/urandom")?.read_exact(buf)
}
