// Tests for the pure part of the Achievements page (src/achievements-model.js). Run: node launcher/scripts/test-achievements-model.mjs
import { dateText, earnedCount, groupList, lockLines, modActions, modLine, modState, percentText, rarest, totals } from '../src/achievements-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };

const item = (id, cat, players) => ({ id, name: id, desc: '', cat, goal: 1, picture: '', players });
const list = [item('dex_5', 'pokedex', 90), item('dex_25', 'pokedex', 10), item('habitat_5', 'habitats', 10), item('town_2', 'town', 0), item('dish_1', 'dishes', 1)];

const groups = groupList(list);
check(groups.map((g) => g.cat).join() === 'pokedex,habitats,town,dishes', 'groups in the hub\'s order');
check(groups[0].items.length === 2 && groups[0].title === 'Pokédex' && groups[2].title === 'Towns', 'items sit under their group, with our titles');
check(groupList([item('x', 'newgroup', 1)])[0].title === 'Newgroup' && groupList([item('x', '', 1)])[0].title === 'Other', 'an unknown group still has a title');

check(earnedCount(list, { dex_5: 1, dish_1: 2, other_id: 3 }) === 2, 'earned counts only the list\'s ids');
check(earnedCount(list, {}) === 0, 'nothing earned');

check(percentText(51, 100) === '51%' && percentText(1, 100) === '1%', 'percent');
check(percentText(1, 1000) === 'under 1%', 'under one percent');
check(percentText(0, 100) === '0%' && percentText(5, 0) === '' && percentText(null, 10) === '', 'zero, no players, no counts');
check(percentText(200, 100) === '100%', 'never over 100');

check(totals({ stats: null, list }) === null, 'no counts, no totals');
check(JSON.stringify(totals({ stats: { players: 120 }, list })) === '{"players":120,"earned":111}', 'players and the sum of what was earned');

check(rarest(list).map((a) => a.id).join() === 'dish_1,dex_25,habitat_5', 'rarest: fewest first, ties in list order, nobody-has-it left out');
check(rarest(list, 1).length === 1 && rarest([item('a', 'x', undefined)]).length === 0, 'rarest: n and unknown counts');

check(dateText(0, 'en-US').length > 0 && /2024/.test(dateText(1718000000, 'en-US')), 'a date from the unix time');

const mod = (over = {}, share = 1) => ({ id: 'community_achievements', name: 'Community Achievements', enabled: true, state: 'ok', settings: [{ key: 'messages', number: 1 }, { key: 'share', number: share }], ...over });
const kind = (v) => modState(v, 'community_achievements').kind;
check(kind(null) === 'unknown', 'mods list not read');
check(kind({ mods: [{ id: 'other', enabled: true, settings: [] }] }) === 'missing', 'not installed');
check(kind({ mods: [mod({ enabled: false, state: 'off' })] }) === 'off', 'installed, off');
check(kind({ mods: [mod({ state: 'error' })] }) === 'broken', 'broken');
check(kind({ mods: [mod()] }) === 'sharing', 'on, sharing');
check(kind({ mods: [mod({}, 0)] }) === 'private', 'on, sharing off');
check(kind({ mods: [mod({ settings: [] })] }) === 'sharing', 'on without a share setting counts as on');
check(modState({ mods: [mod()] }, 'community_achievements').mod.id === 'community_achievements', 'the mod comes with its state');

const acts = (k) => modActions({ kind: k }).join();
check(acts('missing') === 'install,folder' && acts('off') === 'enable' && acts('private') === 'share', 'each state offers its action');
check(acts('sharing') === '' && acts('unknown') === '' && acts('broken') === 'folder', 'nothing to do when it is working');
check(['missing', 'broken', 'off', 'private', 'sharing', 'unknown'].every((k) => modLine({ kind: k }).length > 20), 'every state has a sentence');

// locks (mods_lock.rs marks the list: the mod and its share setting, and the view's { kept, sharing })
const lockedMod = (locked, shareLocked, enabled = true) => ({ id: 'community_achievements', state: 'ok', enabled, locked, lockable: true,
  settings: [{ key: 'messages', number: 1, locked: false, lockable: false }, { key: 'share', number: 1, locked: shareLocked, lockable: true }] });
const st = (m, locks) => modState({ mods: m ? [m] : [], locks }, 'community_achievements');
check(modActions(st(lockedMod(true, true), { kept: true, sharing: true })).join() === 'unlock,unlock-sharing', 'locked: both unlocks');
check(modActions(st(lockedMod(false, false, false), { kept: false, sharing: false })).join() === 'enable,lock,lock-sharing', 'unlocked and off: switch on, or lock both');
check(modActions(st(null, { kept: false, sharing: true })).join() === 'install,folder,lock', 'unlocked and removed: Lock installs it again');
check(modActions(st(null, { kept: true, sharing: true })).join() === 'install,folder', 'locked and missing: the launcher installs it');
check(st(null, null).locks === null && modActions(st(null, undefined)).join() === 'install,folder', 'an unmarked list offers no lock');
check(lockLines(st(lockedMod(true, true), {})).length === 2 && lockLines(st(lockedMod(false, false), {})).length === 0, 'lock lines follow the locks');

console.log(bad ? `TEST FAILED (${bad})` : 'achievements model tests passed');
process.exit(bad ? 1 : 0);
