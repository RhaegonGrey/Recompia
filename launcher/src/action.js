// Pure state logic: the hero's one button and the background task. No Tauri import, so `node --test` runs it.
// Only these keys of the store state are read: app, setup, game, news, update, nup, dlss, repack, starting, loadError.

import { t } from './i18n.js';

const FINISH_UPDATE = new Set(['rebuild', 'retranslate', 'rebase']);

/** The installed update needs setup to run again before Play. */
export const needsFinish = (kind) => FINISH_UPDATE.has(kind);

const setupRunning = (s) => s.setup?.progress?.state === 'running';

/**
 * Where the game is. Rust reports state "running" for three different moments, told apart by play.rs's message:
 * the shader step before launch, the game itself ("pkp started (process N)" / "pkp.exe started ..."), the shader step after
 * it exits. macOS has no shader step: its play.rs sends "Starting the game..." and then the game's message.
 */
export function gamePhase(game) {
  if (game?.state === 'exited') return 'exited';
  if (game?.state !== 'running') return 'idle';
  const m = game.message || '';
  if (m.startsWith('pkp.exe started') || m.startsWith('pkp started')) return 'running';
  if (m.startsWith('Translating the shaders the game built')) return 'finishing';
  return 'starting';
}

const gameActive = (s) => ['starting', 'running', 'finishing'].includes(gamePhase(s.game));

/** Full-window onboarding instead of the shell. */
export function isFirstRun(s) {
  const kind = s.app?.readiness?.kind;
  return kind === 'no_files' || (kind === 'bad_files' && !s.setup?.started);
}

const pctOf = (p) => (p?.total ? Math.min(100, Math.floor((100 * p.done) / p.total)) : null);

const UPDATE_LABEL = {
  download: t('Downloading update'),
  verify: t('Checking update'),
  extract: t('Unpacking update'),
  apply: t('Installing update'),
  restart: t('Restarting'),
};

/** The one background task shown in the sidebar pill, or null. An update outranks the rest: it restarts the launcher.
 *  ponytail: one pill, one task; a second task running at once waits behind it until the first ends. */
export function taskOf(s) {
  if (s.update) return { kind: 'update', label: UPDATE_LABEL[s.update.phase] || t('Updating'), pct: s.update.phase === 'restart' ? null : pctOf(s.update) };
  if (setupRunning(s)) return { kind: 'setup', label: t('Setting up'), pct: pctOf(s.setup.progress) };
  if (s.nup) return { kind: 'nup', label: t('Unpacking game files'), pct: pctOf(s.nup) };
  if (s.dlss) return { kind: 'dlss', label: t('Downloading DLSS'), pct: pctOf(s.dlss) };
  if (s.repack) return { kind: 'repack', label: s.repack.kind === 'expand' ? t('Unpacking shaders') : t('Packing shaders'), pct: pctOf(s.repack) };
  return null;
}

/** The Update button is usable: Rust refuses an update while the game or setup runs. */
export function canUpdate(s) {
  return !!s.news?.update && !taskOf(s) && !gameActive(s) && !s.starting;
}

/** A build the player picked can be installed (no newer update needs to exist: it may be an older one). Nothing else is running. */
export function canInstall(s) {
  return !s.update && !taskOf(s) && !gameActive(s) && !s.starting;
}

/** Rust stops an update until files start to move; after that it must finish. */
export const canCancelUpdate = (s) => ['download', 'verify', 'extract'].includes(s.update?.phase);

const off = (label, icon, busy = true) => ({ label, icon, run: null, disabled: true, busy });
const on = (label, icon, run) => ({ label, icon, run, disabled: false, busy: false });

/**
 * The hero's button. `run` names what a click does, for store.perform(): 'choose' | 'game-folder' | 'setup' | 'play' |
 * 'refresh', or null when disabled. Anything running wins over readiness; Play is only ever offered for "ready".
 */
export function primaryAction(s) {
  if (!s.app) return s.loadError ? on(t('Try again'), 'refresh-cw', 'refresh') : off(t('Loading'), 'loader-circle');
  if (s.starting) return off(t('One moment'), 'loader-circle');
  if (setupRunning(s)) return off(t('Setting up'), 'loader-circle');
  switch (gamePhase(s.game)) {
    case 'starting': return off(t('Starting'), 'loader-circle');
    case 'running': return off(t('Running'), 'gamepad-2', false);
    case 'finishing': return off(t('Finishing up'), 'loader-circle');
  }
  if (s.update) return off(t('Updating'), 'loader-circle');
  if (s.nup) return off(t('Unpacking'), 'loader-circle');
  if (s.repack) return off(s.repack.kind === 'expand' ? t('Unpacking shaders') : t('Packing shaders'), 'loader-circle');

  const kind = s.app.readiness?.kind;
  const stopped = ['cancelled', 'error'].includes(s.setup?.progress?.state);
  if (kind === 'no_files' || kind === 'bad_files') return on(t('Choose game files'), 'folder-open', 'choose');
  if (kind === 'no_exe' || kind === 'no_runtime') return on(t('Choose game folder'), 'folder-open', 'game-folder');
  if (kind === 'mixed_release') return off(t('Not ready'), 'circle-alert', false);   // nothing to click: the detail says to put the whole release in place
  if (kind === 'setup') return on(stopped ? t('Continue setup') : t('Set up'), 'wrench', 'setup');
  if (kind === 'resume') return on(t('Continue setup'), 'wrench', 'setup');
  if (needsFinish(kind)) return on(t('Finish update'), 'wrench', 'setup');
  if (kind === 'mods') return on(t('Set up again'), 'wrench', 'setup');   // the mods' hook points changed (Mods page)
  if (kind === 'ready') return on(t('Play'), 'play', 'play');
  return off(t('Not ready'), 'circle-alert', false);   // a kind this build does not know must not fall through to Play
}
