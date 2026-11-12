// The "which mods are on" notice's pure part (no DOM, no Tauri; scripts/test-mods-notice.mjs runs it). It lists what the NEXT start will
// use: the ini value if there is one, else the schema default (what readAll returns). An environment override of a game started
// separately is invisible here; pkp.log's "mods:" line is what the running game used.

/** Player-facing names of the gameplay mods and the two experimental switches. A key without one is shown by its key name. */
export const MOD_LABELS = {
  jump_momentum: 'Running jump',
  camera_aim: 'Camera aim',
  inventory_sort: 'Sort',
  craft_search: 'Search',
  item_editor: 'Item editor',
  pokemon_cap_50: 'Pokemon cap',
  menu_in_game: "Mouse in the game's menus",
  taller_fov: 'Taller view on 16:10 screens',
  fix_4x3: '4:3 Fix',
};

const isOn = (v) => v === true || v === '1' || v === 1;
const label = (key) => MOD_LABELS[key] ?? key;
const listOf = (keys, values) => keys.filter((k) => isOn(values[k])).map(label);

/**
 * @param {Record<string, unknown>} values  readAll() output (field id -> value)
 * @param {string[]} gameplayKeys  the keys of the mods whose switch the launcher shows (gameplaySwitchKeys)
 * @param {string[]} experimentalKeys  the Testing tab's experimental toggles
 * @returns {{ gameplay: string[], experimental: string[], lines: string[], text: string }} lists of labels, one sentence per category
 */
export function modsNotice(values, gameplayKeys, experimentalKeys = []) {
  const gameplay = listOf(gameplayKeys, values);
  const experimental = listOf(experimentalKeys, values);
  const lines = [`Enabled gameplay mods: ${gameplay.length ? gameplay.join(', ') : 'none'}.`];
  if (experimentalKeys.length) lines.push(`Experimental features enabled: ${experimental.length ? experimental.join(', ') : 'none'}.`);
  return { gameplay, experimental, lines, text: `${lines.join(' ')} Shown for the next start.` };
}
