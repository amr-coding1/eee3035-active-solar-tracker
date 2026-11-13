/**
 * AST metrics: pure functions implementing CONTRACT.md section 6 (v1.5).
 *
 * No DOM, no network, no clock: everything a function needs is passed in, so
 * the same file runs in the browser and under `node --test`.
 * The Python reference with the same formulas is analysis/py/ast_metrics.py.
 */
import { fmtFixed, fmtSig } from './format.js';

export const DT_S = 60;                       // nominal sample interval [s]
export const MIN_MS = 60_000;
export const DAY_MS = 86_400_000;
export const HIST_LO = -30;
export const HIST_HI = 30;
export const HIST_BINS = HIST_HI - HIST_LO + 2; // underflow + 60 + overflow = 62
export const TRACKING_STATES = new Set(['TRK', 'MOV', 'HLD']);
const WH_PER_MW_SAMPLE = DT_S / 3600 / 1000;  // one sample of 1 mW = this many Wh
export const CAL_RUN_MIN = 900;               // run IDs 900-999 = calibration runs (contract section 5, v1.4)
export const CAL_RUN_MAX = 999;

export const STATE_WORDS = {
  TRK: 'Tracking',
  MOV: 'Moving',
  HLD: 'Holding (sensor uncertain)',
  IDL: 'Idle (dark)',
  STW: 'Stowed',
  FLT: 'Fault',
};

/** Timeline label for a STATE or 'GAP' (minutes with no sample). */
export const TIMELINE_WORDS = { ...STATE_WORDS, GAP: 'No data (gap)' };

export const FAULT_WORDS = {
  0: 'No fault',
  1: 'Tracked-panel power sensor missing',
  2: 'Fixed-panel power sensor missing',
  3: 'Consumption power sensor missing',
  4: 'Clock (RTC) missing or lost time',
  5: 'Sun-sensor arc fault',
  6: 'Local buffer overflow (data lost)',
  7: 'Limit switch hit unexpectedly',
  8: 'Motor stall or timeout',
  9: 'Upload failing for over 60 minutes',
};

/* ------------------------------------------------------------------ parsing */

// Same rule as the Python reference and MATLAB (NUM_PATTERN): optional sign,
// digits with an optional point, optional exponent, surrounding white space.
// '  ', '0x10', 'NaN', 'Infinity' are not numbers (review finding 4).
const NUM_RE = /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/;
function num(v) {
  if (v === null || v === undefined || typeof v === 'boolean') return null;
  if (typeof v === 'number') return Number.isFinite(v) ? v : null;
  const s = String(v).trim();
  if (!NUM_RE.test(s)) return null;
  const x = Number(s);
  return Number.isFinite(x) ? x : null;
}

function intOr0(v) {
  return /^-?\d+$/.test(v) ? parseInt(v, 10) : 0;
}

/**
 * 'TRK;r=12;p=A;e=0' -> { state:'TRK', run:12, panel:'A', fault:0, sim:false }.
 * 's=1' (v1.4) marks a sample made by the firmware simulator; unknown keys are ignored.
 */
export function parseStatus(s) {
  const out = { state: null, run: 0, panel: null, fault: 0, sim: false };
  if (!s) return out;
  const parts = String(s).split(';');
  out.state = parts[0] || null;
  for (const p of parts.slice(1)) {
    const i = p.indexOf('=');
    if (i < 0) continue;
    const k = p.slice(0, i);
    const v = p.slice(i + 1);
    if (k === 'r') out.run = intOr0(v);
    else if (k === 'p') out.panel = v || null;
    else if (k === 'e') out.fault = intOr0(v);
    else if (k === 's') out.sim = v === '1';
  }
  return out;
}

/** One ThingSpeak channel-1 feed object -> entry used by every metric. */
export function parseFeed(f) {
  const st = parseStatus(f.status);
  const seq = num(f.field8);
  return {
    t: Date.parse(f.created_at),
    entryId: f.entry_id ?? null,
    pTrk: num(f.field1),
    pFix: num(f.field2),
    pUse: num(f.field3),
    angPanel: num(f.field4),
    angSun: num(f.field5),
    temp: num(f.field6),
    vbat: num(f.field7),
    seq: seq === null ? null : Math.trunc(seq),
    state: st.state,
    run: st.run,
    panel: st.panel,
    fault: st.fault,
    sim: st.sim,
    status: f.status ?? null,
  };
}

/**
 * Channel-2 (results) feed -> one MATLAB summary row. `date` is the summarised
 * UTC day for a `period=day;date=YYYY-MM-DD` entry, otherwise null.
 * v1.5 status also carries v (contract version), ns (simulated samples) and
 * nd (duplicates dropped); field8 is N, the number of UNIQUE samples.
 */
export function parseResultFeed(f) {
  const kv = {};
  for (const p of String(f.status || '').split(';')) {
    const i = p.indexOf('=');
    if (i > 0) kv[p.slice(0, i)] = p.slice(i + 1);
  }
  let label = utcDay(Date.parse(f.created_at));
  if (kv.period === 'day' && kv.date) label = kv.date;
  else if (kv.period === 'run') label = `Run ${kv.r ?? '?'}`;
  return {
    t: Date.parse(f.created_at),
    entryId: f.entry_id ?? null,
    period: kv.period || 'day',
    date: kv.period === 'day' && /^\d{4}-\d{2}-\d{2}$/.test(kv.date || '') ? kv.date : null,
    label,
    version: kv.v ?? null,
    nSim: /^\d+$/.test(kv.ns ?? '') ? parseInt(kv.ns, 10) : null,
    nDup: /^\d+$/.test(kv.nd ?? '') ? parseInt(kv.nd, 10) : null,
    eTrk: num(f.field1),
    eFix: num(f.field2),
    eUse: num(f.field3),
    gGross: num(f.field4),
    gNet: num(f.field5),
    mae: num(f.field6),
    complete: num(f.field7),
    n: num(f.field8),
  };
}

/* -------------------------------------------------------------------- time */

/** ms epoch -> 'YYYY-MM-DD' of the UTC day containing it. */
export function utcDay(ms) {
  return new Date(ms).toISOString().slice(0, 10);
}

/** 'YYYY-MM-DD' -> ms epoch of 00:00:00 UTC on that day. */
export function dayStartMs(day) {
  return Date.parse(`${day}T00:00:00Z`);
}

export function addDays(day, n) {
  return utcDay(dayStartMs(day) + n * DAY_MS);
}

/**
 * Expected entries for one UTC day (contract section 6, today's rule v1.4):
 * 1440 for a past day; for the current day the whole minutes from 00:00 UTC to
 * the newest received entry of that day, inclusive (0 when none has arrived
 * yet); 0 for a day that has not started. Upload lag is shown by the
 * stale-data indicator, not by completeness.
 * @param {string} day  'YYYY-MM-DD'
 * @param {number} nowMs  the clock, only used to tell a past day from today
 * @param {number|null} newestMsOfDay  time of the newest entry received for `day`
 */
export function expectedForDay(day, nowMs, newestMsOfDay = null) {
  const start = dayStartMs(day);
  if (nowMs >= start + DAY_MS) return 1440;
  if (nowMs < start) return 0;
  if (newestMsOfDay === null || newestMsOfDay === undefined || newestMsOfDay < start) return 0;
  return Math.min(1440, Math.floor((newestMsOfDay - start) / MIN_MS) + 1);
}

/** Run type from the run ID: 900-999 are calibration runs, every other ID above 0 is a tracking test. */
export function runKind(run) {
  if (!(run > 0)) return null;
  return run >= CAL_RUN_MIN && run <= CAL_RUN_MAX ? 'calibration' : 'test';
}

/* ----------------------------------------------------------------- metrics */

/** 0 = underflow (< -30), 1..60 = [-30+k-1, -30+k), 61 = overflow (>= 30). */
export function histIndex(err) {
  if (err < HIST_LO) return 0;
  if (err >= HIST_HI) return HIST_BINS - 1;
  return Math.floor(err - HIST_LO) + 1;
}

export function histLabels() {
  const labels = [`<${HIST_LO}`];
  for (let b = HIST_LO; b < HIST_HI; b++) labels.push(`[${b},${b + 1})`);
  labels.push(`>=${HIST_HI}`);
  return labels;
}

/** Pointing error of one entry, or null where the contract says not to count it. */
export function pointingError(e) {
  if (e.angSun === null || e.angPanel === null || !TRACKING_STATES.has(e.state)) return null;
  return e.angSun - e.angPanel;
}

/**
 * De-duplication (contract section 6.1, v1.5). Entries with the same `seq` AND
 * the same created_at are copies of one sample (a batch re-sent after a lost
 * response); only the first received copy (lowest entry_id) is kept. Same seq
 * at a different minute is a different sample (the device's seq restarted,
 * e.g. after its flash was formatted), so ranges of many days stay right
 * (review finding 3). Entries without a seq are all kept. Returns the unique
 * entries in time order.
 */
export function uniqueEntries(entries) {
  const order = [...entries].sort((a, b) =>
    (a.entryId ?? Infinity) - (b.entryId ?? Infinity) || a.t - b.t);
  const seen = new Set();
  const out = [];
  for (const e of order) {
    if (e.seq !== null && e.seq !== undefined) {
      const key = `${e.seq}|${e.t}`;
      if (seen.has(key)) continue;
      seen.add(key);
    }
    out.push(e);
  }
  return out.sort((a, b) => a.t - b.t || (a.entryId ?? 0) - (b.entryId ?? 0));
}

/**
 * Summarise a set of entries (contract section 6, v1.5).
 * `n` counts every entry received; every other figure (energies, gains,
 * pointing error, histogram, moves, simulated and quality counts, gaps,
 * completeness) uses the de-duplicated set, whose size is `nUnique` (= N).
 * @param {Array} entries  parsed entries (any order)
 * @param {number|null} expected  minute slots in the period; null = first to
 *        last received entry inclusive (rule for a run or partial period)
 */
export function summarise(entries, expected = null) {
  const es = uniqueEntries(entries);
  const s = {
    n: entries.length, nUnique: es.length, expected: expected ?? 0,
    eTrk: 0, eFix: 0, eUse: 0,
    gGross: null, gNet: null, mae: null, complete: null,
    moves: 0, gaps: 0, longestGapMin: 0, missingMin: 0,
    missingPowerFields: 0, negativePower: 0, nSim: 0, duplicates: entries.length - es.length, nPointing: 0,
    hist: new Array(HIST_BINS).fill(0),
    first: null, last: null,
  };
  if (!es.length) {
    s.complete = expected ? 0 : null;
    return s;
  }
  s.first = es[0].t;
  s.last = es[es.length - 1].t;

  let sumTrk = 0, sumFix = 0, sumUse = 0, sumAbsErr = 0;
  for (const e of es) {
    if (e.pTrk === null || e.pFix === null || e.pUse === null) s.missingPowerFields++;
    if (e.pTrk < 0 || e.pFix < 0 || e.pUse < 0) s.negativePower++; // null < 0 is false; values are still summed as received
    if (e.sim) s.nSim++;
    sumTrk += e.pTrk ?? 0;
    sumFix += e.pFix ?? 0;
    sumUse += e.pUse ?? 0;
    if (e.state === 'MOV') s.moves++;
    const err = pointingError(e);
    if (err !== null) {
      sumAbsErr += Math.abs(err);
      s.nPointing++;
      s.hist[histIndex(err)]++;
    }
  }
  s.eTrk = sumTrk * WH_PER_MW_SAMPLE;
  s.eFix = sumFix * WH_PER_MW_SAMPLE;
  s.eUse = sumUse * WH_PER_MW_SAMPLE;
  if (s.eFix > 0) {
    s.gGross = 100 * (s.eTrk - s.eFix) / s.eFix;
    s.gNet = 100 * (s.eTrk - s.eFix - s.eUse) / s.eFix;
  }
  if (s.nPointing) s.mae = sumAbsErr / s.nPointing;

  // Gaps: missing minutes between consecutive (unique) timestamps.
  let prev = null;
  for (const e of es) {
    if (prev !== null && e.t !== prev) {
      const miss = Math.round((e.t - prev) / MIN_MS) - 1;
      if (miss > 0) {
        s.gaps++;
        s.missingMin += miss;
        if (miss > s.longestGapMin) s.longestGapMin = miss;
      }
    }
    prev = e.t;
  }
  if (expected === null) s.expected = Math.round((s.last - s.first) / MIN_MS) + 1;
  s.complete = s.expected ? 100 * s.nUnique / s.expected : null;
  return s;
}

/** Map 'YYYY-MM-DD' -> entries of that UTC day (insertion order = day order if input is sorted). */
export function groupByDay(entries) {
  const m = new Map();
  for (const e of entries) {
    const d = utcDay(e.t);
    let a = m.get(d);
    if (!a) m.set(d, (a = []));
    a.push(e);
  }
  return new Map([...m].sort((a, b) => (a[0] < b[0] ? -1 : 1)));
}

/** Per-UTC-day summaries: [{ day, ...summary }] oldest first. Today is expected up to its newest entry. */
export function dailySummaries(entries, nowMs) {
  return [...groupByDay(entries)].map(([day, es]) => {
    const newest = es.reduce((m, e) => (e.t > m ? e.t : m), -Infinity);
    return { day, ...summarise(es, expectedForDay(day, nowMs, newest)) };
  });
}

/**
 * Bars for the daily net gain chart: one row per UTC day that has fixed-panel
 * energy (a day with E_fix <= 0 has no defined gain and is left out).
 */
export function dailyGainSeries(days) {
  return days.filter((d) => d.gNet !== null).map((d) => ({ day: d.day, gNet: d.gNet, gGross: d.gGross, n: d.n, nSim: d.nSim }));
}

/* ------------------------------------------- channel-2 (MATLAB) cross-check */

/**
 * Channel 2 is re-written every night for the last 9 complete days (contract
 * section 3.2, v1.5): keep, for each summarised date, the row with the newest created_at.
 * Rows that are not `period=day;date=...` are ignored.
 * @returns {Map<string, object>} date -> newest row
 */
export function latestResultsByDate(rows) {
  const m = new Map();
  for (const r of rows) {
    if (!r.date || !Number.isFinite(r.t)) continue;
    const prev = m.get(r.date);
    if (!prev || r.t >= prev.t) m.set(r.date, r);
  }
  return m;
}

/** The eight compared quantities, in the order they are checked and reported. */
const AGREEMENT = [
  ['eTrk', 'tracked Wh', fmtSig],
  ['eFix', 'fixed Wh', fmtSig],
  ['eUse', 'own use Wh', fmtSig],
  ['gGross', 'gross gain %', fmtFixed],
  ['gNet', 'net gain %', fmtFixed],
  ['mae', 'pointing error °', fmtFixed],
  ['complete', 'complete %', fmtFixed],
];

/**
 * Channel-2 agreement rule (contract section 6.5, v1.5): energies equal at 3
 * significant figures, gains, pointing error and completeness at 1 dp, N exactly.
 * @param {object} local   this page's summary of the day (from channel 1)
 * @param {object|null} matlab  newest channel-2 row for the same date, or null/undefined
 * @returns {{status:'agree'|'differ'|'missing', reason:string}} reason names the first differing quantity
 */
export function matlabAgreement(local, matlab) {
  if (!matlab) return { status: 'missing', reason: '' };
  for (const [key, name, fmt] of AGREEMENT) {
    const a = fmt(local[key]);
    const b = fmt(matlab[key]);
    if (a !== b) return { status: 'differ', reason: `${name}: dashboard ${a}, MATLAB ${b}` };
  }
  // v1.5: channel-2 field8 is N, the de-duplicated sample count.
  if (matlab.n === null || local.nUnique !== matlab.n) {
    return { status: 'differ', reason: `samples: dashboard ${local.nUnique}, MATLAB ${matlab.n === null ? 'n/a' : matlab.n}` };
  }
  return { status: 'agree', reason: '' };
}

/**
 * Per-run summaries for run IDs > 0. Grouped by (run, panel) like the Python
 * reference, so a run that (wrongly) spans a panel swap shows as two rows.
 * `kind` is 'test' or 'calibration' (run IDs 900-999).
 */
export function runSummaries(entries) {
  const m = new Map();
  for (const e of entries) {
    if (!(e.run > 0)) continue;
    const k = `${e.run}|${e.panel ?? ''}`;
    let g = m.get(k);
    if (!g) m.set(k, (g = { run: e.run, panel: e.panel, entries: [] }));
    g.entries.push(e);
  }
  return [...m.values()]
    .sort((a, b) => a.run - b.run || String(a.panel ?? '').localeCompare(String(b.panel ?? '')))
    .map((g) => ({ run: g.run, panel: g.panel, kind: runKind(g.run), ...summarise(g.entries) }));
}

/**
 * Totals over the tracking-test entries, per panel letter (panel-swap
 * comparison). Calibration runs (900-999) are left out: both panels are fixed
 * there, so they say nothing about tracking.
 */
export function panelSummaries(entries) {
  const m = new Map();
  for (const e of uniqueEntries(entries)) {
    if (runKind(e.run) !== 'test') continue;
    const k = e.panel ?? '?';
    let g = m.get(k);
    if (!g) m.set(k, (g = { panel: k, runs: new Set(), eTrk: 0, eFix: 0, eUse: 0, n: 0 }));
    g.runs.add(e.run);
    g.n++;
    g.eTrk += (e.pTrk ?? 0) * WH_PER_MW_SAMPLE;
    g.eFix += (e.pFix ?? 0) * WH_PER_MW_SAMPLE;
    g.eUse += (e.pUse ?? 0) * WH_PER_MW_SAMPLE;
  }
  return [...m.values()].sort((a, b) => a.panel.localeCompare(b.panel)).map((g) => ({
    panel: g.panel, runs: g.runs.size, n: g.n, eTrk: g.eTrk, eFix: g.eFix, eUse: g.eUse,
    gGross: g.eFix > 0 ? 100 * (g.eTrk - g.eFix) / g.eFix : null,
    gNet: g.eFix > 0 ? 100 * (g.eTrk - g.eFix - g.eUse) / g.eFix : null,
  }));
}

/**
 * Calibration runs (IDs 900-999: both panels fixed side by side). The panel
 * mismatch is 100 * (E_trk - E_fix) / E_fix with the energies summed over all
 * calibration entries; it is a difference between the panels, not a gain.
 * `byPanel` gives the same figure per panel letter on the tracker mount.
 * @returns {null | {runs:number, n:number, nSim:number, eTrk:number, eFix:number, mismatch:number|null, byPanel:Array}}
 */
export function calibrationSummary(entries) {
  const total = { runs: new Set(), n: 0, nSim: 0, eTrk: 0, eFix: 0 };
  const m = new Map();
  for (const e of uniqueEntries(entries)) {
    if (runKind(e.run) !== 'calibration') continue;
    const k = e.panel ?? '?';
    let g = m.get(k);
    if (!g) m.set(k, (g = { panel: k, runs: new Set(), n: 0, nSim: 0, eTrk: 0, eFix: 0 }));
    for (const x of [total, g]) {
      x.runs.add(e.run);
      x.n++;
      if (e.sim) x.nSim++;
      x.eTrk += (e.pTrk ?? 0) * WH_PER_MW_SAMPLE;
      x.eFix += (e.pFix ?? 0) * WH_PER_MW_SAMPLE;
    }
  }
  if (!total.n) return null;
  const done = (x) => ({ runs: x.runs.size, n: x.n, nSim: x.nSim, eTrk: x.eTrk, eFix: x.eFix,
    mismatch: x.eFix > 0 ? 100 * (x.eTrk - x.eFix) / x.eFix : null });
  return { ...done(total), byPanel: [...m.values()].sort((a, b) => a.panel.localeCompare(b.panel)).map((g) => ({ panel: g.panel, ...done(g) })) };
}

/**
 * Running totals through a period, one point per entry (time order):
 * cumulative Wh for tracked, fixed, tracked minus own use, and the running
 * net gain % (null while the fixed-panel energy so far is zero).
 */
export function cumulativeSeries(entries) {
  const es = uniqueEntries(entries);
  let trk = 0, fix = 0, use = 0;
  return es.map((e) => {
    trk += (e.pTrk ?? 0) * WH_PER_MW_SAMPLE;
    fix += (e.pFix ?? 0) * WH_PER_MW_SAMPLE;
    use += (e.pUse ?? 0) * WH_PER_MW_SAMPLE;
    return { t: e.t, eTrk: trk, eFix: fix, eTrkNet: trk - use, gNet: fix > 0 ? 100 * (trk - fix - use) / fix : null };
  });
}

/** Stale = newest sample older than 2 upload intervals + 5 minutes. */
export function staleLimitMs(uploadMin) {
  return (2 * uploadMin + 5) * MIN_MS;
}

export function isStale(newestMs, nowMs, uploadMin) {
  return newestMs === null || newestMs === undefined || nowMs - newestMs > staleLimitMs(uploadMin);
}

/* ------------------------------------------------------- display-only tools */

/**
 * Min/max decimation of {x,y} points for DISPLAY ONLY (never used for metrics).
 * Keeps the first/last point and the min and max of every bucket so peaks
 * survive; null y values (line breaks) are preserved.
 */
export function decimateMinMax(points, maxPoints = 1500) {
  if (points.length <= maxPoints) return points;
  const buckets = Math.max(1, Math.floor(maxPoints / 2));
  const size = points.length / buckets;
  const out = [];
  for (let b = 0; b < buckets; b++) {
    const lo = Math.floor(b * size);
    const hi = Math.min(points.length, Math.floor((b + 1) * size));
    let min = null, max = null, brk = null;
    for (let i = lo; i < hi; i++) {
      const p = points[i];
      if (p.y === null) { brk = p; continue; }
      if (min === null || p.y < min.y) min = p;
      if (max === null || p.y > max.y) max = p;
    }
    const keep = [min, max, brk].filter(Boolean);
    if (min === max && min) keep.splice(1, 1);
    keep.sort((a, c) => a.x - c.x);
    out.push(...keep);
  }
  if (out[0] !== points[0]) out.unshift(points[0]);
  if (out[out.length - 1] !== points[points.length - 1]) out.push(points[points.length - 1]);
  return out;
}

/** {x,y} line data with a null inserted wherever more than `maxStepMs` is missing, so lines break at gaps. */
export function lineWithBreaks(entries, pick, maxStepMs = 1.5 * MIN_MS) {
  const out = [];
  let prev = null;
  for (const e of entries) {
    const y = pick(e);
    if (prev !== null && e.t - prev > maxStepMs) out.push({ x: prev + MIN_MS, y: null });
    out.push({ x: e.t, y });
    prev = e.t;
  }
  return out;
}

/** Hourly rows for the accessible data tables: mean of each picked value per UTC hour. */
export function hourlyMeans(entries, picks) {
  const m = new Map();
  for (const e of entries) {
    const h = Math.floor(e.t / 3_600_000) * 3_600_000;
    let r = m.get(h);
    if (!r) m.set(h, (r = { t: h, n: 0, sum: picks.map(() => 0), cnt: picks.map(() => 0) }));
    r.n++;
    picks.forEach((p, i) => {
      const v = p(e);
      if (v !== null && v !== undefined) { r.sum[i] += v; r.cnt[i]++; }
    });
  }
  return [...m.values()].sort((a, b) => a.t - b.t)
    .map((r) => ({ t: r.t, n: r.n, values: r.sum.map((s, i) => (r.cnt[i] ? s / r.cnt[i] : null)) }));
}

/* ------------------------------------------------- state timeline (status) */

/** Rows of the state timeline, top to bottom: the six STATEs, then gaps with no data. */
export const TIMELINE_ROWS = ['TRK', 'MOV', 'HLD', 'IDL', 'STW', 'FLT', 'GAP'];

/**
 * Contiguous state segments for the status timeline (project lead's "Status"
 * graph). Uses the de-duplicated entries; a segment covers [first sample,
 * last sample + 1 min). Missing minutes between samples become 'GAP'
 * segments, so outages are visible on the same chart.
 * @returns [{ state, start, end, minutes }]  in time order
 */
export function stateSegments(entries) {
  const es = uniqueEntries(entries);
  const out = [];
  let cur = null;
  let prevT = null;
  for (const e of es) {
    const st = TRACKING_STATES.has(e.state) || STATE_WORDS[e.state] ? e.state : 'FLT';
    if (prevT !== null && e.t - prevT > 1.5 * MIN_MS) {
      if (cur) { out.push(cur); cur = null; }
      out.push({ state: 'GAP', start: prevT + MIN_MS, end: e.t, minutes: Math.round((e.t - prevT) / MIN_MS) - 1 });
    }
    if (cur && cur.state === st) {
      cur.end = e.t + MIN_MS;
      cur.minutes++;
    } else {
      if (cur) out.push(cur);
      cur = { state: st, start: e.t, end: e.t + MIN_MS, minutes: 1 };
    }
    prevT = e.t;
  }
  if (cur) out.push(cur);
  return out;
}

/** Minutes spent in each timeline row, in TIMELINE_ROWS order: [{ state, minutes }]. */
export function stateTotals(segments) {
  const m = new Map(TIMELINE_ROWS.map((r) => [r, 0]));
  for (const s of segments) m.set(s.state, (m.get(s.state) || 0) + s.minutes);
  return TIMELINE_ROWS.map((state) => ({ state, minutes: m.get(state) }));
}
