# Data Management Plan — v2 (13 Nov 2026)

Replaces the 27 Oct draft in Notion (Appendix 3 of the design brief). Change:
data host Supabase -> ThingSpeak (decision matrix, docs/decisions/data_host_matrix.md),
channels public-read, SD card replaced by on-board flash. Draft by AM, AI-assisted
(declared in the AI log). To be agreed at the 19 Nov meeting.

PROJECT TITLE: Active Solar Tracker — Group 8, EEE3035 (2026/27)

## 1. Data collection
**What data?** Once a minute (UTC): tracked-panel, fixed-panel and tracker-own-use power
(mW), panel angle and sun angle (°), panel temperature (°C, if fitted), supply voltage (V),
sample sequence number, and a status string (state, test-run number, panel letter, fault
code, simulated flag). Derived nightly by MATLAB: daily energy (Wh), gross and net gain (%),
mean pointing error (°), completeness (%). Also calibration records, V&V test logs (F1-F12),
photos/videos, CAD, code, minutes and the AI log.
Volume: ~1,440 entries/day; ~0.2 MB/day as CSV; ~15 MB over the test period; media/CAD a few GB.

**How?** Three power monitors (INA226 or INA219, auto-detected) and the control system's
angle/sun sensors feed an Arduino Nano ESP32. Each one-minute record is time-stamped from a DS3231 RTC (UTC, set by
NTP), stored in on-board flash (8-day ring buffer), and uploaded over HTTPS to ThingSpeak
every 15 minutes in bulk. Sensors are calibrated against the bench DMM before testing (CLI
`cal`); calibration constants are stored on the device and recorded in the test log.

## 2. Documentation and metadata
- Data contract (docs/CONTRACT.md, v1.5): every field, unit, timestamp rule and formula.
- Channel metadata on ThingSpeak (JSON: contract version, units, interval).
- README per dataset export: firmware version, calibration used, test conditions.
- Test log per run: date/time (UTC), location, lamp or sun, weather, calibration, initials, run number.
- File names: `G8_<type>_<YYYYMMDD>_run<NN>.csv` for a test log; the dashboard's exports use the date range,
  `G8_<type>_<YYYYMMDD>-<YYYYMMDD>.csv` (type = raw, runs, pointing-histogram, energy-budget);
  open formats only (CSV, PDF, PNG, STEP, .m, .js, .cpp).
- Simulated data are marked `s=1` in every record and flagged in every export.

## 3. Ethics and legal compliance
No human participants; all data come from our own hardware (low risk; Ethics form
submitted with the design brief). No personal data in any channel. No university
(eduroam) credentials on the device: a phone hotspot is used. API write keys are never
put in the repository, screenshots, the dashboard or AI tools. Datasheets, papers and
PVGIS data are referenced, not copied. Code and data are our own work under University
regulations; library licences are listed in the README; AI use is declared in the AI log.

## 4. Storage and backup
**University enterprise offering (design-brief feedback).** Every service holding measured data is
provided through the University: ThingSpeak and MATLAB (analysis, charts, report figures) run under
the University's MathWorks Campus-Wide licence on a @surrey.ac.uk account (MathWorks: a school with a
Campus-Wide licence can use the Free ThingSpeak licence for non-commercial work,
https://www.mathworks.com/matlabcentral/answers/754094), and backups and the final archive sit in
the University's Microsoft 365 (OneDrive). Our load is about 18 % of the free allowance, so a paid
academic ThingSpeak licence is not needed. The Supabase plan of the design brief was dropped; GitHub
holds code, documents and simulated test samples only, never measured data or keys.

| Copy | Where | Role |
|---|---|---|
| Device flash | ESP32 LittleFS ring buffer, 8 days | primary until uploaded; survives outages (F7) |
| ThingSpeak ch1 3578277 / ch2 3578278 | MathWorks cloud, HTTPS; the free ThingSpeak tier that comes with the University of Surrey's MathWorks Campus-Wide (Total Academic Headcount) licence, on AM's university account | working copy; public-read, write needs the channel key |
| University OneDrive (Surrey365, 250 GB per student; group folder shared from AM's account) | weekly CSV export from the dashboard (4 files) | backup: ThingSpeak's free licence has no backup; Surrey IT asks for University-provided storage for University work |
| GitHub repository | code, contract, docs | version control (no keys) |

**Access and security.** Writes need the channel write key (device and MATLAB job only);
reads are public because the data are non-personal measurements and the dashboard would
otherwise expose a read key in a public page anyway (CIA triad: integrity and availability
matter here, confidentiality does not). Admin (ThingSpeak account): AM; backup admin: TBC
at the 19 Nov meeting. Weekly check: ThingSpeak entry count vs the device's sequence
numbers (completeness column on the dashboard).

## 5. Selection and preservation
Kept: cleaned datasets behind the report's results, calibration records, test logs,
README, final code/CAD, decision matrices, minutes, AI log. Deleted after marking:
debug runs and simulated data (channel cleared before real tests start). After the
25 Jan 2027 presentation, all files are archived on OneDrive and submitted with the
report; the ThingSpeak channels are exported and then cleared/deleted.

## 6. Data sharing
Live results on the phone dashboard (static page; CSV export for raw samples, gain per
run incl. calibration runs, pointing-error histogram, energy budget). Raw exports on
OneDrive; data and code appended to the final report. No restriction on measurement data;
staff may reuse it for teaching. Keys and contact details are never shared.

## 7. Responsibilities and resources
- AM — data manager: ThingSpeak channels and MATLAB job, dashboard, weekly backup and
  check, README, final archive.
- Backup admin — TBC (19 Nov).
- Control-system lead (TBC) — calls the data-link telemetry API (firmware/INTERFACE.md),
  calibration with AM.
- Person running each test — completes the test log; LA keeps minutes and the AI log.
Resources: all free (ThingSpeak free licence, GitHub, OneDrive, lab DMM). Nothing from the
£125 budget (the SD-card module is no longer needed). Time: ~4 h setup (done), ~1 h/week,
~3 h final archive.
