// Checks the Settings page (src/settings-schema.js) against the runtime's generated schemas (runtime/docs/controls_schema.json,
// runtime/docs/pkp_ini_schema.json; runtime/scripts/gen_settings_doc.ps1 writes them from the runtime's settings tables):
//   - every controls.ini key and every pkp.ini [pkp] key a row reads or writes is in the schema, not retired and not read only;
//   - a row's default is the schema's (the runtime's) default;
//   - every option a row offers is a value the runtime takes (one of its words, its auto word, or a number in its range);
//   - the quality presets are the runtime's preset words and their numbers are in the keys' ranges.
// pkp.ini sections other than [pkp] ([fs], [backup], [time], [audio]) have no table and are not checked.
// Run: node scripts/test-settings-schema.mjs   (exit 1 on a mismatch)
import { FIELDS, QUALITY_PRESETS, SCHEMAS, gameplayKeys, isGameplaySwitch, schemaKey, isRetired } from '../src/settings-schema.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };
const name = (at) => `${at.file} [${at.section}] ${at.key}`;
const described = (at) => at.file === 'controls' || (at.file === 'pkp' && SCHEMAS.pkp.sections.some((s) => s.name === at.section));
const word = (v) => String(v).trim().toLowerCase().replace(/-/g, '_');

/** A value the runtime accepts for this key (the launcher's spellings: words lower case). */
function accepts(k, value) {
  const v = word(value);
  if (k.choices) return k.choices.includes(v);
  if (k.auto && v === k.auto) return true;
  if (k.type === 'bool') return ['0', '1', 'true', 'false'].includes(v);
  if (k.type === 'int' || k.type === 'real') {
    const n = Number(String(value).trim());
    if (!Number.isFinite(n) || (k.type === 'int' && !Number.isInteger(n))) return false;
    if (k.zero_ok && n === 0) return true;
    return (k.min === undefined || n >= k.min) && (k.max === undefined || n <= k.max);
  }
  return true;
}

let keys = 0;
for (const f of Object.values(FIELDS)) {
  const ats = [f.at].flat().filter(described);
  for (const at of ats) {
    keys++;
    const k = schemaKey(at);
    check(!!k, `${f.id}: ${name(at)} is not in the runtime's schema`);
    check(!isRetired(at), `${f.id}: ${name(at)} is retired`);
    if (!k) continue;
    check(!k.read_only, `${f.id}: ${name(at)} is read only`);
  }
  if (ats.length !== 1 || f.read) continue;   // sizes and rows with their own mapping are checked below / by their helpers
  const k = schemaKey(ats[0]);
  if (!k) continue;
  if (f.def !== undefined) check(String(f.def) === k.default, `${f.id}: default ${f.def}, the runtime's is ${k.default}`);
  for (const o of f.options ?? []) check(accepts(k, o.value), `${f.id}: option ${o.value} is not a value of ${name(ats[0])}`);
  for (const [from, to] of Object.entries(f.aliases ?? {})) check(accepts(k, to), `${f.id}: alias ${from} -> ${to} is not a value of ${name(ats[0])}`);
  if (f.custom) {
    check(f.custom.min === k.min && f.custom.max === k.max, `${f.id}: custom range ${f.custom.min}..${f.custom.max}, the runtime's ${k.min}..${k.max}`);
    check(accepts(k, f.custom.start), `${f.id}: custom start ${f.custom.start} is out of range`);
  }
}

// The window size: both keys' defaults and the shared range.
const size = FIELDS.window_size;
if (size) {
  const [w, h] = [size.at].flat().map(schemaKey);
  check(size.def === `${w?.default}x${h?.default}`, `window_size: default ${size.def}`);
  for (const o of size.options) {
    const [ow, oh] = o.value.split('x');
    check(accepts(w, ow) && accepts(h, oh), `window_size: option ${o.value} is out of range`);
  }
}

// Quality presets: ids are the runtime's words, values within the keys' ranges.
const at = (section, key) => ({ file: 'controls', section, key });
const presetKey = schemaKey(at('display', 'quality_preset'));
for (const p of QUALITY_PRESETS) {
  check(accepts(presetKey, p.id), `quality preset ${p.id} is not a runtime preset`);
  check(accepts(schemaKey(at('display', 'lod_bias')), p.lod_bias), `quality preset ${p.id}: lod_bias ${p.lod_bias}`);
  check(accepts(schemaKey(at('display', 'anisotropy')), p.anisotropy), `quality preset ${p.id}: anisotropy ${p.anisotropy}`);
  check(accepts(schemaKey(at('display', 'shadow_quality')), p.shadow_quality), `quality preset ${p.id}: shadow_quality ${p.shadow_quality}`);
  check(accepts(schemaKey(at('upscale', 'aa')), p.aa), `quality preset ${p.id}: aa ${p.aa}`);
  if (p.render_scale !== undefined) check(p.render_scale === 0 || accepts(schemaKey(at('display', 'render_scale')), p.render_scale), `quality preset ${p.id}: render_scale`);
  if (p.fps_limit !== undefined) check(p.fps_limit === -1 || accepts(schemaKey(at('display', 'fps_limit')), p.fps_limit), `quality preset ${p.id}: fps_limit`);
}

// Gameplay mods (the runtime's "gameplay" group). A switch is a bool, on by default unless the runtime marks it opt_in (then off), and a
// toggle of Settings > Gameplay with that default unless it is hidden (then no row). A mod's setting ("parent") is a row shown under its
// switch with the runtime's default. A release run (release.yml) runs this too, so a launcher whose schema has a switch turned off for a
// test build does not ship.
const mods = gameplayKeys();
check(mods.length > 0, 'no gameplay mods in the [pkp] schema');
for (const key of mods) {
  const k = schemaKey({ file: 'pkp', section: 'pkp', key });
  const f = FIELDS[key];
  if (isGameplaySwitch(k)) {
    const def = k.opt_in ? '0' : '1';
    check(k.type === 'bool' && k.default === def, `gameplay mod [pkp] ${key}: ${k.type}, default ${k.default}; a switch is a bool, default ${def}${k.opt_in ? ' (opt_in)' : ''}`);
    if (k.hidden) check(!f, `gameplay mod [pkp] ${key} is hidden but has a row in Settings > Gameplay`);
    else check(!!f && f.type === 'toggle' && String(f.def) === def, `gameplay mod [pkp] ${key}: no toggle with default ${def} in Settings > Gameplay`);
  } else {
    const parent = schemaKey({ file: 'pkp', section: 'pkp', key: k.parent });
    check(!!parent && isGameplaySwitch(parent) && !parent.hidden, `gameplay setting [pkp] ${key}: its parent ${k.parent} is not a shown switch`);
    check(!!f && String(f.def) === k.default && typeof f.when === 'function', `gameplay setting [pkp] ${key}: no row with default ${k.default} under ${k.parent} in Settings > Gameplay`);
  }
}

// Pokémon cap is a selectable gameplay mod and must stay within the 64-slot pool.
const capSpec = schemaKey({ file: 'pkp', section: 'pkp', key: 'pokemon_cap_limit' });
const capOptions = FIELDS.pokemon_cap_limit?.options?.map((o) => Number(o.value)) ?? [];
check(FIELDS.pokemon_cap_50?.type === 'toggle', 'Pokemon cap must have a launcher toggle');
check(capSpec?.default === '30' && capSpec?.min === 25 && capSpec?.max === 63,
  'Pokemon cap range/default must be 25..63 / 30');
check(capOptions.length === 39 && capOptions.every((v, i) => v === 25 + i),
  'Pokemon cap selector must cover all values from 25 to 63');
check(keys > 10, `only ${keys} keys checked`);
console.log(bad ? `${bad} failed` : `settings schema: ${keys} keys ok`);
process.exit(bad ? 1 : 0);
