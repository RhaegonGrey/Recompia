// Mods: the player's Modtopia mods (Installed) and the Modtopia mod hub's list (Browse). The core behind them, the trust rule and the
// hook plan are in Rust (mods.rs, mods_catalog.rs, mods_install.rs); what a row or a card says and the questions are ../mods-model.js.
// Every change answers with the new list, which is drawn whole; the plan it wrote may need Set up again, which the store's readiness
// says (kind 'mods') and this page offers with the same Set up as Home. A mod or setting the launcher keeps on (mods_lock.rs, Community
// Achievements and its sharing) shows locked, with Unlock behind its question and Lock to keep it on again.
import './mods.css';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { fromBackend } from '../backend-text.js';
import { confirm, errText, esc, field, icon, row, segmented, select, slider, toast, toggle } from '../ui.js';
import { store } from '../store.js';
import { gamePhase } from '../action.js';
import { allowQuestion, badges, byLine, canMove, cardAction, detailsView, filterByGroup, groupChips, installQuestion, isSlider, isTested, keepGroup, lockAction, settingValue, unlockQuestion } from '../mods-model.js';

const gameActive = (s) => ['starting', 'running', 'finishing'].includes(gamePhase(s.game));

const chip = (b) => `<span class="chip ${b.kind}">${esc(b.text)}</span>`;
const linkButton = (url, label, ic = 'external-link') => (url ? `<button class="btn quiet sm" data-act="link" data-url="${esc(url)}">${icon(ic)}${esc(label)}</button>` : '');
const questionHtml = (q) => q.lines.map((l) => `<p>${esc(l)}</p>`).join('');

export async function render(el, ctx) {
  let tab = 'installed';
  let view = null;        // mods_list
  let listError = '';
  let catalog = null;     // mods_catalog
  let catalogError = '';
  let group = '';         // the Browse filter chip: a group key, '' = All
  let busy = false;       // a change or an install is on its way: every control waits
  let autoUpdate = false; // launcher.json mods_auto_update (the switch for the hub's updates)

  el.innerHTML = `<div class="page mods">
    <div class="mods-head">
      <h1>${esc(t('Mods'))}</h1>
      ${segmented('mods-tab', [['installed', t('Installed')], ['browse', t('Browse')]], tab, { label: t('Mods') })}
    </div>
    <div class="mods-part" id="mods-banners"></div>
    <div class="mods-part" id="mods-prefs"></div>
    <div class="mods-part" id="mods-body"></div>
  </div>`;
  const part = (id) => el.querySelector(`#${id}`);

  // --- what is on the page

  function bannersHtml() {
    const s = store.state;
    const out = [];
    if (s.app?.readiness?.kind === 'mods') {
      out.push(`<div class="banner warn">${icon('triangle-alert')}<div class="grow">${esc(fromBackend(s.app.readiness.detail))}</div>
        <button class="btn sm" data-act="setup" ${s.task || s.starting ? 'disabled' : ''}>${icon('wrench')}${esc(t('Set up again'))}</button></div>`);
    }
    if (gameActive(s)) out.push(`<div class="banner info">${icon('info')}<div class="grow">${esc(t('Close the game to change your mods.'))}</div></div>`);
    return out.join('');
  }

  /** The launcher's own preference for the hub's mods (the update itself runs at launcher start, mods_update.rs). */
  function prefsHtml() {
    if (tab !== 'installed') return '';
    return `<div class="mods-pref glass">${row(t('Update mods by themselves'), toggle('mods-auto-update', autoUpdate, { label: t('Update mods by themselves') }))}
      <p class="faint mods-note">${esc(t('Once each time the launcher starts, from the mod hub. Mods you switched off stay off.'))}</p></div>`;
  }

  /** Unlock or Lock for a row the launcher locks (what: 'mod' or 'sharing'); '' for any other. */
  function lockButton(x, what) {
    const act = lockAction(x);
    if (!act) return '';
    const text = act === 'unlock' ? t('Unlock') : t('Lock');
    return `<button class="btn quiet sm" data-act="${act}" data-id="${what}" ${busy ? 'disabled' : ''}>${icon(act === 'unlock' ? 'lock-open' : 'lock')}${esc(text)}</button>`;
  }

  function settingControl(m, s, i, mi) {
    const id = `mod-${mi}-set-${i}`;
    const attrs = `data-mod="${esc(m.id)}" data-key="${esc(s.key)}"`;
    const opts = { label: s.label, disabled: busy || s.locked };
    let html;
    if (s.kind === 'bool') html = toggle(id, s.number !== 0, opts);
    else if (s.kind === 'choice') html = select(id, s.choices.map((c, k) => [String(k), c]), String(s.number), opts);
    else if (isSlider(s)) html = slider(id, s.min, s.max, s.number, { ...opts, step: s.step > 0 ? s.step : 'any' });
    else html = field(id, s.text, '', { ...opts, mono: s.kind === 'key' });
    // the data attributes go on the control itself, so a change event names its mod and setting
    html = html.replace(`id="${id}"`, `id="${id}" ${attrs}`) + lockButton(s, 'sharing');
    const label = s.restart ? `${s.label} ${t('(after a restart of the game)')}` : s.label;
    return row(label, html);
  }

  function modHtml(m, mi, list) {
    const msg = m.message ? `<div class="banner ${m.state === 'error' ? 'danger' : 'warn'} mod-msg">${icon(m.state === 'error' ? 'circle-x' : 'circle-alert')}<div class="grow selectable">${esc(m.message)}</div></div>` : '';
    const sets = m.settings.length ? `<div class="mod-settings">${m.settings.map((s, i) => settingControl(m, s, i, mi)).join('')}</div>` : '';
    const lock = m.locked ? `<p class="faint mods-note">${esc(t('Locked: the launcher keeps it on, so your achievements and stats are recorded. Unlock it to switch it off or remove it.'))}</p>` : '';
    return `<section class="mod glass" data-id="${esc(m.id)}">
      <div class="mod-head">
        <div class="grow">
          <h2 class="mod-name"><span class="clip">${esc(m.name || m.id)}</span> <span class="faint num">${esc(m.version)}</span></h2>
          <p class="mod-by">${esc(byLine(m))}</p>
        </div>
        ${toggle(`mod-on-${mi}`, m.enabled, { label: t('{name} on', { name: m.name || m.id }), disabled: busy || (m.locked && m.enabled) }).replace(`id="mod-on-${mi}"`, `id="mod-on-${mi}" data-on="${esc(m.id)}"`)}
      </div>
      <div class="cluster">${badges(m).map(chip).join('')}</div>
      ${m.description ? `<p class="mod-desc">${esc(m.description)}</p>` : ''}
      ${msg}
      ${lock}
      ${sets}
      <div class="cluster mod-actions">
        <button class="btn quiet sm" data-act="up" data-id="${esc(m.id)}" aria-label="${esc(t('Load {name} earlier', { name: m.name || m.id }))}" ${canMove(list, mi, -1) && !busy ? '' : 'disabled'}>${icon('arrow-up')}${esc(t('Earlier'))}</button>
        <button class="btn quiet sm" data-act="down" data-id="${esc(m.id)}" aria-label="${esc(t('Load {name} later', { name: m.name || m.id }))}" ${canMove(list, mi, 1) && !busy ? '' : 'disabled'}>${icon('arrow-down')}${esc(t('Later'))}</button>
        <span class="grow"></span>
        ${lockButton(m, 'mod')}
        <button class="btn quiet sm" data-act="remove" data-id="${esc(m.id)}" ${busy || m.locked ? 'disabled' : ''}>${icon('trash-2')}${esc(t('Remove'))}</button>
      </div>
    </section>`;
  }

  function installedHtml() {
    const folder = `<div class="cluster"><button class="btn sm" data-act="folder">${icon('folder-open')}${esc(t('Open mods folder'))}</button></div>`;
    if (listError) return `<div class="banner danger">${icon('circle-x')}<div class="grow selectable">${esc(listError)}</div><button class="btn sm" data-act="reload">${esc(t('Try again'))}</button></div>${folder}`;
    if (!view) return `<div class="empty">${icon('loader-circle', 'spin')}</div>`;
    if (!view.mods.length) {
      return `<div class="empty glass">${icon('puzzle')}<strong>${esc(t('No mods yet'))}</strong>
        <span>${esc(t('Find mods under Browse, or put them into the mods folder.'))}</span></div>${folder}`;
    }
    const order = view.mods.length > 1 ? `<p class="faint mods-note">${esc(t('Mods load from the top down: on the same game file, the one further down wins.'))}</p>` : '';
    return `${order}${view.mods.map((m, i) => modHtml(m, i, view.mods)).join('')}${folder}`;
  }

  function cardHtml(c) {
    const act = cardAction(c, catalog.preview);
    const dis = busy ? 'disabled' : '';
    const button = {
      install: `<button class="btn primary sm" data-act="install" data-id="${esc(c.id)}" ${dis}>${icon('download')}${esc(t('Install'))}</button>`,
      update: `<button class="btn primary sm" data-act="install" data-id="${esc(c.id)}" ${dis}>${icon('download')}${esc(t('Update to {version}', { version: c.version }))}</button>`,
      installed: `<span class="chip ok">${icon('check')}${esc(t('Installed'))}</span>`,
      soon: `<span class="chip">${esc(t('Coming soon'))}</span>`,
      closed: `<button class="btn sm" disabled>${icon('download')}${esc(t('Install'))}</button>`,
    }[act];
    const chips = [{ text: c.gameplay ? t('Changes gameplay') : t('Looks only'), kind: c.gameplay ? 'warn' : '' }];
    if (c.network) chips.push({ text: t('Goes online'), kind: '' });
    if (isTested(c, catalog.game)) chips.push({ text: t('Tested'), kind: 'ok' });
    return `<section class="mod glass card-mod">
      <div class="mod-head">
        <div class="grow">
          <h2 class="mod-name"><span class="clip">${esc(c.name)}</span> <span class="faint num">${esc(c.version)}</span></h2>
          <p class="mod-by">${esc(byLine(c))}</p>
        </div>
      </div>
      ${c.summary ? `<p class="mod-desc">${esc(c.summary)}</p>` : ''}
      <div class="cluster">${chips.map(chip).join('')}${c.installed && c.newer ? `<span class="chip">${esc(t('{version} installed', { version: c.installed }))}</span>` : ''}</div>
      <div class="cluster mod-actions"><button class="btn quiet sm" data-act="details" data-id="${esc(c.id)}">${icon('info')}${esc(t('Details'))}</button>${linkButton(c.page, t('Mod page'))}<span class="grow"></span>${button}</div>
    </section>`;
  }

  /** The group chips of Browse (only when the hub names groups): one button each, the chosen one pressed. */
  function groupsHtml() {
    const chips = groupChips(catalog.groups);
    if (!chips.length) return '';
    return `<div class="cluster mods-groups" role="group" aria-label="${esc(t('Mod groups'))}">${chips.map((g) => `<button class="chip" data-act="group" data-id="${esc(g.key)}" aria-pressed="${g.key === group}">${esc(g.label)}</button>`).join('')}</div>`;
  }

  function browseHtml() {
    if (catalogError) return `<div class="banner danger">${icon('circle-x')}<div class="grow">${esc(catalogError)}</div><button class="btn sm" data-act="refresh">${esc(t('Try again'))}</button></div>`;
    if (!catalog) return `<div class="empty">${icon('loader-circle', 'spin')}</div>`;
    const note = `<div class="banner info">${icon('info')}<div class="grow">${esc(t('These mods come from the Modtopia mod hub. Its team checks every mod before it is listed. Made one? Send it to the hub.'))}
        <div class="cluster mods-links">${linkButton(catalog.hub, t('Mod hub'))}${linkButton(catalog.submit, t('Submit a mod'))}
        <button class="btn quiet sm" data-act="refresh" ${busy ? 'disabled' : ''}>${icon('refresh-cw')}${esc(t('Refresh'))}</button></div></div></div>`;
    const offline = catalog.offline ? `<div class="banner warn">${icon('triangle-alert')}<div class="grow">${esc(t('The mod hub cannot be reached: this is the last list the launcher saw. Installing needs the hub.'))}</div></div>` : '';
    const preview = catalog.preview ? `<div class="banner warn">${icon('triangle-alert')}<div class="grow">${esc(t('The mod hub is not open for installs yet: this is what is coming.'))}</div></div>` : '';
    const shown = filterByGroup(catalog.mods, group);
    const cards = shown.length
      ? shown.map(cardHtml).join('')
      : `<div class="empty glass">${icon('puzzle')}<strong>${esc(catalog.mods.length ? t('No mods in this group') : t('No mods for this game version yet'))}</strong></div>`;
    return note + offline + preview + groupsHtml() + cards;
  }

  // a redraw keeps the focus on the same control (the pad and the keyboard must not lose their place)
  function focusKey() {
    const a = document.activeElement;
    if (!el.contains(a)) return null;
    if (a.id) return `#${CSS.escape(a.id)}`;
    if (a.dataset.act) return `[data-act="${a.dataset.act}"]${a.dataset.id ? `[data-id="${CSS.escape(a.dataset.id)}"]` : ''}`;
    return a.name ? `input[name="${a.name}"]:checked` : null;
  }

  function draw(key = focusKey()) {
    part('mods-banners').innerHTML = bannersHtml();
    part('mods-prefs').innerHTML = prefsHtml();
    part('mods-body').innerHTML = tab === 'installed' ? installedHtml() : browseHtml();
    if (key) el.querySelector(key)?.focus({ preventScroll: true });
  }

  // --- the calls

  async function loadList() {
    try {
      view = await api.modsList();
      listError = '';
    } catch (e) {
      listError = errText(e);
    }
    draw();
  }

  async function loadCatalog(refresh = false) {
    if (refresh) { catalog = null; catalogError = ''; draw(); }
    try {
      catalog = await api.modsCatalog({ refresh });
      group = keepGroup(catalog.groups, group);
      catalogError = '';
    } catch (e) {
      catalogError = errText(e);
    }
    draw();
  }

  /** Runs one change; its answer is the new list. The Home button and this page's banner follow the plan it wrote. */
  async function apply(call, done) {
    if (busy) return false;
    const key = focusKey();   // the controls are disabled while it runs, which takes the focus away
    busy = true;
    draw(key);
    try {
      view = await call();
      if (done) toast(done, 'ok');
      return true;
    } catch (e) {
      toast(errText(e), 'error');
      return false;
    } finally {
      busy = false;
      draw(key);
      store.refresh();
    }
  }

  async function saveAutoUpdate(on) {
    autoUpdate = on;
    try {
      await api.saveConfig({ config: { ...(await api.getConfig()), mods_auto_update: on } });
    } catch (e) {
      autoUpdate = !on;
      toast(errText(e), 'error');
    }
    draw();
  }

  async function switchMod(input) {
    const m = view?.mods.find((x) => x.id === input.dataset.on);
    if (!m) return;
    const on = input.checked;
    let allow = false;
    if (on && m.needs_yes) {
      const q = allowQuestion(m);
      allow = await confirm({ title: q.title, html: questionHtml(q), ok: q.ok });
      if (!allow) { input.checked = false; return; }
    }
    await apply(() => api.modsSetEnabled({ id: m.id, on, allow }));
  }

  async function install(id) {
    const c = catalog?.mods.find((x) => x.id === id);
    if (!c) return;
    const q = installQuestion(c);
    if (!await confirm({ title: q.title, html: questionHtml(q), ok: q.ok })) return;
    const ok = await apply(() => api.modsInstall({ id }), t('{name} is installed', { name: c.name }));
    if (ok) await loadCatalog();
  }

  /** The whole description, the settings it adds, what the hub says about testing, size and links. Plain text only. */
  async function details(id) {
    const c = catalog?.mods.find((x) => x.id === id);
    if (!c) return;
    const d = detailsView(c, catalog.game);
    const html = `<p class="mod-by">${esc(d.by)}</p>
      ${d.description ? `<p class="mod-long selectable">${esc(d.description)}</p>` : ''}
      ${d.settings.length ? `<h3 class="mod-sub">${esc(t('Settings you can change'))}</h3><ul class="mod-set-list">${d.settings.map((s) => `<li class="selectable">${esc(s)}</li>`).join('')}</ul>` : ''}
      ${d.facts.map((f) => `<p>${esc(f)}</p>`).join('')}
      ${d.links.length ? `<div class="cluster">${d.links.map((l) => linkButton(l.url, l.label)).join('')}</div>` : ''}`;
    await confirm({ title: d.title, html, ok: t('Close'), alert: true, onAct: (b) => api.openLink({ url: b.dataset.url }).catch((err) => toast(errText(err), 'error')) });
  }

  async function remove(id) {
    const m = view?.mods.find((x) => x.id === id);
    if (!m) return;
    const yes = await confirm({
      title: t('Remove {name}?', { name: m.name || m.id }),
      body: t('{file} is deleted from the mods folder. Its settings are kept, in case you install it again.', { file: m.source }),
      ok: t('Remove'),
      danger: true,
    });
    if (yes) await apply(() => api.modsRemove({ id }), t('{name} was removed', { name: m.name || m.id }));
  }

  /** Unlock asks first (what turning it off costs the player); Lock keeps it at once (mods_set_lock installs or switches it back on). */
  async function setLock(what, locked) {
    if (!locked) {
      const q = unlockQuestion(what);
      if (!await confirm({ title: q.title, html: questionHtml(q), ok: q.ok, danger: true })) return;
    }
    await apply(() => api.modsSetLock({ what, locked }), locked ? t('Locked again') : t('Unlocked'));
  }

  // --- events

  el.addEventListener('change', (e) => {
    const c = e.target;
    if (c.name === 'mods-tab') {
      tab = c.value;
      draw();
      if (tab === 'browse' && !catalog && !catalogError) loadCatalog();
      return;
    }
    if (c.id === 'mods-auto-update') return saveAutoUpdate(c.checked);
    if (c.dataset.on) return switchMod(c);
    if (c.dataset.key) {
      const m = view?.mods.find((x) => x.id === c.dataset.mod);
      const s = m?.settings.find((x) => x.key === c.dataset.key);
      if (s) apply(() => api.modsSetSetting({ id: m.id, key: s.key, ...settingValue(s, c) }));
    }
  });
  el.addEventListener('click', (e) => {
    const b = e.target.closest('[data-act]');
    if (!b || b.disabled) return;
    const id = b.dataset.id;
    switch (b.dataset.act) {
      case 'up': return apply(() => api.modsMove({ id, delta: -1 }));
      case 'down': return apply(() => api.modsMove({ id, delta: 1 }));
      case 'remove': return remove(id);
      case 'unlock': return setLock(id, false);
      case 'lock': return setLock(id, true);
      case 'install': return install(id);
      case 'details': return details(id);
      case 'group': group = id; return draw();
      case 'folder': return api.openModsDir().catch((err) => toast(errText(err), 'error'));
      case 'link': return api.openLink({ url: b.dataset.url }).catch((err) => toast(errText(err), 'error'));
      case 'refresh': return loadCatalog(true);
      case 'reload': return loadList();
      case 'setup': return store.perform('setup', ctx.go);
    }
  });

  ctx.on((_s, why) => {
    if (['app', 'game', 'task', 'setup'].includes(why)) part('mods-banners').innerHTML = bannersHtml();
  });

  api.getConfig().then((c) => { autoUpdate = !!c?.mods_auto_update; draw(); }).catch((e) => console.warn('get_config', e));
  draw();
  await loadList();
}
