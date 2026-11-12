// Tests for the pure part of the Mods page (src/mods-model.js). Run: node launcher/scripts/test-mods-model.mjs
import { allowQuestion, badges, byLine, canMove, cardAction, detailsView, filterByGroup, groupChips, installQuestion, isSlider, isTested, keepGroup, keptLine, lockAction, settingValue, sizeText, unlockQuestion, updatedLine } from '../src/mods-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };

const mod = { id: 'm', name: 'Mod', version: '1.0.0', authors: 'A, B', license: 'MIT', gameplay: true, network: 'x.example', needs_yes: true, state: 'off' };
const texts = (m) => badges(m).map((b) => b.text);
check(texts(mod).join() === 'Not allowed yet,Changes gameplay,Goes online', 'badges of a gameplay mod with new code that goes online');
check(texts({ ...mod, gameplay: false, network: '', needs_yes: false }).join() === 'Looks only', 'a plain look-only mod');
check(texts({ ...mod, state: 'error' })[0] === 'Problem', 'a broken mod says so first');
check(texts({ ...mod, needs_yes: false, locked: true })[0] === 'Locked', 'a locked mod says so');

check(byLine(mod) === 'By A, B. Licence: MIT.', 'by line from the core');
check(byLine({ authors: ['A', 'B'], license: '' }) === 'By A, B. Licence: not stated.', 'by line from the catalog, licence missing');
check(byLine({ authors: [], license: 'MIT' }) === 'Licence: MIT.', 'no authors');

const card = { id: 'c', name: 'Card', version: '1.1.0', authors: ['A'], license: 'MIT', gameplay: false, network: false, available: true, installed: '', newer: false };
check(cardAction(card, false) === 'install', 'install');
check(cardAction(card, true) === 'closed', 'a preview list installs nothing');
check(cardAction({ ...card, available: false }, false) === 'soon', 'no download: coming soon');
check(cardAction({ ...card, installed: '1.1.0' }, false) === 'installed', 'installed');
check(cardAction({ ...card, installed: '1.0.0', newer: true }, false) === 'update', 'a newer version');
check(cardAction({ ...card, installed: '1.0.0', newer: true }, true) === 'closed', 'no update from a preview list');

const iq = installQuestion({ ...card, gameplay: true, network: true });
check(iq.title === 'Install Card 1.1.0?' && iq.ok === 'Install', 'install title');
check(iq.lines[0] === 'By A. Licence: MIT.' && /rights/.test(iq.lines[1]) && /Link Play/.test(iq.lines[2]) && iq.lines[3] === 'It goes online.', 'install lines: who, code, gameplay, online');
check(installQuestion({ ...card, installed: '1.0.0' }).ok === 'Update', 'update question');
const aq = allowQuestion(mod);
check(aq.title === 'Allow Mod 1.0.0 to run?' && aq.lines.some((l) => l === 'It goes online (x.example).'), 'allow question names the hosts');

check(settingValue({ kind: 'bool' }, { checked: true }).number === 1 && settingValue({ kind: 'bool' }, { checked: false }).number === 0, 'bool');
check(settingValue({ kind: 'choice' }, { value: '2' }).number === 2, 'choice by index');
check(settingValue({ kind: 'number' }, { value: '1.25' }).number === 1.25, 'number');
check(settingValue({ kind: 'key' }, { value: 'F2' }).text === 'F2', 'key as text');
check(isSlider({ kind: 'number', min: 0.5, max: 1.6 }) && !isSlider({ kind: 'number', min: 0, max: 0 }), 'slider only with a range');
check(!canMove([1, 2], 0, -1) && canMove([1, 2], 0, 1) && !canMove([1, 2], 1, 1), 'move at the ends');

check(updatedLine([{ name: 'A', from: '1.0.0', to: '1.1.0' }, { name: 'B', from: '0.1.0', to: '0.2.0' }]) === 'Updated by itself: A 1.0.0 → 1.1.0, B 0.1.0 → 0.2.0', 'the update line names each mod and its versions');

// Browse: groups, the Tested tag, Details
const cards = [{ id: 'a', group: 'camera' }, { id: 'b', group: 'qol' }, { id: 'c', group: '' }, { id: 'd', group: 'camera' }];
check(filterByGroup(cards, '') === cards, 'no group chosen: every card');
check(filterByGroup(cards, 'camera').map((c) => c.id).join() === 'a,d', 'one group');
check(filterByGroup(cards, 'nowhere').length === 0, 'a group without cards');
const groups = [{ key: 'camera', label: 'Camera' }, { key: 'qol', label: 'Quality of life' }];
check(groupChips(groups).map((g) => g.label).join() === 'All,Camera,Quality of life' && groupChips(groups)[0].key === '', 'chips: All first, then the hub order');
check(groupChips([]).length === 0 && groupChips(undefined).length === 0, 'no groups, no chips');
check(keepGroup(groups, 'qol') === 'qol' && keepGroup(groups, 'gone') === '' && keepGroup([], 'qol') === '', 'a group that is gone falls back to All');
check(isTested({ tested: ['2.0.1', '1.0.5'] }, '2.0.1') && !isTested({ tested: ['1.0.5'] }, '2.0.1') && !isTested({ tested: [] }, '2.0.1') && !isTested({}, '2.0.1') && !isTested({ tested: ['2.0.1'] }, ''), 'tested only when the list holds the game version');
check(sizeText(512) === '512 B' && sizeText(77330) === '76 KB' && sizeText(3 * 1048576) === '3.0 MB', 'sizes');

const rich = { name: 'Card', version: '1.1.0', authors: ['A'], license: 'MIT', summary: 'Short.', description: 'Short. And more.', settings: ['Key', 'Speed'], tested: ['2.0.1'], size: 77330, gameplay: true, network: true, page: 'https://pokopia-native.vercel.app/mods/card', source: 'https://github.com/x/y' };
const d = detailsView(rich, '2.0.1');
check(d.title === 'Card 1.1.0' && d.by === 'By A. Licence: MIT.' && d.description === 'Short. And more.' && d.settings.join() === 'Key,Speed', 'details: name, who, whole text, settings');
check(d.facts[0] === 'Tested on game version 2.0.1.' && d.facts[1] === 'Download size: 76 KB' && d.facts.length === 4, 'details: tested, size, gameplay, online');
check(d.links.map((l) => l.label).join() === 'Mod page,Source', 'details: links');
const plain = detailsView({ name: 'P', version: '1.0.0', authors: [], license: '', summary: 'Only the summary.', tested: [], size: 0, gameplay: false, network: false, page: '', source: '' }, '2.0.1');
check(plain.description === 'Only the summary.' && plain.settings.length === 0 && plain.links.length === 0, 'details: no description falls back to the summary');
check(plain.facts.join() === 'The hub has not marked it as tested on game version 2.0.1.', 'details: not tested says so, no size, no extras');

// locks (mods_lock.rs marks the list)
check(lockAction({ lockable: true, locked: true }) === 'unlock' && lockAction({ lockable: true, locked: false }) === 'lock', 'a lockable row offers unlock or lock');
check(lockAction({ lockable: false, locked: false }) === '' && lockAction(null) === '', 'any other row offers nothing');
const um = unlockQuestion('mod');
check(um.ok === 'Unlock' && /no longer recorded/.test(um.lines[0]) && /installs it again/.test(um.lines[1]), 'unlocking the mod warns that nothing is recorded');
const us = unlockQuestion('sharing');
check(us.title === 'Unlock sharing?' && /community stats/.test(us.lines[0]) && /still recorded on this PC/.test(us.lines[0]), 'unlocking sharing warns about the stats');
check(keptLine([]) === '' && keptLine(undefined) === '', 'nothing kept: no line');
check(/installed and switched on/.test(keptLine(['install'])), 'install line');
check(/switched on again/.test(keptLine(['switch_on', 'share_on'])) && /^Sharing/.test(keptLine(['share_on'])), 'switch and sharing lines');

console.log(bad ? `TEST FAILED (${bad})` : 'mods model tests passed');
process.exit(bad ? 1 : 0);
