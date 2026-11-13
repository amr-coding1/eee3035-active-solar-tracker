# AST data link: analysis side (MATLAB, Octave, Python reference)

EEE3035 Group 8, Active Solar Tracker. Implements `docs/CONTRACT.md` v1.5,
sections 3.2, 4, 5, 6 and 8. Everything here is owned by "Agent M".

| Path | What |
|---|---|
| `matlab/ast_nightly.m` | ThingSpeak **MATLAB Analysis**: reads channel 1, writes the last 9 complete UTC days to channel 2 in one bulk update |
| `matlab/vis_power_today.m`, `vis_energy_today.m`, `vis_angles_today.m`, `vis_status_today.m` | ThingSpeak **MATLAB Visualizations** of the latest UTC day of channel 1 |
| `matlab/vis_daily_net_gain.m` | ThingSpeak MATLAB Visualization: net gain per day (last 14 days) from channel 2 |
| `matlab/ast_report_figures.m` | desktop MATLAB / Octave function: 7 report figures (PNG 300 dpi + PDF) from the dashboard's raw CSV export |
| `py/ast_metrics.py` | Python reference of contract section 6 (the numbers MATLAB and the dashboard must match) |
| `py/test_ast_metrics.py` | 40 pytest cases with hand-computed expectations |
| `py/ast_sim.py`, `py/make_sample_csv.py` | simulator for tests and the example CSV (every sample marked `s=1` / `sim=1`) |
| `figures/` | example figures made from `figures/sample_raw_SIMULATED.csv` (**simulated data**) |
| `tests/run_tests.sh` | runs everything below |
| `tests/nightly_check.py` | `ast_nightly.m` in Octave against the mock, checked row by row against the reference |
| `tests/vis_check.py` | runs the five visualisations and the report figures in Octave |
| `tests/matlab_lint.py` | static check: no Octave-only syntax, no API keys in `matlab/*.m` |
| `tests/fixture_server.py` | minimal ThingSpeak stand-in that returns untidy JSON |
| `tests/octave_shims/webwrite.m` | MATLAB-compatible `webwrite` for Octave (tests only, see Decisions 5) |

## 1. Nightly job on ThingSpeak

Channels: 1 = `3578277` "AST raw" (public read), 2 = `3578278` "AST results".

1. ThingSpeak > **Apps > MATLAB Analysis > New > Custom (no starter code)**.
   Name it `AST nightly`. Paste the whole of `matlab/ast_nightly.m`.
2. **Save and Run** once as it is: with the key placeholder still in place
   (`DRY_RUN = 'auto'`) it is a dry run. Nothing is written; the output lists
   the nine rows that would be written and the size of the JSON body.
3. Paste the channel-2 **Write API Key** into `CH2_WRITE_KEY` (line 28) and
   **Save and Run**. Nothing else to change: `'auto'` starts writing as soon as
   a real key is there. The output ends with
   `Channel 3578278: wrote 9 row(s) in one bulk update.` Channel 2 now holds
   nine new entries. The key stays in ThingSpeak only: never commit it.
   (`DRY_RUN = true` forces a dry run; `false` refuses to run without a key.)
4. If channel 1 is made private again, paste its Read API Key into
   `CH1_READ_KEY` (empty = no `api_key` parameter is sent).

### TimeControl (when it runs)

**Apps > TimeControl > New TimeControl**: Name `AST nightly 00:20 UTC`,
Frequency **Recurring**, Recurrence **Day**, every day of the week, Action
**MATLAB Analysis**, code to execute `AST nightly`. Leave "Fuzzy Time" at 0 if
the form offers it.

The time you type is in the **time zone of the ThingSpeak account profile**
(Account > My Profile > Time Zone), not UTC. The job must run shortly after
00:00 UTC, when the UTC day is complete and the device's 00:00 upload has
arrived:

| Profile time zone | Type this time | Runs at |
|---|---|---|
| UTC (recommended: set the profile to "UTC") | 00:20 | 00:20 UTC all year |
| Europe/London, late October to late March (GMT) | 00:20 | 00:20 UTC |
| Europe/London, late March to late October (BST = UTC+1) | 01:20 | 00:20 UTC |

With a Europe/London profile and no wish to edit the TimeControl twice a year,
type **01:20**: it runs at 01:20 UTC in winter and 00:20 UTC in summer, always
after the UTC day has ended. Never use a time before 01:00 local in summer:
00:20 BST is 23:20 UTC, the day is not over, and its row would appear a day
late. The project runs in November (GMT), so 00:20 is right today.

A late run does no harm: the script always summarises the 9 complete UTC days
before the moment it runs, and rewrites them every night, so a day that was
back-filled late is corrected the next night (readers take the newest row per
date, contract 3.2).

### Message budget (free licence: 3 million messages per year)

| Writer | Messages |
|---|---|
| Device to channel 1 | 1,440 per day (one per minute, sent in bulk updates of up to 960) |
| `ast_nightly.m` to channel 2 | 7 per day (one bulk update = 1 write request, 7 entries) |
| Total | 1,447 per day = about 528,000 per year = 17.6 % of the quota |

Reads (dashboard, MATLAB Analysis, visualisations) do not count as messages.
The nightly job makes 2 read requests in the normal case (windows of 4 and 3
days: 5,760 and 4,320 entries, under the 8,000 limit) and 1 write request, so
the "one write per channel every 15 s" rule is not an issue. Run time measured
in Octave: about 3 s for 13,000 entries (limit on ThingSpeak: 20 s).

## 2. MATLAB Visualizations and iframes

For each of the five `matlab/vis_*.m` files:

1. **Apps > MATLAB Visualizations > New > Custom (no starter code)**, name it
   as the file (for example `AST power today`), paste the file.
2. Channel IDs are already filled in; read keys are empty because the channels
   are public. For a private channel paste the Read API Key into
   `CH1_READ_KEY` / `CH2_READ_KEY`.
3. **Save and Run** and check the picture.
4. Tick **Create a public URL** (below the editor) and save again. The page
   then shows the public address
   `https://thingspeak.com/apps/matlab_visualizations/<ID>`.
5. Embed it on the dashboard:

   ```html
   <iframe width="450" height="260" style="border:1px solid #cccccc;"
           title="Panel power today (MATLAB chart)"
           src="https://thingspeak.com/apps/matlab_visualizations/<ID>"></iframe>
   ```

   Give every iframe a `title` (screen readers) and keep the dashboard's own
   accessible data table next to it: a picture has no text alternative.

Only the picture is public; the code and any key in it are not shown.

| File | Shows | Reads |
|---|---|---|
| `vis_power_today.m` | P_trk, P_fix, P_use (mW) through the day | channel 1 |
| `vis_energy_today.m` | cumulative Wh: tracked, fixed, tracked minus own use; net gain so far | channel 1 |
| `vis_angles_today.m` | panel angle and sun angle; mean absolute pointing error | channel 1 |
| `vis_status_today.m` | state timeline, one row per state, minutes per state, "No data" row | channel 1 |
| `vis_daily_net_gain.m` | net gain per UTC day, last 14 days, newest row per date | channel 2 |

"Today" is the UTC day of the newest entry in channel 1. Accessibility:
Okabe-Ito colours; a different line style and marker per series; loss bars are
hatched and every bar carries its signed value; axis labels with units; "UTC"
on the time axis and in the title; the date in the title; a "No data" text
when the channel is empty; "SIMULATED DATA" in the title when any sample has
`s=1`. Duplicates are removed by `seq` (contract 6.1).

## 3. Report figures (desktop)

```matlab
addpath('analysis/matlab')
ast_report_figures('G8_raw_20261120-20261127.csv', 'figures')            % defaults
ast_report_figures(csv, out_dir, 'Day', '2026-11-21', 'DeltaTrk', 0.02, 'DeltaFix', 0.02, 'DeltaUse', 0.02)
```

Input: the dashboard's raw-sample CSV export. Output: `fig1_power_<day>`,
`fig2_energy_<day>`, `fig3_daily_net_gain`, `fig4_pointing_histogram`,
`fig5_energy_budget`, `fig6_calibration_mismatch`, `fig7_completeness` (each
`.png` at 300 dpi and `.pdf`, 16 cm x 10 cm) and `report_daily_summary.csv`.

Uncertainty: each power channel has a relative scale uncertainty (default
2 %), independent between channels. For r = E_trk / E_fix this is the group's
delta_r / r = sqrt(2) delta_P. With u = E_use / E_fix the error bar of the net
gain is `100 sqrt((r d_trk)^2 + (u d_use)^2 + ((r - u) d_fix)^2)` percentage
points (about 3 points at 2 %). Calibration runs (900 to 999) are left out of
the gain figures and shown as a panel mismatch in figure 6.

The example figures in `figures/` come from `py/make_sample_csv.py`:
**simulated data**, labelled so on every figure. Regenerate (Octave on a
machine without a display):

```sh
python3 analysis/py/make_sample_csv.py
xvfb-run -a octave --no-gui -q --eval "graphics_toolkit('qt'); addpath('analysis/matlab'); ast_report_figures('analysis/figures/sample_raw_SIMULATED.csv', 'analysis/figures');"
```

Octave prints a harmless "figure is too large and will be cropped" warning for
the PDFs; the pages are 16 cm x 10 cm and complete.

## 4. Tests

```sh
analysis/tests/run_tests.sh      # about 25 s; needs python3 + pytest, octave 8, curl, xvfb-run
```

Results on 13 Nov 2026 (GNU Octave 8.4.0, Python 3.13, the shared
`tools/mock_thingspeak.py`):

| Step | Result |
|---|---|
| pytest `py/test_ast_metrics.py` | 40 passed |
| MATLAB lint (7 files) | passed, 0 problems |
| Nightly check, scenarios A, B, D | 21 channel-2 rows: 21 identical to the reference character for character, 21 agree under rule 6.5 |
| Largest difference from the UNROUNDED reference | energies 8.0e-6 relative (6 s.f. rounding), percentages and angles 4.6e-5 (4 dp rounding) |
| Scenario C (wrong read key) | script stops with an error naming `CH1_READ_KEY`, nothing written |
| Scenario E (`DRY_RUN`) | rows printed equal the reference, nothing written; placeholder key refused when `DRY_RUN` is off |
| `ast_nightly.m` run time in Octave | 2.7 to 3.1 s for 13,400 to 15,000 entries (4 reads, 1 write); 0.3 s for 1,200 entries |
| Visualisations in Octave | 5 scripts x (data, empty channel) = 10 runs, each one figure, each under 0.3 s |
| Report figures in Octave | 7 figures x (PNG, PDF); daily table equals the reference (largest difference 4.4e-5) |

What the nightly scenarios contain: a normal day of real samples; a day with a
150-minute gap, missing fields, a negative power and unknown status keys; a
day with a re-sent batch plus a later copy that carries a different value (the
first copy must win); an empty day; a fully simulated day; a night-only day
(E_fix = 0, no pointing samples, entries without `seq`, a value on a rounding
boundary); a day re-sent four more times so a read window holds more than
8,000 entries and is split; entries before the first day and after midnight of
"today" (ignored; the run spans midnight). Scenario B back-fills the gap and
runs the next night: channel 2 then has two rows for that date and the newest
is the correct one. Scenario D uses `fixture_server.py`: a public channel (no
`api_key` sent), feeds with missing keys (jsondecode then gives a cell array
instead of a struct array), JSON numbers, `+00:00` times, null status, junk
values and an unreadable timestamp.

Also done by hand on 13 Nov 2026, not part of `run_tests.sh` (it needs the
internet): `ast_nightly.m` with `DRY_RUN` in Octave against the real channel
3578277 gave rows identical to the Python reference for the same data, and the
five visualisations drew from the real channels in Octave (1.1 to 1.7 s each).

### Not executed here

There is no MATLAB on the build machine. All seven `.m` files were executed in
GNU Octave only; the lint guards against Octave-only syntax. To be confirmed
on ThingSpeak's MATLAB by the lead:

- `ast_nightly.m`: run with `DRY_RUN = true` and compare the printed rows with
  the reference; then the real write. Two lines exist only for MATLAB and were
  never executed: the clock, `posixtime(datetime('now','TimeZone','UTC'))`,
  and the built-in `webwrite(url, char_json, weboptions('MediaType','application/json'))`.
  MathWorks documents that a character-vector `data` is sent without
  conversion. If ThingSpeak answers 401 or 400, check the key first.
- The five `vis_*.m`: appearance (fonts, legend placement) on MATLAB.
- `ast_report_figures.m` on desktop MATLAB.

## 5. MATLAB and Octave differences handled

| Topic | MATLAB | Octave 8 | Handling |
|---|---|---|---|
| `webread` of JSON | returns a decoded struct | returns text | `if ischar(reply), reply = jsondecode(reply); end` |
| `webwrite` with a JSON body | sends char data as it is | form fields only ("DATA not a valid query string") | script uses the MATLAB call; tests put `tests/octave_shims/webwrite.m` (curl) on the path |
| `jsondecode` of the feed list | struct array, or cell array when feeds have different keys; `null` is `[]` | same | both shapes are read; `[]` becomes NaN / empty text |
| Current time in UTC | `datetime` + `posixtime` | `time()` | one `if exist('OCTAVE_VERSION','builtin')` branch; everything else is POSIX seconds |
| Local functions in a script | allowed at the end | must be defined before use | ThingSpeak scripts contain no functions |
| Text | "..." is a string object | "..." is char | single quotes only |
| Bars coloured per bar, hatching | no portable call | no portable call | bars and hatch lines drawn with `patch` and `plot` |
| Octave off screen | n/a | needs `xvfb-run`, the qt toolkit, package `fonts-freefont-otf`, Ghostscript for PDF | test set-up only |

## 6. Decisions

1. **De-duplication scope is the set being summarised.** Duplicates are
   removed inside each UTC day (and inside each run). A day's result then never
   depends on which other days were loaded, so the dashboard and MATLAB agree
   whatever range each one reads. It also limits the damage if `seq` ever
   restarts (flash erased): with one global pass every new sample would be
   dropped as a "duplicate" of an old one. `nd` = received minus unique for
   that day. The dashboard must de-duplicate per day too.
2. **Tie guard in the channel-2 text.** Readers round the written values again
   (3 s.f., 1 dp, rule 6.5). Rounding twice can differ from rounding once when
   the written value lands exactly on a boundary (for example 1.2349996 Wh is
   written as 1.23500, which a reader may show as 1.24 while the dashboard's
   own value shows 1.23). This would give a false "differ" on roughly 1 day in
   300. A value whose written digits end in `500` is therefore moved one
   last-digit step back towards the unrounded value (`1.23499`). The text is
   still within one unit of the last written digit.
3. **Field values are written as JSON strings** (`"field1":"7.82209"`), so the
   stored text is exactly the contract format: 6 significant figures with
   trailing zeros, plain decimals, never exponent notation; zero is `0`.
4. **A day with no entries is still written**: N = 0, completeness 0.0000,
   energies 0, fields 4 to 6 omitted. Gains are omitted when E_fix <= 0, MAE
   when there is no pointing sample.
5. **Octave cannot post JSON with its own `webwrite`.** Rather than put a
   test-only branch into the production script, the tests shadow `webwrite`
   with a small MATLAB-compatible version. Without it Octave stops at the
   write with a clear error; `DRY_RUN` works in plain Octave.
6. **Strict number parsing**: optional sign, digits, optional decimal point and
   exponent, surrounding spaces. Anything else (`NaN`, `1,000`, text) is
   "missing". `str2double` alone would accept `1,000` and `Inf`.
7. **Read windows of 4 days** (5,760 entries), split in halves on a
   whole-minute boundary whenever a reply holds 8,000 entries.
8. **Simulated samples are counted, not removed**: `ns` in the status, a title
   line on every chart. The decision to exclude them belongs to the reader.
9. **Sums run over unique entries in time order** with the constant
   `60/3600/1000`, as in the dashboard, so the unrounded values agree to
   floating-point rounding (about 1e-13 relative).
10. **Visualisations read the day in two half-day windows** (up to 16,000
    entries with duplicates) and take "today" from the newest entry, so no
    clock or time zone is involved.
11. **Report figures leave calibration runs out of every gain** and show them
    as mismatch; uncertainties are scale errors per channel, constant through
    a day (the conservative reading of "+/- 2 %").
12. **`DRY_RUN`** (and `AST_DRY_RUN`) computes and prints everything but does
    not write and does not need the write key.
13. **Test overrides**: `AST_BASE_URL`, `AST_CH1_ID`, `AST_CH1_READ_KEY`,
    `AST_CH2_ID`, `AST_CH2_WRITE_KEY` / `AST_CH2_READ_KEY`, `AST_NOW_UTC`
    (POSIX seconds), `AST_NDAYS`, `AST_DRY_RUN`, `AST_VIS_DAY` are used when
    they exist in the workspace before `run('...')`.
