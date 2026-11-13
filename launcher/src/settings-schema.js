// Every row of the Settings page, and the pure mapping between the files and the controls. No DOM, no Tauri: `node --test` runs it.
// A new setting is one entry in SECTIONS; pages/settings.js draws and saves it without being touched.
// Keys, defaults and ranges come from the runtime's schemas (runtime/docs/controls_schema.json for controls.ini, pkp_ini_schema.json for
// pkp.ini's [pkp] section), which runtime/scripts/gen_settings_doc.ps1 generates from the runtime's settings tables: a row takes them
// through fromSchema(at), so a missing key reads as the default the game itself uses. Labels, grouping, option lists and help text stay
// here. scripts/test-settings-schema.mjs checks every key shown is in the schema, live and writable, and every option is a value it takes.
//
// A source is { pkp, controls, config, controls_exists }: read_settings' `section.key` maps plus get_config.
// An edit is { file, section, key, value }. file 'pkp' and 'controls' go to write_settings, 'config' to save_config.
//
// Field: { id, label, type, at, def, options, min, max, step, suffix, ... }
//   type     segmented | select | choice | size | toggle | slider | path | clock
//   at       where it lives: controls(), pkp() or config(); an array when it has two keys
//   read     (src) => the control's value, when the plain mapping of `type` does not fit
//   write    (value, src) => edits, likewise
//   writeSame (src) => true to call write even when the value is unchanged (another key may still need resetting)
//   when     (values, env) => shown or not
//   enabled  (src) => false disables the row; the default disables what lives in a controls.ini that does not exist yet
//   lock     (env) => true disables the row for now
//   replaces true: writes other fields too, so the page closes any Custom number field it holds open
//   quality  'value' | 'frame': a value quality presets set, so changing it by hand makes the preset Custom (see markCustom)
// A choice is a list of presets plus Custom with a number field (custom: { min, max, suffix, start, label }), a size likewise with two.
// Rows that are not fields: info (label, value, actions), keycap (label, key), note (a line of text), and { expand } for rows made from env.

import { num, t } from './i18n.js';
import { previewItem } from './prompt-pack-model.js';
import controlsSchema from '../../runtime/docs/controls_schema.json' with { type: 'json' };
import gameIniSchema from '../../runtime/docs/pkp_ini_schema.json' with { type: 'json' };

const controls = (section, key) => ({ file: 'controls', section, key });
const pkp = (section, key) => ({ file: 'pkp', section, key });
const config = (key) => ({ file: 'config', section: '', key });

/** The logo and icon choices: 'Recompia' is a name and stays in English. */
const LOOKS = [{ value: 'recompia', label: 'Recompia' }, { value: 'classic', label: t('Classic') }];

const keyOf = (at) => (at.file === 'config' ? at.key : `${at.section}.${at.key}`);
const rawOf = (src, at) => src[at.file]?.[keyOf(at)];
const has = (src, at) => {
  const raw = rawOf(src, at);
  return raw != null && String(raw).trim() !== '';
};
const edit = (at, value) => ({ ...at, value });
const atsOf = (f) => [f.at ?? []].flat();

const clamp = (n, lo, hi) => Math.min(hi, Math.max(lo, n));
const truthy = (v) => ['1', 'true', 'yes', 'on'].includes(String(v).trim().toLowerCase());
const fwd = (s) => String(s).trim().replace(/\\/g, '/');
/** The runtime's name_key(): case and `-` are forgiven in the words of the file. */
const word = (v) => String(v).trim().toLowerCase().replace(/-/g, '_');

// ---- the runtime's schema -----------------------------------------------------------------------------------------------------------

/** The generated schema of each file a row can live in. pkp.ini sections other than [pkp] ([fs], [backup], [time]) have no table yet. */
export const SCHEMAS = { controls: controlsSchema, pkp: gameIniSchema };

/** The runtime's entry for a key ({ key, type, default, min, max, choices, auto, read_only, ... }), or undefined. */
export const schemaKey = (at) => SCHEMAS[at.file]?.sections.find((s) => s.name === at.section)?.keys.find((k) => k.key === at.key);

/** A retired key: read and ignored by the runtime, never written. */
export const isRetired = (at) => !!SCHEMAS[at.file]?.retired.some((r) => r.section === at.section && r.key === at.key);

/** { at, def, min, max } of a key from the runtime's schema (min / max only when it has a range). A key it lacks is a bug: throws. */
export function fromSchema(at) {
  const k = schemaKey(at);
  if (!k) throw new Error(`settings-schema: [${at.section}] ${at.key} is not in the runtime's ${at.file} schema`);
  return { at, def: k.default, ...(k.min !== undefined && { min: k.min }), ...(k.max !== undefined && { max: k.max }) };
}

// ---- button icons -------------------------------------------------------------------------------------------------------------------

/** The other spellings of [prompts] style the runtime reads (parse_prompt_style). */
const PROMPT_ALIASES = {
  ps: 'playstation', ps4: 'playstation', ps5: 'playstation', dualsense: 'playstation', steamcontroller: 'steam', steam_controller: 'steam',
  deck: 'steamdeck', steam_deck: 'steamdeck', nintendo_switch: 'switch1', ns1: 'switch1', switch_2: 'switch2', nintendo_switch_2: 'switch2',
  nintendo_switch2: 'switch2', ns2: 'switch2', wii_u: 'wiiu', steam_frame: 'steamframe', frame: 'steamframe', valve_index: 'index',
  valveindex: 'index', touchscreen: 'touch', kbm: 'keyboard', keyboard_mouse: 'keyboard', mouse: 'keyboard',
};

/** The Button icons preview of a style: { text } for the game's own icons and Automatic, else { items: [{ button, icon }] } with the icon
 *  of each Switch button as the runtime shows it with the factory pad mapping and keys (the schema's choice_details, from prompt_preview). */
export function promptPreview(style) {
  const w = PROMPT_ALIASES[word(style)] ?? word(style);
  if (w === 'switch') return { text: t("The game's own Switch icons") };
  if (w === 'auto') return { text: t('The icons of the device you used last') };
  return { items: (schemaKey(controls('prompts', 'style'))?.choice_details?.[w] ?? []).map(previewItem) };
}

// ---- shader files -------------------------------------------------------------------------------------------------------------------
// env.storage: shader_storage (shader_archive.rs Storage, plus complete and running). env.repack: the running job, or null.

const GB = 2 ** 30;
const gb = (b) => `${(b / GB).toFixed(b >= 10 * GB ? 0 : 1)} GB`;
const count = (n) => num(n);

export function storageText(env) {
  const st = env.storage;
  const r = env.repack;
  if (r) {
    const what = r.kind === 'expand' ? t('Unpacking') : t('Packing');
    return r.total ? `${what}: ${Math.floor((100 * r.done) / r.total)}%` : `${what}...`;
  }
  if (!st) return t('Checking');
  switch (st.mode) {
    case 'none': return t('Not set up yet');
    case 'loose': return t('Loose files, {size}', { size: gb(st.loose_bytes) });
    case 'compact': return t('Packed, {size}', { size: gb(st.archive_bytes + st.loose_bytes) });
    default: return t('Partly packed, {size}', { size: gb(st.archive_bytes + st.loose_bytes) });
  }
}

export function storageActions(env) {
  if (env.repack) return [{ act: 'stop-repack', label: t('Stop'), icon: 'square' }];
  const st = env.storage;
  const off = !st?.complete || env.gameBusy || env.taskBusy;
  if (st?.mode === 'loose' || st?.mode === 'mixed') return [{ act: 'compact-shaders', label: t('Free up space'), icon: 'hard-drive', primary: true, disabled: off }];
  if (st?.mode === 'compact') return [{ act: 'expand-shaders', label: t('Use loose files'), icon: 'rotate-ccw', disabled: off }];
  return [];
}

export function storageNote(env) {
  const st = env.storage;
  if (!st || env.repack) return null;
  if (st.mode === 'loose' || st.mode === 'mixed') {
    return { text: t('Packing frees about {size}. Nothing is translated again and the game runs the same. A later update stops reading loose shader files.', { size: gb(st.free_estimate) }) };
  }
  if (st.mode === 'compact' && st.archive_raw_bytes > 0) {
    const ratio = st.archive_raw_bytes / Math.max(1, st.archive_bytes);
    return { text: t('{count} shaders in {size}, {ratio}x smaller than loose files ({raw}).', { count: count(st.archive_shaders), size: gb(st.archive_bytes), ratio: ratio.toFixed(1), raw: gb(st.archive_raw_bytes) }) };
  }
  return null;
}

// ---- quality presets --------------------------------------------------------------------------------------------------------------

// Ids, labels and which ones set the frame values: kQualityPresets in runtime/src/gfx/play/play_types.cpp (labels without its bracket).
// The values: kPresets in runtime/games/pkp/render/pkp_quality.cpp, which the F1 page applies. The tests parse both files.
// shadow_quality is [display] shadow_quality and aa is [upscale] aa. render_scale 0 and fps_limit -1 are written as auto.
export const QUALITY_PRESETS = [
  { id: 'switch2', label: t('Switch 2'), lod_bias: 0, anisotropy: 0, shadow_quality: 'default', aa: 'off' },
  { id: 'high', label: t('High'), lod_bias: -0.5, anisotropy: 8, shadow_quality: 'high', aa: 'smaa_high' },
  { id: 'ultra', label: t('Ultra'), lod_bias: -1, anisotropy: 16, shadow_quality: 'very_high', aa: 'smaa_ultra' },
  { id: 'steamdeck', label: t('Steam Deck'), lod_bias: 0, anisotropy: 4, shadow_quality: 'default', aa: 'off', render_scale: 75, fps_limit: 40 },
  { id: 'legion_go_s', label: t('Legion Go S'), lod_bias: 0, anisotropy: 8, shadow_quality: 'default', aa: 'off', render_scale: 80, fps_limit: 60 },
  { id: 'laptop', label: t('Laptop'), lod_bias: 0, anisotropy: 4, shadow_quality: 'default', aa: 'off', render_scale: 75, fps_limit: 60 },
  { id: 'desktop', label: t('Desktop PC'), lod_bias: -0.5, anisotropy: 16, shadow_quality: 'very_high', aa: 'smaa_high', render_scale: 0, fps_limit: -1 },
];

const PRESET = fromSchema(controls('display', 'quality_preset'));
const AT_PRESET = PRESET.at;
const RENDER_SCALE = fromSchema(controls('display', 'render_scale'));
const AT_RENDER_SCALE = RENDER_SCALE.at;
const FPS_LIMIT = fromSchema(controls('display', 'fps_limit'));
const AT_FPS_LIMIT = FPS_LIMIT.at;
const AT_LOD_BIAS = fromSchema(controls('display', 'lod_bias')).at;
const AT_ANISOTROPY = fromSchema(controls('display', 'anisotropy')).at;
const AT_SHADOW_QUALITY = fromSchema(controls('display', 'shadow_quality')).at;
const AT_AA = fromSchema(controls('upscale', 'aa')).at;

function presetEdits(id) {
  const p = QUALITY_PRESETS.find((q) => q.id === id);
  if (!p) return [];   // Custom is what a preset becomes, never something picked: the F1 list does not offer it either
  const edits = [
    edit(AT_PRESET, id),
    edit(AT_LOD_BIAS, String(p.lod_bias)),
    edit(AT_ANISOTROPY, String(p.anisotropy)),
    edit(AT_SHADOW_QUALITY, p.shadow_quality),
    edit(AT_AA, p.aa),
  ];
  if (p.render_scale !== undefined) edits.push(edit(AT_RENDER_SCALE, p.render_scale > 0 ? String(p.render_scale) : 'auto'));
  if (p.fps_limit !== undefined) edits.push(edit(AT_FPS_LIMIT, p.fps_limit >= 0 ? String(p.fps_limit) : 'auto'));
  return edits;
}

/** quality_preset_edited() of play_types.cpp: the render scale and the frame limit only belong to the device presets. */
function markCustom(kind, src) {
  const cur = word(has(src, AT_PRESET) ? rawOf(src, AT_PRESET) : PRESET.def);
  if (cur === 'custom') return [];
  const setsFrame = QUALITY_PRESETS.find((q) => q.id === cur)?.render_scale !== undefined;
  return kind === 'frame' && !setsFrame ? [] : [edit(AT_PRESET, 'custom')];
}

// ---- volume and mouse ---------------------------------------------------------------------------------------------------------------

const VOLUME = fromSchema(controls('audio', 'volume'));
const AT_VOLUME = VOLUME.at;
const AT_VOLUME_PKP = pkp('audio', 'volume');

/** [audio] volume in pkp.ini beats controls.ini and undoes the F1 slider, so what it says is what plays. Shown as 0 to 100. */
function readVolume(src) {
  const at = [AT_VOLUME_PKP, AT_VOLUME].find((a) => has(src, a));
  const def = parseFloat(VOLUME.def);
  const v = at ? parseFloat(rawOf(src, at)) : def;
  return Math.round(clamp(Number.isFinite(v) ? v : def, VOLUME.min, VOLUME.max) * 100);
}

function writeVolume(value, src) {
  const text = String(value / 100);
  return has(src, AT_VOLUME_PKP) ? [edit(AT_VOLUME, text), edit(AT_VOLUME_PKP, text)] : [edit(AT_VOLUME, text)];
}

export const SOURCE_RAD = (0.022 * Math.PI) / 180;   // kSourceRadPerCount: radians per mouse count at sensitivity 1
const YAW = fromSchema(controls('mouse', 'yaw'));
const AT_YAW = YAW.at;
const AT_PITCH = fromSchema(controls('mouse', 'pitch')).at;
// The slider's own range on the Source scale (the file takes any number); the default is the runtime's, 2 at 0.000767945 rad.
const SENS_MIN = 0.1;
const SENS_MAX = 20;
const SENS_DEFAULT = Math.round((parseFloat(YAW.def) / SOURCE_RAD) * 10) / 10;

function readSensitivity(src) {
  const rad = parseFloat(rawOf(src, AT_YAW));
  return Math.round(clamp(Number.isFinite(rad) ? rad / SOURCE_RAD : SENS_DEFAULT, SENS_MIN, SENS_MAX) * 10) / 10;
}

function writeSensitivity(value) {
  const rad = (clamp(value, SENS_MIN, SENS_MAX) * SOURCE_RAD).toPrecision(9);
  return [edit(AT_YAW, rad), edit(AT_PITCH, rad)];
}

// ---- the plain mapping of each type -------------------------------------------------------------------------------------------------

const CLOCK_LIMIT_MIN = 10 * 366 * 24 * 60;
const clockMinutes = (value) => {
  const n = Math.round(Number(String(value).trim()));
  return Number.isFinite(n) ? clamp(n, -CLOCK_LIMIT_MIN, CLOCK_LIMIT_MIN) : 0;
};

const AT_CLOCK_OFFSET = pkp('time', 'clock_offset');
const AT_CLOCK_SCOPE = pkp('time', 'clock_scope');
const AT_EVENT_PRESET = pkp('time', 'event_preset');
const TIMED_EVENTS = [
  { id: 'copycat', label: t('Copycat Challenge (April 2, 2026)') },
  { id: 'hoppip', label: t('More Spores for Hoppip (March 12, 2026)') },
  { id: 'bulbasaur', label: t("Bulbasaur's Jump Rope Contest (April 21, 2026)") },
  { id: 'sableye', label: t("Sableye's Gem Hunt (May 1, 2026)") },
];
const clockScope = (src) => (has(src, AT_CLOCK_SCOPE) && word(rawOf(src, AT_CLOCK_SCOPE)) === 'all' ? 'all' : 'user');
const scopeNeedsReset = (src) => clockScope(src) === 'all';
const activeEvent = (src) => {
  const id = has(src, AT_EVENT_PRESET) ? word(rawOf(src, AT_EVENT_PRESET)) : 'off';
  return TIMED_EVENTS.some((e) => e.id === id) ? id : 'off';
};

function clockEdits(value, src) {
  const edits = [edit(AT_CLOCK_OFFSET, String(clockMinutes(value)))];
  // Do not add the default key to an older file; only clear a scope left by the earlier preset implementation.
  if (scopeNeedsReset(src)) edits.push(edit(AT_CLOCK_SCOPE, 'user'));
  return edits;
}

const wordOf = (f, src) => {
  const v = word(has(src, f.at) ? rawOf(src, f.at) : f.def);
  return f.aliases?.[v] ?? v;
};

const READ = {
  segmented: wordOf,
  select: wordOf,
  choice: wordOf,
  size: (f, src) => atsOf(f).map((at, i) => (has(src, at) ? String(rawOf(src, at)).trim() : f.def.split('x')[i])).join('x'),
  toggle: (f, src) => truthy(has(src, f.at) ? rawOf(src, f.at) : f.def),
  slider: (f, src) => {
    const v = parseFloat(has(src, f.at) ? rawOf(src, f.at) : f.def);
    const near = Math.round(clamp(Number.isFinite(v) ? v : f.def, f.min, f.max) / f.step) * f.step;
    return +near.toFixed(2);
  },
  path: (f, src) => String(rawOf(src, f.at) ?? ''),
  clock: (f, src) => clockMinutes(has(src, f.at) ? rawOf(src, f.at) : f.def),
};

const WRITE = {
  segmented: (f, v) => [edit(f.at, String(v))],
  select: (f, v) => [edit(f.at, String(v))],
  choice: (f, v) => [edit(f.at, String(v))],
  size: (f, v) => String(v).split('x').map((part, i) => edit(atsOf(f)[i], part)),
  toggle: (f, v) => [edit(f.at, f.at.file === 'config' ? !!v : v ? '1' : '0')],
  slider: (f, v) => [edit(f.at, String(v))],
  path: (f, v) => [edit(f.at, fwd(v))],
  clock: (f, v) => [edit(f.at, String(clockMinutes(v)))],
};

// ---- the page ---------------------------------------------------------------------------------------------------------------------

const REPO_URL = 'https://github.com/featureJosh/pokopia-recomp';
const DISCORD_URL = 'https://discord.recompia.com';
const open = (url, label = t('Open')) => ({ act: 'open-url', arg: url, label, icon: 'external-link' });
const hasControls = (src) => src.controls_exists !== false;

const WIDTH = fromSchema(controls('display', 'width'));
const HEIGHT = fromSchema(controls('display', 'height'));
const WINDOW_SIZE = {
  at: [WIDTH.at, HEIGHT.at], def: `${WIDTH.def}x${HEIGHT.def}`, min: Math.max(WIDTH.min, HEIGHT.min), max: Math.min(WIDTH.max, HEIGHT.max),
};
// ---- gameplay mods ------------------------------------------------------------------------------------------------------------------
// The runtime marks its gameplay mods: [pkp] keys in the schema's "gameplay" group (runtime/games/pkp/core/pkp_settings_table.h). A mod's
// switch is a bool, on by default unless the schema says opt_in; a key with a "parent" is a setting of that mod, shown under its switch
// while the switch is on; "hidden" keys get no row. Every row of Settings > Gameplay comes from that list, in the runtime's order; the
// launcher only names them here (label, the options of a number, notes that follow a row). A gameplay key without an entry here throws,
// so a new mod cannot ship without its row (scripts/test-settings-schema.mjs runs this).
const isOn = (id) => (v) => v[id] === true || v[id] === '1';
const GAMEPLAY = {
  jump_momentum: { label: t('Running jump keeps speed') },
  camera_aim: {
    label: t('Camera aim'),
    after: [{ id: 'camera_aim_note', type: 'note', when: isOn('camera_aim'), note: () => ({ text: t('Abilities and tools aim where the camera looks instead of where your character faces, and your character turns to the target when the ability starts.') }) }],
  },
  camera_aim_reach: { label: t('Extra reach in blocks'), options: (k) => Array.from({ length: k.max - k.min + 1 }, (_, n) => n + k.min).map((n) => ({ value: String(n), label: n === 0 ? t('Off') : num(n) })) },
  inventory_sort: { label: t('Sort button in the bag and containers') },
  inventory_autosort: { label: t('Sort each time the bag opens') },
  craft_search: { label: t('Search in the crafting screen') },
  item_editor: {
    label: t('Item editor (experimental)'),
    after: [{ id: 'item_editor_note', type: 'note', when: isOn('item_editor'), note: () => ({ text: t('Adds an Items tab to the game\'s Settings screen where you change how many of an item in your bag you hold. Off during Link Play. Back up your save first.') }) }],
  },
  pokemon_cap_50: {
    label: t('More Pokémon in town'),
    after: [{ id: 'pokemon_cap_note', type: 'note', when: isOn('pokemon_cap_50'), note: () => ({ text: t('Experimental. The 64 actor slots are shared by residents and visitors; 50 residents leaves 14 slots for visitors. Above 50, visitor space is limited.') }) }],
  },
  pokemon_cap_limit: { label: t('Pokémon in town'), options: (k) => Array.from({ length: k.max - k.min + 1 }, (_, n) => n + k.min).map((n) => ({ value: String(n), label: num(n) })) },
};

const gameplaySchema = () => SCHEMAS.pkp.sections.find((s) => s.name === 'pkp').keys.filter((k) => k.group === 'gameplay');
/** Every gameplay key of the runtime: switches and their settings, shown or hidden. */
export const gameplayKeys = () => gameplaySchema().map((k) => k.key);
/** A mod's switch (not the setting of another switch). */
export const isGameplaySwitch = (k) => k.group === 'gameplay' && !k.parent;

function gameplayRow(k) {
  const g = GAMEPLAY[k.key];
  if (!g) throw new Error(`settings-schema: gameplay mod [pkp] ${k.key} has no row in GAMEPLAY`);
  const at = fromSchema(pkp('pkp', k.key));
  const row = { id: k.key, label: g.label, ...at };
  if (k.type === 'bool') row.type = 'toggle';
  else if (g.options) Object.assign(row, { type: 'select', options: g.options(k) });
  else throw new Error(`settings-schema: gameplay setting [pkp] ${k.key} (${k.type}) needs options in GAMEPLAY`);
  if (k.parent) row.when = isOn(k.parent);
  return [row, ...(g.after ?? [])];
}

function gameplayRows() {
  const keys = gameplaySchema().filter((k) => !k.hidden);
  return keys.filter(isGameplaySwitch).flatMap((sw) => [
    ...gameplayRow(sw),
    ...keys.filter((k) => k.parent === sw.key).flatMap(gameplayRow),
  ]);
}

export const DLSS_DOWNLOAD = {
  title: t('Download DLSS?'),
  html: `<p>NVIDIA licenses this file separately, so it does not come with the launcher. It comes from NVIDIA's DLSS repository on GitHub, and its version and hash are checked.</p>
    <p>By downloading it you accept the NVIDIA RTX SDKs licence: <code class="selectable">licenses/LICENSE.NVIDIA-RTX-SDKs.txt</code></p>`,
  ok: t('Download'),
};

export const IMPORT_FACTS = t('Your current save moves to a backup folder first, and the source is only read. A copied save cannot join its original in Link Play.');

export const SECTIONS = [
  {
    id: 'display',
    label: t('Display'),
    groups: [
      {
        title: t('Picture'),
        rows: [
          {
            id: 'window_mode', label: t('Window'), type: 'segmented', ...fromSchema(controls('display', 'window_mode')),
            options: [{ value: 'windowed', label: t('Windowed') }, { value: 'borderless', label: t('Borderless') }, { value: 'exclusive', label: t('Fullscreen') }],
          },
          {
            id: 'window_size', label: t('Window size'), type: 'size', ...WINDOW_SIZE, when: (v) => v.window_mode === 'windowed',
            options: [
              { value: '1280x720', label: t('1280 x 720') }, { value: '1600x900', label: t('1600 x 900') }, { value: '1920x1080', label: t('1920 x 1080') },
              { value: '2560x1440', label: t('2560 x 1440') }, { value: '3840x2160', label: t('3840 x 2160') },
            ],
          },
          {
            // 100 is 1920 x 1080: the percentages are the game's own size, see render_scale in play_types.h
            id: 'render_scale', label: t('Render resolution'), type: 'choice', at: AT_RENDER_SCALE, def: RENDER_SCALE.def, quality: 'frame',
            custom: { min: RENDER_SCALE.min, max: RENDER_SCALE.max, suffix: '%', start: 100, label: t('Custom render resolution in percent') },
            options: [
              { value: 'auto', label: t('Match window') }, { value: '67', label: t('720p') }, { value: '100', label: t('1080p') },
              { value: '133', label: t('1440p') }, { value: '200', label: t('2160p') },
            ],
          },
          {
            // [display] aspect: the runtime writes a preset back as W:H (write_aspect in play_config_table.cpp)
            id: 'aspect', label: t('Aspect ratio'), type: 'select', ...fromSchema(controls('display', 'aspect')),
            options: [
              { value: 'auto', label: t('Automatic') }, { value: '16:9', label: t('16:9') }, { value: '16:10', label: t('16:10') },
              { value: '21:9', label: t('21:9') }, { value: '32:9', label: t('32:9') },
            ],
          },
          { id: 'keep_aspect', label: t('Letterbox instead of stretching'), type: 'toggle', ...fromSchema(controls('display', 'keep_aspect')) },
        ],
      },
      {
        title: t('Frame rate'),
        rows: [
          {
            id: 'fps_limit', label: t('Frame rate limit'), type: 'choice', at: AT_FPS_LIMIT, def: FPS_LIMIT.def, quality: 'frame',
            custom: { min: FPS_LIMIT.min, max: FPS_LIMIT.max, suffix: 'fps', start: 60, label: t('Custom frame rate limit') },
            options: [
              { value: 'auto', label: t('Match display') }, { value: '30', label: t('30') }, { value: '60', label: t('60') },
              { value: '120', label: t('120') }, { value: '144', label: t('144') }, { value: '0', label: t('Unlimited') },
            ],
          },
          { id: 'vsync', label: t('VSync'), type: 'toggle', ...fromSchema(controls('display', 'vsync')) },
          { id: 'fps_show', label: t('Frame rate counter'), type: 'toggle', ...fromSchema(controls('fps', 'show')) },
        ],
      },
    ],
  },
  {
    id: 'graphics',
    label: t('Graphics'),
    groups: [
      {
        title: t('Quality'),
        rows: [
          {
            id: 'quality_preset', label: t('Quality preset'), type: 'select', ...PRESET,
            options: QUALITY_PRESETS.map((q) => ({ value: q.id, label: q.label })),
            write: (value) => presetEdits(value), replaces: true,
          },
        ],
      },
      {
        title: t('Upscaling'),
        rows: [
          {
            // spatial is the runtime's other word for fsr1. ponytail: fsr3 / nis / dlss / dlaa / xess are switched off in the runtime until
            // they are reworked (#232) and run as off; add their options back here (and the dlss guard) when they return.
            id: 'upscale_mode', label: t('Upscaler'), type: 'select', ...fromSchema(controls('upscale', 'mode')),
            aliases: { spatial: 'fsr1', xess: 'off', fsr3: 'off', nis: 'off', dlss: 'off', dlaa: 'off' }, lock: (env) => env.dlssBusy,
            options: [{ value: 'off', label: t('Off') }, { value: 'fsr1', label: t('FSR 1') }, { value: 'nearest', label: t('Nearest (sharp pixels)') }],
          },
          {
            id: 'upscale_quality', label: t('Upscaling quality'), type: 'select', ...fromSchema(controls('upscale', 'quality')),
            when: (v) => ['fsr3', 'dlss'].includes(v.upscale_mode),
            options: [
              { value: 'native', label: t('Native') }, { value: 'quality', label: t('Quality') }, { value: 'balanced', label: t('Balanced') },
              { value: 'performance', label: t('Performance') }, { value: 'ultra_performance', label: t('Ultra performance') },
            ],
          },
          {
            id: 'dlss_file', label: t('DLSS file'), type: 'info', value: (env) => (env.dlssPresent ? t('Downloaded') : t('Missing')),
            when: (v, env) => ['dlss', 'dlaa'].includes(v.upscale_mode) && env.dlssCanFetch,
            actions: (env) => [{ act: 'dlss-get', label: env.dlssPresent ? t('Download again') : t('Download'), icon: 'download', disabled: env.dlssBusy }],
          },
        ],
      },
      { title: '', rows: [{ id: 'f1_graphics', label: t('More options in game'), type: 'keycap', key: 'F1' }] },
    ],
  },
  {
    id: 'audio',
    label: t('Audio'),
    groups: [
      {
        title: '',
        rows: [
          {
            id: 'volume', label: t('Volume'), type: 'slider', at: AT_VOLUME, min: VOLUME.min * 100, max: VOLUME.max * 100, step: 1, suffix: '%',
            read: readVolume, write: writeVolume, enabled: (src) => hasControls(src) || has(src, AT_VOLUME_PKP),
          },
          { id: 'mute_unfocused', label: t('Mute when not focused'), type: 'toggle', ...fromSchema(controls('audio', 'mute_unfocused')) },
        ],
      },
    ],
  },
  {
    id: 'controls',
    label: t('Controls'),
    groups: [
      {
        title: t('Mouse'),
        rows: [
          { id: 'sensitivity', label: t('Mouse sensitivity'), type: 'slider', at: AT_YAW, min: SENS_MIN, max: SENS_MAX, step: 0.1, read: readSensitivity, write: writeSensitivity },
          { id: 'invert_x', label: t('Invert horizontal'), type: 'toggle', ...fromSchema(controls('mouse', 'invert_x')) },
          { id: 'invert_y', label: t('Invert vertical'), type: 'toggle', ...fromSchema(controls('mouse', 'invert_y')) },
        ],
      },
      {
        title: t('Buttons'),
        rows: [
          {
            id: 'prompt_style', label: t('Button icons'), type: 'select', ...fromSchema(controls('prompts', 'style')),
            aliases: PROMPT_ALIASES,
            options: [
              { value: 'auto', label: t('Automatic') }, { value: 'switch', label: t('Switch') }, { value: 'xbox', label: t('Xbox') },
              { value: 'playstation', label: t('PlayStation') }, { value: 'steam', label: t('Steam Controller') }, { value: 'steamdeck', label: t('Steam Deck') },
              { value: 'switch1', label: t('Nintendo Switch') }, { value: 'switch2', label: t('Nintendo Switch 2') }, { value: 'wiiu', label: t('Wii U') },
              { value: 'steamframe', label: t('Steam Frame') }, { value: 'index', label: t('Valve Index') }, { value: 'touch', label: t('Touch') },
              { value: 'keyboard', label: t('Keyboard and mouse') },
            ],
          },
          { id: 'prompt_preview', label: t('Preview'), type: 'glyphs', glyphs: (v) => promptPreview(v.prompt_style) },
          { id: 'f1_bindings', label: t('Change bindings in game'), type: 'keycap', key: 'F1' },
        ],
      },
    ],
  },
  {
    id: 'saves',
    label: t('Saves'),
    groups: [
      {
        title: t('Profiles'),
        rows: [
          { id: 'profiles_note', type: 'note', note: () => ({ text: t('Each profile has its own save, so two people can share one install. They share the game files and the shaders. Switch profiles with the game closed.') }) },
          {
            expand: (env) => (env.profiles?.profiles ?? []).map((p) => {
              const on = p.id === env.profiles.active;
              return {
                id: `profile_${p.id}`, label: p.name, type: 'info', value: on ? t('Playing') : '',
                actions: [
                  ...(on ? [] : [{ act: 'profile-use', arg: p.id, name: p.name, label: t('Use'), icon: 'user-check', disabled: env.gameBusy }]),
                  { act: 'profile-rename', arg: p.id, name: p.name, label: t('Rename'), icon: 'pencil', disabled: env.gameBusy },
                  ...(p.id === 'default' ? [] : [{ act: 'profile-remove', arg: p.id, name: p.name, label: t('Remove'), icon: 'trash-2', disabled: env.gameBusy || on }]),
                ],
              };
            }),
          },
          {
            id: 'profile_new', label: t('New profile'), type: 'info',
            actions: (env) => [{ act: 'profile-new', label: t('Add'), icon: 'plus', disabled: env.gameBusy }],
          },
        ],
      },
      {
        title: '',
        rows: [{
          id: 'save_dir', label: t('Save folder'), type: 'path', at: pkp('fs', 'save_dir'), pick: t('Choose the save folder'), open: 'save',
          placeholder: (env) => `${env.gameDir}/save`,
        }],
      },
      {
        title: t('Backups'),
        rows: [
          {
            id: 'backup_note', type: 'note',
            note: () => ({ text: t('Copies your save every few minutes while you play, so you can go back if a bug stops your progress. It is off until you pick a time. A copy is also made when the game closes. Restoring needs the game closed, and your current save is copied first.') }),
          },
          {
            id: 'backup_minutes', label: t('Back up automatically'), type: 'select', at: pkp('backup', 'interval_minutes'), def: '0',
            options: [{ value: '0', label: t('Off') }, { value: '5', label: t('Every 5 minutes') }, { value: '10', label: t('Every 10 minutes') }, { value: '15', label: t('Every 15 minutes') }],
          },
          {
            id: 'backup_keep', label: t('Backups to keep'), type: 'select', at: pkp('backup', 'keep'), def: '10', when: (v) => v.backup_minutes !== '0',
            options: [{ value: '5', label: t('5') }, { value: '10', label: t('10') }, { value: '20', label: t('20') }, { value: '30', label: t('30') }],
          },
          {
            id: 'backup_state', label: t('Backups'), type: 'info', value: (env) => env.backupLine,
            actions: (env) => [
              { act: 'backup-now', label: t('Back up now'), icon: 'save' },
              { act: 'backup-restore', label: t('Restore'), icon: 'rotate-ccw', disabled: env.gameBusy || !env.backups?.count },
              { act: 'backup-open', label: t('Open folder'), icon: 'folder-open' },
            ],
          },
          { id: 'backup_detail', type: 'note', when: (v, env) => !!env.backupDetail, note: (env) => ({ text: env.backupDetail }) },
        ],
      },
      {
        title: t('Import a save'),
        rows: [
          {
            expand: (env) => env.saveSources.map((s, i) => ({
              id: `source_${i}`, label: s.label, type: 'info', value: s.path, mono: true,
              actions: [{ act: 'import', arg: s.path, name: s.label, label: t('Import'), icon: 'upload', disabled: env.gameBusy || env.importing }],
            })),
          },
          {
            id: 'import_folder', label: t('From a folder'), type: 'info',
            actions: (env) => [{ act: 'import-folder', label: t('Choose a folder'), icon: 'folder-open', disabled: env.gameBusy || env.importing }],
          },
          { id: 'import_result', type: 'note', when: (v, env) => !!env.importResult, note: (env) => env.importResult },
        ],
      },
    ],
  },
  {
    id: 'game',
    label: t('Game'),
    groups: [
      {
        title: '',
        rows: [
          {
            id: 'game_files', label: t('Game files'), type: 'info', value: (env) => env.gameFiles, mono: true,
            actions: [{ act: 'change-files', label: t('Change'), icon: 'folder-open' }],
          },
          {
            id: 'setup_again', label: t('Set up again'), type: 'info',
            actions: (env) => [{ act: 'setup-again', label: t('Set up'), icon: 'wrench', disabled: !env.canSetUp }],
          },
          { id: 'setup_log', label: t('Setup log'), type: 'info', actions: [{ act: 'setup-log', label: t('Open'), icon: 'file-text' }] },
        ],
      },
      {
        title: t('Gameplay'),
        rows: [
          { id: 'gameplay_note', type: 'note', note: () => ({ text: t('New ways of playing: switch each one on or off here. They take effect the next time the game starts, and they need the game built with this version (Set up again once after an update).') }) },
          ...gameplayRows(),
        ],
      },
      {
        title: t('Experimental'),
        rows: [
          // pkp.ini [display] unpaced_loading, read by runtime/src/gfx/nvn/nvn_replay.cpp; that section has no generated schema yet
          { id: 'unpaced_loading', label: t('Fast loading screens'), type: 'toggle', at: pkp('display', 'unpaced_loading'), def: '0' },
          { id: 'unpaced_loading_note', type: 'note', note: () => ({ text: t('Lets loading screens run past your frame limit, so loading ends a little sooner. Their animations then play too fast above 60 fps. Takes effect the next time the game starts.') }) },
        ],
      },
      {
        title: t('Logo and icon'),
        rows: [
          // launcher.json; the launcher puts the choice in place (writer.js -> apply_branding, src-tauri/src/branding.rs)
          { id: 'logo', label: t('Logo'), type: 'segmented', at: config('logo'), def: 'recompia', options: LOOKS },
          { id: 'icon', label: t('Icon'), type: 'segmented', at: config('icon'), def: 'recompia', options: LOOKS },
          { id: 'look_note', type: 'note', note: () => ({ text: t('Classic is the bubble-letter logo and R icon of earlier versions. The icon changes for the launcher, the game and the Steam shortcut (Steam shows it after a restart). Windows can take a moment to redraw an icon it has already shown.') }) },
        ],
      },
      {
        title: t('Steam'),
        rows: [
          {
            id: 'steam_note', type: 'note',
            note: (env) => ({ text: env.steam && !env.steam.found
              ? t('Steam was not found on this PC, or nobody has signed in to it yet. Start Steam once, sign in, close it, and come back.')
              : t('Adds a shortcut with the Recompia artwork to your Steam library, so it shows in Game Mode and Big Picture. Close Steam first, then start it again afterwards. Add the game to start playing at once, or the launcher to open this window first.') }),
          },
          {
            id: 'steam_game', label: t('Game in Steam'), type: 'info', when: (v, env) => !!env.steam?.found,
            value: (env) => (env.steam?.game_added ? t('In Steam as {name}', { name: env.steam.game_name }) : t('Not in Steam. Starts the game at once.')),
            actions: (env) => [env.steam?.game_added
              ? { act: 'steam-remove-game', label: t('Remove'), icon: 'trash-2', disabled: env.steamBusy || env.steam?.running }
              : { act: 'steam-add-game', label: t('Add to Steam'), icon: 'plus', disabled: env.steamBusy || env.steam?.running }],
          },
          {
            id: 'steam_launcher', label: t('Launcher in Steam'), type: 'info', when: (v, env) => !!env.steam?.found,
            value: (env) => (env.steam?.launcher_added ? t('In Steam as {name}', { name: env.steam.launcher_name }) : t('Not in Steam. Opens this window.')),
            actions: (env) => [env.steam?.launcher_added
              ? { act: 'steam-remove-launcher', label: t('Remove'), icon: 'trash-2', disabled: env.steamBusy || env.steam?.running }
              : { act: 'steam-add-launcher', label: t('Add to Steam'), icon: 'plus', disabled: env.steamBusy || env.steam?.running }],
          },
          { id: 'steam_running', type: 'note', when: (v, env) => !!env.steam?.running, note: () => ({ text: t('Steam is running. Close it (Steam, Exit) to add or remove a shortcut.') }) },
        ],
      },
      {
        title: t('Shader files'),
        rows: [
          { id: 'shader_storage', label: t('Storage'), type: 'info', value: storageText, actions: storageActions },
          { id: 'shader_storage_note', type: 'note', when: (v, env) => !!storageNote(env), note: storageNote },
        ],
      },
      {
        title: '',
        rows: [
          {
            // saved on leaving the field, not while typing: half a path would make the whole launcher think the game is gone
            id: 'game_dir', label: t('Install folder'), type: 'path', at: config('game_dir'), pick: t('Choose the install folder'), open: 'game',
            placeholder: (env) => env.defaultGameDir, commit: 'blur', reloads: true,
          },
          {
            id: 'romfs_mod', label: t('Mods'), type: 'path', at: pkp('fs', 'romfs_mod'), pick: t('Choose the mods folder'),
            placeholder: () => '',
          },
          {
            id: 'romfs_mod_note', type: 'note',
            note: () => ({ text: t("Choose the folder that holds the same files and folders as the game's romfs (for example LD_C0_R.BIN at its top level), not the folder around it. Its files replace the game's files with the same path.") }),
          },
          { id: 'minimize_on_play', label: t('Minimise while playing'), type: 'toggle', at: config('minimize_on_play'), def: false },
          { id: 'discord_presence', label: t('Show on Discord while playing'), type: 'toggle', at: config('discord_presence'), def: false },
        ],
      },
      {
        title: t('Game clock'),
        rows: [
          ...TIMED_EVENTS.map((event) => ({
            id: `event_${event.id}`, label: event.label, type: 'toggle', at: AT_EVENT_PRESET, def: false,
            read: (src) => activeEvent(src) === event.id,
            write: (on) => [edit(AT_EVENT_PRESET, on ? event.id : 'off')], replaces: true,
          })),
          {
            id: 'game_clock', label: t('Date and time in the game'), type: 'clock', at: AT_CLOCK_OFFSET, def: '0', commit: 'blur',
            write: (value, src) => clockEdits(value, src), writeSame: scopeNeedsReset, enabled: (src) => activeEvent(src) === 'off',
          },
          {
            id: 'game_clock_note', type: 'note',
            note: () => ({ text: t('Use this in single player only for now. Play applies the enabled event\'s start date automatically. Your ordinary Game clock is kept and used again when all events are off.') }),
          },
        ],
      },
    ],
  },
  {
    id: 'about',
    label: t('About'),
    groups: [
      {
        title: '',
        rows: [
          { id: 'version', label: t('Version'), type: 'info', value: (env) => env.version, selectable: true },
          {
            id: 'channel', label: t('Update channel'), type: 'select', at: config('channel'), def: 'stable', guard: 'testing',
            lock: (env) => env.updating || env.gameBusy,
            options: [{ value: 'stable', label: t('Stable') }, { value: 'testing', label: t('Testing') }],
          },
          {
            id: 'channel_note', type: 'note', when: (v) => v.channel === 'testing',
            note: () => ({ text: t('Testing builds come first and may have bugs. Testing does not update by itself: pick each build with Choose a version below. As a tester you are expected to tell us what you find and share pkp.log: use Report a problem below. Your saves and settings are kept. To go back to Stable, wait until Stable has a newer version than the one you have, or install it from the releases page.') }),
          },
          {
            id: 'testing_help', label: t('Testing help'), type: 'info', when: (v) => v.channel === 'testing',
            value: t('What to test is listed with each Testing build in What\'s New.'),
            actions: () => [
              { act: 'open-url', arg: `${REPO_URL}/issues/new?template=bug_report.yml`, label: t('Report a problem'), icon: 'external-link' },
              { act: 'open-log', label: t('Show pkp.log'), icon: 'file-text' },
            ],
          },
          {
            id: 'builds', label: t('Versions'), type: 'info', value: (env) => t('You have {version}', { version: env.version }),
            actions: (env) => [{ act: 'builds-open', label: t('Choose a version'), icon: 'download', disabled: env.gameBusy || env.updating || env.taskBusy }],
          },
          {
            id: 'updates', label: t('Updates'), type: 'info', value: (env) => env.updateText,
            actions: (env) => [
              ...(env.canUpdate ? [{ act: 'update', label: t('Update'), icon: 'download', primary: true }] : []),
              { act: 'check-updates', label: t('Check for updates'), icon: 'refresh-cw', disabled: env.checking },
            ],
          },
          // not a file setting: the language is per PC like the colours (i18n.js), so the row has no `at` and pages/settings.js hands the pick to setLanguage
          { id: 'language', label: t('Language'), type: 'language' },
          { id: 'licence', label: t('Licence'), type: 'info', value: 'GPL-3.0-or-later' },
          { id: 'built_with', label: t('Built with'), type: 'info', value: t('Tauri 2, Lucide (ISC), Inter (OFL)') },
          { id: 'repo', label: t('Source code'), type: 'info', actions: [open(REPO_URL, 'GitHub')] },
          { id: 'community', label: t('Community'), type: 'info', value: t('Recompia Discord: help, testing and chat'), actions: [open(DISCORD_URL, t('Join Discord'))] },
          { id: 'koi-credit', label: "Koi's Camp", type: 'info', value: t('Artwork by Koi, hand drawn') },
          { id: 'meadow-credit', label: 'Meadow', type: 'info', value: t('Original artwork made for the project') },
          { id: 'bug', label: t('Report a bug'), type: 'info', actions: [open(`${REPO_URL}/issues/new/choose`)] },
        ],
      },
      {
        title: t('Troubleshooting'),
        rows: [
          {
            id: 'repair', label: t('Repair and reset'), type: 'info',
            value: t('Settings, caches and old copies. You see what changes first.'),
            actions: (env) => [{ act: 'repair-open', label: t('Repair and reset'), icon: 'wrench', disabled: env.gameBusy || env.updating || env.taskBusy }],
          },
          {
            id: 'without_mods', label: t('Start without mods'), type: 'info',
            value: t('Starts the game once without mods. Your mods and their settings stay as they are.'),
            actions: (env) => [{ act: 'play-without-mods', label: t('Start without mods'), icon: 'play', disabled: !env.canPlay }],
          },
          {
            id: 'repair_undo', label: t('Undo last repair'), type: 'info', when: (v, env) => !!env.repairBackup,
            value: (env) => (env.repairBackup ? t('Puts back {files}', { files: env.repairBackup.files.join(', ') }) : ''),
            actions: (env) => [{ act: 'repair-undo', label: t('Undo'), icon: 'rotate-ccw', disabled: env.gameBusy || env.updating || env.taskBusy }],
          },
        ],
      },
      {
        title: '',
        rows: [{
          id: 'legal', type: 'note',
          note: () => ({ text: t('No game files are included. You need your own copy of the game, from a console you own. This project is not affiliated with or endorsed by Nintendo, The Pokemon Company or Koei Tecmo.') }),
        }],
      },
    ],
  },
];

export const FIELDS = Object.fromEntries(
  SECTIONS.flatMap((s) => s.groups.flatMap((g) => g.rows)).filter((r) => r.at).map((f) => [f.id, f]),
);

// ---- reading and writing ------------------------------------------------------------------------------------------------------------

export const makeSource = (dump, cfg) => ({ pkp: dump.pkp, controls: dump.controls, config: cfg, controls_exists: dump.controls_exists });

export const readField = (f, src) => (f.read ? f.read(src) : READ[f.type](f, src));

export const readAll = (src) => Object.fromEntries(Object.values(FIELDS).map((f) => [f.id, readField(f, src)]));

/**
 * What it takes to give field `f` this value: nothing when that is what it already reads as, and never a key that already holds the
 * text or a file that does not exist. Reading and writing back is therefore no edit at all, so a value the page cannot show survives.
 */
export function writeField(f, value, src) {
  if (String(readField(f, src)) === String(value) && !f.writeSame?.(src)) return [];
  const edits = f.write ? f.write(value, src) : WRITE[f.type](f, value);
  if (f.quality) edits.push(...markCustom(f.quality, src));
  return edits.filter((e) => !(e.file === 'controls' && src.controls_exists === false) && String(rawOf(src, e)) !== String(e.value));
}

/** The source as it reads after the edits. */
export function applyEdits(src, edits) {
  const next = { ...src, pkp: { ...src.pkp }, controls: { ...src.controls }, config: { ...src.config } };
  for (const e of edits) next[e.file][keyOf(e)] = e.value;
  return next;
}

/** { ini: edits for write_settings, config: the keys for save_config, or null } */
export function splitEdits(edits) {
  const own = edits.filter((e) => e.file === 'config');
  return { ini: edits.filter((e) => e.file !== 'config'), config: own.length ? Object.fromEntries(own.map((e) => [e.key, e.value])) : null };
}

// ---- what a control shows ---------------------------------------------------------------------------------------------------------

/** The option a value selects, 'custom' when it is none of them. Never changes the value itself. */
export function shownChoice(f, value) {
  return f.options.some((o) => o.value === String(value)) ? String(value) : 'custom';
}

/** The options to offer. Custom is a real choice on a number control, and on a plain list only the name of a value it cannot show. */
export function optionsFor(f, value, env = {}) {
  const list = f.options.filter((o) => !o.offer || o.offer(env) || o.value === value).map(({ value: v, label }) => ({ value: v, label }));
  if (f.type === 'choice' || f.type === 'size') return [...list, { value: 'custom', label: t('Custom') }];
  return list.some((o) => o.value === value) ? list : [...list, { value: 'custom', label: t('Custom'), disabled: true }];
}

/** What a number field starts from when Custom is picked: the value when it is a number, else the control's starting point. */
export function customStart(f, value) {
  return /^\d+$/.test(String(value)) ? String(value) : String(f.custom.start);
}

/** A whole number from a number field, kept in range; null when the text is not a number. */
export function fitNumber(text, min, max) {
  const n = Math.round(Number(String(text).trim() || NaN));
  return Number.isFinite(n) ? clamp(n, min, max) : null;
}

export const isVisible = (row, values, env) => (row.when ? row.when(values, env) : true);

/** The file is there. A field in controls.ini waits for the game to write it: the launcher never creates that file (settings.rs apply). */
export function isAvailable(f, src) {
  if (f.enabled) return f.enabled(src);
  return !atsOf(f).some((at) => at.file === 'controls') || src.controls_exists !== false;
}

export const isEnabled = (f, src, env) => isAvailable(f, src) && !f.lock?.(env);

/** The rows of a group with `expand` entries unfolded and hidden rows gone. */
export const rowsOf = (group, values, env) => group.rows.flatMap((r) => (r.expand ? r.expand(env) : [r])).filter((r) => isVisible(r, values, env));
