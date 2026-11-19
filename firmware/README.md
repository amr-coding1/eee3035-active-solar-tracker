# AST firmware (EEE3035 Group 8, Active Solar Tracker)

Two sketches share the same data link:

- **`tracker/` - the whole tracker** (19 Nov 2026): `transmission` (the data
  link below), `sensing` (photodiode arc), `servo` (DS-R001 with a power
  switch) and `control` (tracking decisions), called from `main.cpp`. Opens in
  the Arduino IDE and in VS Code with PlatformIO (`platformio.ini` here). Start
  with [`tracker/README.md`](tracker/README.md): wiring, commands, first power-up.
- **`ast_datalink/` - the data link on its own**, for bench tests of the
  data path (B1-B4) and the 24 h energy run.

This file documents the data link. Owner: AM (data manager). Implements
`docs/CONTRACT.md` v1.5, section 7: the device side of "timed data
transmission to a known free secure location" (requirements F7, F8). Board:
Arduino Nano ESP32 (ESP32-S3), core `esp32:esp32` 3.3.x (tested 3.3.12).

**Status (19 Nov 2026): both sketches compile with no warnings and the logic
passes 211 PC tests, including a closed-loop digital twin of the tracker. Not
yet run on the real hardware** - see "Known limitations".

## What it does

```
3 power sensors + control-system telemetry --1 Hz--> minute aggregator --1/min--> ring buffer on LittleFS
   (or the built-in simulator, s=1)                                          (11,520 records = 8 days)
                                                                                   |
ThingSpeak channel 1  <-- HTTPS bulk_update, every 15 min at second 20 -- upload state machine
```

- Every second: reads the three power sensors and the tracker state; every
  UTC minute: stores one record (mean powers, end-of-minute angles/state,
  `MOV` if the motor ran, one fault code).
- Every 15 minutes (5-60, at hh:mm:20): uploads everything not yet
  acknowledged, at most 960 records per request, further batches 16 s apart.
- Survives: Wi-Fi outages up to 8 days (nothing lost), power cuts at any moment
  (two alternating metadata copies with CRC), lost HTTP responses (re-send,
  optional read-back), a record the server refuses (isolated by bisection and
  dropped alone), clock steps backwards (a minute is never logged twice).
- Saves energy: radio off between uploads, CPU at 80 MHz, LEDs off.
- Never logs with an unknown time (RTC lost and no NTP: fault 4, no samples).

| Directory | Content |
|---|---|
| `core/` | portable C++17 (no Arduino headers): record codec, ring buffer, aggregator, status string, JSON builder, upload state machine, time validity, simulator, CLI parser, calibration maths (INA226 and INA219), telemetry API, `DataLink`; and for the tracker: sun estimate (`ast_sun`), servo pulse map and move sequence (`ast_servo`), tracking decisions and stall guard (`ast_tracker`) |
| `ast_datalink/` | data-link sketch: `ast_datalink.ino`, adapters in `src/esp_*.{h,cpp}` (LittleFS, Wi-Fi/TLS/HTTP, DS3231, INA226/INA219, NVS, tasks, CLI), `src/core/` = generated copy of `core/` |
| `tracker/` | tracker sketch: `main.cpp`, `config.h`, `transmission`, `sensing`, `servo`, `control`; `src/datalink/` = generated copy of `core/` and the adapters |
| `host/` | CMake build for the PC: 211 unit tests (ASan + UBSan) incl. the closed-loop twin, ThreadSanitizer test, `ast_host_device`, `smoke_mock.sh` |
| `build.sh`, `sync_core.sh` | build + test everything; copy `core/` and the adapters into the sketches |
| `platformio.ini` | VS Code + PlatformIO project for `tracker/` (pioarduino platform, core 3.3.12) |
| `INTERFACE.md`, `TESTS.md` | API for the control-system code; test list and results |

The ESP32 and the PC run the same `core/` sources; `build.sh` refuses to
compile if a copy in a sketch differs.

## Wiring

All modules are powered from the Nano ESP32 **3V3** pin, so every I2C line
idles at 3.3 V. **No GPIO of the Nano ESP32 is 5 V tolerant.** The tracker's
photodiodes, servo and power switch are in [`tracker/README.md`](tracker/README.md#wiring).

| Signal | Nano ESP32 pin | Goes to |
|---|---|---|
| SDA | A4 (GPIO11) | SDA of the power sensors and DS3231 |
| SCL | A5 (GPIO12) | SCL of the power sensors and DS3231 |
| 3V3 | 3V3 | VCC/VS of all four modules |
| GND | GND | GND of all modules, common with panel and battery negative |

| I2C address | Device | Address pins | Measures | Channel field |
|---|---|---|---|---|
| 0x40 | INA226 #1 | A1 = GND, A0 = GND | TRACKED panel output | field1 `P_trk` |
| 0x41 | INA226 #2 | A1 = GND, A0 = VS | FIXED panel output | field2 `P_fix` |
| 0x44 | INA226 #3 | A1 = VS, A0 = GND | tracker consumption on the **5 V feed** (MCU, sensors, servo) | field3 `P_use`, field7 `V_supply` (bus voltage) |
| 0x68 | DS3231 | fixed | UTC clock with backup cell | timestamps |

**INA226 or INA219.** Each address is identified at first contact (register
0xFE "TI" = INA226; otherwise a reset that reads back 0x399F = INA219; an
INA3221 is refused). A four-channel INA219 board usually answers at
0x40-0x43: set the addresses in `tracker/config.h` (or `Defaults.ina_addr`).
A second consumption branch (MCU and servo fed separately) can be added to
P_use with `kInaUse2`. The INA219 averages over 68 ms instead of the INA226's
1.05 s, so a servo burst shorter than about 0.1 s can be missed in P_use.

Limits to respect:

- INA226 supply VS 2.7-5.5 V: use 3V3 (so that the pull-ups are at 3.3 V).
  Bus input (VBUS, IN+/IN-) up to 36 V, independent of VS. Shunt voltage at
  most +/-81.92 mV: with the 0.1 ohm shunt of the common breakout that is
  +/-819 mA; for more current fit a smaller shunt and tell the firmware
  (`cal <ch> shunt <ohms>`). High-side wiring: IN+ to the source, IN- to the load.
- DS3231 module: power it from 3V3. Many "ZS-042" boards have a charging
  resistor/diode for a rechargeable LIR2032: check before fitting a
  non-rechargeable CR2032.
- Pull-ups: the modules carry their own (typically 10 k or 4.7 k each); four in
  parallel is fine at 100 kHz. Keep the bus short (< 30 cm).
- A sensor that does not answer is not fatal: its field is omitted and fault
  1/2/3 is reported; it is picked up again when it reappears.

## Build and flash

Tested tool versions: arduino-cli 1.6.0, core `esp32:esp32` 3.3.12.

```
arduino-cli core install esp32:esp32@3.3.12 \
  --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
cp firmware/tracker/secrets.example.h firmware/tracker/secrets.h   # optional, see below
firmware/build.sh esp32                          # compile both sketches
firmware/build.sh upload /dev/ttyACM0            # compile and flash the tracker (Windows: COM5)
firmware/build.sh upload-datalink /dev/ttyACM0   # the data link on its own
```

FQBN: `esp32:esp32:nano_nora:PartitionScheme=spiffs` ("With SPIFFS partition
(advanced)": 2x 3 MB app + 9.4 MB data partition, mounted as LittleFS; the ring
buffer uses about 0.5 MB). In the Arduino IDE: open `ast_datalink/ast_datalink.ino`,
Tools > Partition Scheme > "With SPIFFS partition (advanced)". The first boot
formats the data partition, which takes a while (duration not measured yet).

Serial monitor: 115200 baud, line ending "Newline".

## Wi-Fi and keys

Either put defaults in `secrets.h` (git-ignored) before flashing, or type them
on the serial console; console values are stored in NVS and win.

```
wifi "My Phone" hotspot-password      2.4 GHz phone hotspot (ESP32 has no 5 GHz); no eduroam on the device
keys 3578277 <WRITE KEY> [<READ KEY>]  channel-1 id and keys; the read key is optional (read-back)
upload every 15                        5-60 minutes
status                                 check: time valid, Wi-Fi, last upload
upload                                 send now
```

The WRITE key exists only on the device (contract 9). `status` shows keys masked.

TLS: the server certificate is always verified, with the CA bundle built into
the esp32 core (`NetworkClientSecure::useBuiltinCACertBundle()`, Mozilla root
list). To pin one root instead, define `TS_ROOT_CA_PEM` in `secrets.h` with a
PEM obtained on a trusted network (never from a network that inspects TLS).

## Serial commands (contract 7.6)

`help`, `status`, `time`, `buffer`, `upload`, `run <id>`, `panel A|B`,
`sim on|off`, `cal`, `wifi <ssid> <pass>`, `keys <ch1> <write> [read]`, `reboot`,
plus: `time <ISO>` (set the clock by hand), `buffer dump [n]` (CSV of the newest
records), `buffer clear yes`, `upload every <5-60>`, `sim on <scenario>`
(clear, cloudy, overcast, night, lamp), `power`.

Before a test: `run 12`, `panel A`. Calibration runs (both panels fixed) use
`run 900`..`999`. `run 0` when nothing is being tested.

## Calibrating the INA226 against the bench DMM

Each sensor is corrected with `true = gain x reading + offset`, separately for
voltage (V) and current (mA), from two operating points. Stored in NVS.

1. `cal` shows raw and calibrated readings of the three sensors.
2. Voltage of the tracked-panel sensor: bench supply on the panel input at a
   low voltage, DMM (volts) across VBUS-GND. Read the DMM, type
   `cal trk v 1 2.004`. Raise the supply near the top of the range, type
   `cal trk v 2 5.998`. The firmware prints gain and offset and stores them.
3. Current: DMM (mA range) in series with a load. `cal trk i 1 20.3` at a
   small current, `cal trk i 2 251.0` at a large one.
4. Repeat for `fix` and `use`. Check with `cal` against the DMM at a third
   point; note the residual error in the lab book.

The firmware takes its own reading at the moment you press Enter (a 1 s
average): keep the operating point steady. It refuses points closer than 20 %
of the larger reading and gains outside 0.5-2 (typing mistakes).
`cal <ch> <v|i> reset` returns to gain 1, offset 0.
`cal <ch> shunt <ohms> [current LSB mA]` sets the shunt value supplied by the
power-system lead (default 0.1 ohm, 0.1 mA/bit).

## Energy budget

The tracker only pays off if its own consumption stays below the tracking
gain. Figures supplied by the lead (PVGIS, Guildford, 5.4 W panel, November):
gain about 1.5-2.5 Wh/day; a board left fully awake at 30-40 mA uses
2.4-4.8 Wh/day. `P_use` (INA226 0x44) measures the truth; the table is the
design estimate to compare it with. **The currents are datasheet-typical
values quoted by the lead (Espressif ESP32-S3 datasheet: modem-sleep about
27 mA at 3.3 V; Wi-Fi connect + TLS + POST 3-5 s at 100-150 mA). None of them
has been measured on our board yet.**

| Mode | Current at 3.3 V (assumed) | Duty | Energy |
|---|---|---|---|
| CPU 80 MHz, radio off (sampling, logging) | 27 mA | ~99 % (85,500 s/day) | 2.12 Wh/day |
| Upload session: radio on, CPU 160 MHz (96/day x ~9 s: 8 s lead + request) | 125 mA | ~1 % (864 s/day) | 0.10 Wh/day |
| 3x INA226 + DS3231 (about 1.2 mA together, datasheet typical) | 1.2 mA | 100 % | 0.10 Wh/day |
| **Data link total (estimate)** | | | **about 2.3 Wh/day (96 mW average)** |
| For comparison: same board, Wi-Fi always connected, 240 MHz (30-40 mA or more) | | | 2.4-4.8 Wh/day or more |

What the firmware does about it:

- (a) Wi-Fi is OFF between uploads: on 8 s before an upload is due (so the
  request still starts at second 20), kept on while backlog batches follow
  (16 s apart), then `WiFi.mode(WIFI_OFF)`. With no valid time it tries NTP for
  60 s every 5 min instead of staying on.
- (b) CPU at 80 MHz (the lowest setting that keeps Wi-Fi/USB working); 160 MHz
  only while the radio is on, to shorten the TLS handshake.
- (c) Yellow and RGB LEDs off (the green power LED is hard-wired).
- (d) The 1 Hz sampler sleeps in `vTaskDelayUntil`; nothing busy-waits.
- `power` prints CPU MHz, Wi-Fi state and radio-on seconds today; every upload
  line in the log carries `radio_on_s`.

The estimate shows that the baseline (27 mA) dominates, not the radio.
**Follow-up for the bench (measure first, then decide):**

1. Measure the real board current in each mode with the DMM in the battery
   line (or read `P_use`): radio off at 80 MHz, during an upload, with the
   motor driver idle. The on-board regulator and power LED add to the
   datasheet figures.
2. Light sleep between samples (ESP32-S3 datasheet: a few hundred uA) would cut
   the baseline by an order of magnitude. Automatic light sleep is not
   available in this core build (`CONFIG_PM_ENABLE` is not set in
   esp32s3-libs 3.3.12); an explicit `esp_light_sleep_start()` in the sampler
   stops the USB serial port and the control loop while asleep, so it must be
   agreed with the control-system owner.
3. Shorter radio lead (`kRadioLeadS` in `core/ast_power.h`, 8 s now) once the
   real hotspot connect time is known.

## Tests

```
firmware/build.sh host     # 195 unit tests (ASan+UBSan), TSan test, selftest, smoke test against tools/mock_thingspeak.py
firmware/build.sh          # the above + ESP32 compile with zero warnings in our files
```

Details and results: `TESTS.md`.

## Decisions (where the contract left a choice, the safer one was taken)

1. **TLS trust.** The firmware verifies the server with the core's built-in
   CA bundle, not a single pinned root: a correct PEM could not be obtained on
   the build machine (its proxy re-signs TLS), and a wrong or expired pin stops
   all uploads. `setInsecure()` is never used; `TS_ROOT_CA_PEM` pins one root
   when wanted. Contract 7.3 was amended to say so (13 Nov 2026).
2. **Fault 9** = records pending AND uploads failing (a failed attempt since
   the last success, or no channel/key configured, or a rejected batch being
   narrowed down) AND 60 min or more since the last success (since boot if
   none). So hourly uploads that work never raise it.
3. **Fault 4.** While time is invalid nothing is logged, so no sample could
   carry the code: the first sample logged after time becomes valid carries
   `e=4` once (the gap is visible in the data). If no RTC is fitted at all,
   every sample carries `e=4` even after NTP.
4. **Fault 1/2/3** (power sensor missing) are detected by the data link (it
   owns the INA226): priority is control code, then 1/2/3, then 4, 6, 9.
   Latched 4 and 6 are cleared only by a sample that actually carried them.
5. **MOV vs FLT.** `MOV` replaces the end-of-minute state when the motor ran,
   except `FLT`, which is kept (a faulted minute must not count as pointing).
   A control fault seen at any second of the minute is reported for that minute.
6. **Partial minutes** (boot, restart) are logged from the samples available.
   `V_supply` and `T_panel` are minute means (like the powers); `V_supply` is
   the bus voltage of the consumption sensor (0x44, 5 V feed).
7. **Strictly increasing timestamps.** The newest logged minute is stored with
   the cursors; a record for a minute not later than it is refused (log line
   `skip`), after a clock correction or a reboot inside the same minute too.
   The JSON builder also leaves out any record whose minute is not later than
   the previous one in the batch (logged as `drop`). If the clock was ever set
   wrongly into the future, `buffer clear yes` removes the guard (seq is kept).
8. **Upload timing.** After boot the uploader waits for the next slot. A
   backlog continues 16 s later only after a full batch of 960 (a record that
   arrives during an upload waits for the next slot). Spacing is measured from
   the END of the previous request.
9. **Lost response.** Counts as a failure (backoff 1 min first). With a read
   key, one GET of `fields/8/last.json` at least 30 s later; the answer is
   trusted only if it lies inside the doubtful batch, otherwise the whole
   batch is sent again (readers de-duplicate by seq).
10. **Rejected content** (HTTP 400, or 200/202 without `"success":true`): the
    batch is halved every 16 s until one record is alone; only that record is
    dropped (`DROP seq=...` in the log). At most 3 records are dropped without
    a success in between; after that the data is kept and retried every
    15 min (the server is refusing everything, not one record).
    HTTP 401/403/404: nothing is dropped, retry every 15 min, fault 9 after 60 min.
    429/5xx/others: same batch again with backoff 1, 2, 4, 8, 15 min.
11. **Storage layout.** 120 segment files of 96 records (3,840 bytes, one flash
    block) instead of one large file, because LittleFS rewrites a file from the
    changed block onwards. Records are 40 bytes with CRC-32 and hold scaled
    integers (0.1 mW, 0.1 deg, 1 mV), so the JSON text is identical on ESP32 and PC.
12. **Both metadata copies unreadable:** the buffer is rebuilt by scanning the
    records (highest seq wins, so seq is still never reused) and everything
    found is uploaded again.
13. **Control system silent for more than 5 s:** angles are reported as
    unknown (omitted) and the state as `IDL`, never the last known values.
14. **Simulator.** A minute that contains any simulated second is marked
    `s=1`. While the simulator is on the real sensors are ignored completely.
    Simulator geometry: N-S axis tilted 35 deg, fixed panel at the same tilt,
    travel +/-60 deg, sun sensor sees +/-70 deg.
15. **JSON values are numbers** (`"field1":123.4`), not strings.
16. **HTTP timeouts:** TCP connect 10 s, TLS handshake 20 s, response 30 s
    (contract 7.4); all far below the 180 s watchdog.
17. **Energy:** radio lead 8 s, linger 20 s, CPU 160 MHz during uploads (see
    "Energy budget"); all three are constants to tune after bench measurements.

## Known limitations

- Not yet run on the Nano ESP32: Wi-Fi/TLS against the real ThingSpeak, I2C
  drivers (INA226, INA219, DS3231), LittleFS timing, watchdog, servo PWM and
  current consumption are untested on hardware. The PC tests cover the logic only.
- The PC device (`ast_host_device`) speaks plain HTTP to the mock only.
- No light sleep yet (see "Energy budget").
- One-second samples are taken on the FreeRTOS tick, not locked to the RTC
  second; a minute can contain 59 or 61 samples.
- The optional `arduino:esp32` 2.0.18 core was not tried (large download).

## Correction (AM, 19 Nov 2026)

Apply these before the 26 Nov bench session:

- **INA226 #3 (0x44) goes in the 5 V feed to the tracker, not the battery line** (wiring table above, and step 1 of the energy follow-up). With the Solar Power Manager the battery line also carries the charging current, so P_use can't be separated from charging there. In the 5 V feed it sees only what the tracker draws: MCU, sensors and servo. Its bus voltage (field7, renamed `V_supply` on 19 Nov) reads the 5 V rail, not the battery.
- **Power the Nano ESP32 through its USB-C port.** VBUS is not a power input on this board, and VIN expects 6–21 V.
- **The 2.3 Wh/day estimate is low.** It leaves out the converter loss, the power LED and the photodiode bias. With those the firmware as built comes to about 2.87 Wh/day (120 mW; 3.11 before the servo replaced the motor driver on 19 Nov), with the servo switched off between moves. See `docs/energy/power_budget.md`; bench steps are in `docs/BENCH_RUNBOOK_26NOV.md`.
