// Link Play. What the settings mean, and every edit, is in ../linkplay-model.js; this page is the fields and the calls around them. Nothing is
// written on opening. A choice or a field applies itself: a commit reads the whole page, so nothing half-written reaches pkp.ini.
// Fields never redraw on commit (tabbing to the next field must keep its focus); only a mode, role or toggle change redraws, and that keeps focus.
import './linkplay.css';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { fromBackend } from '../backend-text.js';
import { errText, esc, field, group, icon, row, segmented, toast, toggle } from '../ui.js';
import { store } from '../store.js';
import { gamePhase } from '../action.js';
import {
  HOST_PORT, RELAY_PORT, addrProblem, parseAddr, addressRows, applyEdits, codeEdits, editsFor, hostsOverInternet, interactEdits, internetShare, pathParts,
  portForwardEdits, protocolEdits, localAddr, readIni, relayPort, shownCode, storyEdits, uiFrom,
} from '../linkplay-model.js';
import { inviteNote, inviteToUi, makeInvite, parseInvite } from '../link-invite-model.js';
import { probeMessage, setupFindings } from '../setup-check-model.js';
import { friendRows, joinNote, joinUi } from '../tailscale-model.js';

const LS = 'pkp.linkplay';
const POLL_MS = 3000;   // link count and the game's room code, while the Host view is on screen

const stored = () => { try { return JSON.parse(localStorage.getItem(LS)) || {}; } catch { return {}; } };
const keep = (patch) => { try { localStorage.setItem(LS, JSON.stringify({ ...stored(), ...patch })); } catch { /* private window: fine */ } };

const gameActive = (s) => ['starting', 'running', 'finishing'].includes(gamePhase(s.game));

// which `ui` value each field edits
const FIELDS = { 'lp-join': 'lanJoin', 'lp-relay': 'relay', 'lp-addr': 'visitAddr', 'lp-code-in': 'visitCode' };

export async function render(el, ctx) {
  let pkp = { ...(await api.readSettings()).pkp };
  const mem = stored();
  const ui = uiFrom(pkp, mem);
  let relay = await api.relayStatus().catch(() => null);
  // this PC's relay, remembered from last time, is only offered while it runs: the launcher just started, so it does not until the
  // toggle is turned on, and a host there would open the room on a relay that is not there (#239)
  if (!relay && /^127\./.test(ui.relay.trim())) ui.relay = '';
  let lan = null;
  let publicIp;            // this PC's public address: undefined not asked yet, null not found
  let asking = false;
  let gameCode = null;     // room-code.txt: the game's own
  let dropped = null;      // the game's code the player replaced with New code
  let problems = {};       // field id -> what is wrong with it
  // the read-only checks (linkcheck.rs): what each last said, and which one is running
  let probeOut = null;     // { level, text, addr } of Test connection
  let setupOut = null;     // setupFindings(): [{ level, title, detail, fix }]
  let friendsOut = null;   // friendRows(): { ok, note, rows }
  let busy = '';           // 'probe' | 'setup-check' | 'friends' while one runs

  // commits run one at a time, in the order the player made them: a blur, then the click that caused it
  let tail = Promise.resolve();
  const inOrder = (fn) => { tail = tail.then(fn).catch((e) => toast(errText(e), 'error')); return tail; };

  const remember = (patch) => { Object.assign(mem, patch); keep(patch); };
  const isHost = () => ui.view === 'internet' && ui.role === 'host';
  const isVisit = () => ui.view === 'internet' && ui.role === 'visit';
  const isOwn = () => ui.view === 'internet' && ui.server === 'own';   // Advanced: an own relay, or a direct address
  let advOpen = ui.server === 'own';

  async function save(edits) {
    await api.writeSettings({ edits });
    pkp = applyEdits(pkp, edits);
    ui.legacy = readIni(pkp).legacy;
  }

  async function loadLan() {
    lan = await api.lanReport().catch(() => null);
    drawLan();
  }

  // asked only for an internet host: one HTTPS request to a what-is-my-IP service (launcher/src-tauri/src/public_ip.rs)
  async function loadPublicIp() {
    if (publicIp || asking) return;
    asking = true;
    publicIp = await api.publicIp().catch(() => null) ?? null;
    asking = false;
    patch(part('lp-share'), shareHtml());
  }

  async function loadGameCode() {
    const c = await api.roomCodeHost().catch(() => null);
    gameCode = c === dropped ? null : c;
    drawCode();
  }

  // --- what is on the page

  const copyButton = (addr) => `<button class="btn quiet sm lp-copy" data-copy="${esc(addr)}" aria-label="${esc(t('Copy {text}', { text: addr }))}" title="${esc(t('Copy'))}">${icon('copy')}</button>`;
  // an invite carries the address, the room code and the mode in one paste (link-invite-model.js); built when pressed, from the code shown then
  const inviteButton = (r) => (r.invite ? `<button class="btn quiet sm lp-invite" data-invite="${esc(r.addr)}" data-through="${r.through ? 1 : 0}" title="Copy an invite that fills in the address and room code for your friend">${icon('link')}Copy invite</button>` : '');
  const addressRow = (r) => row(r.label, `<code class="selectable">${esc(r.addr)}</code>${copyButton(r.addr)}${inviteButton(r)}`);
  const inviteField = () => `<div class="lp-field">${field('lp-invite', '', 'Paste the invite here', { mono: true })}<small class="lp-msg" id="lp-invite-msg" hidden></small></div>`;
  const fieldAt = (id, placeholder) => `<div class="lp-field">${field(id, ui[FIELDS[id]], placeholder, { mono: true })}<small class="lp-msg" id="${id}-msg" hidden></small></div>`;
  const visitPlaceholder = () => (ui.through ? `203.0.113.5:${RELAY_PORT}` : '192.0.2.10');

  function bannersHtml() {
    const legacy = ui.legacy ? `<div class="banner warn">${icon('triangle-alert')}<div class="grow">${esc(t('This install uses the old Link Play protocol'))}</div><button class="btn sm" data-act="switch">${esc(t('Switch to current'))}</button></div>` : '';
    const later = gameActive(store.state) ? `<div class="banner info">${icon('info')}<div class="grow">${esc(t('Changes apply next time the game starts'))}</div></div>` : '';
    return legacy + later;
  }

  const live = () => ui.view !== 'off' || ui.legacy;   // the old protocol ignores mode, so Off does not stop it

  function pathHtml() {
    if (!live()) return '';
    // the menu path inside the game (its Link Play menu always says Local connection), not a breadcrumb of this page
    return `<span class="lp-path-lead">${icon('gamepad-2')}${esc(t('In game'))}</span>`
      + pathParts(ui).map((p) => `<span class="chip on-art">${esc(p)}</span>`).join(icon('chevron-right'));
  }

  function sharedHtml() {
    if (!live()) return '';
    const exp = `<span class="chip lp-exp">${esc(t('Experimental'))}</span>`;
    return group(t('Same for everyone'), [
      row(t('Visitors can build everywhere'), exp + toggle('lp-interact', ui.interact || ui.story, { disabled: ui.story })),
      row(t('Visitors can talk to story characters'), exp + toggle('lp-story', ui.story)),
    ]);
  }

  function lanHtml() {
    if (!lan) return '';
    const notice = [lan.warning, lan.firewall_hint].filter(Boolean);
    const note = notice.length ? `<div class="banner warn">${icon('triangle-alert')}<div class="grow">${esc(fromBackend(notice[0]))}${notice[1] ? `<div>${esc(fromBackend(notice[1]))}</div>` : ''}</div></div>` : '';
    const rows = addressRows(lan, HOST_PORT).map((r) => addressRow({ ...r, invite: localAddr(r.addr) }));   // a Tailscale address needs the room code: its invite is on the Internet page
    return note + group(t('This PC'), rows.length ? rows : [row(t('No address found'), '')]);
  }

  // what visitors type: the public address over the internet (#245), this PC's LAN addresses on the same network
  function shareHtml() {
    if (!isHost()) return '';
    if (!isOwn()) return `<div class="banner info">${icon('info')}<div class="grow">${esc(t('Visitors pick Internet, then Visit, and type this room code. Nothing else to set up.'))}</div></div>`;
    const share = internetShare(ui, { publicIp, relayOn: relay ? relayPort(relay) : 0 });
    if (!share) {
      return `<div class="banner warn">${icon('triangle-alert')}<div class="grow">${esc(t('The relay on this PC is off, so visitors cannot join. Turn on Run a relay on this PC.'))}</div></div>`;
    }
    const internet = share.addr
      ? addressRow({ addr: share.addr, label: t('Over the internet'), invite: true, through: share.through })
      : row(t('Over the internet'), `<span class="faint">${esc(publicIp === null ? t('Could not find this PC\'s public address. Search "what is my IP" in a browser.') : t('Looking it up\u2026'))}</span>`);
    // a VPN (Tailscale and the like) keeps its own name: it is not the same network. Its address is shown with or without a relay: a friend
    // on the same tailnet reaches this PC at it directly
    const mine = (a) => ({ addr: `${a.ip}:${share.port}`, invite: true, through: !!relay });
    const local = share.own && lan
      ? [...(relay ? lan.lan.map((a) => addressRow({ ...mine(a), label: t('Same network ({adapter})', { adapter: a.adapter }) })) : []),
        ...lan.overlay.map((a) => addressRow({ ...mine(a), label: a.label.replace(/^internet via /, '') }))]
      : [];
    const links = relay ? [row(t('Links'), `<span class="num" id="lp-links">${esc(relay.clients)}</span>`)] : [];
    const how = share.through ? t('Visitors pick Visit and type this address and the room code, with Through a relay on.') : t('Visitors pick Visit and type this address and the room code, with Through a relay off.');
    // without a relay the game asks the router itself ([net] port_forward); this PC's relay listens on its own port, which it does not
    const auto = share.own && !share.through && ui.portForward;
    const router = !share.own ? ''
      : auto ? ` While you host, the game asks your router to forward port ${share.port} (TCP and UDP) to this PC. If the router does not allow it, the game's log says why: forward the port by hand then.`
      : ` ${t('Your router must forward port {port} (TCP and UDP) to this PC.', { port: share.port })}`;
    return group(t('Share with visitors'), [internet, ...local, ...links])
      + `<div class="banner info">${icon('info')}<div class="grow">${esc(how)}${esc(router)} ${esc(t("Never give out 127.0.0.1: on a visitor's PC it means their own PC."))}</div></div>`;
  }

  // --- Test connection, Check my setup, Friends on my Tailscale: read only, nothing is written to pkp.ini by them

  const checkIcon = { ok: 'circle-check', tip: 'info', problem: 'triangle-alert' };
  const checkTone = { ok: 'ok', tip: 'info', problem: 'warn' };
  const checkBanner = (level, title, detail, fix) => `<div class="banner ${checkTone[level]}">${icon(checkIcon[level])}<div class="grow">${title ? `<strong>${esc(title)}</strong>` : ''}<div>${esc(detail)}</div>${fix ? `<div>${esc(fix)}</div>` : ''}</div></div>`;
  const checkButton = (act, label, ico) => `<button class="btn sm" id="lp-btn-${act}" data-act="${act}" ${busy ? 'disabled' : ''}>${icon(ico)}${busy === act ? 'Working\u2026' : label}</button>`;

  /** The address Test connection tries: the one typed on this page, with the relay's port when the visit goes through a relay. */
  function probeTarget() {
    const typed = (ui.view === 'lan' ? ui.lanJoin : isVisit() ? ui.visitAddr : '').trim();
    const a = parseAddr(typed);
    return a && a.port === null ? `${typed}:${isVisit() && ui.through ? RELAY_PORT : HOST_PORT}` : typed;
  }

  function checksHtml() {
    if (ui.view === 'off') return '';
    const rows = [];
    if (ui.view === 'lan' || isVisit()) rows.push(row('Test connection', checkButton('probe', 'Test', 'refresh-cw')));
    rows.push(row('Check my setup', checkButton('setup-check', 'Check', 'wrench')));
    if (isVisit()) rows.push(row('Friends on my Tailscale', checkButton('friends', 'Look', 'users')));
    let out = '';
    if (probeOut && probeOut.addr === probeTarget() && (ui.view === 'lan' || isVisit())) out += checkBanner(probeOut.level, '', probeOut.text);   // only for the address it tried
    if (setupOut) out += setupOut.map((f) => checkBanner(f.level, f.title, f.detail, f.fix)).join('');
    if (friendsOut && isVisit()) {
      out += friendsOut.ok
        ? group('Your Tailscale devices', friendsOut.rows.length ? friendsOut.rows.map((r) => row(r.dns ? `${r.name} (${r.dns})` : r.name, `<span class="faint">${esc(r.hint)}</span>${r.active ? `<button class="btn sm" data-friend="${esc(r.key)}">Join</button>` : ''}`)) : [row(friendsOut.note, '')])
        : checkBanner('tip', '', friendsOut.note);
      if (friendsOut.ok && friendsOut.rows.length && friendsOut.note) out += checkBanner('tip', '', friendsOut.note);
    }
    return group('Check the connection', rows) + out;
  }

  const drawChecks = () => patch(part('lp-checks'), checksHtml());

  const probe = () => inOrder(async () => {
    const addr = probeTarget();
    if (!addr) {
      probeOut = { level: 'tip', text: 'Type or paste the host\'s address first, then test.', addr };
      drawChecks();
      return;
    }
    busy = 'probe';
    drawChecks();
    try {
      probeOut = { ...probeMessage(await api.linkProbe({ addr }), addr), addr };
    } finally {
      busy = '';
      drawChecks();
    }
  });

  const runSetupCheck = () => inOrder(async () => {
    busy = 'setup-check';
    drawChecks();
    try {
      const [facts, report] = await Promise.all([api.setupFacts().catch(() => null), api.lanReport().catch(() => null)]);
      lan = report || lan;
      // the port a friend needs: this PC's relay while it runs, else the host port
      setupOut = setupFindings({ lan, facts, port: relay ? relayPort(relay) : HOST_PORT });
    } finally {
      busy = '';
      drawChecks();
    }
  });

  const runFriends = () => inOrder(async () => {
    busy = 'friends';
    drawChecks();
    try {
      friendsOut = friendRows(await api.tailscalePeers().catch(() => null));
    } finally {
      busy = '';
      drawChecks();
    }
  });

  /** Join on a Tailscale friend: the page shows that address as an Internet visit and waits for the room code (or an invite). */
  const joinFriend = (key) => inOrder(async () => {
    const r = friendsOut?.rows.find((x) => x.key === key);
    if (!r) return;
    Object.assign(ui, joinUi(r));
    remember({ role: 'visit' });
    problems = {};
    el.querySelector('input[name="view"][value="internet"]').checked = true;
    draw();
    enter();
    part('lp-code-in')?.focus();
    toast(joinNote(r), 'ok');
  });

  function bodyHtml() {
    if (ui.view === 'off') return '';
    if (ui.view === 'lan') {
      return `<div class="lp-part" id="lp-lan"></div>
        ${group('', [row('Paste invite', inviteField()), row(t('Join directly'), fieldAt('lp-join', '192.0.2.10'))])}
        ${ui.lastJoin ? `<datalist id="lp-last"><option value="${esc(ui.lastJoin)}"></option></datalist>` : ''}`;
    }
    const role = segmented('role', [['host', t('Host')], ['visit', t('Visit')]], ui.role, { label: t('Host or visit') });
    if (ui.role === 'host') {
      return `<div>${role}</div>
        ${group('', [
          row(t('Room code'), `<code class="selectable" id="lp-code"></code>${copyButton('')}<button class="btn sm" data-act="new-code">${icon('refresh-cw')}${esc(t('New code'))}</button>`),
        ])}
        <div class="lp-part" id="lp-share"></div>
        ${advancedHtml([
          row(t('Relay address'), fieldAt('lp-relay', t('No relay'))),
          row(t('Run a relay on this PC'), toggle('lp-run-relay', !!relay)),
        ])}`;
    }
    return `<div>${role}</div>
      ${group('', [row(t('Room code'), fieldAt('lp-code-in', 'XXXX-XXXX-XXXX-XXXX'))])}
      ${advancedHtml([
        row(t('Address'), fieldAt('lp-addr', visitPlaceholder())),
        row(t('Through a relay'), toggle('lp-through', ui.through)),
      ])}`;
  }

  // the public relay needs nothing typed; an own relay, this PC's relay or a direct address (forwarded port) live here
  function advancedHtml(ownRows) {
    return `<details class="lp-adv"${advOpen ? ' open' : ''}>
      <summary>${icon('chevron-right')}${esc(t('Advanced'))}</summary>
      ${group('', [row(t('Use my own relay or address'), toggle('lp-own', isOwn())), ...(isOwn() ? ownRows : [])])}
    </details>`;
  }

  // a part is only rewritten when it changed; focus survives a rewrite
  function patch(node, html) {
    if (!node || node.dataset.html === html) return false;
    const a = document.activeElement;
    const key = node.contains(a) ? (a.id ? `#${a.id}` : a.name ? `input[name="${a.name}"]:checked` : null) : null;
    const caret = key && typeof a.selectionStart === 'number' ? [a.selectionStart, a.selectionEnd] : null;
    node.innerHTML = html;
    node.dataset.html = html;
    if (key) {
      const back = node.querySelector(key);
      back?.focus();
      if (caret) { try { back?.setSelectionRange(...caret); } catch { /* not a text field */ } }
    }
    return true;
  }

  const part = (id) => el.querySelector(`#${id}`);

  function drawLan() {
    patch(part('lp-lan'), lanHtml());
    patch(part('lp-share'), shareHtml());
  }

  function drawCode() {
    const code = part('lp-code');
    if (!code) return;
    const shown = shownCode({ gameCode, hostCode: ui.hostCode, running: gameActive(store.state) });
    code.textContent = shown || t('New each time the game starts');
    code.classList.toggle('faint', !shown);
    const button = code.nextElementSibling;
    button.dataset.copy = shown;
    button.disabled = !shown;
    button.setAttribute('aria-label', t('Copy room code'));
  }

  function drawProblems() {
    for (const id of Object.keys(FIELDS)) {
      const input = part(id);
      if (!input) continue;
      const text = problems[id] || '';
      const msg = part(`${id}-msg`);
      msg.textContent = text;
      msg.hidden = !text;
      if (text) {
        input.setAttribute('aria-invalid', 'true');
        input.setAttribute('aria-describedby', msg.id);
      } else {
        input.removeAttribute('aria-invalid');
        input.removeAttribute('aria-describedby');
      }
    }
  }

  function drawChrome() {
    patch(part('lp-banners'), bannersHtml());
    patch(part('lp-path'), pathHtml());
  }

  function draw() {
    drawChrome();
    patch(part('lp-shared'), sharedHtml());
    if (patch(part('lp-body'), bodyHtml())) {
      part('lp-join')?.setAttribute('list', 'lp-last');
      drawLan();
      drawCode();
    }
    patch(part('lp-checks'), checksHtml());
    drawProblems();
  }

  // --- commits

  function checkFormat() {
    if (ui.view === 'lan') problems['lp-join'] = addrProblem(ui.lanJoin, { lan: true, remote: true });
    if (!isOwn()) return;
    if (isHost()) problems['lp-relay'] = addrProblem(ui.relay, { port: RELAY_PORT });
    if (isVisit()) problems['lp-addr'] = addrProblem(ui.visitAddr, { port: ui.through ? RELAY_PORT : HOST_PORT, remote: true });
  }

  /** Applies the page as it stands, if it is complete. `left`: the field just left, so an empty one beside a filled one is called out. */
  async function commit(left) {
    checkFormat();
    if (isVisit()) {
      const code = ui.visitCode.trim();
      problems['lp-code-in'] = '';
      if (code) {
        try {
          ui.visitCode = await api.roomCodeCheck({ code });
          const input = part('lp-code-in');
          if (input) input.value = ui.visitCode;
        } catch (e) { problems['lp-code-in'] = errText(e); }
      }
      if (isOwn() && left === 'lp-addr' && !ui.visitAddr.trim() && code) problems['lp-addr'] = t('Needed');
      if (isOwn() && left === 'lp-code-in' && !code && ui.visitAddr.trim()) problems['lp-code-in'] = t('Needed');
    }
    drawProblems();
    if (Object.values(problems).some(Boolean)) return;

    let edits = editsFor(ui, pkp);
    if (!edits?.length) return;
    if (isHost() && !ui.hostCode) {
      ui.hostCode = mem.hostCode || await api.roomCodeNew();
      edits = editsFor(ui, pkp);
    }
    await save(edits);
    if (ui.view === 'lan' && ui.lanJoin.trim()) remember({ joinAddr: ui.lanJoin.trim() });
    if (isHost()) remember(isOwn() ? { relayAddr: ui.relay.trim(), hostCode: ui.hostCode } : { hostCode: ui.hostCode });
    if (isVisit() && !isOwn()) remember({ visitCode: ui.visitCode });
    else if (isVisit()) {
      const addr = ui.visitAddr.trim();
      remember(ui.through ? { relayAddr: addr, visitCode: ui.visitCode } : { joinAddr: addr, joinCode: ui.visitCode });
      if (ui.through) ui.relay = addr;   // one relay for both roles: Host shows what Visit just set
    }
    drawChrome();
    drawCode();
    patch(part('lp-share'), shareHtml());   // the relay field decides what visitors are given
  }

  function enter() {
    if (ui.view === 'lan' && !lan) loadLan();
    if (isHost()) {
      loadGameCode();
      if (isOwn()) loadPublicIp();
      if (!lan) loadLan();
    }
  }

  const changeView = (view) => inOrder(async () => {
    const was = ui.view;
    ui.view = view;
    problems = {};
    draw();
    enter();
    try { await commit(); } catch (e) {
      ui.view = was;
      el.querySelector(`input[name="view"][value="${was}"]`).checked = true;
      draw();
      throw e;
    }
  });

  const changeRole = (role) => inOrder(async () => {
    ui.role = role;
    remember({ role });
    if (role === 'visit' && ui.through && ui.relay.trim()) ui.visitAddr = ui.relay.trim();
    problems = {};
    draw();
    enter();
    await commit();
  });

  const setOwn = (on) => inOrder(async () => {
    ui.server = on ? 'own' : 'public';
    remember({ server: ui.server });
    if (on && isVisit() && ui.through && ui.relay.trim()) ui.visitAddr = ui.relay.trim();
    problems = {};
    part('lp-body').dataset.html = '';
    draw();
    enter();
    await commit();
  });

  const setRelay = (on) => inOrder(async () => {
    try {
      if (on) {
        relay = await api.relayStart({ port: RELAY_PORT });
        await loadLan();
        if (!ui.relay.trim()) ui.relay = `127.0.0.1:${relayPort(relay)}`;
        await commit();   // also when the field was filled: the file may still say join:<ip>, and the game could not host (#239)
      } else {
        const own = `127.0.0.1:${relayPort(relay)}`;   // left pointing at the relay just stopped, the game could not host
        await api.relayStop();
        relay = null;
        if (ui.relay.trim() === own) {
          ui.relay = '';
          await commit();
        }
      }
    } finally {
      part('lp-body').dataset.html = '';   // the toggle and the field have moved on from the drawn markup
      draw();
    }
  });

  const newCode = () => inOrder(async () => {
    const code = await api.roomCodeNew();
    ui.hostCode = code;
    remember({ hostCode: code });
    dropped = gameCode;
    gameCode = null;
    if (hostsOverInternet(readIni(pkp))) await save(codeEdits(code));
    drawChrome();
    drawCode();
  });

  const setShared = (edits, input) => inOrder(async () => {
    try {
      await save(edits);
      const n = readIni(pkp);
      ui.story = n.story;
      ui.interact = n.interact;
    } catch (e) {
      input.checked = !input.checked;
      throw e;
    }
    patch(part('lp-shared'), sharedHtml());
  });

  const setForward = (on, input) => inOrder(async () => {
    try {
      await save(portForwardEdits(on));
      ui.portForward = readIni(pkp).portForward;
    } catch (e) {
      input.checked = !input.checked;
      throw e;
    }
    patch(part('lp-share'), shareHtml());
  });

  const invitePart = () => ({ input: part('lp-invite'), msg: part('lp-invite-msg') });

  /** A pasted invite fills the page exactly as typing its values would, then the page commits as usual. */
  const applyInvite = (text) => inOrder(async () => {
    const { input, msg } = invitePart();
    if (!input || !String(text).trim()) return;
    const r = parseInvite(text);
    if (msg) {
      msg.textContent = r.ok ? '' : r.error;
      msg.hidden = r.ok;
    }
    if (!r.ok) return;
    Object.assign(ui, inviteToUi(r.invite));
    if (ui.view === 'internet') remember({ role: 'visit' });
    problems = {};
    el.querySelector(`input[name="view"][value="${ui.view}"]`).checked = true;
    draw();
    enter();
    await commit();
    if (Object.values(problems).some(Boolean)) return;   // the field shows what is wrong: no "applied" on top of it
    input.value = '';   // done: leaving the box must not apply the same text twice
    toast(inviteNote(r.invite), 'ok');
  });

  async function copy(text) {
    try {
      await navigator.clipboard.writeText(text);
      toast(t('Copied.'), 'ok');
    } catch { toast(t('Cannot copy here. Select the text and press Ctrl+C.'), 'error'); }
  }

  el.innerHTML = `<div class="page linkplay">
    <div class="lp-head">
      <h1>${esc(t('Link Play'))}</h1>
      ${segmented('view', [['off', t('Off')], ['lan', t('Same network')], ['internet', t('Internet')]], ui.view, { label: t('Link Play') })}
    </div>
    <div class="lp-part" id="lp-banners"></div>
    <div class="lp-path" id="lp-path"></div>
    <div class="lp-part" id="lp-body"></div>
    <div class="lp-part" id="lp-checks"></div>
    <div class="lp-part" id="lp-shared"></div>
  </div>`;

  el.addEventListener('input', (e) => {
    const key = FIELDS[e.target.id];
    if (!key) return;
    ui[key] = e.target.value;
    if (problems[e.target.id]) {
      problems[e.target.id] = '';
      drawProblems();
    }
  });
  el.addEventListener('paste', (e) => {
    if (e.target.id === 'lp-invite') setTimeout(() => applyInvite(e.target.value), 0);
  });
  el.addEventListener('focusout', (e) => {
    if (e.target.id === 'lp-invite') applyInvite(e.target.value);
    if (FIELDS[e.target.id]) inOrder(() => commit(e.target.id));
  });
  el.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && e.target.id === 'lp-invite') applyInvite(e.target.value);
    else if (e.key === 'Enter' && FIELDS[e.target.id]) inOrder(() => commit(e.target.id));
  });
  el.addEventListener('change', (e) => {
    const t = e.target;
    if (t.name === 'view') changeView(t.value);
    else if (t.name === 'role') changeRole(t.value);
    else if (t.id === 'lp-run-relay') setRelay(t.checked);
    else if (t.id === 'lp-own') setOwn(t.checked);
    else if (t.id === 'lp-interact') setShared(interactEdits(t.checked), t);
    else if (t.id === 'lp-story') setShared(storyEdits(t.checked), t);
    else if (t.id === 'lp-through') {
      ui.through = t.checked;
      remember({ through: t.checked });
      part('lp-addr').placeholder = visitPlaceholder();
      inOrder(() => commit());
    }
  });
  // 'toggle' does not bubble: capture it, so a redraw keeps Advanced open or closed as the player left it
  el.addEventListener('toggle', (e) => { if (e.target.classList?.contains('lp-adv')) advOpen = e.target.open; }, true);
  el.addEventListener('click', (e) => {
    const t = e.target.closest('[data-copy], [data-act], [data-invite], [data-friend]');
    if (!t) return;
    if (t.dataset.invite) {
      const made = makeInvite({ addr: t.dataset.invite, code: shownCode({ gameCode, hostCode: ui.hostCode, running: gameActive(store.state) }), through: t.dataset.through === '1' });
      if (made) copy(made.text);
      else toast('An invite needs the room code first: press New code, or start the game as host.', 'error');
    } else if (t.dataset.copy) copy(t.dataset.copy);
    else if (t.dataset.friend) joinFriend(t.dataset.friend);
    else if (t.dataset.act === 'probe') probe();
    else if (t.dataset.act === 'setup-check') runSetupCheck();
    else if (t.dataset.act === 'friends') runFriends();
    else if (t.dataset.act === 'new-code') newCode();
    else if (t.dataset.act === 'switch') inOrder(async () => { await save(protocolEdits()); draw(); });
  });

  ctx.on((s, why) => {
    drawChrome();
    if (why === 'game' && isHost()) loadGameCode();
  });

  const timer = setInterval(async () => {
    if (document.hidden || !isHost()) return;
    const now = await api.relayStatus().catch(() => relay);
    const flipped = !!now !== !!relay;
    relay = now;
    if (flipped) {
      part('lp-body').dataset.html = '';
      draw();
    } else if (relay) {
      const links = part('lp-links');
      if (links) links.textContent = relay.clients;
    }
    loadGameCode();
  }, POLL_MS);

  checkFormat();
  draw();
  enter();
  return () => clearInterval(timer);
}
