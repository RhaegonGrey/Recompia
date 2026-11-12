// Achievements: a mirror of the Modtopia community tracker. The hub's list with everyone's counts and the pictures (Rust, achievements.rs:
// the launcher only reads, never reports), what this player has earned (the mod's earned.tsv), and the state of the Community Achievements
// mod with the one action that fits it. What the page says and groups is ../achievements-model.js. The mod's switch and its sharing
// setting go through the Mods page's commands (mods.rs), so there is one way into the mods folder. The launcher keeps the mod and its
// sharing on until the player unlocks them (mods_lock.rs): Unlock asks first, as on the Mods page.
import './achievements.css';
import { api } from '../api.js';
import { t, language } from '../i18n.js';
import { confirm, errText, esc, icon, toast } from '../ui.js';
import { store } from '../store.js';
import { allowQuestion, installQuestion, unlockQuestion } from '../mods-model.js';
import { PICTURES_LICENCE, dateText, earnedCount, groupList, lockLines, modActions, modLine, modState, percentText, rarest, sharingText, totals } from '../achievements-model.js';


/** The achievement's picture, or the trophy when it has none. */
const pictureHtml = (item) => `<span class="ach-pic">${item.picture ? `<img src="${esc(item.picture)}" alt="" draggable="false">` : icon('trophy')}</span>`;

const banner = (kind, ic, text, extra = '') => `<div class="banner ${kind}">${icon(ic)}<div class="grow">${esc(text)}</div>${extra}</div>`;

export async function render(el, _ctx) {
  let view = null;        // achievements_view
  let error = '';
  let modsView = null;    // mods_list: the mod's state
  let busy = false;       // a refresh or a change of the mod is on its way

  el.innerHTML = `<div class="page achievements">
    <div class="ach-head">
      <h1>${esc(t('Achievements'))}</h1>
      <button class="btn quiet sm" data-act="refresh" id="ach-refresh">${icon('refresh-cw')}${esc(t('Refresh'))}</button>
    </div>
    <div class="ach-part" id="ach-body"></div>
  </div>`;
  const body = el.querySelector('#ach-body');
  const refreshButton = el.querySelector('#ach-refresh');

  function modHtml() {
    const state = modState(modsView, view.mod_id);
    const buttons = {
      install: `<button class="btn primary sm" data-act="install" ${busy ? 'disabled' : ''}>${icon('download')}${esc(t('Install the mod'))}</button>`,
      folder: `<button class="btn quiet sm" data-act="folder">${icon('folder-open')}${esc(t('Open mods folder'))}</button>`,
      enable: `<button class="btn primary sm" data-act="enable" ${busy ? 'disabled' : ''}>${icon('check')}${esc(t('Switch the mod on'))}</button>`,
      share: `<button class="btn sm" data-act="share" ${busy ? 'disabled' : ''}>${icon('users')}${esc(t('Turn sharing on'))}</button>`,
      unlock: `<button class="btn quiet sm" data-act="unlock" ${busy ? 'disabled' : ''}>${icon('lock-open')}${esc(t('Unlock the mod'))}</button>`,
      lock: `<button class="btn quiet sm" data-act="lock" ${busy ? 'disabled' : ''}>${icon('lock')}${esc(t('Lock the mod'))}</button>`,
      'unlock-sharing': `<button class="btn quiet sm" data-act="unlock-sharing" ${busy ? 'disabled' : ''}>${icon('lock-open')}${esc(t('Unlock sharing'))}</button>`,
      'lock-sharing': `<button class="btn quiet sm" data-act="lock-sharing" ${busy ? 'disabled' : ''}>${icon('lock')}${esc(t('Lock sharing'))}</button>`,
    };
    const actions = modActions(state).map((a) => buttons[a]).join('');
    const message = state.mod?.message ? `<p class="faint selectable">${esc(state.mod.message)}</p>` : '';
    return `<section class="ach-mod glass">
      <p>${esc(modLine(state))}</p>${message}${lockLines(state).map((l) => `<p class="faint">${esc(l)}</p>`).join('')}
      <p class="faint ach-note">${esc(sharingText())}</p>
      ${actions ? `<div class="cluster">${actions}</div>` : ''}
    </section>`;
  }

  function summaryHtml() {
    const all = totals(view);
    const tile = (value, label) => `<div class="ach-tile glass"><strong class="num">${esc(value)}</strong><span>${esc(label)}</span></div>`;
    return `<div class="ach-tiles">${tile(t('{n} of {total}', { n: earnedCount(view.list, view.earned), total: view.list.length }), t('Yours'))}
      ${all ? tile(all.players, t('Players sharing')) + tile(all.earned, t('Achievements earned in total')) : ''}</div>`;
  }

  function percentHtml(item) {
    const pct = percentText(item.players, view.stats?.players);
    return pct ? `<span class="ach-pct num">${esc(t('{percent} of players', { percent: pct }))} <span class="faint">(${item.players})</span></span>` : '';
  }

  function rarestHtml() {
    const rare = rarest(view.list);
    if (!rare.length) return '';
    return `<section class="ach-group"><h2>${esc(t('Rarest right now'))}</h2><div class="ach-rare">${rare.map((a) => `<div class="ach-card glass">${pictureHtml(a)}
      <div class="grow"><strong>${esc(a.name)}</strong><div class="faint">${percentHtml(a)}</div></div></div>`).join('')}</div></section>`;
  }

  function itemHtml(a) {
    const at = view.earned[a.id];
    const have = at !== undefined;
    const pct = percentHtml(a);
    return `<div class="ach-item glass ${have ? 'earned' : ''}">${pictureHtml(a)}
      <div class="grow"><strong>${esc(a.name)}</strong><div class="ach-desc">${esc(a.desc)}</div>${pct ? `<div>${pct}</div>` : ''}</div>
      ${have ? `<span class="chip ok">${icon('check')}${esc(at > 0 ? dateText(at, language) : t('Earned'))}</span>` : `<span class="chip">${esc(t('Not yet'))}</span>`}
    </div>`;
  }

  function creditsHtml() {
    return `<p class="faint ach-note">${esc(t('Counts from the Modtopia community service: players of Pokopia Native and Recompia together. Names and descriptions are the hub\'s own, in English. Pictures: Microsoft Fluent Emoji (MIT).'))}</p>
      <div class="cluster"><button class="btn quiet sm" data-act="licence">${icon('file-text')}${esc(t('Pictures\' licence'))}</button></div>`;
  }

  function draw() {
    if (error) {
      body.innerHTML = banner('danger', 'circle-x', error, `<button class="btn sm" data-act="refresh">${esc(t('Try again'))}</button>`);
      return;
    }
    if (!view) {
      body.innerHTML = `<div class="empty">${icon('loader-circle', 'spin')}</div>`;
      return;
    }
    const offline = view.offline ? banner('warn', 'triangle-alert', t('The mod hub cannot be reached: this is the last list the launcher saw.')) : '';
    if (!view.available) {
      body.innerHTML = `${offline}<div class="empty glass">${icon('trophy')}<strong>${esc(t('The list of achievements is not available right now'))}</strong>
        <span>${esc(t('Try again later, with the internet connection on.'))}</span></div>${modHtml()}`;
      return;
    }
    let counts = '';
    if (!view.stats) counts = banner('info', 'info', t('Everyone\'s counts are not available right now.'));
    else if (view.stats.kept) counts = banner('warn', 'triangle-alert', t('The counts cannot be reached: these are the last ones the launcher saw.'));
    body.innerHTML = `${offline}${counts}${modHtml()}${summaryHtml()}${rarestHtml()}
      ${groupList(view.list).map((g) => `<section class="ach-group"><h2>${esc(g.title)}</h2>${g.items.map(itemHtml).join('')}</section>`).join('')}
      ${creditsHtml()}`;
  }

  // a redraw must not drop the pad's place: the focused control comes back by its action
  function redraw() {
    const act = el.contains(document.activeElement) ? document.activeElement.dataset.act : null;
    draw();
    if (act) el.querySelector(`#ach-body [data-act="${act}"]:not([disabled])`)?.focus({ preventScroll: true });
  }

  // --- the calls

  async function loadMods() {
    try {
      modsView = await api.modsList();
    } catch (e) {
      console.warn('mods_list', e);
      modsView = null;
    }
  }

  async function load(refresh = false) {
    if (busy) return;
    busy = true;
    refreshButton.disabled = true;
    if (refresh) { view = null; error = ''; draw(); }
    try {
      [view] = await Promise.all([api.achievementsView({ refresh }), loadMods()]);
      error = '';
    } catch (e) {
      error = errText(e);
    }
    busy = false;
    refreshButton.disabled = false;
    redraw();
  }

  /** One change of the mod: the Mods page's command answers with the new list, and the page shows the mod's new state. */
  async function change(call) {
    if (busy) return;
    busy = true;
    redraw();
    try {
      modsView = await call();
    } catch (e) {
      toast(errText(e), 'error');
    }
    busy = false;
    redraw();
    store.refresh();
  }

  async function enable() {
    const { mod } = modState(modsView, view.mod_id);
    if (!mod) return;
    let allow = false;
    if (mod.needs_yes) {
      const q = allowQuestion(mod);
      allow = await confirm({ title: q.title, html: q.lines.map((l) => `<p>${esc(l)}</p>`).join(''), ok: q.ok });
      if (!allow) return;
    }
    await change(() => api.modsSetEnabled({ id: mod.id, on: true, allow }));
  }

  async function install() {
    let card = null;
    try {
      card = (await api.modsCatalog({})).mods.find((c) => c.id === view.mod_id && c.available);
    } catch (e) {
      toast(errText(e), 'error');
      return;
    }
    if (!card) { toast(t('The mod hub cannot be reached: this is the last list the launcher saw.'), 'error'); return; }
    const q = installQuestion(card);
    if (!await confirm({ title: q.title, html: q.lines.map((l) => `<p>${esc(l)}</p>`).join(''), ok: q.ok })) return;
    await change(() => api.modsInstall({ id: card.id }));
  }

  function licence() {
    return confirm({ title: t('Pictures\' licence'), html: `<pre class="ach-licence selectable">${esc(PICTURES_LICENCE)}</pre>`, alert: true });
  }

  async function shareOn() {
    const { mod } = modState(modsView, view.mod_id);
    if (mod) await change(() => api.modsSetSetting({ id: mod.id, key: 'share', number: 1 }));
  }

  /** Unlock asks first (what turning it off costs the player); Lock keeps it on again at once (installs it when it was removed). */
  async function setLock(what, locked) {
    if (!locked) {
      const q = unlockQuestion(what);
      if (!await confirm({ title: q.title, html: q.lines.map((l) => `<p>${esc(l)}</p>`).join(''), ok: q.ok, danger: true })) return;
    }
    await change(() => api.modsSetLock({ what, locked }));
  }

  el.addEventListener('click', (e) => {
    const b = e.target.closest('[data-act]');
    if (!b || b.disabled) return;
    switch (b.dataset.act) {
      case 'refresh': return load(true);
      case 'enable': return enable();
      case 'share': return shareOn();
      case 'install': return install();
      case 'unlock': return setLock('mod', false);
      case 'lock': return setLock('mod', true);
      case 'unlock-sharing': return setLock('sharing', false);
      case 'lock-sharing': return setLock('sharing', true);
      case 'licence': return licence();
      case 'folder': return api.openModsDir().catch((err) => toast(errText(err), 'error'));
    }
  });

  draw();
  await load();
}
