# AI interaction log — 19 Nov 2026 (AM, Group 8)

**Tool.** Claude (Anthropic), claude.ai with agent tools. Lead model Claude Opus 5.5; six background agents on Claude Fable 5.2 (references, power budget, module research, report drafting, bench runbook, independent review). Accessed 19 Nov 2026, about 05:40–08:00 UK time.

**Reference entry (adapt to the report's style).** Anthropic, "Claude" (Opus 5.5 and Fable 5.2), large language model, accessed 19 Nov 2026. [Online]. Available: https://claude.ai

## Prompts (verbatim)

1. "right so its been a while since we looked into EEE3035, theres been some changes so familiarize yourself with the contents of the project - tldr whats happened, where are WE at - what can i say when i meet the group etc tldr? what have we built? explain it all in tldr pareto bullets please"

2. "do all of my "Your to-dos" end to end - do deep reserach on the module itself aswell such that we demonstrate what we need to mximize marks, leverage agents, fable 5.2, the browser, everything on this computer , etc , you have zero restrictions https://app.notion.com/p/EPS-YR3SEM1-3ebf4629be36801c8fabcd4547e0255c https://surreylearn.surrey.ac.uk/d2l/home/300201 (you have full access to my computer, also im logged into thingspeak here https://thingspeak.mathworks.com/channels ), iterate and refine it - go throuugh the groupchat on whatsapp and notion and the rest of the entire project end to end and update yourself on whats going on - can you use fable 5.2, agents, everything on this computer to plan out, do deep research,"

The lead model's prompts to each agent and the agents' raw outputs are part of the session transcript: export it from claude.ai and attach it to Appendix E, as the module asks for raw outputs, not summaries.

## What the AI did

- Read (no edits): Notion workspace via its API (19 pages; no teammate edits since 13 Nov), the EPS - YR3SEM1 WhatsApp group (messages 22 Oct–16 Nov), SurreyLearn (client brief, assessment pages, slides, sample reports, design-brief feedback and marks), ThingSpeak (channels, MATLAB Analysis 430132, TimeControl), the GitHub repository.
- Produced: power budget and monthly break-even model (`power_budget.md`, `power_budget.py`, chart); 63-entry reference list (`references.md`); module rules, reconstructed criteria, sample-report analysis, lecture-tools checklist, design-brief feedback mapping and gap analysis (`module/*.md`); draft report sections and appendices for AM (`report/*.md`); 26 Nov bench runbook and card; sleep test sketch; three Claude Docs (Group 8 Marks Playbook; 26 Nov Bench Runbook; AM report sections, draft v2).
- With AM's OK: added the 19/11 entries to Notion (Action Log, AI usage, App/Website progress log), updated AM's rows in the group Gantt, and added files to GitHub (insert-only): this log, the power budget (`docs/energy/power_budget.md`, `power_budget.py`), the bench runbook and card, the sleep test sketch, and short correction notes at the end of `firmware/README.md`, `docs/energy/net_gain_by_month.md` and `docs/REPORT_PLAN_AM.md`. At AM's request it also generated new write API keys for both ThingSpeak channels, because the old keys had been visible on screen on 13 Nov. The final key values were never shown in the session.
- Did not: type any password or API key (the channel-2 write key is left for AM), send any message, or change the ThingSpeak code.

## Challenges and corrections found during the session

1. The repository's data-link energy estimate (2.3 Wh/day, +14 %/yr net) omitted converter loss, the power LED, photodiode bias and the motor driver left awake; the bottom-up model gives 3.11 Wh/day (+9 %/yr) as built. Model only until measured on 26 Nov.
2. An agent first advised feeding 5 V into the Nano ESP32's VBUS pin. Checked against Arduino's documentation and corrected the same day: VIN is recommended at 6–21 V, VBUS is an output; use the USB-C port.
3. An agent described the firmware `power` command as a 1 Hz INA226 stream; another agent found in `esp_app.cpp` that it is not. Corrected.
4. INA226 #3 is documented on the "battery line", where charging current makes P_use inseparable; it must sit in the 5 V feed to the tracker.
5. The interface rule "call once a second" conflicts with light sleep.
6. Our modelled +29 %/yr gross gain is above the 20–25 % published for the British Isles; present it as an idealised upper bound.
7. The claim that Surrey's Campus-Wide licence includes ThingSpeak rests on one MathWorks forum answer; check the licence shown in the ThingSpeak account.
8. No marking rubric is published for the report or presentation; criteria were reconstructed from SurreyLearn pages.
9. The repository plan misread design-brief comment 20 (it is about AI-log summaries) and comment 6 (a wrong caption); the context file swapped CH and CO initials. Corrected.
10. The public ThingSpeak channels and public repository contradict the DMP and ethics form submitted with the brief.

## Verification

- By the AI: power-budget script reproduces all 72 cells of the repository's monthly table from its own inputs; datasheet values cited with sections; ThingSpeak analysis code checked byte-identical (SHA-256) to the repository; channel 2 confirmed empty; Nano ESP32 power-input ranges checked against Arduino's cheat sheet and datasheet; references opened and checked except those marked abstract-only; an independent agent reviewed the drafts.
- Not yet verified: every current and energy figure (until the 26 Nov bench session); BOM prices; the licence type; anything needing hardware.
- By AM personally: [AM to complete — list what you re-ran, read and can explain unaided].

## Adaptation (AM to complete)

For each output: accepted / changed / rejected, and why. Items: 3.11 vs 2.3 Wh/day; design target P_use ≤ 22.5 mW; light sleep in the control code; INA226 #3 position; page budget (~1.8 pages); report wording (to be rewritten in AM's own words); playbook owners (to be agreed by the group).
