// The one source of app state. Owns every Tauri event listener, so a page never calls listen(); a page reads store.state
// and subscribes with ctx.on(fn). Long work is started here, not on a page, so it survives navigation.
import { listen } from '@tauri-apps/api/event';
import { api } from './api.js';
import { num, t } from './i18n.js';
import { fromBackend } from './backend-text.js';
import { confirm, errCode, errText, esc, icon, toast } from './ui.js';
import { keptLine, updatedLine } from './mods-model.js';
import { canInstall, canUpdate, gamePhase, needsFinish, taskOf } from './action.js';
import { askOnlineOnce } from './online.js';

const subs = new Set();
let autoFinishDone = false;   // `updated` is acted on once
let compactOffered = false;   // the "free up space" question is asked once per launcher start
const GB = 2 ** 30;
const UPDATE_CANCELLED = 'update_cancelled';   // error.rs: the player stopped it, not a failure

/** Applies a patch, derives `task`, tells subscribers. `why`: 'app' | 'setup' | 'game' | 'news' | 'task' | 'update' | 'drop' | 'storage'. */
function set(patch, why) {
  Object.assign(store.state, patch);
  store.state.task = taskOf(store.state);
  for (const fn of [...subs]) {
    try {
      fn(store.state, why);
    } catch (e) {
      console.error(e);   // one broken subscriber must not stop the others
    }
  }
}

function preflightList(pre) {
  const rows = pre.checks.filter((c) => c.level !== 'ok').map((c) => {
    const bad = c.level === 'fail';
    return `<li class="${bad ? 'bad' : 'warn'}">${icon(bad ? 'circle-x' : 'triangle-alert')}<div><strong>${esc(fromBackend(c.title))}</strong><span>${esc(fromBackend(c.detail))}</span></div></li>`;
  });
  return `<ul class="modal-list">${rows.join('')}</ul>`;
}

/** preflight, shown in one dialog from wherever setup was started. All fine starts at once; warnings can be passed, a failed check cannot. */
async function passPreflight() {
  let pre;
  try {
    pre = await api.preflight();
  } catch (e) {
    toast(errText(e), 'error');
    return { go: true };
  }
  if (pre.blocked) {
    await confirm({ title: t('Set up cannot start yet'), html: preflightList(pre), ok: t('Close'), alert: true });
    return { go: false };
  }
  if (pre.warnings) {
    const go = await confirm({ title: t('Check these before you start'), html: preflightList(pre), ok: t('Set up anyway') });
    return { go, gentle: pre.suggest_gentle || undefined };
  }
  return { go: true };
}

async function onSetupEvent(p) {
  const was = store.state.setup?.progress?.state;
  set({ setup: { ...store.state.setup, progress: p } }, 'setup');
  if (p.state === 'running') return;
  await store.refresh();
  if (was === 'running' && p.state === 'done') toast(t('Setup finished'), 'ok');
  if (was === 'running' && p.state === 'error') toast(t('Setup stopped with an error'), 'error');
}

export const store = {
  state: {
    app: null,            // launcher_state, null until it loads
    setup: null,          // setup_status
    game: null,           // game_status
    news: { releases: [], update: null },
    task: null,           // derived: { kind: 'setup'|'update'|'nup'|'dlss', label, pct (null: unknown) } | null
    update: null,         // last update-progress while an update runs
    updateError: null,
    nup: null,            // last nup-progress while a .nup unpacks
    dlss: null,           // last dlss-progress while DLSS downloads
    storage: null,        // shader_storage: how the translated shaders are kept (loose files or the shader archive), null until asked
    repack: null,         // { kind: 'compact'|'expand', done, total } while the shader files are being repacked
    starting: false,      // a start click is on its way to Rust
    dragging: false,      // files are being dragged over the window
    dropped: null,        // the paths of the drop that fired why 'drop'
    loadError: null,
  },

  /** ctx.on(fn) wraps this for pages. fn(state, why). */
  subscribe(fn) {
    subs.add(fn);
    return () => subs.delete(fn);
  },

  /** Attaches the listeners, then loads everything. Call once, before the first page. */
  async start() {
    const on = (event, fn) => listen(event, (ev) => fn(ev.payload)).catch((e) => console.error(event, e));
    await Promise.all([
      on('game-status', (p) => set({ game: p }, 'game')),
      on('setup-status', onSetupEvent),
      on('update-progress', (p) => set({ update: p }, 'update')),
      on('dlss-progress', (p) => store.state.dlss && set({ dlss: p }, 'task')),
      on('nup-progress', (p) => store.state.nup && set({ nup: p }, 'task')),
      on('shader-storage-progress', (p) => store.state.repack && set({ repack: p }, 'task')),
      on('tauri://drag-enter', () => set({ dragging: true, dropped: null }, 'drop')),
      on('tauri://drag-leave', () => set({ dragging: false, dropped: null }, 'drop')),
      on('tauri://drag-drop', (p) => set({ dragging: false, dropped: p.paths ?? [] }, 'drop')),
    ]);
    await store.refresh();
    store.refreshNews(false).then(() => store.refreshNews(true));
    afterUpdate();
    store.refreshStorage().then(offerCompact);
    store.keepLockedMods().then(() => store.updateMods());
  },

  /** The mod the launcher keeps on (Community Achievements and its sharing, mods_lock.rs): installed or switched back on at start unless
   *  the player unlocked it, then one line says what changed. Offline or busy: nothing, the next start tries again. Never in Play's way. */
  async keepLockedMods() {
    try {
      const line = keptLine(await api.modsKeepLocked());
      if (!line) return;
      toast(line, 'ok');
      await store.refresh();   // a new mod may need hook points: readiness says Set up again
    } catch (e) {
      console.warn('mods_keep_locked', e);
    }
  },

  /** Mods by themselves (launcher preference, off by default; once per session in Rust): says once which were updated. Silent otherwise. */
  async updateMods() {
    try {
      const done = await api.modsAutoUpdate();
      if (!done.length) return;
      toast(updatedLine(done), 'ok');
      await store.refresh();   // a new version may need other hook points: readiness says Set up again
    } catch (e) {
      console.warn('mods_auto_update', e);
    }
  },

  /** Reloads shader_storage (it lists the cache folder: a second or two on a hard drive, so it is not part of refresh()). */
  async refreshStorage() {
    try {
      set({ storage: await api.shaderStorage() }, 'storage');
    } catch (e) {
      console.warn('shader_storage', e);
    }
  },

  /** Moves the loose shader files into the shader archive through the sidebar task. Nothing is translated again. */
  async compactShaders() {
    if (store.state.repack) return;
    const before = store.state.storage;
    set({ repack: { kind: 'compact', done: 0, total: before?.loose_files ?? 0 } }, 'task');
    try {
      const r = await api.compactShaders();
      await store.refreshStorage();
      const freed = Math.max(0, (before?.loose_bytes ?? 0) + (before?.archive_bytes ?? 0) - (store.state.storage?.loose_bytes ?? 0) - (store.state.storage?.archive_bytes ?? 0));
      if (r.finished) toast(t('Shader files packed: {size} freed', { size: gb(freed) }), 'ok');
      else toast(t('Stopped. {size} freed so far, the rest is packed next time.', { size: gb(freed) }));
    } catch (e) {
      toast(errText(e), 'error');
    } finally {
      set({ repack: null }, 'task');
      store.refreshStorage();
    }
  },

  /** The undo: writes the shader archive back out as loose files. */
  async expandShaders() {
    if (store.state.repack) return;
    set({ repack: { kind: 'expand', done: 0, total: store.state.storage?.archive_shaders ?? 0 } }, 'task');
    try {
      if (await api.expandShaders()) toast(t('Shader files unpacked'), 'ok');
      else toast(t('Stopped. Unpacking continues where it stopped.'));
    } catch (e) {
      toast(errText(e), 'error');
    } finally {
      set({ repack: null }, 'task');
      store.refreshStorage();
    }
  },

  cancelRepack: () => api.cancelShaderStorage().catch((e) => toast(errText(e), 'error')),

  /** Reloads launcher_state, setup_status and game_status. A failed part keeps its last value. */
  async refresh() {
    const [app, setup, game] = await Promise.allSettled([api.launcherState(), api.setupStatus(), api.gameStatus()]);
    const patch = { loadError: app.status === 'rejected' ? errText(app.reason) : null };
    if (app.status === 'fulfilled') patch.app = app.value;
    if (setup.status === 'fulfilled') patch.setup = setup.value;
    if (game.status === 'fulfilled') patch.game = game.value;
    set(patch, 'app');
  },

  /** get_releases. refresh=false serves the cache, true asks GitHub. Silent on failure: patch notes are optional. */
  async refreshNews(refresh = false) {
    try {
      const news = await api.getReleases({ refresh });
      set({ news: { releases: news?.releases ?? [], update: news?.update ?? null } }, 'news');
    } catch (e) {
      console.warn('get_releases', e);
    }
  },

  /** Runs setup through preflight. gentle: undefined keeps the saved choice. Resolves true when setup started. */
  async startSetup({ gentle } = {}) {
    const s = store.state;
    if (s.starting || s.setup?.progress?.state === 'running') return false;
    set({ starting: true }, 'task');
    try {
      const pass = await passPreflight();
      if (!pass.go) return false;
      await api.startSetup({ gentle: gentle ?? pass.gentle });
      const setup = await api.setupStatus().catch(() => null);
      if (setup) set({ setup }, 'setup');
      return true;
    } catch (e) {
      toast(errText(e), 'error');
      store.refresh();
      return false;
    } finally {
      set({ starting: false }, 'task');
    }
  },

  /** Pause: finished work is kept, startSetup continues it. */
  async pauseSetup() {
    try {
      await api.cancelSetup();
    } catch (e) {
      toast(errText(e), 'error');
    }
  },

  /** `withoutMods`: this one start runs without mods (the player's way out when a mod keeps the game from starting). */
  async play({ withoutMods = false } = {}) {
    if (store.state.starting) return;
    await askOnlineOnce();   // once per install, before the first Play: online presence is off unless the player says yes
    set({ starting: true }, 'task');
    try {
      await api.play(withoutMods ? { withoutMods } : undefined);
    } catch (e) {
      toast(errText(e), 'error');
      store.refresh();
    } finally {
      set({ starting: false }, 'task');
    }
  },

  /** Downloads and applies the update; on success Rust restarts the launcher, so this only returns on failure. */
  async installUpdate() {
    if (!canUpdate(store.state)) return;
    set({ update: { phase: 'download', done: 0, total: 0 }, updateError: null }, 'update');
    try {
      await api.installUpdate();
    } catch (e) {
      const updateError = errCode(e) === UPDATE_CANCELLED ? null : errText(e);
      set({ update: null, updateError }, 'update');
      if (updateError) toast(updateError, 'error');
    }
  },

  /** Installs the build the player picked (an older one only after the warning was accepted). Like installUpdate: on success Rust restarts. */
  async installVersion(tag, channel, accepted) {
    if (!canInstall(store.state)) return;
    set({ update: { phase: 'download', done: 0, total: 0 }, updateError: null }, 'update');
    try {
      await api.installVersion({ tag, channel, accepted });
    } catch (e) {
      const updateError = errCode(e) === UPDATE_CANCELLED ? null : errText(e);
      set({ update: null, updateError }, 'update');
      if (updateError) toast(updateError, 'error');
    }
  },

  async cancelUpdate() {
    try {
      await api.cancelUpdate();
    } catch (e) {
      toast(errText(e), 'error');
    }
  },

  /** Unpacks a .nup through the sidebar task. Resolves to the unpacked folder; rejects like the command. */
  async importNup(path) {
    set({ nup: { done: 0, total: 0, file: '' } }, 'task');
    try {
      return await api.importNup({ path });
    } finally {
      set({ nup: null }, 'task');
    }
  },

  cancelNup: () => api.cancelNup(),

  /** Downloads NVIDIA's DLSS file through the sidebar task; rejects like the command. */
  async fetchDlss() {
    set({ dlss: { done: 0, total: 0, message: '' } }, 'task');
    try {
      await api.fetchDlss();
    } finally {
      set({ dlss: null }, 'task');
    }
  },

  /** Carries out primaryAction(state).run. `go` is the page's ctx.go. */
  perform(run, go) {
    switch (run) {
      case 'choose': return go('setup');
      case 'game-folder': return go('settings', { section: 'game' });
      case 'setup': return store.startSetup();
      case 'play': return store.play();
      case 'refresh': return store.refresh();
    }
  },
};

const gb = (bytes) => `${(bytes / GB).toFixed(bytes >= 10 * GB ? 0 : 1)} GB`;

/** Loose shader files take ~26 GB that the shader archive keeps in a fraction of that: a finished setup that still has them is asked
 *  once per launcher start. Not now only lasts until the next start: loose storage is on its way out. */
export function compactOffer(s) {
  const st = s.storage;
  if (!st?.complete || st.running || s.repack || s.task || s.starting) return null;
  if (!['loose', 'mixed'].includes(st.mode) || st.free_estimate < GB) return null;
  if (s.app?.readiness?.kind !== 'ready' || s.setup?.progress?.state === 'running') return null;
  if (['starting', 'running', 'finishing'].includes(gamePhase(s.game))) return null;
  return { free: st.free_estimate, loose: st.loose_bytes, files: st.loose_files };
}

/** The question itself; Settings > Game offers the same button without it. */
export async function askCompact() {
  const o = compactOffer(store.state) ?? { free: store.state.storage?.free_estimate ?? 0, loose: store.state.storage?.loose_bytes ?? 0, files: store.state.storage?.loose_files ?? 0 };
  const li = (cls, ic, title, text) => `<li class="${cls}">${icon(ic)}<div><strong>${esc(title)}</strong><span>${esc(text)}</span></div></li>`;
  const go = await confirm({
    title: t('Free up about {size}?', { size: gb(o.free) }),
    html: `<p>${esc(t('Your translated shaders take {size} as {files} separate files. Packed into a few compressed files, they take a fraction of that.', { size: gb(o.loose), files: num(o.files) }))}</p>
      <ul class="modal-list">
        ${li('ok', 'circle-check', t('Nothing is translated again'), t('The shaders are packed as they are. It takes a few minutes, and you can stop at any time.'))}
        ${li('ok', 'circle-check', t('The game runs the same'), t('It reads packed shaders as quickly as loose ones.'))}
        ${li('warn', 'triangle-alert', t('Loose shader files are going away'), t('A later update stops reading them, so this will be needed anyway.'))}
      </ul>`,
    ok: t('Free up space'),
    cancel: t('Not now'),
  });
  if (go) await store.compactShaders();
}

async function offerCompact() {
  if (compactOffered || !compactOffer(store.state)) return;
  compactOffered = true;
  await askCompact();
}

/** After a self-update restart: say so, and finish the update without being asked. */
function afterUpdate() {
  const app = store.state.app;
  if (!app?.updated || autoFinishDone) return;
  autoFinishDone = true;
  toast(t('Updated to {version}', { version: app.version }), 'ok');
  if (needsFinish(app.readiness?.kind)) store.startSetup();
}
