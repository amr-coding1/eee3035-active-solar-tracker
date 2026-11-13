// Browser tests for the phone dashboard: real Chrome (Playwright) against the
// mock ThingSpeak server. Run: node --test tests/browser.test.mjs
// Needs: python3, tools/mock_thingspeak.py, Chrome for Testing at /usr/bin/google-chrome,
// Playwright (/opt/npm-tools) and axe-core (AXE_PATH or /tmp/axe/node_modules/axe-core/axe.min.js).
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { chromium } from '/opt/npm-tools/node_modules/playwright/index.mjs';
import { demoRawFeeds } from '../demo.js';
import { parseFeed, summarise, dailySummaries, utcDay, expectedForDay } from '../metrics.js';
import { fmtSig, fmtFixed, fmtSigned } from '../format.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DASH = path.resolve(HERE, '..');
const ROOT = path.resolve(DASH, '..');
const AXE = process.env.AXE_PATH || '/tmp/axe/node_modules/axe-core/axe.min.js';
const NOW = Date.parse('2026-11-20T14:00:00Z');   // virtual "now" for the page (?now=)

function startProc(cmd, args, readyRe) {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let buf = '';
    const onData = (d) => { buf += d; const m = buf.match(readyRe); if (m) resolve({ p, m }); };
    p.stdout.on('data', onData); p.stderr.on('data', onData);
    p.on('exit', (c) => reject(new Error(`${cmd} exited ${c}: ${buf.slice(-500)}`)));
    setTimeout(() => reject(new Error(`${cmd} not ready: ${buf.slice(-500)}`)), 15000);
  });
}

let mock, web, base, site, browser, feeds;

test.before(async () => {
  const m = await startProc('python3', [path.join(ROOT, 'tools/mock_thingspeak.py'), '--port', '0', '--channel', '1001:W1001:', '--channel', '1002:W1002:'], /LISTENING (http:\/\/127\.0\.0\.1:\d+)/);
  mock = m.p; base = m.m[1];
  const w = await startProc('python3', ['-m', 'http.server', '0', '--bind', '127.0.0.1', '--directory', DASH], /port (\d+)/);
  web = w.p; site = `http://127.0.0.1:${w.m[1]}`;

  // Seed channel 1001 with the deterministic demo days ending at NOW (every sample s=1).
  feeds = demoRawFeeds(NOW);
  let t = Math.floor(NOW / 1000) - 3 * 86400;
  for (let i = 0; i < feeds.length; i += 900) {
    const updates = feeds.slice(i, i + 900).map(({ entry_id, ...f }) => f);
    t += 20; // >= 15 s between writes, in mock virtual time
    const r = await fetch(`${base}/channels/1001/bulk_update.json`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Sim-Time': String(t) }, body: JSON.stringify({ write_api_key: 'W1001', updates }) });
    assert.equal(r.status, 202, await r.text());
  }
  // Channel 1002: one agreeing summary (yesterday) and one deliberately wrong one (day before).
  const entries = feeds.map(parseFeed);
  const days = dailySummaries(entries, NOW);
  const row = (d, eTrkFactor) => ({
    created_at: new Date(NOW - 3600_000).toISOString().replace('.000Z', 'Z'),
    field1: (d.eTrk * eTrkFactor).toPrecision(6), field2: d.eFix.toPrecision(6), field3: d.eUse.toPrecision(6),
    field4: d.gGross?.toFixed(4), field5: d.gNet?.toFixed(4), field6: d.mae?.toFixed(4), field7: d.complete.toFixed(4),
    field8: String(d.nUnique), status: `period=day;date=${d.day};v=1.5;ns=${d.nSim};nd=${d.duplicates}`,
  });
  const y = days.find((d) => d.day === utcDay(NOW - 86400_000));
  const yy = days.find((d) => d.day === utcDay(NOW - 2 * 86400_000));
  const r2 = await fetch(`${base}/channels/1002/bulk_update.json`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Sim-Time': String(t + 30) },
    body: JSON.stringify({ write_api_key: 'W1002', updates: [{ ...row(yy, 1.10), created_at: new Date(NOW - 3600_000).toISOString() }, { ...row(y, 1), created_at: new Date(NOW - 3599_000).toISOString() }] }) });
  assert.equal(r2.status, 202, await r2.text());
  browser = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
});

test.after(async () => { await browser?.close(); mock?.kill(); web?.kill(); });

const liveUrl = (extra = '') => `${site}/index.html?base=${encodeURIComponent(base)}&ch1=1001&ch2=1002&now=${new Date(NOW).toISOString()}${extra}`;

async function open(url, opts = {}) {
  const ctx = await browser.newContext({ viewport: opts.viewport || { width: 412, height: 915 }, colorScheme: opts.colorScheme || 'light', acceptDownloads: true });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(url);
  if (!opts.noWait) await page.waitForFunction(() => document.body.dataset.phase === 'full' || document.body.dataset.state === 'error', null, { timeout: 30000 });
  return { ctx, page, errors };
}

test('live mode: KPIs equal metrics.js on the same samples; MATLAB check agrees/differs as seeded', async () => {
  const { ctx, page, errors } = await open(liveUrl());
  const today = utcDay(NOW);
  const es = feeds.map(parseFeed).filter((e) => utcDay(e.t) === today);
  const s = summarise(es, expectedForDay(today, NOW, es[es.length - 1].t));
  const kpi = async (k) => (await page.textContent(`[data-kpi="${k}"] .kpi-value`)).trim();
  assert.equal(await kpi('eTrk'), fmtSig(s.eTrk));
  assert.equal(await kpi('eFix'), fmtSig(s.eFix));
  assert.equal(await kpi('gNet'), fmtSigned(s.gNet));
  assert.equal(await kpi('mae'), fmtFixed(s.mae));
  const checks = await page.$$eval('#daily-table tbody tr', (trs) => trs.map((tr) => [tr.dataset.date, tr.dataset.matlab]));
  const byDate = Object.fromEntries(checks);
  assert.equal(byDate[utcDay(NOW - 86400_000)], 'agree');
  assert.equal(byDate[utcDay(NOW - 2 * 86400_000)], 'differ');
  assert.equal(byDate[today], 'missing');
  assert.match(await page.textContent('#st-data'), /Simulated/);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('status timeline: data table lists periods and totals', async () => {
  const { ctx, page } = await open(liveUrl(`&day=${utcDay(NOW - 86400_000)}`));
  await page.click('#fig-status summary');
  await page.waitForSelector('#fig-status table tbody tr'); // the table is built on the async 'toggle' event
  const rows = await page.$$eval('#fig-status table tbody tr', (trs) => trs.map((tr) => [...tr.cells].map((c) => c.textContent)));
  assert.ok(rows.length >= 5, `rows ${rows.length}`);
  assert.ok(rows.some((r) => /No data \(gap\)/.test(r[2])), 'the seeded 15-minute gap shows as a GAP period');
  assert.match(await page.textContent('#fig-status caption'), /Totals: .*Tracking/);
  await ctx.close();
});

test('accessibility: no axe-core violations (WCAG 2.x A/AA), light and dark', { skip: !existsSync(AXE) }, async () => {
  for (const colorScheme of ['light', 'dark']) {
    const { ctx, page } = await open(liveUrl('&vis=power:111'), { colorScheme });
    await page.addScriptTag({ content: readFileSync(AXE, 'utf8') });
    const res = await page.evaluate(async () => {
      // The ThingSpeak iframe is a third-party document; it is checked separately.
      const r = await window.axe.run(document, { runOnly: { type: 'tag', values: ['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa'] }, exclude: [['#matlab-grid iframe']] });
      return r.violations.map((v) => `${v.id}: ${v.nodes.length} (${v.nodes[0]?.target})`);
    });
    assert.deepEqual(res, [], `${colorScheme}: ${res.join('; ')}`);
    await ctx.close();
  }
});

test('narrow phone (320 px): no horizontal scrolling', async () => {
  const { ctx, page } = await open(liveUrl(), { viewport: { width: 320, height: 640 } });
  const over = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
  assert.ok(over <= 1, `overflow ${over}px`);
  await ctx.close();
});

test('MATLAB charts: hidden unless configured; only numeric IDs become ThingSpeak iframes', async () => {
  let { ctx, page } = await open(liveUrl());
  assert.equal(await page.isHidden('#matlab'), true);
  await ctx.close();
  ({ ctx, page } = await open(liveUrl('&vis=' + encodeURIComponent('gain:333,power:111,evil:"><b>,status:abc'))));
  const srcs = await page.$$eval('#matlab-grid iframe', (fs) => fs.map((f) => [f.src, f.title]));
  assert.equal(srcs.length, 2);
  assert.match(srcs[0][0], /^https:\/\/thingspeak\.mathworks\.com\/apps\/matlab_visualizations\/111\?/);
  assert.match(srcs[1][1], /Net gain per day/);
  await ctx.close();
});

test('errors are explained in words: unknown channel, host unreachable', async () => {
  let { ctx, page } = await open(`${site}/index.html?base=${encodeURIComponent(base)}&ch1=999999&ch2=&now=${new Date(NOW).toISOString()}`);
  assert.match(await page.textContent('#messages'), /not found/i);
  await ctx.close();
  ({ ctx, page } = await open(`${site}/index.html?base=http://127.0.0.1:9&ch1=1001&ch2=`));
  assert.match(await page.textContent('#messages'), /Cannot reach the data host/);
  await ctx.close();
});

test('CSV export downloads with the documented header', async () => {
  const { ctx, page } = await open(liveUrl());
  const [dl] = await Promise.all([page.waitForEvent('download'), page.click('#csv-raw')]);
  const text = readFileSync(await dl.path(), 'utf8');
  assert.match(dl.suggestedFilename(), /^G8_raw_\d{8}-\d{8}\.csv$/);   // DMP naming rule
  assert.equal(text.split('\r\n')[0], 'created_at_utc,time_uk,entry_id,seq,p_trk_mw,p_fix_mw,p_use_mw,ang_panel_deg,ang_sun_deg,temp_c,vbat_v,state,run,panel,fault,sim,status');
  await ctx.close();
});

test('demo mode needs no network and is labelled simulated', async () => {
  const { ctx, page, errors } = await open(`${site}/index.html?demo=1&now=${new Date(NOW).toISOString()}`);
  assert.equal(await page.isVisible('#demo-badge'), true);
  assert.match(await page.textContent('#summary'), /simulated/);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('keyboard: the first Tab reaches the skip link', async () => {
  const { ctx, page } = await open(liveUrl());
  await page.keyboard.press('Tab');
  assert.equal(await page.evaluate(() => document.activeElement.className), 'skip-link');
  await ctx.close();
});
