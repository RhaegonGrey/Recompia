// Tests for the pure part of the launcher's controller navigation (src/gamepad-nav-model.js). Run: node launcher/scripts/test-gamepad-nav.mjs
import { readPad, newlyDown, repeatStep, pickNext, cycle, keyDir, arrowRoute, BTN, FIRST_REPEAT_MS, REPEAT_MS } from '../src/gamepad-nav-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };
const pad = (buttons = {}, axes = [0, 0, 0, 0]) => ({ buttons: Array.from({ length: 17 }, (_, i) => ({ pressed: !!buttons[i], value: buttons[i] ? 1 : 0 })), axes });

// readPad
check(readPad(null).dir === null, 'no pad: no direction');
check(readPad(pad({ [BTN.UP]: 1 })).dir === 'up', 'd-pad up');
check(readPad(pad({}, [0.9, 0.1, 0, 0])).dir === 'right', 'stick right');
check(readPad(pad({}, [0.3, 0.3, 0, 0])).dir === null, 'a small stick push is no direction');
check(readPad(pad({}, [-0.2, -0.8, 0, 0])).dir === 'up', 'stick mostly up');
check(readPad(pad({}, [0, 0, 0, 0.8])).scroll === 0.8 && readPad(pad({}, [0, 0, 0, 0.1])).scroll === 0, 'right stick scrolls past its dead zone only');
check(readPad(pad({ [BTN.A]: 1 })).down.has(BTN.A), 'A is down');

// newlyDown: only a fresh press counts
const held = readPad(pad({ [BTN.A]: 1 }));
check(newlyDown(held, held).length === 0, 'a held button is not new');
check(newlyDown(readPad(pad()), held).join() === String(BTN.A), 'a press is new');
check(newlyDown(null, held).length === 1, 'with no earlier state every held button is new');

// repeat: at once, then after the first delay, then at the repeat rate
const st = { dir: null, since: 0, last: 0 };
check(repeatStep(st, 'down', 1000) === true, 'first step at once');
check(repeatStep(st, 'down', 1000 + FIRST_REPEAT_MS - 10) === false, 'no repeat before the first delay');
check(repeatStep(st, 'down', 1000 + FIRST_REPEAT_MS) === true, 'repeat after the first delay');
check(repeatStep(st, 'down', 1000 + FIRST_REPEAT_MS + REPEAT_MS - 5) === false, 'no repeat inside the repeat rate');
check(repeatStep(st, 'down', 1000 + FIRST_REPEAT_MS + REPEAT_MS) === true, 'repeat at the rate');
check(repeatStep(st, null, 5000) === false && repeatStep(st, 'left', 5001) === true, 'a new direction steps at once');

// pickNext on a grid:   a b c
//                       d e f
const box = (x, y) => ({ left: x, top: y, right: x + 100, bottom: y + 40 });
const R = [box(0, 0), box(120, 0), box(240, 0), box(0, 60), box(120, 60), box(240, 60)];   // a b c d e f
check(pickNext(R[0], R, 'right') === 1, 'a right -> b');
check(pickNext(R[1], R, 'down') === 4, 'b down -> e');
check(pickNext(R[4], R, 'up') === 1, 'e up -> b');
check(pickNext(R[4], R, 'left') === 3, 'e left -> d');
check(pickNext(R[0], R, 'left') === -1, 'nothing left of a');
check(pickNext(R[0], R, 'up') === -1, 'nothing above a');
check(pickNext(R[2], R, 'down') === 5, 'c down -> f, not another column');
// a sideways miss counts double: from a, down goes to d (same column), not e
check(pickNext(R[0], R, 'down') === 3, 'a down -> d');
// a tall control beside two short ones: the nearer row wins
const side = [box(0, 0), { left: 200, top: -100, right: 300, bottom: 150 }, box(200, 200)];
check(pickNext(side[0], side, 'right') === 1, 'right from a short control reaches the tall one that overlaps its row');

// Browse's group chips wrap onto a second line; the first card's buttons sit below: a chip row is walked along, then down into the cards
const chip = (x, y, w = 60) => ({ left: x, top: y, right: x + w, bottom: y + 28 });
const C = [chip(0, 0), chip(70, 0, 110), chip(190, 0), chip(0, 40, 90), chip(100, 40)];   // All, Camera and photos, Looks / Quality of life, Gameplay
const below = [{ left: 0, top: 100, right: 80, bottom: 130 }, { left: 90, top: 100, right: 190, bottom: 130 }];   // a card's Details and Mod page
check(pickNext(C[0], C, 'right') === 1 && pickNext(C[1], C, 'right') === 2, 'chips: right walks the row');
check(pickNext(C[1], C, 'down') === 3 || pickNext(C[1], C, 'down') === 4, 'chips: down from the first line reaches the second');
check(pickNext(C[3], [...C, ...below], 'down') === 5, 'chips: down from the last line reaches the card');
check(pickNext(C[4], [...C, ...below], 'up') === 1 || pickNext(C[4], [...C, ...below], 'up') === 0, 'chips: up goes back to the first line');

// cycle
check(cycle(0, 4, 1) === 1 && cycle(3, 4, 1) === 0 && cycle(0, 4, -1) === 3, 'cycle wraps');
check(cycle(-1, 4, 1) === 0 && cycle(-1, 4, -1) === 3, 'cycle from nothing');
check(cycle(0, 0, 1) === -1, 'cycle of an empty list');

// arrow keys
check(keyDir({ key: 'ArrowUp' }) === 'up' && keyDir({ key: 'ArrowRight' }) === 'right', 'arrow keys give directions');
check(keyDir({ key: 'Enter' }) === null && keyDir({ key: 'a' }) === null, 'other keys give none');
check(keyDir({ key: 'ArrowLeft', shiftKey: true }) === null && keyDir({ key: 'ArrowLeft', ctrlKey: true }) === null && keyDir({ key: 'ArrowLeft', altKey: true }) === null, 'an arrow with a modifier is not navigation');
check(['up', 'down', 'left', 'right'].every((d) => arrowRoute('text', d) === 'native'), 'a text field keeps all four arrows');
check(arrowRoute('select', 'up') === 'native' && arrowRoute('select', 'down') === 'native' && arrowRoute('select', 'left') === 'nav', 'a drop-down keeps up and down');
check(arrowRoute('range', 'left') === 'native' && arrowRoute('range', 'right') === 'native' && arrowRoute('range', 'down') === 'nav', 'a slider keeps left and right');
check(arrowRoute('radio', 'right') === 'native' && arrowRoute('radio', 'up') === 'nav', 'a radio group keeps left and right');
check(arrowRoute('other', 'down') === 'nav' && arrowRoute('other', 'left') === 'nav', 'a button moves the focus');
check(arrowRoute('none', 'down', true) === 'native' && arrowRoute('none', 'down', false) === 'nav', 'nothing focused: down scrolls only a page that can scroll');
check(arrowRoute('none', 'left', true) === 'nav' && arrowRoute('none', 'right', false) === 'nav', 'nothing focused: left and right take the first control');

console.log(bad ? `TEST FAILED (${bad})` : 'gamepad nav model tests passed');
process.exit(bad ? 1 : 0);
