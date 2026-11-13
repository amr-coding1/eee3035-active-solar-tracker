# AM — data link and dashboard: aim, SMART objectives, traceability, FMEA

Owner: Abdulrahman Mustafa (AM). Subsystem: App/Website (Testing Phase table on
Notion: "App/Website — Abdul 1st"; second choice Control System). Data manager
in the DMP. Date: 13 Nov 2026 (week 7).

## Aim
Give the group a timed, secure and free path from the tracker to a phone, with
data complete and traceable enough to prove (or disprove) net energy gain.

## SMART objectives (each tied to a requirement and a Gantt week)

| # | Objective (Specific, Measurable) | Achievable because | Relevant to | Time-bound |
|---|---|---|---|---|
| O1 | Phone dashboard shows live power, angles, daily energy, gross and net gain, status timeline and 4 CSV exports from the real ThingSpeak channels | Built and working on real ThingSpeak with simulated data (13 Nov) | F9, client req. (iii) | Demo at the 19 Nov lab |
| O2 | ESP32 uploads every 15 min over HTTPS; >= 99 % of samples on ThingSpeak within 16 min of their minute when online (24 h bench run) | Firmware written and host-tested; ESP32 compile passes | F8, client req. (ii) | 26 Nov lab |
| O3 | No sample lost across a 7-day Wi-Fi outage (8-day buffer): proven in virtual-time E2E and a >= 2 h hotspot-off test on hardware | Buffer and back-fill already pass in simulation | F7 | 3 Dec lab |
| O4 | Nightly MATLAB job agrees with the dashboard on 100 % of complete days (contract 6.5) for 7 consecutive days | Same formulas in Python, MATLAB, JS; agreement rule automated | F9 analysis, MPS 5 | 4 Dec |
| O5 | Data-link energy measured (radio-on s/day, MCU current via INA226 ch3) and kept <= 0.5 Wh/day | Radio off between uploads, 80 MHz CPU; PVGIS shows why it matters | F10 | 3 Dec lab |
| O6 | DMP v2 agreed by the group; weekly CSV backup to OneDrive every week to 22 Jan | DMP v2 drafted; export button exists | Data governance (report rubric) | Agree 19 Nov; weekly |
| O7 | AM report sections drafted for peer review, then final | Material in this pack | Final report 65 % | Draft 4 Dec, final 10 Dec |

## Requirement traceability (design brief F-list -> evidence)

| Req | Metric and margin (design brief) | Design element | Verification (docs/VV_RESULTS.md) |
|---|---|---|---|
| F7 | Logging interval minutes; > 7 days offline | 1-min records, LittleFS ring >= 11,520 records, DS3231 UTC, seq numbers | E2E S2/S3/S4/S8/S9 + hotspot-off bench test |
| F8 | Timed packets (15 min), latency within packet | bulk_update.json every 15 min, 960/batch, backoff, HTTPS + write key | E2E S1/S5/S6/S7/S12 + 24 h bench |
| F9 | Current metrics, Wh/day, gain 0.1 %, status | Dashboard (metrics.js), MATLAB nightly + charts, CSV exports | Unit tests, browser checks, MATLAB check column, two-phone test |
| F10 | Daily energy use vs gain | P_use field, energy budget, radio-off policy | Energy budget (docs/energy) + bench current |
| F11 | Fault reporting | Status `e=` codes, fault 6/9 from the data link, stale-data banner | E2E S8/S9 + intentional faults on bench |

## FMEA (Week 1 lecture: failure mode and effects analysis)
S = severity, O = occurrence, D = detection difficulty (1 good - 10 bad); RPN = S x O x D.

| Failure mode | Effect | Cause | S | O | D | RPN | Mitigation in the design | RPN after |
|---|---|---|---|---|---|---|---|---|
| Wi-Fi/hotspot unavailable for days | Gap in results, F7 fails | Phone hotspot off, lab Wi-Fi blocks devices | 8 | 6 | 3 | 144 | 8-day flash buffer, back-fill, nightly rewrite of last 9 days | 32 (S8 O4 D1) |
| Response lost after ThingSpeak stored a batch | Duplicate samples inflate energy | Timeout after send | 7 | 4 | 6 | 168 | seq numbers + de-duplication in every metric (contract 6.1); whether ThingSpeak's bulk update rejects a repeated timestamp is to be confirmed on the real service (the CSV import does) | 14 |
| One bad record makes ThingSpeak reject a batch | Queue blocked forever ("poison batch") | Duplicate timestamp in a batch, clock stepped back | 9 | 3 | 7 | 189 | strictly increasing timestamps; bisect a 400-rejected batch and drop only the bad record | 27 |
| RTC loses time (coin cell, first boot) | Wrong timestamps corrupt days | DS3231 oscillator stop | 8 | 3 | 5 | 120 | no logging until NTP/RTC valid, fault 4 visible | 24 |
| Flash buffer full | Oldest unsent data overwritten | Outage > 8 days | 7 | 2 | 4 | 56 | fault 6 raised, loss counted on dashboard | 14 |
| ESP32 baseline current erases the gain | Net gain negative, project aim fails | MCU awake at 240 MHz, radio on | 9 | 7 | 4 | 252 | radio off between uploads, 80 MHz, LEDs off, measure P_use; group to define P_use | 72 |
| Write key leaked | Anyone can write fake data | Key in repo/screenshot | 8 | 3 | 6 | 144 | key only in secrets.h/NVS, never in repo or dashboard; regenerate key if exposed | 24 |
| Simulated data mistaken for real | False conclusion in report | Demo data left in channel | 9 | 3 | 3 | 81 | `s=1` on every simulated sample, banner and CSV flag; clear channel before real tests | 9 |
| MATLAB job fails silently | Results channel stale | Script error, 20 s limit | 5 | 4 | 4 | 80 | dashboard computes everything itself; MATLAB check column shows "not yet summarised" | 20 |
| Free-licence quota exhausted | Channel stops accepting data | 3 M msgs/yr exceeded | 7 | 1 | 5 | 35 | budget 0.53 M/yr (18 %); no 15 s single writes | 14 |
