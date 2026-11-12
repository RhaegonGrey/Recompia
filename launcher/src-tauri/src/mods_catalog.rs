// SPDX-License-Identifier: GPL-3.0-or-later
//! The Mods page's Browse tab: the Modtopia mod hub's list, `catalog.json` (schema 1). Its mods are checked by hand by the hub's team
//! before they are listed. The launcher reads it from the hub only, keeps the last good copy in its settings folder for browsing offline,
//! and shows only the mods made for this game version whose `needs` range holds the core this launcher carries. Fields it does not know
//! are ignored (schema 1 may grow); an entry it cannot read is left out, never the whole list.

use serde::{Deserialize, Serialize};
use std::path::Path;
use std::sync::Mutex;

use crate::download;
use crate::modtopia_ffi::{self as ffi, CORE_VERSION, GAME_VERSION};

pub const CATALOG_URL: &str = "https://pokopia-native.vercel.app/mods/catalog.json";
/// Where the hub's mod pages live (a mod's `page`, the hub, the submit form): the only links of the list the launcher opens.
pub const HUB_SITE: &str = "https://pokopia-native.vercel.app/";
/// The hub's download server: every install comes from here and nowhere else.
pub const DOWNLOAD_HOST: &str = "https://downloads.pokopia-native.workers.dev/";
const SCHEMA: u64 = 1;
const LIST_LIMIT: u64 = 4 << 20;
/// Longest text the launcher keeps of one hub field (a description, a setting's label, a group's name): the hub's words are plain text,
/// never markup, and a runaway one is cut rather than drawn whole.
const TEXT_LIMIT: usize = 4000;
const SETTINGS_LIMIT: usize = 40;

/// A field the launcher only shows: a wrong type reads as absent, never as an unreadable entry.
fn lenient<'de, D, T>(d: D) -> Result<T, D::Error>
where
    D: serde::Deserializer<'de>,
    T: serde::de::DeserializeOwned + Default,
{
    let v = serde_json::Value::deserialize(d)?;
    Ok(serde_json::from_value(v).unwrap_or_default())
}

fn clip(s: &str, max: usize) -> String {
    s.trim().chars().take(max).collect()
}

/// A published package: its address on the download server, file name, size and SHA-256.
#[derive(Deserialize, Serialize, Clone, Debug, PartialEq)]
pub struct Download {
    pub url: String,
    pub file: String,
    pub size: u64,
    pub sha256: String,
}

/// One mod of the list as the hub publishes it (the fields the launcher uses).
#[derive(Deserialize, Clone, Debug)]
pub struct Entry {
    pub id: String,
    pub name: String,
    pub version: String,
    #[serde(default)]
    pub summary: String,
    /// the whole text, shown in Details (the summary is its first line)
    #[serde(default, deserialize_with = "lenient")]
    pub description: String,
    /// what the mod lets the player set: one label each
    #[serde(default, deserialize_with = "lenient")]
    pub settings: Vec<String>,
    /// `true`/`false` (the hub's flag: tried on the versions in `game`) or the list of game versions it was tried on
    #[serde(default, deserialize_with = "lenient")]
    pub tested: serde_json::Value,
    /// the key of one of the list's `groups`
    #[serde(default, deserialize_with = "lenient")]
    pub group: String,
    /// where the mod's code is published: shown as a link only when it is one the launcher opens (open.rs), else not at all
    #[serde(default, deserialize_with = "lenient")]
    pub source: String,
    /// "gameplay" or "visual"
    #[serde(default)]
    pub affects: String,
    #[serde(default)]
    pub network: bool,
    #[serde(default)]
    pub game: Vec<String>,
    #[serde(default)]
    pub needs: String,
    #[serde(default)]
    pub authors: Vec<String>,
    #[serde(default)]
    pub license: String,
    #[serde(default)]
    pub page: String,
    /// null until the mod is out ("Coming soon")
    #[serde(default)]
    pub download: Option<Download>,
}

/// One achievement of the community tracker's list, as the hub publishes it (names and sentences are the hub's, in English).
#[derive(Deserialize, Serialize, Clone, Debug, PartialEq)]
pub struct Achievement {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub desc: String,
    /// the group it is listed under; the list's order is the hub's order
    #[serde(default)]
    pub cat: String,
    #[serde(default)]
    pub goal: u64,
    /// the picture's address on the hub site ("" = none)
    #[serde(default)]
    pub picture: String,
}

/// The catalog's `achievements` block: where everyone's counts are and the list (the page shows both in the launcher; its web
/// tracker page and licence link are not read).
#[derive(Clone, Debug, Default)]
pub struct Achievements {
    pub stats: String,
    pub list: Vec<Achievement>,
}

/// A group of the list: the filter chips of Browse (hub text, plain).
#[derive(Deserialize, Serialize, Clone, Debug, PartialEq)]
pub struct Group {
    pub key: String,
    pub label: String,
}

/// The list, read.
#[derive(Clone, Debug, Default)]
pub struct Catalog {
    /// nothing is installable yet: every Install stays off
    pub preview: bool,
    pub site: String,
    pub submit: String,
    pub groups: Vec<Group>,
    pub mods: Vec<Entry>,
    /// None when the hub's list has no such block (the Achievements page says so)
    pub achievements: Option<Achievements>,
}

impl Entry {
    /// The game versions the hub tried the mod on: its own list, or `game` when the hub only says `true`.
    pub fn tested_versions(&self) -> Vec<String> {
        match &self.tested {
            serde_json::Value::Bool(true) => self.game.clone(),
            serde_json::Value::Array(a) => a.iter().filter_map(|v| v.as_str().map(str::to_string)).collect(),
            _ => Vec::new(),
        }
    }
}

/// Reads a catalog.json. Err for anything but schema 1; entries that cannot be read are left out.
pub fn parse(body: &[u8]) -> Result<Catalog, String> {
    let v: serde_json::Value = serde_json::from_slice(body).map_err(|e| format!("The mod list cannot be read: {}", e))?;
    if v.get("schema").and_then(|s| s.as_u64()) != Some(SCHEMA) {
        return Err("The mod list is in a newer format than this launcher reads. Update the launcher to browse mods.".into());
    }
    let text = |k: &str| v.get(k).and_then(|s| s.as_str()).unwrap_or("").to_string();
    let mods = v.get("mods").and_then(|m| m.as_array()).map(|a| a.iter().filter_map(|m| serde_json::from_value(m.clone()).ok()).collect()).unwrap_or_default();
    let achievements = v.get("achievements").filter(|a| a.is_object()).map(|a| {
        let list = a.get("list").and_then(|l| l.as_array()).map(|l| l.iter().filter_map(|x| serde_json::from_value(x.clone()).ok()).collect()).unwrap_or_default();
        Achievements { stats: a.get("stats").and_then(|s| s.as_str()).unwrap_or("").to_string(), list }
    });
    let groups = v
        .get("groups")
        .and_then(|g| g.as_array())
        .map(|a| a.iter().filter_map(|g| serde_json::from_value::<Group>(g.clone()).ok()).filter(|g| !g.key.is_empty() && !g.label.trim().is_empty()).collect())
        .unwrap_or_default();
    Ok(Catalog { preview: v.get("preview").and_then(|p| p.as_bool()).unwrap_or(false), site: text("site"), submit: text("submit"), groups, mods, achievements })
}

/// Made for this game version, and for a core this launcher has.
pub fn fits(e: &Entry) -> bool {
    e.game.iter().any(|g| g == GAME_VERSION) && ffi::version_satisfies(CORE_VERSION, &e.needs) == Some(true)
}

/// A link of the list the launcher may open in the browser: a page of the hub. Anything else shows as text only.
pub fn hub_link(url: &str) -> bool {
    url.starts_with(HUB_SITE) && !url.chars().any(|c| c.is_whitespace() || c == '"' || c == '&' || c == '\\')
}

/// A download the launcher takes: on the hub's download server, over https, a plain `.pkmod` file name.
pub fn download_ok(d: &Download) -> bool {
    let name_ok = !d.file.is_empty() && d.file.ends_with(".pkmod") && !d.file.starts_with('.') && !d.file.contains(['/', '\\', ':', '\0']);
    let hex_ok = d.sha256.len() == 64 && d.sha256.chars().all(|c| c.is_ascii_hexdigit());
    d.url.starts_with(DOWNLOAD_HOST) && !d.url.contains(['@', '\\', ' ', '"']) && name_ok && hex_ok && d.size > 0
}

/// This session's copy of the list, and whether it came from the cache file (offline).
#[derive(Default)]
pub struct CatalogStore(Mutex<Option<(Catalog, bool)>>);

/// The list: this session's copy unless `refresh`, else one GET from the hub, else the cache file `cache` (offline), else an error.
/// The lock is held over the GET, so the page's calls make one request between them.
pub fn load(store: &CatalogStore, cache: &Path, refresh: bool) -> Result<(Catalog, bool), String> {
    let mut mem = store.0.lock().unwrap_or_else(|e| e.into_inner());
    if let (Some(c), false) = (mem.as_ref(), refresh) {
        return Ok(c.clone());
    }
    let fetched = fetch().and_then(|body| parse(&body).map(|c| (body, c)));
    let got = match fetched {
        Ok((body, c)) => {
            if let Err(e) = crate::settings::write_atomic(cache, &String::from_utf8_lossy(&body)) {
                eprintln!("mod list not cached: {}", e);
            }
            (c, false)
        }
        Err(e) => {
            eprintln!("mod list: {}", e);
            let cached = std::fs::read(cache).ok().and_then(|b| parse(&b).ok());
            match cached.or_else(|| mem.take().map(|(c, _)| c)) {
                Some(c) => (c, true),
                None => return Err("The mod list could not be loaded. Check the internet connection and try again.".into()),
            }
        }
    };
    *mem = Some(got.clone());
    Ok(got)
}

fn fetch() -> Result<Vec<u8>, String> {
    use std::io::Read;
    let resp = download::agent(false).get(CATALOG_URL).call().map_err(|e| e.to_string())?;
    let mut body = Vec::new();
    resp.into_reader().take(LIST_LIMIT).read_to_end(&mut body).map_err(|e| e.to_string())?;
    Ok(body)
}

/// One mod of the Browse tab.
#[derive(Serialize, Clone, Debug)]
pub struct CardView {
    pub id: String,
    pub name: String,
    pub version: String,
    pub summary: String,
    /// the whole text ("" when the hub gave none: the page then shows the summary)
    pub description: String,
    pub settings: Vec<String>,
    /// the game versions it was tried on
    pub tested: Vec<String>,
    /// the key of its group in `CatalogView.groups`, "" when it has none the list names
    pub group: String,
    /// the hub's link for where the mod comes from, "" when it is not one the launcher opens
    pub source: String,
    /// the package size in bytes, 0 when there is no download yet
    pub size: u64,
    pub authors: Vec<String>,
    pub license: String,
    pub gameplay: bool,
    pub network: bool,
    /// the mod's page on the hub, "" when it is not one the launcher opens
    pub page: String,
    /// it has a download (else "Coming soon")
    pub available: bool,
    /// the installed version, "" when it is not installed
    pub installed: String,
    /// the list has a newer version than the installed one
    pub newer: bool,
}

/// The Browse tab.
#[derive(Serialize, Clone, Debug)]
pub struct CatalogView {
    /// read from the copy kept for offline browsing
    pub offline: bool,
    pub preview: bool,
    /// the game version this launcher is for (a card's `tested` is compared with it)
    pub game: String,
    /// the groups that have a card here, in the hub's order
    pub groups: Vec<Group>,
    /// the hub's mods page and its submit form, "" when not links the launcher opens
    pub hub: String,
    pub submit: String,
    pub mods: Vec<CardView>,
}

/// The cards of the mods that fit, with what is installed (`installed`: id and version of each installed mod).
pub fn view(c: &Catalog, offline: bool, installed: &[(String, String)]) -> CatalogView {
    let link = |u: &str| if hub_link(u) { u.to_string() } else { String::new() };
    let mods: Vec<CardView> = c
        .mods
        .iter()
        .filter(|e| fits(e))
        .map(|e| {
            let have = installed.iter().find(|(id, _)| *id == e.id).map(|(_, v)| v.clone()).unwrap_or_default();
            CardView {
                id: e.id.clone(),
                name: e.name.clone(),
                version: e.version.clone(),
                summary: e.summary.clone(),
                description: clip(&e.description, TEXT_LIMIT),
                settings: e.settings.iter().map(|s| clip(s, TEXT_LIMIT)).filter(|s| !s.is_empty()).take(SETTINGS_LIMIT).collect(),
                tested: e.tested_versions(),
                group: if c.groups.iter().any(|g| g.key == e.group) { e.group.clone() } else { String::new() },
                source: if crate::open::openable_link(&e.source) { e.source.clone() } else { String::new() },
                size: e.download.as_ref().map_or(0, |d| d.size),
                authors: e.authors.clone(),
                license: e.license.clone(),
                gameplay: e.affects != "visual",
                network: e.network,
                page: link(&e.page),
                available: e.download.as_ref().is_some_and(download_ok),
                newer: !have.is_empty() && ffi::version_compare(&e.version, &have) == std::cmp::Ordering::Greater,
                installed: have,
            }
        })
        .collect();
    let groups = c.groups.iter().filter(|g| mods.iter().any(|m| m.group == g.key)).map(|g| Group { key: g.key.clone(), label: clip(&g.label, 80) }).collect();
    let hub = if c.site.is_empty() { String::new() } else { link(&format!("{}/mods", c.site.trim_end_matches('/'))) };
    CatalogView { offline, preview: c.preview, game: GAME_VERSION.to_string(), groups, hub, submit: link(&c.submit), mods }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = r#"{"schema": 1, "preview": false, "site": "https://pokopia-native.vercel.app", "submit": "https://pokopia-native.vercel.app/mods/submit",
      "future_field": [1, 2],
      "mods": [
        {"id": "auto_run", "name": "Auto-Run", "version": "1.0.0", "affects": "visual", "network": false, "game": ["2.0.1", "1.0.5"],
         "needs": ">=0.3.0 <1.0.0", "authors": ["The Modtopia contributors"], "license": "MIT", "page": "https://pokopia-native.vercel.app/mods/auto-run",
         "download": {"url": "https://downloads.pokopia-native.workers.dev/mods/auto-run/1.0.0/auto_run-1.0.0.pkmod", "file": "auto_run-1.0.0.pkmod",
                      "size": 77330, "sha256": "e70a6ef937952a9fe23aa7847df56f787fa8164129d53a081c1bec664b228ab8"}, "new": true},
        {"id": "old_only", "name": "Old", "version": "1.0.0", "game": ["1.0.5"], "needs": ">=0.1.0"},
        {"id": "too_new", "name": "New", "version": "1.0.0", "game": ["2.0.1"], "needs": ">=0.7.0"},
        {"id": "soon", "name": "Soon", "version": "0.9.0", "affects": "gameplay", "game": ["2.0.1"], "needs": ">=0.1.0 <1.0.0", "download": null},
        {"name": "no id"}
      ]}"#;

    #[test]
    fn reads_schema_1_and_keeps_only_what_fits() {
        let c = parse(SAMPLE.as_bytes()).unwrap();
        assert_eq!(c.mods.len(), 4, "the entry without an id is left out, not the list");
        let v = view(&c, false, &[("auto_run".into(), "0.9.0".into())]);
        let ids: Vec<&str> = v.mods.iter().map(|m| m.id.as_str()).collect();
        assert_eq!(ids, ["auto_run", "soon"]);
        assert!(v.mods[0].available && v.mods[0].newer && !v.mods[0].gameplay && v.mods[0].installed == "0.9.0");
        assert!(!v.mods[1].available && v.mods[1].gameplay && v.mods[1].installed.is_empty());
        assert_eq!(v.submit, "https://pokopia-native.vercel.app/mods/submit");
        assert_eq!(v.hub, "https://pokopia-native.vercel.app/mods");
        assert!(parse(br#"{"schema": 2, "mods": []}"#).is_err());
    }

    const RICH: &str = r#"{"schema": 1, "preview": false, "site": "https://pokopia-native.vercel.app",
      "groups": [{"key": "camera", "label": "Camera and photos", "icon": "camera"}, {"key": "empty", "label": "No mod here"}, {"key": "", "label": "No key"}, {"key": "bad"}],
      "mods": [
        {"id": "a", "name": "A", "version": "1.0.0", "summary": "Short.", "description": "Short. And the rest of it, on and on.", "group": "camera", "game": ["2.0.1", "1.0.5"],
         "needs": ">=0.3.0 <1.0.0", "settings": ["First label", "Second label"], "tested": true, "source": "https://github.com/example/a", "icon": "bolt",
         "download": {"url": "https://downloads.pokopia-native.workers.dev/mods/a/1.0.0/a-1.0.0.pkmod", "file": "a-1.0.0.pkmod", "size": 4321, "sha256": "e70a6ef937952a9fe23aa7847df56f787fa8164129d53a081c1bec664b228ab8"}},
        {"id": "b", "name": "B", "version": "1.0.0", "summary": "Only this.", "group": "nowhere", "game": ["2.0.1"], "needs": ">=0.3.0 <1.0.0",
         "tested": ["2.0.1"], "source": "catalog"},
        {"id": "c", "name": "C", "version": "1.0.0", "game": ["2.0.1"], "needs": ">=0.3.0 <1.0.0", "tested": false, "settings": "not a list", "description": 7, "group": ["x"]}
      ]}"#;

    #[test]
    fn keeps_whole_descriptions_and_the_labels_of_a_card() {
        let v = view(&parse(RICH.as_bytes()).unwrap(), false, &[]);
        assert_eq!(v.mods.len(), 3, "a field of the wrong type is absent, not a lost entry");
        let a = &v.mods[0];
        assert_eq!(a.summary, "Short.");
        assert_eq!(a.description, "Short. And the rest of it, on and on.");
        assert_eq!(a.settings, ["First label", "Second label"]);
        assert_eq!((a.size, a.source.as_str()), (4321, "https://github.com/example/a"));
        let c = &v.mods[2];
        assert!(c.description.is_empty() && c.settings.is_empty() && c.group.is_empty());
    }

    #[test]
    fn a_long_description_is_cut_not_dropped() {
        let long = "x".repeat(TEXT_LIMIT + 500);
        let body = format!(r#"{{"schema": 1, "mods": [{{"id": "a", "name": "A", "version": "1.0.0", "description": "{long}", "game": ["2.0.1"], "needs": ">=0.3.0 <1.0.0"}}]}}"#);
        let v = view(&parse(body.as_bytes()).unwrap(), false, &[]);
        assert_eq!(v.mods[0].description.chars().count(), TEXT_LIMIT);
    }

    #[test]
    fn groups_keep_the_hubs_order_and_only_those_with_a_card() {
        let c = parse(RICH.as_bytes()).unwrap();
        assert_eq!(c.groups.iter().map(|g| g.key.as_str()).collect::<Vec<_>>(), ["camera", "empty"], "a group without a key or a label is left out");
        let v = view(&c, false, &[]);
        assert_eq!(v.groups, [Group { key: "camera".into(), label: "Camera and photos".into() }], "no card is in `empty`");
        assert_eq!(v.mods[0].group, "camera");
        assert!(v.mods[1].group.is_empty(), "a group the list does not name is shown under All only");
        assert!(parse(br#"{"schema": 1, "mods": []}"#).unwrap().groups.is_empty());
    }

    #[test]
    fn tested_is_the_hubs_flag_or_a_list_of_game_versions() {
        let v = view(&parse(RICH.as_bytes()).unwrap(), false, &[]);
        assert_eq!(v.game, GAME_VERSION);
        assert_eq!(v.mods[0].tested, ["2.0.1", "1.0.5"], "`true` stands for the versions the entry lists");
        assert_eq!(v.mods[1].tested, ["2.0.1"]);
        assert!(v.mods[2].tested.is_empty());
        assert!(v.mods[1].source.is_empty(), "a source that is not a link the launcher opens is not passed on");
    }

    #[test]
    fn reads_the_achievements_block_and_its_absence() {
        let with = r#"{"schema": 1, "mods": [], "achievements": {"page": "https://pokopia-native.vercel.app/mods/achievements",
          "stats": "https://achievements.pokopia-native.workers.dev/api/stats", "license": "https://pokopia-native.vercel.app/img/ach/LICENSE.txt", "extra": 1,
          "list": [{"id": "dex_5", "name": "New Neighbors", "desc": "Have 5 Pokemon in your Pokedex.", "cat": "pokedex", "goal": 5,
                    "picture": "https://pokopia-native.vercel.app/img/ach/dex_5.webp", "new": true}, {"name": "no id"}, {"id": "dex_25", "name": "Busy Street"}]}}"#;
        let a = parse(with.as_bytes()).unwrap().achievements.unwrap();
        assert_eq!(a.list.len(), 2, "an entry without an id is left out, not the list");
        assert_eq!((a.list[0].cat.as_str(), a.list[0].goal), ("pokedex", 5));
        assert!(a.list[1].picture.is_empty() && a.list[1].cat.is_empty());
        assert!(a.stats.ends_with("/api/stats"));
        assert!(parse(br#"{"schema": 1, "mods": []}"#).unwrap().achievements.is_none());
        assert!(parse(br#"{"schema": 1, "achievements": []}"#).unwrap().achievements.is_none());
    }

    #[test]
    fn downloads_come_from_the_hub_server_only() {
        let good = Download { url: format!("{DOWNLOAD_HOST}mods/a/1.0.0/a-1.0.0.pkmod"), file: "a-1.0.0.pkmod".into(), size: 10, sha256: "a".repeat(64) };
        assert!(download_ok(&good));
        let bad = |f: &dyn Fn(&mut Download)| {
            let mut d = good.clone();
            f(&mut d);
            !download_ok(&d)
        };
        assert!(bad(&|d| d.url = "https://downloads.pokopia-native.workers.dev.evil.example/a.pkmod".into()));
        assert!(bad(&|d| d.url = "http://downloads.pokopia-native.workers.dev/a.pkmod".into()));
        assert!(bad(&|d| d.url = "https://github.com/x/a.pkmod".into()));
        assert!(bad(&|d| d.file = "../a.pkmod".into()));
        assert!(bad(&|d| d.file = "a.dll".into()));
        assert!(bad(&|d| d.sha256 = "zz".into()));
        assert!(bad(&|d| d.size = 0));
        assert!(hub_link("https://pokopia-native.vercel.app/mods/auto-run") && !hub_link("https://pokopia-native.vercel.app.evil.example/x"));
    }
}
