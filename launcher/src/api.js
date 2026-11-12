// Every Rust command the pages call, one named function each. The command names and argument names are the ones in the Rust
// signatures (src-tauri/src/*.rs, listed in main.rs generate_handler!): an argument object goes through as it is. A page never
// types a command name; checks that every name here is registered in Rust, and every one registered is here, live in
// scripts/check-api.mjs.
import { invoke } from '@tauri-apps/api/core';

export const api = {
  // Launcher state, settings and the launcher's own options
  getConfig: () => invoke('get_config'),
  saveConfig: (args) => invoke('save_config', args),   // { config }
  launcherState: () => invoke('launcher_state'),
  getOverview: () => invoke('get_overview'),
  readSettings: () => invoke('read_settings'),
  writeSettings: (args) => invoke('write_settings', args),   // { edits }
  promptIconPack: () => invoke('prompt_icon_pack'),   // ArrayBuffer: assets/prompts/prompt_icons.pak

  // Updates and builds
  getReleases: (args) => invoke('get_releases', args),   // { refresh }
  listBuilds: (args) => invoke('list_builds', args),   // { channel, refresh }
  installUpdate: () => invoke('install_update'),
  installVersion: (args) => invoke('install_version', args),   // { tag, channel, accepted }
  cancelUpdate: () => invoke('cancel_update'),

  // Opening links, files and folders
  openLink: (args) => invoke('open_link', args),   // { url }
  openGameFile: (args) => invoke('open_game_file', args),   // { name }
  openGameDir: () => invoke('open_game_dir'),
  openSaveDir: () => invoke('open_save_dir'),
  openBackupDir: () => invoke('open_backup_dir'),
  openSetupLog: () => invoke('open_setup_log'),

  // The Testing tab's logs (testlogs_cmds.rs)
  testingLogsList: () => invoke('testing_logs_list'),
  testingLogRead: (args) => invoke('testing_log_read', args),   // { name, lines }
  testingLogsExport: (args) => invoke('testing_logs_export', args),   // { version }
  testingLogsOpenDir: () => invoke('testing_logs_open_dir'),

  // Choosing the game files
  pickFolder: (args) => invoke('pick_folder', args),   // { title, start }
  pickNup: (args) => invoke('pick_nup', args),   // { start }
  pickNsp: () => invoke('pick_nsp'),
  validateDump: (args) => invoke('validate_dump', args),   // { path }
  saveDump: (args) => invoke('save_dump', args),   // { path }
  importNup: (args) => invoke('import_nup', args),   // { path }
  importDlc: (args) => invoke('import_dlc', args),   // { path }
  cancelNup: () => invoke('cancel_nup'),

  // Play and setup
  play: (args) => invoke('play', args),   // { withoutMods } (optional: this one start runs without mods)
  gameStatus: () => invoke('game_status'),
  setupStatus: () => invoke('setup_status'),
  startSetup: (args) => invoke('start_setup', args),   // { gentle }
  cancelSetup: () => invoke('cancel_setup'),
  preflight: () => invoke('preflight'),

  // Shader storage and DLSS
  shaderStorage: () => invoke('shader_storage'),
  compactShaders: () => invoke('compact_shaders'),
  expandShaders: () => invoke('expand_shaders'),
  cancelShaderStorage: () => invoke('cancel_shader_storage'),
  dlssStatus: () => invoke('dlss_status'),
  fetchDlss: () => invoke('fetch_dlss'),

  // Saves, backups, repair and profiles
  saveSources: () => invoke('save_sources'),
  importSave: (args) => invoke('import_save', args),   // { source }
  backupStatus: () => invoke('backup_status'),
  backupList: () => invoke('backup_list'),
  backupNow: () => invoke('backup_now'),
  backupRestore: (args) => invoke('backup_restore', args),   // { name }
  repairPlan: (args) => invoke('repair_plan', args),   // { actions, groups }
  repairApply: (args) => invoke('repair_apply', args),   // { actions, groups }
  repairUndo: () => invoke('repair_undo'),
  repairLast: () => invoke('repair_last'),
  profilesList: () => invoke('profiles_list'),
  profileAdd: (args) => invoke('profile_add', args),   // { name }
  profileRename: (args) => invoke('profile_rename', args),   // { id, name }
  profileRemove: (args) => invoke('profile_remove', args),   // { id }
  profileSelect: (args) => invoke('profile_select', args),   // { id }

  // Steam
  steamStatus: () => invoke('steam_status'),
  steamAdd: (args) => invoke('steam_add', args),   // { target }
  steamRemove: (args) => invoke('steam_remove', args),   // { target }

  // Link Play
  relayStart: (args) => invoke('relay_start', args),   // { port }
  relayStop: () => invoke('relay_stop'),
  relayStatus: () => invoke('relay_status'),
  lanReport: () => invoke('lan_report'),
  publicIp: () => invoke('public_ip'),
  roomCodeNew: () => invoke('room_code_new'),
  roomCodeCheck: (args) => invoke('room_code_check', args),   // { code }
  roomCodeHost: () => invoke('room_code_host'),
  // Link Play checks (src-tauri/src/linkcheck.rs holds the shapes): read only
  linkProbe: (args) => invoke('link_probe', args),   // { addr } -> { kind: answered | refused | timed_out | bad_address, ms?, message? }
  setupFacts: () => invoke('setup_facts'),           // -> { firewalld: { state, zones, ports } }
  tailscalePeers: () => invoke('tailscale_peers'),   // -> { found, error?, self, peers: [{ name, dns_name, ips, online, hosting }] }

  // Mods (Modtopia)
  modsList: () => invoke('mods_list'),
  modsSetEnabled: (args) => invoke('mods_set_enabled', args),   // { id, on, allow }
  modsMove: (args) => invoke('mods_move', args),   // { id, delta }
  modsSetSetting: (args) => invoke('mods_set_setting', args),   // { id, key, number | text }
  modsRemove: (args) => invoke('mods_remove', args),   // { id }
  modsCatalog: (args) => invoke('mods_catalog', args),   // { refresh }
  modsInstall: (args) => invoke('mods_install', args),   // { id }
  modsAutoUpdate: () => invoke('mods_auto_update'),   // [{ name, from, to }] updated, once per session, only with the launcher preference on
  modsKeepLocked: () => invoke('mods_keep_locked'),   // ['install' | 'switch_on' | 'share_on'] done, once at launcher start
  modsSetLock: (args) => invoke('mods_set_lock', args),   // { what: 'mod' | 'sharing', locked }
  openModsDir: () => invoke('open_mods_dir'),
  // Achievements (the community tracker)
  achievementsView: (args) => invoke('achievements_view', args),   // { refresh }
};
