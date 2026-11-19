# EEE3035 – AM report sections (draft v2)

19 Nov 2026 · AM. Moved here from Claude Docs on 19 Nov (evening). Overview of the whole repository: [START_HERE.md](../../START_HERE.md).

**AI-assisted draft (Claude and Fable agents, 19 Nov 2026). Verify every number, rewrite it in your own words and log it in the AI appendix before any of it goes into the report.**

> **Updates since this draft was written (19 Nov, evening)**
>
> - The actuator is now the DS-R001 servo, switched off between moves; the worm gear and DRV8833 are out. The power sensors may be INA219 instead of INA226 (the firmware detects either), so check "three INA226" in X.3 against what is on the bench.
> - Field 7 is now V_supply (the 5 V feed voltage).
> - Energy as built, with the servo: 2.87 Wh/day (about 120 mW), replacing 130 mW (3.11 Wh/day) below. Light sleep: 0.49 Wh/day (about 20 mW). See [docs/energy/power_budget.md](../energy/power_budget.md).
> - The whole tracker firmware now exists ([firmware/tracker](../../firmware/tracker/README.md)) and passes 215 PC tests; nothing has run on hardware yet.
> - ThingSpeak licence checked on 19 Nov: Free licence, expiring 12 Nov 2027 (this answers the X.2 [VERIFY]).
> - Both ThingSpeak write keys were renewed on 19 Nov (07:45–07:55 UTC).
> - The appendices file mentioned below (AM appendices draft v2, .docx) is not in this repository yet.

## How to use this draft

This is about 960 words of body text for your ~1.8-page share of the 25 pages. It leaves room for your own results in the "own work" slots. Nothing in it has run on hardware yet.

- **AI rules (Week 1 slides p18–24):** declared AI use is allowed. Presenting AI output as your own thought is misconduct, and the slides warn against unverified autonomous agents. So the section opens by saying what the AI generated and what you did. Rewrite it in your own words and log the session in Appendix E.
- **Ask Dr Stolojan in writing this week** whether declared use of AI agents is acceptable for the individual mark. If the answer is no, rebuild the section around X.4 and X.5 only.
- **[AM: own work] slots** are the evidence for your individual mark. Each one names a task only you can do. Write them after you have done it, in the past tense. If a task didn't get done, delete the slot.
- **Status words are fixed:** "Pass (simulation)", "Pending bench 26 Nov" or "Pending 3 Dec". Replace them only with your own bench results. No [VERIFY], [MEASURE] or [AM] tag may survive into the report.
- **Citations:** [A1] is the Claude session of 13 Nov and [A2] the session of 19 Nov. Other numbers follow the verified reference list. Sources [1] and [9] were checked at abstract level only, [23] is a product page and [30] a forum answer, so open or replace them.
- **Appendices** (data contract, V&V log, traceability, DMP v2, AI log, decision matrix, FMEA, energy chapter draft) are in the attached appendices file, not here.

## X Data link and dashboard (AM)

**Who did what.** Claude AI agents generated the firmware, test suites, MATLAB job, dashboard, data contract, objectives O1–O7, host decision matrix, FMEA and DMP v2 draft in one session on 13 Nov [A1]. The energy model and the first draft of this text were generated on 19 Nov [A2]. Prompts, outputs and my checks are in Appendix E. My own work is the requirements and design decisions I chose or accepted (Appendix E, adaptation log), the light-sleep change to the sampler, and the bench verification in X.4 and X.5. **[AM: confirm each is true before submission; delete any that is not.]**

### X.1 Requirements, objectives and status (AM)

The aim is a timed, secure and free path from the tracker to a phone, with data complete enough to prove or disprove a net energy gain (client aspects ii and iii). Table A links each requirement to its objective, test and status. The simulation results come from AI-run tests of the current firmware in virtual time [A1]. They check the logic, not the hardware.

**Table A. Data-link requirements (F7–F9, with F10 in support), objectives, tests and status at 19 Nov.**

| Req. | Target (contract v1.5) | Objective, due | Method | Simulation result (AI-run, 13 Nov) | Status |
| --- | --- | --- | --- | --- | --- |
| F7 | ≥ 99 % of 1,440 samples a day reach the cloud | O2, 26 Nov | 24 h run, entries counted per day | 1,440 of 1,440 on 3 virtual days | Pass (simulation); Pending bench 26 Nov |
| F7 | No logged sample lost in a 7-day outage (8-day buffer) | O3, 3 Dec | Hotspot off 6 h, sequence numbers compared | 0 of 11,535 minutes missing after 7.5 virtual days | Pass (simulation); Pending 3 Dec |
| F7 | Time stamps within 2 s of UTC | O2, 26 Nov | Clock against a reference, with NTP and after 24 h without | Logic only: nothing logged while the clock is invalid | Pending bench 26 Nov |
| F8 | Upload every 15 min; on ThingSpeak ≤ 16 min after the minute ends | O2, 26 Nov | Upload times from the serial log and the channel | 15 min 00 s; 14 min 20 s (design value, no network delay) | Pass (simulation); Pending bench 26 Nov |
| F8 | HTTPS only; write key stays on the device | O2, 26 Nov | Traffic capture; request inspection | Key in 0 of 48 dashboard requests; HTTPS not testable on the mock | Pending bench 26 Nov |
| F9 | Dashboard and MATLAB agree on every complete day | O4, each real night to the 8 Dec freeze [VERIFY date] | "MATLAB check" column | 5 of 5 simulated days agree | Pass (simulation); Pending 3 Dec |
| F9 | Sample on screen ≤ 17 min; two phones identical | O1, 10 Dec | Stopwatch; two real phones | 15 min 20 s (design value); two browser profiles identical | Pass (simulation); Pending 3 Dec |
| F10 | Tracker consumption P_use ≤ 22.5 mW mean (0.54 Wh/day) | O5, 3 Dec | INA226 #3 and DMM, 24 h | Model only: 130 mW with the current firmware | Pending bench 26 Nov |

The objective wording, the data-link MoSCoW list (AI-proposed, not yet a group decision) and the traceability matrix are in Appendices B and C.

### X.2 Host choice (AM)

The design brief named Supabase. On 13 Nov the host was changed to ThingSpeak, and the AI produced a weighted decision matrix as a retrospective check of that change [A1]. ThingSpeak scored 4.55 against Supabase's 3.80 on seven criteria (Appendix F). The result holds across the weights tested: ThingSpeak stays first in 2,184 of 2,187 weight combinations and in 95.8 % of random score changes. **[AM: own work — re-score the matrix by hand; say which weights or scores you changed and whether the winner changed.]**

The client asks for a "secure" location. For non-personal sensor readings [37] we read that as integrity and availability. Only the device and the MATLAB job hold a write key [33], and the device buffer and a weekly backup protect against loss. Reads are public, because a private channel would put a read key in a public web page.

Design-brief comment 18 (University enterprise service) is answered, pending confirmation of the licence type shown in my ThingSpeak account. The channels sit on a University MathWorks account with backups on University OneDrive. However, MathWorks states that a Campus-Wide licence gives the Free ThingSpeak licence [30] **[VERIFY: Account > My Account]**. Where MathWorks hosts the data is not confirmed **[VERIFY]**.

### X.3 Design (AM)

*Figure X (draw from the text diagram in docs/CONTRACT.md section 1). Caption: "Data path: device, HTTPS bulk write, channel 1, nightly MATLAB job, channel 2, charts and phone dashboard."*

The current firmware reads three INA226 power monitors [22] at 1 Hz. It stores the mean of each UTC minute, with angles, state, a fault code and a sequence number, in a flash ring buffer of at least 11,520 records (8 days). Every 15 min it sends the unacknowledged records to ThingSpeak channel 1 in one HTTPS bulk write of up to 960 entries [34]. A nightly MATLAB job [31] rewrites daily energy, gross gain and net gain for the last 9 complete days into channel 2. A static phone dashboard recalculates the same figures from channel 1 and compares them with channel 2, so the two implementations check each other (fields and rules: Appendix A). This answers comments 7 and 8: data are stored on the device so that an outage loses nothing, and online so that analysis and display do not depend on the device.

Three rules protect the data:

- Failed uploads are retried with back-off, and a batch the server refuses is halved until the one bad record is found.
- Nothing is logged while the clock is invalid, because a missing sample is better than a wrongly dated one.
- Loss beyond the buffer raises a fault code and is never silent.

Integration is through four telemetry calls from the control code (Appendix A.5) and three hardware points to agree with CO and AA **[VERIFY: agreed, with date]**:

- All I2C modules run from 3.3 V, because no Nano ESP32 pin is 5 V tolerant [21].
- INA226 #3 sits in the 5 V feed to the tracker, not the battery line, so that it sees consumption and not charging current.
- The 0.1 Ω shunts clip at 819 mA, close to the panel's short-circuit current [26] **[VERIFY shunt value]**.

The energy model (X.5) shows that the current firmware, which keeps the processor awake, has to change. The sampler must sleep, wake every 15 s and let the INA226 average internally. That replaces the 1 Hz design, so the simulations in Table A apply to the current firmware and will be repeated after the change.

### X.4 What I did on hardware (AM)

**[AM: own work — bring-up, 26 Nov: what you wired and flashed, the first real upload, what failed and how you fixed it. Use your lab notes.]**

**[AM: own work — light sleep: the change you made to the sampler (esp_light_sleep_start, 15 s wake, INA226 averaging), the control-call time-out you changed in INTERFACE.md, and the unit and end-to-end test counts when you re-ran them yourself.]**

**[AM: own work — if the parts were late: the fallback you ran instead. The Nano ESP32 alone on USB, uploading for 24 h with no sensors, still tests F8, HTTPS, the clock and the awake current.]**

### X.5 Results and discussion (AM)

**Table C. Bench results (slot; runbook items M0–M12 and B1–B4). Give the instrument, number of repeats, mean and spread for every row.**

| Quantity | Model or target | Measured (n, mean ± spread) | Status |
| --- | --- | --- | --- |
| Board awake, radio off, sensors on (M2) | Model 114 mW | [MEASURE 26 Nov] | Pending bench 26 Nov |
| Energy per upload (M3) | Model 4.2 J | [MEASURE 26 Nov] | Pending bench 26 Nov |
| Board in light sleep (M4) | Model 13.9 mW | [MEASURE 26 Nov] | Pending bench 26 Nov |
| P_use over 24 h, current firmware (M12) | Model 130 mW | [MEASURE 26 Nov] | Pending bench 26 Nov |
| P_use over 24 h, light-sleep firmware | Target ≤ 22.5 mW | [MEASURE] | Pending 3 Dec |
| Completeness and upload delay, 24 h (B1) | ≥ 99 %; ≤ 16 min | [MEASURE 26 Nov] | Pending bench 26 Nov |
| Clock error, with NTP and after 24 h (B3) | ≤ 2 s | [MEASURE 26 Nov] | Pending bench 26 Nov |
| Samples lost in a 6 h outage (B2) | 0 | [MEASURE] | Pending 3 Dec |
| Two real phones (B4) | Identical | [MEASURE] | Pending 3 Dec |

**[AM: own work — two or three sentences on what the measurements show against Table A and the model, including anything that failed.]**

**Energy.** The group energy chapter (CO, AM) shows that in December the tracker pays for itself only if it draws less than 29 mW on average. The whole tracker with the current firmware is modelled at 130 mW (3.11 Wh/day); this is a model, not a measurement. The light-sleep design is modelled at 21.9 mW, only 3 % inside the 22.5 mW target, so the result depends on the measurements in Table C.

### X.6 Data governance (AM)

DMP v2 (Appendix D) replaces the plan in the design brief. Data go from device flash to ThingSpeak, with a weekly export to University OneDrive. Write keys stay on the device and in the MATLAB job. Simulated samples are marked s=1, and the channel is cleared before real tests **[AM: date cleared]**. The channels and repository are now public, unlike the submitted DMP and ethics form; the group agreed the change on **[VERIFY: date and minute]**, and the updated forms are appended.

### X.7 Risk and trade-off (AM)

The highest-scoring failure mode in the FMEA (Appendix G) is that the electronics' own consumption erases the tracking gain. The mitigation is the light-sleep change, judged by the P_use rows of Table C. The main trade-off is the upload interval. Uploading every 15 min meets F8 but is modelled at 0.11 Wh/day, 16 % of the December budget, against 0.03 Wh/day for hourly uploads.

## Outside your 1.8 pages

These go elsewhere in the report and don't count toward your technical section.

- **Energy analysis** goes in the group chapter as "(CO, AM)". The draft text and figure reference are Appendix H of the appendices file.
- **Cost of data-link parts and cost per extra Wh** go in the group BOM and economics section (AA, CO); material in Appendix I.
- **Accessibility:** one sentence in the group EDI section beside the EIA; material in Appendix I.
- **Your personal reflection** (about 150 words) goes in the group's reflections block. Write it yourself; these are prompts only:
    - What the minutes record about your attendance (8, 13, 15, 22 Oct and 5 Nov), and its effect on the group
    - What you delivered afterwards, with dates, and what you did by hand
    - Your use of AI agents: what they produced, what you checked, one thing you changed or rejected, and what you would do differently (learning outcome 006)
    - One skill learned, with evidence; one SMART CPD goal with a date

**Before submission:** export the full transcripts of the 13 and 19 Nov AI sessions to PDF for Appendix E (the folder takes no links). Fill "By AM personally" and "Adaptation" in the AI log. Regenerate both ThingSpeak write keys before the first real run, because they were visible on screen during the 13 Nov AI session. *[Done on 19 Nov, 07:45–07:55 UTC.]*
