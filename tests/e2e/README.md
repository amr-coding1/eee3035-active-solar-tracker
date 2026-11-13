# End-to-end tests of the AST data link

Tests the whole chain of contract v1.5 (`docs/CONTRACT.md`) on one computer, in virtual time:

```
device  ->  ThingSpeak channel 1  ->  nightly job  ->  channel 2  ->  phone dashboard
```

## How to run

```
python3 tests/e2e/run_e2e.py                      # all 13 scenarios, about 1.5 min
python3 tests/e2e/run_e2e.py --scenarios S1,S6    # some scenarios
python3 tests/e2e/run_e2e.py --device sim         # force the Python device stand-in
python3 tests/e2e/run_e2e.py --nightly python     # force the Python nightly stand-in
python3 tests/e2e/run_e2e.py --no-browser         # skip the dashboard checks (recorded as "not run")
python3 tests/e2e/run_e2e.py --keep               # keep device state and logs in the temp directory
python3 -m pytest tools -q                        # unit tests of the mock server and the simulator
```

Needs: Python 3.9+ (standard library only), `analysis/py/ast_metrics.py`. Optional and
picked up automatically: `firmware/host/build/ast_host_device`, GNU Octave with
`analysis/matlab/ast_nightly.m`, Node.js with Playwright in `/opt/npm-tools/node_modules/playwright`
and `google-chrome` on the PATH. The script starts the mock server and a static file
server on free ports and stops them at the end. Exit code 0 only when no check failed.

Outputs:

| File | Content |
|---|---|
| `tests/e2e/results/e2e_summary.md` | every check: expected, measured, PASS/FAIL (last run) |
| `tests/e2e/results/e2e_results.json` | the same as data, plus components used, timings, nightly-job output |
| `tests/e2e/results/e2e_summary_sim_device.md`, `e2e_results_sim_device.json` | the same run made with the Python device stand-in (`--device sim`) |
| `docs/VV_RESULTS.md` (block between the `E2E` markers) | F7, F8, F9: target, method, measured, verdict, and the bench procedures. Rewritten only by a run of all scenarios |

## Which parts are real

The script prints this at the start and writes it into every report.

| Part | Real component | Stand-in used when the real one is missing |
|---|---|---|
| Device | `firmware/host/build/ast_host_device`: the portable firmware core compiled for the PC, with simulated clock, sensors and Wi-Fi | `tools/sim_device.py` (Python, same command line, same protocol) |
| ThingSpeak | - | always `tools/mock_thingspeak.py` (the agents cannot hold API keys) |
| Nightly job | `analysis/matlab/ast_nightly.m`, run unchanged by GNU Octave (Octave stands in for MATLAB) | `tests/e2e/nightly_standin.py` (Python, labelled STAND-IN) |
| Dashboard | `dashboard/` in headless Google Chrome | none: checks are marked "not run" |
| Reference | `analysis/py/ast_metrics.py` (contract section 6) | none: required |

**Last run (13 Nov 2026):** device = real firmware host build; nightly job = real
`ast_nightly.m` in GNU Octave 8.4.0; dashboard = real code in headless Chrome;
ThingSpeak = mock. The same scenarios were also run with `--device sim`
(results kept next to the main ones). Both runs, after the firmware's final changes
(timestamp guard, poison batch, radio policy): 129 of 129 checks pass.

Not covered here, by design: real Wi-Fi, TLS, the real ThingSpeak service, the
real RTC and the sensors. Those are the bench procedures B1 to B4 in `docs/VV_RESULTS.md`.

## Virtual time

The device runs with `--virtual`: it does not sleep, and every request carries
`X-Sim-Time: <unix seconds>`. The mock uses that header as "now" for the 15-second
rule and records it as the receive time of every entry. Before each nightly run
the script sets the mock's admin clock (`POST /__admin/clock`), passes the same
time to the job (`AST_NOW_UTC`), and opens the dashboard with `&now=<ISO time>`.
All scenarios start at 2026-11-20 00:00 UTC.

## What each scenario proves

| Scenario | What happens | What it proves |
|---|---|---|
| S1 | three normal days | 1 sample per minute, contract formats and status grammar, upload every 15 min at second 20, 15 entries per upload, on the server <= 16 min after the minute ends, nightly summary and dashboard table agree with the reference |
| S2 | Wi-Fi off 10:00 to 16:00 | nothing is lost, the back-fill finishes within minutes, fault 9 appears after 60 min without an upload |
| S3 | Wi-Fi off for 7.5 days; the nightly job keeps running | all 10,800 buffered samples arrive (8-day buffer, no fault 6); the next nightly run corrects the days it rewrites. S3.6-S3.7 re-run the old 7-day window as regression evidence: the first outage day is 8 days old, outside that window, and its channel-2 row stays wrong (the defect this test found, fixed in contract v1.5); S3.8-S3.9 show the production 9-day window repairs it |
| S4 | the answer to one upload is lost and the device crashes 10 s later; restart from the same state directory | the unacknowledged batch is sent again, sequence numbers continue, duplicates are removed by `seq`; only the minute in which the device was down is missing |
| S5 | three answers are lost | duplicates appear in channel 1; energies, N and completeness are unchanged; channel 2 reports `nd=`; the dashboard shows a duplicate note |
| S5b | as S5 with `existing_timestamp_policy = reject` on the mock | the device and the metrics also work if ThingSpeak drops entries whose timestamp already exists (what the MathWorks import documentation suggests) |
| S6 | eight HTTP 500 answers in a row | waits of 1, 2, 4, 8, 15, 15, 15, 15 min; fault 9 only after 60 min; nothing lost or duplicated |
| S7 | Wi-Fi off for 3 days | backlog sent in requests of at most 960 entries, at least 16 s apart, no HTTP 429 |
| S8 | Wi-Fi off for 9 days (longer than the buffer) | the oldest minutes are lost visibly: fault 6 in the status, sequence-number gap equal to the loss, completeness below 99 % on the dashboard |
| S9 | RTC lost time, no Wi-Fi for 3 h | no entry with a wrong timestamp, logging starts when NTP has set the clock, fault 4 visible in channel 1 and on the dashboard |
| S10 | five different days (clear; cloudy with lost answers; overcast with an outage and panel B; dark; calibration run 900) | dashboard table, KPI cards, nightly job and reference agree on every complete day (rule 6.5); two phone profiles show identical figures; the CSV export equals channel 1 and the device log; the dashboard never sends a write key or a write request |
| S11 | three days, one with a 45 min Wi-Fi drop | completeness >= 99 % by reference, channel 2 and dashboard |
| S12 | half a day, dashboard opened 60 s after an upload | worst-case sample-to-screen time = server latency + refresh period + page update <= 17 min; the page refreshes itself |

Every check is an assertion with numbers (`h.check(id, what, expected, measured, ok)`);
a scenario that throws is itself a failed check.

## How faults are injected

| Fault | Means |
|---|---|
| Wi-Fi outage | device option `--wifi-down START/END` |
| crash | device option `--crash-at ISO` (exit code 3), then a restart with `--start <next minute> --set-time` |
| RTC lost | device option `--rtc-lost` |
| lost HTTP answer | mock `POST /__admin/faults {"mode":"drop_response","count":N}`: the write is stored, the socket is closed without a reply |
| server error | mock `{"mode":"http500","count":N}` |
| duplicate-timestamp policy | mock `POST /__admin/config {"existing_timestamp_policy":"reject"}` |

To arm a fault at a given virtual time the script stops the device with `--minutes`,
arms the fault, and starts the device again from the same state directory.

## Files

| File | Purpose |
|---|---|
| `run_e2e.py` | orchestrator, scenarios, report writers |
| `dash_probe.js` | opens the dashboard in headless Chrome and returns what is on the page (KPI cards `[data-kpi] .kpi-value`, `#daily-table tbody tr`, quality notes, network requests, CSV export) |
| `nightly_standin.py` | Python stand-in for the nightly job (only used when Octave or the `.m` file is missing) |
| `../../tools/mock_thingspeak.py`, `test_mock_thingspeak.py` | mock server and its 54 unit tests |
| `../../tools/sim_device.py`, `test_sim_device.py` | device simulator (modes `run`, `history`, `live`, `csv`) and its 15 unit tests |

## Notes on the device driver

`ast_host_device` follows the command line in `docs/PLAN.md`; the driver needed no
adaptation. Differences the tests allow for: scenario `mixed` exists only in
`sim_device.py`, so the scenarios use `clear`, `cloudy`, `overcast` and `night`;
after a restart the firmware counts the 60 minutes for fault 9 from the boot time
(the stand-in remembers the last successful upload), both inside the tolerance of S6.5.
