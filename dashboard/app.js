/**
 * AST phone dashboard: loads samples from ThingSpeak (or demo data), computes
 * the contract metrics with metrics.js and renders status, KPIs, charts,
 * tables and CSV exports. No framework, no build step.
 */
import { DEFAULTS, forgetSavedConfig, loadSavedConfig, resolveConfig, safeStorage, saveConfig } from './config.js';
import { ApiError, fetchLatest, fetchRange } from './api.js';
import {
  DAY_MS, FAULT_WORDS, MIN_MS, STATE_WORDS, addDays, calibrationSummary, cumulativeSeries, dailyGainSeries, dailySummaries,
  dayStartMs, expectedForDay, histLabels, hourlyMeans, isStale, latestResultsByDate, lineWithBreaks, matlabAgreement,
  panelSummaries, parseFeed, parseResultFeed, pointingError, runSummaries, staleLimitMs, summarise, utcDay,
  TIMELINE_ROWS, TIMELINE_WORDS, stateSegments, stateTotals,
} from './metrics.js';
import { NA, fmtDateTime, fmtDay, fmtDuration, fmtFixed, fmtSig, fmtSigned, fmtTime, gainWords } from './format.js';
import { copyCsv, downloadCsv, toCsv } from './csv.js';
import { energyCsv, histCsv, rawCsv, runsCsv } from './exports.js';
import { renderBarLegend, renderDailyGain, renderHistogram, renderLegend, renderLineChart, renderStateTimeline } from './charts.js';
import { DEMO_DAYS, demoRawFeeds, demoResultFeeds } from './demo.js';

const $ = (sel, root = document) => root.querySelector(sel);

// Channel settings from a link are remembered on this device (review m9), so
// the "Add to Home Screen" icon, which opens the page without its query
// string, still finds the channel. Nothing is stored in demo mode, and the
// page works the same when the browser refuses storage.
const storage = safeStorage();
const cfg = resolveConfig(location.search, DEFAULTS, loadSavedConfig(storage));
if (cfg.shouldSave) saveConfig(storage, cfg);
let hasSavedConfig = loadSavedConfig(storage) !== null;
const clockOffset = cfg.nowOverrideMs === null ? 0 : cfg.nowOverrideMs - Date.now();
const now = () => Date.now() + clockOffset;

const state = {
  day: cfg.day || utcDay(now()),
  last: null,          // newest sample on the channel (parsed), for the status bar
  empty: false,        // channel 1 has no entries at all
  results: new Map(),  // channel 2: date -> newest MATLAB summary row for that date
  ch2: cfg.demo || cfg.ch2 ? 'empty' : 'none', // ok | empty | error | none (data-ch2 on #daily)
  errors: { ch1: null, ch2: null },
  phase: 'loading',    // loading | day | full | error
  gen: 0,
  abort: null,
  loadedAt: null,
  renders: 0,
  liveKey: '',
  chartKey: '',
  gainKey: '',
  gainRows: [],        // rows of the daily net gain chart (also feeds its data table)
};

/** Sample store: every entry loaded so far, indexed by UTC day. */
const store = { byKey: new Map(), byDay: new Map(), dayInfo: new Map() };

/* ------------------------------------------------------------ data sources */

function makeSource() {
  if (cfg.demo) {
    const raw = () => demoRawFeeds(now());
    return {
      latest: async () => raw().slice(-1),
      range: async (startMs, endMs) => raw().filter((f) => { const t = Date.parse(f.created_at); return t >= startMs && t <= endMs; }),
      results: async () => demoResultFeeds(raw(), now()),
    };
  }
  return {
    latest: (signal) => fetchLatest(cfg, cfg.ch1, cfg.rk1, 1, { signal }),
    range: (startMs, endMs, signal) => fetchRange(cfg, cfg.ch1, cfg.rk1, startMs, endMs, { signal }),
    results: (signal) => (cfg.ch2 ? fetchLatest(cfg, cfg.ch2, cfg.rk2, resultsWanted(), { signal }) : Promise.resolve(null)),
  };
}

/**
 * Channel-2 entries to read. The MATLAB job writes 9 entries per night
 * (contract section 3.2), so 9 * historyDays + 18 covers the loaded range when it
 * ends today; when an older day is selected the nights since then are added.
 */
function resultsWanted() {
  const nightsSince = Math.max(0, Math.round((dayStartMs(utcDay(now())) - dayStartMs(state.day)) / DAY_MS));
  return Math.min(8000, 9 * (cfg.historyDays + nightsSince) + 18);
}
const source = makeSource();

function mergeFeeds(feeds) {
  const touched = new Set();
  for (const f of feeds) {
    const e = parseFeed(f);
    if (!Number.isFinite(e.t)) continue;
    store.byKey.set(e.entryId ?? `t${e.t}`, e);
    touched.add(utcDay(e.t));
  }
  if (!touched.size) return;
  for (const d of touched) store.byDay.set(d, []);
  for (const e of store.byKey.values()) {
    const d = utcDay(e.t);
    if (touched.has(d)) store.byDay.get(d).push(e);
  }
  for (const d of touched) store.byDay.get(d).sort((a, b) => a.t - b.t || (a.entryId ?? 0) - (b.entryId ?? 0));
}

function rangeDays() {
  const n = cfg.demo ? Math.max(cfg.historyDays, DEMO_DAYS) : cfg.historyDays;
  return Array.from({ length: n }, (_, i) => addDays(state.day, i - (n - 1)));
}

function rangeEntries() {
  return rangeDays().flatMap((d) => store.byDay.get(d) || []);
}

/**
 * Make sure the given (ascending) days are loaded. A day is "final" once the
 * channel holds a newer day's sample (the device uploads oldest first), and is
 * then never fetched again. Open days are topped up from two upload intervals
 * (at least 30 min) before their newest sample, so copies of a batch re-sent
 * after a lost response are seen too (independent review, finding 9).
 * Entries are keyed by entry_id, so the overlap is never counted twice.
 */
async function loadDays(days, lastT, signal) {
  const need = days.filter((d) => !store.dayInfo.get(d)?.final);
  const groups = [];
  for (const d of need) {
    const g = groups[groups.length - 1];
    if (g && addDays(g[g.length - 1], 1) === d) g.push(d); else groups.push([d]);
  }
  for (const g of groups) {
    const newest = store.dayInfo.get(g[0])?.newest ?? null;
    const overlapMs = Math.max(2 * cfg.uploadMin, 30) * MIN_MS;
    const start = newest === null ? dayStartMs(g[0]) : Math.max(dayStartMs(g[0]), newest - overlapMs);
    const end = Math.min(dayStartMs(g[g.length - 1]) + DAY_MS - 1000, lastT ?? -Infinity);
    if (start <= end) mergeFeeds(await source.range(start, end, signal));
    for (const d of g) {
      const es = store.byDay.get(d);
      store.dayInfo.set(d, {
        final: lastT !== null && lastT >= dayStartMs(d) + DAY_MS,
        newest: es?.length ? es[es.length - 1].t : null,
      });
    }
  }
}

async function loadResults(signal) {
  try {
    const feeds = await source.results(signal);
    state.errors.ch2 = null;
    state.results = latestResultsByDate((feeds || []).map(parseResultFeed));
    state.ch2 = feeds === null ? 'none' : state.results.size ? 'ok' : 'empty';
  } catch (err) {
    if (err?.name === 'AbortError') throw err;
    state.errors.ch2 = err;
    state.results = new Map();
    state.ch2 = 'error';
  }
}

async function refresh() {
  if (!cfg.configured) {
    state.phase = 'error';
    state.errors.ch1 = new ApiError('config', 'Not configured');
    render();
    return;
  }
  const gen = ++state.gen;
  state.abort?.abort();
  state.abort = new AbortController();
  const { signal } = state.abort;
  document.body.setAttribute('aria-busy', 'true');
  try {
    const latest = await source.latest(signal);
    if (gen !== state.gen) return;
    state.errors.ch1 = null;
    state.empty = latest.length === 0;
    state.last = latest.length ? parseFeed(latest[latest.length - 1]) : null;
    const lastT = state.last?.t ?? null;

    // First paint: the selected day only. Then the history for the tables.
    await loadDays([state.day], lastT, signal);
    if (gen !== state.gen) return;
    if (state.phase !== 'full') state.phase = 'day';
    render();

    await Promise.all([loadDays(rangeDays(), lastT, signal), loadResults(signal)]);
    if (gen !== state.gen) return;
    state.phase = 'full';
    state.loadedAt = now();
    render();
  } catch (err) {
    if (err?.name === 'AbortError' || gen !== state.gen) return;
    state.errors.ch1 = err instanceof ApiError ? err : new ApiError('format', 'Unexpected error', String(err?.message || err));
    if (!(err instanceof ApiError)) console.error(err);
    state.phase = 'error';
    render();
  } finally {
    if (gen === state.gen) document.body.removeAttribute('aria-busy');
  }
}

/* ---------------------------------------------------------------- helpers */

function setText(el, text) {
  if (el.textContent !== text) el.textContent = text;
}

/**
 * Fill a table body. A row is an array of cells or { cells, data } where `data`
 * becomes data-* attributes on the <tr>. A cell is text or { text, col, cls,
 * colSpan }; `col` becomes data-col (hooks for the end-to-end tests).
 */
function fillTable(tbody, rows, emptyText) {
  const cols = tbody.parentElement.tHead.rows[0].cells.length;
  tbody.replaceChildren(...(rows.length ? rows : [[{ text: emptyText, colSpan: cols, cls: 'empty' }]]).map((row) => {
    const tr = document.createElement('tr');
    const cells = Array.isArray(row) ? row : row.cells;
    if (!Array.isArray(row)) for (const [k, v] of Object.entries(row.data || {})) tr.dataset[k] = String(v);
    cells.forEach((c, i) => {
      const spec = typeof c === 'object' && c !== null ? c : { text: c };
      const td = document.createElement(i === 0 && !spec.colSpan ? 'th' : 'td');
      if (td.tagName === 'TH') td.scope = 'row';
      if (spec.colSpan) td.colSpan = spec.colSpan;
      if (spec.cls) td.className = spec.cls;
      if (spec.col) td.dataset.col = spec.col;
      td.textContent = String(spec.text);
      tr.appendChild(td);
    });
    return tr;
  }));
}

function simpleTable(container, caption, header, rows, emptyText = 'No data for this day.') {
  const table = document.createElement('table');
  const cap = table.createCaption();
  cap.textContent = caption;
  const tr = table.createTHead().insertRow();
  for (const h of header) {
    const th = document.createElement('th');
    th.scope = 'col';
    th.textContent = h;
    tr.appendChild(th);
  }
  table.createTBody();
  container.replaceChildren(table);
  fillTable(table.tBodies[0], rows, emptyText);
}

const SIM_MARK = '⚠ simulated';
const plural = (n, one, many) => `${n} ${n === 1 ? one : many}`;

const pctCell = (x) => {
  if (x === null) return NA;
  const g = gainWords(x);
  return `${fmtSigned(x)} ${g.symbol} ${g.word}`;
};

function describeError(err, channelLabel, channelId) {
  switch (err.kind) {
    case 'config':
      return 'No channel is configured yet. Add the channel ID and read key to config.js (or to the link), or open the built-in demo.';
    case 'key':
      return `The data host rejected the read key for ${channelLabel} (channel ${channelId}): it answered −1. Check the read key in config.js or in the link you opened.`;
    case 'network':
      return `Cannot reach the data host at ${cfg.base}. Check the internet connection on this device; the page will try again automatically.`;
    case 'notfound':
      return `${channelLabel} (channel ${channelId}) was not found on the data host. Check the channel ID.`;
    case 'http':
      return `The data host returned an error for ${channelLabel} (${err.message}). It will be tried again automatically.`;
    default:
      return `The data host sent a reply this page did not understand for ${channelLabel} (${err.detail || err.message}).`;
  }
}

/* ----------------------------------------------------------------- render */

function renderStatus() {
  const t = now();
  const last = state.last;
  const box = $('#freshness');
  const icon = $('.fresh-icon', box);
  if (!last) {
    box.dataset.fresh = state.errors.ch1 ? 'unknown' : state.empty ? 'empty' : 'unknown';
    icon.textContent = state.empty || state.errors.ch1 ? '⚠' : '…';
    setText($('#freshness-text'), state.errors.ch1 ? 'Status unknown: no data could be loaded.'
      : state.empty ? 'No data: the tracker has not uploaded any samples yet.' : 'Loading the latest sample…');
    for (const id of ['st-age', 'st-state', 'st-fault', 'st-run', 'st-panel', 'st-data', 'st-power', 'st-angle']) setText($(`#${id}`), '–');
    delete $('#st-data').dataset.sim;
    return;
  }
  const age = t - last.t;
  const stale = isStale(last.t, t, cfg.uploadMin);
  box.dataset.fresh = stale ? 'stale' : 'fresh';
  icon.textContent = stale ? '⚠' : '✔';
  setText($('#freshness-text'), stale
    ? `Data is stale: the last sample is ${fmtDuration(age)} old (expected within ${fmtDuration(staleLimitMs(cfg.uploadMin))}). Check the tracker's power and Wi-Fi.`
    : `Data is up to date: last sample ${fmtDuration(age)} ago.`);
  setText($('#st-age'), `${fmtDuration(age)} ago (${fmtDateTime(last.t)})`);
  setText($('#st-state'), last.state ? `${STATE_WORDS[last.state] || 'Unknown'} (${last.state})` : 'Not reported');
  const faultEl = $('#st-fault');
  faultEl.dataset.fault = last.fault ? 'yes' : 'no';
  setText(faultEl, last.fault
    ? `⚠ Fault ${last.fault}: ${FAULT_WORDS[last.fault] || 'unknown fault code'}`
    : '✔ None');
  setText($('#st-run'), last.run > 0 ? `Run ${last.run}${last.run >= 900 && last.run <= 999 ? ' (calibration)' : ''}` : 'Not in a run');
  const dataEl = $('#st-data');
  dataEl.dataset.sim = last.sim ? 'yes' : 'no';
  setText(dataEl, last.sim ? '⚠ Simulated (sim mode)' : 'Measured');
  setText($('#st-panel'), last.panel ? `Panel ${last.panel}` : 'Not reported');
  const mw = (x) => (x === null ? NA : `${fmtFixed(x, 1)} mW`);
  setText($('#st-power'), `Tracked ${mw(last.pTrk)} · Fixed ${mw(last.pFix)} · Own use ${mw(last.pUse)}`);
  const deg = (x) => (x === null ? NA : `${fmtFixed(x, 1)}°`);
  setText($('#st-angle'), `Panel ${deg(last.angPanel)} · Sun ${deg(last.angSun)}`);
}

function renderMessages() {
  const msgs = [];
  if (cfg.baseRejected) msgs.push(['warn', 'The link asked for another data host; it was ignored and the group\'s ThingSpeak channels are shown instead.', 'host']);
  if (state.errors.ch1) msgs.push(['error', describeError(state.errors.ch1, 'the raw data channel', cfg.ch1)]);
  else if (state.empty) msgs.push(['warn', `The raw data channel (channel ${cfg.ch1}) is empty: the tracker has not uploaded any samples yet.`]);
  const box = $('#messages');
  const key = JSON.stringify(msgs);
  if (box.dataset.key === key) return;
  box.dataset.key = key;
  box.replaceChildren(...msgs.map(([level, text, kind]) => {
    const p = document.createElement('p');
    p.className = `msg msg-${level}`;
    p.dataset.kind = kind || state.errors.ch1?.kind || 'empty';
    const icon = document.createElement('span');
    icon.setAttribute('aria-hidden', 'true');
    icon.textContent = level === 'error' ? '✖ ' : '⚠ ';
    const strong = document.createElement('strong');
    strong.textContent = level === 'error' ? 'Problem: ' : 'Note: ';
    p.append(icon, strong, text);
    if (!kind && state.errors.ch1?.kind === 'config') {
      const a = document.createElement('a');
      a.href = '?demo=1';
      a.textContent = 'Open the demo';
      p.append(' ', a, '.');
    }
    return p;
  }));
}

function summarySentence(s, dayLabel, isToday) {
  const who = isToday ? 'Today' : `On ${dayLabel}`;
  if (!s.n) return `${isToday ? 'Today no' : `On ${dayLabel} no`} samples ${isToday ? 'have been' : 'were'} received`;
  let gain;
  if (s.gNet === null) gain = 'net gain cannot be worked out because the fixed panel produced no energy';
  else {
    const g = gainWords(s.gNet);
    gain = g.cls === 'even' ? 'no net gain or loss (0.0%)' : `a net ${g.word} of ${fmtFixed(Math.abs(s.gNet))}%`;
  }
  const sim = s.nSim ? ` ⚠ ${s.nSim === s.n ? `All ${s.n} samples are` : `${s.nSim} of ${s.n} samples are`} simulated, not measured.` : '';
  return `${who} the tracker produced ${fmtSig(s.eTrk)} Wh against ${fmtSig(s.eFix)} Wh for the fixed panel, ${gain} after using ` +
    `${fmtSig(s.eUse)} Wh itself.${sim} Data is ${fmtFixed(s.complete)}% complete${isToday ? ' up to the newest sample' : ''}`;
}

function lastUpdatePhrase() {
  if (!state.last) return '';
  const stale = isStale(state.last.t, now(), cfg.uploadMin);
  return `; last update ${fmtDuration(now() - state.last.t)} ago${stale ? ' (data is stale)' : ''}.`;
}

function setKpi(key, value, note) {
  const el = $(`[data-kpi="${key}"]`);
  setText($('.kpi-value', el), value);
  if (note !== undefined) setText($('.kpi-note', el), note);
}

function daySummary() {
  const entries = store.byDay.get(state.day) || [];
  const newest = entries.length ? entries[entries.length - 1].t : null;
  return { entries, s: summarise(entries, expectedForDay(state.day, now(), newest)) };
}

function renderDay() {
  const today = utcDay(now());
  const isToday = state.day === today;
  const input = $('#day-input');
  input.max = today;
  if (input.value !== state.day) input.value = state.day;
  $('#day-next').disabled = state.day >= today;
  $('#day-today').disabled = isToday;
  const dayLabel = fmtDay(state.day);
  setText($('#kpi-day'), isToday ? `today (${dayLabel})` : dayLabel);

  const loaded = state.phase === 'day' || state.phase === 'full' || store.dayInfo.has(state.day);
  if (!loaded) {
    setText($('#summary'), state.errors.ch1 ? 'No data could be loaded.' : 'Loading…');
    return;
  }
  const { entries, s } = daySummary();

  // Plain-language summary. The live region only changes when the data does,
  // so screen readers are not interrupted every minute by the ticking age.
  const sentence = summarySentence(s, dayLabel, isToday);
  const tail = (s.n || isToday ? lastUpdatePhrase() : '') || '.';
  setText($('#summary'), sentence + tail);
  const liveKey = `${state.day}|${s.n}|${entries.length ? entries[entries.length - 1].t : ''}`;
  if (liveKey !== state.liveKey) {
    state.liveKey = liveKey;
    $('#summary-live').textContent = sentence + tail;
  }

  // KPI cards
  setKpi('eTrk', s.n ? fmtSig(s.eTrk) : NA);
  setKpi('eFix', s.n ? fmtSig(s.eFix) : NA);
  setKpi('eUse', s.n ? fmtSig(s.eUse) : NA);
  for (const [key, v] of [['gGross', s.gGross], ['gNet', s.gNet]]) {
    const g = gainWords(v);
    const el = $(`[data-kpi="${key}"]`);
    el.dataset.sign = g.cls;
    const what = key === 'gNet' ? 'after own use' : 'before own use';
    setKpi(key, fmtSigned(v), v === null
      ? (s.n ? 'Not available: the fixed panel produced no energy' : 'No samples')
      : `${g.symbol} ${g.word === 'no change' ? 'No gain or loss' : `Tracker ${g.word}`} ${what}`);
  }
  const gapsText = `${plural(s.gaps, 'gap', 'gaps')}${s.gaps ? `, longest ${s.longestGapMin} min` : ''}`;
  let completeNote;
  if (!s.expected) completeNote = isToday ? `No samples received yet today; uploads arrive every ${cfg.uploadMin} min` : 'Day has not started';
  else if (isToday) {
    // Contract v1.5 (rule from v1.4): today is expected only up to its newest sample; upload lag is the stale indicator's job.
    completeNote = `${s.nUnique} of ${s.expected} expected up to the newest sample at ${fmtTime(s.last)}; ` +
      `uploads arrive every ${cfg.uploadMin} min · ${gapsText}`;
  } else completeNote = `${s.nUnique} of ${s.expected} expected samples · ${gapsText}`;
  setKpi('complete', s.expected ? fmtFixed(s.complete) : NA, completeNote);
  setKpi('moves', s.n ? String(s.moves) : NA);
  setKpi('mae', fmtFixed(s.mae), s.nPointing ? `mean of |sun − panel| over ${s.nPointing} samples` : 'No tracking samples with both angles');

  // Data-quality notes (text + icon, not colour)
  const notes = [];
  if (s.nSim) {
    notes.push(s.nSim === 1 ? '1 sample is SIMULATED (firmware sim mode), not a measurement.'
      : `${s.nSim} samples are SIMULATED (firmware sim mode), not measurements.`);
  }
  if (s.negativePower) {
    notes.push(`${plural(s.negativePower, 'sample has', 'samples have')} a negative power value. This usually means a current-sense shunt ` +
      'is wired the wrong way round; the values are summed as received, so the energies and gains may be wrong.');
  }
  if (s.missingPowerFields) notes.push(`${s.missingPowerFields} ${s.missingPowerFields === 1 ? 'sample has' : 'samples have'} a missing power value (counted as 0 mW).`);
  if (s.duplicates) notes.push(`${s.duplicates} duplicate ${s.duplicates === 1 ? 'sample' : 'samples'} (repeated sequence number) received; duplicates do not raise completeness.`);
  if (s.gaps) notes.push(`${s.missingMin} ${s.missingMin === 1 ? 'minute is' : 'minutes are'} missing in ${s.gaps} ${s.gaps === 1 ? 'gap' : 'gaps'}; the longest gap is ${s.longestGapMin} min.`);
  const faults = entries.filter((e) => e.fault > 0);
  if (faults.length) {
    const codes = [...new Set(faults.map((e) => e.fault))].sort((a, b) => a - b)
      .map((c) => `${c} (${FAULT_WORDS[c] || 'unknown'})`).join(', ');
    notes.push(`${faults.length} ${faults.length === 1 ? 'sample reports' : 'samples report'} a fault: code ${codes}.`);
  }
  $('#quality').replaceChildren(...notes.map((text) => {
    const li = document.createElement('li');
    li.textContent = `⚠ ${text}`;
    return li;
  }));

  renderCharts(entries, s);
}

/**
 * Y range for the running net gain chart (display only). Just after sunrise
 * the fixed-panel total is tiny, so the percentage swings wildly; the axis is
 * fitted to the part of the day after the first 2 % of fixed-panel energy.
 */
function gainAxis(cum) {
  if (!cum.length) return {};
  const settled = cum.filter((p) => p.gNet !== null && p.eFix >= 0.02 * cum[cum.length - 1].eFix).map((p) => p.gNet);
  if (!settled.length) return {};
  const lo = Math.min(0, ...settled);
  const hi = Math.max(0, ...settled);
  const pad = Math.max(1, (hi - lo) * 0.1);
  return { yMin: Math.floor(lo - pad), yMax: Math.ceil(hi + pad) };
}

function renderCharts(entries, s) {
  const scheme = matchMedia('(prefers-color-scheme: dark)').matches ? 'd' : 'l';
  const key = `${state.day}|${entries.length}|${entries.length ? entries[entries.length - 1].t : ''}|${scheme}`;
  if (key === state.chartKey) return;
  state.chartKey = key;
  const fallbackMin = dayStartMs(state.day);
  const cum = cumulativeSeries(entries);
  const dayLabel = fmtDay(state.day);

  const figs = [
    {
      id: 'fig-power', opts: { yTitle: 'Power (mW)', unit: 'mW', beginAtZero: true, fallbackMin },
      series: [
        { label: 'Tracked', style: 'trk', data: lineWithBreaks(entries, (e) => e.pTrk) },
        { label: 'Fixed', style: 'fix', data: lineWithBreaks(entries, (e) => e.pFix) },
        { label: 'Own use', style: 'use', data: lineWithBreaks(entries, (e) => e.pUse) },
      ],
      alt: `Line chart of power on ${dayLabel}.`,
    },
    {
      id: 'fig-energy', opts: { yTitle: 'Energy (Wh)', unit: 'Wh', dp: 3, beginAtZero: true, fallbackMin },
      series: [
        { label: 'Tracked', style: 'trk', data: cum.map((p) => ({ x: p.t, y: p.eTrk })) },
        { label: 'Fixed', style: 'fix', data: cum.map((p) => ({ x: p.t, y: p.eFix })) },
        { label: 'Tracked minus own use', style: 'net', data: cum.map((p) => ({ x: p.t, y: p.eTrkNet })) },
      ],
      alt: `Line chart of cumulative energy on ${dayLabel}: tracked ${fmtSig(s.eTrk)} Wh, fixed ${fmtSig(s.eFix)} Wh, tracked minus own use ${fmtSig(s.eTrk - s.eUse)} Wh at the end.`,
    },
    {
      id: 'fig-gain', opts: { yTitle: 'Net gain (%)', unit: '%', zeroLine: true, fallbackMin, ...gainAxis(cum) },
      series: [{ label: 'Running net gain', style: 'trk', data: cum.filter((p) => p.gNet !== null).map((p) => ({ x: p.t, y: p.gNet })) }],
      alt: `Line chart of running net gain on ${dayLabel}, ending at ${fmtSigned(s.gNet)} percent.`,
    },
    {
      id: 'fig-angle', opts: { yTitle: 'Angle (°)', unit: '°', zeroLine: true, fallbackMin },
      series: [
        { label: 'Panel', style: 'trk', data: lineWithBreaks(entries.filter((e) => e.angPanel !== null), (e) => e.angPanel, 5 * MIN_MS) },
        { label: 'Sun', style: 'fix', data: lineWithBreaks(entries.filter((e) => e.angSun !== null), (e) => e.angSun, 5 * MIN_MS) },
      ],
      alt: `Line chart of panel angle and sun angle on ${dayLabel}; mean pointing error ${fmtFixed(s.mae)} degrees.`,
    },
  ];
  for (const f of figs) {
    const fig = $(`#${f.id}`);
    const canvas = $('canvas', fig);
    canvas.setAttribute('aria-label', `${f.alt} The data table below gives the numbers.`);
    renderLegend($('.legend', fig), f.series);
    renderLineChart(canvas, f.series, f.opts);
  }
  // Status timeline (project lead's "Status" graph): one row per state plus gaps.
  const segs = stateSegments(entries);
  const totals = stateTotals(segs).filter((r) => r.minutes > 0);
  const tlCanvas = $('#fig-status canvas');
  tlCanvas.setAttribute('aria-label', `Status timeline for ${dayLabel}: ` + (totals.length
    ? totals.map((r) => `${TIMELINE_WORDS[r.state]} ${fmtDuration(r.minutes * MIN_MS)}`).join(', ')
    : 'no samples') + '. The data table below lists every period.');
  renderStateTimeline(tlCanvas, segs, TIMELINE_ROWS.map((k) => ({ key: k, label: TIMELINE_WORDS[k] })),
    { xMin: dayStartMs(state.day), xMax: dayStartMs(state.day) + DAY_MS });

  const histCanvas = $('#fig-hist canvas');
  histCanvas.setAttribute('aria-label', `Bar chart of pointing error on ${dayLabel}: ${s.nPointing} samples, mean absolute error ${fmtFixed(s.mae)} degrees. The data table below gives the numbers.`);
  renderHistogram(histCanvas, s.hist, histLabels());
  document.querySelectorAll('details.data-table[open]').forEach(renderDataTable);
}

/** Text alternative for a chart, built when its "Show data table" is opened. */
function renderDataTable(details) {
  const box = $('.table-scroll', details);
  const id = details.closest('figure').id;
  if (id === 'fig-daily-gain') {
    const days = rangeDays();
    simpleTable(box, `Net and gross gain for each UTC day from ${fmtDay(days[0])} to ${fmtDay(days[days.length - 1])}, worked out on this device ` +
      'from the raw samples. Days with no fixed-panel energy are left out.',
      ['Date (UTC day)', 'Net gain %', 'Gross gain %', 'Samples'],
      state.gainRows.map((r) => ({ data: { date: r.day }, cells: [`${fmtDay(r.day)}${r.nSim ? ` ${SIM_MARK}` : ''}`, pctCell(r.gNet), pctCell(r.gGross), String(r.n)] })),
      state.phase === 'full' ? 'No day in this range has fixed-panel energy yet.' : 'Loading…');
    return;
  }
  const { entries, s } = daySummary();
  const dayLabel = fmtDay(state.day);
  const hour = (t) => `${fmtTime(t)}–${fmtTime(t + 3_600_000)}`;
  const lastPerHour = () => {
    const m = new Map();
    for (const p of cumulativeSeries(entries)) m.set(Math.floor(p.t / 3_600_000), p);
    return [...m.values()];
  };
  if (id === 'fig-power') {
    simpleTable(box, `Mean power in each hour of ${dayLabel} (UK time). Use the raw-samples CSV for every minute.`,
      ['Hour', 'Tracked mW', 'Fixed mW', 'Own use mW', 'Samples'],
      hourlyMeans(entries, [(e) => e.pTrk, (e) => e.pFix, (e) => e.pUse])
        .map((r) => [hour(r.t), ...r.values.map((v) => fmtFixed(v, 1)), r.n]));
  } else if (id === 'fig-energy') {
    simpleTable(box, `Energy collected by the end of each hour of ${dayLabel} (UK time), in Wh.`,
      ['Up to', 'Tracked Wh', 'Fixed Wh', 'Tracked minus own use Wh'],
      lastPerHour().map((p) => [fmtTime(p.t), fmtSig(p.eTrk), fmtSig(p.eFix), fmtSig(p.eTrkNet)]));
  } else if (id === 'fig-gain') {
    simpleTable(box, `Running net gain at the end of each hour of ${dayLabel} (UK time).`,
      ['Up to', 'Net gain %'], lastPerHour().map((p) => [fmtTime(p.t), pctCell(p.gNet)]));
  } else if (id === 'fig-status') {
    const segs = stateSegments(entries);
    const totals = stateTotals(segs).filter((r) => r.minutes > 0)
      .map((r) => `${TIMELINE_WORDS[r.state]} ${fmtDuration(r.minutes * MIN_MS)}`).join(' · ');
    simpleTable(box, `Status periods on ${dayLabel} (UK time). Totals: ${totals || 'none'}.`,
      ['From', 'To', 'State', 'Minutes'],
      segs.map((g) => [fmtTime(g.start), fmtTime(g.end), TIMELINE_WORDS[g.state] + (g.state === 'GAP' ? '' : ` (${g.state})`), String(g.minutes)]));
  } else if (id === 'fig-angle') {
    simpleTable(box, `Mean angles in each hour of ${dayLabel} (UK time), in degrees.`,
      ['Hour', 'Panel angle °', 'Sun angle °', 'Mean pointing error °'],
      hourlyMeans(entries, [(e) => e.angPanel, (e) => e.angSun, (e) => { const x = pointingError(e); return x === null ? null : Math.abs(x); }])
        .map((r) => [hour(r.t), ...r.values.map((v) => fmtFixed(v, 1))]));
  } else {
    const labels = histLabels();
    simpleTable(box, `Pointing-error histogram for ${dayLabel}: samples per 1° band. Empty bands are left out.`,
      ['Error band (degrees)', 'Samples'],
      s.hist.map((c, i) => [labels[i].replace(/-/g, '−'), c]).filter((r) => r[1] > 0));
  }
}

/** Bar chart of net gain per UTC day over the loaded range (channel 1 values, like the daily table). */
function renderDailyGainFigure(computed, days, full) {
  const fig = $('#fig-daily-gain');
  const canvas = $('canvas', fig);
  const rows = full ? dailyGainSeries(computed) : [];
  const scheme = matchMedia('(prefers-color-scheme: dark)').matches ? 'd' : 'l';
  const key = `${full}|${days[0]}|${days.length}|${scheme}|${rows.map((r) => `${r.day}:${r.gNet.toFixed(3)}:${r.n}`).join(',')}`;
  if (key === state.gainKey) return;
  state.gainKey = key;
  state.gainRows = rows;
  fig.dataset.bars = String(rows.length);
  const short = (day) => fmtDay(day).replace(/^\w+, /, '').replace(/ \d{4}$/, ''); // "9 Nov"
  let alt;
  if (!full) alt = 'Bar chart of net gain per day: loading.';
  else if (!rows.length) alt = 'Bar chart of net gain per day: no day in the loaded range has fixed-panel energy yet.';
  else {
    const lo = rows.reduce((a, r) => (r.gNet < a.gNet ? r : a));
    const hi = rows.reduce((a, r) => (r.gNet > a.gNet ? r : a));
    const losses = rows.filter((r) => gainWords(r.gNet).cls === 'loss').length;
    const list = rows.length <= 7 ? ` By day: ${rows.map((r) => `${short(r.day)} ${fmtSigned(r.gNet)}`).join(', ')}.` : '';
    const sim = rows.some((r) => r.nSim) ? ' Includes simulated samples.' : '';
    alt = `Bar chart of net gain per UTC day, ${plural(rows.length, 'day', 'days')} from ${short(rows[0].day)} to ${short(rows[rows.length - 1].day)}: ` +
      `lowest ${fmtSigned(lo.gNet)} percent on ${short(lo.day)}, highest ${fmtSigned(hi.gNet)} percent on ${short(hi.day)}, ` +
      `${plural(losses, 'day', 'days')} with a net loss.${list}${sim}`;
  }
  canvas.setAttribute('aria-label', `${alt} The data table below gives the numbers.`);
  renderBarLegend($('.legend', fig));
  renderDailyGain(canvas, rows.map((r) => ({ label: short(r.day), value: r.gNet, text: fmtSigned(r.gNet) })));
  const details = $('details.data-table', fig);
  if (details.open) renderDataTable(details);
}

/** One row of the daily table: channel-1 values plus the MATLAB (channel 2) cross-check. */
function dailyRow(d, today) {
  let check;
  if (state.ch2 === 'none' || state.ch2 === 'error') check = { status: 'na', text: NA };
  else if (d.day === today) check = { status: 'missing', text: '– not yet summarised' }; // the job only summarises complete days
  else {
    const a = matlabAgreement(d, state.results.get(d.day));
    check = { status: a.status, text: a.status === 'agree' ? '✔ agrees' : a.status === 'differ' ? `⚠ differs (${a.reason})` : '– not yet summarised' };
  }
  return {
    data: { date: d.day, matlab: check.status, sim: d.nSim ? 'yes' : 'no' },
    cells: [
      { col: 'period', text: `${fmtDay(d.day)}${d.nSim ? ` ${SIM_MARK}` : ''}` },
      { col: 'eTrk', text: fmtSig(d.eTrk) }, { col: 'eFix', text: fmtSig(d.eFix) }, { col: 'eUse', text: fmtSig(d.eUse) },
      { col: 'gGross', text: pctCell(d.gGross) }, { col: 'gNet', text: pctCell(d.gNet) }, { col: 'mae', text: fmtFixed(d.mae) },
      { col: 'complete', text: fmtFixed(d.complete) }, { col: 'n', text: String(d.nUnique) }, { col: 'matlab', text: check.text },
    ],
  };
}

function dailySourceNote(rows) {
  const base = 'Values are worked out on this device from the raw samples (channel 1), newest first; today covers the day so far.';
  if (state.ch2 === 'none') return `${base} MATLAB check: not available, because no results channel (channel 2) is configured.`;
  if (state.ch2 === 'error') {
    return `${base} MATLAB check: not available, because the results channel could not be read (${describeError(state.errors.ch2, 'the results channel', cfg.ch2)})`;
  }
  const count = (st) => rows.filter((r) => r.data.matlab === st).length;
  const rule = 'The MATLAB check compares each complete day with the newest summary the MATLAB analysis wrote to the results channel (channel 2): ' +
    'energies to 3 significant figures, gains, pointing error and completeness to 1 decimal place, the sample count exactly.';
  const tally = state.ch2 === 'empty' ? 'The results channel is empty: the MATLAB analysis has not written a summary yet.'
    : `${plural(count('agree'), 'day agrees', 'days agree')}, ${plural(count('differ'), 'differs', 'differ')}, ${count('missing')} not yet summarised.` +
      (count('differ') ? ' A day usually differs because samples reached the host after the nightly MATLAB run; the next run corrects it. If it stays different, check the MATLAB script.' : '');
  return `${base} ${rule} ${tally}`;
}

function renderTables() {
  const entries = rangeEntries();
  const days = rangeDays();
  const full = state.phase === 'full';
  const waiting = state.errors.ch1 ? 'No data could be loaded.' : 'Loading…';
  const today = utcDay(now());

  // Daily summary: ALWAYS computed here from channel 1; channel 2 is a cross-check column (review B2).
  const computed = dailySummaries(entries, now());
  const rows = computed.slice(-14).reverse().map((d) => dailyRow(d, today));
  const dailyEl = $('#daily');
  dailyEl.dataset.ch2 = state.ch2;
  dailyEl.dataset.source = 'computed';
  setText($('#daily-source'), full ? dailySourceNote(rows) : waiting);
  fillTable($('#daily-table tbody'), full ? rows : [], full ? 'No daily results yet.' : waiting);
  renderDailyGainFigure(computed, days, full);

  // Runs: tracking tests (1-899) and calibration runs (900-999)
  const runs = runSummaries(entries);
  setText($('#runs-note'), `Runs found between ${fmtDay(days[0])} and ${fmtDay(days[days.length - 1])}. ` +
    'A run is every sample tagged with the same run number (set on the device before a test). Run numbers 900 to 999 are calibration runs: ' +
    `both panels fixed side by side. “${SIM_MARK}” marks a run that contains simulated samples.`);
  fillTable($('#runs-table tbody'), full ? runs.map((r) => ({
    data: { run: r.run, kind: r.kind, sim: r.nSim ? 'yes' : 'no' },
    cells: [`Run ${r.run}${r.nSim ? ` ${SIM_MARK}` : ''}`, r.kind === 'calibration' ? 'Calibration' : 'Tracking test',
      r.panel ? `Panel ${r.panel}` : 'Not reported', fmtDateTime(r.first), fmtDuration(r.expected * MIN_MS), fmtSig(r.eTrk), fmtSig(r.eFix),
      fmtSig(r.eUse), pctCell(r.gGross), pctCell(r.gNet), String(r.nUnique), fmtFixed(r.mae), fmtFixed(r.complete), String(r.moves)],
  })) : [], full ? 'No test runs in this period.' : waiting);

  const cal = full ? calibrationSummary(entries) : null;
  const calEl = $('#calib-note');
  calEl.hidden = !cal;
  if (cal) {
    const mm = (x) => (x.mismatch === null ? 'not available (the fixed-position panel gave no energy)' : `${fmtSigned(x.mismatch)}%`);
    const perPanel = cal.byPanel.length > 1
      ? ` By panel in the tracker position: ${cal.byPanel.map((p) => `panel ${p.panel} ${mm(p)} (${plural(p.runs, 'run', 'runs')})`).join(', ')}.` : '';
    calEl.dataset.runs = String(cal.runs);
    setText(calEl, `Calibration: ${plural(cal.runs, 'run', 'runs')} with both panels fixed side by side. Mean panel mismatch ${mm(cal)} ` +
      `(${fmtSig(cal.eTrk)} Wh from the panel in the tracker position against ${fmtSig(cal.eFix)} Wh from the other, ${plural(cal.n, 'sample', 'samples')}). ` +
      `This is the difference between the two panels, not a tracking gain.${perPanel}${cal.nSim ? ` ${SIM_MARK}: ${cal.nSim} of these samples.` : ''}`);
  }

  fillTable($('#panels-table tbody'), full ? panelSummaries(entries).map((p) => [`Panel ${p.panel}`, String(p.runs), String(p.n),
    fmtSig(p.eTrk), fmtSig(p.eFix), fmtSig(p.eUse), pctCell(p.gGross), pctCell(p.gNet)]) : [], full ? 'No tracking tests in this period.' : waiting);

  // Export info
  setText($('#export-range'), `Exports cover ${fmtDay(days[0])} to ${fmtDay(days[days.length - 1])} (${days.length} UTC days ending on the selected day). ` +
    'Files open in Excel, MATLAB or Python. Simulated samples are flagged in the sim and n_sim columns.' +
    (cfg.exportMode === 'clipboard' ? ' In this preview the buttons copy the CSV to the clipboard, because downloads are blocked here.' : ''));
  const rowsWord = (n) => plural(n, 'row', 'rows');
  const nCal = runs.filter((r) => r.kind === 'calibration').length;
  setText($('#csv-raw-info'), full ? `${rowsWord(entries.length)} · one per sample, with a sim column (1 = simulated)` : waiting);
  setText($('#csv-runs-info'), full ? `${rowsWord(runs.length)} · gain per run; the kind column says test or calibration (${nCal} calibration)` : waiting);
  setText($('#csv-hist-info'), full ? `62 rows · one column each for the selected day, the whole range and every run (${plural(runs.length, 'run', 'runs')}, calibration runs included)` : waiting);
  setText($('#csv-energy-info'), full ? `${rowsWord(computed.length)} · tracked, fixed, own use and net per day` : waiting);
  for (const id of ['csv-raw', 'csv-runs', 'csv-hist', 'csv-energy']) $(`#${id}`).disabled = !full;
}

function renderFooter() {
  const auto = `Refreshes every ${cfg.refreshS} s while this page is visible.`;
  setText($('#refresh-info'), state.loadedAt ? `Last checked for new data at ${fmtTime(state.loadedAt)} (UK time). ${auto}` : auto);
  $('#saved-config').hidden = !hasSavedConfig;
  setText($('#saved-config-info'), cfg.fromSaved
    ? `Showing channel ${cfg.ch1} from the settings saved on this device.`
    : 'Channel settings from a link are saved on this device, so the home-screen icon works.');
}

function render() {
  renderStatus();
  renderMessages();
  renderDay();
  renderTables();
  renderFooter();
  $('#demo-badge').hidden = !cfg.demo;
  const b = document.body;
  b.dataset.state = state.phase === 'error' ? 'error' : state.phase === 'loading' ? 'loading' : 'ready';
  b.dataset.phase = state.phase;
  b.dataset.day = state.day;
  b.dataset.renders = String(++state.renders);
}

/**
 * MATLAB charts from ThingSpeak (project lead's design). Only numeric IDs from
 * the configuration are used (config.parseVis), and only ThingSpeak's own
 * MATLAB Visualization address, so a link cannot embed another site.
 */
function renderMatlab() {
  const sec = $('#matlab');
  if (cfg.demo || !cfg.visList.length) { sec.hidden = true; return; }
  sec.hidden = false;
  const grid = $('#matlab-grid');
  grid.replaceChildren(...cfg.visList.map((v) => {
    const fig = document.createElement('figure');
    fig.dataset.vis = v.key;
    const cap = document.createElement('figcaption');
    cap.textContent = v.title;
    const frame = document.createElement('iframe');
    frame.title = `MATLAB chart from ThingSpeak: ${v.title}`;
    frame.loading = 'lazy';
    frame.referrerPolicy = 'no-referrer';
    frame.src = `https://thingspeak.mathworks.com/apps/matlab_visualizations/${encodeURIComponent(v.id)}?width=auto&height=auto`;
    const note = document.createElement('p');
    note.className = 'hint';
    note.textContent = 'Image drawn by MATLAB on ThingSpeak; the same numbers are in the tables on this page.';
    fig.append(cap, frame, note);
    return fig;
  }));
}

/* ----------------------------------------------------------------- events */

function setDay(day) {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(day) || !Number.isFinite(dayStartMs(day))) return;
  const today = utcDay(now());
  state.day = day > today ? today : day;
  state.phase = 'loading';
  render();
  refresh();
}

function exportCsv(build) {
  const { filename, header, rows } = build();
  const text = toCsv(header, rows);
  const status = $('#export-status');
  const count = `${rows.length} ${rows.length === 1 ? 'row' : 'rows'}`;
  if (cfg.exportMode !== 'clipboard') {
    downloadCsv(filename, text);
    setText(status, `Downloaded ${filename} (${count}).`);
    return;
  }
  copyCsv(text).then(
    () => setText(status, `Copied ${filename} (${count}) to the clipboard. Paste it into Excel or a text editor.`),
    () => setText(status, `Could not copy ${filename}: this browser blocked the clipboard. The deployed dashboard downloads the file instead.`),
  );
}

function wireEvents() {
  $('#day-prev').addEventListener('click', () => setDay(addDays(state.day, -1)));
  $('#day-next').addEventListener('click', () => setDay(addDays(state.day, 1)));
  $('#day-today').addEventListener('click', () => setDay(utcDay(now())));
  $('#day-input').addEventListener('change', (ev) => setDay(ev.target.value));
  $('#refresh-now').addEventListener('click', refresh);

  const span = () => { const d = rangeDays(); return [d[0], d[d.length - 1]]; };
  $('#csv-raw').addEventListener('click', () => exportCsv(() => rawCsv(rangeEntries(), ...span())));
  $('#csv-runs').addEventListener('click', () => exportCsv(() => runsCsv(runSummaries(rangeEntries()), ...span())));
  $('#csv-hist').addEventListener('click', () => exportCsv(() => {
    const entries = rangeEntries();
    const [fromDay, toDay] = span();
    return histCsv({ day: state.day, dayHist: daySummary().s.hist, fromDay, toDay, rangeHist: summarise(entries).hist, runs: runSummaries(entries) });
  }));
  $('#forget-config').addEventListener('click', () => {
    const ok = forgetSavedConfig(storage);
    hasSavedConfig = loadSavedConfig(storage) !== null;
    setText($('#saved-config-status'), ok && !hasSavedConfig
      ? 'Saved channel settings removed from this device. This page keeps showing the current channel until it is closed or reloaded.'
      : 'The saved channel settings could not be removed (the browser refused access to its storage).');
    renderFooter();
    $('#refresh-now').focus(); // the button that had focus is now hidden
  });
  $('#csv-energy').addEventListener('click', () => exportCsv(() => energyCsv(dailySummaries(rangeEntries(), now()), ...span())));

  document.querySelectorAll('details.data-table').forEach((d) => {
    const summary = $('summary', d);
    d.addEventListener('toggle', () => {
      summary.textContent = d.open ? 'Hide data table' : 'Show data table';
      if (d.open) renderDataTable(d);
    });
  });

  // Auto-refresh, paused while the tab is hidden.
  let timer = null;
  let lastRun = Date.now();
  const tick = () => { lastRun = Date.now(); refresh(); };
  const start = () => { if (timer === null) timer = setInterval(tick, cfg.refreshS * 1000); };
  const stop = () => { clearInterval(timer); timer = null; };
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) { stop(); return; }
    if (Date.now() - lastRun >= cfg.refreshS * 1000) tick();
    start();
  });
  if (!document.hidden) start();

  // Keep "x minutes ago" and the stale warning current between refreshes.
  setInterval(() => { if (!document.hidden) { renderStatus(); renderDay(); } }, 15_000);

  const redraw = () => { state.chartKey = ''; state.gainKey = ''; renderDay(); renderTables(); };
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', redraw);
}

wireEvents();
renderMatlab();
render();
refresh();
