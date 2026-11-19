# AI interaction log — 19 Nov 2026, evening (AM, Group 8)

**Tool.** Claude (Anthropic), claude.ai with agent tools, model Claude Opus 5.5. Same session as the [afternoon entry](AI_LOG_ENTRY_2026-11-19b.md), continued about 15:30–16:45 UTC (UK time is UTC in November).

**Reference entry (adapt to the report's style).** Anthropic, "Claude" (Opus 5.5), large language model, accessed 19 Nov 2026. [Online]. Available: https://claude.ai

## Prompts (verbatim)

1. "how can i get you to push it to github? i want YOU to do it"
2. "okay so again, what have we done? is everything ready end to end? is it all on github for lem and the team to view? i need everything all in one place (github) for the team to access and view seamlessly"
3. The AI asked one multiple-choice question: the repository is public, so anyone can read and copy it and the plagiarism checker could match the final report against draft text found there; what should happen to the report drafts, the design-brief marks and the SurreyLearn module notes? Options: make the repo private first (recommended), leave them off GitHub, or put them in the public repo. AM chose **"Put them in the public repo"**.

## What the AI did

- **Push.** The first request for GitHub push access (afternoon) had been refused by the permission system; the second, after prompt 1, was granted. Before pushing, the AI scanned the 8 new commits for keys, passwords and secrets files (none; `secrets.h` stays git-ignored) and checked the deleted lines (a regenerated sample CSV and the INA226-only driver replaced by the two-chip driver). It fast-forwarded `main` from 243b974 to e49c1be at 15:34 UTC (no force push). GitHub Pages redeployed successfully and the live dashboard showed the servo wording.
- **A ninth commit.** a186541 (15:49 UTC, "Runbook: host test count is 215", one line) was pushed from the same session by a helper process started before the conversation was summarised. The AI found it afterwards, checked the diff and reported it to AM.
- **One place.** The AI moved the Claude Docs into this repository: the team status ([docs/team/STATUS_2026-11-19.md](team/STATUS_2026-11-19.md)), the marks playbook ([docs/team/MARKS_PLAYBOOK.md](team/MARKS_PLAYBOOK.md)) and AM's report sections draft v2 ([docs/report/AM_REPORT_SECTIONS_DRAFT_v2.md](report/AM_REPORT_SECTIONS_DRAFT_v2.md)). Facts that changed after they were written are marked as notes in brackets, not rewritten. It wrote [START_HERE.md](../START_HERE.md) and [docs/README.md](README.md), linked them from README.md and the project hub (index.html), and corrected stale test counts (211 → 215).
- **Not copied.** The morning Claude Doc "26 Nov Bench Runbook" (worm-gear tests) is replaced by [BENCH_RUNBOOK_26NOV.md](BENCH_RUNBOOK_26NOV.md) (servo tests M6–M9). AM's appendices draft (.docx) and the 19 Nov evidence pack (.zip) were no longer in the AI's workspace. Its request to read AM's Downloads folder for them was refused by the permission system, and it did not try another way.
- **Did not:** type any key or password, send any message, change ThingSpeak or its MATLAB code, or edit Notion.

## How the output was checked

- Every relative link in the new and changed Markdown files and every GitHub link in index.html was checked against the files in the repository by a script.
- The team status, playbook and report draft were copied from the Claude Docs' own text (read through the docs tool), not retyped from memory.
- After the push: GitHub `main` hash, the Pages deployment run, and the live hub page.

## By AM personally

*[AM: what you read, checked or changed yourself.]*

## Adaptation

*[AM: what you kept, changed or rejected, and why. Note the decision to publish the drafts and marks, and whether the group agreed it.]*
