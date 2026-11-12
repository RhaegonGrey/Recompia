// Link Play page: pkp.ini values <-> what the page shows, and the edits a choice makes. Pure: no DOM, no Tauri, so `node --test` runs it.
//
// The contract, unchanged from the page before it (runtime/docs/CONFIG.md [net], docs/multiplayer/README.md):
//   mode off, 0 or an empty value          Off. A missing key is auto, like the runtime.
//   mode auto, internet false              Same network. One setting both hosts and visits: the game decides by Invite Others / Visit a Friend.
//   mode join:<ip>[:port], internet false  Same network, Join directly: scanning asks that host only.
//   mode auto, internet true, relay, code  Internet through a relay. Host or Visit is the same file, so the role lives in localStorage.
//   mode auto, internet true, no relay     Internet host without a relay (router forwards 47600). A visitor cannot use this, so it opens as Host.
//   mode join:<ip>, internet true, code    Internet visit without a relay.
//   protocol linkplay                      the old relay Link Play. Any Link Play choice writes protocol = native; the toggles do not (as before).
// Internet goes through PUBLIC_RELAY unless the player turns on Advanced > Use my own relay or address (`server` 'own', kept in
// localStorage while nothing is written yet); a file whose relay is another address, or with no relay, opens in 'own'.
// `[net] server` belongs to the old protocol and is never written here.
import { t } from './i18n.js';

export const HOST_PORT = 47600;
export const RELAY_PORT = 47610;

/** The project's public relay (launcher/relay, run as a service): where Internet hosts open their rooms and visitors find them. IPv4 only:
 *  the game's [net] relay takes no host name (coop_cfg_parse_ex). */
export const PUBLIC_RELAY = '168.138.90.76:47610';

const truthy = (s) => /^(1|true|yes|on)$/i.test((s || '').trim());
const net = (key, value) => ({ file: 'pkp', section: 'net', key, value });
const pkpKey = (key, value) => ({ file: 'pkp', section: 'pkp', key, value });

const IPV4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?::(\d{1,5}))?$/;

/** { ip: [a, b, c, d], port } of `IPv4[:port]`, or null. Refuses what coop_cfg_parse_ex refuses: 0.x, multicast and up, port 0. */
export function parseAddr(s) {
  const m = IPV4.exec(String(s ?? '').trim());
  if (!m) return null;
  const ip = m.slice(1, 5).map(Number);
  if (ip.some((x) => x > 255) || ip[0] === 0 || ip[0] >= 224) return null;
  const port = m[5] === undefined ? null : +m[5];
  if (port !== null && (port < 1 || port > 65535)) return null;
  return { ip, port };
}

export const validAddr = (s) => !!parseAddr(s);

/** coop_addr_allowed for a LAN: loopback, RFC 1918, link-local. With internet off the game turns Link Play off for any other join address. */
export function localAddr(s) {
  const a = parseAddr(s);
  if (!a) return false;
  const [x, y] = a.ip;
  return x === 127 || x === 10 || (x === 172 && y >= 16 && y <= 31) || (x === 192 && y === 168) || (x === 169 && y === 254);
}

/** 100.64.0.0/10: Tailscale's range (and carrier-grade NAT's). The game counts it as internet, so it needs internet play and the host's room code. */
export function tailscaleAddr(s) {
  const a = parseAddr(s);
  return !!a && a.ip[0] === 100 && a.ip[1] >= 64 && a.ip[1] <= 127;
}

/** 127.x: this PC itself. */
export const loopbackAddr = (s) => parseAddr(s)?.ip[0] === 127;

/**
 * What is wrong with a typed address, '' when nothing. `lan`: it is a Join directly address. `remote`: it is another PC's (a visitor
 * typing the host's or the relay's), so 127.x, which means the visitor's own PC, is refused (#245).
 */
export function addrProblem(s, { port = HOST_PORT, lan = false, remote = false } = {}) {
  const v = String(s ?? '').trim();
  if (!v) return '';
  if (!validAddr(v)) return t('Type an address like 192.0.2.10 or 192.0.2.10:{port}', { port });
  if (remote && loopbackAddr(v)) return t('That is your own PC. Type the address the host shares with you.');
  if (lan && tailscaleAddr(v)) return 'That is a Tailscale address. Paste the host\'s invite below, or use Internet, then Visit, with their room code.';
  return lan && !localAddr(v) ? t('Not on your network. Use Internet for this address.') : '';
}

/** The keys this page cares about, read the way the runtime reads them. */
export function readIni(pkp) {
  const ini = (k, d = '') => (pkp[k] ?? d).trim();
  return {
    legacy: ini('net.protocol', 'native').toLowerCase() === 'linkplay',
    mode: ini('net.mode', 'auto').toLowerCase(),
    internet: truthy(ini('net.internet')),
    relay: ini('net.relay'),
    code: ini('net.room_code'),
    interact: truthy(ini('pkp.link_interact_everywhere')),
    story: truthy(ini('pkp.link_story_coop')),
    portForward: truthy(ini('net.port_forward', 'true')),   // the runtime's default: on
  };
}

const isOff = (n) => n.mode === 'off' || n.mode === '0' || !n.mode;
const joinOf = (n) => (n.mode.startsWith('join:') ? n.mode.slice(5) : '');
const realCode = (code) => (code.toLowerCase() === 'new' ? '' : code);   // `new`: the game makes one each run
// the forms of one code are one code (room_code::normalize)
const codeKey = (code) => realCode(code).toUpperCase().replace(/[- ]/g, '').replaceAll('O', '0').replace(/[IL]/g, '1');

export const viewOf = (n) => (isOff(n) ? 'off' : n.internet ? 'internet' : 'lan');

/** Does the file already host over the internet (mode auto, internet on)? New code writes straight into it. */
export const hostsOverInternet = (n) => viewOf(n) === 'internet' && !joinOf(n);

/**
 * The page's state for this file and this browser's memory. `mem` is localStorage: { server, role, through, hostCode, visitCode, joinAddr,
 * joinCode, relayAddr }. `relayAddr` is only ever an own relay: the public one is never remembered, so Advanced opens with the player's own.
 */
export function uiFrom(pkp, mem = {}) {
  const n = readIni(pkp);
  const view = viewOf(n);
  const internet = view === 'internet';
  const join = joinOf(n);
  const direct = internet && !!join;
  const pub = internet && n.relay === PUBLIC_RELAY;
  const server = internet ? (pub ? 'public' : 'own') : mem.server === 'own' ? 'own' : 'public';
  const role = direct ? 'visit' : internet && !n.relay ? 'host' : mem.role === 'visit' ? 'visit' : 'host';
  const through = direct ? false : internet && n.relay && !pub ? true : mem.through !== false;
  const remembered = (s) => (validAddr(s) && s !== PUBLIC_RELAY ? s : '');
  const memRelay = remembered(mem.relayAddr);
  const ownRelay = pub ? '' : n.relay;
  return {
    view,
    server,
    role,
    through,
    legacy: n.legacy,
    lanJoin: view === 'lan' ? join : '',
    lastJoin: remembered(mem.joinAddr),
    hostCode: internet && role === 'host' ? realCode(n.code) : mem.hostCode || '',
    relay: internet && !direct && !pub ? ownRelay : ownRelay || memRelay,
    visitAddr: direct ? join : internet && ownRelay ? ownRelay : through ? memRelay : remembered(mem.joinAddr),
    visitCode: internet && role === 'visit' ? n.code : (through ? mem.visitCode : mem.joinCode) || '',
    interact: n.interact || n.story,
    story: n.story,
    portForward: n.portForward,
  };
}

const choice = (...edits) => [net('protocol', 'native'), ...edits];

export const offEdits = () => choice(net('mode', 'off'));

/** Same network: automatic, or one host's address. */
export const lanEdits = (addr = '') => choice(net('mode', addr ? `join:${addr}` : 'auto'), net('internet', 'false'), net('relay', ''), net('room_code', ''));

export const hostEdits = (relay, code) => choice(net('mode', 'auto'), net('internet', 'true'), net('relay', relay), net('room_code', code));

export const visitEdits = (addr, code, through) => choice(net('mode', through ? 'auto' : `join:${addr}`), net('internet', 'true'), net('relay', through ? addr : ''), net('room_code', code));

export const codeEdits = (code) => choice(net('room_code', code));

export const protocolEdits = () => choice();

/** A host over the internet asks the router to forward its port (UPnP, NAT-PMP or PCP) while it hosts. */
export const portForwardEdits = (on) => [net('port_forward', on ? 'true' : 'false')];

export const interactEdits = (on) => [pkpKey('link_interact_everywhere', on ? 'true' : 'false')];

/** Story on turns the first toggle on too and locks it; story off leaves it as it is. */
export const storyEdits = (on) => (on ? [pkpKey('link_story_coop', 'true'), pkpKey('link_interact_everywhere', 'true')] : [pkpKey('link_story_coop', 'false')]);

export function applyEdits(pkp, edits) {
  const next = { ...pkp };
  for (const e of edits) next[`${e.section}.${e.key}`] = e.value;
  return next;
}

// What the game does with these values: forms that mean the same compare equal, keys it ignores do not count.
function meaning(n) {
  if (isOff(n)) return 'off';
  // auto and auto:<port> are one choice. Anything else (join:<ip>, a bare address, a typo) is its own, so choosing Host or Same network
  // rewrites it: a join:<ip> left in the file makes the game visit-only, and a host there is unreachable (#239)
  const mode = /^auto(:\d+)?$/.test(n.mode) ? 'auto' : n.mode;
  return n.internet ? JSON.stringify([mode, n.relay, codeKey(n.code)]) : mode;
}

/** Would these edits change what the game does? The protocol line alone never counts: it goes with a real choice. */
export const changes = (pkp, edits) => meaning(readIni(pkp)) !== meaning(readIni(applyEdits(pkp, edits)));

/**
 * The edits for what the page shows now: [] when the file already says it, null while a field is empty or invalid, so nothing half-written
 * is saved. A host with no code yet gets [] or edits with an empty code; the page fills it in with room_code_new first.
 */
export function editsFor(ui, pkp) {
  let edits;
  if (ui.view === 'off') edits = offEdits();
  else if (ui.view === 'lan') {
    const addr = ui.lanJoin.trim();
    if (addrProblem(addr, { lan: true, remote: true })) return null;
    edits = lanEdits(addr);
  } else if (ui.server === 'public') {
    if (ui.role === 'host') edits = hostEdits(PUBLIC_RELAY, ui.hostCode);
    else {
      const code = ui.visitCode.trim();
      if (!code) return null;
      edits = visitEdits(PUBLIC_RELAY, code, true);
    }
  } else if (ui.role === 'host') {
    const relay = ui.relay.trim();
    if (addrProblem(relay, { port: RELAY_PORT })) return null;
    edits = hostEdits(relay, ui.hostCode);
  } else {
    const addr = ui.visitAddr.trim();
    const code = ui.visitCode.trim();
    if (!validAddr(addr) || addrProblem(addr, { remote: true }) || !code) return null;
    edits = visitEdits(addr, code, ui.through);
  }
  return changes(pkp, edits) ? edits : [];
}

/** The code visitors need. The game's own (room-code.txt) wins while it runs. Stopped, the file holds the last run: a fixed code in pkp.ini is the next one. */
export const shownCode = ({ gameCode, hostCode, running }) => (running || !hostCode ? gameCode || hostCode || '' : hostCode);

/** Port of a relay_status / relay_start answer. */
export const relayPort = (status) => +String(status?.addr ?? '').split(':').pop() || RELAY_PORT;

/** lan_report as rows to show: { addr, label }, best first. An overlay's label is "internet via Tailscale": the name is enough. */
export function addressRows(report, port) {
  if (!report) return [];
  return [
    ...report.lan.map((a) => ({ addr: `${a.ip}:${port}`, label: a.adapter })),
    ...report.overlay.map((a) => ({ addr: `${a.ip}:${port}`, label: a.label.replace(/^internet via /, '') })),
  ];
}

/**
 * What an internet host gives its visitors: { addr, through, port } or null. `publicIp`: this PC's public address (null: not known),
 * `relayOn`: the port of this PC's relay while it runs. A relay elsewhere is shared as it is; this PC's relay and a host without a relay
 * are reached at the public address, on the relay's port or the host port.
 */
export function internetShare(ui, { publicIp, relayOn }) {
  const relay = ui.relay.trim();
  if (relay && !loopbackAddr(relay)) return { addr: relay, through: true, port: relayPort({ addr: relay }), own: false };
  if (relay && !relayOn) return null;   // pointed at this PC's relay while it is off: nothing to share
  const port = relay ? relayOn : HOST_PORT;
  return { addr: publicIp ? `${publicIp}:${port}` : '', through: !!relay, port, own: true };
}

/** Where to go in the game (the game's own menu names, translated like the rest of the launcher). */
export function pathParts(ui) {
  const invite = t('Invite Others to Visit');
  const visit = t('Visit a Friend');
  const last = ui.view === 'internet' ? (ui.role === 'host' ? invite : visit) : ui.lanJoin.trim() ? visit : `${invite} / ${visit}`;
  return ['Link Play', t('Local connection'), last];
}
