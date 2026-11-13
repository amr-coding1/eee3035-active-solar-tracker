/**
 * Display formatting (CONTRACT.md section 6: energies 3 significant figures,
 * gains 1 dp, angles 1 dp; times in Europe/London).
 */
export const TZ = 'Europe/London';
export const NA = 'n/a';
const MINUS = '−';

function nice(s) {
  return s.replace('-', MINUS);
}

/** 3 significant figures without exponent notation: 12.3, 0.457, 1230, 0.00. */
export function fmtSig(x, sig = 3) {
  if (x === null || x === undefined || !Number.isFinite(x)) return NA;
  if (x === 0) return (0).toFixed(sig - 1);
  const mag = Math.floor(Math.log10(Math.abs(x)));
  const rounded = Number(x.toPrecision(sig));
  const mag2 = Math.floor(Math.log10(Math.abs(rounded))); // 9.996 -> 10.0
  const decimals = Math.max(0, sig - 1 - Math.max(mag, mag2));
  return nice(rounded.toFixed(decimals));
}

export function fmtFixed(x, dp = 1) {
  if (x === null || x === undefined || !Number.isFinite(x)) return NA;
  const s = x.toFixed(dp);
  return nice(Number(s) === 0 ? (0).toFixed(dp) : s); // no "-0.0"
}

/** Signed percentage, 1 dp: "+18.4", "−3.2", "0.0". */
export function fmtSigned(x, dp = 1) {
  const s = fmtFixed(x, dp);
  return s === NA || s.startsWith(MINUS) || Number(s) === 0 ? s : `+${s}`;
}

/** Sign in words and a symbol so gain/loss never depends on colour. */
export function gainWords(x) {
  if (x === null || x === undefined || !Number.isFinite(x)) return { symbol: '', word: 'not available', cls: 'na' };
  const r = Number(x.toFixed(1));
  if (r > 0) return { symbol: '▲', word: 'gain', cls: 'gain' };
  if (r < 0) return { symbol: '▼', word: 'loss', cls: 'loss' };
  return { symbol: '●', word: 'no change', cls: 'even' };
}

const timeFmt = new Intl.DateTimeFormat('en-GB', { timeZone: TZ, hour: '2-digit', minute: '2-digit', hourCycle: 'h23' });
const dateTimeFmt = new Intl.DateTimeFormat('en-GB', {
  timeZone: TZ, day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit', hourCycle: 'h23',
});
const dateFmt = new Intl.DateTimeFormat('en-GB', { timeZone: 'UTC', weekday: 'short', day: 'numeric', month: 'short', year: 'numeric' });
const isoFmt = new Intl.DateTimeFormat('en-CA', {
  timeZone: TZ, year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit', hourCycle: 'h23',
});

/** "14:05" in UK time. */
export const fmtTime = (ms) => timeFmt.format(ms);
/** "20 Nov, 14:05" in UK time. */
export const fmtDateTime = (ms) => dateTimeFmt.format(ms);
/** "Fri, 20 Nov 2026" for a UTC day string. */
export const fmtDay = (day) => dateFmt.format(Date.parse(`${day}T12:00:00Z`));
/** "2026-11-20 14:05:00" in UK time (for CSV). */
export const fmtLondonIso = (ms) => isoFmt.format(ms).replace(',', '');

/** "4 minutes", "1 hour 12 minutes", "3 days 2 hours". */
export function fmtDuration(ms) {
  const min = Math.max(0, Math.floor(ms / 60_000));
  const unit = (n, w) => `${n} ${w}${n === 1 ? '' : 's'}`;
  if (min < 1) return 'less than a minute';
  if (min < 60) return unit(min, 'minute');
  const h = Math.floor(min / 60);
  if (h < 24) return min % 60 ? `${unit(h, 'hour')} ${unit(min % 60, 'minute')}` : unit(h, 'hour');
  const d = Math.floor(h / 24);
  return h % 24 ? `${unit(d, 'day')} ${unit(h % 24, 'hour')}` : unit(d, 'day');
}

/** CSV number: plain decimal, up to `dp` places, empty for null. */
export function csvNum(x, dp = 6) {
  if (x === null || x === undefined || !Number.isFinite(x)) return '';
  return String(Number(x.toFixed(dp)));
}
