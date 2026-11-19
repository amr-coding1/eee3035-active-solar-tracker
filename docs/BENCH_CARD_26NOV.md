# Bench card: Thu 26 Nov 2026, 10:00–13:00 (EEE3035 Group 8)

**Rules.** Keys and Wi-Fi typed by AM only; clear the monitor before photos. One power source at a time, through the USB-C port: laptop (serial) or power manager 5 V through INA226 #3 (energy tests). Nothing into VIN or the VBUS pin. All modules on 3V3. `sim off`, `run 0`. Write every reading down at once.

**Port and flash**
```
arduino-cli board list
arduino-cli compile --fqbn esp32:esp32:nano_nora:PartitionScheme=spiffs --build-path /tmp/ast_build firmware/ast_datalink
arduino-cli upload  --fqbn esp32:esp32:nano_nora:PartitionScheme=spiffs --port <port> --input-dir /tmp/ast_build firmware/ast_datalink
arduino-cli monitor -p <port> -c baudrate=115200
```
**Console (115200, Newline):** `status` · `power` · `time` · `cal` · `cal use shunt <ohms>` · `cal use v 1|2 <DMM V>` · `cal use i 1|2 <DMM mA>` · `wifi "<ssid>" <pass>` · `keys 3578277 <key>` · `upload` · `upload every 15` · `buffer` · `time <ISO UTC>`

| Time | Step | Do | Expect | Pass | Measured |
|---|---|---|---|---|---|
| 10:00 | Decide | fixed panel at tracker tilt; control lead; 1 s rule vs light sleep; who hosts the 24 h run | | agreed | |
| 10:10 | S1 | flash, `status`, `power`; time the first-boot format | contract 1.5, 80 MHz | boots | |
| 10:25 | S2 | modules on 3V3 (A4/A5); `cal`; AM types `wifi`, `keys`; `time` beside time.is | 4 devices, valid=yes | error ≤ 2 s | |
| 10:40 | M0 | `use` zero, load open | 0.0 mA | ≤ 0.1 mA | |
| | S3 | `cal use` V at 3.0 and 5.5 V; I at about 20 and 200 mA | gain 0.95–1.05 | | |
| | M1 | third point, DMM against `cal` | equal | ±2 % | |
| 11:20 | S4 | `upload`; check channel 3578277, dashboard, stopwatch | http 200, radio_on_s 9 | ≤ 12 s; screen ≤ 17 min | |
| 11:35 | M10 | DMM in the 3V3 feed to the modules | 1.19 mA | ≤ 1.46 | |
| | M2 | power manager feed, radio off, 60 s | 22.9 mA | ≤ 35.8 | |
| | M3 | next quarter-hour slot: DMM peak; field3 minute difference × 60 s | 4.2 J | ≤ 7.1 J | |
| 12:10 | M4 / M5 | sleep sketch: light / deep | 2.78 / 3.20 mA | ≤ 3.4 / info | |
| parallel | M11 | power manager efficiency 20, 100 mA; no-load | 86 %; < 2 mA | ≥ 80 %; ≤ 2 | |
| parallel | M6 | DRV8833 nSLEEP high − low | 1.7 mA | ≤ 3 | |
| parallel | M7 / M8 | motor current; seconds for 90° | 200 mA; 45 s | ≤ 400 mA; ≤ 1.5 J/° | |
| parallel | M9 | push panel, driver asleep | 0° | < 0.5° | |
| 12:35 | S7 / M12 | reflash, `status`, `upload every 15`, move to power manager feed, see one upload arrive; note time, seq, V_bat | 3.11 Wh/day | 2.5–3.7; ≥ 99 % samples | |
| 12:50 | Evidence | bench photo, DMM beside `cal`, time.is beside `time`, ThingSpeak + dashboard screenshots, serial log saved, table signed | | no key in any image | |

**Overrun: drop M5, then trk/fix calibration, then M4. Never drop S7.**

**If it fails.** No port: double-press reset, other cable. No join: 2.4 GHz ("Maximise Compatibility"), ASCII hotspot name, keep the hotspot screen open. 401/404: key or channel. TLS error: clock not valid, `time <ISO>`. INA "NOT ANSWERING": address pins, 3V3, repeat after 2 s. Only the board: S1, Wi-Fi + NTP, `sim on clear` upload (rows marked s=1), M2 by DMM from a bench 5.0 V feed into USB-C.

**Tonight:** results into `power_budget.py` and a new bench section of `VV_RESULTS.md`; Notion log, Gantt, AI log; summary and photo to the group. **Fri 12:40:** read the 24 h E_use, then finish B1 on laptop USB. **3 Dec:** B2 outage, data-link energy, first MATLAB night. **10 Dec:** B4 two phones, final figures.
