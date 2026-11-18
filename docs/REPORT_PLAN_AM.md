# Final report (due Fri 11 Dec 2026, 4 pm) — AM's part, mapped to the marking description

The module page describes the report as testing: systems-engineering methods,
hardware/software integration, cost-effectiveness and trade-offs, validation of
the final specification against customer requirements, end-to-end project
management, risk mitigation and DATA GOVERNANCE, sustainability, and synthesis of
weekly decisions and individual contributions. 20-25 pages main body for the
whole group (about 3 pages per member plus shared chapters); appendices are
mandatory and do not count. 50 % of the mark is individual: put your initials on
every heading you write, as both sample reports do ("Database and Data Management
(SK)", "3.3 Frontend Software [RBV]").

## Sections AM should own (about 3.5 pages + appendices)

| Section (initials on heading) | Pages | Content | Evidence already built |
|---|---|---|---|
| Data link requirements and objectives (AM) | 0.4 | F7-F9 metrics/margins, aim + SMART objectives O1-O7, link to Gantt rows | docs/AM_OBJECTIVES_FMEA.md |
| Data host selection (AM) | 0.5 | Weighted decision matrix, sensitivity (2,184/2,187 weight sets; 95.8 % under score noise), spider plot; public-read decision via CIA triad | docs/decisions/, docs/figures/data_host_radar.png |
| Data-link design (AM) | 0.8 | Architecture diagram; contract v1.5 (fields, status grammar, timestamps); firmware upload state machine (8-day buffer, 960/batch, backoff, poison-batch bisect, de-duplication); energy-aware radio policy | docs/CONTRACT.md, firmware/README.md |
| Analysis and display (AM) | 0.6 | Nightly MATLAB job + charts on ThingSpeak (Lem's design); phone dashboard; accessibility commitments from the EIA (WCAG 1.4.1, 1.4.3, 1.1.1) | analysis/README.md, dashboard screenshots |
| Verification and validation of F7-F9 (AM) | 0.8 | Test table: requirement, target, method, result, pass/fail (E2E virtual-time scenarios, real-ThingSpeak run, bench tests on hardware); FMEA before/after | docs/VV_RESULTS.md, tests/ |
| Data governance (AM) | 0.3 | DMP v2 summary: storage/backup, access, retention, simulated-data marking | docs/DMP_v2.md |
| Personal reflection (AM) | 0.3 | Honest: attendance early on, what you did to recover, what you learned, CPD goal | — |

Appendices: data contract; V&V logs; AI interaction log (full prompts and
responses, verification, adaptation) — REQUIRED and "monitored strictly".

## Group-level points AM should raise (they change the group's validation)
1. **Fixed reference must share the tracker's tilt.** PVGIS (Guildford, Nov): flat fixed
   panel 4.1 Wh/day, 39°-tilted fixed 8.6 Wh/day, inclined-axis tracker 10.1 Wh/day.
   Against a flat panel the "gain" (+146 %) is mostly tilt; against the tilted panel it is
   +17 %. Lem's 5 Nov plan compares with a panel "FLAT on the ground".
2. **P_use is everything (Lem, 17 Nov).** Net gain counts all the tracker draws: MCU
   with Wi-Fi, sensors, driver and motor (INA226 #3 on the whole tracker supply line).
   PVGIS model, same 5.4 W panel, inclined-axis tracker vs 39° fixed (docs/energy/):
   gross gain +29 %/year (Nov +17 %, Dec +12 %). Net of the data link's estimated
   2.3 Wh/day: +14 %/year, but negative in Nov-Feb (Nov -10 %, Dec -27 %), so the
   late-November/December lab runs will probably show a net LOSS. That is fine if the
   report shows gross and net month by month (answers comments 9, 15 and 19 below).
   A board left fully awake (~4.8 Wh/day) loses over the whole year (-2 %); light sleep
   between samples (~0.24 Wh/day) gives +28 %/year and a gain in every month. The
   motor adds on top: measure it on the bench (INA226 #3 or DMM).
3. **Indoor lamp tests prove the mechanism, not the yield.** Use PVGIS for the outdoor
   yield argument (validation), lamp runs for F1-F3 verification.
4. **Control: worked out during testing (Lem, 17 Nov).** The control code is simple;
   the data link only needs the four calls in INTERFACE.md (angles, state, motor,
   fault, once a second), and light sleep (point 2) is a choice for whoever writes it.

## Design-brief feedback (13 Nov): what the final report must fix
| Marker's point | Fix in the final report | Evidence ready |
|---|---|---|
| 0, 3: give tracked vs fixed output for the same panel, results in the first paragraph | Open with the headline numbers: +29 % gross/year, +14 % net of the data link (model, motor not yet included), then the measured lab values | docs/energy/ |
| 1: define "ideal"; 1 Sun is not available all year | Replace "ideal" with PVGIS monthly irradiance for Guildford | docs/energy/pvgis_*.json |
| 2: reference AI use | AI log appendix and AI tools cited in the references | docs/AI_LOG_ENTRY_2026-11-13.md |
| 5, 6: SMART objectives and requirements | Numbered requirements with metric and margin; SMART objectives per subsystem | docs/AM_OBJECTIVES_FMEA.md (data link) |
| 7, 8: store what, send what, why both on the device and online | Field table (what is sent each minute and each day); device buffer covers outages, ThingSpeak does analysis | docs/CONTRACT.md, docs/DMP_v2.md |
| 9, 15, 19: can the panel pay for its own movement; model the gain; energy gained vs energy spent | Gross and net gain month by month (point 2), then the bench P_use measurement | docs/energy/, INA226 #3 |
| 10, 13: actuator and roof mounting under wind and weight | Wind-load torque against the actuator rating; mounting mass | mechanical team |
| 11: fewer sensors? | Trade-off: sensor count vs pointing accuracy and cost | sensing team |
| 12: what cost? | BOM total (parts ordered 17 Nov) and cost per extra Wh | BOM |
| 14: Gantt | Work packages, milestones, deliverables, dependencies and the critical path | Gantt |
| 18: University enterprise offering | Done: ThingSpeak and MATLAB run on the University's MathWorks Campus-Wide licence; backups in University OneDrive | docs/DMP_v2.md section 4 |
| 20: the brief asked for all output | List every logged quantity against the client brief's output list | docs/CONTRACT.md section 3 |

## Week-by-week (for the Gantt)
| Lab | AM deliverable |
|---|---|
| Thu 19 Nov | Demo the live dashboard + MATLAB charts on ThingSpeak (simulated data); agree DMP v2; agree INTERFACE.md and who leads control |
| Thu 26 Nov | Parts ordered 17 Nov (Lawrence), due the week of 23 Nov: flash the ESP32; 24 h upload run; calibration with DMM; first P_use reading |
| Thu 3 Dec | Hotspot-off outage test; data-link energy measurement; first real MATLAB night |
| Thu 10 Dec | Two-phone test; final figures; report sections peer-reviewed |
| Fri 11 Dec | Report submitted (each member submits) |
| Mon 25 Jan | Presentation + live demo (AM: live dashboard on phones) |
