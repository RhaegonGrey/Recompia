// The Link Play invite: one line a host copies and a friend pastes, so the address, the room code and the mode travel together. Pure: no DOM,
// no Tauri, so scripts/test-link-invite.mjs runs it with node. It only builds and reads text and the page state; what is written to pkp.ini
// is still decided by linkplay-model.js (`[net]` keys only, the same edits the page makes by hand).
//
//   pokopia-link://join?v=1&a=<ip[:port]>&c=<room code>&r=<relay ip[:port]>&m=<lan|tailscale|internet>
//
//   a  the host's address (LAN or Tailscale or public). With r, the host is reached through that relay and a is not needed.
//   c  the room code; needed for everything except a plain same-network address.
//   r  a coop relay's address, when the game is hosted through one.
//   m  a hint for the person reading it; the receiving launcher works the kind out from the address itself.
import { HOST_PORT, RELAY_PORT, parseAddr, localAddr, loopbackAddr, tailscaleAddr } from './linkplay-model.js';


export const INVITE_SCHEME = 'pokopia-link://join';
export const INVITE_VERSION = 1;

const B32 = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';   // Crockford, as relay/room_code.rs

/** XXXX-XXXX-XXXX-XXXX of what the player typed (case, '-' and spaces do not matter; O is 0, I and L are 1), or null. */
export function normalizeCode(input) {
  let out = '';
  for (const ch of String(input ?? '')) {
    if (ch === '-' || ch === ' ') continue;
    const c = ch.toUpperCase() === 'O' ? '0' : /[IL]/i.test(ch) ? '1' : ch.toUpperCase();
    if (c.length !== 1 || !B32.includes(c) || out.length === 16) return null;
    out += c;
  }
  return out.length === 16 ? out.replace(/(.{4})(?=.)/g, '$1-') : null;
}

/** 'lan' (loopback, private, link-local), 'tailscale' or 'internet', of an address. */
export const addrKind = (s) => (localAddr(s) ? 'lan' : tailscaleAddr(s) ? 'tailscale' : 'internet');

const withPort = (addr, port) => (parseAddr(addr)?.port ? addr : `${addr}:${port}`);

/**
 * The invite for what this PC shares. `addr`: the address friends use (a relay's address when `through`). `code`: the room code (not needed
 * for a plain same-network address). Returns { link, text, kind } or null when something is missing.
 */
export function makeInvite({ addr, code = '', through = false }) {
  const a = parseAddr(addr);
  if (!a || loopbackAddr(addr)) return null;
  const kind = addrKind(addr);
  const norm = normalizeCode(code);
  if ((kind !== 'lan' || through) && !norm) return null;   // a relay always needs the code, whatever its address
  const full = withPort(String(addr).trim(), through ? RELAY_PORT : HOST_PORT);
  const q = [`v=${INVITE_VERSION}`, through ? `r=${full}` : `a=${full}`];
  if (norm && (kind !== 'lan' || through)) q.push(`c=${norm}`);
  q.push(`m=${through ? 'internet' : kind}`);
  const link = `${INVITE_SCHEME}?${q.join('&')}`;
  const how = kind === 'lan' ? 'on the same network' : kind === 'tailscale' && !through ? 'over Tailscale' : 'over the internet';
  const lines = ['Pokopia Link Play invite', `Join ${how}: ${full}`];
  if (norm && (kind !== 'lan' || through)) lines.push(`Room code: ${norm}`);
  lines.push('In the launcher: Link Play, then paste this whole message into Paste invite.', link);
  return { link, text: lines.join('\n'), kind: through ? 'relay' : kind };
}

const LINK_RE = /pokopia-link:\/\/join\?[^\s"'<>]+/i;

/**
 * Reads what the player pasted: the whole message, just the link line, or an `address code` pair. Returns
 * { ok: true, invite: { kind, addr, code, relay, label } } or { ok: false, error } in plain words.
 */
export function parseInvite(input) {
  const text = String(input ?? '').trim();
  if (!text) return { ok: false, error: 'Paste the invite your friend sent.' };
  let addr = '';
  let code = '';
  let relay = '';
  const m = LINK_RE.exec(text);
  if (m) {
    const q = new URLSearchParams(m[0].slice(m[0].indexOf('?') + 1));
    const v = q.get('v');
    if (v !== null && Number(v) !== INVITE_VERSION) return { ok: false, error: 'This invite comes from a newer launcher. Update the launcher, then paste it again.' };
    addr = (q.get('a') || '').trim();
    relay = (q.get('r') || '').trim();
    code = q.get('c') || '';
  } else {
    // "Join over Tailscale: 100.64.0.5:47600" / "Room code: ABCD-..." lines, or just "100.64.0.5 ABCD-EFGH-..."
    const ip = /(\d{1,3}(?:\.\d{1,3}){3}(?::\d{1,5})?)/.exec(text);
    if (ip) addr = ip[1];
    const labelled = /room\s*code\s*[:=]\s*((?:[0-9A-Za-z]{4}[- ]?){3}[0-9A-Za-z]{4})/i.exec(text);
    if (labelled) code = labelled[1];
    else {
      const rest = ip ? text.replace(ip[1], ' ') : text;
      const word = /[0-9A-Za-z]{4}(?:[- ]?[0-9A-Za-z]{4}){3}/.exec(rest);
      if (word) code = word[0];
    }
    if (!addr) return { ok: false, error: 'That does not look like an invite. Paste the whole message your friend sent, or its pokopia-link:// line.' };
  }
  const target = relay || addr;
  const p = parseAddr(target);
  if (!p) return { ok: false, error: `The address in this invite (${target || 'none'}) is not an IPv4 address like 192.0.2.10:${HOST_PORT}.` };
  if (loopbackAddr(target)) return { ok: false, error: 'This invite points at 127.0.0.1, which is your own PC. Ask your friend to send the address from their Link Play page.' };
  const kind = relay ? 'relay' : addrKind(target);
  const norm = code ? normalizeCode(code) : null;
  if (code && !norm) return { ok: false, error: 'The room code in this invite is not valid (it is 16 letters and digits, like ABCD-EFGH-JKMN-PQRS).' };
  if (kind !== 'lan' && !norm) return { ok: false, error: 'This invite has no room code. Ask your friend to copy the invite again, or send you their room code.' };
  const label = kind === 'lan' ? 'Same network' : kind === 'tailscale' ? 'Tailscale' : kind === 'relay' ? 'Through a relay' : 'Internet';
  return { ok: true, invite: { kind, addr: withPort(target, relay ? RELAY_PORT : HOST_PORT), code: norm || '', relay: relay ? withPort(relay, RELAY_PORT) : '', label } };
}

/**
 * The page state an invite means (assign it onto the page's `ui`, then let the page commit as for hand-typed values):
 * same network: Join directly; Tailscale, internet and relay: Internet, Visit, with the address and the code.
 */
export function inviteToUi(invite) {
  if (invite.kind === 'lan') return { view: 'lan', lanJoin: invite.addr };
  if (invite.kind === 'relay') return { view: 'internet', server: 'own', role: 'visit', through: true, visitAddr: invite.relay, visitCode: invite.code, relay: invite.relay };
  return { view: 'internet', server: 'own', role: 'visit', through: false, visitAddr: invite.addr, visitCode: invite.code };
}

/** What the page tells the player after an invite was applied. */
export function inviteNote(invite) {
  if (invite.kind === 'lan') return `Invite applied: join ${invite.addr} on your network.`;
  return `Invite applied: ${invite.label.toLowerCase()}, ${invite.relay || invite.addr}, room code set.`;
}
