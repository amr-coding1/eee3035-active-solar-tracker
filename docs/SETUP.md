# AST data link - what is set up, and the few steps left

EEE3035 Group 8, Active Solar Tracker. Owner: AM (data manager). Contract: `docs/CONTRACT.md` v1.5.
State on 13 Nov 2026.

## Already set up and running

| Item | Where | State |
|---|---|---|
| Project hub (share this link) | https://amr-coding1.github.io/eee3035-active-solar-tracker/ | public |
| Phone dashboard | https://amr-coding1.github.io/eee3035-active-solar-tracker/dashboard/ | public, reads both channels, embeds the five MATLAB charts |
| Code and documents | https://github.com/amr-coding1/eee3035-active-solar-tracker | public repository |
| Channel 1 "AST raw - Group 8 Active Solar Tracker" | ThingSpeak channel **3578277** | public-read; holds 9,465 **simulated** test rows (s=1) for 7-13 Nov |
| Channel 2 "AST results - Group 8 Active Solar Tracker" | ThingSpeak channel **3578278** | public-read; filled by the nightly job once its key is in (step 1) |
| Nightly job "AST nightly summary (contract v1.5)" | ThingSpeak Apps > MATLAB Analysis, ID **430132** | identical to `analysis/matlab/ast_nightly.m`; `DRY_RUN = 'auto'`: a dry run until the key is pasted, then it writes |
| Schedule "AST nightly summary 00:20 UTC" | Apps > TimeControl, ID **99365** | daily 00:20 UTC, email to AM on failure |
| MATLAB charts (public URLs) | Apps > MATLAB Visualizations | 689480 power, 689481 energy, 689483 angles, 689484 status, 689485 daily net gain |
| Firmware | `firmware/` | compiles for the Nano ESP32 with 0 warnings; tested on the PC (step 3 needs the board) |

Checks already run on the real ThingSpeak: the nightly job on ThingSpeak's own MATLAB read 8,585
entries in 3 requests (1.3 s) and its 9 rows were identical to the Python reference; all five
charts render; the dashboard reads both channels from a phone-sized browser.

## 1. Switch the nightly job on (1 minute, AM, needs AM's ThingSpeak login)

API keys are typed by a person, never by the AI assistant, so this one step is AM's.

1. ThingSpeak > Channels > **AST results** (3578278) > **API Keys**: copy the **Write API Key**.
   (Optional: press "Generate New Write API Key" first; the keys were visible on screen during set-up.)
2. Apps > MATLAB Analysis > **AST nightly summary (contract v1.5)** > Edit.
3. Line 28: paste the key between the quotes of `CH2_WRITE_KEY = '...';`. Nothing else to change.
4. **Save and Run**. The output must end with `Channel 3578278: wrote 9 row(s) in one bulk update.`
5. Check: chart 689485 shows bars labelled "sim"; the dashboard's "MATLAB check" column says
   "agrees" for the complete days. From now on it runs by itself every night at 00:20 UTC.

Never put the write key in the repository, a chat or a screenshot. If it leaks: generate a new
one and paste it at line 28 again.

## 2. On every phone (30 seconds, everyone)

Open https://amr-coding1.github.io/eee3035-active-solar-tracker/dashboard/ and use Share >
**Add to Home Screen**. `...?demo=1` shows built-in simulated data without the network (a fallback
for the presentation); `...?day=2026-11-12` opens a given UTC day.

## 3. When the hardware arrives (bench, AM with the control and power leads)

Full guide: `firmware/README.md` ("Wiring", "Build and flash", "Wi-Fi and keys",
"Calibrating the INA226"). In short:

```
arduino-cli core install esp32:esp32@3.3.12 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
firmware/build.sh upload /dev/ttyACM0          # Windows: COM5
```

Serial monitor at 115200 baud, line ending "Newline":

```
wifi "Phone hotspot name" hotspot-password     2.4 GHz hotspot; no eduroam on the device
keys 3578277 <CH1 WRITE KEY>                   typed by AM (ThingSpeak > AST raw > API Keys)
time                                           must say the clock is valid (RTC or NTP)
run 1                                          run number of the test (900-999 = calibration)
panel A
upload                                         send now; then check channel 3578277
```

Then the bench procedures B1-B4 in `docs/VV_RESULTS.md` (real Wi-Fi, TLS, RTC, sensors), the
check of how ThingSpeak answers a repeated timestamp, and calibrate the three INA226 against
the bench DMM.

Optional before the first real test: export the simulated data (dashboard > Export), then
Channel 3578277 > Channel Settings > Clear Channel (and the same for 3578278), so the report
figures hold real data only. Without this, simulated days stay labelled "simulated" and drop out
of the nightly summary after 9 days.

## 4. Every week until the demo (25 Jan 2027)

- Dashboard > Export > raw and energy-budget CSV > save to the group OneDrive (DMP, `docs/DMP_v2.md`).
- Glance at the "MATLAB check" column: "differs" on a day means late data arrived after that
  night's run; the next nightly run (9-day window) corrects it.
- An email from MathWorks about a failed TimeControl run means the nightly job stopped: open
  430132 and press Save and Run to see the error.

## Limits to keep in mind (free ThingSpeak licence)

| Limit | Use |
|---|---|
| 3 million messages a year (~8,200 a day) | ~1,450 a day: 1,440 samples + 9 nightly rows |
| one write per 15 s per channel | device: one bulk upload every 15 min, back-log batches 16 s apart |
| 960 entries per bulk upload | device sends at most 960 |
| 8,000 entries per read | dashboard and MATLAB read in windows and split full ones |
| MATLAB: 20 s per run, TimeControl at most every 5 min | nightly job ~1-4 s, once a day |
