# AI interaction log — 19 Nov 2026, afternoon (AM, Group 8)

**Tool.** Claude (Anthropic), claude.ai with agent tools. Lead model Claude Opus 5.5; three background agents on Claude Fable 5.2 (DS-R001 servo research; dashboard, analysis and docs update; independent firmware review). Accessed 19 Nov 2026, about 14:10–18:00 UK time.

**Reference entry (adapt to the report's style).** Anthropic, "Claude" (Opus 5.5 and Fable 5.2), large language model, accessed 19 Nov 2026. [Online]. Available: https://claude.ai

## Prompt (verbatim, as pasted by AM; it includes messages forwarded from LA)

"hey tldr pareto bullets how do i explain everything to the team update yourself - so is the entire stuff that built so far made for the nano esp 32? check the BOM (doesnt it contain everything we're usingincluding hardware etc?) also i go this message on whatsapp: https://app.notion.com/p/EPS-YR3SEM1-3ebf4629be36801c8fabcd4547e0255c @all please go to the notion and then -> active solar tracker -> testing phase -> block diagram. understand what each part does, there's a description below the general block diagram, then below it there's more in depth parts of each subsystem. Pick the one you want, caleb is already doing mechanical design + actuators (ill take ovr the app / dashbaord / websie and the firmware porbably) another thing i just got: why do we need to monitor the charge at all? what graph shows that? we are using a servo (DS-R001) [LA's AI answer about field 7 and the DS-R001, pasted in full] ingest this and make changes to the dashboard you initially used some assumptions (worm gear vs servo) etc. - we can now say that it is a servo (DS-R001) + some tests (ALSO GOT THIS FROM LEM:): Check the output below. If the repo defines the following data alongside the formatting for proper dashboard communication, draft a ".h + .cpp" file dedicated for data sending (name it appriorately i.e. "webhook", "dashboard", "transmission". The idea is to run multiple files simultaneously via VSC i.e. sensing.cpp, transmission.cpp, servo.cpp (I think that is it) with functions to be pulled from a main.cpp (whatever suffix) file. [LA's AI answer about which data the repo produces itself, pasted in full] This is already a prompt btw - i'll handoff to you since your window/claude has more context on the repo Abdul - we are planning to test next Wednesday so can you do transmission.cpp before then? go through the whastapp chat, check all the messages on EPS - YR3SEM1, - go through everything on the notion aswell, whats going on? what did we miss? whats the groups porgression been like / whats the status? what do we need to do? think to lem's messages - do deep reserach on the module itself aswell such that we demonstrate what we need to mximize marks, leverage agents, fable 5.2, the browser, everything on this computer , etc , you have zero restrictions [links to Notion, SurreyLearn, ThingSpeak] iterate and refine it - go throuugh the groupchat on whatsapp and notion and the rest of the entire project end to end and update yourself on whats going on - can you use fable 5.2, agents, everything on this computer to plan out, do deep research, look at Lem's instructions, the notion , the whatsapp, and anythng relevant to how we will build our part? then build it end to end and test it please? or are missing something?"

The two pasted LA answers are in the WhatsApp chat (LA to AM, 19 Nov 12:10 and 12:40). The lead model's prompts to each agent and the agents' raw outputs are in the session transcript: export it from claude.ai for Appendix E (the module asks for raw outputs, not summaries).

## What the AI did

- Read (no edits): EPS - YR3SEM1 WhatsApp group (13–19 Nov) and AM's chat with LA; Notion (TESTING PHASE incl. block diagram and subsystem breakdowns, Minutes 19/11, Active Solar Tracker incl. the BOM); SurreyLearn home (no new announcements since 12 Nov); the GitHub repository; the earlier session notes and the Marks Playbook.
- Research agent (Fable 5.2): DS-R001 identity and specs from listings (DSpower part, very probably DFRobot SER0051), holding behaviour, Nano ESP32 pin map, LEDC on core 3.x, power-gating circuit.
- Built and tested (branch `servo-transmission-2026-11-19`): the tracker firmware `firmware/tracker` with `transmission.h/.cpp` (LA's request), `sensing`, `servo`, `control`, `main.cpp`, `config.h`; INA226/INA219 auto-detection; optional second consumption branch; portable, unit-tested logic for the sun estimate, servo sequence and tracking decisions; a closed-loop digital-twin test; PlatformIO project; field 7 renamed V_supply across firmware, dashboard, analysis and contract; servo wording for faults 7/8 and MOV; energy model with the servo; bench runbook for 25–26 Nov.
- Did not: type any password or API key, send any message, change the ThingSpeak MATLAB code, or edit teammates' Notion text.

## Challenges and corrections found during the session

1. The Notion block diagram (servo, power bank, four-channel INA219 board, no clock) does not match the BOM ordered on 17 Nov (worm gear + DRV8833, Waveshare manager + 18650, 3× INA226, DS3231). The servo, power bank and INA219 board are not on the BOM. The firmware was written to work with either sensor chip; the group must still say which parts are on the bench.
2. DS-R001 listings disagree (standby ≤ 10, ≤ 20 or ≤ 40 mA; stall torque 6, 8 or 9 kgf·cm; 180° or 300°). LA's figures "≤ 20 mA" and "1.25 kgf·cm" were not found on any page the agent could open. Treated as ranges until measured (M6–M8).
3. The servo has plastic spur gears and a slip clutch, so it probably does not hold the panel unpowered; switching it off between moves (needed for energy) must be tested first (M9).
4. A cosine-fit sun estimate is very sensitive to sensor mismatch at the ends of the arc (3 % mismatch gave up to 14° error in simulation); after flat-field calibration (1 %) it stays under 5°. Hence the `sun cal` commands.
5. Many power banks switch off below 50–100 mA, which clashes with light sleep (M13).
6. With the board awake the modelled consumption is 2.87 Wh/day, above the winter tracking gain: the sleep change is still needed for the energy result.

## Independent review of the firmware (agent on Claude Fable 5.2, not involved in writing it)

Eleven findings, ranked; what was done:

1. HIGH: diffuse light gave random "valid" directions (44 % of tries), so the panel would hunt about 350 times an hour. Fixed: a direction needs a beam above 30 mV and a contrast of 0.15 against the ambient level; a short gap no longer restarts the smoothing.
2. HIGH: the stall guard could not trip at the shipped 30°/s (moves are shorter than its 2.5 s window). Fixed: it now counts the energy above the pre-move level over each move and 1.5 s after, against 1.5 J + 0.06 J per degree; tested with the shipped settings for 5–90° jams.
3. HIGH (bench safety): the firmware started tracking at boot. Fixed: the servo stays off after every boot until `servo <deg>` or `servo auto` (`kStartTracking`).
4. MEDIUM/HIGH: stopping the PWM could cut the last pulse short. Fixed: duty 0, wait 25 ms, then release the pin; the supply stays on for 40 ms after the pulses stop.
5. MEDIUM: the trim could push past the ±50° limit. Fixed: the limit applies after the trim; the trim is limited to ±10°.
6. MEDIUM: a dead or stuck photodiode gave wrong "valid" angles. Fixed: the fit flags a poor match (misfit, negative ambient, direction far from the brightest sensor); fault 5 after 30 s. The estimator was rewritten as a fit over every direction, which also handles a narrower response (`kSunResponsePower`).
7. MEDIUM: the saved angle went stale after a stop or a reset mid-move. Fixed: a flag in flash says whether the last move ended cleanly; if not, the first move goes straight to its target.
8. MEDIUM: the wiring drawing charged a 1000 µF capacitor through the hard-switched MOSFET (inrush, possible reset). Fixed in the README: capacitor before the switch, slowed gate.
9. LOW/MEDIUM: a race when summing two consumption branches. Fixed: published once under one lock; the second branch no longer adds the calibration offset twice.
10. LOW: an INA260 or other TI die could be taken for an INA226. Fixed (die id 0x226x only). Not changed: `ina219_overflow()` stays unused (the firmware computes current from the shunt register), and a failed ID read on an INA226 resets it once (harmless, it is set up again).
11. LOW: `servo test` timing after 24.8 days, silent manual commands during a latched fault, a late loop making the ramp jump, STW never reported with the pot fitted. All fixed.

After the fixes: 215 host tests pass (4 new), both sketches compile with no warnings.

## Verification

- By the AI: 215 host unit tests (ASan/UBSan) incl. 20 new and a closed-loop twin; TSan; both sketches compile with 0 warnings in our files on core 3.3.12, the tracker also on 4.0.0 and with PlatformIO; end-to-end suite 129/129 with the new core; dashboard 32 + 9 tests; Python 110 tests; the independent review above.
- Not yet verified: anything on hardware (ADC readings, servo PWM and switch, I2C drivers, Wi-Fi/TLS, currents) until the 25–26 Nov bench session.
- By AM personally: [AM to complete: what you re-ran, read and can explain unaided].

## Adaptation (AM to complete)

For each output: accepted / changed / rejected, and why. Items: module split (transmission/sensing/servo/control); starter code for CO and CH (they own and may replace it); INA219 support; servo power switch; V_supply rename; deadband 5°, range ±45°, stow −45°; runbook W1–W8.
