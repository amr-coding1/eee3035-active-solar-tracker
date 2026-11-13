#!/usr/bin/env node
/**
 * dash_probe.js - open the phone dashboard in headless Chrome and read what a
 * user would see. Used by run_e2e.py (never by the dashboard itself).
 *
 *   node dash_probe.js job.json      -> JSON on stdout
 *
 * job.json: { "chrome": "/usr/bin/google-chrome", "playwright": "/opt/npm-tools/node_modules/playwright",
 *             "pages": [ { "name": "...", "url": "...", "device": "iPhone 13" | "Pixel 7" | null,
 *                          "csv": false, "watchMs": 0 } ] }
 * For every page: KPI cards ([data-kpi] .kpi-value), the daily table
 * (#daily-table tbody tr, with data-* attributes and data-col per cell), the
 * status block, quality notes, messages, console/page errors, every network
 * request the page made (method + URL) and, when csv=true, the raw-samples CSV
 * export (download, or clipboard in the preview build).
 */
const fs = require('fs');

const job = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const { chromium, devices } = require(job.playwright || '/opt/npm-tools/node_modules/playwright');

async function readPage(page) {
  return page.evaluate(() => {
    const txt = (el) => (el ? el.textContent.replace(/\s+/g, ' ').trim() : null);
    const q = (sel) => document.querySelector(sel);
    const kpis = {};
    for (const el of document.querySelectorAll('[data-kpi]')) {
      kpis[el.dataset.kpi] = { value: txt(el.querySelector('.kpi-value')), note: txt(el.querySelector('.kpi-note')), sign: el.dataset.sign ?? null };
    }
    const table = (sel) => {
      const t = q(sel);
      if (!t) return null;
      const head = [...t.querySelectorAll('thead th')].map(txt);
      const rows = [...t.querySelectorAll('tbody tr')].map((tr) => ({
        data: { ...tr.dataset },
        cells: [...tr.children].map((td) => ({ col: td.dataset.col ?? null, text: txt(td), colSpan: td.colSpan })),
      }));
      return { head, rows };
    };
    const status = {};
    for (const el of document.querySelectorAll('#status [id]')) status[el.id] = txt(el);
    return {
      body: { ...document.body.dataset },
      title: document.title,
      kpis,
      kpiDay: txt(q('#kpi-day')),
      daily: table('#daily-table'),
      dailyAttrs: q('#daily') ? { ...q('#daily').dataset } : null,
      dailySource: txt(q('#daily-source')),
      runs: table('#runs-table'),
      panels: table('#panels-table'),
      status,
      fresh: q('#freshness') ? q('#freshness').dataset.fresh ?? null : null,
      fault: q('#st-fault') ? { text: txt(q('#st-fault')), flag: q('#st-fault').dataset.fault ?? null } : null,
      quality: [...document.querySelectorAll('#quality li')].map(txt),
      messages: txt(q('#messages')),
      summary: txt(q('#summary')),
      refreshInfo: txt(q('#refresh-info')),
      exportStatus: txt(q('#export-status')),
      mainText: txt(q('main')) ? txt(q('main')).slice(0, 20000) : null,
    };
  });
}

async function probe(browser, spec) {
  const dev = spec.device && devices[spec.device] ? devices[spec.device] : { viewport: { width: 390, height: 844 } };
  const context = await browser.newContext({ ...dev, acceptDownloads: true });
  try { await context.grantPermissions(['clipboard-read', 'clipboard-write']); } catch { /* not all builds */ }
  const page = await context.newPage();
  const out = { name: spec.name, url: spec.url, device: spec.device || null, consoleErrors: [], pageErrors: [], requests: [] };
  page.on('console', (m) => { if (m.type() === 'error') out.consoleErrors.push(m.text().slice(0, 300)); });
  page.on('pageerror', (e) => out.pageErrors.push(String(e).slice(0, 300)));
  page.on('request', (r) => out.requests.push({ method: r.method(), url: r.url(), t: Date.now() }));
  const t0 = Date.now();
  try {
    await page.goto(spec.url, { waitUntil: 'load', timeout: 30000 });
    await page.waitForFunction(() => {
      const b = document.body.dataset;
      if (b.phase === 'full' || b.phase === 'error' || b.state === 'error') return true;
      if (b.phase !== undefined) return false;
      // fallback when the page does not publish its phase: table filled and not busy
      const rows = document.querySelectorAll('#daily-table tbody tr');
      return rows.length > 0 && !document.body.hasAttribute('aria-busy') && !/Loading/.test(rows[0].textContent);
    }, null, { timeout: 45000 });
    out.ready = true;
  } catch (err) {
    out.ready = false;
    out.error = String(err).slice(0, 300);
  }
  out.loadMs = Date.now() - t0;
  try {
    Object.assign(out, await readPage(page));
  } catch (err) {
    out.error = (out.error || '') + ' read: ' + String(err).slice(0, 300);
  }
  try {
    out.defaults = await page.evaluate(async () => {
      const m = await import('./config.js');
      return { refreshS: m.DEFAULTS.refreshS, uploadMin: m.DEFAULTS.uploadMin, exportMode: m.DEFAULTS.exportMode };
    });
  } catch { out.defaults = null; }
  if (spec.csv && out.ready) {
    out.csv = { via: null, text: null, filename: null };
    try {
      const dl = page.waitForEvent('download', { timeout: 4000 }).catch(() => null);
      await page.click('#csv-raw');
      const d = await dl;
      if (d) {
        const p = await d.path();
        out.csv = { via: 'download', filename: d.suggestedFilename(), text: fs.readFileSync(p, 'utf8') };
      } else {
        await page.waitForTimeout(300);
        const text = await page.evaluate(() => navigator.clipboard.readText()).catch(() => null);
        out.csv = { via: text ? 'clipboard' : null, filename: null, text };
      }
      out.exportStatus = await page.evaluate(() => document.querySelector('#export-status')?.textContent ?? null);
    } catch (err) {
      out.csv.error = String(err).slice(0, 300);
    }
  }
  if (spec.watchMs) {
    // auto-refresh evidence: count data requests made after the first load
    const mark = Date.now();
    await page.waitForTimeout(spec.watchMs);
    out.watch = { ms: spec.watchMs, feedRequests: out.requests.filter((r) => r.t > mark && /feeds\.json/.test(r.url)).length };
    try { out.afterWatch = await readPage(page); } catch { /* ignore */ }
  }
  await context.close();
  return out;
}

(async () => {
  const browser = await chromium.launch({ executablePath: job.chrome, headless: true, args: ['--no-sandbox', '--disable-gpu'] });
  const results = [];
  try {
    for (const spec of job.pages) results.push(await probe(browser, spec));
  } finally {
    await browser.close();
  }
  process.stdout.write(JSON.stringify({ ok: true, pages: results }));
})().catch((err) => {
  process.stdout.write(JSON.stringify({ ok: false, error: String(err) }));
  process.exit(1);
});
