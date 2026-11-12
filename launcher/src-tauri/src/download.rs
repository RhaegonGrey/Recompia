// SPDX-License-Identifier: GPL-3.0-or-later
//! Downloads checked against the size and SHA-256 their publisher lists for them: the Linux build tools (host.rs) and mods from the
//! Modtopia catalog (mods_install.rs). A file that does not match is never kept, and a finished one appears under its name at once
//! (written as `<file>.part`, then renamed), so nothing ever reads half a download. `copy_checked` is the one copy-and-hash loop; the
//! launcher's own update (update.rs) streams through it too.

use std::io::{Read, Write};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

/// One download: the file name it gets, where it comes from, and what it must be.
pub struct Pinned<'a> {
    pub file: &'a str,
    pub url: &'a str,
    pub size: u64,
    /// lower-case hex
    pub sha256: &'a str,
}

/// The launcher's HTTP agent for downloads. `redirects`: false refuses an answer that sends it elsewhere, for a download that must come
/// from one host only.
pub fn agent(redirects: bool) -> ureq::Agent {
    ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(20))
        .timeout_read(Duration::from_secs(60))
        .redirects(if redirects { 5 } else { 0 })
        .user_agent(&format!("PokopiaRecomp-Launcher/{}", env!("CARGO_PKG_VERSION")))
        .build()
}

fn hex(h: &[u8]) -> String {
    h.iter().map(|b| format!("{:02x}", b)).collect()
}

/// What a failed copy says: one sentence for the reading side and one for the writing side, each given the system's error text. The
/// callers word them for their player (a tool download, an update).
pub struct Said<'a> {
    pub read: &'a dyn Fn(&str) -> String,
    pub write: &'a dyn Fn(&str) -> String,
}

/// The one download loop: copies at most `size` + 1 bytes (one more shows an oversized file) from `from` to `to`, hashing them. Gives
/// (bytes, lower-case SHA-256), or None when `cancel` was set. `progress` gets the bytes so far, at most `size`. The caller compares the
/// size and the hash and cleans up its file.
pub fn copy_checked(from: &mut dyn Read, to: &mut dyn Write, size: u64, cancel: &AtomicBool, progress: &mut dyn FnMut(u64), say: &Said) -> Result<Option<(u64, String)>, String> {
    use sha2::{Digest, Sha256};
    let (mut h, mut n, mut buf) = (Sha256::new(), 0u64, vec![0u8; 1 << 16]);
    let mut from = from.take(size.saturating_add(1));
    loop {
        if cancel.load(Ordering::SeqCst) {
            return Ok(None);
        }
        let k = from.read(&mut buf).map_err(|e| (say.read)(&e.to_string()))?;
        if k == 0 {
            break;
        }
        h.update(&buf[..k]);
        to.write_all(&buf[..k]).map_err(|e| (say.write)(&e.to_string()))?;
        n += k as u64;
        progress(n.min(size));
    }
    Ok(Some((n, hex(&h.finalize()))))
}

/// Downloads `p` into `dir` unless a file that matches it is there. Ok(false) = cancelled. `progress` gets the bytes so far.
pub fn fetch(agent: &ureq::Agent, p: &Pinned, dir: &Path, cancel: &AtomicBool, progress: &mut dyn FnMut(u64)) -> Result<bool, String> {
    use sha2::{Digest, Sha256};
    let dst = dir.join(p.file);
    let want = p.sha256.to_ascii_lowercase();
    if std::fs::read(&dst).map(|b| b.len() as u64 == p.size && hex(&Sha256::digest(&b)) == want).unwrap_or(false) {
        progress(p.size);
        return Ok(true);
    }
    let part = dir.join(format!("{}.part", p.file));
    let resp = agent.get(p.url).call().map_err(|e| format!("Could not download {}: {}", p.url, e))?;
    let mut f = std::fs::File::create(&part).map_err(|e| format!("cannot write {}: {}", part.display(), e))?;
    let say = Said { read: &|e| format!("Downloading {} failed: {}", p.url, e), write: &|e| format!("cannot write {}: {}", part.display(), e) };
    let copied = copy_checked(&mut resp.into_reader(), &mut f, p.size, cancel, progress, &say);
    drop(f);
    let (n, got) = match copied {
        Ok(Some(done)) => done,
        Ok(None) => {
            let _ = std::fs::remove_file(&part);
            return Ok(false);
        }
        Err(e) => {
            let _ = std::fs::remove_file(&part);
            return Err(e);
        }
    };
    if n != p.size || got != want {
        let _ = std::fs::remove_file(&part);
        return Err(format!("{} does not match its published size and SHA-256 ({} bytes, {}): not kept.", p.file, n, got));
    }
    std::fs::rename(&part, &dst).map_err(|e| {
        let _ = std::fs::remove_file(&part);
        format!("cannot move {}: {}", dst.display(), e)
    })?;
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(data: &[u8], size: u64, cancel: bool) -> (Result<Option<(u64, String)>, String>, Vec<u8>, u64) {
        let (mut out, mut last) = (Vec::new(), 0u64);
        let flag = AtomicBool::new(cancel);
        let say = Said { read: &|e| format!("read {e}"), write: &|e| format!("write {e}") };
        let r = copy_checked(&mut &data[..], &mut out, size, &flag, &mut |n| last = n, &say);
        (r, out, last)
    }

    const ABC: &str = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    #[test]
    fn copies_hashes_and_reports_up_to_the_size() {
        let (r, out, last) = run(b"abc", 3, false);
        assert_eq!(r.unwrap(), Some((3, ABC.to_string())));
        assert_eq!((out, last), (b"abc".to_vec(), 3));
    }

    #[test]
    fn one_byte_too_many_shows_but_never_reports_past_the_size() {
        let (r, _, last) = run(b"abcd", 3, false);
        assert_eq!(r.unwrap().unwrap().0, 4);
        assert_eq!(last, 3);
    }

    #[test]
    fn a_cancel_stops_before_any_byte() {
        let (r, out, _) = run(b"abc", 3, true);
        assert_eq!(r.unwrap(), None);
        assert!(out.is_empty());
    }

    #[test]
    fn a_failing_writer_gets_the_callers_sentence() {
        struct Full;
        impl Write for Full {
            fn write(&mut self, _: &[u8]) -> std::io::Result<usize> {
                Err(std::io::Error::other("disk full"))
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
        let say = Said { read: &|e| format!("read {e}"), write: &|e| format!("write {e}") };
        let r = copy_checked(&mut &b"abc"[..], &mut Full, 3, &AtomicBool::new(false), &mut |_| {}, &say);
        assert_eq!(r.unwrap_err(), "write disk full");
    }
}
