# AST data link - end-to-end test results

Generated 2026-11-13T16:07:12Z by `tests/e2e/run_e2e.py` (contract v1.5). Virtual time starts at 2026-11-20T00:00:00Z.

| Component | Used in this run |
|---|---|
| Device | firmware/host/build/ast_host_device (REAL firmware core, host build) |
| Server | tools/mock_thingspeak.py (mock; HTTP on 127.0.0.1; virtual time) |
| Nightly job | analysis/matlab/ast_nightly.m run by GNU Octave, version 8.4.0 (REAL script; Octave stands in for MATLAB) |
| Reference | analysis/py/ast_metrics.py |
| Dashboard | dashboard/ in headless Google Chrome via Playwright (REAL dashboard code) |

**129 checks: 129 PASS, 0 FAIL, 0 not run.** Wall time 66 s (31 device runs, 16 nightly runs, slowest 2.5 s, 24 dashboard pages).

## S1 - Three normal days: PASS

Proves: normal operation: 1 sample/min, contract formats, 15-minute uploads, nightly summary, dashboard table.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S1.1 | device runs 3 days + 16 min of virtual time | exit 0 | exit 0 (1.1 s wall) | PASS |
| S1.2 | samples in channel 1 per day | 1440, 1440, 1440 | 1440, 1440, 1440 | PASS |
| S1.3 | sequence numbers start at 1, +1 per sample, no duplicates | 1..4335 contiguous | 1..4335, 4335 unique of 4335 | PASS |
| S1.4 | field formats and status grammar (contract 3.1, 4) on every entry | 0 violations of 4335 | 0 violations | PASS |
| S1.5 | every bulk update accepted | all HTTP 202 | 289 requests, status codes [202] | PASS |
| S1.6 | upload interval | 15 min 00 s between uploads | min 15 min 00 s, max 15 min 00 s over 288 intervals | PASS |
| S1.7 | uploads start at second 20 of the minute (contract 7.3) | [20] | [20] | PASS |
| S1.8 | time from end of a sample's minute to its arrival on the server | <= 16 min 00 s | max 14 min 20 s, mean 7 min 20 s (n=4335) | PASS |
| S1.9 | entries per bulk update in normal operation | 15 (one upload interval) | [15] | PASS |
| S1.10 | simulated samples are marked s=1 (contract 7.5) | 4335 of 4335 | 4335 of 4335 | PASS |
| S1.11n | nightly job runs to the end (octave) | exit 0 | exit 0 in 0.8 s | PASS |
| S1.11a | channel 2 equals the Python reference computed from channel 1 (rule 6.5) on 3 complete day(s) | 3 of 3 agree | 3 of 3 agree | PASS |
| S1.11v | channel-2 status carries v=1.5 and field8 = N unique | v=1.5; N equal | v=1.5; N equal | PASS |
| S1.11d | dashboard 'MATLAB check' column on the 3 complete day(s) | all 'agrees' | 3 of 3 agree | PASS |
| S1.11t | dashboard daily table values equal the reference (3 s.f. energies, 1 dp others, N exact) | 0 differences | 0 differences | PASS |

## S2 - 6-hour Wi-Fi outage, then back-fill: PASS

Proves: a 6 h Wi-Fi outage loses nothing; fault 9 is raised; back-fill is quick.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S2.1 | device runs through the outage | exit 0 | exit 0 | PASS |
| S2.2 | requests reaching the server during the outage 10:00-16:00 | 0 | 0 | PASS |
| S2.3 | minutes missing in channel 1 after back-fill (00:00 day 1 to 00:14 day 2) | 0 of 1455 | 0 of 1455 | PASS |
| S2.4 | back-fill of the 360 outage samples finished after Wi-Fi returned | <= 16 min | 360 samples, last arrived 0 min 20 s after 16:00 | PASS |
| S2.5 | day completeness and duplicates | 100.0 %, 0 duplicates | 100.00 %, 0 duplicates | PASS |
| S2.6 | fault 9 (upload failing > 60 min) visible in the status of samples logged late in the outage | >= 1 sample with e=9, none outside 10:45-16:16 | 315 samples, first at 10:45 | PASS |
| S2.7 | server answers during back-fill | only HTTP 202 | [202] | PASS |
| S2.8n | nightly job runs to the end (octave) | exit 0 | exit 0 in 0.3 s | PASS |
| S2.8a | channel 2 equals the Python reference computed from channel 1 (rule 6.5) on 1 complete day(s) | 1 of 1 agree | 1 of 1 agree | PASS |
| S2.8v | channel-2 status carries v=1.5 and field8 = N unique | v=1.5; N equal | v=1.5; N equal | PASS |
| S2.8d | dashboard 'MATLAB check' column on the 1 complete day(s) | all 'agrees' | 1 of 1 agree | PASS |
| S2.8t | dashboard daily table values equal the reference (3 s.f. energies, 1 dp others, N exact) | 0 differences | 0 differences | PASS |

## S3 - 7.5-day outage, back-fill from the 8-day buffer, nightly job corrects late days: PASS

Proves: a 7.5-day outage fits the 8-day buffer; the nightly job repairs the late days it still rewrites.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S3.1 | device runs 8 days with a 7.5-day outage (06:00 day 1 to 18:00 day 8) | exit 0 x 8 | [0, 0, 0, 0, 0, 0, 0, 0] | PASS |
| S3.2 | minutes missing in channel 1 after back-fill | 0 of 11535 | 0 of 11535 | PASS |
| S3.3 | outage samples buffered and delivered | 10800 samples (7.5 days), no overflow (0 x fault 6) | 10800 unique samples, 0 x fault 6, back-fill finished 3 min 16 s after Wi-Fi returned | PASS |
| S3.4 | back-fill requests | <= 960 entries each, no HTTP 429 | sizes [960, 960, 960, 960, 960, 960, 960, 960, 960, 960, 960, 258, 12, 15], status codes [202] | PASS |
| S3.5 | before the next nightly run: days whose channel-2 row is out of date (shown as 'differs'/'not yet summarised') | 8 (every outage day) | 8: 11-20=differ, 11-21=differ, 11-22=differ, 11-23=differ, 11-24=differ, 11-25=differ, 11-26=differ, 11-27=missing | PASS |
| S3.6 | old v1.4 setting (AST_NDAYS = 7, octave): the 7 days inside its rewrite window | 7 of 7 agree | 7 of 7 agree (run took 1.9 s) | PASS |
| S3.7 | old v1.4 setting: the first outage day (8 days old) stays stale - the defect that made v1.5 widen the window to 9 days | differs (regression evidence) | differ tracked Wh: reference 4.25, MATLAB 0.00 | PASS |
| S3.7d | dashboard 'MATLAB check' after the old 7-day run matches the reference verdicts | ['differ', 'agree', 'agree', 'agree', 'agree', 'agree', 'agree', 'agree'] | ['differ', 'agree', 'agree', 'agree', 'agree', 'agree', 'agree', 'agree'] | PASS |
| S3.8 | production nightly job (NDAYS = 9, contract v1.5): all 8 complete days after the 7.5-day outage | 8 of 8 agree | 8 of 8 agree | PASS |
| S3.9 | dashboard after the 9-day run: MATLAB check column and table values on the 8 days | 8 x agree, 0 value differences | 8 x agree, 0 value differences | PASS |

## S4 - Crash in the middle of an upload, restart from the same state directory: PASS

Proves: a crash between 'request sent' and 'acknowledgement stored' loses nothing; duplicates are removed by seq.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S4.1 | exit codes: clean stop, simulated crash at 06:00:30, clean run after restart | 0, 3, 0 | 0, 3, 0 | PASS |
| S4.2 | the request before the crash reached the server but got no answer | 1 request stored without reply | 1 (entries stored: [15]); 360 entries on the server at the crash | PASS |
| S4.3 | minutes missing in channel 1 after the restart | none except the minute of the crash itself (06:00) | 1 of 1455: 06:00 | PASS |
| S4.4 | sequence numbers survive the crash: contiguous and never reused for another minute | 1..1454, 0 reused | 1..1454, 1454 distinct, 0 reused | PASS |
| S4.5 | the batch sent before the crash is sent again (duplicates in channel 1) | >= 1 duplicate entry | 15 duplicate entries | PASS |
| S4.6 | reference metrics after de-duplication (contract 6.1) | N = 1439 (every logged sample once), completeness >= 99.9 % | n = 1454 received, N = 1439, duplicates = 15, completeness 99.93 % | PASS |
| S4.7n | nightly job runs to the end (octave) | exit 0 | exit 0 in 0.3 s | PASS |
| S4.7a | channel 2 equals the Python reference computed from channel 1 (rule 6.5) on 1 complete day(s) | 1 of 1 agree | 1 of 1 agree | PASS |
| S4.7v | channel-2 status carries v=1.5 and field8 = N unique | v=1.5; N equal | v=1.5; N equal | PASS |
| S4.7d | dashboard 'MATLAB check' column on the 1 complete day(s) | all 'agrees' | 1 of 1 agree | PASS |
| S4.7t | dashboard daily table values equal the reference (3 s.f. energies, 1 dp others, N exact) | 0 differences | 0 differences | PASS |
| S4.8 | channel-2 status reports the dropped duplicates (nd=) | nd=15 | nd=15 | PASS |

## S5 - Lost responses -> duplicates in channel 1, metrics unaffected: PASS

Proves: lost HTTP responses create duplicates in channel 1 that do not change any metric.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S5.1 | device runs a day with 3 lost responses (2 at 00:15, 1 at 12:00) | exit 0, 0 | 0, 0 | PASS |
| S5.2 | requests processed by the server without a reply | 3 | 3 | PASS |
| S5.3 | duplicates in channel 1 (same seq stored twice) | >= 1 | 46 duplicate entries, 1501 entries for 1455 sequence numbers | PASS |
| S5.4 | minutes missing in channel 1 | 0 of 1455 | 0 of 1455 | PASS |
| S5.5 | metrics unaffected by duplicates: reference energies equal a first-copy-per-seq sum | E_trk/E_fix/E_use equal, N = 1440 | E = 4.2471/3.6054/2.9339 Wh (sum over all received copies would be 4.4943/3.8525/3.0272), N = 1440, n = 1486 | PASS |
| S5.6n | nightly job runs to the end (octave) | exit 0 | exit 0 in 0.4 s | PASS |
| S5.6a | channel 2 equals the Python reference computed from channel 1 (rule 6.5) on 1 complete day(s) | 1 of 1 agree | 1 of 1 agree | PASS |
| S5.6v | channel-2 status carries v=1.5 and field8 = N unique | v=1.5; N equal | v=1.5; N equal | PASS |
| S5.6d | dashboard 'MATLAB check' column on the 1 complete day(s) | all 'agrees' | 1 of 1 agree | PASS |
| S5.6t | dashboard daily table values equal the reference (3 s.f. energies, 1 dp others, N exact) | 0 differences | 0 differences | PASS |
| S5.7 | channel-2 row: N unique and duplicates dropped | field8 = 1440, nd=46 | field8 = 1440, nd=46 | PASS |
| S5.8 | dashboard tells the user about the duplicates (data-quality note for the day) | note mentioning 46 duplicates | ⚠ 46 duplicate samples (repeated sequence number) received; duplicates do not raise completeness. | PASS |

## S5b - Lost responses with the server rejecting existing timestamps (assumed real behaviour): PASS

Proves: same as S5 when the server drops entries whose timestamp already exists (assumed real ThingSpeak behaviour).

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S5b.1 | device runs a day with 3 lost responses (2 at 00:15, 1 at 12:00) | exit 0, 0 | 0, 0 | PASS |
| S5b.2 | requests processed by the server without a reply | 3 | 3 | PASS |
| S5b.3 | server policy 'reject existing timestamps': duplicates in channel 1 | 0 | 0 duplicate entries (46 re-sent entries dropped by the server) | PASS |
| S5b.4 | minutes missing in channel 1 | 0 of 1455 | 0 of 1455 | PASS |
| S5b.5 | metrics unaffected by duplicates: reference energies equal a first-copy-per-seq sum | E_trk/E_fix/E_use equal, N = 1440 | E = 4.2471/3.6054/2.9339 Wh (sum over all received copies would be 4.2471/3.6054/2.9339), N = 1440, n = 1440 | PASS |
| S5b.6n | nightly job runs to the end (octave) | exit 0 | exit 0 in 0.4 s | PASS |
| S5b.6a | channel 2 equals the Python reference computed from channel 1 (rule 6.5) on 1 complete day(s) | 1 of 1 agree | 1 of 1 agree | PASS |
| S5b.6v | channel-2 status carries v=1.5 and field8 = N unique | v=1.5; N equal | v=1.5; N equal | PASS |
| S5b.6d | dashboard 'MATLAB check' column on the 1 complete day(s) | all 'agrees' | 1 of 1 agree | PASS |
| S5b.6t | dashboard daily table values equal the reference (3 s.f. energies, 1 dp others, N exact) | 0 differences | 0 differences | PASS |
| S5b.7 | channel-2 row: N unique and duplicates dropped | field8 = 1440, nd=0 | field8 = 1440, nd=0 | PASS |

## S6 - HTTP 500 burst -> backoff 1, 2, 4, 8, 15 min: PASS

Proves: server errors: retry waits 1, 2, 4, 8, 15 min, fault 9 after 60 min, nothing lost.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S6.1 | device runs through 8 consecutive HTTP 500 answers | exit 0, 0 | 0, 0 | PASS |
| S6.2 | waits between attempts after each failure | 1, 2, 4, 8, 15, 15, 15, 15 min | 1, 2, 4, 8, 15, 15, 15, 15 min | PASS |
| S6.3 | first failure at the regular slot, recovery with HTTP 202 | 03:00:20, then 202 | 03:00:20, then 202 | PASS |
| S6.4 | no sample lost or duplicated by the failed uploads | 0 missing, 0 duplicates | 0 missing, 0 duplicates | PASS |
| S6.5 | fault 9 appears only once no upload has succeeded for 60 min, and clears after recovery | >= 1 sample with e=9, all between 03:45 and 04:15 | 25 samples with e=9, 03:50 to 04:14 | PASS |
| S6.6 | the backlog after recovery is sent in one request | 1 request with 75-91 entries | 90 entries | PASS |

## S7 - Large backlog (3 days offline): request spacing and batch size: PASS

Proves: a large backlog is sent in batches of <= 960 at >= 16 s spacing, so ThingSpeak never answers 429.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S7.1 | device runs 3 days offline, then back-fills | exit 0 | exit 0 | PASS |
| S7.2 | HTTP 429 (too many requests) answers from the server | 0 | 0 of 15 requests | PASS |
| S7.3 | entries per back-fill request | <= 960, backlog of 4320+ sent in >= 5 requests | max 960; first requests [960, 960, 960, 960, 496, 14, 15] | PASS |
| S7.4 | shortest time between two requests to channel 1 | >= 16 s | 16 s (over 15 requests) | PASS |
| S7.5 | channel 1 after back-fill | 0 missing minutes, 0 duplicates | 0 missing of 4485, 0 duplicates | PASS |
| S7.6 | back-fill time after Wi-Fi returned | first request <= 15 min, backlog cleared <= 20 min | first request after 0 min 20 s, backlog cleared after 1 min 24 s (4 full batches) | PASS |

## S8 - Offline longer than the 8-day buffer: loss is flagged (fault 6) and counted: PASS

Proves: an outage longer than the buffer loses the oldest data visibly: fault 6, sequence gap, completeness.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S8.1 | device runs 9 days offline (buffer holds 8), then reconnects | exit 0 | exit 0 | PASS |
| S8.2 | minutes lost (not in channel 1) | 1440 to 1500 in one block (9 days offline + up to 1 h, minus the 11520-record buffer) | 1455 minutes in 1 block(s): 2026-11-20T00:45:00Z to 2026-11-21T00:59:00Z | PASS |
| S8.3 | loss counted by sequence numbers (never reused, so the gap equals the loss) | seq gap = 1455 | seq gap = 1455 (seq 1..13125, 11670 received) | PASS |
| S8.4 | fault 6 (buffer overflow, data lost) visible in channel-1 status | >= 1 sample with e=6 | 1455 samples with e=6, 2026-11-28T00:46:00Z to 2026-11-29T01:00:00Z | PASS |
| S8.5 | the device's own count of overwritten records (device log) | 1455 (= minutes missing on the server) | 1455 | PASS |
| S8.6 | reference completeness shows the loss on the affected days | missing samples on 2026-11-20, 2026-11-21 add up to 1455 | 1455 (11-20: 3.1 %, 11-21: 95.8 %) | PASS |
| S8.7 | dashboard daily table: completeness of the affected day(s) | < 99 % and equal to the reference | 11-20: 3.1 %, 11-21: 95.8 % | PASS |
| S8.8 | dashboard day view of 2026-11-20: completeness card states received/expected samples | 45 of 1440 | 45 of 1440 expected samples · 0 gaps | PASS |
| S8.9 | dashboard day view of 2026-11-28: fault 6 named in the data-quality notes | note with fault code 6 (1394 samples that day) | ⚠ 1440 samples report a fault: code 6 (Local buffer overflow (data lost)), 9 (Upload failing for over 60 minut | PASS |

## S9 - RTC lost time at boot, no network for 3 h: PASS

Proves: after the RTC lost time nothing is logged with a wrong timestamp; fault 4 is reported.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S9.1 | device boots with the RTC oscillator-stop flag and no Wi-Fi until 03:00 | exit 0 | exit 0 | PASS |
| S9.2 | entries with a timestamp from before the clock was valid (or outside the test period) | 0 | 0 before 02:59, 0 after the end; first entry 2026-11-20T03:00:00Z | PASS |
| S9.3 | logging resumes once NTP has set the clock | first entry between 02:59 and 03:02 | 03:00 | PASS |
| S9.4 | after the clock is valid: minutes missing up to 06:29 | 0 | 0 of 210 | PASS |
| S9.5 | fault 4 (clock lost) visible in channel-1 status | >= 1 sample with e=4 | 1 sample(s) with e=4, first at 03:00 | PASS |
| S9.6 | sequence numbers of the logged samples | contiguous, no reuse | 1..210, 210 distinct of 210 | PASS |
| S9.7 | dashboard day view: fault 4 named in the data-quality notes | note with fault code 4 | ⚠ 1 sample reports a fault: code 4 (Clock (RTC) missing or lost time). | PASS |

## S10 - Dashboard vs nightly job on every complete day (5 different days), two phones, CSV export: PASS

Proves: dashboard, nightly job and Python reference agree on every complete day; two phones; CSV export; read-only keys.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S10.1 | 5 days: clear r=0 / cloudy r=12 panel A with lost responses / overcast r=13 panel B with a 3 h outage / dark (no power) / calibration r=900 | exit 0 x 5 | [0, 0, 0, 0, 0] | PASS |
| S10.2n | nightly job runs to the end (octave) | exit 0 | exit 0 in 1.3 s | PASS |
| S10.2a | channel 2 equals the Python reference computed from channel 1 (rule 6.5) on 5 complete day(s) | 5 of 5 agree | 5 of 5 agree | PASS |
| S10.2v | channel-2 status carries v=1.5 and field8 = N unique | v=1.5; N equal | v=1.5; N equal | PASS |
| S10.2d | dashboard 'MATLAB check' column on the 5 complete day(s) | all 'agrees' | 5 of 5 agree | PASS |
| S10.2t | dashboard daily table values equal the reference (3 s.f. energies, 1 dp others, N exact) | 0 differences | 0 differences | PASS |
| S10.3 | edge cases present in the data set | duplicates > 0; a day with E_fix = 0 (gains undefined); 5 x 1440 unique samples | duplicates 31; dark day E_fix = 0.000 Wh, gross = None; N = [1440, 1440, 1440, 1440, 1440] | PASS |
| S10.4 | channel-2 row of the dark day omits both gains (contract 3.2) | field4, field5 empty | field4 = None, field5 = None | PASS |
| S10.5 | KPI cards of each of the 5 days equal the reference (7 figures per day) | 0 differences in 35 figures | 0 differences | PASS |
| S10.6 | two emulated phones (iPhone 13 / Pixel 7 profiles) show the same figures | 8 KPI values and the daily table identical | 8 KPI values compared, 0 differ; 5 table rows compared, identical | PASS |
| S10.7 | raw-samples CSV export (download) equals channel 1: header as agreed in PLAN.md, one row per received entry, every value equal | 7246 rows, 0 differences | 7246 rows, 0 differences | PASS |
| S10.8 | CSV export equals the device's own log: minute, 7 values and status of every logged sample (7216 log lines) | 0 rows differ; only samples logged after the last upload are absent (<= 16) | 0 of 7246 rows differ; 1 logged samples not yet uploaded | PASS |
| S10.9 | dashboard uses READ access only | 0 requests containing a write key, 0 non-GET requests to the API | 48 API requests: 0 with a write key, 0 non-GET | PASS |
| S10.10 | JavaScript errors on the 8 pages opened | 0 | 0 | PASS |

## S11 - Completeness on normal days (including a short Wi-Fi drop): PASS

Proves: completeness >= 99 % on normal days.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S11.1 | 3 days (clear, cloudy, overcast); day 2 has a 45-minute Wi-Fi drop | exit 0 x 3 | [0, 0, 0] | PASS |
| S11.2n | nightly job runs to the end (octave) | exit 0 | exit 0 in 0.9 s | PASS |
| S11.2a | channel 2 equals the Python reference computed from channel 1 (rule 6.5) on 3 complete day(s) | 3 of 3 agree | 3 of 3 agree | PASS |
| S11.2v | channel-2 status carries v=1.5 and field8 = N unique | v=1.5; N equal | v=1.5; N equal | PASS |
| S11.2d | dashboard 'MATLAB check' column on the 3 complete day(s) | all 'agrees' | 3 of 3 agree | PASS |
| S11.2t | dashboard daily table values equal the reference (3 s.f. energies, 1 dp others, N exact) | 0 differences | 0 differences | PASS |
| S11.3 | reference completeness per day | >= 99 % | 100.00 %, 100.00 %, 100.00 % | PASS |
| S11.4 | channel-2 field7 (completeness written by the nightly job) | >= 99 % | 100.0 %, 100.0 %, 100.0 % | PASS |
| S11.5 | dashboard 'Complete %' column | >= 99 % | 100.0 %, 100.0 %, 100.0 % | PASS |

## S12 - Sample-to-screen latency (virtual time): PASS

Proves: a sample is on the screen <= 17 min after its minute ends.

| Check | What | Expected | Measured | Result |
|---|---|---|---|---|
| S12.1 | device runs from 00:00 to 12:16 | exit 0 | exit 0 | PASS |
| S12.2 | end of minute -> on the server (F8 target 16 min) | <= 16 min 00 s | max 14 min 20 s, mean 7 min 20 s (n=735) | PASS |
| S12.3 | dashboard opened one refresh period (60 s) after the upload shows every uploaded sample of today | Samples = 735, status 'fresh' | Samples = 735, status 'fresh', page ready in 0.4 s | PASS |
| S12.4 | dashboard refreshes by itself (page left open 20 s with refresh=15 s) | >= 1 automatic reload of the data | 4 data requests after the first load | PASS |
| S12.5 | worst-case sample-to-screen = server latency + refresh period + page update | <= 17 min 00 s | 15 min 20 s = 14 min 20 s + 60 s + 0.4 s | PASS |
| S12.6 | newest uploaded sample (on screen, see check 3) when the dashboard is opened 60 s after an upload | sample of 12:14, age <= 3 min | sample of 12:14, age 1 min 20 s | PASS |
