// Pure model for the Tailscale friend list on the Link Play page: no DOM, no fetch, no invoke.
import { HOST_PORT } from './linkplay-model.js';

const NOT_FOUND = 'Tailscale was not found on this PC';
const NO_PEERS = 'Tailscale is running, but no other devices are on your tailnet.';
const IPV4 = /^\d{1,3}(\.\d{1,3}){3}$/;

const HINTS = {
  hosting: 'Hosting now',
  idle: 'Online, not hosting Pokopia yet',
  offline: 'Offline',
  unknown: 'Not checked',
};
const ORDER = { hosting: 0, idle: 1, unknown: 1, offline: 2 };

function stateOf(p) {
  if (!p.online) return 'offline';
  if (p.hosting === true) return 'hosting';
  if (p.hosting === false) return 'idle';
  return 'unknown';
}

/** The `tailscale_peers` report as rows for the friend list. */
export function friendRows(report) {
  if (!report || report.found === false) {
    return { ok: false, note: (report && report.error) || NOT_FOUND, rows: [] };
  }
  const rows = [];
  for (const p of report.peers || []) {
    const ip = (p.ips || []).find((s) => IPV4.test(String(s)));
    if (!ip) continue;
    const state = stateOf(p);
    rows.push({
      key: p.dns_name || p.name || ip,
      name: p.name || p.dns_name || ip,
      dns: p.dns_name || '',
      addr: `${ip}:${HOST_PORT}`,
      state,
      active: state === 'hosting',
      hint: HINTS[state],
    });
  }
  rows.sort((a, b) => ORDER[a.state] - ORDER[b.state] || a.name.toLowerCase().localeCompare(b.name.toLowerCase()));
  return { ok: true, note: rows.length ? '' : NO_PEERS, rows };
}

/** The Link Play page state a click on Join means. The room code is not known here, the page asks for it. */
export const joinUi = (row) => ({ view: 'internet', server: 'own', role: 'visit', through: false, visitAddr: row.addr, visitCode: '' });

export const joinNote = (row) => `Now paste the invite from ${row.name} (or type their room code) to finish.`;
