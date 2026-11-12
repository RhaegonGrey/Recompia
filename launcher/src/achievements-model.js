// The Achievements page, the pure part: the hub's list grouped as the hub orders it, the percentages and the rarest ones, what the
// player has earned, and what the Community Achievements mod's state means for the page. No DOM and no Tauri, so
// `node scripts/test-achievements-model.mjs` runs it. The page is pages/achievements.js; the list comes from Rust (achievements.rs
// AchievementsView), the mod's state from the Mods page's list (mods.rs ModsView).
import { t } from './i18n.js';
import { lockAction } from './mods-model.js';

/** The title of a group of the hub's list (its `cat`). The hub names no titles, so these are ours; an unknown group shows its id. */
export function categoryTitle(cat) {
  switch (cat) {
    case 'pokedex': return t('Pokédex');
    case 'habitats': return t('Habitats');
    case 'items': return t('Items');
    case 'recipes': return t('Recipes');
    case 'emotes': return t('Emotes');
    case 'town': return t('Towns');
    case 'streak': return t('Days in a row');
    case 'dishes': return t('Dishes');
    case 'relics': return t('Relics');
    case 'photos': return t('Photos');
    case 'buildings': return t('Buildings');
    default: return cat ? cat.charAt(0).toUpperCase() + cat.slice(1) : t('Other');
  }
}

/** The list as groups, in the order each group first appears in the hub's list: [{ cat, title, items }]. */
export function groupList(list) {
  const groups = [];
  for (const item of list) {
    let g = groups.find((x) => x.cat === item.cat);
    if (!g) groups.push((g = { cat: item.cat, title: categoryTitle(item.cat), items: [] }));
    g.items.push(item);
  }
  return groups;
}

/** How many of the list the player has earned (an id in `earned` that the list does not have is not counted). */
export const earnedCount = (list, earned) => list.filter((a) => Object.hasOwn(earned, a.id)).length;

/** The share of the players who have an achievement, as the mod words it: "12%", "under 1%", "0%"; '' when the counts are not known. */
export function percentText(count, players) {
  if (count == null || !(players > 0)) return '';
  if (count <= 0) return '0%';
  const pct = (count / players) * 100;
  return pct < 1 ? t('under 1%') : `${Math.min(100, Math.round(pct))}%`;
}

/** The players' side of the summary: the number sharing and the achievements earned in total; null when the counts are not known. */
export function totals(view) {
  if (!view.stats) return null;
  return { players: view.stats.players, earned: view.list.reduce((n, a) => n + (a.players ?? 0), 0) };
}

/** The rarest right now: up to `n` achievements somebody has earned, fewest players first (the list's order breaks a tie). */
export function rarest(list, n = 3) {
  return list
    .map((item, i) => ({ item, i }))
    .filter(({ item }) => item.players > 0)
    .sort((a, b) => a.item.players - b.item.players || a.i - b.i)
    .slice(0, n)
    .map(({ item }) => item);
}

/** The date an achievement was earned, from the unix time the mod keeps. */
export const dateText = (unix, locale) => new Date(unix * 1000).toLocaleDateString(locale, { year: 'numeric', month: 'short', day: 'numeric' });

/** What the mod's state is, from the Mods page's list: { kind, mod, locks }. kind: 'unknown' (the list could not be read), 'missing' (not
 *  installed), 'broken', 'off' (installed, switched off), 'private' (on, sharing off) or 'sharing' (on, sharing on). locks: what the
 *  launcher keeps on ({ kept, sharing }, mods_lock.rs), null when the list does not say. */
export function modState(modsView, modId) {
  if (!modsView) return { kind: 'unknown', mod: null, locks: null };
  const locks = modsView.locks ?? null;
  const mod = modsView.mods.find((m) => m.id === modId);
  if (!mod) return { kind: 'missing', mod: null, locks };
  if (mod.state === 'error') return { kind: 'broken', mod, locks };
  if (!mod.enabled) return { kind: 'off', mod, locks };
  const share = mod.settings.find((s) => s.key === 'share');
  return { kind: share && share.number === 0 ? 'private' : 'sharing', mod, locks };
}

/** The lock sentences for the mod card: the mod kept on, its sharing kept on (each while the launcher locks it). */
export function lockLines(state) {
  const out = [];
  if (state.mod?.locked) out.push(t('Locked: the launcher keeps it on, so your achievements and stats are recorded. Unlock it to switch it off or remove it.'));
  if (state.mod?.settings.some((s) => s.key === 'share' && s.locked)) out.push(t('Sharing is locked on. Unlock it to switch sharing off.'));
  return out;
}

/** The one sentence for the mod's state. */
export function modLine(state) {
  switch (state.kind) {
    case 'missing': return t('The Community Achievements mod is not installed, so nothing is earned yet. Install it here to start earning achievements.');
    case 'broken': return t('The Community Achievements mod has a problem and does not run.');
    case 'off': return t('The Community Achievements mod is installed but switched off, so nothing is earned.');
    case 'private': return t('The Community Achievements mod is on. Sharing is off: your achievements stay on this PC and you are not counted.');
    case 'sharing': return t('The Community Achievements mod is on and sharing: you are counted with the other players.');
    default: return t('The mods could not be read, so the mod\'s state is not known.');
  }
}

/** The buttons the state offers, in order: 'install' (from the hub, in the launcher), 'folder' (the mods folder), 'enable' (switch it on), 'share'
 *  (switch sharing on), then the locks: 'unlock' / 'lock' (the mod), 'unlock-sharing' / 'lock-sharing'. A missing mod the player unlocked
 *  offers 'lock' (it installs it again). */
export function modActions(state) {
  const out = {
    missing: ['install', 'folder'],
    broken: ['folder'],
    off: ['enable'],
    private: ['share'],
  }[state.kind] ?? [];
  if (state.kind === 'missing') return state.locks && !state.locks.kept ? [...out, 'lock'] : out;
  if (!state.mod) return out;
  const mod = lockAction(state.mod);
  const sharing = lockAction(state.mod.settings.find((s) => s.key === 'share'));
  return [...out, ...(mod ? [mod] : []), ...(sharing ? [`${sharing}-sharing`] : [])];
}

/** The achievement pictures' licence (the hub ships it beside them), shown in the launcher. */
export const PICTURES_LICENCE = `Microsoft Fluent Emoji (https://github.com/microsoft/fluentui-emoji)

MIT License

Copyright (c) Microsoft Corporation.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.`;

/** What sharing sends, for the page (the mod's own words in its design notes: Privacy). */
export const sharingText = () =>
  t('Sharing sends the list of the achievements you have earned and the game version, under a random number made on your PC. Never your name, your save or anything else from your PC. Only the mod sends it; the launcher sends nothing.');
