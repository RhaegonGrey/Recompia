// Controller navigation for the launcher (Steam Deck, ROG Ally and other handhelds, or any pad): the d-pad or left stick moves between
// controls, A presses the focused one, B goes back (closes a question, or returns Home), LB and RB switch pages, the right stick scrolls.
// A slider moves with left and right, a drop-down with up and down. It works beside touch and the mouse: the focus ring only shows while a
// pad is being used. The logic that needs no page is in gamepad-nav-model.js.
import { BTN, readPad, newlyDown, repeatStep, pickNext, cycle, keyDir, arrowRoute } from './gamepad-nav-model.js';

const FOCUSABLE = 'button, a[href], input, select, textarea, [tabindex]';
let started = false;

function usable(el, scope) {
  if (!scope.contains(el) || el.disabled || el.getAttribute('tabindex') === '-1' || el.type === 'hidden') return false;
  if (el.closest('[hidden], [inert]') || el.closest('#winbtns')) return false;   // the window buttons are for the mouse and touch
  const s = getComputedStyle(el);
  if (s.visibility === 'hidden' || s.display === 'none') return false;
  const r = el.getBoundingClientRect();
  return r.width > 0 && r.height > 0;
}

function candidates() {
  const scope = document.querySelector('dialog[open]') || document.body;   // a question owns the pad while it is open
  return { scope, list: [...scope.querySelectorAll(FOCUSABLE)].filter((el) => usable(el, scope)) };
}

function rectOf(el) {
  const r = el.getBoundingClientRect();
  return { left: r.left, top: r.top, right: r.right, bottom: r.bottom };
}

function focusEl(el) {
  el.focus({ preventScroll: true });
  el.scrollIntoView({ block: 'nearest', inline: 'nearest' });
}

function move(dir) {
  const { list } = candidates();
  if (!list.length) return;
  const cur = document.activeElement;
  if (!cur || !list.includes(cur)) {   // nothing focused yet: the page's main action, else the first control
    focusEl(document.querySelector('dialog[open] .btn.primary, #view .btn.primary') || list[0]);
    return;
  }
  // a slider uses left and right, a drop-down up and down, for their value
  if (cur.matches('input[type="range"]') && (dir === 'left' || dir === 'right')) return nudgeRange(cur, dir === 'right' ? 1 : -1);
  if (cur.matches('select') && (dir === 'up' || dir === 'down')) return nudgeSelect(cur, dir === 'down' ? 1 : -1);
  const i = pickNext(rectOf(cur), list.map(rectOf), dir);
  if (i >= 0) focusEl(list[i]);
}

function fire(el, type) {
  el.dispatchEvent(new Event(type, { bubbles: true }));
}

function nudgeRange(el, step) {
  const lo = Number(el.min || 0), hi = Number(el.max || 100);
  const unit = Number(el.step) > 0 ? Number(el.step) : Math.max(1, (hi - lo) / 20);
  el.value = String(Math.min(hi, Math.max(lo, Number(el.value) + step * unit)));
  fire(el, 'input');
  fire(el, 'change');
}

function nudgeSelect(el, step) {
  const n = Math.min(el.options.length - 1, Math.max(0, el.selectedIndex + step));
  if (n === el.selectedIndex) return;
  el.selectedIndex = n;
  fire(el, 'change');
}

function press() {
  const el = document.activeElement;
  if (!el || el === document.body) return move(null);
  if (el.matches('select')) { if (el.showPicker) { try { el.showPicker(); return; } catch { /* not allowed now: fall through */ } } }
  el.click();
}

function back() {
  const dlg = document.querySelector('dialog[open]');
  if (dlg) {
    const cancel = dlg.querySelector('[data-answer=""], [value="cancel"], .btn.quiet');
    if (cancel) cancel.click();
    else dlg.close('');
    return;
  }
  const home = document.querySelector('#nav [data-page="home"]');
  if (home && !home.hasAttribute('aria-current')) home.click();   // from any other page, B goes Home
}

function switchPage(step) {
  if (document.querySelector('dialog[open]')) return;
  const items = [...document.querySelectorAll('#nav [data-page]')].filter((b) => !b.hidden);   // a page the player hid is skipped
  const at = items.findIndex((b) => b.hasAttribute('aria-current'));
  const to = cycle(at, items.length, step);
  if (to >= 0 && to !== at) items[to].click();
}

// What has the focus, for arrowRoute: a field with a caret, a drop-down, a slider, a radio, any other control, or nothing.
function focusKind() {
  const el = document.activeElement;
  if (!el || el === document.body || el.id === 'view') return 'none';
  if (el.isContentEditable || el.matches('textarea, input:not([type]), input[type="text"], input[type="search"], input[type="number"], input[type="password"], input[type="email"], input[type="url"]')) return 'text';
  if (el.matches('select')) return 'select';
  if (el.matches('input[type="range"]')) return 'range';
  if (el.matches('input[type="radio"]')) return 'radio';
  return 'other';
}

export function startGamepadNav() {
  if (started) return;
  started = true;
  let prev = null;
  const rep = { dir: null, since: 0, last: 0 };
  let active = false;   // a pad or the arrow keys have been used since the mouse or a finger last was

  const padOn = () => { if (!active) { active = true; document.body.classList.add('pad-nav'); } };
  const padOff = () => { if (active) { active = false; document.body.classList.remove('pad-nav'); } };
  addEventListener('pointerdown', padOff, true);
  addEventListener('mousemove', (e) => { if (e.movementX || e.movementY) padOff(); }, true);

  // The arrow keys move the focus the same way (and Escape goes back), for a handheld whose d-pad arrives as keys. Enter and Space already press.
  addEventListener('keydown', (e) => {
    if (e.defaultPrevented) return;
    const dir = keyDir(e);
    if (dir) {
      const view = document.getElementById('view');
      const canScroll = !!view && view.scrollHeight > view.clientHeight + 1;
      if (arrowRoute(focusKind(), dir, canScroll) !== 'nav') return;
      e.preventDefault();
      padOn();
      move(dir);
    } else if (e.key === 'Escape' && !document.querySelector('dialog[open]') && focusKind() !== 'text') {
      back();   // a question closes on Escape by itself; elsewhere it is B
    }
  });

  if (!navigator.getGamepads) return;

  const tick = (now) => {
    requestAnimationFrame(tick);
    if (!document.hasFocus()) { prev = null; return; }
    const pad = [...navigator.getGamepads()].find((p) => p && p.connected);
    if (!pad) { prev = null; return; }
    const cur = readPad(pad);
    const fresh = prev ? newlyDown(prev, cur) : [];   // the first frame only records what is held (a button held while the launcher opened)
    prev = cur;
    const used = cur.dir || cur.scroll || fresh.length;
    if (used) padOn();
    if (!active) return;
    if (repeatStep(rep, cur.dir, now)) move(cur.dir);
    for (const b of fresh) {
      if (b === BTN.A) press();
      else if (b === BTN.B) back();
      else if (b === BTN.LB) switchPage(-1);
      else if (b === BTN.RB) switchPage(1);
    }
    if (cur.scroll) (document.querySelector('dialog[open]') || document.getElementById('view'))?.scrollBy({ top: cur.scroll * 22 });   // a long question scrolls itself
  };
  requestAnimationFrame(tick);
}
