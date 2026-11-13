# AI usage record — AM, 13 Nov 2026 (for the group AI log / report appendix)

Format follows the Week 1 "Acknowledge, Describe, Evidence" guidance and the AI
record-keeping slide (tool and session, exact prompt, verification, adaptation,
declaration). Add this as a row in the Notion "AI usage" database and paste the
full transcript into the report appendix.

## 1. Acknowledge
- Tool: Claude (Anthropic), web app with agent tools. Lead model Claude Opus 5.5; three
  build agents and one reviewer ran on Claude Fable 5.2. Session date: 13 Nov 2026.
  Earlier sessions the same day produced the first version (contract v1.4); their
  transcripts must be exported and logged too.
- Reference entry (Surrey style): Anthropic. (2026). *Claude* (Opus 5.5 and Fable 5.2,
  13 Nov 2026 version) [Large language model]. Used to plan, build and test the EEE3035
  Group 8 data link and dashboard. https://claude.ai
- Note: Surrey's guidance recommends Microsoft Copilot or myai.surrey.ac.uk and advises
  against unverified autonomous agents. This use is declared in full; every output was
  verified (section 3), and AM remains accountable for all of it.

## 2. Describe (what the AI did)
1. Read the group WhatsApp, Notion, SurreyLearn (client brief, Week 1-5 slides, sample
   reports, Vlad's 12 Nov feedback) and summarised status and gaps.
2. Built the data contract v1.5, rebuilt the ESP32 data-link firmware, MATLAB nightly job
   and charts, a ThingSpeak mock server and end-to-end tests, and extended the dashboard
   (status timeline, MATLAB chart embeds, de-duplication).
3. Set up the ThingSpeak channels (AM's account, with AM's approval at each step) and
   imported 7 days of SIMULATED data (marked s=1) to test the real cloud path.
4. Produced the data-host decision matrix (sensitivity analysis, spider plot), the
   PVGIS energy budget, SMART objectives, FMEA, DMP v2 and this record.

## 3. Evidence (verification and human control)
- Code (results of 13 Nov 2026, all passing): firmware 195 unit tests under ASan/UBSan,
  a ThreadSanitizer test and a board compile with 0 warnings; analysis 40 pytest plus
  Octave runs of all seven .m files against the Python reference; dashboard 32 unit
  tests and 9 tests in real Chrome (incl. axe-core WCAG checks); mock/simulator 69
  pytest; end-to-end 129 of 129 checks with the real firmware core, the real nightly
  script in Octave and the real dashboard. An independent AI reviewer (a separate
  agent that had not written the code) checked the whole tree: no high-severity
  defect, 3 medium and 7 low findings, all fixed and re-tested (list in
  docs/VV_RESULTS.md). Analysis 41 pytest after the fixes.
- A defect found by the tests, not by the AI that wrote the code: the nightly job
  rewrote only 7 days while the device buffer holds 8, so a day back-filled after a
  long outage stayed wrong for ever (E2E S3). Fixed in contract v1.5 (9 days).
- Facts: ThingSpeak limits checked against MathWorks pages; PVGIS figures fetched from
  the JRC API (stored JSON in docs/energy); module requirements from the client brief.
- Real system: dashboard read the real ThingSpeak channels; MATLAB ran on ThingSpeak
  (dry run of the nightly job: rows identical to the Python reference); every MATLAB
  script on ThingSpeak was checked byte-for-byte against the repository copy (SHA-256).
- Not verified yet (needs the hardware): Wi-Fi, TLS, RTC, sensors, current draw.
- To do by AM before the report: read and be able to explain every part (contract,
  upload state machine, de-duplication, nightly job), re-run the tests, and record any
  change made or rejected (adaptation log below).

## 4. Prompt record (verbatim, first prompt of this session)
> right so its been a while since we looked into EEE3035 - go through the whastapp chat,
> check all the messages on EPS - YR3SEM1, (outlook is currently down) - go through
> everything on the notion aswell, whats going on? what did we miss? whats the groups
> porgression been like / whats the status? what do we need to do? think to lem's messages
> - do deep reserach on the module itself aswell such that we demonstrate what we need to
> mximize marks, leverage agents, fable 5.2, the browser, everything on this computer , etc
> , you have zero restrictions [Notion and SurreyLearn links] (you have full access to my
> computer, also im logged into thingspeak here [ThingSpeak link] ), iterate and refine it
> - go throuugh the groupchat on whatsapp and notion and the rest of the entire project end
> to end and update yourself on whats going on - can you use fable 5.2, agents, everything
> on this computer to plan out, do deep research, look at Lem's instructions, the notion ,
> the whatsapp, and anythng relevant to how we will build our part? then build it end to
> end and test it please? or are missing something?

Follow-up decisions given by AM in the session: set up ThingSpeak fully (channels,
nightly MATLAB, public MATLAB charts); rebuild the lost code; make both channels
public-read.

## 5. Adaptation log (AM to complete)
| Date | AI suggestion | Accepted / changed / rejected | Why |
|---|---|---|---|
| 13 Nov | ThingSpeak over Supabase (matrix 4.55 vs 3.80) | | |
| 13 Nov | Public-read channels | Accepted by AM | fewer keys; data non-personal |
| 13 Nov | Fixed reference panel at the tracker's tilt, not flat | | discuss with group |
| 13 Nov | Nightly window 7 -> 9 days (found by E2E S3) | Accepted | buffer is 8 days |
| 13 Nov | TLS: core CA bundle instead of one pinned root | | pinned PEM could not be fetched safely through the build machine's proxy |
| 13 Nov | Poison-batch drop limit (max 3 records without a success) | | a server that rejects all would otherwise empty the buffer |

## 6. Declaration (for the report)
"Generative AI (Anthropic Claude, Opus 5.5 with Fable 5.2 agents, 13 Nov 2026) was used
to research, generate and test parts of the data-link firmware, MATLAB analysis,
dashboard and supporting analyses described in Section X. All AI output was checked
by the author through the tests and sources listed in Appendix Y, and the author is
responsible for the final content. Full prompts and responses are in Appendix Y."
