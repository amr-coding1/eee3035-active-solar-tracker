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
2. **Define P_use.** An always-awake ESP32 board (~40 mA at 5 V) uses ~4.8 Wh/day, more
   than a flat panel makes in November. Decide whether P_use is the whole tracker supply
   (current INA226 #3 wiring) or the tracking overhead only, and report both if possible.
3. **Indoor lamp tests prove the mechanism, not the yield.** Use PVGIS for the outdoor
   yield argument (validation), lamp runs for F1-F3 verification.
4. **Control system still has no lead.** AM is second choice; the data link's
   INTERFACE.md is ready for whoever takes it.

## Week-by-week (for the Gantt)
| Lab | AM deliverable |
|---|---|
| Thu 19 Nov | Demo the live dashboard + MATLAB charts on ThingSpeak (simulated data); agree DMP v2; agree INTERFACE.md and who leads control |
| Thu 26 Nov | Flash the ESP32 (if parts arrived); 24 h upload run; calibration with DMM |
| Thu 3 Dec | Hotspot-off outage test; data-link energy measurement; first real MATLAB night |
| Thu 10 Dec | Two-phone test; final figures; report sections peer-reviewed |
| Fri 11 Dec | Report submitted (each member submits) |
| Mon 25 Jan | Presentation + live demo (AM: live dashboard on phones) |
