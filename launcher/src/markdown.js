// Patch notes: GitHub release bodies and CHANGELOG.md to HTML. Pure, no Tauri import, so `node --test` runs it.
// Everything is escaped first and the markup is applied to the escaped text, so nothing in a note can open a tag or an
// attribute. The only thing that can leave the page is <a data-url> on https://github.com/; it has no href, so a click
// goes to the page's one listener (open_link) and never navigates the webview.
//
// Images (`![alt](url)`) become their alt text: the app's CSP allows no remote images.

import { date } from './i18n.js';

const ISSUES = 'https://github.com/featureJosh/pokopia-recomp/issues/';

// What open_link accepts, tested on escaped text: an escaped & " ' < > starts with "&", so it never gets through.
const SAFE_URL = /^https:\/\/github\.com\/[^\s&\\\u0001\u0002]*$/;

const FENCE = /^\s*(`{3,}|~{3,})/;
const HEADING = /^#{1,6}\s+(.*?)\s*#*\s*$/;
const RULE = /^\s*([-*_])(\s*\1){2,}\s*$/;
const ITEM = /^(\s*)[-*]\s+(.*)$/;
const TABLE_RULE = /^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$/;

const escape = (s) => s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

const isTableAt = (lines, i) => lines[i].includes('|') && !!lines[i + 1]?.includes('|') && TABLE_RULE.test(lines[i + 1]);
const cells = (line) => line.trim().replace(/^\|/, '').replace(/\|$/, '').split('|').map((c) => c.trim());
const startsBlock = (lines, i) => FENCE.test(lines[i]) || HEADING.test(lines[i]) || RULE.test(lines[i]) || ITEM.test(lines[i]) || isTableAt(lines, i);

/** Items with one level of nesting. A wrapped line, with or without its indent, belongs to the item above it. */
function parseList(lines, start) {
  const items = [];
  const base = ITEM.exec(lines[start])[1].length;
  let last = null;
  let i = start;
  for (; i < lines.length; i++) {
    if (!lines[i].trim()) {
      let next = i + 1;
      while (next < lines.length && !lines[next].trim()) next++;
      if (next >= lines.length || !(ITEM.test(lines[next]) || /^\s{2,}\S/.test(lines[next]))) break;
      i = next - 1;
      continue;
    }
    const m = ITEM.exec(lines[i]);
    if (RULE.test(lines[i])) break;
    if (m) {
      last = { text: m[2], sub: [] };
      if (m[1].length >= base + 2 && items.length) items.at(-1).sub.push(last);
      else items.push(last);
    } else if (startsBlock(lines, i)) break;
    else last.text += ` ${lines[i].trim()}`;
  }
  return [{ type: 'list', items }, i];
}

/** Splits a note into blocks of unescaped text: heading, para, list, code, rule, table. */
function parse(md) {
  // \u0000-\u0008 cannot be in a real note, and \u0001 \u0002 mark the stash in inline()
  const lines = String(md ?? '').replace(/[\u0000-\u0008]/g, '').split(/\r?\n/);
  const blocks = [];
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    const fence = FENCE.exec(line);
    if (!line.trim()) i++;
    else if (fence) {
      const close = new RegExp(`^\\s*${fence[1][0]}{${fence[1].length},}\\s*$`);
      const code = [];
      for (i++; i < lines.length && !close.test(lines[i]); i++) code.push(lines[i]);
      i++;
      blocks.push({ type: 'code', text: code.join('\n') });
    } else if (HEADING.test(line)) {
      blocks.push({ type: 'heading', level: /^#+/.exec(line)[0].length, text: HEADING.exec(line)[1] });
      i++;
    } else if (RULE.test(line)) {
      blocks.push({ type: 'rule' });
      i++;
    } else if (ITEM.test(line)) {
      const [list, next] = parseList(lines, i);
      blocks.push(list);
      i = next;
    } else if (isTableAt(lines, i)) {
      const head = cells(line);
      const rows = [];
      for (i += 2; i < lines.length && lines[i].includes('|'); i++) rows.push(cells(lines[i]));
      blocks.push({ type: 'table', head, rows });
    } else {
      const text = [line.trim()];
      for (i++; i < lines.length && lines[i].trim() && !startsBlock(lines, i); i++) text.push(lines[i].trim());
      blocks.push({ type: 'para', text: text.join(' ') });
    }
  }
  return blocks;
}

const emphasis = (s) => s
  .replace(/\*\*(?=\S)(.+?)(?<=\S)\*\*(?!\*)/g, '<strong>$1</strong>')
  .replace(/(?<![\w*])\*(?=[^\s*])(.+?)(?<=[^\s*])\*(?![\w*])/g, '<em>$1</em>');

/** One line of text to HTML. What is done goes in a stash, so a later rule cannot reach into code or into a URL. */
function inline(raw) {
  const stash = [];
  const hold = (html) => `\u0001${stash.push(html) - 1}\u0002`;
  const restore = (s) => s.replace(/\u0001(\d+)\u0002/g, (_, n) => restore(stash[n]));
  const link = (url, text) => hold(`<a data-url="${url}" role="link" tabindex="0" title="${url}">${text}</a>`);

  const text = escape(raw)
    .replace(/(?<!`)(`+)(?!`)(.+?)(?<!`)\1(?!`)/g, (_, __, code) => hold(`<code>${code}</code>`))
    // one level of parentheses in a URL; an optional "title" is dropped
    .replace(/(!?)\[([^\]]*)\]\(\s*((?:[^()\s]|\([^()\s]*\))*)(?:\s+&quot;.*?&quot;)?\s*\)/g, (_, image, label, url) =>
      (!image && SAFE_URL.test(url) ? link(url, emphasis(label)) : label))
    // not after & : the #39 of an escaped apostrophe is no issue
    .replace(/(?<![\w&;#/])#(\d+)\b/g, (_, n) => link(`${ISSUES}${n}`, `#${n}`));
  return restore(emphasis(text));
}

function listHtml(items) {
  const li = items.map((it) => `<li>${inline(it.text)}${it.sub.length ? listHtml(it.sub) : ''}</li>`);
  return `<ul>${li.join('')}</ul>`;
}

function blockHtml(b) {
  switch (b.type) {
    case 'heading': {
      const tag = b.level >= 4 ? 'h4' : 'h3';
      return `<${tag}>${inline(b.text)}</${tag}>`;
    }
    case 'para': return `<p>${inline(b.text)}</p>`;
    case 'list': return listHtml(b.items);
    case 'code': return `<pre><code>${escape(b.text)}</code></pre>`;
    case 'rule': return '<hr>';
    case 'table': {
      const row = (r, tag) => `<tr>${r.map((c) => `<${tag}>${inline(c)}</${tag}>`).join('')}</tr>`;
      return `<div class="md-table"><table><thead>${row(b.head, 'th')}</thead><tbody>${b.rows.map((r) => row(r, 'td')).join('')}</tbody></table></div>`;
    }
  }
  return '';
}

/**
 * Markdown to HTML, safe to assign to innerHTML. `title`: the version the page already shows as the note's title; a first
 * heading that repeats it is left out.
 */
export function render(md, { title = '', warn = false } = {}) {
  const blocks = parse(md);
  if (title && blocks[0]?.type === 'heading' && blocks[0].text.includes(title)) blocks.shift();
  if (!warn) return blocks.map(blockHtml).join('\n');
  // Testing notes: a "Regressions and known problems" section is drawn as a highlighted block at the top of the note
  const [top, flagged, asked, rest] = [[], [], [], []];
  let into = top;
  for (const b of blocks) {
    if (b.type === 'heading') into = WARN_HEADING.test(b.text.trim()) ? flagged : TEST_HEADING.test(b.text.trim()) ? asked : rest;
    into.push(b);
  }
  const box = (cls, list) => (list.length ? `<section class="${cls}">${list.map(blockHtml).join('\n')}</section>` : '');
  return [box('md-warn', flagged), box('md-test', asked), ...[...top, ...rest].map(blockHtml)].filter(Boolean).join('\n');
}

/** The headings the launcher flags in a Testing note. */
export const WARN_HEADING = /^(regressions|known problems)\b/i;
/** The section that tells a tester what to try in this build. */
export const TEST_HEADING = /^what to test\b/i;

/**
 * The raw markdown of one section: its heading line and everything up to the next heading (any `##`, `###` or `####`), or ''.
 * `name` is matched against the heading text, case-insensitively, at the start.
 */
export function section(md, name) {
  const lines = String(md ?? '').split(/\r?\n/);
  const at = lines.findIndex((l) => /^#{2,4}\s/.test(l) && new RegExp(`^${name}`, 'i').test(l.replace(/^#+\s+/, '').trim()));
  if (at < 0) return '';
  let end = lines.findIndex((l, i) => i > at && /^#{2,4}\s/.test(l));
  if (end < 0) end = lines.length;
  return lines.slice(at, end).join('\n').trim();
}

/** Text with the markup taken off, for a one-line preview. Not escaped: it goes through esc() like any other text. */
export const plain = (text) => text
  .replace(/!?\[([^\]]*)\]\([^)]*\)/g, '$1')
  .replace(/\*\*|`|(?<![\w*])\*(?=\S)(?=[^\s.]*\*)|(?<=\S)\*(?![\w*])/g, '');

const firstSentence = (text) => /^.+?[.!?](?=\s|$)/.exec(text)?.[0] ?? text;

/**
 * The first lines of a note for a card: list items of its `### Highlights` section (`### What's new` in releases up to
 * 0.5.0), or else the first sentence of the note.
 */
export function highlights(md, count = 2) {
  const blocks = parse(md).filter((b) => b.type !== 'rule');
  const at = blocks.findIndex((b) => b.type === 'heading' && /^(highlights|what.?s new)$/i.test(b.text.trim()));
  if (at >= 0) {
    const end = blocks.findIndex((b, i) => i > at && b.type === 'heading');
    const list = blocks.slice(at + 1, end < 0 ? undefined : end).find((b) => b.type === 'list');
    if (list) return list.items.slice(0, count).map((it) => plain(it.text));
  }
  const first = blocks.find((b) => b.type === 'para' || b.type === 'list');
  if (!first) return [];
  return [firstSentence(plain(first.type === 'para' ? first.text : first.items[0].text))];
}

/** "Pokopia Recomp 0.7.0 (build K: Kangaroo)" -> { version: '0.7.0', build: 'Kangaroo' }. The tag stands in for a name without a version. */
export function parseReleaseName(name, tag = '') {
  const text = String(name ?? '');
  return {
    version: /\b\d+\.\d+[\w.+-]*/.exec(text)?.[0] ?? String(tag ?? '').replace(/^v/, ''),
    build: /\(build\s+\w+:\s*([^,)]+)/i.exec(text)?.[1].trim() ?? '',
  };
}

const numbers = (v) => String(v ?? '').replace(/^v/, '').replace(/[-+].*$/, '').split('.').map((n) => parseInt(n, 10) || 0);
/** The prerelease identifiers of "0.8.1-beta.2+build" -> ['beta', '2']; none for a release. */
const pre = (v) => {
  const m = /^[^-+]*-([^+]*)/.exec(String(v ?? '').replace(/^v/, ''));
  return m ? m[1].split('.') : [];
};
const identifier = (x, y) => {
  const [nx, ny] = [/^\d+$/.test(x), /^\d+$/.test(y)];
  if (nx && ny) return Math.sign(parseInt(x, 10) - parseInt(y, 10));
  if (nx !== ny) return nx ? -1 : 1;   // a number ranks below a word, as in semver
  return x < y ? -1 : x > y ? 1 : 0;
};

/** a is a later version than b, as the launcher's update check ranks them: 0.8.1-beta.1 is after 0.8.0 and before 0.8.1, and beta.10 is after beta.2. A +build suffix is ignored. */
export function isNewer(a, b) {
  const [x, y] = [numbers(a), numbers(b)];
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    if ((x[i] ?? 0) !== (y[i] ?? 0)) return (x[i] ?? 0) > (y[i] ?? 0);
  }
  const [px, py] = [pre(a), pre(b)];
  if (!px.length || !py.length) return !px.length && py.length > 0;   // a release is after its own prerelease
  for (let i = 0; i < Math.max(px.length, py.length); i++) {
    if (px[i] === undefined) return false;      // fewer identifiers rank lower
    if (py[i] === undefined) return true;
    const c = identifier(px[i], py[i]);
    if (c) return c > 0;
  }
  return false;
}

/** "Oct 30, 2026" in English (the language's own order and words otherwise), or "October 30, 2026" with month 'long'. A date that does not parse gives ''. */
export function formatDate(iso, month = 'short') {
  const d = new Date(iso ?? '');
  return Number.isNaN(+d) ? '' : date(d, { month, day: 'numeric', year: 'numeric' });
}
