# EEE3035 Group 8 – Marks Playbook

19 Nov 2026 · AM. Moved here from Claude Docs on 19 Nov (evening). Notes in *[italics in brackets]* mark what changed after the playbook was written. Overview of the whole repository: [START_HERE.md](../../START_HERE.md).

## Bottom line

This is AM's proposal for the 19 Nov lab; every owner is a suggestion until the group agrees it. Most of the remaining marks depend on three things. We need a measured answer to "does the tracker pay for itself?". We need to rebuild project management and risk, where the brief lost 9 of 20. And we need a complete, verbatim AI log. Initials: LA Lemuel, AA Abdulmohsen, CH Caleb, CO Chimdi, AO Achire, AM Abdulrahman.

- [ ] Everyone, 19 Nov: fix the reference panel at the tracker's tilt, agree a control lead, agree the page budget (see "Decisions")
- [ ] AM, by 26 Nov: change the data-link firmware to light sleep between samples (it currently keeps the board awake) and re-run its tests
- [ ] CO + AM, 26 Nov and 3 Dec: measure the tracker's own draw (P_use) on the bench; report gross and net gain by month
- [ ] LA + AA, by 26 Nov: rebuild the Gantt around work packages, milestones and the critical path; group SMART objectives; extend the risk register
- [ ] Each lead, by 26 Nov: one weighted decision matrix with a sensitivity check (AM shares his script as a template)
- [ ] Everyone, by 26 Nov: log every AI session verbatim in the Notion AI log; AM compiles the appendix, LA reviews it
- [ ] AM, 19 Nov: set up the V&V sheet with empty result cells; each lead fills their rows at every lab
- [ ] CO + AM, 26 Nov and 3 Dec: at least 5 repeated tracked vs fixed runs and a paired t-test, before the 8 Dec freeze
- [ ] A rotating note-taker at every lab: minutes in the Week 1 format (present, absent, decisions, who does what by when)
- [ ] Everyone: drafts Fri 4 Dec, peer review back Mon 7 Dec, results freeze Tue 8 Dec; upload the same final PDF on Fri 11 Dec well before 4 pm

## How the marks work

Half of the report mark and half of the presentation mark are individual, and peer marking can move each person's group share by up to 20 marks either way.

| Assessment | Weight | Individual / group | When |
| --- | --- | --- | --- |
| Outline Design Brief | 10 % | 0 / 100 | Submitted 3 Nov: 82/100 |
| Final Project Report | 65 % | 50 / 50 | Fri 11 Dec 2026, 4 pm (folder due 16:01) |
| Presentation + live demo | 25 % | 50 / 50 | Mon 25 Jan 2027, from 9 am, in person |

- **Module split:** 45 % individual, 55 % group. In module points: report 32.5 individual + 32.5 group, presentation 12.5 + 12.5, brief 10 (Week 1 slides p17).
- **Peer marking:** everyone rates every other member from −5 to +5 on attendance and task focus, quality of contribution, attitude, and team dynamics. Your ratings of the others must sum to zero. Personal group mark = group mark + (average peer mark ÷ 5) × 20, so +5 turns a group 65 into 85 and −2.5 turns it into 55. It changes only the group share and stays hidden from the group (Week 4 slides p7–12).
- **Submission:** each member has their own SurreyLearn folder "FINAL PROJECT REPORT", checked by Turnitin; .doc/.docx/.pdf under 80 MB, no links to online documents. For the brief every member uploaded the same PDF; assume the same for the report and confirm with Dr Stolojan.
- **Title and file name:** follow the brief's rule ("Group 8 …: title", then members in alphabetical order of surname), as the safe choice.
- **Not written anywhere, so ask Dr Stolojan:** whether cover, contents and references count toward the 25 pages (both sample reports keep them outside a 24-page body), font and margins, referencing style (sample 1 and our brief used IEEE), and the late-submission rule.

## What the report is marked on

The report is marked against nine things named on its SurreyLearn page. Our weakest areas are validation on real hardware, the revised risk matrix and the weekly-decisions record. None of them can be added at the last minute.

| Marked on (SurreyLearn) | What we already have | What is missing |
| --- | --- | --- |
| Systems engineering methods | MoSCoW (22 Oct), data-host decision matrix, requirements F1–F11, block diagram | Requirements with metric and margin, traced to design and to a test (V-model table) |
| Hardware and software integration | Data-link firmware, interface spec for control (4 calls), INA226/RTC design | Integration on the real board (26 Nov), wiring diagram, 3.3 V levels for the photodiode arc |
| Cost-effectiveness and trade-offs | BOM (ordered 17 Nov), data-host decision matrix | BOM total against £125, cost per extra Wh, trade-off tables (sensor count vs accuracy vs cost) |
| Validation against customer requirements | Data-link logic verified in simulation (129/129 checks); no hardware validation yet | Hardware results for F1–F11, measured gross and net gain, test logs in the appendix |
| End-to-end project management | Gantt, minutes, action log | Work packages, milestones, dependencies and critical path; planned vs actual; Gantt tied to the text |
| Risk mitigation | Design-brief risk matrix, data-link FMEA | Revised risk matrix compared with the brief's (the module descriptor asks for it) |
| Data governance | DMP v2 (University licence, OneDrive backup, public-read reasons) | DMP in the appendix; one paragraph in the body |
| Sustainability impact | SDG 7 and IS-8 named in the brief | SDGs "in more detail" (client brief), circular economy, end of life, energy payback |
| Weekly decisions and individual contributions | Notion minutes and action log | Weekly-decisions appendix (present, absent, decided, who does what by when); contribution table with initials |

The module descriptor also expects a state-of-the-art review, the economic benefit, key findings against objectives, group and individual reflections, and a financial plan with costings.

## Report structure and page budget

Six members writing 3.5 pages each would fill 21 of the 25 pages before any shared chapter, so each person's technical section should be about 1.8 pages. Detail goes to the appendices, which do not count. Both sample reports use 24 body pages, put initials on every heading and include a contributions table.

| Section | Pages | Suggested owner (to agree) |
| --- | --- | --- |
| Abstract: modelled and measured gain, gross and net, in the first lines | 0.3 | LA |
| 1 Introduction: problem, SDG 7 and IS-8, state of the art, competitors with prices, TRL | 1.5 | LA, AA |
| 2 Aim, SMART objectives, requirements F1–F11 with metric and margin, MoSCoW | 1.5 | LA, AA |
| 3 Systems engineering approach: method table, concept options, decision matrices | 2.0 | All leads |
| 4 Design and implementation: one initialled section per member, about 1.8 pages each (sensing AA, power and measurement CO, actuators AO, mechanical incl. wind load CH, data link, firmware and dashboard AM, LA to agree) | 10.8 | Each member |
| 5 Verification and validation: test table, gross and net gain by month, measured P_use, statistics | 2.5 | CO, AM, all |
| 6 Project management: Gantt with work packages and critical path, planned vs actual, BOM vs £125, hours, revised risk matrix | 1.8 | LA, AA |
| 7 Sustainability, ethics, EDI and data governance | 1.0 | AO, CH, AM |
| 8 Conclusions against objectives; group reflection | 0.6 | LA |
| Individual reflections (about 150 words each) | 1.5 | Everyone |
| **Total** | **23.5** | 1.5 pages spare |

*[19 Nov: the subsystem picks in the group chat differ from this table (CH mechanical and actuators, CO solar sensing and tracking control, AA PCB and power, AO PV panels and test loads, AM app/website and firmware, LA fills gaps). Re-agree the owners of section 4.]*

**Appendices (outside the page count):** AI interaction log (verbatim), weekly decisions and minutes, EIA (re-dated), DMP v2, updated ethics form, V&V test logs, data contract, BOM and order form, FMEA tables, full decision matrices, link to the code repository.

**House rules:** initials on every heading you wrote, e.g. "4.5 Data link (AM)"; one template (11 pt body, modest headings, as the 12 Nov news asked); a named editor and a second reader check captions and cross-references; IEEE references unless Dr Stolojan says otherwise.

## Design-brief feedback mapped to fixes

The brief scored 82/100, but the formative technical mark is 73: half of the 18 lost marks were in project management and risk. The final report has no 20 % "appendices present" block, so the same work would score nearer 73.

| Criterion | Score |
| --- | --- |
| Project statement | 18 / 20 |
| Technical description | 15 / 20 |
| Project management and risk | 11 / 20 |
| Appendices present | 20 / 20 |
| Sustainability | 15 / 15 |
| Overall presentation | 3 / 5 |

The marker left 21 margin comments. Initials below: LA Lemuel, AA Abdulmohsen, CH Caleb, CO Chimdi, AO Achire, AM Abdulrahman.

| Marker's comment | Fix in the final report | Owner |
| --- | --- | --- |
| 0, 3: compare the same panel tracked and fixed; move the +29 % result to the first paragraph | Open the abstract and introduction with the modelled gain (same panel, same tilt), then the measured figure | LA, with AM's figures |
| 1: define "ideal"; 1 Sun is not available all year | Rated output at standard test conditions plus Guildford monthly irradiance from PVGIS | LA |
| 2: reference the AI as well as its sources | Cite the AI tool in the text wherever it shaped a paragraph, plus the reference list and declaration | Everyone; LA compiles |
| 5: aims and SMART objectives are missing | One aim; 6–8 group SMART objectives (metric, threshold, owner, week); subsystem objectives; a results table against each | LA leads; each lead writes theirs |
| 6: Table 1 is captioned "Main risks" but holds requirements | Caption and cross-reference check by a named editor | LA + a second reader |
| 7, 8: store what, send what, and why both on the device and online? | Field table: stored each minute, sent every 15 min, computed nightly; why both places | AM |
| 9, 15, 19: can the panel power its own movement? Model the gain; energy gained vs spent | The central chapter: monthly gross and net gain, break-even budget, measured P_use, verdict | CO with AM; AO for motor energy |
| 10, 13, 19: strong wind; added weight on the roof | Wind-load torque against the worm gear's holding torque; added mass at product scale *[now the DS-R001 servo: use its holding behaviour from test M9]* | CH with AO |
| 11: how many light sensors are needed? | Trade study: five-photodiode arc vs two angled sensors (decision matrix); test both if time allows | AA |
| 12: commercial trackers at what cost? | Prices of two or three small commercial trackers; cost per extra Wh vs simply adding a panel | AA with LA |
| 14: Gantt not linked to work packages | Work packages, milestones, deliverables, dependencies and critical path; referenced in each section; planned vs actual | LA |
| 18: could a University enterprise service be used? | Answered in DMP v2 (ThingSpeak under Surrey's MathWorks licence, backup on University OneDrive); confirm the licence type before printing it *[checked 19 Nov: the account shows a Free ThingSpeak licence, expiring 12 Nov 2027]* | AM |
| 20: the AI log gave a small subset of the output | Verbatim prompts and raw outputs, with verification and adaptation columns | Everyone; AM compiles, LA reviews |

Keep what was praised: remote checking instead of site visits (4), the ethics sign-off (16) and the EIA's affordability row (17). The annotated feedback file came to AM's SurreyLearn account; it is not in this repository.

## The energy question: can the tracker pay for itself?

Yes over a year, but only if the electronics sleep. In December the whole tracker may average at most 29 mW, and the current firmware is modelled at about 130 mW for the whole tracker. *[19 Nov afternoon, with the servo switched off between moves: 2.87 Wh/day, about 120 mW.]*

*Chart: monthly gross and net gain from `power_budget.py` (19 Nov 2026, PVGIS 5.3 monthly means, Guildford; modelled, not measured). The tables are in [docs/energy/power_budget.md](../energy/power_budget.md).*

The model counts everything the tracker draws (P_use, agreed 17 Nov) and compares the tracked panel with an identical fixed panel at the same tilt. With the current firmware the radio is off between uploads but the board stays awake, so the tracker would show a net loss in January, February, October, November and December. That covers all of our lab dates. Waking every 15 s from light sleep and uploading every 15 min gives a gain in every month (+26 % a year), but only 3 % under the 22.5 mW target; hourly uploads (about 14 mW) leave more headroom. These are datasheet-based estimates; the 26 Nov bench session replaces them with measurements.

- **Firmware:** light sleep between samples, never a `delay()` loop; AM changes the data-link firmware and the control loop fits the same wake cycle. Target: 22.5 mW or less averaged over 24 h at INA226 #3 (the December budget with a 30 % margin). Lamp runs verify the mechanism (F1–F3); the yield argument rests on PVGIS plus any outdoor or window runs.
- **INA226 #3:** in the 5 V feed from the power manager to the tracker, not on the battery line, which also carries charging current.
- **Fixed reference:** at the tracker's tilt. Against a flat panel most of the "gain" is tilt (November: flat 4.1, tilted 8.6, tracked 10.1 Wh/day).
- **How to present it:** our modelled +29 % a year is above the 20–25 % published for inclined-axis trackers in the British Isles ([Huld et al.](https://www.pv-tech.org/wp-content/uploads/legacy-publication-pdfs/7d07c0b79c-performance-of-singleaxis-tracking-photovoltaic-systems-in-europe.pdf); [Huld et al. 2010](https://doi.org/10.1002/pip.948)), so call it an idealised upper bound. Conventional trackers spend 2–3 % of their extra energy on themselves ([Mousazadeh et al. 2009](https://doi.org/10.1016/j.rser.2009.01.022)); ours with the current firmware would spend most of it, which is why the measured P_use answers comment 19.

## Lecture tools to show visibly

The 12 Nov feedback says the best briefs used MoSCoW, decision matrices and SMART objectives, and that many groups showed little of the lecture tools. Neither sample report uses SMART objectives, matrices, statistics or an AI log, so these are where we can beat them.

| Tool (lecture) | Show it in the report as | Owner | Have now |
| --- | --- | --- | --- |
| SMART objectives (Week 4) | 6–8 group objectives plus subsystem ones; a results table against each in the conclusions | LA + each lead | Data link only (O1–O7) |
| MoSCoW (Week 2b) | Table of every requirement with class and reason, including the "Won't" list (e.g. dual axis) | AA | Done in October, not written up |
| Weighted decision matrix + spider plot + sensitivity (Week 2b) | One per major choice: sensor layout, motor and drive, power architecture, controller, single vs dual axis | AA, AO, CO, CH, LA | Data host only (AM; script reusable as a template) |
| Risk register and 5×5 matrix (Week 2b) | Register with owner, response type and outcome; before/after matrix compared with the brief's | AA + all | Four risks in the brief |
| FMEA (Week 1) | One table per subsystem, before and after mitigation | Each lead | Data link only |
| Ten-step SE process; verification vs validation (Week 2a) | One-row-per-step table; separate verification (built right) and validation (net gain, customer view) tables | LA | Not yet |
| Technology readiness levels (Week 3) | TRL at start, now and at the demo, with evidence | LA | Not yet |
| Digital twin (Weeks 1 and 3) | PVGIS model + device simulator as the tracker's twin: predicted vs measured; plus logged AI customer personas | AM + CO; everyone for personas | Model and simulator exist |
| Circular economy and e-waste (Week 3) | Five-row circularity table (durability, repairability, upgradability, efficiency, recoverability) | AO | One WEEE sentence |
| Design of experiments and hypothesis testing (Week 5) | Repeated tracked vs fixed runs (at least 5 pairs) with a paired t-test; a small 2×2 experiment on upload interval × sleep mode | CO + AM | Not yet |
| ISO 9001 PDCA, Industry 4.0/5.0 (Week 5) | One plan-do-check-act loop from testing; a four-row Industry 4.0 mapping | Any lead; AM | One PDCA example in the repo |
| Cost-effectiveness with uncertainty (Week 2a) | Cost per extra Wh as a range, compared with adding a second fixed panel | AA + CO | Not yet |

## AI use: what the module requires

AI is allowed, but every use must be declared, verified and logged in full. Presenting AI output as your own thought counts as academic misconduct, and the slides warn against unverified autonomous agents (Week 1 slides p18).

**Required in the final report (Week 1 slides p18–24, p39):**

1. A declaration of AI use, naming the roles AI played.
2. Each tool acknowledged (model, developer, date accessed) and listed in the references.
3. A description of how it was used.
4. An "AI Interaction Log" appendix with the exact prompts, the raw outputs, how technical claims were checked against datasheets or papers, and what the team changed or rejected.

**Encouraged:** AI as a "digital twin" (expert or customer personas to stress-test the design), a work-breakdown and critical-path check of the Gantt, an FMEA draft, a review of test protocols, and SDG mapping. **Recommended tools:** Microsoft Copilot or myai.surrey.ac.uk. **Never:** share credentials with AI tools.

**How it will be policed:** the 12 Nov SurreyLearn news says almost no brief referenced its AI use, and that this "will be monitored strictly" at the final report. Our brief's AI appendix was flagged for giving "a subset of a subset" of the outputs (comment 20).

**Our gap:** the Notion AI-usage database holds 5 entries, so most of our AI use so far is unlogged.

- [ ] Everyone: add your past AI sessions to the Notion AI-usage database by 26 Nov (date, tool, exact prompt, raw output, how you checked it, what you changed)
- [ ] Everyone: from now on, save each prompt and its raw output when you use it, in a shared folder
- [ ] AM: compile the AI appendix and declaration from everyone's entries; LA reviews. Everyone: cite the AI tool in the text wherever it shaped a paragraph (comment 2) and give a critical view of your AI use in your reflection. The folder takes no links, so export full AI transcripts to PDF for the appendix
- [ ] Optional, worth marks: two or three customer "digital twin" personas (e.g. a homeowner, an off-grid user) to test our requirements, with the prompts logged

## Lab plan: 26 Nov, 3 Dec, 10 Dec

The parts arrive the week of 23 Nov, so every hardware result in the report has to come from the 26 Nov and 3 Dec labs. The 10 Dec lab falls after the results freeze and is for checking and assembling the report. *[The parts arrived early, on 19 Nov, and an integration test was added on Wed 25 Nov: see the [bench runbook](../BENCH_RUNBOOK_26NOV.md).]*

| Date | Milestone |
| --- | --- |
| Thu 19 Nov | Lab: decisions, V&V sheet, questions for Dr Stolojan |
| Thu 26 Nov | Lab: first power-up, first real upload, 24 h run started |
| Thu 3 Dec | Lab: tracked vs fixed runs, tracker draw measured |
| Fri 4 Dec | Drafts in |
| Mon 7 Dec | Peer review back |
| Tue 8 Dec | Results freeze |
| Thu 10 Dec | Lab: final read-through and PDF |
| Fri 11 Dec | Every member uploads, well before 4 pm |
| Mon 25 Jan | Presentation and live demo |

**Thu 19 Nov**

- [ ] Group: decide the panel tilt, control lead, page budget and template; set up the V&V sheet; agree the AI back-fill; put the questions to Dr Stolojan
- [ ] AM: demo the dashboard; share the marker's annotated feedback file; table DMP v2; share the decision-matrix script as a template

**Thu 26 Nov**

- [ ] Group: parts checked in, first power-up, one matrix per lead, group objectives drafted, Gantt rebuilt, AI log back-filled
- [ ] AM: board flashed, first real upload, 24 h run started (bench runbook), first OneDrive backup, DMP v2 minuted

**Thu 3 Dec**

- [ ] Group: first tracked vs fixed runs, tracker draw measured in each mode, risk register and MoSCoW tables done
- [ ] AM: hotspot-off outage test, data-link energy measured, first real nightly analysis

**4–11 Dec:** drafts Fri 4, peer review back Mon 7, results freeze Tue 8, final read-through and PDF at the 10 Dec lab, every member uploads Fri 11 well before 4 pm.

## Presentation and live demo (25 Jan)

The presentation is worth 25 % and must include a live demonstration of the final product, in person on Mon 25 Jan 2027 between 9 am and about 2 pm. Its information page on SurreyLearn is broken, so slot length, slide limits and whether everyone speaks are unknown until Dr Stolojan confirms.

- **Group half:** structure, visual aids and how well the speakers link together (module descriptor). Peer marking adjusts it.
- **Individual half:** voice, audibility, pace and timing.
- **The demo should show every client requirement live:** tracking a lamp, both panels measured, a timed upload arriving, and the phone dashboard showing the analysis.
- **Advantage:** the demo is six weeks after the report deadline. Keep logging through January and show a multi-week net-gain chart that the report could not.

- [ ] LA: ask for the presentation format at the 26 Nov lab
- [ ] LA: storyboard by 18 Dec (problem, aim, design, results, energy verdict, lessons), slides by 15 Jan
- [ ] Everyone: two timed rehearsals, weeks of 11 and 18 Jan
- [ ] CO, AO, CH: hardware demo script with roles; book a lamp and stand
- [ ] AM: live data on phones over a hotspot, with a recorded fallback clip and the dashboard's offline demo mode in case the room's Wi-Fi fails

## Decisions the group must make now

These decisions block the lab work or the report. Settle them at the lab and record them in the minutes, because minuted decisions are themselves marked.

1. **Fixed reference panel at the tracker's tilt, on the same frame.** Against a flat panel the headline gain is mostly tilt.
2. **Control lead.** Control has no first owner. AM proposes to take the control firmware, because the sampler and sleep logic sit in his data-link code, with AO as second. *[19 Nov chat: CO took solar sensing and tracking control; AM is second on firmware.]*
3. **Where P_use is measured.** INA226 #3 goes in the 5 V feed to the tracker; field 7 ("V_bat") then reports the feed voltage. *[Field 7 is now named V_supply.]*
4. **Sleep policy.** Light sleep between samples; AM changes the interface rule "call once a second" to match a 15 s wake.
5. **Page budget and template:** about 1.8 pages of technical section each; a named editor and a second reader, who also set the peer-review pairs and the contributions table.
6. **Public material.** The submitted DMP and ethics form said the data stay within the group, unpublished, on University drives, but the build uses public-read ThingSpeak channels and a public GitHub repo. Agree the change, minute it and update the ethics answer. The dashboard is served by GitHub Pages from that repo (a private repo needs a paid or education plan), so keep draft report text out of it. The public Notion site also shows our names and minutes: agree what stays public. *[19 Nov evening: AM chose to put the drafts and this playbook in the public repo anyway, so the team has everything in one place. Turnitin may match public draft text against the final report; the group can still move these files to a private repo.]*
7. **Late parts.** If parts are late or fail on 26 Nov, the minimum result is the model and the simulation, clearly labelled, plus lamp tests of the mechanism.
8. **Light source and validation.** Lamp runs verify the mechanism (F1–F3); the yield argument uses PVGIS plus any outdoor or window runs. Book a lamp and stand.
9. **Owners for the loose ends:** EIA review and re-dating (AO), ethics form update (CH, with AM for data storage), BOM against £125 (AA), how each panel is held at its operating point (CO), and the unassigned PV and PCB areas.
10. **Questions for Dr Stolojan (who asks?):**
    1. Is there a marking scheme for the report and the presentation?
    2. Do the cover, abstract, contents and references count toward the 25 pages? Is there a font or margin rule?
    3. Must every member upload the report?
    4. Which appendices are mandatory?
    5. What is the presentation format: minutes per group, does everyone speak, are there questions?
    6. Is a referencing style expected?
    7. Does peer marking apply to the report, the presentation or both, and when does it open?

## Sources

SurreyLearn pages need a University login. Everything was read on 19 Nov 2026.

- [EEE3035 SurreyLearn home](https://surreylearn.surrey.ac.uk/d2l/home/300201): news of 12 Nov "Project briefs marks and feedback"; Final Project Report and Presentation "Important Information" pages; Week 1 slides (assessment, AI policy, minutes format); Week 2a/2b, 3, 4 (SMART objectives, peer marking) and 5 slides; client brief overview 2026-27; sample final reports 1 and 2; assignment folders
- Group 8 design-brief feedback: marks breakdown and 21 margin comments by Dr Stolojan (12 Nov), on AM's SurreyLearn account
- [Group 8 Notion workspace](https://fishy-canoe-485.notion.site/EPS-YR3SEM1-3ebf4629be36801c8fabcd4547e0255c): minutes, action log, AI usage, testing-phase allocation
- [This repository](https://github.com/amr-coding1/eee3035-active-solar-tracker): energy model ([docs/energy](../energy/)), [report plan](../REPORT_PLAN_AM.md), [DMP v2](../DMP_v2.md), [V&V results](../VV_RESULTS.md)
- Power budget and break-even model, 19 Nov 2026 (model only, not yet measured)
- T. Huld, M. Šúri and T. Cebecauer, ["Performance of single-axis tracking photovoltaic systems in Europe"](https://www.pv-tech.org/wp-content/uploads/legacy-publication-pdfs/7d07c0b79c-performance-of-singleaxis-tracking-photovoltaic-systems-in-europe.pdf), Photovoltaics International
- T. Huld et al., ["Analysis of one-axis tracking strategies for PV systems in Europe"](https://doi.org/10.1002/pip.948), Prog. Photovolt. 18(3), 2010
- H. Mousazadeh et al., ["A review of principle and sun-tracking methods for maximizing solar systems output"](https://doi.org/10.1016/j.rser.2009.01.022), Renew. Sustain. Energy Rev. 13(8), 2009
- [Arduino Nano ESP32 cheat sheet](https://docs.arduino.cc/tutorials/nano-esp32/cheat-sheet) (power input ranges)

Drafted with AI (Claude, Anthropic) for AM as a proposal for the group. The figures come from the sources above; AM confirms them before they go in the report. Logged in AM's AI appendix.
