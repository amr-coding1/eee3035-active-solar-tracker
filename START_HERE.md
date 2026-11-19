# Start here: EEE3035 Group 8, Active Solar Tracker

Everything for the project is in this repository: the tracker firmware, the data link and dashboard, the bench plan, the energy model, the team status, the marks playbook, the report drafts and the AI logs. This page says where we are and where each thing lives. Updated 19 Nov 2026, evening (AM).

**Live:** [phone dashboard](https://amr-coding1.github.io/eee3035-active-solar-tracker/dashboard/) · [project hub](https://amr-coding1.github.io/eee3035-active-solar-tracker/) · ThingSpeak [raw samples 3578277](https://thingspeak.mathworks.com/channels/3578277) and [daily results 3578278](https://thingspeak.mathworks.com/channels/3578278)

## Where we are (19 Nov)

The parts arrived on 19 Nov. The software is ready for the first bench test on **Wed 25 Nov**; nothing has run on the real board yet.

1. **The firmware for the whole tracker is written and tested on a PC.** It is one program for the Arduino Nano ESP32, split the way Lem asked: `main.cpp` calls `transmission` (data to ThingSpeak), `sensing` (photodiodes), `servo` (DS-R001) and `control` (tracking). 215 PC tests pass and it compiles with no warnings. See [firmware/tracker](firmware/tracker/README.md).
2. **The data path is live.** ThingSpeak, the nightly MATLAB job and the phone dashboard run today on simulated data. The whole chain passed 129 of 129 end-to-end checks in virtual time ([V&V results](docs/VV_RESULTS.md)).
3. **The servo replaced the worm gear everywhere:** firmware, dashboard, docs and energy model. The firmware powers it only while it moves (MOSFET on D8). Test M9 checks whether it holds the panel unpowered.
4. **Field 7 is V_supply,** the 5 V feed voltage, not battery charge.
5. **The block diagram and the BOM disagree.** The diagram has a servo, a power bank and an INA219 board; the BOM has a worm gear, an 18650 and INA226s. The firmware handles both sensor chips, but the group must say what is on the bench by Mon 23 Nov.
6. **Energy is the headline risk.** As built, the tracker is modelled at 2.87 Wh/day, while in December tracking gains about 0.70 Wh/day over a fixed panel. Light sleep (modelled at 0.49 Wh/day) is the next firmware job, after the bench measurements.
7. **The cheapest marks left** are project management and risk (11/20 in the brief) and a verbatim AI log ([marks playbook](docs/team/MARKS_PLAYBOOK.md)).

Full status, the BOM table, owners and to-dos: [docs/team/STATUS_2026-11-19.md](docs/team/STATUS_2026-11-19.md).

## Is it ready end to end?

| Part | State on 19 Nov | Evidence | Still to do |
| --- | --- | --- | --- |
| Tracker firmware (Nano ESP32) | Built; 0 warnings on ESP32 core 3.3.12 and 4.0.0 and in PlatformIO | 215 PC tests incl. a closed-loop lamp-sweep twin; independent review, 11 issues fixed ([tests](firmware/TESTS.md)) | First flash and W1–W8 on Wed 25 Nov |
| Data link to ThingSpeak | Built, inside the tracker firmware | 129/129 end-to-end checks in virtual time ([results](tests/e2e/results/e2e_summary.md)) | First real upload; 24 h run from 26 Nov |
| Nightly analysis (MATLAB) | Live on ThingSpeak, 00:20 UTC | Daily rows identical to the Python reference ([analysis](analysis/README.md)) | Check channel 2 has new rows on 20 Nov |
| Phone dashboard | Live, with servo wording and V_supply | 32 unit and 9 real-Chrome tests incl. accessibility ([dashboard](dashboard/README.md)) | Real data once the board uploads |
| Energy model | Done; modelled, not measured | [Power budget](docs/energy/power_budget.md) | Replace with bench numbers (M2–M5, M10–M13) |
| Hardware | Parts arrived 19 Nov | — | Confirm the bench parts; servo switch (CH); photodiode resistors for 3.3 V (CO) |
| Light sleep | Not started | [Sleep test sketch](firmware/tools/sleep_test/sleep_test.ino) ready | After M2/M4; needed for a net gain in winter |
| Report | LA's draft on OneDrive; AM's sections drafted | [Report plan](docs/REPORT_PLAN_AM.md), [AM sections draft](docs/report/AM_REPORT_SECTIONS_DRAFT_v2.md) | Drafts Fri 4 Dec; upload Fri 11 Dec before 4 pm |
| AI log | AM's sessions logged verbatim | [AI logs](#where-everything-is) | Everyone back-fills theirs by 26 Nov |

## What we've done

| Date | What | Where |
| --- | --- | --- |
| 19 Nov, evening | Everything gathered in this repo: team status, marks playbook, report draft, this page | This page |
| 19 Nov | Tracker firmware (transmission, sensing, servo, control), servo update, V_supply rename, energy model with the servo, Wednesday bench plan; pushed to GitHub | [firmware/tracker](firmware/tracker/README.md), [runbook](docs/BENCH_RUNBOOK_26NOV.md) |
| 19 Nov | Parts arrived; subsystems picked in the group chat | [Status](docs/team/STATUS_2026-11-19.md) |
| 19 Nov, morning | Energy model redone bottom-up; ThingSpeak write keys renewed; nightly job writing to channel 2 | [Power budget](docs/energy/power_budget.md) |
| 17 Nov | BOM ordered (£120.27 of £125); P_use agreed as everything the tracker draws | Notion (BOM) |
| 13 Nov | Data link, nightly MATLAB job and dashboard built and tested; dashboard live; data host changed to ThingSpeak | [Contract](docs/CONTRACT.md), [V&V](docs/VV_RESULTS.md), [decision matrix](docs/decisions/data_host_matrix.md) |
| 12 Nov | Design-brief feedback: 82/100, with project management and risk at 11/20 | [Marks playbook](docs/team/MARKS_PLAYBOOK.md) |
| 3 Nov | Design brief submitted | SurreyLearn |

## Next steps

| When | Who | What |
| --- | --- | --- |
| Mon 23 Nov | AA + CO | Say which parts are on the bench; update the BOM against £125 |
| Mon 23 Nov | CH | Build the servo power switch; mount the servo with the panel balanced |
| Mon 23 Nov | CO | Review `sensing.cpp` and `control.cpp`; set the photodiode resistors for 3.3 V |
| Mon 23 Nov | AO | Wire both panels to their 15 Ω loads and to sensors #1 and #2 |
| Mon 23 Nov | LA | Book the lamp, stand and inclinometer; confirm the room |
| Wed 25 Nov | All | Integration test W1–W8 ([runbook](docs/BENCH_RUNBOOK_26NOV.md), [bench card](docs/BENCH_CARD_26NOV.md)) |
| Thu 26 Nov | All | Lab S1–S7: calibration, first P_use numbers, start the 24 h run; minute the decisions |
| Thu 3 Dec | AM, CO, each lead | Light-sleep firmware; five tracked-vs-fixed runs; one decision matrix per lead; AI log back-filled |
| 4–11 Dec | All | Drafts Fri 4 Dec, results freeze Tue 8 Dec, everyone uploads Fri 11 Dec before 4 pm |
| Mon 25 Jan | All | Presentation and live demo |

## Where everything is

**Team**

- [Team status, 19 Nov](docs/team/STATUS_2026-11-19.md): TL;DR, BOM against the block diagram, what we missed, owners, to-dos, messages
- [Marks playbook](docs/team/MARKS_PLAYBOOK.md): how the marks work, what the report is marked on, page budget, brief feedback mapped to fixes, lab plan, presentation, decisions

**Firmware**

- [Tracker firmware](firmware/tracker/README.md): wiring, servo switch, serial commands, first power-up. Start here for Wednesday.
- [Firmware overview](firmware/README.md) · [interface between control and the data link](firmware/INTERFACE.md) · [tests and results](firmware/TESTS.md)
- Pins and settings: [`firmware/tracker/config.h`](firmware/tracker/config.h)

**Bench tests**

- [Bench runbook, 25–26 Nov](docs/BENCH_RUNBOOK_26NOV.md) (W1–W8, S1–S7, M0–M13) · [one-page bench card](docs/BENCH_CARD_26NOV.md)

**Data link, cloud and dashboard**

- [Data contract v1.5](docs/CONTRACT.md): the one specification every part follows
- [Set-up](docs/SETUP.md) · [V&V results](docs/VV_RESULTS.md) · [end-to-end results](tests/e2e/results/e2e_summary.md)
- [Dashboard](dashboard/README.md) · [analysis (MATLAB, Python)](analysis/README.md) · [report figures](analysis/figures)

**Energy**

- [Power budget and break-even](docs/energy/power_budget.md) · [net gain by month](docs/energy/net_gain_by_month.md) · [figures](docs/figures)

**Report**

- [AM's report plan](docs/REPORT_PLAN_AM.md) · [AM's report sections, draft v2](docs/report/AM_REPORT_SECTIONS_DRAFT_v2.md)
- [Data-link objectives, traceability and FMEA](docs/AM_OBJECTIVES_FMEA.md) · [data management plan v2](docs/DMP_v2.md) · [data-host decision matrix](docs/decisions/data_host_matrix.md)

**AI logs** (verbatim prompts and what was checked)

- [13 Nov](docs/AI_LOG_ENTRY_2026-11-13.md) · [19 Nov, morning](docs/AI_LOG_ENTRY_2026-11-19.md) · [19 Nov, afternoon](docs/AI_LOG_ENTRY_2026-11-19b.md) · [19 Nov, evening](docs/AI_LOG_ENTRY_2026-11-19c.md)

**Outside this repo**

- The group chat (WhatsApp) and the [Notion workspace](https://fishy-canoe-485.notion.site/EPS-YR3SEM1-3ebf4629be36801c8fabcd4547e0255c): minutes, action log, AI-usage database, block diagram, BOM
- LA's final report draft (.docx on OneDrive)
- SurreyLearn (university login): briefs, slides, sample reports, submission folders
- AM's appendices draft (.docx) and the 19 Nov evidence pack (.zip) were shared in AM's AI chat; add them under `docs/report/` when ready

## Rules

- **This repository is public.** Anyone can read it, including the drafts and marks in `docs/`.
- **Never commit keys or passwords.** Wi-Fi and ThingSpeak keys go in `secrets.h`, which git ignores; on the board they are typed with the `keys` command.
- **Log AI use verbatim:** prompt, raw output, how you checked it and what you changed. Vlad checks the AI log strictly.
- **Own your subsystem's code.** `sensing`, `servo` and `control` are starter code for CO and CH: change them, test them, and say what you changed in the commit message.
