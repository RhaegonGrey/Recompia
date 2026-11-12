// The Mods page, the pure part: what a mod's row and a Browse card say, the questions asked before a mod's code first runs, and the
// value a setting's control sends back. No DOM and no Tauri, so `node scripts/test-mods-model.mjs` runs it. The page is pages/mods.js;
// the list and the catalog come from Rust (mods.rs ModView, mods_catalog.rs CardView).
import { t } from './i18n.js';

/** The small labels on an installed mod's row: [{ text, kind: '' | 'ok' | 'warn' | 'danger' }]. */
export function badges(mod) {
  const out = [];
  if (mod.state === 'error') out.push({ text: t('Problem'), kind: 'danger' });
  if (mod.locked) out.push({ text: t('Locked'), kind: 'ok' });
  if (mod.needs_yes) out.push({ text: t('Not allowed yet'), kind: 'warn' });
  out.push(mod.gameplay ? { text: t('Changes gameplay'), kind: 'warn' } : { text: t('Looks only'), kind: '' });
  if (mod.network) out.push({ text: t('Goes online'), kind: '' });
  return out;
}

/** "By A, B. Licence: MIT." for a mod or a card (authors are a string from the core, a list from the catalog). */
export function byLine(m) {
  const authors = Array.isArray(m.authors) ? m.authors.join(', ') : m.authors;
  const licence = m.license || t('not stated');
  return authors ? t('By {authors}. Licence: {licence}.', { authors, licence }) : t('Licence: {licence}.', { licence });
}

/** The one line that tells the player which mods were updated by themselves: [{ name, from, to }] from the launcher. */
export function updatedLine(done) {
  return t('Updated by itself: {list}', { list: done.map((u) => `${u.name} ${u.from} → ${u.to}`).join(', ') });
}

/** What a Browse card's button is: 'install', 'update' (a newer version than the installed one), 'installed', 'soon' (no download
 *  yet) or 'closed' (the hub says nothing is installable yet). */
export function cardAction(card, preview) {
  if (!card.available) return 'soon';
  if (card.installed && !card.newer) return 'installed';
  if (preview) return 'closed';
  return card.installed ? 'update' : 'install';
}

/** The hub's card is marked as tried on this game version (`tested` is the list of versions Rust passes on, mods_catalog.rs). */
export const isTested = (card, game) => !!game && Array.isArray(card.tested) && card.tested.includes(game);

/** The cards of one group; '' is every card. */
export const filterByGroup = (cards, key) => (key ? cards.filter((c) => c.group === key) : cards);

/** The filter chips: "All" first, then the groups in the hub's order. Nothing when the list has no groups. */
export function groupChips(groups) {
  return groups?.length ? [{ key: '', label: t('All') }, ...groups.map((g) => ({ key: g.key, label: g.label }))] : [];
}

/** The chosen group, or '' (All) when a refreshed list no longer has it. */
export const keepGroup = (groups, key) => (groups?.some((g) => g.key === key) ? key : '');

/** A package size for a person: "75 KB", "1.2 MB". */
export const sizeText = (bytes) => (bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : bytes >= 1024 ? `${Math.round(bytes / 1024)} KB` : `${bytes} B`);

/** What a card's Details dialog says, all plain text: { title, by, description, settings: [label], facts: [line], links: [{ label, url }] }.
 *  The description falls back to the summary when the hub gave no longer text. `game` is the game version of the list. */
export function detailsView(card, game) {
  const facts = [];
  if (card.tested?.length) facts.push(t('Tested on game version {versions}.', { versions: card.tested.join(', ') }));
  else if (game) facts.push(t('The hub has not marked it as tested on game version {version}.', { version: game }));
  if (card.size > 0) facts.push(t('Download size: {size}', { size: sizeText(card.size) }));
  if (card.gameplay) facts.push(t('It changes gameplay: everyone in a Link Play session needs it too.'));
  if (card.network) facts.push(t('It goes online.'));
  const links = [];
  if (card.page) links.push({ label: t('Mod page'), url: card.page });
  if (card.source) links.push({ label: t('Source'), url: card.source });
  return {
    title: `${card.name} ${card.version}`,
    by: byLine(card),
    description: card.description || card.summary || '',
    settings: card.settings || [],
    facts,
    links,
  };
}

/** The lines of the Mods page's two questions about what a mod does, in the order shown. */
function whatItDoes(m, codeLine) {
  const lines = [byLine(m), codeLine];
  if (m.gameplay) lines.push(t('It changes gameplay: everyone in a Link Play session needs it too.'));
  if (m.network) lines.push(typeof m.network === 'string' ? t('It goes online ({hosts}).', { hosts: m.network }) : t('It goes online.'));
  return lines;
}

/** Install (or update) from the hub: { title, lines, ok }. */
export function installQuestion(card) {
  const update = !!card.installed;
  return {
    title: update ? t('Update {name} to {version}?', { name: card.name, version: card.version }) : t('Install {name} {version}?', { name: card.name, version: card.version }),
    lines: whatItDoes(card, t('A mod with code runs that code on this PC with your rights, like any program you install.')),
    ok: update ? t('Update') : t('Install'),
  };
}

/** Switching on a mod whose code the player has not allowed (put into the mods folder by hand, or changed since): { title, lines, ok }. */
export function allowQuestion(mod) {
  return {
    title: t('Allow {name} {version} to run?', { name: mod.name || mod.id, version: mod.version }),
    lines: [...whatItDoes(mod, t('Its code is new on this PC, or changed since you last allowed it. It runs with your rights, like any program you install.')),
      t('It did not come through the launcher: only allow it if you trust where you got it.')],
    ok: t('Allow and switch on'),
  };
}

/** The value a setting's control sends to mods_set_setting: { number } for on/off, numbers and choices (by index), { text } for the rest. */
export function settingValue(s, control) {
  if (s.kind === 'bool') return { number: control.checked ? 1 : 0 };
  if (s.kind === 'number' || s.kind === 'choice') return { number: Number(control.value) };
  return { text: String(control.value) };
}

/** A number setting draws as a slider when it has a range, else as a field. */
export const isSlider = (s) => s.kind === 'number' && s.max > s.min;

/** Whether the mod at index i of the list can go up (-1) or down (+1). */
export const canMove = (list, i, delta) => i + delta >= 0 && i + delta < list.length;

// --- the locks (mods_lock.rs): the launcher keeps Community Achievements installed and on, and its sharing on, until the player unlocks

/** The lock button a mod or a setting row offers: 'unlock', 'lock' or '' (nothing the launcher locks). */
export const lockAction = (x) => (!x?.lockable ? '' : x.locked ? 'unlock' : 'lock');

/** The question before an unlock: { title, lines, ok }. `what`: 'mod' (switch off or remove it) or 'sharing' (switch sharing off). */
export function unlockQuestion(what) {
  if (what === 'sharing') {
    return {
      title: t('Unlock sharing?'),
      lines: [t('Switching sharing off stops your achievements from being counted in the community stats. They are still recorded on this PC.'),
        t('Lock it again at any time to switch sharing back on.')],
      ok: t('Unlock'),
    };
  }
  return {
    title: t('Unlock Community Achievements?'),
    lines: [t('Turning the Community Achievements mod off or removing it means your achievements and stats are no longer recorded.'),
      t('Lock it again at any time: the launcher switches it back on, and installs it again if it was removed.')],
    ok: t('Unlock'),
  };
}

/** The one line after the launcher kept the locks at its start (mods_keep_locked: ['install' | 'switch_on' | 'share_on']); '' for none. */
export function keptLine(done) {
  if (!done?.length) return '';
  if (done.includes('install')) return t('Community Achievements was installed and switched on. It is locked: unlock it on the Mods page to switch it off.');
  if (done.includes('switch_on')) return t('Community Achievements was switched on again. It is locked: unlock it on the Mods page to switch it off.');
  return t('Sharing of Community Achievements was switched on again. It is locked: unlock it on the Mods page to switch it off.');
}