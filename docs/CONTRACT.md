# AST data-link contract — v1.5 (13 Nov 2026)

EEE3035 Group 8, Active Solar Tracker. Owner: AM (App/Website, data manager).
This file is the single source of truth shared by the firmware, the ThingSpeak
channels, the MATLAB jobs, the phone dashboard, the mock server and the tests.
Every component quotes the section it implements.

Covers client requirements (ii) "timed data transmission to a known free secure
location" and (iii) "timed data display and analysis on a portable device"
(Week 1 MPS 3 and 4), and requirements F7, F8, F9 (supports F10, F11).

## 1. Architecture

```
 sensors (3x INA226, angle, sun arc, temp)            ThingSpeak (free licence)
        |  1 Hz                                    +---------------------------+
 [ESP32 sampler task] -- 60 s means --> LittleFS   | ch1 "AST raw"   (1/min)   |
        ring buffer (>= 8 days) --> [uploader task]-HTTPS bulk_update every 15 min-> |
                                                   | ch2 "AST results" (daily) |
                                                   +-----^------------+--------+
                         MATLAB Analysis (TimeControl, nightly 00:20 UTC) |
                         reads ch1, writes ch2 (last 9 complete days)     |
                         MATLAB Visualizations (public PNG, iframes) -----+
 phone dashboard (static page, READ keys only) <-- feeds.json ------------+
```

## 2. Versioning

`v=1.5` is written in every channel-2 status. Firmware prints `contract 1.5`
on boot. A change to any field meaning bumps the minor version and is logged
in section 12.

## 3. Channels

Free licence limits that shape this contract (MathWorks, Nov 2026): 4
channels, 3 million messages/year (~8,200/day), one write request per channel
every 15 s, at most 960 entries per bulk update, at most 8,000 entries per
read, MATLAB code must finish within 20 s, TimeControl no faster than every
5 min. Budget: ch1 1,440 messages/day + ch2 9/day = ~530k/year (18 % of quota).

### 3.1 Channel 1 "AST raw" — one entry per logged minute (device writes)

| Field | Name | Unit | Format | Meaning |
|---|---|---|---|---|
| field1 | P_trk | mW | 1 dp | mean output power of the TRACKED panel over the minute |
| field2 | P_fix | mW | 1 dp | mean output power of the FIXED panel over the minute |
| field3 | P_use | mW | 1 dp | mean power drawn by the tracker itself (MCU, sensors, driver, motor) |
| field4 | θ_panel | ° | 1 dp | panel angle about the tracking axis at the end of the minute; 0 = level, east negative, west positive |
| field5 | θ_sun | ° | 1 dp | sun angle from the sensor arc on the same axis; OMITTED when unknown (dark, cloud, fault) |
| field6 | T_panel | °C | 1 dp | panel temperature; OMITTED when no sensor fitted (SHOULD requirement F6) |
| field7 | V_bat | V | 3 dp | tracker battery voltage |
| field8 | seq | – | integer | sample sequence number, starts at 1, +1 per logged sample, persisted across reboots, never reused |
| status | – | – | text | section 4 |
| created_at | – | UTC | `YYYY-MM-DD HH:MM:00 +0000` | start of the one-minute interval the entry covers |

Rules: one entry per UTC minute; `created_at` seconds are always `00`. A value
that is unknown is omitted from the JSON object (ThingSpeak stores null), never
written as 0. Powers are never negative in a valid sample (a negative mean is
clamped to 0 on the device and flagged `e=0` unchanged; the raw negative count
is a dashboard quality note).

### 3.2 Channel 2 "AST results" — one entry per summarised UTC day (MATLAB writes)

| Field | Name | Unit | Format |
|---|---|---|---|
| field1 | E_trk | Wh | 6 significant figures |
| field2 | E_fix | Wh | 6 s.f. |
| field3 | E_use | Wh | 6 s.f. |
| field4 | gross gain | % | 4 dp, omitted when E_fix <= 0 |
| field5 | net gain | % | 4 dp, omitted when E_fix <= 0 |
| field6 | mean abs pointing error | ° | 4 dp, omitted when no pointing samples |
| field7 | completeness | % | 4 dp |
| field8 | N | – | integer: number of UNIQUE samples (section 6.1) |
| status | – | – | `period=day;date=YYYY-MM-DD;v=1.5;ns=<simulated samples>;nd=<duplicates dropped>` |
| created_at | – | UTC | time of the nightly run (+1 s per row so timestamps stay unique) |

The nightly job rewrites the last 9 COMPLETE UTC days every night in one bulk
update (the device buffers 8 days, plus one day of margin), so a day that was
back-filled late, even after the longest outage the buffer survives, is corrected. The
reader keeps, for each `date`, the row with the newest `created_at`.

## 4. Channel-1 status grammar

```
status := STATE ( ";" key "=" value )*
STATE  := TRK | MOV | HLD | IDL | STW | FLT
keys   := r (run id, integer >= 0) | p (panel letter on the tracker, A or B)
        | e (fault code, integer 0-9) | s (1 = simulated sample; absent = real)
example : TRK;r=12;p=A;e=0          real sample
          MOV;r=12;p=A;e=0;s=1      simulated sample
```

| STATE | Words on the dashboard | Counts for pointing error? |
|---|---|---|
| TRK | Tracking | yes |
| MOV | Moving (motor ran this minute) | yes |
| HLD | Holding (sensor uncertain) | yes |
| IDL | Idle (dark) | no |
| STW | Stowed | no |
| FLT | Fault | no |

Fault codes `e`: 0 none; 1 tracked-panel power sensor missing; 2 fixed-panel
power sensor missing; 3 consumption power sensor missing; 4 clock (RTC)
missing or lost time; 5 sun-sensor arc fault; 6 local buffer overflow (data
lost); 7 limit switch hit unexpectedly; 8 motor stall or timeout; 9 upload
failing for over 60 minutes. One code per sample: the control system's code
wins when non-zero, otherwise the data link's own (4, 6, 9 in that priority).

Unknown keys are ignored by every reader (forward compatibility).

## 5. Runs and panels

- `r=0`: no test running (free running / demo).
- `r=1..899`: tracking tests. Panel A or B is on the tracker (`p`).
- `r=900..999`: calibration runs: BOTH panels fixed side by side, used to
  measure panel mismatch. Mismatch = 100 (E_trk - E_fix) / E_fix; it is a
  panel difference, never reported as a gain.
- Panel-swap check: tracking-test totals grouped by `p`; similar net gains for
  A and B mean the gain comes from tracking, not from one panel being better.

## 6. Metrics (dashboard, MATLAB and the Python reference must agree)

### 6.1 De-duplication (new in v1.5)
Entries with the same `seq` AND the same `created_at` are duplicates (a batch
re-sent after a lost response). Every metric uses only the first received copy
(lowest `entry_id`, then the order received). The same `seq` at a different
minute is a different sample: the device's `seq` restarts if its flash is
formatted, and the channel may still hold older data. Entries with no `seq`
are all kept.
`n` = entries received, `N` = `nUnique` = entries after de-duplication,
`duplicates` = n - N.

### 6.2 Energy and gain (over a set S of unique entries; Δt = 60 s)
```
E_x   = sum_{i in S} P_x,i * 60 / 3600 / 1000          [Wh]   (null P counts as 0)
gross = 100 * (E_trk - E_fix) / E_fix                    [%]   (null if E_fix <= 0)
net   = 100 * (E_trk - E_fix - E_use) / E_fix            [%]   (null if E_fix <= 0)
```

### 6.3 Pointing error
`err = θ_sun - θ_panel` for entries in TRK, MOV or HLD with both angles present.
MAE = mean |err| (null when none). Histogram: 62 bins: bin 0 = err < -30,
bins 1..60 = [-30 + k - 1, -30 + k), bin 61 = err >= 30.

### 6.4 Completeness
`complete = 100 * N / expected`. expected = 1440 for a past UTC day; for the
current day the whole minutes from 00:00 UTC to the newest received entry of
that day inclusive; for a run or arbitrary period, first-to-last entry
inclusive. Gaps: consecutive unique timestamps more than 60 s apart; missing
minutes = round(Δ/60 s) - 1.

### 6.5 Agreement rule (dashboard "MATLAB check" column)
Dashboard (from ch1) and MATLAB (ch2 row) agree when E_trk, E_fix, E_use match
at 3 significant figures, gross, net, MAE and completeness match at 1 dp (both
null also matches), and N matches exactly.

## 7. Device behaviour (firmware)

### 7.1 Sampling
- Board: Arduino Nano ESP32 (ESP32-S3), core `esp32:esp32` 3.3.x, FQBN
  `esp32:esp32:nano_nora`, partition scheme with SPIFFS/LittleFS space.
  Inputs <= 3.3 V. I2C on A4/A5, modules on 3V3. Wi-Fi 2.4 GHz phone hotspot
  (no eduroam credentials on the device).
- 1 Hz reads; each logged sample is the mean over the UTC minute
  [t, t+60 s) for powers, instantaneous end-of-minute values for angles and
  state; `MOV` if the motor ran at any time in the minute.
- Time: DS3231 RTC in UTC, disciplined by NTP when online (set RTC when
  |RTC - NTP| > 2 s). If the RTC reports lost time (oscillator-stop flag) and
  NTP has not succeeded since boot, samples are NOT logged and fault 4 is
  raised; logging resumes as soon as time is valid. The first sample after time
  becomes valid carries `e=4` once (with no RTC fitted, every sample does).
- Timestamps are strictly increasing: the newest logged minute is persisted
  with the ring metadata and a record for a minute not later than it is
  refused (logged as `skip`), also after a clock step back or a reboot inside
  the same minute. If the clock was ever set into the future, `buffer clear yes`
  removes the guard (seq is kept).

### 7.2 Local buffer
- LittleFS ring buffer of fixed-size records with a CRC each, capacity at
  least 11,520 records (8 days at 1/min). Head/tail/ack cursors persisted
  power-loss-safely (two alternating metadata copies with sequence + CRC).
- When full, the oldest UNSENT record is overwritten and fault 6 raised on the
  next sample (data loss is visible, never silent).

### 7.3 Upload (write API)
- Every `upload_min` minutes (default 15, allowed 5-60) at second 20 of the
  minute: POST `https://api.thingspeak.com/channels/<ch1>/bulk_update.json`,
  `Content-Type: application/json`, body
  `{"write_api_key":"<KEY>","updates":[{created_at, field1..8, status}, ...]}`
  with up to 960 unacknowledged records, oldest first.
- Success = HTTP 202 (or 200) with a body containing `"success":true`
  -> the ack cursor moves past the batch and is persisted.
- Backlog: further batches follow at >= 16 s spacing until the buffer is empty.
- Failure (connect/TLS error, timeout, HTTP 429 or 5xx): retry the same batch
  with backoff 1, 2, 4, 8, 15 min (cap 15); records stay in the buffer.
- HTTP 401/403/404 (wrong key or channel): records are kept and retried at the
  15 min cap; nothing is dropped.
- Poison batch (HTTP 400, or 200/202 without `"success":true`): the batch is
  halved every 16 s until a single record is rejected; only that record is
  dropped, logged with its seq and the reason. At most 3 records are dropped
  without a successful upload in between; after that the data is kept and
  retried every 15 min (a server that rejects everything must not empty the
  buffer one record at a time). The JSON builder also leaves out any record
  whose minute is not later than the previous one in the batch (`drop`).
- Fault 9 when records are pending, uploads are failing and it is 60 min or
  more since the last success (counted from boot when there has been none
  since boot).
- Lost response (request sent, no response): the batch is "in doubt" and is
  re-sent; readers de-duplicate by `seq` (6.1). If a channel-1 READ key is
  configured the device MAY first GET `/channels/<ch1>/fields/8/last.json`
  (>= 30 s after the doubtful request) and skip records with seq <= last.
- TLS: `WiFiClientSecure` verifying the server certificate against the ESP32
  core's built-in CA bundle (`useBuiltinCACertBundle()`); `setInsecure()` is
  never used. A single root can be pinned instead by defining `TS_ROOT_CA_PEM`
  in `secrets.h` (copy it from a trusted network, not through a proxy).
- Energy: Wi-Fi is off between uploads and switched on 8 s before the upload
  slot; CPU at 80 MHz (160 MHz while the radio is on); `radio_on_s` is logged.
- Secrets (`WIFI_SSID`, `WIFI_PASS`, `TS_CH1`, `TS_WRITE_KEY`, optional
  `TS_READ_KEY`) live in `secrets.h` (git-ignored) or NVS via the CLI, never in
  the repository.

### 7.4 Tasks and watchdog
Sampler and uploader are separate FreeRTOS tasks sharing the buffer behind a
mutex; the uploader never blocks sampling. Task watchdog 180 s; a hung upload
is aborted by a 30 s HTTP timeout.

### 7.5 Simulator
Firmware simulator (sun model + clouds + tracker) is OFF by default; every
simulated sample carries `s=1`. Never mixed silently with real data.

### 7.6 Serial CLI (115200 baud)
`help`, `status`, `time [<ISO>]`, `buffer [dump [n] | clear yes]`, `upload`
(now), `upload every <5-60>`, `run <id>`, `panel A|B`, `sim on [<scenario>]|off`,
`cal` (two-point INA226 calibration against the bench DMM, stored in NVS),
`power`, `wifi <ssid> <pass>`, `keys <ch1> <write> [read]`, `reboot`.

### 7.7 Interface to the rest of the firmware
See `firmware/INTERFACE.md`: the control system calls
`ast::telemetry_set_angles()`, `ast::telemetry_set_state()`, etc. once per
second; the data link owns the INA226 reads (power system lead supplies the
shunt values) and everything after that.

## 8. Read API (dashboard and MATLAB)

GET `/channels/<id>/feeds.json?api_key=<READ>&start=<YYYY-MM-DD HH:NN:SS>&end=<...>&timezone=Etc/UTC&status=true&results=8000`
(spaces as `%20`). ThingSpeak returns at most 8,000 entries: the NEWEST in the
window, oldest first. Readers page in windows of at most 5 days and split a
window that comes back full. A read key that is wrong returns `-1`.

## 9. Security and data protection (DMP)
- WRITE keys stay secret: the channel-1 key only on the device (NVS or the
  git-ignored `secrets.h`), the channel-2 key only inside the ThingSpeak MATLAB
  Analysis. Regenerate a write key if it is ever shown on screen or shared.
- Both channels are public-read (group decision, 13 Nov 2026): they hold
  measurement data only, so the dashboard and MATLAB need no read key. If a
  channel is made private again, its READ key goes in the dashboard
  configuration and the MATLAB code (it exposes measurement data only).
- No personal data in any channel. No university credentials on the device.
- MATLAB Visualizations have public URLs so the dashboard can embed them.
- Data are exported weekly (CSV from the dashboard) to the group OneDrive.

## 10. Mock server (tests only)
`tools/mock_thingspeak.py` implements sections 3, 7.3 and 8 on
`http://127.0.0.1:<port>`: `bulk_update.json`, `feeds.json`, `feeds/last.json`,
`fields/<n>/last.json`, with write/read key checks, the 960 and 8,000 limits,
the 15 s spacing rule, and admin endpoints for fault injection and a virtual
clock (see the file header).

## 11. Acceptance targets (from the design-brief requirements)
- F7: >= 99 % of expected samples per day reach ch1; no loss of logged samples
  for a 7-day outage (8-day buffer) or a power cut (the minute being averaged
  when the power fails is lost); timestamps within 2 s of UTC.
- F8: uploads every 15 min; a sample is on ThingSpeak <= 16 min after its
  minute ends (when online); HTTPS only; write key never leaves the device.
- F9: two phones show the same figures; CSV export equals the device log;
  dashboard and MATLAB agree (6.5) on every complete day; sample-to-screen
  <= 17 min.

## 12. Change log
- v1.5 amendment (13 Nov 2026, wire format unchanged, still `v=1.5`): 7.3 TLS
  uses the core CA bundle (optional pinned root); poison-batch bisection with a
  3-record drop limit; strictly increasing timestamps; 401/403/404 keep data;
  radio-off energy policy; fault 4/9 details; extra CLI commands; section 9:
  both channels are public-read. After the independent review: 6.1 duplicates
  need the same seq AND the same created_at; uploads keep 16 s spacing during
  bisection and the batch size grows back after a rejection episode.
- v1.5 (13 Nov 2026): nightly rewrite window 7 -> 9 complete days (E2E S3 found that
  an 8-day back-fill left the oldest day stale); de-duplication by seq in every metric (6.1); ch2 field8
  is now N unique; ch2 status adds `ns`, `nd`, `v`; created_at format fixed to
  `+0000`; MATLAB charts embedded on the dashboard.
- v1.4 (13 Nov 2026, earlier session): `s=1` simulated marker; calibration
  runs 900-999; today's completeness rule.
