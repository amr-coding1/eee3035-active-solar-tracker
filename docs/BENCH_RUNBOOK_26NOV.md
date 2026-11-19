# Bench runbook: first hardware tests, Wed 25 Nov (integration) and Thu 26 Nov 2026 (lab, 10:00–13:00)

EEE3035 Group 8, Active Solar Tracker. For AM (data link and firmware, App/Website) with CO (sensing and control), CH (mechanical and actuators), AA (power and PCB), AO (PV panels and loads) and LA. Print this and the card. Lab time is GMT = UTC, so the serial log and the wall clock agree.
Sources: `firmware/tracker/README.md` (tracker firmware, wiring, commands), `docs/SETUP.md` §3, `docs/VV_RESULTS.md` (B1–B4), `firmware/README.md`, `docs/CONTRACT.md` v1.5, `power_budget.md` §7–8 (M0–M13). The model figures are estimates that have not been measured: the lab replaces them.

**Updated 19 Nov:** the actuator is the DS-R001 servo (power-switched on D8), not the worm-gear motor and DRV8833, so M6–M9 are now servo measurements and M13 (power bank at low load) is new. The whole tracker firmware (`firmware/tracker`) is ready for Wednesday; the data-link-only sketch (`firmware/ast_datalink`) stays for S1–S4 and the 24 h run if the control hardware is not ready. Field 7 is now V_supply (the 5 V feed).

## 1. Before the lab (by Wed 25 Nov)

**AM, on the Mac**
- [ ] `brew install arduino-cli` (tested with 1.6.0), then
      `arduino-cli core install esp32:esp32@3.3.12 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json`
- [ ] `git pull`, then compile with no board attached (expect 0 errors, about 41 % flash): `firmware/build.sh esp32` (both sketches), or
      `arduino-cli compile --fqbn esp32:esp32:nano_nora:PartitionScheme=spiffs --build-path /tmp/ast_build/tracker firmware/tracker`
      Arduino IDE route: core "esp32 by Espressif" **3.3.12** (not 4.x, not "Arduino ESP32 Boards" 2.0.18), board Arduino Nano ESP32, Tools > Partition Scheme > "With SPIFFS partition (advanced)", open `firmware/tracker/tracker.ino`. VS Code route: open `firmware/` with PlatformIO (see `firmware/tracker/README.md`).
- [ ] Sleep test sketch for M4/M5: `firmware/tools/sleep_test/sleep_test.ino` (compile it once).
- [ ] Phone hotspot: 2.4 GHz (iPhone: "Maximise Compatibility" on), plain ASCII name with no apostrophe, password 8–63 characters. Test that the laptop joins it.
- [ ] ThingSpeak login works on the laptop. Decide: export the 9,465 simulated rows (dashboard > Export), then clear channel 3578277 (SETUP §3, optional).
- [ ] Dashboard on the phone's home screen. Send the group the message that the data link is ready, with the three questions below.

**Bring**
- [ ] Laptop + charger, USB-C **data** cable (and a spare), phone + charger, second phone if possible
- [ ] DMM with leads (two if possible), croc clips, jumper wires, breadboard, a USB-A to USB-C cable that may be cut (or USB breakouts)
- [ ] Load resistors: about 250 Ω (20 mA at 5 V) and 22–27 Ω rated ≥ 2 W (200 mA at 5 V), or use a lab electronic load
- [ ] CR2032 for the DS3231 (check whether the module has a charging circuit before fitting it), protractor, stopwatch, this sheet, pen, tape for labels

**Power lead (AA, with CO) to confirm, in writing in the group chat**
- [ ] Which power sensors are on the bench: 3× INA226 (BOM) or the four-channel INA219 board (block diagram). The firmware works with either; the boot log prints what answers where. Set the addresses in `firmware/tracker/config.h` to match.
- [ ] Shunt value on each sensor (default assumed 0.1 Ω: ±819 mA on an INA226, ±3.2 A on an INA219 at ±320 mV). The 9 V 600 mA panels into 15 Ω stay under 0.8 A.
- [ ] Sensor #3 (0x44) sits in the **5 V feed after the power bank (or power manager) and before the Nano, servo and sensors**, IN+ to the source, IN− to the load. If the Nano and the servo come off the bank as two separate branches, put a sensor in each and set `kInaUse2`, or P_use misses one of them.
- [ ] The Nano is powered from the power manager's **5 V USB output into the Nano's USB-C port** (the documented 5 V path). **Not VIN** (Arduino: 6–21 V recommended, so 5 V is too low) and **not the VBUS pin** (it is an output, live only when USB powers the board; there is no 5 V input pin). 3.3 V into the 3V3 pin only in the way Arduino describes (solder jumper), and only if the group decides to.
- [ ] A way to break the 5 V wire of that USB feed for INA226 #3 and the DMM: a cut USB-A to USB-C cable or two USB breakout boards (data wires not needed).
- [ ] Every module (power sensors, DS3231, photodiode circuits) is on **3V3**. No Nano ESP32 pin is 5 V tolerant. The 12 Nov photodiode test was on a 5 V Uno, so the load resistors must be redone for 3.3 V (no channel above about 3.0 V under the lamp).
- [ ] Servo: red to the 5 V branch **through the MOSFET switch** (gate driver on D8, 100 kΩ pull-down), 470–1000 µF at the servo, orange to D9 through 330 Ω, brown to common GND (`firmware/tracker/README.md#servo-power-switch`). Without the switch, set `kServoPowerPin = -1` and note that the energy result is then not valid.
- [ ] Addresses: INA226 0x40 tracked (A1 = GND, A0 = GND), 0x41 fixed (A0 = VS), 0x44 use (A1 = VS); INA219 board usually 0x40–0x43; DS3231 0x68. SDA = A4, SCL = A5, bus under 30 cm.
- [ ] Power bank charged: the 24 h run needs about 3.6 Wh from it (and see M13: some banks switch off at low load).

**Group decisions to settle before or at 10:00**
- [ ] Fixed reference panel at the **tracker's tilt**, not flat (model: flat makes November "gain" +146 % against +17 % at equal tilt; the difference is tilt, not tracking).
- [ ] Control: CO leads sensing and tracking control (19 Nov), AM second on firmware. CO checks `sensing.cpp` and `control.cpp` (starter code) before Wednesday, CH checks `servo.cpp`.
- [ ] The control code reports five times a second (`transmission::report()`); light sleep with a 15 s wake changes that. Agree the sleep policy after the M2/M4 numbers, with AM changing the firmware.
- [ ] Who takes the kit home for the 24 h run (it needs the hotspot nearby).

## 2. Secrets

- Wi-Fi name/password and the channel-1 **Write API key** are typed **only by AM**, into `firmware/tracker/secrets.h` or `firmware/ast_datalink/secrets.h` (both git-ignored) or the serial console (`wifi`, `keys`; stored on the board, kept when the other sketch is flashed). No assistant, no teammate, no chat, no Notion.
- [ ] Before any commit: `git status` must not list `secrets.h`.
- [ ] Clear the serial monitor after typing `keys` and before any photo or screenshot (`status` masks keys, the typed line does not).
- If a key is exposed: ThingSpeak > channel > API Keys > Generate New Write API Key, then `keys` again.

## 3. Rules on the bench

1. **One source at a time, always through the USB-C port.** Laptop USB for S1–S4 (serial console); power manager 5 V into the same port, through INA226 #3, for every energy measurement (no serial then: read the DMM and ThingSpeak). Nothing is ever fed into the VBUS pin or VIN.
2. Measure the 5 V feed with the DMM (4.75–5.25 V, polarity) at the cut cable before it is plugged into the board.
3. Bench runs use `run 0`. Without panels, fault 1 and 2 are expected.
4. Write every reading in the table as it is taken, with units and time.

## 3a. Wednesday 25 Nov: integration test with the tracker firmware

Goal: every subsystem working together on the bench under a lamp, with data on the dashboard. About 2 hours. Commands and wiring: `firmware/tracker/README.md`.

| Step | What | Pass | Who |
|---|---|---|---|
| W1 | Flash `firmware/tracker` (`firmware/build.sh upload <port>` or the IDE). Boot log: I2C scan, chip at each address, servo line. The servo stays off until `servo <deg>` or `servo auto` | 3 power sensors (+ DS3231) answer; addresses match `config.h` | AM |
| W2 | `status`, `time`: clock valid (DS3231 or NTP over the hotspot) | `valid=yes` | AM |
| W3 | `cal`: each sensor reads the panel or feed it is wired to; shunts set (`cal <ch> shunt <ohms>`) | sensible V and mA; P_use about 0.6–0.9 W with the board awake | AA + AM |
| W4 | Photodiodes: lamp on, `sun`; cover all, `sun cal dark`; paper over them, `sun cal flat`; lamp at known angles (inclinometer/protractor), `sun` at each; set `kSunResponsePower` if cos^2 fits better | no SATURATED; estimate within about 5 deg, east negative | CO |
| W5 | Servo by hand: `servo 0` (inclinometer: level), `servo test` (8 s at each of -45 ... +45). Note commanded vs measured; set `kServoTrimDeg` and, if reversed, `kServoWestIsHigherPulse`; reflash | error within ±2 deg after trim; no reset when the servo starts | CH + CO |
| W6 | M6–M9 (servo standby current, move energy, speed, unpowered holding), see S6 | results in the table | CH + AA |
| W7 | Tracking: `servo auto`, `live on`; move the lamp slowly across; lamp off for 1 min; lamp on | panel follows in about 5 deg steps; IDL then STW at -45 in the dark; tracks again | CO + all |
| W8 | `run 12`, `panel A`, then `upload`; dashboard on a phone | minute records with TRK/MOV/STW and angles; no `s=1` | AM |

If the servo or the photodiodes are not ready, W1–W3 and W8 still prove the data path (the angles are simply omitted, state IDL).

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
- [ ] `firmware/build.sh upload <port>` (tracker firmware; it contains the data link) or `firmware/build.sh upload-datalink <port>` (data link only).
      If no port or the upload fails: double-press the reset button and try again; change the cable.
- [ ] Serial monitor 115200 baud, line ending Newline: `arduino-cli monitor -p <port> -c baudrate=115200`. Save the log to a file.
- [ ] The first boot formats the data partition: wait, do not unplug. Time it.
- [ ] `status`, `power`. Type `sim off`, `run 0`, `panel A`.

Expect: `contract 1.5`; cpu 80 MHz; simulator off. Record: format time, flash %. Pass: boots, console answers. Evidence: firmware runs on the real board (closes "not yet run on hardware" in `VV_RESULTS.md`).

### S2 Modules, Wi-Fi, clock (10:25)
- [ ] Power off. Wire the DS3231 and the power sensors to 3V3, GND, A4, A5. Power on.
- [ ] The boot log's I2C scan lists them; `cal` shows each configured address with a chip name and readings (not "NOT ANSWERING"); `time` shows an RTC line.
- [ ] **AM types** `wifi "<hotspot>" <password>` and `keys 3578277 <WRITE KEY>`. Clear the monitor.
- [ ] Wait up to 60 s, then `time`: `valid=yes`, an NTP sync, oscillator-stop flag clear, "diff to system" 0 or ±1 s.
- [ ] Hold the phone showing time.is next to the `time` output; photograph both.

Pass: all four I2C devices answer; clock error ≤ 2 s. Evidence: F7 (timestamps within 2 s of UTC), B3 step 3, first reading.

### S3 Power-sensor zero and calibration (10:40)
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
- [ ] Unplug the laptop. Feed checked with the DMM. Wire: power bank (or power manager) 5 V USB out > DMM (mA) > sensor #3 > Nano USB-C port. Servo switched off (gate open) or unplugged.
- [ ] **M2**: DMM mean over 60 s, radio off (avoid the minute after each quarter hour). Expect 22.9 mA (114 mW); pass ≤ 35.8 mA (179 mW).
- [ ] **M3**: across the next upload slot (radio on from about hh:mm:12) note the DMM peak (max-hold) and the time. Afterwards read field3 on ThingSpeak: energy = (P of the upload minute − P of the minute before) × 60 s + M2 power × session length. Expect about 4.2 J; pass ≤ 7.1 J.
- [ ] Cross-check: field3 for a quiet minute against the DMM reading × feed voltage (±2 %).

Evidence: P_use (F10, the marker's "energy gained against energy spent"); replaces the 2.3 Wh/day estimate in the README.

### S6 Sleep and actuators (12:10)
- [ ] Flash the sleep sketch from the laptop, then move the USB-C port to the power manager feed. **M4** light sleep, 60 s DMM mean: expect 2.78 mA; pass ≤ 3.4 mA. **M5** deep sleep: expect 3.20 mA (information only).
- [ ] Parallel track results (servo on the 5 V branch through its switch, tracker firmware, `servo <deg>` / `servo off` from the console): **M11** power source efficiency at 20 mA and 100 mA (expect 86 %, pass ≥ 80 %) and no-load current (pass ≤ 2 mA); **M6** servo standby current, powered and holding with pulses, then with no pulses, then gated off (listings 10–40 mA; gated off < 0.01 mA); **M7** one 5° move each way and a 90° return: mean current and duration, start peak on max-hold (expect about 100 mA for 0.4 s = 0.2 J; pass ≤ 2 J per small move); **M8** time for 90° with the panel fitted (expect about 0.3 s unloaded; pass ≤ 1 s); **M9** `servo off`, push the panel edge with about 10 N, then leave it tilted 10 min (pass < 0.5° movement: switching off between moves is allowed); **M13** light-sleep sketch from the power bank for 30 min (pass: 5 V still present, no gap in the log).

Evidence: light-sleep decision and the 22.5 mW design target; servo energy term of P_use; whether power-switching the servo is safe (M9).

### S7 Start the 24 h run (12:35)
- [ ] Reflash the tracker firmware (or the data-link sketch if the tracker hardware is not ready) on USB. `status`: keys and Wi-Fi still stored, time valid, `sim off`, `run 0`, `upload every 15`. Note `next seq`.
- [ ] Move the USB-C port to the power-bank feed through sensor #3, final wiring. Note the start time and V_supply.
- [ ] Confirm one scheduled upload arrives on ThingSpeak before packing.
- [ ] Keep the hotspot on and within range for 24 h, with a second device joined to it if the phone drops an idle hotspot.

Expect (firmware as built, servo switched off between moves): 2.87 Wh/day, about 120 mW mean. Pass: within ±20 % (2.3–3.4 Wh); completeness ≥ 99 % (≥ 1,426 of 1,440 samples). Evidence: F7 completeness, F8 over 24 h, measured E_use.

## 5. Fallbacks

| Problem | Still do |
|---|---|
| Only the board arrived | S1; Wi-Fi + NTP (no RTC: every sample carries e=4, expected); S4 with `sim on clear` (rows marked s=1) to prove Wi-Fi, TLS and flash storage on the real board; M2 with a lab DMM in a bench 5.0 V feed into the USB-C port (cut cable) |
| No power sensors | Fields omitted, fault 1–3 expected. All currents by DMM only |
| No DS3231 | NTP only; record e=4 behaviour (B3 step 4, FMEA row "RTC loses time") |
| No power bank / power manager | Bench supply at 5.0 V into the USB-C port (cut cable) for M2–M4; M11 and M13 move to 3 Dec |
| Servo not mounted or no switch yet | `kServoPowerPin = -1`; W5 on the bench with the servo loose (horn marked); M9 waits for the frame |
| Photodiodes not ready | Tracking off (`servo 0`, manual); the data path and powers still work (angles omitted) |
| Hotspot will not join | Second phone; hotspot settings screen kept open; `time 2026-11-26T10:40:00Z` by hand to keep logging; upload later (8-day buffer) |
| No board at all | `firmware/build.sh host` on the laptop (211 tests incl. the closed-loop twin), sleep sketch, wiring check against section 1, agree the decisions, M11 with the bench supply |
| Upload refused | Note the HTTP code and log line; do not retype keys in front of others; carry on with S5 using the DMM |

## 6. Results table

| # | Quantity | Expected | Pass | Measured | Time | Pass? | Initials |
|---|---|---|---|---|---|---|---|
| S1 | First-boot format time (s) | not known | boots | | | | |
| S2 | I2C devices answering (chip names) | 4 | 4 | | | | |
| S2 | Clock error against time.is (s) | 0 | ≤ 2 | | | | |
| M0 | Sensor #3 zero (mA) | 0.0 | ≤ 0.1 | | | | |
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
| M6 | Servo standby: pulsing / no pulses / gated off (mA) | 10–40; < 0.01 off | record | | | | |
| M7 | 5° move E / W: mean mA × s; peak (mA); J | 100 × 0.4; 0.2 J | ≤ 2 J | | | | |
| M8 | 90° travel with panel E / W (s) | 0.3 | ≤ 1 | | | | |
| M9 | Unpowered: movement when pushed; after 10 min (°) | not known | < 0.5 | | | | |
| W5 | Servo test: commanded vs inclinometer at -45/0/+45 (°); trim set | 0 | ±2 after trim | | | | |
| W7 | Tracking a lamp: steps of about 5°; IDL→STW in the dark | yes | yes | | | | |
| M13 | Power bank, 30 min light sleep: still on? | yes | yes | | | | |
| M11 | Efficiency at 20 / 100 mA (%); no-load mA | 86; < 2 | ≥ 80; ≤ 2 | | | | |
| M12 | Start time, seq, V_supply; E_use after 24 h (Wh) | 2.87 | 2.3–3.4 | | | | |

5 V feed voltage: ______ V   Shunts #1/#2/#3: ______ / ______ / ______ Ω   DMM model: ____________   Hotspot phone: ____________

## 7. Evidence checklist (report appendices, V&V log)

- [ ] Photo: whole bench with the wiring visible; close-up of sensor #3 position, the USB-C 5 V feed and the servo switch
- [ ] Photo: DMM display beside the `cal` output for each calibration point
- [ ] Photo: phone on time.is beside the `time` output
- [ ] Saved serial log file (keys line removed) and `status`, `power`, `cal` outputs
- [ ] Screenshot: ThingSpeak channel 1 with the first real entries; dashboard on the phone
- [ ] Photo: DMM for M2, M4; servo set-up for M6–M9; inclinometer readings for W5 (video of the tracking run W7)
- [ ] This table, signed, photographed. Names of who did what (peer-marking evidence)
- [ ] No key, password or hotspot name in any image

## 8. After the lab (Thu evening and Fri 27 Nov)

- [ ] `power_budget.py` constants: M2 > `I_MS80`; M3 > `I_UPLOAD`, `T_UPLOAD_S`; M4/M5 > `I_LS_BOARD`, `I_DS_BOARD`; M6 > `I_SERVO_STBY`; M7/M8 > `I_MOVE_TYP`, `T_MOVE`, `T_GATE_ON`; M11 > `ETA_BOOST`; if M9 fails, `SERVO_GATED = False`. Run it again; update the break-even figure.
- [ ] `docs/VV_RESULTS.md`: add a "Bench results 25–26 Nov" section **outside** the generated E2E block; update the B1/B3 rows and "Known limitations" in `firmware/README.md` and `firmware/tracker/README.md`; put the measured trim and sensor directions in `config.h`.
- [ ] Fri 12:40: read E_use for the 24 h run (dashboard export), battery voltage, completeness. Then move the USB-C port back to the laptop and log 12 consecutive `upload_ok` lines to finish B1.
- [ ] Notion: App/Website progress log, Control log, weekly outcomes; Gantt row "first hardware test" marked done with the date.
- [ ] AI log: this runbook was drafted with AI, so add an entry (prompt, what was checked against the repo, what was changed), plus any AI use during the lab.
- [ ] Export raw CSV to the group OneDrive (DMP). Post a three-line summary with one photo in the group chat.

**Next labs.** 3 Dec: hotspot-off outage test B2 (6 h, overnight), data-link energy with the measured constants, first real MATLAB night (AM pastes the channel-2 key at line 28 of job 430132), HTTPS capture (B1 step 5), repeated-timestamp check on a test channel, RTC battery-out test (B3 step 4), `trk`/`fix` calibration if not done. 10 Dec: two-phone test B4, final figures, report sections peer-reviewed. Report due Fri 11 Dec, 16:00.
