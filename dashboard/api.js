/**
 * ThingSpeak read client (CONTRACT.md section 8).
 * Works against api.thingspeak.com and the local mock server.
 */
import { DAY_MS } from './metrics.js';

export const MAX_RESULTS = 8000;
export const MAX_WINDOW_DAYS = 5;

/** Error with a `kind` the UI turns into a plain-language message. */
export class ApiError extends Error {
  constructor(kind, message, detail = '') {
    super(message);
    this.name = 'ApiError';
    this.kind = kind; // 'key' | 'network' | 'http' | 'format' | 'config'
    this.detail = detail;
  }
}

/** ms epoch -> 'YYYY-MM-DD HH:NN:SS' (UTC), the format ThingSpeak expects. */
export function tsTime(ms) {
  return new Date(ms).toISOString().slice(0, 19).replace('T', ' ');
}

/**
 * Split [startMs, endMs] (inclusive, whole seconds) into consecutive windows
 * of at most `maxDays` days. Windows do not overlap: each ends 1 s before the
 * next one starts.
 */
export function planWindows(startMs, endMs, maxDays = MAX_WINDOW_DAYS) {
  const out = [];
  const span = maxDays * DAY_MS;
  for (let s = startMs; s <= endMs; s += span) {
    out.push({ start: s, end: Math.min(endMs, s + span - 1000) });
  }
  return out;
}

export function feedsUrl(base, channel, key, params = {}) {
  const q = new URLSearchParams();
  if (key) q.set('api_key', key);
  if (params.start !== undefined) q.set('start', tsTime(params.start));
  if (params.end !== undefined) q.set('end', tsTime(params.end));
  q.set('timezone', 'Etc/UTC');
  q.set('status', 'true');
  q.set('results', String(params.results ?? MAX_RESULTS));
  // URLSearchParams writes spaces as '+'; ThingSpeak documents %20.
  return `${base}/channels/${encodeURIComponent(channel)}/feeds.json?${q.toString().replace(/\+/g, '%20')}`;
}

async function getJson(url, fetchImpl, signal) {
  let resp;
  try {
    resp = await fetchImpl(url, { signal, cache: 'no-store' });
  } catch (err) {
    if (err?.name === 'AbortError') throw err;
    throw new ApiError('network', 'Network error', String(err?.message || err));
  }
  const text = (await resp.text()).trim();
  if (text === '-1') throw new ApiError('key', 'Read key rejected');
  if (!resp.ok) {
    throw new ApiError(resp.status === 404 ? 'notfound' : 'http', `HTTP ${resp.status}`, text.slice(0, 200));
  }
  try {
    const body = JSON.parse(text);
    if (!body || !Array.isArray(body.feeds)) throw new Error('no feeds array');
    return body;
  } catch (err) {
    throw new ApiError('format', 'Unexpected response', String(err.message));
  }
}

/**
 * Read every feed with start <= created_at <= end, paging in windows of at
 * most 5 days. If a window comes back full (8000 entries, so older ones may
 * have been cut off) it is split in half and read again.
 */
export async function fetchRange(cfg, channel, key, startMs, endMs, { fetchImpl = fetch, signal } = {}) {
  const feeds = [];
  const queue = planWindows(startMs, endMs);
  while (queue.length) {
    const w = queue.shift();
    const body = await getJson(feedsUrl(cfg.base, channel, key, w), fetchImpl, signal);
    if (body.feeds.length >= MAX_RESULTS && w.end - w.start > 120_000) {
      const mid = w.start + Math.floor((w.end - w.start) / 2000) * 1000;
      queue.unshift({ start: w.start, end: mid - 1000 }, { start: mid, end: w.end });
      continue;
    }
    feeds.push(...body.feeds);
  }
  return feeds;
}

/** Newest `n` feeds of a channel (also validates the read key). */
export async function fetchLatest(cfg, channel, key, n = 1, { fetchImpl = fetch, signal } = {}) {
  const body = await getJson(feedsUrl(cfg.base, channel, key, { results: n }), fetchImpl, signal);
  return body.feeds;
}
