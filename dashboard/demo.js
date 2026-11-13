/**
 * Built-in simulated data for ?demo=1 (no network needed, e.g. at the lab).
 *
 * Deterministic: the same clock time always gives the same numbers. It
 * imitates three UTC days ending "now": a clear November day shape, passing
 * cloud, tracker moves, two panel-swap runs (A then B), a short gap and a
 * sensor fault yesterday, and one calibration run (r=900, both panels fixed)
 * on the oldest day. Output has the same shape as ThingSpeak feeds, so the
 * demo exercises exactly the same parsing and metric code as live data.
 *
 * Honest labelling (contract v1.4): every sample carries `;s=1`, the marker
 * the firmware's own simulator uses, so every view and export flags the demo
 * numbers as simulated. The channel-2 entries imitate the nightly MATLAB job
 * (00:10 UTC, last complete days, created_at = job start minus k seconds) and
 * assume the last 15 minutes of a day are uploaded just after the job ran, so
 * the newest day is summarised short until the next night corrects it.
 */
import { MIN_MS, addDays, dayStartMs, parseFeed, summarise, utcDay } from './metrics.js';

export const DEMO_DAYS = 3;
const SUNRISE_MIN = 450;   // 07:30 UTC
const SUNSET_MIN = 960;    // 16:00 UTC
const P_MAX_MW = 1500;
const STOW_DEG = -60;
const CAL_RUN = 900;       // calibration run ID (900-999)
const CAL_MISMATCH = 1.02; // demo panel A reads 2 % above panel B at the same orientation
const JOB_MIN = 10;        // the MATLAB TimeControl job starts at 00:10 UTC
const LATE_MIN = 15;       // minutes of a day that reach the host only after that night's job
export const DEMO_RESULT_DAYS = 9; // the job re-summarises the last 9 complete days (contract 3.2, v1.5)

function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const rad = (deg) => (deg * Math.PI) / 180;

/** One simulated day. `offset` is 0 for today, -1 yesterday, ... */
function simulateDay(day, offset, seqStart, untilMs) {
  const rand = mulberry32(1000 + (offset + 10) * 7919);
  const base = dayStartMs(day);
  const cloudiness = [0.15, 0.45, 0.3][((offset % 3) + 3) % 3];
  const cloudKnots = Array.from({ length: 146 }, () => 1 - cloudiness * rand() ** 2 * 1.6);
  const feeds = [];
  let panel = STOW_DEG;
  for (let m = 0; m < 1440; m++) {
    const t = base + m * MIN_MS;
    if (t > untilMs) break;
    const seq = seqStart + m;
    const noise = () => 1 + (rand() - 0.5) * 0.02;
    const daylight = m >= SUNRISE_MIN && m <= SUNSET_MIN;
    let run = 0;
    let onTracker = 'A';
    if (offset === -1 && m >= 600 && m < 720) run = 1;
    if (offset === -1 && m >= 750 && m < 870) { run = 2; onTracker = 'B'; }
    if (offset === -1 && m >= 720) onTracker = 'B';
    // Calibration run on the oldest day, 10:00-11:00 UTC: tracker parked at 0 deg beside the fixed panel.
    const calibrating = offset === -(DEMO_DAYS - 1) && m >= 600 && m < 660;
    if (calibrating) run = CAL_RUN;
    const f = { created_at: new Date(t).toISOString().replace('.000Z', 'Z'), entry_id: seq + 1 };
    let state = 'IDL';
    let fault = 0;
    let use = 3 * noise();
    if (daylight) {
      const frac = (m - SUNRISE_MIN) / (SUNSET_MIN - SUNRISE_MIN);
      const sun = -60 + 120 * frac;
      const k = m / 10;
      const cloud = Math.max(0.2, cloudKnots[Math.floor(k)] * (1 - (k % 1)) + cloudKnots[Math.floor(k) + 1] * (k % 1));
      const irr = Math.sin(Math.PI * frac) ** 1.2 * cloud;
      const sensorFault = offset === -1 && m >= 680 && m < 683;
      state = 'TRK';
      use = 15 * noise();
      if (calibrating) {
        panel = 0;
        state = 'STW';
        use = 3 * noise();
      } else if (sensorFault) {
        state = 'HLD';
        fault = 5;
      } else if (Math.abs(sun - panel) > 4) {
        panel = Math.round((sun + 1.5) * 10) / 10;
        state = 'MOV';
        use = 300 * noise();
      }
      const swap = onTracker === 'B' ? 0.97 : 1;
      const fixed = P_MAX_MW * irr * Math.cos(rad(sun));
      f.field1 = ((calibrating ? fixed * CAL_MISMATCH : P_MAX_MW * irr * Math.cos(rad(sun - panel)) * swap) * noise()).toFixed(1);
      f.field2 = ((fixed * noise()) / swap).toFixed(1);
      if (!sensorFault) f.field5 = (sun + (rand() - 0.5) * 0.6).toFixed(1);
      f.field6 = (8 + 14 * irr).toFixed(1);
    } else {
      if (m > SUNSET_MIN) { panel = STOW_DEG; state = m < SUNSET_MIN + 3 ? 'STW' : 'IDL'; }
      f.field1 = '0.0';
      f.field2 = '0.0';
      f.field6 = '6.5';
    }
    f.field3 = use.toFixed(1);
    f.field4 = panel.toFixed(1);
    f.field7 = (3.95 - 0.1 * (m / 1440) + (rand() - 0.5) * 0.01).toFixed(3);
    f.field8 = String(seq);
    f.status = `${state};r=${run};p=${onTracker};e=${fault};s=1`;
    const lost = (offset === -1 && m >= 790 && m < 805) || (offset === -2 && m >= 540 && m < 580);
    if (!lost) feeds.push(f);
  }
  return feeds;
}

/** Simulated channel-1 feeds for the DEMO_DAYS days ending at nowMs (newest sample ~3 min old). */
export function demoRawFeeds(nowMs) {
  const today = utcDay(nowMs);
  const until = Math.floor((nowMs - 3 * MIN_MS) / MIN_MS) * MIN_MS;
  const feeds = [];
  for (let offset = -(DEMO_DAYS - 1); offset <= 0; offset++) {
    feeds.push(...simulateDay(addDays(today, offset), offset, (offset + DEMO_DAYS - 1) * 1440, until));
  }
  return feeds;
}

/**
 * Simulated channel-2 feeds, as the v1.4 MATLAB job writes them: every night at
 * 00:10 UTC it summarises the last complete days from what the host holds at
 * that moment; entry k (0 = yesterday) gets created_at = job start minus k s.
 * The last LATE_MIN minutes of the day before each job are taken to arrive
 * after it, so: the newest complete day is summarised short (the dashboard
 * shows "differs") and older days were corrected by a later night ("agrees").
 */
export function demoResultFeeds(rawFeeds, nowMs) {
  const entries = rawFeeds.map(parseFeed);
  const days = [...new Set(entries.map((e) => utcDay(e.t)))].sort();
  const fx = (v, dp) => (v === null ? null : v.toFixed(dp));
  const feeds = [];
  for (const jobDay of days.slice(1)) {
    const jobStart = dayStartMs(jobDay) + JOB_MIN * MIN_MS;
    if (jobStart > nowMs) break;
    const onHost = dayStartMs(jobDay) - LATE_MIN * MIN_MS; // samples from here on had not been uploaded yet
    for (let k = 0; k < DEMO_RESULT_DAYS; k++) {
      const date = addDays(jobDay, -(k + 1));
      if (date < days[0]) break;
      const s = summarise(entries.filter((e) => utcDay(e.t) === date && e.t < onHost), 1440);
      feeds.push({
        created_at: new Date(jobStart - k * 1000).toISOString().replace('.000Z', 'Z'),
        field1: fx(s.eTrk, 4), field2: fx(s.eFix, 4), field3: fx(s.eUse, 4),
        field4: fx(s.gGross, 2), field5: fx(s.gNet, 2), field6: fx(s.mae, 2),
        field7: fx(s.complete, 2), field8: String(s.n),
        status: `period=day;date=${date}`,
      });
    }
  }
  feeds.sort((a, b) => (a.created_at < b.created_at ? -1 : 1));
  return feeds.map((f, i) => ({ ...f, entry_id: i + 1 }));
}
