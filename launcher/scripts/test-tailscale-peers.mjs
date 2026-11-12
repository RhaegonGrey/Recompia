// Run: node scripts/test-tailscale-peers.mjs (no browser, no Tauri). The Tailscale friend list model.
import assert from 'node:assert/strict';
import { editsFor, uiFrom } from '../src/linkplay-model.js';
import { friendRows, joinNote, joinUi } from '../src/tailscale-model.js';

// not found
let r = friendRows({ found: false, error: 'Tailscale is not installed on this PC.' });
assert.equal(r.ok, false);
assert.ok(r.note.includes('not installed'));
assert.deepEqual(r.rows, []);
r = friendRows({ found: false });
assert.equal(r.ok, false);
assert.equal(r.note, 'Tailscale was not found on this PC');
assert.equal(friendRows(null).ok, false);

// zero peers
r = friendRows({ found: true, self: ['100.64.0.1'], peers: [] });
assert.equal(r.ok, true);
assert.equal(r.note, 'Tailscale is running, but no other devices are on your tailnet.');
assert.deepEqual(r.rows, []);

// ordering and states
const peer = (name, ip, online, hosting) => ({ name, dns_name: `${name}.tail.ts.net`, ips: [ip], online, hosting });
r = friendRows({ found: true, self: [], peers: [
  peer('zed', '100.64.0.9', false, null), // online: false
  peer('bob', '100.64.0.2', true, false), // hosting: false
  peer('Amy', '100.64.0.3', true, null), // hosting: null
  peer('yan', '100.64.0.4', true, true), // hosting: true
  peer('Cat', '100.64.0.5', true, true), // hosting: true
  peer('dan', '100.64.0.6', false, true), // online: false
] });
assert.deepEqual(r.rows.map((x) => x.name), ['Cat', 'yan', 'Amy', 'bob', 'dan', 'zed']);
assert.deepEqual(r.rows.map((x) => x.state), ['hosting', 'hosting', 'unknown', 'idle', 'offline', 'offline']);
assert.deepEqual(r.rows.map((x) => x.active), [true, true, false, false, false, false]);
assert.equal(r.rows[0].addr, '100.64.0.5:47600');
assert.equal(r.rows[0].hint, 'Hosting now');
assert.equal(r.rows[2].hint, 'Not checked');
assert.equal(r.rows[3].hint, 'Online, not hosting Pokopia yet');
assert.equal(r.rows[4].hint, 'Offline');
assert.equal(r.note, '');

// IPv6 only is skipped; an IPv4 later in the list is used
r = friendRows({ found: true, peers: [
  { name: 'six', dns_name: 'six.t', ips: ['fd7a:115c:a1e0::1'], online: true, hosting: true },
  { name: 'both', dns_name: 'both.t', ips: ['fd7a:115c:a1e0::2', '100.64.0.7'], online: true, hosting: true },
] });
assert.deepEqual(r.rows.map((x) => x.name), ['both']);
assert.equal(r.rows[0].addr, '100.64.0.7:47600');

// joinUi
const row = friendRows({ found: true, peers: [peer('amy', '100.64.0.3', true, true)] }).rows[0];
assert.deepEqual(joinUi(row), { view: 'internet', server: 'own', role: 'visit', through: false, visitAddr: '100.64.0.3:47600', visitCode: '' });
assert.equal(joinNote(row), 'Now paste the invite from amy (or type their room code) to finish.');
assert.equal(editsFor({ ...uiFrom({}, {}), ...joinUi(row) }, ''), null, 'the page waits for the room code');

for (const s of [joinNote(row), ...r.rows.map((x) => x.hint)]) assert.ok(!/[–—]/.test(s));
console.log('tailscale peers ok');
