// Unit tests for metrics.js (contract v1.5 section 6). Run: node --test tests/
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  parseStatus, parseFeed, parseResultFeed, uniqueEntries, summarise, dailySummaries, runSummaries, panelSummaries,
  calibrationSummary, cumulativeSeries, matlabAgreement, latestResultsByDate, expectedForDay, histIndex, histLabels,
  stateSegments, stateTotals, isStale, staleLimitMs, decimateMinMax, lineWithBreaks, utcDay, addDays, dayStartMs,
  runKind, dailyGainSeries, HIST_BINS, MIN_MS, DAY_MS,
} from '../metrics.js';

const T0 = Date.parse('2026-11-20T00:00:00Z');
let nextId = 1;
/** Build a channel-1 feed object like ThingSpeak returns (strings, null for missing). */
function feed(minute, o = {}) {
  const f = {
    created_at: new Date(T0 + minute * MIN_MS).toISOString().replace('.000Z', 'Z'),
    entry_id: o.entry_id ?? nextId++,
    field1: o.p1 === undefined ? '600.0' : o.p1, field2: o.p2 === undefined ? '500.0' : o.p2,
    field3: o.p3 === undefined ? '100.0' : o.p3,
    field4: o.ap === undefined ? '10.0' : o.ap, field5: o.as === undefined ? '12.0' : o.as,
    field6: '20.0', field7: '3.900', field8: o.seq === undefined ? String(minute + 1) : o.seq,
    status: o.status ?? `${o.state ?? 'TRK'};r=${o.run ?? 0};p=${o.panel ?? 'A'};e=${o.e ?? 0}${o.sim ? ';s=1' : ''}`,
  };
  return f;
}
const E = (minute, o) => parseFeed(feed(minute, o));
const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

test('parseStatus: grammar, unknown keys ignored, sim flag', () => {
  assert.deepEqual(parseStatus('TRK;r=12;p=A;e=0'), { state: 'TRK', run: 12, panel: 'A', fault: 0, sim: false });
  assert.deepEqual(parseStatus('MOV;r=3;p=B;e=8;s=1;zz=9'), { state: 'MOV', run: 3, panel: 'B', fault: 8, sim: true });
  assert.equal(parseStatus('').state, null);
  assert.equal(parseStatus(null).run, 0);
  assert.equal(parseStatus('HLD;r=x;e=').run, 0); // non-integer -> 0
});

test('parseFeed: strings to numbers, empty/null to null', () => {
  const e = parseFeed({ created_at: '2026-11-20T10:00:00Z', entry_id: 5, field1: '12.5', field2: null, field3: '', field8: '42', status: 'IDL;r=0;p=A;e=0' });
  assert.equal(e.pTrk, 12.5); assert.equal(e.pFix, null); assert.equal(e.pUse, null);
  assert.equal(e.seq, 42); assert.equal(e.state, 'IDL'); assert.equal(e.t, Date.parse('2026-11-20T10:00:00Z'));
});

test('uniqueEntries: keeps the lowest entry_id per seq, keeps all entries without seq', () => {
  const es = [E(0, { entry_id: 10, seq: '1', p1: '1' }), E(0, { entry_id: 3, seq: '1', p1: '2' }), E(1, { entry_id: 4, seq: null }), E(1, { entry_id: 5, seq: null })];
  const u = uniqueEntries(es);
  assert.equal(u.length, 3);
  assert.equal(u.find((e) => e.seq === 1).pTrk, 2, 'entry_id 3 wins over 10');
});

test('uniqueEntries: same seq at another minute is a new sample (seq restart, review finding 3)', () => {
  // seq 1-3 on one day, the device's flash is formatted, seq 1-3 again two days later.
  const es = [E(0, { seq: '1' }), E(1, { seq: '2' }), E(2, { seq: '3' }),
    E(2880, { seq: '1' }), E(2881, { seq: '2' }), E(2882, { seq: '3' }),
    E(2882, { seq: '3', entry_id: 999999 }) /* a real re-sent copy: same seq, same minute */];
  const u = uniqueEntries(es);
  assert.equal(u.length, 6);
  const runs = runSummaries(es.map((e) => ({ ...e, run: 4 })));
  assert.equal(runs[0].nUnique, 6); assert.equal(runs[0].duplicates, 1);
  close(runs[0].eTrk, 6 * 600 / 60000);
  const days = dailySummaries(es, T0 + 4 * DAY_MS);
  assert.equal(days.find((d) => d.day === '2026-11-20').nUnique, 3);
  assert.equal(days.find((d) => d.day === '2026-11-22').nUnique, 3);
});

test('parseFeed: number rule is the same as Python and MATLAB (review finding 4)', () => {
  const p1 = (v) => E(0, { p1: v }).pTrk;
  assert.equal(p1('  '), null); assert.equal(p1('0x10'), null); assert.equal(p1('NaN'), null);
  assert.equal(p1('Infinity'), null); assert.equal(p1('1e'), null); assert.equal(p1('.'), null);
  assert.equal(p1(' 12.5 '), 12.5); assert.equal(p1('1e1'), 10); assert.equal(p1('-.5'), -0.5); assert.equal(p1('+3.'), 3);
  assert.equal(parseFeed({ ...feed(0), field1: 7 }).pTrk, 7);  // JSON number
});

test('summarise: energy formula, gains, counts over the unique set (v1.5)', () => {
  const es = [E(0), E(1), E(1, { seq: '2' }) /* duplicate of minute 1 */, E(2, { sim: true, state: 'MOV' })];
  const s = summarise(es, null);
  assert.equal(s.n, 4); assert.equal(s.nUnique, 3); assert.equal(s.duplicates, 1);
  close(s.eTrk, 3 * 600 * 60 / 3600 / 1000); close(s.eFix, 3 * 500 / 60000); close(s.eUse, 3 * 100 / 60000);
  close(s.gGross, 100 * (600 - 500) / 500); close(s.gNet, 100 * (600 - 500 - 100) / 500);
  assert.equal(s.moves, 1); assert.equal(s.nSim, 1); assert.equal(s.nPointing, 3); close(s.mae, 2);
  assert.equal(s.complete, 100); assert.equal(s.expected, 3);
});

test('summarise: no fixed energy -> gains null; empty set', () => {
  const s = summarise([E(0, { p2: '0.0' })], 1440);
  assert.equal(s.gGross, null); assert.equal(s.gNet, null);
  const z = summarise([], 1440);
  assert.equal(z.complete, 0); assert.equal(z.n, 0); assert.equal(z.mae, null);
  assert.equal(summarise([], null).complete, null);
});

test('summarise: pointing error only in TRK/MOV/HLD with both angles', () => {
  const es = [E(0, { state: 'IDL' }), E(1, { state: 'STW' }), E(2, { state: 'FLT' }), E(3, { as: '' }), E(4, { state: 'HLD', ap: '0', as: '-5' })];
  const s = summarise(es, null);
  assert.equal(s.nPointing, 1); close(s.mae, 5);
});

test('histogram bin edges: -30 underflow, [−30,−29) first, 30 overflow', () => {
  assert.equal(histIndex(-30.0001), 0); assert.equal(histIndex(-30), 1); assert.equal(histIndex(-29.0001), 1);
  assert.equal(histIndex(0), 31); assert.equal(histIndex(29.999), 60); assert.equal(histIndex(30), 61);
  assert.equal(histLabels().length, HIST_BINS);
});

test('gaps and missing minutes', () => {
  const s = summarise([E(0), E(1), E(5), E(6), E(10)], 1440);
  assert.equal(s.gaps, 2); assert.equal(s.missingMin, 3 + 3); assert.equal(s.longestGapMin, 3);
  close(s.complete, 100 * 5 / 1440);
});

test('expectedForDay: past day 1440, today up to newest sample, future 0', () => {
  const day = '2026-11-20';
  assert.equal(expectedForDay(day, T0 + 2 * DAY_MS), 1440);
  assert.equal(expectedForDay(day, T0 + 600 * MIN_MS, T0 + 599 * MIN_MS), 600);
  assert.equal(expectedForDay(day, T0 + 600 * MIN_MS, null), 0);
  assert.equal(expectedForDay(day, T0 - 1), 0);
});

test('dailySummaries split at UTC midnight', () => {
  const es = [E(1438), E(1439), E(1440), E(1441)];
  const days = dailySummaries(es, T0 + 3 * DAY_MS);
  assert.deepEqual(days.map((d) => [d.day, d.n]), [['2026-11-20', 2], ['2026-11-21', 2]]);
  assert.equal(days[0].expected, 1440);
});

test('runs: test vs calibration, grouped by run and panel', () => {
  const es = [E(0, { run: 5, panel: 'A' }), E(1, { run: 5, panel: 'A' }), E(2, { run: 900, panel: 'B' }), E(3, { run: 0 })];
  const runs = runSummaries(es);
  assert.deepEqual(runs.map((r) => [r.run, r.panel, r.kind, r.nUnique]), [[5, 'A', 'test', 2], [900, 'B', 'calibration', 1]]);
  assert.equal(runKind(0), null); assert.equal(runKind(899), 'test'); assert.equal(runKind(999), 'calibration');
});

test('panel A/B totals exclude calibration runs and duplicates', () => {
  const es = [E(0, { run: 1, panel: 'A' }), E(0, { run: 1, panel: 'A', seq: '1' }), E(1, { run: 2, panel: 'B' }), E(2, { run: 900, panel: 'A' })];
  const p = panelSummaries(es);
  assert.deepEqual(p.map((x) => [x.panel, x.n, x.runs]), [['A', 1, 1], ['B', 1, 1]]);
});

test('calibration summary: mismatch is a panel difference', () => {
  const c = calibrationSummary([E(0, { run: 901, p1: '102', p2: '100' }), E(1, { run: 901, p1: '102', p2: '100' })]);
  assert.equal(c.runs, 1); close(c.mismatch, 2, 1e-9);
  assert.equal(calibrationSummary([E(0)]), null);
});

test('cumulativeSeries ends at the summarised energies', () => {
  const es = [E(0), E(1), E(1, { seq: '2' }), E(2)];
  const cum = cumulativeSeries(es); const s = summarise(es);
  assert.equal(cum.length, 3); close(cum[2].eTrk, s.eTrk); close(cum[2].gNet, s.gNet);
});

test('channel 2: v1.5 status, newest row per date, agreement rule on N unique', () => {
  const r = parseResultFeed({ created_at: '2026-11-21T00:20:00Z', entry_id: 9, field1: '10.0000', field2: '9.00000', field3: '0.500000',
    field4: '11.1111', field5: '5.5556', field6: '2.0000', field7: '100.0000', field8: '1440', status: 'period=day;date=2026-11-20;v=1.5;ns=3;nd=2' });
  assert.equal(r.date, '2026-11-20'); assert.equal(r.version, '1.5'); assert.equal(r.nSim, 3); assert.equal(r.nDup, 2); assert.equal(r.n, 1440);
  const older = { ...r, t: r.t - 86_400_000, eTrk: 1 };
  assert.equal(latestResultsByDate([older, r]).get('2026-11-20').eTrk, 10);
  const local = { eTrk: 10.0004, eFix: 9.0001, eUse: 0.50004, gGross: 11.11, gNet: 5.56, mae: 2.04, complete: 100, n: 1442, nUnique: 1440 };
  assert.equal(matlabAgreement(local, r).status, 'agree');
  assert.equal(matlabAgreement({ ...local, nUnique: 1439 }, r).status, 'differ');
  assert.match(matlabAgreement({ ...local, eTrk: 10.06 }, r).reason, /tracked Wh/);
  assert.equal(matlabAgreement(local, undefined).status, 'missing');
});

test('state timeline: contiguous segments, gaps as GAP, totals', () => {
  const es = [E(0, { state: 'IDL' }), E(1, { state: 'IDL' }), E(2, { state: 'TRK' }), E(6, { state: 'TRK' }), E(7, { state: 'MOV' })];
  const segs = stateSegments(es);
  assert.deepEqual(segs.map((s) => [s.state, s.minutes]), [['IDL', 2], ['TRK', 1], ['GAP', 3], ['TRK', 1], ['MOV', 1]]);
  assert.equal(segs[2].start, T0 + 3 * MIN_MS); assert.equal(segs[2].end, T0 + 6 * MIN_MS);
  const tot = Object.fromEntries(stateTotals(segs).map((r) => [r.state, r.minutes]));
  assert.equal(tot.TRK, 2); assert.equal(tot.GAP, 3); assert.equal(tot.FLT, 0);
});

test('stale rule: 2 upload intervals + 5 min', () => {
  assert.equal(staleLimitMs(15), 35 * MIN_MS);
  assert.equal(isStale(T0, T0 + 35 * MIN_MS, 15), false);
  assert.equal(isStale(T0, T0 + 35 * MIN_MS + 1, 15), true);
  assert.equal(isStale(null, T0, 15), true);
});

test('display helpers keep peaks and break lines at gaps', () => {
  const pts = Array.from({ length: 5000 }, (_, i) => ({ x: i, y: i === 2500 ? 1e6 : i % 7 }));
  const d = decimateMinMax(pts, 1000);
  assert.ok(d.length <= 1002); assert.ok(d.some((p) => p.y === 1e6)); assert.equal(d[0], pts[0]);
  const l = lineWithBreaks([E(0), E(1), E(5)], (e) => e.pTrk);
  assert.equal(l.filter((p) => p.y === null).length, 1);
});

test('day helpers', () => {
  assert.equal(utcDay(T0 + DAY_MS - 1), '2026-11-20'); assert.equal(addDays('2026-11-30', 1), '2026-12-01');
  assert.equal(addDays('2028-03-01', -1), '2028-02-29'); assert.equal(dayStartMs('2026-11-20'), T0);
  assert.deepEqual(dailyGainSeries([{ day: 'a', gNet: null }, { day: 'b', gNet: 1, gGross: 2, n: 3, nSim: 0 }]).map((r) => r.day), ['b']);
});
