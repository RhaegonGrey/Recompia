// The Settings page's state: one object every module reads and writes (st), and env(st), the facts about the launcher the schema's
// rows ask for (which rows are on, what a button says). Modules never keep page state of their own; they go through st.
import { store } from '../../store.js';
import { t } from '../../i18n.js';
import { canUpdate, gamePhase } from '../../action.js';
import { makeSource } from '../../settings-schema.js';
import { backupDetail, backupText } from '../backups.js';

let lastSection = 'display';   // coming back to Settings lands where the player was

export const rememberSection = (id) => { lastSection = id; };

/** The channel the Settings page shows: 'testing' only when the launcher is set to it. */
export const channelOf = (st) => (st.src.config.channel === 'testing' ? 'testing' : 'stable');

export function createState({ dump, cfg, dlss, overview, promptIcons }, ctx) {
  return {
    src: makeSource(dump, cfg),
    dlss,
    overview,
    promptIcons,          // pack entry -> image URL for the Button icons preview (prompt-icons.js), null without an icon pack
    gameDir: dump.game_dir,
    sources: [],
    profiles: { active: 'default', profiles: [] },
    repairBackup: null,   // the last repair's backup (name, files), when Undo has something to put back
    backups: null,        // savebackup.rs Status: the settings, how many copies and the newest
    steamBusy: false,
    steam: null,          // steam.rs Status: Steam found and running, and whether the game and the launcher shortcuts are in it
    section: store.state.dlss ? 'graphics' : ctx.section || lastSection,
    customOpen: new Set(),   // choice rows showing their number field although the value is a preset
    unsaved: [],             // edits in the queue, not on disk yet
    importing: false,
    importResult: null,
    checking: false,
    checked: false,
  };
}

export function env(st) {
  const s = store.state;
  const phase = gamePhase(s.game);
  const gameBusy = phase === 'starting' || phase === 'running' || phase === 'finishing';
  const update = s.news.update;
  return {
    gameDir: st.gameDir,
    defaultGameDir: st.overview?.default_game_dir ?? '',
    gameFiles: st.src.config.dump_dir || s.app?.dump?.path || '',
    version: s.app?.version ?? '',
    gameBusy,
    canSetUp: !!s.app?.dump && !gameBusy && !s.task && !s.starting,
    canPlay: s.app?.readiness?.kind === 'ready' && !gameBusy && !s.task && !s.starting,
    storage: s.storage,
    repack: s.repack,
    taskBusy: !!s.task || s.starting,
    saveSources: st.sources,
    profiles: st.profiles,
    repairBackup: st.repairBackup,
    backups: st.backups,
    backupLine: backupText(st.backups),
    steam: st.steam,
    linux: /Linux/.test(globalThis.navigator?.userAgent ?? '') && !/Android/.test(globalThis.navigator?.userAgent ?? ''),
    steamBusy: st.steamBusy,
    backupDetail: backupDetail(st.backups),
    importing: st.importing,
    importResult: st.importResult,
    promptIcons: st.promptIcons,
    dlssCanFetch: !!st.dlss?.available,
    dlssPresent: !!st.dlss?.present,
    dlssBusy: !!s.dlss,
    canUpdate: canUpdate(s),
    updating: !!s.update,
    checking: st.checking,
    updateText: st.checking ? t('Checking')
      : update ? t('Version {version} is available', { version: update.tag.replace(/^v/, '') })
      : !st.checked ? ''
      : s.news.releases.length ? t('You have the latest version')
      : t('Could not reach GitHub'),
  };
}
