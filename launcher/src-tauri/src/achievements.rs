// SPDX-License-Identifier: GPL-3.0-or-later
//! The Achievements page: a mirror of the Modtopia community tracker. The list comes with the mod hub's catalog (mods_catalog.rs, one
//! session copy, one kept copy), everyone's counts from the Community Achievements service, the player's own from the mod's `earned.tsv`,
//! and the pictures from the hub site. The launcher only ever GETs: the mod is the one that reports (to the service, under a random
//! number), so nothing of the player's goes out from here. The mod's state on the page is read through mods.rs (the Mods page's list).
//!
//! What is fetched, and from where, is fixed here: the counts only from `STATS_HOST`, pictures only from the hub site (`mods_catalog::HUB_SITE`),
//! no redirects, a size cap on each. The last counts are kept in the launcher's settings folder, the pictures too (fetched once each), so
//! the page works offline. Pictures reach the page as `data:` URLs because the launcher's CSP lets pages load only `'self' data:` images.

use base64::Engine;
use serde::Serialize;
use std::collections::HashMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use crate::download;
use crate::mods_catalog::{self, Achievement, CatalogStore};

/// The Community Achievements service: the only address the counts are read from.
pub const STATS_HOST: &str = "https://achievements.pokopia-native.workers.dev/";
const STATS_LIMIT: u64 = 64 << 10;
const PICTURE_LIMIT: u64 = 512 << 10;
/// How many pictures are fetched at once on the first look.
const PICTURE_THREADS: usize = 4;

/// The mod the page is about: its state comes from the Mods page's list, its page from the hub's list.
pub const MOD_ID: &str = "community_achievements";

/// Where the mod keeps what the player earned, under the game folder (its data folder: `config/<mod id>/`).
pub fn earned_path(game_dir: &Path) -> PathBuf {
    game_dir.join("config").join("community_achievements").join("earned.tsv")
}

/// Everyone's counts: players sharing, and how many of them earned each achievement.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Stats {
    pub players: u64,
    pub earned: HashMap<String, u64>,
}

/// A counts address the launcher reads: on the service's host, over https, nothing that changes the host or the request.
pub fn stats_url_ok(url: &str) -> bool {
    url.starts_with(STATS_HOST) && url.len() > STATS_HOST.len() && !url.contains(['@', '\\', ' ', '"', '?', '#', '&']) && url.is_ascii()
}

/// Reads the service's answer to `GET /api/stats`: `{"players": n, "earned": {"<id>": n}}`. Entries that are not numbers are left out.
pub fn parse_stats(body: &[u8]) -> Result<Stats, String> {
    let v: serde_json::Value = serde_json::from_slice(body).map_err(|e| format!("The counts cannot be read: {}", e))?;
    let players = v.get("players").and_then(|p| p.as_u64()).ok_or("The counts have no number of players.")?;
    let earned = v.get("earned").and_then(|e| e.as_object()).map(|o| o.iter().filter_map(|(k, n)| n.as_u64().map(|n| (k.clone(), n))).collect()).unwrap_or_default();
    Ok(Stats { players, earned })
}

/// Reads the mod's `earned.tsv`: one `id<TAB>unix time` per line. Lines that do not fit are skipped.
pub fn parse_earned(text: &str) -> HashMap<String, i64> {
    text.lines()
        .filter_map(|line| {
            let (id, at) = line.split_once('\t')?;
            let at = at.trim().parse::<i64>().ok()?;
            (!id.is_empty()).then(|| (id.to_string(), at))
        })
        .collect()
}

/// What the player earned so far (nothing when the mod has not written the file yet).
pub fn read_earned(game_dir: &Path) -> HashMap<String, i64> {
    std::fs::read_to_string(earned_path(game_dir)).map(|t| parse_earned(&t)).unwrap_or_default()
}

/// This session's counts, and whether they are the kept copy (the service could not be reached).
#[derive(Default)]
pub struct StatsStore(Mutex<Option<(Stats, bool)>>);

/// The counts: this session's copy unless `refresh`, else one GET from `url`, else the file `cache` (offline). None when there is
/// nothing at all. The lock is held over the GET, so the page's calls make one request between them.
pub fn load_stats(store: &StatsStore, url: &str, cache: &Path, refresh: bool) -> Option<(Stats, bool)> {
    let mut mem = store.0.lock().unwrap_or_else(|e| e.into_inner());
    if let (Some(s), false) = (mem.as_ref(), refresh) {
        return Some(s.clone());
    }
    let fetched = fetch_stats(url).and_then(|body| parse_stats(&body).map(|s| (body, s)));
    let got = match fetched {
        Ok((body, s)) => {
            if let Err(e) = crate::settings::write_atomic(cache, &String::from_utf8_lossy(&body)) {
                eprintln!("achievement counts not kept: {}", e);
            }
            Some((s, false))
        }
        Err(e) => {
            eprintln!("achievement counts: {}", e);
            let kept = std::fs::read(cache).ok().and_then(|b| parse_stats(&b).ok());
            kept.or_else(|| mem.take().map(|(s, _)| s)).map(|s| (s, true))
        }
    };
    if got.is_some() {
        *mem = got.clone();
    }
    got
}

fn fetch_stats(url: &str) -> Result<Vec<u8>, String> {
    let resp = download::agent(false).get(url).call().map_err(|e| e.to_string())?;
    let mut body = Vec::new();
    resp.into_reader().take(STATS_LIMIT).read_to_end(&mut body).map_err(|e| e.to_string())?;
    Ok(body)
}

/// A picture the launcher takes: on the hub site over https, a plain `.webp` or `.png` file name. Gives the file name and its media type.
pub fn picture_ok(url: &str) -> Option<(&str, &'static str)> {
    let rest = url.strip_prefix(mods_catalog::HUB_SITE)?;
    let name = rest.rsplit('/').next()?;
    let plain = !name.is_empty() && !name.starts_with('.') && name.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '.'));
    let mime = if name.ends_with(".webp") { "image/webp" } else if name.ends_with(".png") { "image/png" } else { return None };
    (plain && !rest.contains("..") && !rest.contains(['?', '#', '&', '@', '\\', ' ', '"', ':'])).then_some((name, mime))
}

/// The file really is the picture its name says (a PNG signature, or RIFF....WEBP).
fn image_ok(bytes: &[u8], mime: &str) -> bool {
    match mime {
        "image/png" => bytes.starts_with(&[0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a]),
        _ => bytes.len() > 12 && &bytes[..4] == b"RIFF" && &bytes[8..12] == b"WEBP",
    }
}

fn data_url(bytes: &[u8], mime: &str) -> String {
    format!("data:{};base64,{}", mime, base64::engine::general_purpose::STANDARD.encode(bytes))
}

/// One picture as a `data:` URL: the kept file, else (when `fetch` and the hub answers) one GET that is then kept. "" for none.
fn picture_data(dir: &Path, a: &Achievement, fetch: bool, down: &AtomicBool) -> String {
    let Some((name, mime)) = picture_ok(&a.picture) else { return String::new() };
    let kept = dir.join(name);
    if let Some(b) = std::fs::read(&kept).ok().filter(|b| image_ok(b, mime)) {
        return data_url(&b, mime);
    }
    if !fetch || down.load(Ordering::SeqCst) {
        return String::new();
    }
    let got = download::agent(false).get(&a.picture).call().map_err(|e| e.to_string()).and_then(|resp| {
        let mut body = Vec::new();
        resp.into_reader().take(PICTURE_LIMIT + 1).read_to_end(&mut body).map_err(|e| e.to_string())?;
        if body.len() as u64 > PICTURE_LIMIT || !image_ok(&body, mime) {
            return Err("not a picture of the allowed kind and size".to_string());
        }
        Ok(body)
    });
    match got {
        Ok(body) => {
            if std::fs::create_dir_all(dir).is_ok() {
                if crate::fsutil::write_atomic(&kept, &body).is_err() {
                    eprintln!("achievement picture {} not kept in {}", name, dir.display());
                }
            }
            data_url(&body, mime)
        }
        Err(e) => {
            eprintln!("achievement picture {}: {}", a.picture, e);
            down.store(true, Ordering::SeqCst);   // the hub does not answer: the rest are not tried this time
            String::new()
        }
    }
}

/// The pictures of the list in order, a few fetched at once.
fn pictures(dir: &Path, list: &[Achievement], fetch: bool) -> Vec<String> {
    let down = AtomicBool::new(false);
    let mut out = vec![String::new(); list.len()];
    let per = list.len().div_ceil(PICTURE_THREADS).max(1);
    std::thread::scope(|s| {
        for (items, slots) in list.chunks(per).zip(out.chunks_mut(per)) {
            let down = &down;
            s.spawn(move || {
                for (a, slot) in items.iter().zip(slots.iter_mut()) {
                    *slot = picture_data(dir, a, fetch, down);
                }
            });
        }
    });
    out
}

/// One achievement of the page.
#[derive(Serialize, Clone, Debug)]
pub struct ItemView {
    pub id: String,
    pub name: String,
    pub desc: String,
    pub cat: String,
    pub goal: u64,
    /// a `data:` URL, "" when there is no picture
    pub picture: String,
    /// how many players have it; None when the counts are not known
    pub players: Option<u64>,
}

/// Everyone's counts as the page shows them.
#[derive(Serialize, Clone, Debug)]
pub struct StatsView {
    pub players: u64,
    /// the service could not be reached: these are the last counts the launcher saw
    pub kept: bool,
}

/// The Achievements page.
#[derive(Serialize, Clone, Debug)]
pub struct AchievementsView {
    /// the hub's list has the achievements block
    pub available: bool,
    /// the list is the kept copy (the hub could not be reached)
    pub offline: bool,
    /// the mod's id, to find it in the Mods page's list (modsList)
    pub mod_id: &'static str,
    /// in the hub's order; each item names its group in `cat`
    pub list: Vec<ItemView>,
    /// None when no counts are known (the hub names no address the launcher reads, or nothing was ever fetched)
    pub stats: Option<StatsView>,
    /// the mod's `earned.tsv`: achievement id to the unix time it was earned
    pub earned: HashMap<String, i64>,
}

/// The files the page keeps in the launcher's settings folder.
pub struct Kept {
    pub catalog: PathBuf,
    pub stats: PathBuf,
    pub pictures: PathBuf,
}

/// Builds the page. `refresh` reads the hub's list and the counts again; otherwise this session's copies are used.
pub fn view(game_dir: &Path, catalog: &CatalogStore, stats: &StatsStore, kept: &Kept, refresh: bool) -> Result<AchievementsView, String> {
    let (catalog, offline) = mods_catalog::load(catalog, &kept.catalog, refresh)?;
    let earned = read_earned(game_dir);
    let Some(block) = catalog.achievements else {
        return Ok(AchievementsView { available: false, offline, mod_id: MOD_ID, list: Vec::new(), stats: None, earned });
    };
    let counts = if stats_url_ok(&block.stats) { load_stats(stats, &block.stats, &kept.stats, refresh) } else { None };
    let pics = pictures(&kept.pictures, &block.list, !offline);
    let list = block
        .list
        .into_iter()
        .zip(pics)
        .map(|(a, picture)| ItemView {
            players: counts.as_ref().map(|(s, _)| s.earned.get(&a.id).copied().unwrap_or(0)),
            id: a.id,
            name: a.name,
            desc: a.desc,
            cat: a.cat,
            goal: a.goal,
            picture,
        })
        .collect();
    Ok(AchievementsView {
        available: true,
        offline,
        mod_id: MOD_ID,
        list,
        stats: counts.map(|(s, from_copy)| StatsView { players: s.players, kept: from_copy }),
        earned,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_the_services_counts() {
        let s = parse_stats(br#"{"players": 12, "earned": {"dex_5": 9, "dex_25": 3, "odd": "x"}, "more": 1}"#).unwrap();
        assert_eq!(s.players, 12);
        assert_eq!((s.earned["dex_5"], s.earned["dex_25"], s.earned.len()), (9, 3, 2));
        assert_eq!(parse_stats(br#"{"players": 0}"#).unwrap(), Stats::default());
        assert!(parse_stats(br#"{"earned": {}}"#).is_err() && parse_stats(b"nope").is_err());
    }

    #[test]
    fn counts_come_from_the_service_host_only() {
        assert!(stats_url_ok("https://achievements.pokopia-native.workers.dev/api/stats"));
        for bad in [
            "http://achievements.pokopia-native.workers.dev/api/stats",
            "https://achievements.pokopia-native.workers.dev.evil.example/api/stats",
            "https://achievements.pokopia-native.workers.dev@evil.example/api/stats",
            "https://achievements.pokopia-native.workers.dev/",
            "https://achievements.pokopia-native.workers.dev/api/stats?x=1",
            "https://evil.example/api/stats",
            "https://pokopia-native.vercel.app/api/stats",
            "",
        ] {
            assert!(!stats_url_ok(bad), "{bad}");
        }
    }

    #[test]
    fn reads_the_mods_earned_file() {
        let e = parse_earned("dex_5\t1762900000\nstreak_3\t1762990000\r\nbroken\nbad\tx\n\t5\n");
        assert_eq!((e.len(), e["dex_5"], e["streak_3"]), (2, 1762900000, 1762990000));
        assert!(parse_earned("").is_empty());
    }

    #[test]
    fn a_missing_earned_file_is_nothing_earned() {
        let dir = std::env::temp_dir().join(format!("recompia-ach-test-{}", std::process::id()));
        assert!(read_earned(&dir).is_empty());
        let file = earned_path(&dir);
        std::fs::create_dir_all(file.parent().unwrap()).unwrap();
        std::fs::write(&file, "relic_1\t5\n").unwrap();
        assert_eq!(read_earned(&dir)["relic_1"], 5);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn pictures_come_from_the_hub_site_only() {
        assert_eq!(picture_ok("https://pokopia-native.vercel.app/img/ach/dex_5.webp"), Some(("dex_5.webp", "image/webp")));
        assert_eq!(picture_ok("https://pokopia-native.vercel.app/img/ach/a-b.png"), Some(("a-b.png", "image/png")));
        for bad in [
            "https://pokopia-native.vercel.app.evil.example/img/ach/dex_5.webp",
            "https://evil.example/img/ach/dex_5.webp",
            "http://pokopia-native.vercel.app/img/ach/dex_5.webp",
            "https://pokopia-native.vercel.app/img/ach/dex_5.svg",
            "https://pokopia-native.vercel.app/img/ach/dex_5.webp?x=1",
            "https://pokopia-native.vercel.app/img/ach/../x.webp",
            "https://pokopia-native.vercel.app/img/ach/.webp",
            "https://pokopia-native.vercel.app/img/ach/",
            "https://pokopia-native.vercel.app/img/ach/a b.png",
            "https://pokopia-native.vercel.app/img/ach/a@b.png",
            "",
        ] {
            assert!(picture_ok(bad).is_none(), "{bad}");
        }
    }

    #[test]
    fn a_picture_must_be_the_kind_its_name_says() {
        let mut png = vec![0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
        png.extend_from_slice(&[0; 8]);
        let mut webp = b"RIFF\0\0\0\0WEBPVP8 ".to_vec();
        webp.extend_from_slice(&[0; 8]);
        assert!(image_ok(&png, "image/png") && !image_ok(&png, "image/webp"));
        assert!(image_ok(&webp, "image/webp") && !image_ok(&webp, "image/png"));
        assert!(!image_ok(b"<html>", "image/png") && !image_ok(b"", "image/webp"));
        assert_eq!(data_url(b"ab", "image/png"), "data:image/png;base64,YWI=");
    }

    #[test]
    fn kept_pictures_are_used_without_the_network() {
        let dir = std::env::temp_dir().join(format!("recompia-ach-pics-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let mut png = vec![0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
        png.push(1);
        std::fs::write(dir.join("a.png"), &png).unwrap();
        let a = |p: &str| Achievement { id: "a".into(), name: "A".into(), desc: String::new(), cat: String::new(), goal: 1, picture: p.into() };
        let got = pictures(&dir, &[a("https://pokopia-native.vercel.app/img/ach/a.png"), a("https://pokopia-native.vercel.app/img/ach/b.png"), a("https://evil.example/c.png"), a("")], false);
        assert!(got[0].starts_with("data:image/png;base64,") && got[1..].iter().all(|p| p.is_empty()));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
