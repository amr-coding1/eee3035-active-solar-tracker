# Active Solar Tracker: firmware, data link and dashboard (EEE3035 Group 8)

**Live dashboard: https://amr-coding1.github.io/eee3035-active-solar-tracker/dashboard/**
· Project hub: https://amr-coding1.github.io/eee3035-active-solar-tracker/
· Demo with simulated data: [dashboard/?demo=1](https://amr-coding1.github.io/eee3035-active-solar-tracker/dashboard/?demo=1)

The data link is AM's part of the Group 8 project (University of Surrey, EEE3035, 2026/27). It
gets every measurement from the tracker to a secure online store on a timer, analyses it every
night and shows it on a phone. It covers mandatory product specifications (c) timed transmission
to a known free secure location, (d) timed display and analysis on a portable device, and the
V&V documentation for both (requirements F7, F8, F9; supports F10 and F11).

**New, 19 Nov: the whole tracker firmware** is in [`firmware/tracker/`](firmware/tracker/README.md):
`transmission` (this data link), `sensing` (photodiode arc), `servo` (DS-R001 with a power switch)
and `control` (tracking decisions), called from `main.cpp`. It opens in the Arduino IDE and in VS
Code with PlatformIO. Compiled and unit-tested on a PC; first hardware test on Wed 25 Nov.

```
3 power sensors + control --1/s--> Nano ESP32: 1-min average, 8-day flash buffer --HTTPS every 15 min--> ThingSpeak 3578277
                                                                                                                |
         phone dashboard  <---- reads both channels + embeds MATLAB charts ----  MATLAB nightly 00:20 UTC  -->  ThingSpeak 3578278
```

## What each subsystem plugs in

| Subsystem | What the data link needs | Where |
|---|---|---|
| Control system | `firmware/tracker/control.cpp` (starter code for CO) calls `transmission::report()` five times a second with panel angle, sun angle, state, servo moving and fault. Powers, clock, storage and upload are already handled. | [`firmware/tracker/README.md`](firmware/tracker/README.md), [`firmware/INTERFACE.md`](firmware/INTERFACE.md) |
| Actuators | DS-R001 servo: signal D9 through 330 Ω, power switched by a MOSFET on D8 (off between moves). Trim and direction from `servo test` with the inclinometer. | [`firmware/tracker/README.md#servo-power-switch`](firmware/tracker/README.md#servo-power-switch) |
| Power system | INA226 or INA219 (found automatically), default 0.1 Ω shunts (`cal <trk\|fix\|use> shunt <ohms>`). All modules on 3V3: no Nano ESP32 pin is 5 V tolerant. I2C 0x40 tracked, 0x41 fixed, 0x44 on the 5 V feed (P_use, V_supply); addresses in `config.h`. | [`firmware/tracker/README.md#wiring`](firmware/tracker/README.md#wiring) |
| Sun sensing | Four BPX65 on A0-A3, powered from 3V3; `sensing.cpp` gives the light direction (east negative); `sun cal dark` / `sun cal flat`. | [`firmware/tracker/README.md`](firmware/tracker/README.md) |
| Mechanical and testing | Fixed panel at the tracker's tilt. Before each test type `run <n>` and `panel A` on the serial console (115200 baud); calibration runs are 900-999. | [`docs/VV_RESULTS.md`](docs/VV_RESULTS.md) |
| Report writers | V&V tables, decision matrix, energy budget, DMP v2, SMART objectives and FMEA, AI-log entry, figures. | [`docs/`](docs/), [`analysis/figures/`](analysis/figures/), [`docs/figures/`](docs/figures/) |

## What the headline result depends on

PVGIS for Guildford, 5.4 W panel, November: tracking gains about 1.5 Wh/day (inclined axis vs a
fixed panel at the same tilt) to 2.5 Wh/day (flat vs flat). The firmware as built (awake, radio
off between uploads, servo switched off between moves) is modelled at 2.87 Wh/day; an ESP32 left
awake uses about 4.8 Wh/day; a servo left powered adds 1.2-4.8 Wh/day on its own
([`docs/energy/power_budget.md`](docs/energy/power_budget.md)).

**Decided 17 Nov (Lem): `P_use` is everything the tracker draws** (MCU with Wi-Fi, sensors, servo).
With the board awake that is a net loss in the winter months, so the lab runs will probably show
a loss: report gross and net month by month. Light sleep between samples would keep every month
positive. Still to confirm: the fixed reference panel sits at the tracker's tilt.
Figures: [`docs/figures/net_gain_by_month.png`](docs/figures/net_gain_by_month.png),
[`docs/figures/energy_budget_pvgis.png`](docs/figures/energy_budget_pvgis.png); table:
[`docs/energy/net_gain_by_month.md`](docs/energy/net_gain_by_month.md).

## Live on ThingSpeak

| Item | ID | State |
|---|---|---|
| Channel 1 "AST raw", one entry per minute | [3578277](https://thingspeak.mathworks.com/channels/3578277) | public-read; simulated test data (marked `s=1`) until the hardware runs |
| Channel 2 "AST results", one entry per day | [3578278](https://thingspeak.mathworks.com/channels/3578278) | public-read; filled by the nightly job |
| Nightly job "AST nightly summary (contract v1.5)" | MATLAB Analysis 430132, TimeControl 99365 (00:20 UTC) | dry run until the channel-2 write key is pasted (one step, `docs/SETUP.md`) |
| MATLAB charts with public URLs | [689480](https://thingspeak.mathworks.com/apps/matlab_visualizations/689480) power, [689481](https://thingspeak.mathworks.com/apps/matlab_visualizations/689481) energy, [689485](https://thingspeak.mathworks.com/apps/matlab_visualizations/689485) daily net gain, [689483](https://thingspeak.mathworks.com/apps/matlab_visualizations/689483) angles, [689484](https://thingspeak.mathworks.com/apps/matlab_visualizations/689484) status | embedded on the dashboard |

## Repository

| Folder | What | Tests |
|---|---|---|
| [`firmware/`](firmware/) | Nano ESP32 firmware: `tracker/` (whole tracker), `ast_datalink/` (data link only, bench tests), portable C++17 core, PC host build | 211 unit tests incl. a closed-loop twin (ASan/UBSan), TSan test, both sketches compile with 0 warnings |
| [`analysis/`](analysis/) | ThingSpeak MATLAB: nightly job, 5 visualizations, report figures; Python reference of contract section 6 | 41 pytest; every `.m` run in GNU Octave against the reference |
| [`dashboard/`](dashboard/) | phone web app (no build step, no keys) | 32 unit tests, 9 tests in real Chrome incl. axe-core accessibility |
| [`tools/`](tools/) | mock ThingSpeak server, Python device simulator | 69 pytest |
| [`tests/e2e/`](tests/e2e/) | whole chain in virtual time: firmware -> mock ThingSpeak -> nightly job -> dashboard | 129 checks (S1-S12) |
| [`docs/`](docs/) | [contract](docs/CONTRACT.md), [set-up](docs/SETUP.md), [V&V results](docs/VV_RESULTS.md), [DMP v2](docs/DMP_v2.md), [objectives and FMEA](docs/AM_OBJECTIVES_FMEA.md), [decision matrix](docs/decisions/data_host_matrix.md), [energy budget](docs/energy/), [AI-log entry](docs/AI_LOG_ENTRY_2026-11-13.md), [report plan](docs/REPORT_PLAN_AM.md) | - |

Start with [`docs/CONTRACT.md`](docs/CONTRACT.md): the single specification every part follows.

### Run every test

```
firmware/build.sh host                 # host build, unit tests, TSan, smoke test against the mock
firmware/build.sh esp32                # board compile (arduino-cli, esp32:esp32@3.3.12)
bash analysis/tests/run_tests.sh       # Python reference + MATLAB scripts in GNU Octave
python3 -m pytest tools -q
node --test dashboard/tests/*.test.mjs
python3 tests/e2e/run_e2e.py           # ~70 s; rewrites the E2E block of docs/VV_RESULTS.md
```

Results of 13 Nov 2026: all pass; end to end 129 of 129 with the real firmware core, the real
`ast_nightly.m` in Octave and the real dashboard in Chrome. An independent review found no
high-severity defect; its three medium and seven low findings were fixed
([`docs/VV_RESULTS.md`](docs/VV_RESULTS.md), "Independent review"). Not yet tested: anything that
needs the hardware (Wi-Fi, TLS, clock, sensors, current draw), bench procedures B1-B4.

## Rules

- No API key in this repository, ever. The channel-1 write key lives on the device (serial
  command `keys`), the channel-2 write key inside the ThingSpeak MATLAB Analysis. Both channels
  are public-read: the data are non-personal measurements.
- Simulated data always carries `s=1` and is labelled on every screen and in every export.
- CSV exports follow the DMP naming rule: `G8_<type>_<YYYYMMDD>-<YYYYMMDD>.csv`.
- AI assistance (Anthropic Claude) is declared in the group AI log with the prompts and the
  verification of every output ([`docs/AI_LOG_ENTRY_2026-11-13.md`](docs/AI_LOG_ENTRY_2026-11-13.md)).
- Cite this repository in the report (e.g. "Group 8 (2026) Active Solar Tracker data link,
  GitHub repository") so similarity checks match it to its source.
