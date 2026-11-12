// The one place to pick game files and run setup: one glass panel on the art that changes as you go. viewOf() names what
// the panel shows; a view's markup is only rewritten when it changes, and setup ticks touch a few nodes (live()), so a focused
// control, an open details block or a half typed path stays as it is.
import './setup.css';
import logoUrl from '../art/logo.png';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { fromBackend } from '../backend-text.js';
import { errCode, errText, esc, field, icon, row, toast, toggle } from '../ui.js';
import { store } from '../store.js';
import { needsFinish, primaryAction } from '../action.js';

const GB = 2 ** 30;   // preflight.rs GB
const SLOW_CHECK_MS = 200;   // a faster check never shows the "Checking" panel
const NUP_CANCELLED = 'nup_cancelled';   // error.rs: the player stopped it, not a failure
const SETUP_BUSY = t('Setup is running. Pause it first.');

const STEPS = [t('Build game'), t('Translate shaders'), t('First run')];

// phase -> [step, the step's percent at the phase's start, at its end]. The two phases that take the time get the room.
// Phases: main.rs check, tools, warmup; pkp_build.py verify, lift, compile, link, finish; shaders.rs extract, recheck, translate.
const PHASES = {
  check: [0, 0, 0], tools: [0, 0, 5], verify: [0, 5, 10], lift: [0, 10, 25], compile: [0, 25, 95], link: [0, 95, 100], finish: [0, 100, 100],
  extract: [1, 0, 5], recheck: [1, 5, 10], translate: [1, 5, 100],
  warmup: [2, 0, 0],
};

/** Where setup is: step index and percent of that step, null when the phase has no count. */
function stepOf(p) {
  const [step, from, to] = PHASES[p.phase] ?? [0, 0, 0];
  return { step, pct: p.total ? Math.min(100, Math.floor(from + ((to - from) * p.done) / p.total)) : null };
}

// Time left. Only for the phases that fill their step, so "left in this step" is true for them; the rate is that of the last
// few minutes because shader batches pause to save.
const ETA_PHASES = new Set(['compile', 'translate']);
const ETA_WINDOW_MS = 180_000;
const ETA_MIN_SPAN_MS = 60_000;   // shorter: not meaningful yet
const ETA_MIN_SAMPLES = 3;
const ETA_REDRAW_MS = 15_000;     // the number must not flicker
const eta = { phase: '', samples: [], text: '', shown: 0 };

function etaReset(phase = '') {
  Object.assign(eta, { phase, samples: [], text: '', shown: 0 });
}

function roughly(ms) {
  const min = ms / 60_000;
  if (min < 1) return t('Less than a minute');
  if (min < 10) return t('About {n} min', { n: Math.ceil(min) });
  if (min < 55) return t('About {n} min', { n: Math.round(min / 5) * 5 });
  const total = Math.round(min / 10) * 10;
  const m = total % 60;
  return m ? t('About {h} h {m} min', { h: Math.floor(total / 60), m }) : t('About {h} h', { h: Math.floor(total / 60) });
}

function etaOf(p, now = Date.now()) {
  if (!ETA_PHASES.has(p.phase) || !p.total) {
    etaReset();
    return '';
  }
  const last = eta.samples.at(-1);
  if (p.phase !== eta.phase || (last && p.done < last.done)) etaReset(p.phase);
  if (eta.samples.at(-1)?.done !== p.done) eta.samples.push({ t: now, done: p.done });   // other store events draw too
  while (eta.samples.length > ETA_MIN_SAMPLES && now - eta.samples[0].t > ETA_WINDOW_MS) eta.samples.shift();
  if (eta.text && now - eta.shown < ETA_REDRAW_MS) return eta.text;
  const first = eta.samples[0];
  const span = now - first.t;
  const moved = p.done - first.done;
  if (span < ETA_MIN_SPAN_MS || eta.samples.length < ETA_MIN_SAMPLES || moved <= 0) return eta.text;
  eta.text = t('{time} left in this step', { time: roughly(((p.total - p.done) * span) / moved) });
  eta.shown = now;
  return eta.text;
}

// What outlives one render: unpacking and checking go on while the player is on another page, and the page shows the
// result when they come back. paint is the shown page's redraw, null while it is not shown.
const flow = {
  report: null,      // validate_dump of the last folder looked at
  error: null,       // why the last choice failed outside report.errors
  checking: null,    // the path of a slow check, or true while a .nup is on its way
  checkId: 0,        // the newest check; older answers are dropped
  loaded: false,     // the saved folder has been looked at
  saved: '',         // config dump_dir
  gentle: false,
  small: true,       // a new setup keeps its shaders packed (config loose_shaders: false)
  touched: false,    // the toggle was used: it beats preflight's suggestion
  changing: false,   // "Change": the choose panel over files that are fine
  again: false,      // "Set up again": the set up panel over a finished setup
  typing: false,     // the path field is open
  pausing: false,
  picking: false,    // a file dialog is open
  need: undefined,   // preflight need_bytes: undefined not asked, null asked or unknown
};
let paint = null;
let savingConfig = Promise.resolve();

const setupRunning = (s) => s.setup?.progress?.state === 'running';

const nameOf = (kind, version) => (kind === 'base' ? t('Base game') : kind === 'update' ? t('Update {version}', { version }) : kind === 'nup' ? t('.nup package') : t('Game files'));

/** Validates, and saves at once if the files are usable. */
async function check(path) {
  const id = ++flow.checkId;
  flow.error = null;
  flow.need = undefined;
  const slow = setTimeout(() => {
    if (id !== flow.checkId) return;
    flow.checking = path;
    paint?.();
  }, SLOW_CHECK_MS);
  try {
    const report = await api.validateDump({ path });
    if (id !== flow.checkId) return;
    flow.report = report;
    if (report.chosen) {
      if (setupRunning(store.state)) throw SETUP_BUSY;   // started while the check ran
      await api.saveDump({ path: report.path });
      flow.saved = report.path;
      await store.refresh();
    }
  } catch (e) {
    if (id === flow.checkId) flow.error = errText(e);
  } finally {
    clearTimeout(slow);
    if (id === flow.checkId) {
      Object.assign(flow, { checking: null, changing: false, typing: false, loaded: true });
      paint?.();
    }
  }
}

async function unpack(path) {
  flow.error = null;
  flow.checking = true;   // holds the panel between the last unpack tick and the check
  let dir = null;
  try {
    dir = await store.importNup(path);
  } catch (e) {
    flow.checking = null;
    if (errCode(e) !== NUP_CANCELLED) {
      flow.error = errText(e);
      flow.report = null;
      flow.changing = false;
    }
    paint?.();
  }
  if (dir) await check(dir);
}

/** Unpacks the add-on (DLC) of a decrypted .nsp next to the game; the game lists it at its next start. */
async function addDlc(path) {
  try {
    const ids = await api.importDlc({ path });
    toast(t('DLC added ({ids}). It shows in the game at its next start.', { ids: ids.join(', ') }));
  } catch (e) {
    toast(errText(e), 'error');
  }
}

/** A folder, a .nup or the DLC .nsp, from a dialog, the path field or a drop. */
function take(path) {
  const s = store.state;
  const p = path.trim();
  if (!p) return;
  if (setupRunning(s)) return toast(SETUP_BUSY, 'error');
  if (s.update) return toast(t('The launcher is updating.'), 'error');
  if (s.nup) return toast(t('Game files are being unpacked.'), 'error');
  if (/\.nsp$/i.test(p)) return addDlc(p);
  return /\.nup$/i.test(p) ? unpack(p) : check(p);
}

async function pick(command, args) {
  if (flow.picking) return;
  flow.picking = true;
  try {
    const path = await command(args);
    if (path) await take(path);
  } catch (e) {
    toast(errText(e), 'error');
  } finally {
    flow.picking = false;
  }
}

async function loadSaved() {
  flow.loaded = false;
  try {
    const cfg = await api.getConfig();
    flow.saved = cfg.dump_dir || '';
    flow.gentle = !!cfg.gentle_build;
    flow.small = !cfg.loose_shaders;
    paint?.();
    // a .nup is no folder: readiness read it, app.dump has what there is to say
    if (flow.saved && !/\.nup$/i.test(flow.saved) && !flow.checking && !store.state.nup) {
      const id = flow.checkId;
      const report = await api.validateDump({ path: flow.saved });
      if (id === flow.checkId) flow.report = report;
    }
  } catch (e) {
    console.warn(e);
  }
  flow.loaded = true;
  paint?.();
}

function saveGentle(on) {
  flow.gentle = on;
  flow.touched = true;
  savingConfig = savingConfig
    .then(async () => api.saveConfig({ config: { ...(await api.getConfig()), gentle_build: on } }))
    .catch((e) => toast(errText(e), 'error'));
}

function saveSmall(on) {
  flow.small = on;
  savingConfig = savingConfig
    .then(async () => api.saveConfig({ config: { ...(await api.getConfig()), loose_shaders: !on } }))
    .then(() => {
      flow.need = undefined;   // packed shaders need far less free space: ask preflight again
      wantNeed();
    })
    .catch((e) => toast(errText(e), 'error'));
}

function wantNeed() {
  if (flow.need !== undefined) return;
  flow.need = null;
  api.preflight().then((r) => {
    flow.need = r.need_bytes;
    paint?.();
  }).catch((e) => console.warn(e));
}

function viewOf(s) {
  if (!s.app) return s.loadError ? 'down' : 'loading';
  const p = s.setup?.progress;
  if (p?.state === 'running') return 'running';
  if (s.nup) return 'nup';
  if (flow.checking) return 'checking';
  if (flow.changing) return 'choose';
  if (flow.error || (flow.report && !flow.report.chosen)) return 'bad';
  const kind = s.app.readiness?.kind;
  if (kind === 'no_files') return 'choose';
  if (kind === 'bad_files') return flow.loaded ? 'bad' : 'checking';
  if (kind === 'no_exe' || kind === 'no_runtime') return 'noexe';
  if (p?.state === 'error') return 'error';
  if (kind === 'ready') return flow.again ? 'setup' : 'done';
  return p?.state === 'cancelled' ? 'paused' : 'setup';
}

/** The big button: primaryAction, except where this page has its own word for it. */
function actionOf(s, view) {
  const a = primaryAction(s);
  if (view === 'error' && !a.disabled) return { ...a, label: t('Try again'), icon: 'rotate-ccw', run: 'setup' };
  if (a.run === 'play') return { ...a, label: t('Set up again'), icon: 'wrench', run: 'setup' };
  return a;
}

function factOf(s, view) {
  const parts = [];
  // README: about 7 minutes of build and 1 to 2 hours of shaders. Gentle uses a quarter of the threads.
  if (view === 'setup' && s.app.readiness?.kind === 'setup') parts.push(flow.gentle ? t('Several hours.') : t('About 2 hours.'));
  if (flow.need > 0) parts.push(t('Needs {size} GB free.', { size: Math.ceil(flow.need / GB) }));
  return parts.join(' ');
}

const chooseButtons = () => `<div class="setup-choices">
    <button class="btn primary big" data-act="folder" data-focus>${icon('folder-open')}${esc(t('Choose a folder'))}</button>
    <button class="btn big" data-act="nup">${icon('file-archive')}${esc(t('Choose a .nup file'))}</button>
  </div>`;

const LAYOUT = `Pokopia/
  base/
    exefs/    main, sdk, subsdk0, rtld, main.npdm
    romfs/
  updates/<version>/
    exefs/
    romfs/
  aoc/<add-on id>/romfs/    optional`;

function candidateHtml(c, chosen) {
  const used = chosen?.path === c.path;
  // an incomplete folder that another one makes unnecessary (an empty base/ next to a full update) is not an error
  const unneeded = chosen && !used && !c.ok;
  const chip = used ? `<span class="chip ok">${icon('circle-check')}${esc(t('Will be used'))}</span>`
    : c.ok ? `<span class="chip">${esc(t('Valid, older'))}</span>`
    : unneeded ? `<span class="chip">${esc(t('Not needed'))}</span>`
    : `<span class="chip danger">${icon('circle-x')}${esc(t('Incomplete'))}</span>`;
  const checks = c.checks.map((k) => `<li class="${k.ok ? 'ok' : 'bad'}">${icon(k.ok ? 'circle-check' : 'circle-x')}${esc(fromBackend(k.label))}</li>`);
  return `<div class="setup-candidate">
    <div class="split"><h3>${esc(nameOf(c.kind, c.version))}</h3>${chip}</div>
    <div class="path">${esc(c.path)}</div>
    ${unneeded ? '' : `<ul class="setup-checks">${checks.join('')}</ul>`}
  </div>`;
}

function detailsHtml() {
  const r = flow.report;
  const aoc = r?.aoc ?? [];
  return `<details class="setup-details">
    <summary>${icon('chevron-right')}${esc(t('Show details'))}</summary>
    <div class="setup-details-body">
      ${(r?.candidates ?? []).map((c) => candidateHtml(c, r.chosen)).join('')}
      ${aoc.length ? `<p>${esc(t('Add-on content: {count} found', { count: aoc.length }))} (<span class="selectable">${aoc.map(esc).join(', ')}</span>)</p>` : ''}
      <pre class="path setup-tree">${esc(LAYOUT)}</pre>
      <p>${esc(t('The files must already be decrypted. The highest valid update is used, otherwise the base game.'))}</p>
      <p>${esc(t('A .nup is unpacked once into nup_dump next to the game and needs the free space of the full game. Add-ons (DLC) in it are unpacked to nup_dump/aoc and found on their own.'))}</p>
      <p>${esc(t('The launcher only reads this folder and never writes to it (a .nup is unpacked into its own copy).'))}</p>
      <p>${esc(t("DLC comes as a decrypted .nsp: press Add DLC (or drop the file here) and its add-on is unpacked into the folder pkp.ini's aoc_dir names, aoc next to the game when it names none."))}</p>
    </div>
  </details>`;
}

function filesHtml(s) {
  const c = flow.report?.chosen;
  const d = s.app.dump;
  const name = c ? nameOf(c.kind, c.version) : d ? nameOf(d.kind, d.version) : '';
  const aoc = flow.report?.aoc?.length ?? 0;
  const path = flow.report?.path || d?.path || flow.saved;
  return `<div class="setup-files">
    ${icon('circle-check')}
    <div class="grow">
      <div>${esc(name)}${aoc ? esc(`, ${aoc > 1 ? t('{count} add-ons', { count: aoc }) : t('1 add-on')}`) : ''}</div>
      <div class="path">${esc(path)}</div>
    </div>
    <button class="btn sm" data-act="dlc">${esc(t('Add DLC'))}</button>
    <button class="btn sm" data-act="change">${esc(t('Change'))}</button>
  </div>`;
}

const primaryHtml = () => `<button class="btn primary big setup-go" id="primary" data-act="primary" data-focus><span class="setup-go-icon"></span><span class="setup-go-label"></span></button>`;
const GENTLE_ROW = row(t('Slower, keeps the PC usable'), toggle('gentle', false));   // row() numbers its ids: built once, or every draw differs
// Only before the first setup: an existing cache keeps the storage it has (shaders.rs Job::compact).
const SMALL_ROW = row(t('Packed shader files (about 20 GB smaller)'), toggle('small', true));
const logButton = () => `<button class="btn quiet" data-act="log">${icon('file-text')}${esc(t('Open setup log'))}</button>`;

const stepsHtml = () => `<ol class="setup-steps">${STEPS.map((name, i) => `<li>
    <span class="setup-step-mark"><span class="num">${i + 1}</span>${icon('check')}</span>
    <span class="setup-step-name">${esc(name)}</span>
  </li>`).join('')}</ol>`;

const meterHtml = (label) => `<div class="setup-meter">
    <span class="setup-pct num" id="pct"></span>
    <span class="setup-eta" id="eta"></span>
  </div>
  <progress max="100" id="bar" aria-label="${esc(label)}"></progress>`;

const VIEWS = {
  loading: () => `<h1>${esc(t('Loading'))}</h1>`,

  down: (s) => `<h1>${esc(t('The launcher is not answering'))}</h1>
    <div class="banner danger">${icon('circle-x')}<div class="selectable">${esc(s.loadError)}</div></div>
    ${primaryHtml()}`,

  choose: (s) => `<h1>${esc(t('Choose your game files'))}</h1>
    ${chooseButtons()}
    ${flow.typing
      ? `<form class="setup-path" data-form="path">
          ${field('path', flow.saved, 'C:/path/to/Pokopia', { mono: true, label: t('Game files path') })}
          <button class="btn" type="submit">${esc(t('Use this path'))}</button>
        </form>`
      : `<button class="btn quiet sm setup-link" data-act="type">${esc(t('Enter a path'))}</button>`}
    ${flow.changing && (s.app.dump || flow.saved) ? `<button class="btn quiet sm setup-link" data-act="cancel">${esc(t('Cancel'))}</button>` : ''}
    ${detailsHtml()}
    <p class="faint setup-legal">${esc(t('You need your own copy of the game. None is included.'))}</p>`,

  checking: () => `<h1>${esc(t('Checking your game files'))}</h1>
    <div class="setup-files">${icon('loader-circle', 'spin')}<div class="grow path">${esc(typeof flow.checking === 'string' ? flow.checking : flow.saved)}</div></div>`,

  bad: (s) => {
    const errors = flow.error ? [flow.error] : flow.report?.errors?.length ? flow.report.errors.map(fromBackend) : [fromBackend(s.app.readiness?.detail) || t('These files cannot be used.')];
    return `<h1>${esc(t('These game files will not work'))}</h1>
      <div class="banner danger">${icon('circle-x')}<div class="selectable">${errors.map((m) => `<p>${esc(m)}</p>`).join('')}</div></div>
      ${flow.report?.path ? `<div class="path">${esc(flow.report.path)}</div>` : ''}
      ${chooseButtons()}
      ${s.app.dump ? `<button class="btn quiet sm setup-link" data-act="cancel">${esc(t('Cancel'))}</button>` : ''}
      ${detailsHtml()}`;
  },

  nup: () => `<h1>${esc(t('Unpacking your game files'))}</h1>
    ${meterHtml(t('Unpacking progress'))}
    <div class="setup-foot">
      <div class="path clip grow" id="nup-file"></div>
      <button class="btn sm" data-act="cancel-nup" data-focus>${esc(t('Cancel'))}</button>
    </div>`,

  noexe: (s) => `<h1>${esc(t('The game is missing'))}</h1>
    <p class="setup-line">${esc(fromBackend(s.app.readiness.detail))}</p>
    ${primaryHtml()}`,

  setup: (s) => {
    const kind = s.app.readiness?.kind;
    const finish = needsFinish(kind);
    return `<h1>${esc(flow.again || kind === 'mods' ? t('Set up again') : finish ? t('Finish the update') : kind === 'resume' ? t('Continue setup') : t('Set up the game'))}</h1>
      ${filesHtml(s)}
      ${finish || !['setup', 'resume', 'ready'].includes(kind) ? `<p class="setup-line">${esc(fromBackend(s.app.readiness.detail))}</p>` : ''}
      <div class="setup-options">${kind === 'setup' ? SMALL_ROW : ''}${GENTLE_ROW}</div>
      ${primaryHtml()}
      <p class="faint num setup-fact" id="fact"></p>
      ${flow.again ? `<button class="btn quiet sm setup-link" data-act="not-again">${esc(t('Cancel'))}</button>` : ''}
      ${detailsHtml()}`;
  },

  paused: (s) => `<h1>${esc(t('Setup paused'))}</h1>
    ${filesHtml(s)}
    <p class="setup-line">${esc(t('Finished work is kept.'))}</p>
    <div class="setup-options">${GENTLE_ROW}</div>
    ${primaryHtml()}
    <p class="faint num setup-fact" id="fact"></p>
    <div class="setup-link">${logButton()}</div>`,

  running: () => `<h1>${esc(t('Setting up'))}</h1>
    ${stepsHtml()}
    ${meterHtml(t('Setup progress'))}
    <div class="setup-foot">
      <p class="setup-msg grow num" id="msg"></p>
      <button class="btn sm" id="pause" data-act="pause" data-focus>${icon('pause')}<span>${esc(t('Pause'))}</span></button>
    </div>`,

  error: (s) => `<h1>${esc(t('Setup stopped'))}</h1>
    <pre class="setup-error selectable">${esc(fromBackend(s.setup.progress.message))}</pre>
    ${primaryHtml()}
    <div class="setup-link">
      ${logButton()}
      <button class="btn quiet" data-act="change">${icon('folder-open')}${esc(t('Change game files'))}</button>
      ${s.app.readiness?.kind === 'ready' ? `<button class="btn quiet" data-act="home">${icon('play')}${esc(t('Play'))}</button>` : ''}
    </div>`,

  done: (s) => `<h1>${esc(s.setup?.progress?.state === 'done' ? t('Setup finished') : t('Ready to play'))}</h1>
    ${filesHtml(s)}
    <button class="btn primary big setup-go" data-act="home" data-focus>${icon('play')}${esc(t('Play'))}</button>
    <button class="btn quiet sm setup-link" data-act="again">${esc(t('Set up again'))}</button>
    ${detailsHtml()}`,
};

export function render(el, ctx) {
  // ctx.change: Settings > Game "Change game files". ctx.again: "Set up again" over a finished setup.
  Object.assign(flow, { changing: !!ctx.change, again: !!ctx.again, typing: false, pausing: false, need: undefined });
  if (!flow.checking && !store.state.nup) Object.assign(flow, { report: null, error: null });

  el.innerHTML = `<div class="setup">
    ${ctx.firstRun ? `<div class="setup-brand"><img src="${logoUrl}" alt="Pokopia" /></div>` : ''}
    <section class="setup-panel glass strong" id="panel"></section>
  </div>`;
  const panel = el.querySelector('#panel');
  const part = (id) => panel.querySelector(`#${id}`);
  let shown = null;

  function live(s, view) {
    panel.classList.toggle('dropping', !!s.dragging && !['running', 'nup', 'checking'].includes(view));

    const go = part('primary');
    if (go) {
      const a = actionOf(s, view);
      const html = icon(a.icon, a.busy ? 'spin' : '');
      const slot = go.querySelector('.setup-go-icon');
      if (slot.dataset.html !== html) {
        slot.innerHTML = html;
        slot.dataset.html = html;
      }
      go.querySelector('.setup-go-label').textContent = a.label;
      const back = go.disabled && !a.disabled && document.activeElement === document.body;   // a dialog closed over the disabled button
      go.disabled = a.disabled;
      if (back) go.focus({ preventScroll: true });
      go.toggleAttribute('aria-busy', a.busy);
    }
    const gentle = part('gentle');
    if (gentle) gentle.checked = flow.gentle;
    const small = part('small');
    if (small) small.checked = flow.small;
    const fact = part('fact');
    if (fact) fact.textContent = factOf(s, view);

    const meter = (pct, label) => {
      part('pct').textContent = pct === null ? label : `${pct}%`;
      const bar = part('bar');
      if (pct === null) bar.removeAttribute('value');
      else bar.value = pct;
    };
    if (view === 'running') {
      const p = s.setup.progress;
      const { step, pct } = stepOf(p);
      meter(pct, t('Working'));
      part('eta').textContent = etaOf(p);
      const say = fromBackend(p.message);
      part('msg').textContent = say;
      part('msg').title = say;
      for (const [i, li] of panel.querySelectorAll('.setup-steps li').entries()) {
        li.dataset.state = i < step ? 'done' : i === step ? 'now' : '';
        if (i === step) li.setAttribute('aria-current', 'step');
        else li.removeAttribute('aria-current');
      }
      const pause = part('pause');
      pause.disabled = flow.pausing;
      pause.querySelector('span').textContent = flow.pausing ? t('Pausing') : t('Pause');
    }
    if (view === 'nup') {
      const n = s.nup;
      meter(n.total ? Math.min(100, Math.floor((100 * n.done) / n.total)) : null, t('Reading'));
      part('nup-file').textContent = n.file;
      part('nup-file').title = n.file;
    }
  }

  function draw() {
    const s = store.state;
    const view = viewOf(s);
    if (view === 'setup' || view === 'paused') wantNeed();
    if (view === 'running') flow.again = false;
    else {
      flow.pausing = false;
      etaReset();
    }
    const html = VIEWS[view](s);
    const moved = shown !== null && shown !== view;
    if (panel.dataset.html !== html) {
      panel.innerHTML = html;
      panel.dataset.html = html;
    }
    shown = view;
    live(s, view);
    // the panel changed under the keyboard: the next thing to press takes the focus
    if (moved) panel.querySelector('[data-focus]:not(:disabled)')?.focus({ preventScroll: true });
  }

  panel.addEventListener('submit', (e) => {
    e.preventDefault();
    take(part('path').value);
  });
  panel.addEventListener('change', (e) => {
    if (e.target.id === 'small') return saveSmall(e.target.checked);
    if (e.target.id !== 'gentle') return;
    saveGentle(e.target.checked);
    draw();
  });
  panel.addEventListener('click', (e) => {
    const act = e.target.closest('[data-act]')?.dataset.act;
    switch (act) {
      case 'folder': return pick(api.pickFolder, { title: t('Choose your Pokopia game files folder'), start: flow.saved || null });
      case 'nup': return pick(api.pickNup, { start: null });
      case 'dlc': return pick(api.pickNsp);
      case 'type':
        flow.typing = true;
        draw();
        return part('path').focus();
      case 'change': flow.changing = true; return draw();
      case 'cancel':
        Object.assign(flow, { changing: false, typing: false, report: null, error: null });
        loadSaved();
        return draw();
      case 'again': flow.again = true; return draw();
      case 'not-again': flow.again = false; return draw();
      case 'home': return ctx.go('home');
      case 'cancel-nup': return store.cancelNup().catch((err) => toast(errText(err), 'error'));
      case 'pause': flow.pausing = true; draw(); return store.pauseSetup();
      case 'log': return api.openSetupLog().catch((err) => toast(errText(err), 'error'));
      case 'primary': {
        const run = actionOf(store.state, shown).run;
        // the toggle is saved already; passing a choice the player made keeps preflight's suggestion from overruling it
        if (run === 'setup') return store.startSetup({ gentle: flow.touched ? flow.gentle : undefined });
        return store.perform(run, ctx.go);
      }
    }
  });

  paint = draw;
  ctx.on((s, why) => {
    if (why === 'drop' && s.dropped?.length) take(s.dropped[0]);
    draw();
  });
  loadSaved();
  draw();
  return () => { paint = null; };
}
