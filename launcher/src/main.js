import '@fontsource-variable/inter/opsz.css';
import './theme.css';
import './layout.css';
import './layout-modes.css';
import './background.css';
import './components.css';
import './gamemode.css';
import { getCurrentWindow } from '@tauri-apps/api/window';
import markUrl from './art/mark.png';
import markTestingUrl from './art/mark-testing.png';
import logoUrl from './art/logo.png';
import { api } from './api.js';
import { t } from './i18n.js';
import { errText, esc, icon } from './ui.js';
import { store } from './store.js';
import { isFirstRun } from './action.js';
import { startGamepadNav } from './gamepad-nav.js';
import { loadLayout, applyLayout } from './layout-apply.js';
import { loadBackground, applyBackground } from './background-apply.js';
import * as home from './pages/home.js';
import * as linkplay from './pages/linkplay.js';
import * as achievements from './pages/achievements.js';
import * as mods from './pages/mods.js';
import * as news from './pages/news.js';
import * as settings from './pages/settings.js';
import * as setup from './pages/setup.js';

// nav: shown in the sidebar. art: 'sharp' shows the island, otherwise the blurred copy.
const PAGES = {
  home: { mod: home, nav: t('Home'), icon: 'house', art: 'sharp' },
  linkplay: { mod: linkplay, nav: t('Link Play'), icon: 'users' },
  mods: { mod: mods, nav: t('Mods'), icon: 'puzzle' },
  achievements: { mod: achievements, nav: t('Achievements'), icon: 'trophy' },
  news: { mod: news, nav: t("What's New"), icon: 'newspaper' },
  settings: { mod: settings, nav: t('Settings'), icon: 'settings' },
  setup: { mod: setup },
};

// Dev in a plain browser only: the mock stands in for Rust, ?page=<id> opens a page directly. Vite drops this from production.
let startPage = null;
if (import.meta.env.DEV && !window.__TAURI_INTERNALS__) {
  await import('./dev/mock.js');
  PAGES.kit = { mod: await import('./dev/kit.js') };
  startPage = new URLSearchParams(location.search).get('page');
}

// where the sidebar pill leads
const TASK_PAGE = { setup: 'setup', nup: 'setup', update: 'home', dlss: 'settings', repack: 'home' };

const SWAP_WAIT_MS = 120;   // a page that loads fast swaps in whole; a slow one shows what it has by then

const nav = document.getElementById('nav');
const view = document.getElementById('view');
let current = null;
let token = 0;           // the newest navigation; older ones drop what they rendered
let leaving = [];        // run when the page goes

// Launcher colours: 'koi' (Koi's Camp: a bright daytime picture, the default), 'sunrise' (amber to red; reads well with red-green colour blindness), 'dusk' (the old violet) or 'meadow' (a bright daytime scene made for the project).
// Per PC, kept in localStorage: nothing else needs it, and losing it just shows the default.
const THEMES = ['sunrise', 'dusk', 'koi', 'meadow'];
// theme names are proper names: they stay in English in every language
const THEME_NAME = { sunrise: 'Sunrise', dusk: 'Dusk', koi: "Koi's Camp", meadow: 'Meadow' };
function readTheme() {
  try { return THEMES.includes(localStorage.getItem('theme')) ? localStorage.getItem('theme') : 'koi'; } catch { return 'koi'; }
}
function applyTheme(theme) {
  document.documentElement.dataset.theme = theme;
  const b = document.getElementById('theme-btn');
  if (b) b.querySelector('.nav-label').textContent = b.querySelector('.nav-tip').textContent = t('Colours: {name}', { name: THEME_NAME[theme] });
}
applyTheme(readTheme());
applyLayout(loadLayout());
applyBackground(loadBackground());

// Hide sidebar: a per-PC preference kept in localStorage like the theme. When hidden the nav collapses so the art is
// unobstructed; the floating #show-sidebar button (index.html) is the only way back. data-sidebar="hide" on <html> drives the CSS.
function readSidebar() {
  try { return localStorage.getItem('sidebar') === 'hide'; } catch { return false; }
}
function applySidebar(hidden) {
  if (hidden) document.documentElement.setAttribute('data-sidebar', 'hide');
  else document.documentElement.removeAttribute('data-sidebar');
}
applySidebar(readSidebar());

function drawNav() {
  const items = Object.entries(PAGES).filter(([, p]) => p.nav).map(([id, p]) =>
    `<button class="nav-item" data-page="${id}" aria-label="${esc(p.nav)}">${icon(p.icon)}<span class="nav-label">${esc(p.nav)}</span><span class="nav-tip" aria-hidden="true">${esc(p.nav)}</span></button>`);
  nav.innerHTML = `
    <div class="brand" data-tauri-drag-region><img class="brand-mark" src="${markUrl}" alt="" /><img class="brand-mark-testing" src="${markTestingUrl}" alt="${esc(t('Testing'))}" /><img class="brand-name brand-logo" src="${logoUrl}" alt="Pokopia" /><span class="brand-tag">${esc(t('Testing'))}</span></div>
    ${items.join('')}
    <div class="nav-spacer" data-tauri-drag-region></div>
    <button class="nav-item" id="theme-btn" aria-label="${esc(t('Change colours'))}">${icon('palette')}<span class="nav-label"></span><span class="nav-tip" aria-hidden="true"></span></button>
    <button class="nav-item" id="layout-btn" aria-label="Change layout">${icon('layout-dashboard')}<span class="nav-label">Layout</span><span class="nav-tip" aria-hidden="true">Layout</span></button>
    <button class="nav-item" id="ui-hide-btn" aria-label="${esc(t('Hide sidebar'))}">${icon('eye-off')}<span class="nav-label">${esc(t('Hide sidebar'))}</span><span class="nav-tip" aria-hidden="true">${esc(t('Hide sidebar'))}</span></button>
    <button class="task-pill" id="task-pill" hidden>
      ${icon('loader-circle', 'spin')}<span class="task-name clip"></span><span class="task-pct"></span>
      <span class="task-track"><span class="task-fill"></span></span>
    </button>`;
  applyTheme(readTheme());
  applyLayout(loadLayout());
  nav.addEventListener('click', (e) => {
    if (e.target.closest('#theme-btn')) {
      const next = THEMES[(THEMES.indexOf(readTheme()) + 1) % THEMES.length];
      try { localStorage.setItem('theme', next); } catch { /* the choice lasts until the launcher closes */ }
      applyTheme(next);
    }
    if (e.target.closest('#layout-btn')) {
      import('./layout-panel.js').then((m) => m.openLayoutPanel()).catch((err) => console.error(err));
    }
    if (e.target.closest('#ui-hide-btn')) {
      const hide = !readSidebar();
      try { localStorage.setItem('sidebar', hide ? 'hide' : 'show'); } catch { /* the choice lasts until the launcher closes */ }
      applySidebar(hide);
    }
    const page = e.target.closest('[data-page]')?.dataset.page;
    if (page && page !== current) go(page);
    const target = e.target.closest('#task-pill') && TASK_PAGE[store.state.task?.kind];
    if (target && target !== current) go(target);
  });
}

/** The Testing channel is marked in the sidebar (the Testing icon and a tag), so a tester can tell this launcher from a Stable one at a glance. */
async function syncChannel() {
  try {
    const c = await api.getConfig();
    document.documentElement.dataset.channel = c?.channel === 'testing' ? 'testing' : 'stable';
  } catch (e) {
    console.warn('get_config', e);
  }
}

function drawChrome() {
  for (const b of nav.querySelectorAll('[data-page]')) {
    if (b.dataset.page === current) b.setAttribute('aria-current', 'page');
    else b.removeAttribute('aria-current');
  }
  // the update has its own dot beside What's New, where its notes are
  const item = nav.querySelector('[data-page="news"]');
  const hasDot = !!item.querySelector('.nav-dot');
  if (hasDot !== !!store.state.news.update) {
    if (hasDot) item.querySelector('.nav-dot').remove();
    else item.insertAdjacentHTML('beforeend', '<span class="nav-dot"></span>');
  }

  const pill = document.getElementById('task-pill');
  const task = store.state.task;
  pill.hidden = !task;
  if (!task) return;
  const pct = task.pct === null ? '' : `${task.pct}%`;
  const say = pct ? `${task.label}, ${pct}` : task.label;
  pill.querySelector('.task-name').textContent = task.label;
  pill.querySelector('.task-pct').textContent = pct;
  pill.title = say;
  pill.setAttribute('aria-label', say);
  pill.style.setProperty('--pct', pct || '0%');
  pill.toggleAttribute('data-indeterminate', task.pct === null);
}

/** Shows a page. opts land on the page's ctx (ctx.firstRun, ctx.tag...); firstRun on setup hides the sidebar. */
async function go(page, opts = {}) {
  if (!PAGES[page]) return;
  const id = ++token;
  leaving.forEach((fn) => fn());
  leaving = [];
  current = page;
  document.body.dataset.shell = page === 'setup' && opts.firstRun ? 'full' : '';
  document.body.dataset.art = PAGES[page].art || 'soft';
  drawChrome();

  const host = document.createElement('div');
  host.className = 'page-host';
  const ctx = {
    ...opts,
    go,
    on(fn) {
      if (id !== token) return () => {};
      const off = store.subscribe(fn);
      leaving.push(off);
      return off;
    },
  };
  const rendered = (async () => {
    try {
      const dispose = await PAGES[page].mod.render(host, ctx);
      if (typeof dispose === 'function') (id === token ? leaving.push(dispose) : dispose());
    } catch (e) {
      console.error(e);
      host.innerHTML = `<div class="page"><div class="banner danger">${icon('circle-x')}<div>${esc(errText(e))}</div></div></div>`;
    }
  })();
  await Promise.race([rendered, new Promise((r) => setTimeout(r, SWAP_WAIT_MS))]);
  if (id !== token) return;

  host.classList.add('entering');
  host.addEventListener('animationend', (e) => { if (e.target === host) host.classList.remove('entering'); });
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) host.classList.remove('entering');   // no animation, no animationend (#260)
  view.replaceChildren(host);
  view.scrollTop = 0;
  view.focus({ preventScroll: true });
}

function drawWindowButtons() {
  const host = document.getElementById('winbtns');
  const win = getCurrentWindow();
  const button = (id, name, label, cls = '') => `<button class="win-btn ${cls}" id="${id}" aria-label="${label}" title="${label}">${icon(name)}</button>`;
  const quiet = (p) => p.catch((e) => console.warn(e));
  host.innerHTML = button('win-min', 'minus', t('Minimise')) + button('win-max', 'square', t('Maximise')) + button('win-close', 'x', t('Close'), 'close');
  document.getElementById('win-min').addEventListener('click', () => quiet(win.minimize()));
  document.getElementById('win-max').addEventListener('click', () => quiet(win.toggleMaximize()));
  document.getElementById('win-close').addEventListener('click', () => quiet(win.close()));
  document.getElementById('titlebar').addEventListener('pointerdown', (e) => {
    if (e.pointerType !== 'touch' || e.target.closest('button')) return;
    e.preventDefault();
    quiet(win.startDragging());
  });
  const max = document.getElementById('win-max');
  const syncMax = () => quiet(win.isMaximized().then((on) => {
    max.innerHTML = icon(on ? 'copy' : 'square');
    max.title = on ? t('Restore') : t('Maximise');
    max.setAttribute('aria-label', max.title);
  }));
  quiet(win.onResized(syncMax));
  syncMax();
}

function start() {
  // the static shell of index.html is English; the language of the player goes on here
  nav.setAttribute('aria-label', t('Main'));
  document.getElementById('winbtns').setAttribute('aria-label', t('Window'));
  startGamepadNav();
  drawNav();
  drawWindowButtons();
  const showSidebar = document.getElementById('show-sidebar');
  showSidebar.innerHTML = `${icon('eye')}<span>${esc(t('Show sidebar'))}</span>`;
  showSidebar.setAttribute('aria-label', t('Show sidebar'));
  showSidebar.addEventListener('click', () => {
    try { localStorage.setItem('sidebar', 'show'); } catch { /* the choice lasts until the launcher closes */ }
    applySidebar(false);
  });
  store.subscribe(drawChrome);
  store.subscribe((_s, why) => { if (why === 'news') syncChannel(); });
  syncChannel();
  drawChrome();
  store.start().catch((e) => console.error(e));
  const once = store.subscribe((s, why) => {
    if (why !== 'app') return;
    once();
    if (startPage) go(startPage);
    else go(isFirstRun(s) ? 'setup' : 'home', { firstRun: isFirstRun(s) });
  });
}

start();
