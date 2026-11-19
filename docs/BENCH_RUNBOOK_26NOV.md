# Bench runbook: first hardware lab, Thu 26 Nov 2026, 10:00–13:00

EEE3035 Group 8, Active Solar Tracker. For AM (data link, second on control) with Chimdi (power) and the control lead. Print this and the card. Lab time is GMT = UTC, so the serial log and the wall clock agree.
Sources: `docs/SETUP.md` §3, `docs/VV_RESULTS.md` (B1–B4), `firmware/README.md`, `firmware/INTERFACE.md`, `docs/CONTRACT.md` v1.5, `power_budget.md` §7–8 (M0–M12). The model figures are estimates that have not been measured: the lab replaces them.

## 1. Before the lab (by Wed 25 Nov)

**AM, on the Mac**
- [ ] `brew install arduino-cli` (tested with 1.6.0), then
      `arduino-cli core install esp32:esp32@3.3.12 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json`
- [ ] `git pull`, then compile with no board attached (expect 0 errors, about 40 % flash):
      `arduino-cli compile --fqbn esp32:esp32:nano_nora:PartitionScheme=spiffs --build-path /tmp/ast_build firmware/ast_datalink`
      Arduino IDE route: core "esp32 by Espressif" 3.3.12 (not "Arduino ESP32 Boards" 2.0.18, untried), board Arduino Nano ESP32, Tools > Partition Scheme > "With SPIFFS partition (advanced)".
- [ ] Write and compile a 15-line sleep test sketch for M4/M5 (`esp_sleep_enable_timer_wakeup(15 s)` then `esp_light_sleep_start()` in a loop; second build with `esp_deep_sleep_start()`). There is none in the repo.
- [ ] Phone hotspot: 2.4 GHz (iPhone: "Maximise Compatibility" on), plain ASCII name with no apostrophe, password 8–63 characters. Test that the laptop joins it.
- [ ] ThingSpeak login works on the laptop. Decide: export the 9,465 simulated rows (dashboard > Export), then clear channel 3578277 (SETUP §3, optional).
- [ ] Dashboard on the phone's home screen. Send the group the message that the data link is ready, with the three questions below.

**Bring**
- [ ] Laptop + charger, USB-C **data** cable (and a spare), phone + charger, second phone if possible
- [ ] DMM with leads (two if possible), croc clips, jumper wires, breadboard, a USB-A to USB-C cable that may be cut (or USB breakouts)
- [ ] Load resistors: about 250 Ω (20 mA at 5 V) and 22–27 Ω rated ≥ 2 W (200 mA at 5 V), or use a lab electronic load
- [ ] CR2032 for the DS3231 (check whether the module has a charging circuit before fitting it), protractor, stopwatch, this sheet, pen, tape for labels

**Chimdi (power) to confirm, in writing in the group chat**
- [ ] Shunt value on each INA226 breakout (default assumed 0.1 Ω = ±819 mA). A 5.4 W panel can exceed 819 mA: is a smaller shunt needed on #1/#2?
- [ ] INA226 #3 (0x44) sits in the **5 V feed after the power manager and before the Nano, driver and sensors**, IN+ to the power manager, IN− to the load. On the battery line it also sees charging current and P_use is not separable.
- [ ] The Nano is powered from the power manager's **5 V USB output into the Nano's USB-C port** (the documented 5 V path). **Not VIN** (Arduino: 6–21 V recommended, so 5 V is too low) and **not the VBUS pin** (it is an output, live only when USB powers the board; there is no 5 V input pin). 3.3 V into the 3V3 pin only in the way Arduino describes (solder jumper), and only if the group decides to.
- [ ] A way to break the 5 V wire of that USB feed for INA226 #3 and the DMM: a cut USB-A to USB-C cable or two USB breakout boards (data wires not needed).
- [ ] Every module (3× INA226, DS3231, sun sensor, DRV8833 logic) is on **3V3**. No Nano ESP32 pin is 5 V tolerant. The 12 Nov photodiode test was on a 5 V Uno, so its divider values must be redone for 3.3 V.
- [ ] Addresses: 0x40 tracked (A1 = GND, A0 = GND), 0x41 fixed (A0 = VS), 0x44 use (A1 = VS), DS3231 0x68. SDA = A4, SCL = A5, bus under 30 cm.
- [ ] Battery fitted and charged: the 24 h run needs about 3.6 Wh from it.

**Group decisions to settle before or at 10:00**
- [ ] Fixed reference panel at the **tracker's tilt**, not flat (model: flat makes November "gain" +146 % against +17 % at equal tilt; the difference is tilt, not tracking).
- [ ] Who leads control (unassigned; AM is second).
- [ ] `INTERFACE.md` says call telemetry at least once per second (silent after 5 s gives IDL). Light sleep with a 15 s wake breaks that. Agree: change the rule to match the wake period, with AM changing the firmware.
- [ ] Who takes the kit home for the 24 h run (it needs the hotspot nearby).

## 2. Secrets

- Wi-Fi name/password and the channel-1 **Write API key** are typed **only by AM**, into `firmware/ast_datalink/secrets.h` (git-ignored) or the serial console (`wifi`, `keys`; stored on the board). No assistant, no teammate, no chat, no Notion.
- [ ] Before any commit: `git status` must not list `secrets.h`.
- [ ] Clear the serial monitor after typing `keys` and before any photo or screenshot (`status` masks keys, the typed line does not).
- If a key is exposed: ThingSpeak > channel > API Keys > Generate New Write API Key, then `keys` again.

## 3. Rules on the bench

1. **One source at a time, always through the USB-C port.** Laptop USB for S1–S4 (serial console); power manager 5 V into the same port, through INA226 #3, for every energy measurement (no serial then: read the DMM and ThingSpeak). Nothing is ever fed into the VBUS pin or VIN.
2. Measure the 5 V feed with the DMM (4.75–5.25 V, polarity) at the cut cable before it is plugged into the board.
3. Bench runs use `run 0`. Without panels, fault 1 and 2 are expected.
4. Write every reading in the table as it is taken, with units and time.

## 4. Plan, 10:00–13:00

Chimdi and the actuator lead run the **parallel track** (M11, M6–M9) on the bench supply from 10:20. They need no board.

| Time | Step | Who |
|---|---|---|
| 10:00 | Decisions (section 1), lay out parts, photo of kit | all |
| 10:10 | **S1** flash + serial check | AM |
| 10:25 | **S2** modules on 3V3, Wi-Fi, keys, RTC/NTP | AM + CH |
| 10:40 | **S3** INA226 zero and DMM calibration (M0, M1, B3) | AM + CH |
| 11:20 | **S4** first real upload, dashboard (B1 start) | AM |
| 11:35 | **S5** M10 on USB, then move to the power manager feed: M2, M3 | AM + CH |
| 12:10 | **S6** sleep sketch M4, M5; collect M6–M9, M11 | AM + control lead |
| 12:35 | **S7** reflash data link, start the 24 h run (M12) | AM |
| 12:50 | Photos, sign the table, pack | all |

If a step overruns by 10 min, drop in this order: M5, trk/fix calibration, M4. Never drop S7.

### S1 Flash and serial check (10:10)
- [ ] `arduino-cli board list` and note the port (`/dev/cu.usbmodem…`).
- [ ] `arduino-cli upload --fqbn esp32:esp32:nano_nora:PartitionScheme=spiffs --port <port> --input-dir /tmp/ast_build firmware/ast_datalink`
      If no port or the upload fails: double-press the reset button and try again; change the cable.
- [ ] Serial monitor 115200 baud, line ending Newline: `arduino-cli monitor -p <port> -c baudrate=115200`. Save the log to a file.
- [ ] The first boot formats the data partition: wait, do not unplug. Time it.
- [ ] `status`, `power`. Type `sim off`, `run 0`, `panel A`.

Expect: `contract 1.5`; cpu 80 MHz; simulator off. Record: format time, flash %. Pass: boots, console answers. Evidence: firmware runs on the real board (closes "not yet run on hardware" in `VV_RESULTS.md`).

### S2 Modules, Wi-Fi, clock (10:25)
- [ ] Power off. Wire DS3231 and the three INA226 to 3V3, GND, A4, A5. Power on.
- [ ] `cal` lists 0x40, 0x41, 0x44 with readings (not "NOT ANSWERING"); `time` shows an RTC line.
- [ ] **AM types** `wifi "<hotspot>" <password>` and `keys 3578277 <WRITE KEY>`. Clear the monitor.
- [ ] Wait up to 60 s, then `time`: `valid=yes`, an NTP sync, oscillator-stop flag clear, "diff to system" 0 or ±1 s.
- [ ] Hold the phone showing time.is next to the `time` output; photograph both.

Pass: all four I2C devices answer; clock error ≤ 2 s. Evidence: F7 (timestamps within 2 s of UTC), B3 step 3, first reading.

### S3 INA226 zero and calibration (10:40)
Board on USB; the sensors measure a separate bench-supply circuit with common GND. Do `use` first; `trk` and `fix` if time allows, otherwise on 3 Dec.
- [ ] `cal use shunt <ohms>` with Chimdi's value (also `trk`, `fix`).
- [ ] **M0**: IN+ and IN− connected, load open. `cal`: note the raw mA of `use`. Pass |I| ≤ 0.1 mA.
- [ ] Voltage: supply at 3.0 V, DMM across bus and GND, `cal use v 1 <DMM V>`; supply at 5.5 V, `cal use v 2 <DMM V>`.
- [ ] Current: DMM in series (mA range), 250 Ω load, `cal use i 1 <DMM mA>`; 22–27 Ω load, `cal use i 2 <DMM mA>`. Keep each point steady for 2 s before Enter.
- [ ] **M1**: a third point (about 100 mA): `cal` against the DMM.
- [ ] B3 step 2 as far as time allows: loads up to what the shunt permits; DMM power against `cal` mW.

Expect: gains 0.95–1.05 (the firmware refuses anything outside 0.5–2). Pass: within ±2 % of the DMM at the check point. Evidence: validity of P_use and of every energy figure (F9 figures, report V&V table).

### S4 First real upload (11:20)
- [ ] `status`: time valid, Wi-Fi configured, buffer filling one record per minute.
- [ ] `upload`. Read the log line `upload_ok n=… http=… radio_on_s=…`.
- [ ] Laptop: ThingSpeak channel 3578277 shows new entries with seconds `:00`, status `IDL;r=0;p=A;e=…` and **no** `s=1`.
- [ ] Phone: dashboard > Refresh. Stopwatch from the end of a logged minute to its appearance on screen.
- [ ] Leave it running: scheduled uploads at hh:00:20, :15:20, :30:20, :45:20. Note each `upload_ok` time.

Pass: HTTP 200 (or 202) on the first attempts; `radio_on_s` ≤ 12 s; uploads 15 min ± 30 s apart; sample on ThingSpeak ≤ 16 min and on screen ≤ 17 min after its minute ends. Evidence: F8 (timed upload, HTTPS transport working), F9 (sample to screen), `radio_on_s` for M3.
If it fails: 401/404 = wrong key or channel; TLS failure = clock not valid; no join = hotspot on 5 GHz or asleep.

### S5 Power, as built (11:35)
- [ ] **M10** (still on USB): DMM in series with the 3V3 feed to the four modules. Expect 1.19 mA; pass ≤ 1.46 mA.
- [ ] Unplug the laptop. Feed checked with the DMM. Wire: power manager 5 V USB out > DMM (mA) > INA226 #3 > Nano USB-C port. DRV8833 disconnected.
- [ ] **M2**: DMM mean over 60 s, radio off (avoid the minute after each quarter hour). Expect 22.9 mA (114 mW); pass ≤ 35.8 mA (179 mW).
- [ ] **M3**: across the next upload slot (radio on from about hh:mm:12) note the DMM peak (max-hold) and the time. Afterwards read field3 on ThingSpeak: energy = (P of the upload minute − P of the minute before) × 60 s + M2 power × session length. Expect about 4.2 J; pass ≤ 7.1 J.
- [ ] Cross-check: field3 for a quiet minute against the DMM reading × feed voltage (±2 %).

Evidence: P_use (F10, the marker's "energy gained against energy spent"); replaces the 2.3 Wh/day estimate in the README.

### S6 Sleep and actuators (12:10)
- [ ] Flash the sleep sketch from the laptop, then move the USB-C port to the power manager feed. **M4** light sleep, 60 s DMM mean: expect 2.78 mA; pass ≤ 3.4 mA. **M5** deep sleep: expect 3.20 mA (information only).
- [ ] Parallel track results: **M11** power manager efficiency at 20 mA and 100 mA (expect 86 %, pass ≥ 80 %) and no-load battery current (pass ≤ 2 mA); **M6** DRV8833 nSLEEP high minus low (expect 1.7 mA, pass ≤ 3 mA); **M7** motor running current both ways (expect 200 mA, pass ≤ 400 mA; peak about 1 A); **M8** seconds for 90° (expect 45 s; pass V × I ÷ speed ≤ 1.5 J/°); **M9** push the panel with the driver asleep (pass < 0.5° movement).

Evidence: light-sleep decision and the 22.5 mW design target; motor energy term of P_use.

### S7 Start the 24 h run (12:35)
- [ ] Reflash the data-link firmware on USB. `status`: keys and Wi-Fi still stored, time valid, `sim off`, `run 0`, `upload every 15`. Note `next seq`.
- [ ] Move the USB-C port to the power manager feed through INA226 #3, final wiring. Note the start time and battery voltage.
- [ ] Confirm one scheduled upload arrives on ThingSpeak before packing.
- [ ] Keep the hotspot on and within range for 24 h, with a second device joined to it if the phone drops an idle hotspot.

Expect (firmware as built): 3.11 Wh/day, about 130 mW mean. Pass: within ±20 % (2.5–3.7 Wh); completeness ≥ 99 % (≥ 1,426 of 1,440 samples). Evidence: F7 completeness, F8 over 24 h, measured E_use.

## 5. Fallbacks

| Problem | Still do |
|---|---|
| Only the board arrived | S1; Wi-Fi + NTP (no RTC: every sample carries e=4, expected); S4 with `sim on clear` (rows marked s=1) to prove Wi-Fi, TLS and flash storage on the real board; M2 with a lab DMM in a bench 5.0 V feed into the USB-C port (cut cable) |
| No INA226 | Fields omitted, fault 1–3 expected. All currents by DMM only |
| No DS3231 | NTP only; record e=4 behaviour (B3 step 4, FMEA row "RTC loses time") |
| No power manager | Bench supply at 5.0 V into the USB-C port (cut cable) for M2–M4; M11 moves to 3 Dec |
| Hotspot will not join | Second phone; hotspot settings screen kept open; `time 2026-11-26T10:40:00Z` by hand to keep logging; upload later (8-day buffer) |
| No board at all | `firmware/build.sh host` on the laptop (195 tests), sleep sketch, wiring check with Chimdi against section 1, agree the decisions, M6–M9 and M11 with the bench supply |
| Upload refused | Note the HTTP code and log line; do not retype keys in front of others; carry on with S5 using the DMM |

## 6. Results table

| # | Quantity | Expected | Pass | Measured | Time | Pass? | Initials |
|---|---|---|---|---|---|---|---|
| S1 | First-boot format time (s) | not known | boots | | | | |
| S2 | I2C devices answering | 4 | 4 | | | | |
| S2 | Clock error against time.is (s) | 0 | ≤ 2 | | | | |
| M0 | INA226 #3 zero (mA) | 0.0 | ≤ 0.1 | | | | |
| S3 | `use` V gain / offset | 1.000 / 0 | 0.95–1.05 | | | | |
| S3 | `use` I gain / offset | 1.000 / 0 | 0.95–1.05 | | | | |
| M1 | Check point: DMM mA / INA mA / error % | equal | ±2 % | | | | |
| S3 | `trk`, `fix` calibrated? | yes | ±2 % | | | | |
| S4 | First upload: HTTP code, n | 200 | 200/202 | | | | |
| S4 | radio_on_s (s) | 9 | ≤ 12 | | | | |
| S4 | Upload interval (min:s) | 15:00 | ± 30 s | | | | |
| S4 | Minute end to screen (min:s) | ≤ 15:20 | ≤ 17:00 | | | | |
| M10 | Modules on 3V3 (mA) | 1.19 | ≤ 1.46 | | | | |
| M2 | As built, radio off (mA at 5 V) | 22.9 | ≤ 35.8 | | | | |
| M3 | Energy per upload (J); peak (mA) | 4.2 | ≤ 7.1 | | | | |
| M4 | Light sleep (mA) | 2.78 | ≤ 3.4 | | | | |
| M5 | Deep sleep (mA) | 3.20 | info | | | | |
| M6 | DRV8833 awake − asleep (mA) | 1.7 | ≤ 3 | | | | |
| M7 | Motor running E / W (mA); peak | 200 | ≤ 400 | | | | |
| M8 | 90° travel E / W (s); J/° | 45; 0.5 | ≤ 1.5 J/° | | | | |
| M9 | Movement when pushed (°) | 0 | < 0.5 | | | | |
| M11 | Efficiency at 20 / 100 mA (%); no-load mA | 86; < 2 | ≥ 80; ≤ 2 | | | | |
| M12 | Start time, seq, V_bat; E_use after 24 h (Wh) | 3.11 | 2.5–3.7 | | | | |

5 V feed voltage: ______ V   Shunts #1/#2/#3: ______ / ______ / ______ Ω   DMM model: ____________   Hotspot phone: ____________

## 7. Evidence checklist (report appendices, V&V log)

- [ ] Photo: whole bench with the wiring visible; close-up of INA226 #3 position and the USB-C 5 V feed
- [ ] Photo: DMM display beside the `cal` output for each calibration point
- [ ] Photo: phone on time.is beside the `time` output
- [ ] Saved serial log file (keys line removed) and `status`, `power`, `cal` outputs
- [ ] Screenshot: ThingSpeak channel 1 with the first real entries; dashboard on the phone
- [ ] Photo: DMM for M2, M4; motor set-up for M7–M9
- [ ] This table, signed, photographed. Names of who did what (peer-marking evidence)
- [ ] No key, password or hotspot name in any image

## 8. After the lab (Thu evening and Fri 27 Nov)

- [ ] `power_budget.py` constants: M2 > `I_MS80`; M3 > `I_UPLOAD`, `T_UPLOAD_S`; M4/M5 > `I_LS_BOARD`, `I_DS_BOARD`; M6 > `I_DRV_ON`; M7/M8 > `I_MOTOR`, `OMEGA`; M11 > `ETA_BOOST`. Run it again; update the break-even figure.
- [ ] `docs/VV_RESULTS.md`: add a "Bench results 26 Nov" section **outside** the generated E2E block; update the B1/B3 rows and "Known limitations" in `firmware/README.md`; correct the wiring table (INA226 #3 position).
- [ ] Fri 12:40: read E_use for the 24 h run (dashboard export), battery voltage, completeness. Then move the USB-C port back to the laptop and log 12 consecutive `upload_ok` lines to finish B1.
- [ ] Notion: App/Website progress log, Control log, weekly outcomes; Gantt row "first hardware test" marked done with the date.
- [ ] AI log: this runbook was drafted with AI, so add an entry (prompt, what was checked against the repo, what was changed), plus any AI use during the lab.
- [ ] Export raw CSV to the group OneDrive (DMP). Post a three-line summary with one photo in the group chat.

**Next labs.** 3 Dec: hotspot-off outage test B2 (6 h, overnight), data-link energy with the measured constants, first real MATLAB night (AM pastes the channel-2 key at line 28 of job 430132), HTTPS capture (B1 step 5), repeated-timestamp check on a test channel, RTC battery-out test (B3 step 4), `trk`/`fix` calibration if not done. 10 Dec: two-phone test B4, final figures, report sections peer-reviewed. Report due Fri 11 Dec, 16:00.
