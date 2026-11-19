# AST phone dashboard

Read-only web page for the Active Solar Tracker (EEE3035 Group 8): the "timed display and analysis
on a portable device" part of the data link. It reads ThingSpeak channel 1 (one sample per minute)
and channel 2 (one MATLAB summary per day) and works everything out on the phone, following
`docs/CONTRACT.md` section 6, so its numbers can be checked against MATLAB day by day.

No build step, no server code, no keys: plain HTML, CSS and ES modules plus Chart.js
(`vendor/chart.umd.js`, 4.x, kept locally so the page works without a CDN).

## What it shows

- Today's (or a chosen UTC day's) energy of the tracked and the fixed panel, own use, gross and net
  gain, completeness, servo moves and mean pointing error, with data-age and data-quality notes.
- Charts: power, cumulative energy, running gain, panel and sun angle, status timeline, pointing-error
  histogram, net gain per day. Every chart has a "Show data table" text alternative.
- Daily table for the last 14 days with a "MATLAB check" column: agrees / differs / not yet
  summarised, comparing this page's figures with channel 2 (contract 6.5).
- Runs table (per test run and panel) and calibration runs (900-999).
- The five ThingSpeak MATLAB Visualizations embedded as a second, independent view.
- CSV export of raw samples, daily summaries and runs.
- Simulated samples (s=1) are labelled everywhere; `?demo=1` runs on built-in simulated data.

## Configure

`config.js` > `DEFAULTS`. Already set for the group's channels:

| Key | Value | Meaning |
|---|---|---|
| `ch1`, `ch2` | `3578277`, `3578278` | channel IDs |
| `rk1`, `rk2` | `''` | READ keys; empty while the channels are public. Never a WRITE key |
| `vis` | `power:689480,energy:689481,gain:689485,angles:689483,status:689484` | MATLAB charts to embed (digits only) |
| `refreshS`, `uploadMin`, `historyDays` | 60, 15, 14 | refresh period, device upload interval, days loaded |

URL parameters override the defaults for one visit: `?ch1=...&ch2=...&rk1=...&rk2=...&base=...`,
`?day=YYYY-MM-DD`, `?demo=1`, `?refresh=30`, `?days=21`, `?vis=power:123`. When a link points at
other channels, the default MATLAB charts are left out (they draw the default channels).
A link that carries `ch1` is remembered on that phone, so the home-screen icon reopens it.

## Deploy

Any static host. GitHub Pages: upload the contents of this folder (`tests/` and `scripts/` are not
needed) to a public repository, then Settings > Pages > branch `main`, folder `/`. Step-by-step in
`docs/SETUP.md`, step 2. Opening `index.html` straight from the disk does not work (browsers block
ES modules on `file://`); for a local look run `python3 -m http.server` in this folder.

## Tests

```
node --test tests/metrics.test.mjs tests/api_config_exports.test.mjs   # 32 unit tests
node --test tests/browser.test.mjs                                       # 9 tests in real Chrome
```

The browser tests start `tools/mock_thingspeak.py`, seed it with simulated days and check: KPIs
equal `metrics.js`, the MATLAB check (agree / differ / missing), the status table, no axe-core
WCAG 2.2 A/AA violations in light and dark mode, no horizontal scrolling at 320 px, error messages,
CSV header, demo mode and keyboard access. They need Playwright (`/opt/npm-tools`), Chrome at
`/usr/bin/google-chrome` and axe-core (`AXE_PATH`). The end-to-end tests
(`tests/e2e/run_e2e.py`) also drive this page against the firmware and the nightly job.

## Security

The page only sends GET requests to the read API (checked in E2E S10.9). Channel data are put on
the page as text, never as HTML; MATLAB chart IDs must be digits, so a link cannot inject markup
or another site's address into an iframe. A link may only choose `https://api.thingspeak.com` or a
server on this computer as the data host (`base=`); anything else is ignored with a note.
