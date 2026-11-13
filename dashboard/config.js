/**
 * Dashboard configuration.
 *
 * Edit DEFAULTS before deploying (see README "Configure"). Anything here can
 * be overridden per visit with URL query parameters, e.g.
 *   ?base=http://127.0.0.1:18766&ch1=1001&rk1=READKEY1001&ch2=1002&rk2=READKEY1002
 *   ?demo=1            built-in simulated data, no network
 *
 * SECURITY: only READ keys belong in this file. The WRITE key lives on the
 * device (firmware secrets.h) and must never be committed or put here.
 */
export const DEFAULTS = {
  base: 'https://api.thingspeak.com', // ThingSpeak API root (no trailing slash)
  ch1: '3578277',     // channel 1 "AST raw" ID (public-read since 13 Nov 2026)
  rk1: '',            // channel 1 READ key: not needed while the channel is public
  ch2: '3578278',     // channel 2 "AST results" ID (MATLAB cross-check of the daily table)
  rk2: '',            // channel 2 READ key: not needed while the channel is public
  // MATLAB Visualizations on ThingSpeak (public URLs, created 13 Nov 2026). They draw the
  // DEFAULT channels above, so resolveConfig drops them when a link points at other channels.
  vis: 'power:689480,energy:689481,gain:689485,angles:689483,status:689484',
  refreshS: 60,       // auto-refresh period [s]
  uploadMin: 15,      // device upload interval [min]; sets the stale-data limit
  historyDays: 14,    // days loaded for the daily and runs tables
  demo: false,        // true = built-in simulated data, no network (the claude.ai preview build sets this)
  exportMode: 'download', // 'clipboard' in preview frames that block downloads
};

/** MATLAB Visualizations the page knows how to caption (key -> title), in display order. */
export const VIS_TITLES = {
  power: 'Power through the latest day',
  energy: 'Energy collected through the latest day',
  gain: 'Net gain per day (from the results channel)',
  angles: 'Panel angle and sun angle, latest day',
  status: 'Status through the latest day',
};

/**
 * 'power:123,energy:124' -> [{ key:'power', id:'123', title }]. Unknown keys get
 * a generic title; IDs must be digits (anything else is ignored, so a link
 * cannot inject markup or another site's address).
 */
export function parseVis(text) {
  const out = [];
  for (const part of String(text || '').split(',')) {
    const m = part.trim().match(/^([a-z][a-z0-9_-]{0,20}):(\d{1,12})$/i);
    if (m) out.push({ key: m[1].toLowerCase(), id: m[2], title: VIS_TITLES[m[1].toLowerCase()] || `MATLAB chart ${m[2]}` });
  }
  const rank = (k) => { const i = Object.keys(VIS_TITLES).indexOf(k); return i < 0 ? 99 : i; };
  return out.sort((a, b) => rank(a.key) - rank(b.key));
}

/**
 * Data hosts a link may choose: the real ThingSpeak API, or a server on this
 * computer (the mock used by the tests). Anything else is ignored, so a crafted
 * link cannot make the group's page show another server's data or send read
 * keys there (independent review, finding 8).
 */
export function isAllowedBase(base) {
  return /^https:\/\/api\.thingspeak\.com$/.test(base) ||
    /^http:\/\/(127\.0\.0\.1|localhost|\[::1\])(:\d{1,5})?$/.test(base);
}

const PARAMS = {
  base: ['base', String],
  ch1: ['ch1', String],
  rk1: ['rk1', String],
  ch2: ['ch2', String],
  rk2: ['rk2', String],
  vis: ['vis', String],
  refreshS: ['refresh', Number],
  uploadMin: ['upload', Number],
  historyDays: ['days', Number],
};

function clamp(v, lo, hi, fallback) {
  return Number.isFinite(v) ? Math.min(hi, Math.max(lo, v)) : fallback;
}

/* ------------------------------------------------ saved channel settings */

/** localStorage key holding { base, ch1, rk1, ch2, rk2 } from the last link opened with ch1. */
export const STORAGE_KEY = 'ast-dashboard-config';
const SAVED_KEYS = ['base', 'ch1', 'rk1', 'ch2', 'rk2'];

/** window.localStorage, or null where the browser blocks it (private mode, file://, policy). */
export function safeStorage() {
  try {
    return globalThis.localStorage ?? null;
  } catch {
    return null;
  }
}

/** Saved channel settings, or null when there are none or storage cannot be read. */
export function loadSavedConfig(storage) {
  try {
    const obj = JSON.parse(storage.getItem(STORAGE_KEY));
    if (!obj || typeof obj !== 'object' || !obj.ch1) return null;
    const out = {};
    for (const k of SAVED_KEYS) if (typeof obj[k] === 'string') out[k] = obj[k];
    return out;
  } catch {
    return null;
  }
}

/** Save the five channel settings (READ keys only; never called in demo mode). Returns true when stored. */
export function saveConfig(storage, cfg) {
  if (cfg.demo || !cfg.ch1) return false;
  try {
    storage.setItem(STORAGE_KEY, JSON.stringify(Object.fromEntries(SAVED_KEYS.map((k) => [k, String(cfg[k] ?? '')]))));
    return true;
  } catch {
    return false;
  }
}

export function forgetSavedConfig(storage) {
  try {
    storage.removeItem(STORAGE_KEY);
    return true;
  } catch {
    return false;
  }
}

/**
 * Merge DEFAULTS with saved settings and URL query parameters.
 * Extra parameters: demo=1, day=YYYY-MM-DD (initial day), now=ISO time
 * (pretend clock, used by the tests and for replaying a recorded day).
 *
 * `saved` (from loadSavedConfig) is used only when the link has no ch1,
 * DEFAULTS.ch1 is empty and demo mode is off: this is what makes the "Add to
 * Home Screen" icon work, because it opens the page without the query string.
 * cfg.fromSaved says the saved settings are in use; cfg.shouldSave says the
 * link carried ch1, so the caller should store the five channel settings.
 */
export function resolveConfig(search = '', defaults = DEFAULTS, saved = null) {
  const q = new URLSearchParams(search);
  const cfg = { ...defaults };
  const demo = q.has('demo') ? q.get('demo') !== '0' : Boolean(defaults.demo);
  const urlHasCh1 = q.has('ch1') && q.get('ch1') !== '';
  cfg.fromSaved = Boolean(saved && saved.ch1 && !urlHasCh1 && !defaults.ch1 && !demo);
  if (cfg.fromSaved) for (const k of SAVED_KEYS) if (typeof saved[k] === 'string' && (k !== 'base' || saved[k])) cfg[k] = saved[k];
  cfg.shouldSave = urlHasCh1 && !demo;
  for (const [key, [param, cast]] of Object.entries(PARAMS)) {
    if (q.has(param) && q.get(param) !== '') cfg[key] = cast(q.get(param));
  }
  cfg.base = String(cfg.base).replace(/\/+$/, '');
  cfg.baseRejected = null;
  if (!isAllowedBase(cfg.base)) {
    cfg.baseRejected = cfg.base;
    cfg.base = String(defaults.base).replace(/\/+$/, '');
  }
  // The default MATLAB charts draw the default channels: showing them next to another
  // channel's numbers would mislabel them, so they need an explicit vis= in that case.
  const otherChannels = cfg.base !== String(defaults.base).replace(/\/+$/, '') || cfg.ch1 !== defaults.ch1 || cfg.ch2 !== defaults.ch2;
  if (otherChannels && !(q.has('vis') && q.get('vis') !== '')) cfg.vis = '';
  cfg.refreshS = clamp(cfg.refreshS, 15, 3600, defaults.refreshS);
  cfg.uploadMin = clamp(cfg.uploadMin, 5, 60, defaults.uploadMin);
  cfg.historyDays = Math.round(clamp(cfg.historyDays, 1, 60, defaults.historyDays));
  if (q.has('demo')) cfg.demo = q.get('demo') !== '0';
  cfg.day = /^\d{4}-\d{2}-\d{2}$/.test(q.get('day') || '') ? q.get('day') : null;
  const now = Date.parse(q.get('now') || '');
  cfg.nowOverrideMs = Number.isFinite(now) ? now : null;
  cfg.visList = parseVis(cfg.vis);
  cfg.configured = cfg.demo || Boolean(cfg.ch1);
  return cfg;
}
