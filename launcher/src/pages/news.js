// What's New: two panes like Apple Notes, the releases left, the note right. Each pane scrolls by itself; each note keeps
// its place for as long as the page is open.
import './news.css';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { errText, esc, icon, toast } from '../ui.js';
import { store } from '../store.js';
import { canUpdate } from '../action.js';
import { formatDate, isNewer, parseReleaseName, render as renderNote } from '../markdown.js';
import { groups } from '../testing-log.js';

const isGithub = (url) => /^https:\/\/github\.com\//.test(url ?? '');

function item(r, s) {
  if (r.virtual) {
    return `<button class="note-item" data-tag="${esc(r.tag)}">
    <span class="note-item-title clip"><strong>${esc(r.name)}</strong></span>
    <span class="note-item-sub">${esc(r.count === 1 ? t('1 Testing build') : t('{count} Testing builds', { count: r.count }))}<span class="note-mark">${esc(t('Testing'))}</span></span>
  </button>`;
  }
  const { version, build } = parseReleaseName(r.name, r.tag);
  const date = formatDate(r.date);
  const mark = s.news.update?.tag === r.tag ? `<span class="note-mark new">${esc(t('Update'))}</span>`
    : version === s.app?.version ? `<span class="note-mark">${esc(t('Installed'))}</span>` : r.prerelease ? `<span class="note-mark">${esc(t('Testing'))}</span>` : '';
  return `<button class="note-item" data-tag="${esc(r.tag)}">
    <span class="note-item-title clip"><strong>${esc(version)}</strong>${build ? ` ${esc(build)}` : ''}</span>
    <span class="note-item-sub">${date ? `<time>${esc(date)}</time>` : ''}${mark}</span>
  </button>`;
}

function noteHead(r, s) {
  if (r.virtual) {
    return `<div class="grow">
      <h1>${esc(r.name)}</h1>
      <time>${esc(t('Every Testing build since the last stable release, newest first'))}</time>
    </div>`;
  }
  const { version, build } = parseReleaseName(r.name, r.tag);
  const date = formatDate(r.date, 'long');
  const newer = s.news.update && isNewer(version, s.app?.version);
  const updating = !!s.update;
  const action = newer
    ? `<button class="btn primary" data-act="update" ${canUpdate(s) ? '' : 'disabled'}>${updating ? `${icon('loader-circle', 'spin')}${esc(t('Updating'))}` : `${icon('download')}${esc(t('Update'))}`}</button>`
    : '';
  const open = isGithub(r.url) ? `<button class="btn quiet icon-only" data-url="${esc(r.url)}" aria-label="${esc(t('Open on GitHub'))}" title="${esc(t('Open on GitHub'))}">${icon('external-link')}</button>` : '';
  return `<div class="grow">
      <h1>${esc(version)}${build ? ` <span class="note-build">${esc(build)}</span>` : ''}</h1>
      ${date ? `<time>${esc(date)}</time>` : ''}
    </div>
    ${open}${action}`;
}

const BANNER = () => t('A Testing build: newer than the latest stable release and less tested. You are on Testing: please tell us what you find and attach pkp.log (Settings > About > Report a problem).');

/** A note's body. Testing notes (and the overview) get their regressions as a highlighted block at the top, and a banner. */
function noteBody(r) {
  const { version } = parseReleaseName(r.name, r.tag);
  if (r.virtual) return renderNote(r.body, { warn: true });
  if (!r.prerelease) return renderNote(r.body, { title: version });
  return `<p class="md-banner">${icon('triangle-alert')}<span>${esc(BANNER())}</span></p>\n${renderNote(r.body, { title: version, warn: true })}`;
}

const groupHtml = (g, s) => `${g.label ? `<div class="note-group">${esc(g.label)}</div>` : ''}${g.items.map((x) => item(x, s)).join('')}`;

export function render(el, ctx) {
  el.innerHTML = `<div class="page news-page">
    <div class="news glass" id="split" hidden>
      <div class="note-list" id="list" role="navigation" aria-label="${esc(t('Releases'))}"></div>
      <article class="note" id="note" tabindex="0">
        <header class="note-head" id="head"></header>
        <div class="md selectable" id="body"></div>
      </article>
    </div>
    <div class="empty glass" id="empty" hidden>
      <span>${esc(t('Patch notes load when you are online.'))}</span>
      <button class="btn" id="retry">${icon('refresh-cw')}${esc(t('Try again'))}</button>
    </div>
  </div>`;

  const part = (id) => el.querySelector(`#${id}`);
  const patch = (node, html) => {
    if (node.dataset.html !== html) {
      node.innerHTML = html;
      node.dataset.html = html;
    }
  };

  let tag = ctx.tag ?? null;         // what was asked for; the newest stands in until that release is known
  let shown = null;                  // the tag on screen
  let checking = false;
  const places = new Map();          // tag -> scrollTop of its note

  const items = () => [...part('list').querySelectorAll('.note-item')];

  function markSelected(reveal) {
    for (const b of items()) {
      const on = b.dataset.tag === shown;
      b.toggleAttribute('aria-current', on);
      b.tabIndex = on ? 0 : -1;
      if (on && reveal) b.scrollIntoView({ block: 'nearest' });
    }
  }

  function draw(s) {
    const list = s.news.releases;
    part('split').hidden = !list.length;
    part('empty').hidden = !!list.length || checking;
    if (!list.length) return;

    const grouped = groups(list);
    const r = grouped.flatMap((g) => g.items).find((x) => x.tag === tag) ?? list[0];
    const changed = r.tag !== shown;
    if (shown && changed) places.set(shown, part('note').scrollTop);
    shown = r.tag;
    patch(part('list'), grouped.map((g) => groupHtml(g, s)).join(''));
    patch(part('head'), noteHead(r, s));
    patch(part('body'), noteBody(r));
    markSelected(changed);
    if (changed) part('note').scrollTop = places.get(r.tag) ?? 0;
  }

  async function check() {
    checking = true;
    part('retry').disabled = true;
    draw(store.state);
    await store.refreshNews(true);
    checking = false;
    part('retry').disabled = false;
    draw(store.state);
  }

  function select(next, focus) {
    tag = next;
    draw(store.state);
    if (focus) items().find((b) => b.dataset.tag === next)?.focus();
  }

  el.addEventListener('click', (e) => {
    const hit = e.target.closest('[data-tag], [data-url], [data-act], #retry');
    if (!hit) return;
    if (hit.id === 'retry') return check();
    if (hit.dataset.tag) return select(hit.dataset.tag);
    if (hit.dataset.act === 'update') return store.installUpdate();
    if (hit.dataset.url) api.openLink({ url: hit.dataset.url }).catch((err) => toast(errText(err), 'error'));
  });

  el.addEventListener('keydown', (e) => {
    // links have no href, so Enter does not click them by itself
    if (e.key === 'Enter' && e.target.matches?.('a[data-url]')) return e.target.click();
    if (!e.target.closest?.('#list')) return;
    const all = items();
    const at = all.findIndex((b) => b.dataset.tag === shown);
    const next = { ArrowDown: at + 1, ArrowUp: at - 1, Home: 0, End: all.length - 1 }[e.key];
    if (next === undefined) return;
    e.preventDefault();
    if (all[next]) select(all[next].dataset.tag, true);
  });

  ctx.on(draw);
  if (store.state.news.releases.length) draw(store.state);
  else check();   // nothing loaded yet, or nothing cached: the empty state only shows once this has failed too
}
