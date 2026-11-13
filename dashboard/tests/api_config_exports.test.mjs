// Unit tests for api.js, config.js, exports.js, format.js, csv.js. Run: node --test tests/
import test from 'node:test';
import assert from 'node:assert/strict';
import { ApiError, feedsUrl, fetchLatest, fetchRange, planWindows, tsTime, MAX_RESULTS } from '../api.js';
import { DEFAULTS, parseVis, resolveConfig, loadSavedConfig, saveConfig, isAllowedBase } from '../config.js';
import { energyCsv, histCsv, rawCsv, runsCsv } from '../exports.js';
import { fmtFixed, fmtSig, fmtSigned, gainWords, fmtDuration, csvNum } from '../format.js';
import { toCsv } from '../csv.js';
import { parseFeed, summarise, runSummaries, DAY_MS } from '../metrics.js';

test('tsTime and planWindows (<= 5 days, no overlap)', () => {
  assert.equal(tsTime(Date.parse('2026-11-20T01:02:03Z')), '2026-11-20 01:02:03');
  const s = Date.parse('2026-11-01T00:00:00Z');
  const w = planWindows(s, s + 12 * DAY_MS - 1000);
  assert.equal(w.length, 3);
  assert.equal(w[1].start, w[0].end + 1000);
  assert.equal(w[2].end, s + 12 * DAY_MS - 1000);
});

test('feedsUrl: UTC, status, %20 spaces, api_key only when a key is set', () => {
  const u = feedsUrl('https://api.thingspeak.com', '3578277', '', { start: Date.parse('2026-11-20T00:00:00Z'), end: Date.parse('2026-11-20T23:59:59Z') });
  assert.match(u, /^https:\/\/api\.thingspeak\.com\/channels\/3578277\/feeds\.json\?/);
  assert.ok(!u.includes('api_key'));
  assert.ok(u.includes('start=2026-11-20%2000%3A00%3A00') || u.includes('start=2026-11-20%2000:00:00'));
  assert.ok(u.includes('timezone=Etc%2FUTC')); assert.ok(u.includes('status=true')); assert.ok(!u.includes('+'));
  assert.ok(feedsUrl('b', '1', 'KEY').includes('api_key=KEY'));
});

function fakeFetch(handler) {
  const calls = [];
  const f = async (url) => { calls.push(url); const { status = 200, body } = handler(url, calls.length); return { ok: status < 400, status, text: async () => body }; };
  f.calls = calls;
  return f;
}

test('fetchRange splits a window that comes back full', async () => {
  const start = Date.parse('2026-11-20T00:00:00Z');
  const fetchImpl = fakeFetch((url, n) => ({ body: JSON.stringify({ channel: {}, feeds: n === 1 ? new Array(MAX_RESULTS).fill({}) : [{ created_at: 'x' }] }) }));
  const feeds = await fetchRange({ base: 'b' }, '1', '', start, start + 2 * DAY_MS - 1000, { fetchImpl });
  assert.equal(fetchImpl.calls.length, 3); // full window -> two halves
  assert.equal(feeds.length, 2);
});

test('read errors are classified: -1 key, 404, network, bad JSON', async () => {
  await assert.rejects(fetchLatest({ base: 'b' }, '1', 'K', 1, { fetchImpl: fakeFetch(() => ({ body: '-1' })) }), (e) => e instanceof ApiError && e.kind === 'key');
  await assert.rejects(fetchLatest({ base: 'b' }, '1', '', 1, { fetchImpl: fakeFetch(() => ({ status: 404, body: 'nf' })) }), (e) => e.kind === 'notfound');
  await assert.rejects(fetchLatest({ base: 'b' }, '1', '', 1, { fetchImpl: async () => { throw new TypeError('offline'); } }), (e) => e.kind === 'network');
  await assert.rejects(fetchLatest({ base: 'b' }, '1', '', 1, { fetchImpl: fakeFetch(() => ({ body: '{"x":1}' })) }), (e) => e.kind === 'format');
});

test('config: defaults point at the real public channels; URL overrides; clamps', () => {
  assert.equal(DEFAULTS.ch1, '3578277'); assert.equal(DEFAULTS.ch2, '3578278'); assert.equal(DEFAULTS.rk1, '');
  const c = resolveConfig('');
  assert.equal(c.demo, false); assert.equal(c.configured, true); assert.equal(c.base, 'https://api.thingspeak.com');
  const d = resolveConfig('?demo=1&refresh=1&days=999&upload=2&now=2026-11-20T12:00:00Z&day=2026-11-19');
  assert.equal(d.demo, true); assert.equal(d.refreshS, 15); assert.equal(d.historyDays, 60); assert.equal(d.uploadMin, 5);
  assert.equal(d.day, '2026-11-19'); assert.equal(d.nowOverrideMs, Date.parse('2026-11-20T12:00:00Z'));
  assert.equal(resolveConfig('?base=http://127.0.0.1:9/').base, 'http://127.0.0.1:9');
});

test('config: MATLAB chart IDs are digits only, known keys ordered', () => {
  assert.deepEqual(parseVis('gain:3,power:1').map((v) => v.key), ['power', 'gain']);
  assert.deepEqual(parseVis('power:1"><img src=x>,energy:javascript:alert(1),angles:12345678901234'), []);
  assert.equal(resolveConfig('?vis=status:9').visList[0].id, '9');
});

test('config: default MATLAB charts only beside the default channels', () => {
  assert.deepEqual(resolveConfig('').visList.map((v) => `${v.key}:${v.id}`),
    ['power:689480', 'energy:689481', 'gain:689485', 'angles:689483', 'status:689484']);
  assert.equal(resolveConfig('?ch1=1001&ch2=1002').visList.length, 0);              // other channels: dropped
  assert.equal(resolveConfig('?base=http://127.0.0.1:9').visList.length, 0);        // other server: dropped
  assert.equal(resolveConfig('?ch1=1001&vis=power:5').visList[0].id, '5');          // explicit vis= kept
  assert.equal(resolveConfig('?ch1=3578277').visList.length, 5);                    // same channel named: kept
  assert.equal(resolveConfig('', { ...DEFAULTS, ch1: '' }, { ch1: '77', ch2: '', base: '', rk1: '', rk2: '' }).visList.length, 0);
});

test('config: a link cannot point the page at another data host (review finding 8)', () => {
  assert.ok(isAllowedBase('https://api.thingspeak.com'));
  assert.ok(isAllowedBase('http://127.0.0.1:18766')); assert.ok(isAllowedBase('http://localhost:8080'));
  assert.ok(!isAllowedBase('https://evil.example')); assert.ok(!isAllowedBase('http://api.thingspeak.com'));
  assert.ok(!isAllowedBase('https://api.thingspeak.com.evil.example')); assert.ok(!isAllowedBase('http://127.0.0.1.evil.example'));
  const c = resolveConfig('?base=https://evil.example&rk1=READ');
  assert.equal(c.base, 'https://api.thingspeak.com'); assert.equal(c.baseRejected, 'https://evil.example');
  assert.equal(resolveConfig('').baseRejected, null);
});

test('saved config: only with no default channel and not in demo mode', () => {
  const mem = new Map(); const storage = { getItem: (k) => mem.get(k) ?? null, setItem: (k, v) => mem.set(k, v), removeItem: (k) => mem.delete(k) };
  assert.equal(saveConfig(storage, { demo: true, ch1: '1' }), false);
  assert.equal(saveConfig(storage, { demo: false, ch1: '77', rk1: '', base: 'b', ch2: '', rk2: '' }), true);
  assert.equal(loadSavedConfig(storage).ch1, '77');
  assert.equal(resolveConfig('', { ...DEFAULTS, ch1: '' }, loadSavedConfig(storage)).ch1, '77');
  assert.equal(resolveConfig('', DEFAULTS, loadSavedConfig(storage)).ch1, '3578277'); // default channel wins
});

test('exports: headers, sim flags, unrounded numbers', () => {
  const f = { created_at: '2026-11-20T10:00:00Z', entry_id: 7, field1: '600.25', field2: '500', field3: '100', field4: '1', field5: '2', field6: '20', field7: '3.9', field8: '11', status: 'TRK;r=3;p=A;e=0;s=1' };
  const e = parseFeed(f);
  const raw = rawCsv([e], '2026-11-20', '2026-11-20');
  assert.equal(raw.header[0], 'created_at_utc'); assert.equal(raw.rows[0][0], '2026-11-20T10:00:00Z');
  assert.equal(raw.rows[0][raw.header.indexOf('sim')], 1); assert.equal(raw.rows[0][raw.header.indexOf('p_trk_mw')], '600.25');
  const runs = runsCsv(runSummaries([e]), 'a', 'b');
  assert.equal(runs.rows[0][runs.header.indexOf('n_sim')], 1);
  const s = summarise([e]);
  const hist = histCsv({ day: 'd', dayHist: s.hist, fromDay: 'a', toDay: 'b', rangeHist: s.hist, runs: [] });
  assert.equal(hist.rows.length, 62); assert.equal(hist.rows[0][1], ''); assert.equal(hist.rows[61][2], '');
  const en = energyCsv([{ day: '2026-11-20', ...s }], 'a', 'b');
  assert.equal(en.header[4], 'e_net_gain_wh');
});

test('format: 3 s.f., 1 dp, signs, words, durations, CSV', () => {
  assert.equal(fmtSig(12.345), '12.3'); assert.equal(fmtSig(0.0045678), '0.00457'); assert.equal(fmtSig(9.9996), '10.0');
  assert.equal(fmtSig(1234), '1230'); assert.equal(fmtSig(0), '0.00'); assert.equal(fmtSig(null), 'n/a');
  assert.equal(fmtFixed(-0.04), '0.0'); assert.equal(fmtSigned(3.21), '+3.2'); assert.equal(fmtSigned(-3.21), '−3.2');
  assert.equal(gainWords(-0.04).cls, 'even'); assert.equal(gainWords(0.06).cls, 'gain');
  assert.equal(fmtDuration(90 * 60_000), '1 hour 30 minutes'); assert.equal(fmtDuration(26 * 3_600_000), '1 day 2 hours');
  assert.equal(csvNum(1 / 3), '0.333333'); assert.equal(csvNum(null), '');
  assert.equal(toCsv(['a', 'b'], [['x,y', 'q"']]), 'a,b\r\n"x,y","q"""\r\n');
});
