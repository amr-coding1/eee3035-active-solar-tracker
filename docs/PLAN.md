# Build plan — AST data link (AM's part), rebuilt 13 Nov 2026

Contract: `docs/CONTRACT.md` v1.5. Everyone reads it first.

## Directory ownership (do not edit another owner's directory)

| Path | Owner | What |
|---|---|---|
| `firmware/` | Agent F (firmware) | Arduino Nano ESP32 sketch, portable C++ core, host build, unit tests, `ast_host_device` |
| `analysis/` | Agent M (MATLAB) | `ast_nightly.m`, 5 `vis_*.m`, `ast_report_figures.m`, Python reference `analysis/py/ast_metrics.py`, Octave tests |
| `tools/`, `tests/e2e/` | Agent E (end-to-end) | `mock_thingspeak.py`, `sim_device.py`, E2E orchestrator and scenarios |
| `dashboard/`, `docs/` (except `docs/VV_RESULTS.md` sections written by Agent E) | Lead (main session) | phone dashboard, ThingSpeak setup, report material |

## Shared interfaces

### `ast_host_device` (built by Agent F, driven by Agent E)
Location: `firmware/host/build/ast_host_device` (CMake). Runs the SAME portable
core as the ESP32 build with a simulated clock, simulated sensors and a
plain-HTTP transport.

```
ast_host_device --base http://127.0.0.1:PORT --ch1 ID --write-key KEY [--read-key KEY]
  --state-dir DIR                 persistent "flash": ring buffer, metadata, seq (survives restarts)
  --start 2026-11-20T00:00:00Z    virtual UTC start (ignored when DIR already holds a clock, unless --set-time)
  --minutes N                     run N virtual minutes, then exit 0 (graceful)
  --upload-min 15                 upload interval (5-60)
  --scenario clear|cloudy|overcast|night|lamp   simulated sun model     --seed S
  --wifi-down START/END           Wi-Fi unavailable in this ISO-8601 UTC window (repeatable)
  --crash-at ISO                  abrupt stop at that virtual time, nothing flushed beyond normal persistence -> exit 3
  --rtc-lost                      start with the RTC oscillator-stop flag set and no NTP until the first successful connection
  --run ID --panel A|B            test run id and panel letter for the status string
  --virtual                       no real sleeping; every HTTP request carries header `X-Sim-Time: <unix seconds>`
  --log FILE                      JSON lines: {"t":..., "ev":"sample|upload_ok|upload_fail|in_doubt|fault|backoff", ...}
exit codes: 0 ok, 2 usage error, 3 simulated crash
```

### Mock server (built by Agent E, used by all)
`python3 tools/mock_thingspeak.py --port P --channel ID:WRITEKEY:READKEY [--channel ...] [--state FILE]`
Admin (JSON over HTTP): `POST /__admin/reset`, `POST /__admin/faults`
(`{"mode":"http500"|"drop_response"|"delay"|"reject_all","count":N,"ms":M}`),
`GET /__admin/dump/<channel>`, `GET /__admin/log`. Header `X-Sim-Time`
(unix seconds) is used as "now" for the 15 s spacing rule when present.
CORS `Access-Control-Allow-Origin: *` on every response.

### Dashboard CSV export header (raw samples)
`created_at_utc,time_uk,entry_id,seq,p_trk_mw,p_fix_mw,p_use_mw,ang_panel_deg,ang_sun_deg,temp_c,vbat_v,state,run,panel,fault,sim,status`

## Machine notes
- 2 CPUs, 7 GB RAM shared by everyone: build with `-j2` at most, no parallel
  heavy compiles, kill servers you start.
- ESP32 core `esp32:esp32@3.3.12` is being installed by arduino-cli
  (`/tmp/esp32_core_install.log`, wait for `DONE exit=0`).
- GNU Octave is being installed by apt (`/tmp/octave_install.log`, wait for `DONE`).
- Google Chrome for Testing is on PATH (`google-chrome`); Playwright 1.63 is in
  `/opt/npm-tools/node_modules/playwright` (use `executablePath` of that Chrome).
- The workspace reaches npm, PyPI, apt, GitHub release assets and
  api.thingspeak.com through an HTTPS proxy that RE-SIGNS TLS: never pin or
  copy a certificate seen from this machine; it is the proxy's, not ThingSpeak's.
- Never put real API keys in files. Tests use made-up keys with the mock.
