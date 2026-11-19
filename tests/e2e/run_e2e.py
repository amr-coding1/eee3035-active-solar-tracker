#!/usr/bin/env python3
"""
run_e2e.py - end-to-end tests of the AST data link (CONTRACT.md v1.5).

    device  ->  mock ThingSpeak ch1  ->  nightly job  ->  ch2  ->  dashboard

    python3 tests/e2e/run_e2e.py                    all scenarios
    python3 tests/e2e/run_e2e.py --scenarios S1,S6  some scenarios
    options: --device auto|host|sim   --nightly auto|octave|python
             --no-browser  --wait-host SECONDS  --keep  --no-vv

Components (the report says which were used):
  device   firmware/host/build/ast_host_device (the real portable firmware core)
           when it exists, otherwise tools/sim_device.py (Python stand-in)
  server   tools/mock_thingspeak.py on a free port (always; virtual time through
           the X-Sim-Time header and the admin clock)
  nightly  analysis/matlab/ast_nightly.m in GNU Octave when both exist,
           otherwise tests/e2e/nightly_standin.py (Python STAND-IN)
  checks   analysis/py/ast_metrics.py (Python reference of contract section 6)
  browser  dashboard/ served by python -m http.server, read in headless
           Google Chrome through Playwright (tests/e2e/dash_probe.js)

Outputs: tests/e2e/results/e2e_results.json, tests/e2e/results/e2e_summary.md
and the generated block of docs/VV_RESULTS.md. Exit code 0 only when no check
failed.
"""
import argparse
import json
import math
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "analysis", "py"))
try:
    import ast_metrics as am
except ImportError:
    sys.stderr.write("analysis/py/ast_metrics.py (the Python reference) is required for the cross-checks\n")
    sys.exit(2)

HOST_DEVICE = os.path.join(ROOT, "firmware", "host", "build", "ast_host_device")
SIM_DEVICE = os.path.join(ROOT, "tools", "sim_device.py")
MOCK = os.path.join(ROOT, "tools", "mock_thingspeak.py")
NIGHTLY_M = os.path.join(ROOT, "analysis", "matlab", "ast_nightly.m")
OCTAVE_SHIMS = os.path.join(ROOT, "analysis", "tests", "octave_shims")
NIGHTLY_PY = os.path.join(HERE, "nightly_standin.py")
PROBE = os.path.join(HERE, "dash_probe.js")
DASHBOARD = os.path.join(ROOT, "dashboard")
RESULTS = os.path.join(HERE, "results")
VV_FILE = os.path.join(ROOT, "docs", "VV_RESULTS.md")
PLAYWRIGHT = "/opt/npm-tools/node_modules/playwright"

# made-up keys: they exist only in the mock
CH1, W1, R1 = 1001, "E2EWRITEKEY1001AA", "E2EREADKEY1001AAA"
CH2, W2, R2 = 1002, "E2EWRITEKEY1002BB", "E2EREADKEY1002BBB"
T0 = int(datetime(2026, 11, 20, tzinfo=timezone.utc).timestamp())      # virtual start, 00:00 UTC
DAY = 86400
CAPACITY = 11520
BACKOFF_S = [60, 120, 240, 480, 900]


def T(day=0, h=0, m=0, s=0):
    return T0 + day * DAY + h * 3600 + m * 60 + s


def iso(t):
    return datetime.fromtimestamp(int(t), timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def dayname(d):
    return datetime.fromtimestamp(T(d), timezone.utc).strftime("%Y-%m-%d")


def hm(seconds):
    seconds = float(seconds)
    return "%d min %02d s" % (seconds // 60, round(seconds % 60))


def num(text):
    """Number shown on the dashboard -> float, or None for '-', 'n/a', empty."""
    if text is None:
        return None
    s = str(text).replace("\u2212", "-").replace("\u2013", "-").replace(",", "").replace("\u202f", "").replace("\u00a0", "")
    m = re.search(r"[-+]?\d+(?:\.\d+)?", s)
    if not m or s.strip() in ("-", ""):
        return None
    return float(m.group(0))


def same(a, b):
    """Two displayed numbers are the same (both missing also counts)."""
    if a is None or b is None:
        return a is None and b is None
    return abs(a - b) < 1e-9


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def http(method, url, body=None, timeout=30):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json"} if data is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode()


# ---------------------------------------------------------------- harness

class Harness:
    def __init__(self, args):
        self.args = args
        self.checks = []
        self.notes = []
        self.findings = []
        self.procs = []
        self.tmp = tempfile.mkdtemp(prefix="ast_e2e_")
        self.sid = None
        self.timing = {}
        self.nightly_runs = []
        self.device_runs = 0
        self.browser_pages = 0

    # -- components
    def start(self):
        a = self.args
        # device
        if a.device in ("auto", "host") and a.wait_host and not os.path.exists(HOST_DEVICE):
            print("waiting up to %d s for %s ..." % (a.wait_host, HOST_DEVICE), flush=True)
            end = time.time() + a.wait_host
            while time.time() < end and not os.path.exists(HOST_DEVICE):
                time.sleep(5)
        have_host = os.path.isfile(HOST_DEVICE) and os.access(HOST_DEVICE, os.X_OK)
        if a.device == "host" and not have_host:
            raise SystemExit("--device host: %s does not exist" % HOST_DEVICE)
        self.device_kind = "host" if (have_host and a.device != "sim") else "sim"
        self.device_label = ("firmware/host/build/ast_host_device (REAL firmware core, host build)"
                             if self.device_kind == "host"
                             else "tools/sim_device.py (PYTHON STAND-IN for the firmware)")
        # nightly
        octave = shutil.which("octave")
        have_oct = bool(octave) and os.path.exists(NIGHTLY_M)
        if a.nightly == "octave" and not have_oct:
            raise SystemExit("--nightly octave: octave or %s missing" % NIGHTLY_M)
        self.nightly_kind = "octave" if (have_oct and a.nightly != "python") else "python"
        self.octave_version = ""
        if self.nightly_kind == "octave":
            try:
                self.octave_version = subprocess.run([octave, "--version"], capture_output=True, text=True,
                                                     timeout=60).stdout.splitlines()[0]
            except Exception:
                pass
        self.nightly_label = ("analysis/matlab/ast_nightly.m run by %s (REAL script; Octave stands in for MATLAB)"
                              % (self.octave_version or "GNU Octave") if self.nightly_kind == "octave"
                              else "tests/e2e/nightly_standin.py (PYTHON STAND-IN for ast_nightly.m)")
        # mock
        p = subprocess.Popen([sys.executable, MOCK, "--port", "0", "--channel", "%d:%s:%s" % (CH1, W1, R1),
                              "--channel", "%d:%s:%s" % (CH2, W2, R2)], stdout=subprocess.PIPE, text=True)
        self.procs.append(p)
        line = p.stdout.readline().strip()
        if not line.startswith("LISTENING "):
            raise SystemExit("mock server did not start: %r" % line)
        self.base = line.split()[1]
        # dashboard
        self.browser = not a.no_browser and os.path.exists(os.path.join(DASHBOARD, "index.html")) \
            and shutil.which("node") and shutil.which("google-chrome") and os.path.isdir(PLAYWRIGHT)
        self.dash_base = None
        if self.browser:
            port = free_port()
            p = subprocess.Popen([sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1",
                                  "--directory", DASHBOARD], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.procs.append(p)
            self.dash_base = "http://127.0.0.1:%d" % port
            for _ in range(50):
                try:
                    urllib.request.urlopen(self.dash_base + "/index.html", timeout=2).read()
                    break
                except Exception:
                    time.sleep(0.1)
        self.browser_label = ("dashboard/ in headless Google Chrome via Playwright (REAL dashboard code)" if self.browser
                              else "NOT RUN (no browser): dashboard checks are skipped")
        print("device : %s\nnightly: %s\nbrowser: %s\nmock   : %s\n" % (self.device_label, self.nightly_label,
                                                                       self.browser_label, self.base), flush=True)

    def stop(self):
        for p in self.procs:
            try:
                p.terminate()
            except Exception:
                pass
        for p in self.procs:
            try:
                p.wait(timeout=10)
            except Exception:
                p.kill()
        if not self.args.keep:
            shutil.rmtree(self.tmp, ignore_errors=True)

    # -- mock admin
    def admin(self, method, path, body=None):
        status, text = http(method, self.base + path, body)
        if status != 200:
            raise RuntimeError("mock admin %s %s -> %s %s" % (method, path, status, text[:200]))
        return json.loads(text)

    def fault(self, mode, count=None, **kw):
        body = {"mode": mode}
        if count is not None:
            body["count"] = count
        body.update(kw)
        self.admin("POST", "/__admin/faults", body)

    def mock_log(self, kind="bulk", channel=CH1):
        return [r for r in self.admin("GET", "/__admin/log")["log"] if r["kind"] == kind and r["channel"] == channel]

    def raw(self, channel=CH1):
        return self.admin("GET", "/__admin/dump/%d" % channel)["entries"]

    # -- scenario bookkeeping
    def begin(self, sid, title):
        self.sid = sid
        self.title = title
        self.admin("POST", "/__admin/reset", {})
        self.admin("POST", "/__admin/config", {"existing_timestamp_policy": "store"})
        self.state = os.path.join(self.tmp, sid, "state")
        os.makedirs(os.path.dirname(self.state), exist_ok=True)
        self.devlog = os.path.join(self.tmp, sid, "device.jsonl")
        self.clock = None
        self.after_crash = False
        self.wifi_down = []
        self.rtc_lost = False
        self._t0 = time.time()
        print("== %s %s" % (sid, title), flush=True)

    def end(self):
        self.timing[self.sid] = round(time.time() - self._t0, 1)

    def check(self, cid, what, expected, measured, ok, req=()):
        status = "PASS" if ok is True else "FAIL" if ok is False else str(ok)
        rec = {"scenario": self.sid, "title": self.title, "id": "%s.%s" % (self.sid, cid), "check": what,
               "expected": str(expected), "measured": str(measured), "status": status, "req": list(req)}
        self.checks.append(rec)
        print("   [%s] %s: %s | expected %s | measured %s" % (status, rec["id"], what, expected, measured), flush=True)
        return ok is True

    def skip(self, cid, what, reason, req=()):
        self.check(cid, what, "-", reason, "SKIP", req)

    def finding(self, component, text):
        self.findings.append({"scenario": self.sid, "component": component, "text": text})
        print("   [FINDING] %s: %s" % (component, text), flush=True)

    # -- device
    def dev(self, until, scenario="cloudy", seed=7, run=0, panel="A", crash_at=None, start=T0, read_key=False,
            upload_min=15):
        """Run the device from its current clock until virtual time `until`."""
        if self.clock is None:
            self.clock = start
            first = True
        else:
            first = False
        minutes = int(round((until - self.clock) / 60.0))
        cmd = [HOST_DEVICE] if self.device_kind == "host" else [sys.executable, SIM_DEVICE, "run"]
        cmd += ["--base", self.base, "--ch1", str(CH1), "--write-key", W1, "--state-dir", self.state,
                "--minutes", str(minutes), "--upload-min", str(upload_min), "--scenario", scenario,
                "--seed", str(seed), "--run", str(run), "--panel", panel, "--virtual", "--log", self.devlog]
        if first:
            cmd += ["--start", iso(self.clock)]
        elif self.after_crash:
            cmd += ["--start", iso(self.clock), "--set-time"]
        if read_key:
            cmd += ["--read-key", R1]
        for a, b in self.wifi_down:
            cmd += ["--wifi-down", "%s/%s" % (iso(a), iso(b))]
        if crash_at is not None:
            cmd += ["--crash-at", iso(crash_at)]
        if self.rtc_lost and first:
            cmd += ["--rtc-lost"]
        t = time.time()
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
        self.device_runs += 1
        self.last_dev = {"cmd": " ".join(cmd).replace(W1, "<write key>"), "rc": r.returncode, "wall_s": round(time.time() - t, 2),
                         "stdout": r.stdout[-2000:], "stderr": r.stderr[-2000:]}
        if crash_at is not None and r.returncode == 3:
            self.clock = (crash_at // 60 + 1) * 60      # reboot: back at the next whole minute
            self.after_crash = True
        else:
            self.clock = until
            self.after_crash = False
        expected_rc = 3 if crash_at is not None else 0
        if r.returncode != expected_rc:
            print("   device exit code %d (expected %d)\n   cmd: %s\n   stderr: %s" %
                  (r.returncode, expected_rc, self.last_dev["cmd"], r.stderr[-800:]), flush=True)
        return r.returncode

    def device_events(self, ev=None):
        out = []
        if os.path.exists(self.devlog):
            with open(self.devlog, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    try:
                        rec = json.loads(line)
                    except ValueError:
                        continue
                    if ev is None or rec.get("ev") == ev:
                        out.append(rec)
        return out

    # -- data
    def entries(self):
        """Channel-1 entries as the reference parses them, plus rx = time the server received them."""
        out = []
        for row in self.raw(CH1):
            e = am.parse_feed(row)
            e["rx"] = row["received_at"]
            e["row"] = row
            out.append(e)
        return out

    def daily(self, entries, now_s):
        return {d["day"]: d for d in am.daily_summaries(entries, now_s * 1000)}

    def ch2(self):
        return am.latest_results_by_date([am.parse_result_feed(r) for r in self.raw(CH2)])

    def agreement(self, daily, ch2, days):
        """{day: (status, reason)} by the reference implementation of rule 6.5."""
        out = {}
        for d in days:
            if d not in daily:
                out[d] = ("no-data", "")
                continue
            out[d] = am.agreement(daily[d], ch2.get(d))
        return out

    # -- nightly
    def nightly(self, now_s, ndays=None):
        """ndays=None runs the job with its own production setting (NDAYS = 9 since contract v1.5)."""
        self.admin("POST", "/__admin/clock", {"now": now_s})
        t = time.time()
        if self.nightly_kind == "octave":
            code = ""
            if os.path.isdir(OCTAVE_SHIMS):
                code += "addpath('%s'); " % OCTAVE_SHIMS
            code += ("AST_BASE_URL='%s'; AST_CH1_ID=%d; AST_CH1_READ_KEY='%s'; AST_CH2_ID=%d; AST_CH2_WRITE_KEY='%s'; "
                     "AST_NOW_UTC=%d; " % (self.base, CH1, R1, CH2, W2, now_s))
            if ndays is not None:
                code += "AST_NDAYS=%d; " % ndays
            code += "run('%s');" % NIGHTLY_M
            r = subprocess.run(["octave", "--no-gui", "--quiet", "--no-init-file", "--eval", code], capture_output=True,
                               text=True, timeout=600, cwd=os.path.dirname(NIGHTLY_M))
        else:
            r = subprocess.run([sys.executable, NIGHTLY_PY, "--base", self.base, "--ch1", str(CH1), "--rk1", R1,
                                "--ch2", str(CH2), "--wk2", W2, "--now", iso(now_s), "--days", str(9 if ndays is None else ndays)],
                               capture_output=True, text=True, timeout=600)
        wall = round(time.time() - t, 2)
        rec = {"scenario": self.sid, "now": iso(now_s), "ndays": ndays if ndays is not None else "script default (9)", "rc": r.returncode, "wall_s": wall,
               "out": (r.stdout + r.stderr)[-1500:]}
        self.nightly_runs.append(rec)
        if r.returncode != 0:
            print("   nightly job failed (rc %d): %s" % (r.returncode, rec["out"][-600:]), flush=True)
        return rec

    # -- dashboard
    def dash_url(self, now_s, day=None, extra=""):
        q = {"demo": "0", "base": self.base, "ch1": str(CH1), "rk1": R1, "ch2": str(CH2), "rk2": R2, "now": iso(now_s)}
        if day:
            q["day"] = day
        return "%s/index.html?%s%s" % (self.dash_base, urllib.parse.urlencode(q), extra)

    def dash(self, pages):
        """pages: list of dicts {name, now, day?, device?, csv?, watchMs?, extra?} -> {name: result} or None."""
        if not self.browser:
            return None
        job = {"chrome": shutil.which("google-chrome"), "playwright": PLAYWRIGHT, "pages": []}
        for p in pages:
            job["pages"].append({"name": p["name"], "url": self.dash_url(p["now"], p.get("day"), p.get("extra", "")),
                                 "device": p.get("device"), "csv": bool(p.get("csv")), "watchMs": p.get("watchMs", 0)})
        path = os.path.join(self.tmp, "probe_%s_%d.json" % (self.sid, self.browser_pages))
        with open(path, "w") as fh:
            json.dump(job, fh)
        r = subprocess.run(["node", PROBE, path], capture_output=True, text=True, timeout=600)
        self.browser_pages += len(pages)
        try:
            res = json.loads(r.stdout)
        except ValueError:
            print("   dashboard probe failed: %s" % (r.stderr[-500:] or r.stdout[-500:]), flush=True)
            return {}
        if not res.get("ok"):
            print("   dashboard probe error: %s" % res.get("error"), flush=True)
            return {}
        return {p["name"]: p for p in res["pages"]}


# ---------------------------------------------------------------- dashboard helpers

COLS = ["period", "eTrk", "eFix", "eUse", "gGross", "gNet", "mae", "complete", "n", "matlab"]


def dash_rows(page):
    """{date: {col: text, '_matlab': data-matlab}} from the daily table."""
    out = {}
    if not page or not page.get("daily"):
        return out
    for row in page["daily"]["rows"]:
        date = row["data"].get("date")
        if not date:
            continue
        cells = {}
        for i, c in enumerate(row["cells"]):
            cells[c["col"] or (COLS[i] if i < len(COLS) else "c%d" % i)] = c["text"]
        cells["_matlab"] = row["data"].get("matlab") or ("agree" if "agree" in (cells.get("matlab") or "") else
                                                         "differ" if "differ" in (cells.get("matlab") or "") else "missing")
        out[date] = cells
    return out


def ref_display(d):
    """What the dashboard should show for a reference day summary (rule 6.5 rounding)."""
    return {"eTrk": num(am.fmt_sig(d["e_trk"])), "eFix": num(am.fmt_sig(d["e_fix"])), "eUse": num(am.fmt_sig(d["e_use"])),
            "gGross": None if d["g_gross"] is None else num(am.fmt_fixed(d["g_gross"])),
            "gNet": None if d["g_net"] is None else num(am.fmt_fixed(d["g_net"])),
            "mae": None if d["mae"] is None else num(am.fmt_fixed(d["mae"])),
            "complete": None if d["complete"] is None else num(am.fmt_fixed(d["complete"])),
            "n": float(d["n_unique"])}


def table_mismatches(page, daily, days):
    """Differences between the dashboard daily table and the reference, as text."""
    rows = dash_rows(page)
    bad = []
    for day in days:
        if day not in rows:
            bad.append("%s: no row" % day)
            continue
        want = ref_display(daily[day])
        for col, w in want.items():
            got = num(rows[day].get(col))
            if not same(got, w):
                bad.append("%s %s: dashboard %s, reference %s" % (day, col, rows[day].get(col), w))
    return bad


def kpi_mismatches(page, d):
    bad = []
    if not page or not page.get("kpis"):
        return ["no KPI cards read"]
    want = ref_display(d)
    for key in ("eTrk", "eFix", "eUse", "gGross", "gNet", "complete", "mae"):
        got = num((page["kpis"].get(key) or {}).get("value"))
        if not same(got, want[key]):
            bad.append("%s: dashboard %s, reference %s" % (key, (page["kpis"].get(key) or {}).get("value"), want[key]))
    return bad


def page_ok(page):
    return bool(page) and page.get("ready") and not page.get("pageErrors") and (page.get("body") or {}).get("state") != "error"


# ---------------------------------------------------------------- generic data checks

FIELD_1DP = re.compile(r"^-?\d+\.\d$")
FIELD_3DP = re.compile(r"^\d+\.\d{3}$")
STATUS_RE = re.compile(r"^(TRK|MOV|HLD|IDL|STW|FLT)(;[A-Za-z]+=[A-Za-z0-9.\-]+)*$")


def format_errors(rows):
    """Violations of the contract 3.1 / 4 formats in raw channel-1 rows."""
    bad = []
    for r in rows:
        why = None
        for f in ("field1", "field2", "field3", "field4"):
            if r[f] is None or not FIELD_1DP.match(r[f]):
                why = "%s=%r" % (f, r[f])
        for f in ("field5", "field6"):
            if r[f] is not None and not FIELD_1DP.match(r[f]):
                why = "%s=%r" % (f, r[f])
        if r["field7"] is None or not FIELD_3DP.match(r["field7"]):
            why = "field7=%r" % (r["field7"],)
        if r["field8"] is None or not re.match(r"^\d+$", r["field8"]):
            why = "field8=%r" % (r["field8"],)
        st = r["status"] or ""
        if not STATUS_RE.match(st) or not re.search(r";r=\d+", st) or not re.search(r";p=[AB]", st) \
                or not re.search(r";e=\d\b", st):
            why = "status=%r" % (st,)
        if not r["created_at"].endswith(":00Z"):
            why = "created_at=%r" % (r["created_at"],)
        if any(r[f] is not None and float(r[f]) < 0 for f in ("field1", "field2", "field3")):
            why = "negative power"
        if why:
            bad.append("entry %s: %s" % (r["entry_id"], why))
    return bad


def minute_set(entries):
    return {e["t"] // 1000 for e in entries}


def missing_minutes(entries, first, last):
    """Minute starts in [first, last] (unix s, inclusive) with no entry."""
    have = minute_set(entries)
    return [t for t in range(first, last + 1, 60) if t not in have]


def uploads(h):
    """Channel-1 bulk requests seen by the server: list of log records."""
    return h.mock_log("bulk", CH1)


def fault_count(entries, code):
    return sum(1 for e in entries if e["fault"] == code)


def standard_pipeline(h, now_s, days, prefix, req=("F9",), ndays=None, dash=True, expect_nightly=True):
    """Nightly job at now_s, then cross-check ch2 and the dashboard against the reference
    for the given complete days. Returns (entries, daily, ch2, page)."""
    n = h.nightly(now_s, ndays)
    h.check(prefix + "n", "nightly job runs to the end (%s)" % h.nightly_kind, "exit 0", "exit %d in %.1f s" % (n["rc"], n["wall_s"]),
            n["rc"] == 0, req)
    entries = h.entries()
    daily = h.daily(entries, now_s)
    ch2 = h.ch2()
    ag = h.agreement(daily, ch2, days)
    bad = ["%s: %s %s" % (d, s, r) for d, (s, r) in ag.items() if s != "agree"]
    h.check(prefix + "a", "channel 2 equals the Python reference computed from channel 1 (rule 6.5) on %d complete day(s)" % len(days),
            "%d of %d agree" % (len(days), len(days)), "%d of %d agree%s" % (len(days) - len(bad), len(days), ("; " + "; ".join(bad[:3])) if bad else ""),
            not bad, req)
    versions = sorted({(ch2[d].get("version") or "none") for d in days if d in ch2})
    h.check(prefix + "v", "channel-2 status carries v=1.5 and field8 = N unique", "v=1.5; N equal",
            "v=%s; N %s" % (",".join(versions), "equal" if all(d in ch2 and ch2[d]["n"] == daily[d]["n_unique"] for d in days if d in daily) else "DIFFERENT"),
            versions == ["1.5"] and all(d in ch2 and ch2[d]["n"] == daily[d]["n_unique"] for d in days if d in daily), req)
    page = None
    if dash:
        res = h.dash([{"name": "table", "now": now_s + 60}])
        if res is None:
            h.skip(prefix + "d", "dashboard daily table and MATLAB check column", "browser not available", req)
        else:
            page = res.get("table")
            rows = dash_rows(page)
            flags = {d: rows.get(d, {}).get("_matlab", "no row") for d in days}
            notagree = ["%s: %s (%s)" % (d, f, rows.get(d, {}).get("matlab")) for d, f in flags.items() if f != "agree"]
            h.check(prefix + "d", "dashboard 'MATLAB check' column on the %d complete day(s)" % len(days), "all 'agrees'",
                    "%d of %d agree%s" % (len(days) - len(notagree), len(days), ("; " + "; ".join(notagree[:2])) if notagree else ""),
                    page_ok(page) and not notagree, req)
            mism = table_mismatches(page, daily, [d for d in days if d in daily])
            h.check(prefix + "t", "dashboard daily table values equal the reference (3 s.f. energies, 1 dp others, N exact)",
                    "0 differences", "%d differences%s" % (len(mism), ("; " + "; ".join(mism[:3])) if mism else ""), page_ok(page) and not mism, req)
    return entries, daily, ch2, page


# ---------------------------------------------------------------- scenarios

def s1(h):
    h.begin("S1", "Three normal days")
    rc = h.dev(T(3, 0, 16))
    h.check("1", "device runs 3 days + 16 min of virtual time", "exit 0", "exit %d (%.1f s wall)" % (rc, h.last_dev["wall_s"]), rc == 0)
    entries = h.entries()
    rows = h.raw(CH1)
    days = [dayname(d) for d in range(3)]
    daily = h.daily(entries, T(3, 0, 20))
    counts = [daily[d]["n_unique"] if d in daily else 0 for d in days]
    h.check("2", "samples in channel 1 per day", "1440, 1440, 1440", ", ".join(map(str, counts)), counts == [1440] * 3, ("F7",))
    seqs = [e["seq"] for e in entries]
    h.check("3", "sequence numbers start at 1, +1 per sample, no duplicates", "1..%d contiguous" % len(entries),
            "%s..%s, %d unique of %d" % (min(seqs), max(seqs), len(set(seqs)), len(seqs)),
            seqs == list(range(1, len(seqs) + 1)), ("F7",))
    bad = format_errors(rows)
    h.check("4", "field formats and status grammar (contract 3.1, 4) on every entry", "0 violations of %d" % len(rows),
            "%d violations%s" % (len(bad), ("; " + bad[0]) if bad else ""), not bad, ("F7",))
    up = uploads(h)
    codes = sorted({r["status"] for r in up})
    h.check("5", "every bulk update accepted", "all HTTP 202", "%d requests, status codes %s" % (len(up), codes), codes == [202], ("F8",))
    gaps = [round(b["now"] - a["now"]) for a, b in zip(up, up[1:])]
    h.check("6", "upload interval", "15 min 00 s between uploads", "min %s, max %s over %d intervals" % (hm(min(gaps)), hm(max(gaps)), len(gaps)),
            min(gaps) == 900 and max(gaps) == 900, ("F8",))
    sec = sorted({int(r["now"]) % 60 for r in up})
    h.check("7", "uploads start at second 20 of the minute (contract 7.3)", "[20]", str(sec), sec == [20], ("F8",))
    lat = [e["rx"] - (e["t"] / 1000 + 60) for e in entries]
    h.check("8", "time from end of a sample's minute to its arrival on the server", "<= 16 min 00 s",
            "max %s, mean %s (n=%d)" % (hm(max(lat)), hm(sum(lat) / len(lat)), len(lat)), max(lat) <= 960, ("F8",))
    sizes = sorted({r["updates"] for r in up})
    h.check("9", "entries per bulk update in normal operation", "15 (one upload interval)", str(sizes), sizes == [15], ("F8",))
    nsim = sum(1 for e in entries if e["sim"])
    h.check("10", "simulated samples are marked s=1 (contract 7.5)", "%d of %d" % (len(entries), len(entries)), "%d of %d" % (nsim, len(entries)),
            nsim == len(entries))
    standard_pipeline(h, T(3, 0, 20), days, "11")
    h.end()


def s2(h):
    h.begin("S2", "6-hour Wi-Fi outage, then back-fill")
    h.wifi_down = [(T(0, 10), T(0, 16))]
    rc = h.dev(T(1, 0, 16))
    h.check("1", "device runs through the outage", "exit 0", "exit %d" % rc, rc == 0)
    entries = h.entries()
    up = uploads(h)
    inside = [r for r in up if T(0, 10) <= r["now"] < T(0, 16)]
    h.check("2", "requests reaching the server during the outage 10:00-16:00", "0", str(len(inside)), not inside)
    miss = missing_minutes(entries, T(0), T(1, 0, 14))
    h.check("3", "minutes missing in channel 1 after back-fill (00:00 day 1 to 00:14 day 2)", "0 of 1455", "%d of 1455" % len(miss), not miss, ("F7",))
    out = [e for e in entries if T(0, 10) <= e["t"] / 1000 < T(0, 16)]
    done = max(e["rx"] for e in out) - T(0, 16)
    h.check("4", "back-fill of the 360 outage samples finished after Wi-Fi returned", "<= 16 min", "%d samples, last arrived %s after 16:00" % (len(out), hm(done)),
            len({e["seq"] for e in out}) == 360 and done <= 960, ("F7", "F8"))
    daily = h.daily(entries, T(1, 0, 20))
    d0 = daily[dayname(0)]
    h.check("5", "day completeness and duplicates", "100.0 %, 0 duplicates", "%.2f %%, %d duplicates" % (d0["complete"], d0["duplicates"]),
            d0["complete"] == 100.0 and d0["duplicates"] == 0, ("F7",))
    f9 = [e for e in entries if e["fault"] == 9]
    first9 = min((e["t"] / 1000 for e in f9), default=None)
    ok9 = bool(f9) and all(T(0, 10, 45) <= e["t"] / 1000 <= T(0, 16, 16) for e in f9)
    h.check("6", "fault 9 (upload failing > 60 min) visible in the status of samples logged late in the outage",
            ">= 1 sample with e=9, none outside 10:45-16:16", "%d samples, first at %s" % (len(f9), iso(first9)[11:16] if first9 else "-"), ok9, ("F7",))
    codes = sorted({r["status"] for r in up})
    h.check("7", "server answers during back-fill", "only HTTP 202", str(codes), codes == [202], ("F8",))
    standard_pipeline(h, T(1, 0, 20), [dayname(0)], "8")
    h.end()


def s3(h):
    h.begin("S3", "7.5-day outage, back-fill from the 8-day buffer, nightly job corrects late days")
    h.wifi_down = [(T(0, 6), T(7, 18))]
    rcs = []
    nightly_rc = []
    for d in range(1, 8):                       # nightly job keeps running while the device is offline
        rcs.append(h.dev(T(d, 0, 16)))
        nightly_rc.append(h.nightly(T(d, 0, 20))["rc"])
    rcs.append(h.dev(T(8, 0, 16)))
    h.check("1", "device runs 8 days with a 7.5-day outage (06:00 day 1 to 18:00 day 8)", "exit 0 x 8", str(rcs), not any(rcs))
    entries = h.entries()
    miss = missing_minutes(entries, T(0), T(8, 0, 14))
    total = (T(8, 0, 14) - T(0)) // 60 + 1
    h.check("2", "minutes missing in channel 1 after back-fill", "0 of %d" % total, "%d of %d" % (len(miss), total), not miss, ("F7",))
    out = [e for e in entries if T(0, 6) <= e["t"] / 1000 < T(7, 18)]
    done = max(e["rx"] for e in out) - T(7, 18)
    h.check("3", "outage samples buffered and delivered", "10800 samples (7.5 days), no overflow (0 x fault 6)",
            "%d unique samples, %d x fault 6, back-fill finished %s after Wi-Fi returned" % (len({e["seq"] for e in out}), fault_count(entries, 6), hm(done)),
            len({e["seq"] for e in out}) == 10800 and fault_count(entries, 6) == 0, ("F7",))
    up = uploads(h)
    back = [r for r in up if r["now"] >= T(7, 18)]
    h.check("4", "back-fill requests", "<= 960 entries each, no HTTP 429", "sizes %s, status codes %s" % ([r.get("updates") for r in back[:14]], sorted({r["status"] for r in back})),
            all(r.get("updates", 0) <= 960 for r in back) and all(r["status"] == 202 for r in back), ("F8",))
    days = [dayname(d) for d in range(8)]
    daily = h.daily(entries, T(8, 0, 20))
    before = h.agreement(daily, h.ch2(), days)
    stale = [d for d in days if before[d][0] != "agree"]
    h.check("5", "before the next nightly run: days whose channel-2 row is out of date (shown as 'differs'/'not yet summarised')",
            "8 (every outage day)", "%d: %s" % (len(stale), ", ".join("%s=%s" % (d[5:], before[d][0]) for d in days)), len(stale) == 8, ("F9",))
    n = h.nightly(T(8, 0, 20), ndays=7)          # the v1.4 setting, kept as regression evidence
    after = h.agreement(daily, h.ch2(), days)
    inwin = days[1:]
    bad = [d for d in inwin if after[d][0] != "agree"]
    h.check("6", "old v1.4 setting (AST_NDAYS = 7, %s): the 7 days inside its rewrite window" % h.nightly_kind, "7 of 7 agree",
            "%d of 7 agree (run took %.1f s)" % (7 - len(bad), n["wall_s"]), n["rc"] == 0 and not bad, ("F9",))
    old = after[days[0]]
    h.check("7", "old v1.4 setting: the first outage day (8 days old) stays stale - the defect that made v1.5 widen the window to 9 days",
            "differs (regression evidence)", "%s %s" % (old[0], old[1]), old[0] == "differ", ("F9",))
    res = h.dash([{"name": "t7", "now": T(8, 0, 21)}])
    if res is None:
        h.skip("7d", "dashboard view of the stale day", "browser not available", ("F9",))
    else:
        rows = dash_rows(res.get("t7"))
        flags = [rows.get(d, {}).get("_matlab", "no row") for d in days]
        h.check("7d", "dashboard 'MATLAB check' after the old 7-day run matches the reference verdicts", str([after[d][0] for d in days]),
                str(flags), flags == [after[d][0] for d in days], ("F9",))
    n9 = h.nightly(T(8, 0, 40))                   # production setting: NDAYS = 9 (contract 3.2, v1.5)
    after9 = h.agreement(daily, h.ch2(), days)
    bad9 = [d for d in days if after9[d][0] != "agree"]
    h.check("8", "production nightly job (NDAYS = 9, contract v1.5): all 8 complete days after the 7.5-day outage", "8 of 8 agree",
            "%d of 8 agree" % (8 - len(bad9)), n9["rc"] == 0 and not bad9, ("F9",))
    res = h.dash([{"name": "t9", "now": T(8, 0, 41)}])
    if res is None:
        h.skip("9", "dashboard after the 9-day nightly run", "browser not available", ("F9",))
    else:
        page = res.get("t9")
        rows = dash_rows(page)
        flags = [rows.get(d, {}).get("_matlab", "no row") for d in days]
        mism = table_mismatches(page, daily, days)
        h.check("9", "dashboard after the 9-day run: MATLAB check column and table values on the 8 days", "8 x agree, 0 value differences",
                "%d x agree, %d value differences%s" % (flags.count("agree"), len(mism), ("; " + mism[0]) if mism else ""),
                flags.count("agree") == 8 and not mism, ("F9",))
    h.end()


def s4(h):
    h.begin("S4", "Crash in the middle of an upload, restart from the same state directory")
    rc1 = h.dev(T(0, 5, 59))
    h.fault("drop_response", 1)                  # the 06:00:20 request is stored but never answered
    rc2 = h.dev(T(0, 8), crash_at=T(0, 6, 0, 30))
    stored_before = len(h.raw(CH1))
    rc3 = h.dev(T(1, 0, 16))
    h.check("1", "exit codes: clean stop, simulated crash at 06:00:30, clean run after restart", "0, 3, 0", "%d, %d, %d" % (rc1, rc2, rc3),
            (rc1, rc2, rc3) == (0, 3, 0))
    entries = h.entries()
    up = uploads(h)
    lost = [r for r in up if r["status"] == 0]
    h.check("2", "the request before the crash reached the server but got no answer", "1 request stored without reply",
            "%d (entries stored: %s); %d entries on the server at the crash" % (len(lost), [r.get("stored") for r in lost], stored_before), len(lost) == 1)
    miss = missing_minutes(entries, T(0), T(1, 0, 14))
    crash_minute = T(0, 6)                      # the device was off for part of 06:00-06:01, so no 60 s mean exists
    h.check("3", "minutes missing in channel 1 after the restart", "none except the minute of the crash itself (06:00)",
            "%d of 1455%s" % (len(miss), (": " + ", ".join(iso(t)[11:16] for t in miss[:3])) if miss else ""),
            all(t == crash_minute for t in miss), ("F7",))
    seqs = [e["seq"] for e in entries]
    by_seq = {}
    for e in entries:
        by_seq.setdefault(e["seq"], set()).add(e["t"])
    reused = [s for s, ts in by_seq.items() if len(ts) > 1]
    h.check("4", "sequence numbers survive the crash: contiguous and never reused for another minute", "1..%d, 0 reused" % len(by_seq),
            "%d..%d, %d distinct, %d reused" % (min(seqs), max(seqs), len(by_seq), len(reused)),
            sorted(by_seq) == list(range(1, len(by_seq) + 1)) and not reused, ("F7",))
    dups = len(entries) - len(by_seq)
    h.check("5", "the batch sent before the crash is sent again (duplicates in channel 1)", ">= 1 duplicate entry", "%d duplicate entries" % dups, dups >= 1)
    daily = h.daily(entries, T(1, 0, 20))
    d0 = daily[dayname(0)]
    want_n = 1440 - len([t for t in miss if t < T(1)])
    h.check("6", "reference metrics after de-duplication (contract 6.1)", "N = %d (every logged sample once), completeness >= 99.9 %%" % want_n,
            "n = %d received, N = %d, duplicates = %d, completeness %.2f %%" % (d0["n"], d0["n_unique"], d0["duplicates"], d0["complete"]),
            d0["n_unique"] == want_n and want_n >= 1439 and d0["duplicates"] == dups, ("F7",))
    _, _, ch2, _ = standard_pipeline(h, T(1, 0, 20), [dayname(0)], "7")
    row = ch2.get(dayname(0)) or {}
    h.check("8", "channel-2 status reports the dropped duplicates (nd=)", "nd=%d" % dups, "nd=%s" % row.get("duplicates"), row.get("duplicates") == dups, ("F9",))
    h.end()


def lost_responses(h, sid, title, policy):
    h.begin(sid, title)
    h.admin("POST", "/__admin/config", {"existing_timestamp_policy": policy})
    h.fault("drop_response", 2)
    rc1 = h.dev(T(0, 11, 59))
    h.fault("drop_response", 1)
    rc2 = h.dev(T(1, 0, 16))
    h.check("1", "device runs a day with 3 lost responses (2 at 00:15, 1 at 12:00)", "exit 0, 0", "%d, %d" % (rc1, rc2), (rc1, rc2) == (0, 0))
    entries = h.entries()
    up = uploads(h)
    lost = [r for r in up if r["status"] == 0]
    h.check("2", "requests processed by the server without a reply", "3", str(len(lost)), len(lost) == 3)
    by_seq = {}
    for e in entries:
        by_seq.setdefault(e["seq"], []).append(e)
    dups = len(entries) - len(by_seq)
    miss = missing_minutes(entries, T(0), T(1, 0, 14))
    if policy == "store":
        h.check("3", "duplicates in channel 1 (same seq stored twice)", ">= 1", "%d duplicate entries, %d entries for %d sequence numbers" % (dups, len(entries), len(by_seq)), dups >= 1)
    else:
        h.check("3", "server policy 'reject existing timestamps': duplicates in channel 1", "0", "%d duplicate entries (%d re-sent entries dropped by the server)" %
                (dups, sum(r.get("dup_ts", 0) for r in up)), dups == 0)
    h.check("4", "minutes missing in channel 1", "0 of 1455", "%d of 1455" % len(miss), not miss, ("F7",))
    # independent check that duplicates do not change the result: first copy per seq, summed here
    day0 = [e for e in entries if am.utc_day(e["t"]) == dayname(0)]
    first = {}
    for e in sorted(day0, key=lambda e: e["entry_id"]):
        first.setdefault(e["seq"], e)
    truth = [sum(e[k] or 0 for e in first.values()) / 60000.0 for k in ("p_trk", "p_fix", "p_use")]
    naive = [sum(e[k] or 0 for e in day0) / 60000.0 for k in ("p_trk", "p_fix", "p_use")]
    d0 = h.daily(entries, T(1, 0, 20))[dayname(0)]
    ok = all(abs(a - b) < 1e-9 for a, b in zip(truth, (d0["e_trk"], d0["e_fix"], d0["e_use"]))) and d0["n_unique"] == 1440
    h.check("5", "metrics unaffected by duplicates: reference energies equal a first-copy-per-seq sum", "E_trk/E_fix/E_use equal, N = 1440",
            "E = %.4f/%.4f/%.4f Wh (sum over all received copies would be %.4f/%.4f/%.4f), N = %d, n = %d" %
            (d0["e_trk"], d0["e_fix"], d0["e_use"], naive[0], naive[1], naive[2], d0["n_unique"], d0["n"]), ok, ("F9",))
    _, daily, ch2, page = standard_pipeline(h, T(1, 0, 20), [dayname(0)], "6")
    row = ch2.get(dayname(0)) or {}
    h.check("7", "channel-2 row: N unique and duplicates dropped", "field8 = 1440, nd=%d" % d0["duplicates"],
            "field8 = %s, nd=%s" % ("%g" % row["n"] if row.get("n") is not None else None, row.get("duplicates")), row.get("n") == 1440 and row.get("duplicates") == d0["duplicates"], ("F9",))
    if page is not None and policy == "store":
        notes = " | ".join(page.get("quality") or [])
        res = h.dash([{"name": "day", "now": T(1, 0, 21), "day": dayname(0)}]) or {}
        notes = " | ".join((res.get("day") or {}).get("quality") or [])
        h.check("8", "dashboard tells the user about the duplicates (data-quality note for the day)", "note mentioning %d duplicates" % d0["duplicates"],
                (re.findall(r"[^|]*duplicate[^|]*", notes) or ["no duplicate note"])[0].strip()[:110],
                bool(re.search(r"\b%d\b[^|]*duplicate" % d0["duplicates"], notes)), ("F9",))
    h.end()


def s5(h):
    lost_responses(h, "S5", "Lost responses -> duplicates in channel 1, metrics unaffected", "store")


def s5b(h):
    lost_responses(h, "S5b", "Lost responses with the server rejecting existing timestamps (assumed real behaviour)", "reject")


def s6(h):
    h.begin("S6", "HTTP 500 burst -> backoff 1, 2, 4, 8, 15 min")
    rc1 = h.dev(T(0, 2, 50))
    h.fault("http500", 8)
    rc2 = h.dev(T(0, 6))
    h.check("1", "device runs through 8 consecutive HTTP 500 answers", "exit 0, 0", "%d, %d" % (rc1, rc2), (rc1, rc2) == (0, 0))
    up = uploads(h)
    fails = [r for r in up if r["status"] == 500]
    after = [r for r in up if fails and r["now"] > fails[-1]["now"]]
    seq_t = [r["now"] for r in fails] + ([after[0]["now"]] if after else [])
    gaps = [round(b - a) for a, b in zip(seq_t, seq_t[1:])]
    want = BACKOFF_S + [900, 900, 900]
    ok = len(fails) == 8 and len(gaps) == 8 and all(abs(g - w) <= 5 for g, w in zip(gaps, want))
    h.check("2", "waits between attempts after each failure", "1, 2, 4, 8, 15, 15, 15, 15 min", ", ".join("%g" % (g / 60.0) for g in gaps) + " min",
            ok, ("F8",))
    h.check("3", "first failure at the regular slot, recovery with HTTP 202", "03:00:20, then 202", "%s, then %s" %
            (iso(fails[0]["now"])[11:19] if fails else "-", after[0]["status"] if after else "-"),
            bool(fails) and int(fails[0]["now"]) == T(0, 3, 0, 20) and bool(after) and after[0]["status"] == 202, ("F8",))
    entries = h.entries()
    miss = missing_minutes(entries, T(0), T(0, 5, 44))
    dup = len(entries) - len({e["seq"] for e in entries})
    h.check("4", "no sample lost or duplicated by the failed uploads", "0 missing, 0 duplicates", "%d missing, %d duplicates" % (len(miss), dup), not miss and dup == 0, ("F7",))
    f9 = [e for e in entries if e["fault"] == 9]
    last_ok_before = max(r["now"] for r in up if r["status"] == 202 and r["now"] < fails[0]["now"]) if fails else 0
    recov = after[0]["now"] if after else 0
    ok9 = bool(f9) and all(last_ok_before + 3600 - 60 <= e["t"] / 1000 <= recov + 60 for e in f9)
    h.check("5", "fault 9 appears only once no upload has succeeded for 60 min, and clears after recovery",
            ">= 1 sample with e=9, all between %s and %s" % (iso(last_ok_before + 3600)[11:16], iso(recov)[11:16]),
            "%d samples with e=9, %s to %s" % (len(f9), iso(min(e["t"] for e in f9) / 1000)[11:16] if f9 else "-", iso(max(e["t"] for e in f9) / 1000)[11:16] if f9 else "-"),
            ok9, ("F7",))
    h.check("6", "the backlog after recovery is sent in one request", "1 request with 75-91 entries", "%s entries" % (after[0].get("updates") if after else "-"),
            bool(after) and 75 <= after[0].get("updates", 0) <= 91, ("F8",))
    h.end()


def s7(h):
    h.begin("S7", "Large backlog (3 days offline): request spacing and batch size")
    h.wifi_down = [(T(0, 2), T(3, 2))]
    rc = h.dev(T(3, 3))
    h.check("1", "device runs 3 days offline, then back-fills", "exit 0", "exit %d" % rc, rc == 0)
    up = uploads(h)
    back = [r for r in up if r["now"] >= T(3, 2)]
    n429 = sum(1 for r in up if r["status"] == 429)
    h.check("2", "HTTP 429 (too many requests) answers from the server", "0", "%d of %d requests" % (n429, len(up)), n429 == 0, ("F8",))
    sizes = [r.get("updates", 0) for r in back]
    h.check("3", "entries per back-fill request", "<= 960, backlog of 4320+ sent in >= 5 requests", "max %d; first requests %s" % (max(sizes), sizes[:7]),
            max(sizes) <= 960 and sizes[:4] == [960] * 4, ("F8",))
    gaps = [b["now"] - a["now"] for a, b in zip(up, up[1:])]
    h.check("4", "shortest time between two requests to channel 1", ">= 16 s", "%.0f s (over %d requests)" % (min(gaps), len(up)), min(gaps) >= 16, ("F8",))
    entries = h.entries()
    miss = missing_minutes(entries, T(0), T(3, 2, 44))
    dup = len(entries) - len({e["seq"] for e in entries})
    h.check("5", "channel 1 after back-fill", "0 missing minutes, 0 duplicates", "%d missing of %d, %d duplicates" % (len(miss), (T(3, 2, 44) - T(0)) // 60 + 1, dup),
            not miss and dup == 0, ("F7",))
    first_back = back[0]["now"] - T(3, 2)
    chain = [r for r in back if r.get("updates", 0) == 960]
    last_big = max(r["now"] for r in back if r.get("updates", 0) > 100) - T(3, 2)
    h.check("6", "back-fill time after Wi-Fi returned", "first request <= 15 min, backlog cleared <= 20 min",
            "first request after %s, backlog cleared after %s (%d full batches)" % (hm(first_back), hm(last_big), len(chain)),
            first_back <= 900 and last_big <= 1200, ("F8",))
    h.end()


def s8(h):
    h.begin("S8", "Offline longer than the 8-day buffer: loss is flagged (fault 6) and counted")
    h.wifi_down = [(T(0, 1), T(9, 1))]
    rc = h.dev(T(9, 3))
    h.check("1", "device runs 9 days offline (buffer holds 8), then reconnects", "exit 0", "exit %d" % rc, rc == 0)
    entries = h.entries()
    miss = missing_minutes(entries, T(0), T(9, 2, 44))
    seqs = sorted({e["seq"] for e in entries})
    seq_gap = (seqs[-1] - seqs[0] + 1) - len(seqs)
    runs = 1 + sum(1 for a, b in zip(miss, miss[1:]) if b - a != 60) if miss else 0
    h.check("2", "minutes lost (not in channel 1)", "1440 to 1500 in one block (9 days offline + up to 1 h, minus the 11520-record buffer)",
            "%d minutes in %d block(s): %s to %s" % (len(miss), runs, iso(miss[0]) if miss else "-", iso(miss[-1]) if miss else "-"),
            1440 <= len(miss) <= 1500 and runs == 1, ("F7",))
    h.check("3", "loss counted by sequence numbers (never reused, so the gap equals the loss)", "seq gap = %d" % len(miss), "seq gap = %d (seq %d..%d, %d received)" %
            (seq_gap, seqs[0], seqs[-1], len(seqs)), seq_gap == len(miss), ("F7",))
    f6 = [e for e in entries if e["fault"] == 6]
    h.check("4", "fault 6 (buffer overflow, data lost) visible in channel-1 status", ">= 1 sample with e=6", "%d samples with e=6, %s to %s" %
            (len(f6), iso(min(e["t"] for e in f6) / 1000) if f6 else "-", iso(max(e["t"] for e in f6) / 1000) if f6 else "-"), len(f6) >= 1, ("F7",))
    exits = [ev for ev in h.device_events("exit") if "lost" in ev]
    over = h.device_events("overflow")
    if exits or over:
        counted = exits[-1]["lost"] if exits else len(over)
        h.check("5", "the device's own count of overwritten records (device log)", "%d (= minutes missing on the server)" % len(miss), str(counted),
                counted == len(miss), ("F7",))
    else:
        h.skip("5", "the device's own count of overwritten records", "this device build does not log it", ("F7",))
    now = T(9, 2, 50)
    daily = h.daily(entries, now)
    lost_days = sorted({am.utc_day(t * 1000) for t in miss})
    ref_missing = sum(1440 - daily[d]["n_unique"] if d in daily else 1440 for d in lost_days)
    h.check("6", "reference completeness shows the loss on the affected days", "missing samples on %s add up to %d" % (", ".join(lost_days), len(miss)),
            "%d (%s)" % (ref_missing, ", ".join("%s: %.1f %%" % (d[5:], daily[d]["complete"]) if d in daily else "%s: no data" % d[5:] for d in lost_days)),
            ref_missing == len(miss), ("F7",))
    f6day = am.utc_day(min(e["t"] for e in f6)) if f6 else dayname(8)
    res = h.dash([{"name": "table", "now": now}, {"name": "lost", "now": now, "day": lost_days[0] if lost_days else dayname(0)},
                  {"name": "f6", "now": now, "day": f6day}])
    if res is None:
        h.skip("7", "dashboard shows the loss", "browser not available", ("F7", "F9"))
    else:
        rows = dash_rows(res.get("table"))
        shown = {d: num(rows.get(d, {}).get("complete")) for d in lost_days if d in daily}
        okc = all(v is not None and same(v, num(am.fmt_fixed(daily[d]["complete"]))) and v < 99 for d, v in shown.items()) and bool(shown)
        h.check("7", "dashboard daily table: completeness of the affected day(s)", "< 99 % and equal to the reference",
                ", ".join("%s: %s %%" % (d[5:], rows.get(d, {}).get("complete")) for d in shown) or "no rows", okc, ("F7", "F9"))
        lost = res.get("lost") or {}
        dl = daily.get(lost_days[0]) if lost_days else None
        notes = " | ".join(lost.get("quality") or [])
        want_missing = dl["missing_min"] if dl else 0
        knote = ((lost.get("kpis") or {}).get("complete") or {}).get("note") or ""
        h.check("8", "dashboard day view of %s: completeness card states received/expected samples" % (lost_days[0] if lost_days else "-"),
                "%d of 1440" % (dl["n_unique"] if dl else 0), knote[:70], bool(dl) and bool(re.search(r"\b%d of 1440\b" % dl["n_unique"], knote)), ("F7", "F9"))
        f6p = res.get("f6") or {}
        n6 = sum(1 for e in f6 if am.utc_day(e["t"]) == f6day)
        notes6 = " | ".join(f6p.get("quality") or [])
        hit = [t for t in notes6.split(" | ") if re.search(r"fault", t, re.I) and re.search(r"\b6\b", t)]
        h.check("9", "dashboard day view of %s: fault 6 named in the data-quality notes" % f6day, "note with fault code 6 (%d samples that day)" % n6,
                (hit[0][:110] if hit else "no fault-6 note; notes: %s" % notes6[:80]), bool(hit), ("F7", "F9"))
    h.end()


def s9(h):
    h.begin("S9", "RTC lost time at boot, no network for 3 h")
    h.rtc_lost = True
    h.wifi_down = [(T(0), T(0, 3))]
    rc = h.dev(T(0, 6, 31))
    h.check("1", "device boots with the RTC oscillator-stop flag and no Wi-Fi until 03:00", "exit 0", "exit %d" % rc, rc == 0)
    entries = h.entries()
    ts = sorted(e["t"] // 1000 for e in entries)
    early = [t for t in ts if t < T(0, 3) - 60]
    late = [t for t in ts if t > T(0, 6, 31)]
    h.check("2", "entries with a timestamp from before the clock was valid (or outside the test period)", "0", "%d before 02:59, %d after the end; first entry %s" %
            (len(early), len(late), iso(ts[0]) if ts else "-"), bool(ts) and not early and not late, ("F7",))
    h.check("3", "logging resumes once NTP has set the clock", "first entry between 02:59 and 03:02", iso(ts[0])[11:16] if ts else "no entries",
            bool(ts) and T(0, 3) - 60 <= ts[0] <= T(0, 3, 2), ("F7",))
    miss = missing_minutes(entries, ts[0], T(0, 6, 29)) if ts else [0]
    h.check("4", "after the clock is valid: minutes missing up to 06:29", "0", "%d of %d" % (len(miss), (T(0, 6, 29) - ts[0]) // 60 + 1 if ts else 0), not miss, ("F7",))
    f4 = [e for e in entries if e["fault"] == 4]
    h.check("5", "fault 4 (clock lost) visible in channel-1 status", ">= 1 sample with e=4", "%d sample(s) with e=4%s" %
            (len(f4), (", first at " + iso(min(e["t"] for e in f4) / 1000)[11:16]) if f4 else ""), len(f4) >= 1, ("F7",))
    seqs = [e["seq"] for e in sorted(entries, key=lambda e: e["t"])]
    h.check("6", "sequence numbers of the logged samples", "contiguous, no reuse", "%s..%s, %d distinct of %d" % (seqs[0] if seqs else "-", seqs[-1] if seqs else "-", len(set(seqs)), len(seqs)),
            bool(seqs) and seqs == list(range(seqs[0], seqs[0] + len(seqs))), ("F7",))
    res = h.dash([{"name": "day", "now": T(0, 6, 31), "day": dayname(0)}])
    if res is None:
        h.skip("7", "dashboard names fault 4", "browser not available", ("F9",))
    else:
        notes = (res.get("day") or {}).get("quality") or []
        hit = [t for t in notes if re.search(r"fault", t, re.I) and re.search(r"\b4\b", t)]
        h.check("7", "dashboard day view: fault 4 named in the data-quality notes", "note with fault code 4", (hit[0][:110] if hit else "no fault-4 note; notes: %s" % " | ".join(notes)[:80]),
                bool(hit), ("F9",))
    h.end()


def s10(h):
    h.begin("S10", "Dashboard vs nightly job on every complete day (5 different days), two phones, CSV export")
    rcs = [h.dev(T(1), scenario="clear", run=0, panel="A")]
    h.fault("drop_response", 2)
    rcs.append(h.dev(T(2), scenario="cloudy", run=12, panel="A"))
    h.wifi_down = [(T(2, 9), T(2, 12))]
    rcs.append(h.dev(T(3), scenario="overcast", run=13, panel="B"))
    rcs.append(h.dev(T(4), scenario="night", run=0, panel="B"))
    rcs.append(h.dev(T(5, 0, 16), scenario="clear", run=900, panel="A"))
    h.check("1", "5 days: clear r=0 / cloudy r=12 panel A with lost responses / overcast r=13 panel B with a 3 h outage / dark (no power) / calibration r=900",
            "exit 0 x 5", str(rcs), not any(rcs))
    days = [dayname(d) for d in range(5)]
    now = T(5, 0, 20)
    entries, daily, ch2, page = standard_pipeline(h, now, days, "2")
    dark = daily[dayname(3)]
    h.check("3", "edge cases present in the data set", "duplicates > 0; a day with E_fix = 0 (gains undefined); 5 x 1440 unique samples",
            "duplicates %d; dark day E_fix = %.3f Wh, gross = %s; N = %s" % (sum(daily[d]["duplicates"] for d in days), dark["e_fix"], dark["g_gross"], [daily[d]["n_unique"] for d in days]),
            sum(daily[d]["duplicates"] for d in days) > 0 and dark["g_gross"] is None and all(daily[d]["n_unique"] == 1440 for d in days), ("F9",))
    row = ch2.get(dayname(3)) or {}
    h.check("4", "channel-2 row of the dark day omits both gains (contract 3.2)", "field4, field5 empty", "field4 = %s, field5 = %s" % (row.get("g_gross"), row.get("g_net")),
            bool(row) and row.get("g_gross") is None and row.get("g_net") is None, ("F9",))
    if not h.browser:
        h.skip("5", "KPI cards, two phones, CSV export, key hygiene", "browser not available", ("F9", "F8"))
        h.end()
        return
    pages = [{"name": "kpi_" + d, "now": now + 60, "day": d} for d in days]
    pages.append({"name": "phoneA", "now": now + 60, "day": days[4], "device": "iPhone 13"})
    pages.append({"name": "phoneB", "now": now + 60, "day": days[4], "device": "Pixel 7"})
    pages.append({"name": "csv", "now": now + 60, "csv": True})
    res = h.dash(pages)
    bad = []
    for d in days:
        for m in kpi_mismatches(res.get("kpi_" + d), daily[d]):
            bad.append("%s %s" % (d[5:], m))
    h.check("5", "KPI cards of each of the 5 days equal the reference (7 figures per day)", "0 differences in 35 figures", "%d differences%s" % (len(bad), ("; " + "; ".join(bad[:3])) if bad else ""),
            not bad, ("F9",))
    a, b = res.get("phoneA") or {}, res.get("phoneB") or {}
    ka = {k: v.get("value") for k, v in (a.get("kpis") or {}).items()}
    kb = {k: v.get("value") for k, v in (b.get("kpis") or {}).items()}
    ta = [[c["text"] for c in r["cells"]] for r in (a.get("daily") or {}).get("rows", [])]
    tb = [[c["text"] for c in r["cells"]] for r in (b.get("daily") or {}).get("rows", [])]
    h.check("6", "two emulated phones (iPhone 13 / Pixel 7 profiles) show the same figures", "8 KPI values and the daily table identical",
            "%d KPI values compared, %d differ; %d table rows compared, %s" % (len(ka), sum(1 for k in ka if ka[k] != kb.get(k)), len(ta), "identical" if ta == tb else "DIFFERENT"),
            page_ok(a) and page_ok(b) and len(ka) >= 7 and ka == kb and ta == tb and len(ta) >= 5, ("F9",))
    # CSV export against what the device sent (channel-1 store) and the device log
    c = (res.get("csv") or {}).get("csv") or {}
    text = c.get("text")
    if not text:
        h.check("7", "raw-samples CSV export", "CSV with one row per received entry", "no CSV obtained (%s)" % (c.get("error") or (res.get("csv") or {}).get("exportStatus")), False, ("F9",))
    else:
        import csv as _csv
        import io
        rd = list(_csv.reader(io.StringIO(text.lstrip("\ufeff"))))
        header, body = rd[0], [r for r in rd[1:] if r]
        want_header = am.RAW_CSV_HEADER
        col = {name: i for i, name in enumerate(header)}
        by_id = {str(e["entry_id"]): e for e in entries}
        diffs = 0
        first_diff = ""
        if header == want_header:
            for r in body:
                e = by_id.get(r[col["entry_id"]])
                if e is None:
                    diffs += 1
                    first_diff = first_diff or "entry %s not on the server" % r[col["entry_id"]]
                    continue
                pairs = [("seq", e["seq"]), ("p_trk_mw", e["p_trk"]), ("p_fix_mw", e["p_fix"]), ("p_use_mw", e["p_use"]),
                         ("ang_panel_deg", e["ang_panel"]), ("ang_sun_deg", e["ang_sun"]), ("temp_c", e["temp"]), ("v_supply_v", e["v_supply"])]
                okrow = r[col["created_at_utc"]] == e["row"]["created_at"] and r[col["status"]] == (e["status"] or "")
                for name, v in pairs:
                    cell = r[col[name]]
                    okrow = okrow and ((cell == "" and v is None) or (cell != "" and v is not None and abs(float(cell) - float(v)) < 1e-9))
                if not okrow:
                    diffs += 1
                    first_diff = first_diff or "entry %s differs" % r[col["entry_id"]]
        h.check("7", "raw-samples CSV export (%s) equals channel 1: header as agreed in PLAN.md, one row per received entry, every value equal" % c.get("via"),
                "%d rows, 0 differences" % len(entries), "%d rows, %d differences%s%s" % (len(body), diffs, "" if header == want_header else "; header differs", ("; " + first_diff) if first_diff else ""),
                header == want_header and len(body) == len(entries) and diffs == 0, ("F9",))
        samples = {ev["seq"]: ev["entry"] for ev in h.device_events("sample") if "seq" in ev and isinstance(ev.get("entry"), dict)}
        if samples and header == want_header:
            names = ["p_trk_mw", "p_fix_mw", "p_use_mw", "ang_panel_deg", "ang_sun_deg", "temp_c", "v_supply_v"]
            d2 = 0
            first_d2 = ""
            for r in body:
                if not r[col["seq"]]:
                    continue
                ent = samples.get(int(r[col["seq"]]))
                okrow = ent is not None
                if okrow:
                    okrow = iso(am.parse_time(ent["created_at"]) / 1000) == r[col["created_at_utc"]] and ent.get("status", "") == r[col["status"]]
                    for i, name in enumerate(names, start=1):
                        v, cell = ent.get("field%d" % i), r[col[name]]
                        okrow = okrow and ((v is None and cell == "") or (v is not None and cell != "" and abs(float(v) - float(cell)) < 1e-9))
                if not okrow:
                    d2 += 1
                    first_d2 = first_d2 or "seq %s" % r[col["seq"]]
            unseen = len(set(samples) - {int(r[col["seq"]]) for r in body if r[col["seq"]]})
            h.check("8", "CSV export equals the device's own log: minute, 7 values and status of every logged sample (%d log lines)" % len(samples),
                    "0 rows differ; only samples logged after the last upload are absent (<= 16)",
                    "%d of %d rows differ%s; %d logged samples not yet uploaded" % (d2, len(body), ("; first " + first_d2) if first_d2 else "", unseen),
                    d2 == 0 and unseen <= 16, ("F9",))
        else:
            h.skip("8", "CSV export against the device's own log", "this device build writes no per-sample log lines", ("F9",))
    reqs = [r for p in res.values() for r in p.get("requests", [])]
    api = [r for r in reqs if h.base in r["url"]]
    leak = [r for r in reqs if W1 in r["url"] or W2 in r["url"]]
    writes = [r for r in api if r["method"] not in ("GET", "OPTIONS")]
    h.check("9", "dashboard uses READ access only", "0 requests containing a write key, 0 non-GET requests to the API",
            "%d API requests: %d with a write key, %d non-GET" % (len(api), len(leak), len(writes)), bool(api) and not leak and not writes, ("F8",))
    errs = sum(len(p.get("pageErrors") or []) for p in res.values())
    h.check("10", "JavaScript errors on the %d pages opened" % len(res), "0", "%d%s" % (errs, ("; " + [e for p in res.values() for e in p.get("pageErrors", [])][0][:80]) if errs else ""), errs == 0, ("F9",))
    h.end()


def s11(h):
    h.begin("S11", "Completeness on normal days (including a short Wi-Fi drop)")
    h.wifi_down = [(T(1, 13, 5), T(1, 13, 50))]
    rcs = [h.dev(T(1), scenario="clear"), h.dev(T(2), scenario="cloudy"), h.dev(T(3, 0, 16), scenario="overcast")]
    h.check("1", "3 days (clear, cloudy, overcast); day 2 has a 45-minute Wi-Fi drop", "exit 0 x 3", str(rcs), not any(rcs))
    days = [dayname(d) for d in range(3)]
    now = T(3, 0, 20)
    entries, daily, ch2, page = standard_pipeline(h, now, days, "2", req=("F7", "F9"))
    comp = [daily[d]["complete"] for d in days]
    h.check("3", "reference completeness per day", ">= 99 %", ", ".join("%.2f %%" % c for c in comp), all(c >= 99.0 for c in comp), ("F7",))
    c2 = [(ch2.get(d) or {}).get("complete") for d in days]
    h.check("4", "channel-2 field7 (completeness written by the nightly job)", ">= 99 %", ", ".join("%s %%" % c for c in c2), all(c is not None and c >= 99.0 for c in c2), ("F7",))
    if page is None:
        h.skip("5", "dashboard completeness", "browser not available", ("F7",))
    else:
        rows = dash_rows(page)
        shown = [num(rows.get(d, {}).get("complete")) for d in days]
        h.check("5", "dashboard 'Complete %' column", ">= 99 %", ", ".join("%s %%" % s for s in shown), all(s is not None and s >= 99.0 for s in shown), ("F7",))
    h.end()


def s12(h):
    h.begin("S12", "Sample-to-screen latency (virtual time)")
    rc = h.dev(T(0, 12, 16))                    # stops just after the 12:15:20 upload
    h.check("1", "device runs from 00:00 to 12:16", "exit 0", "exit %d" % rc, rc == 0)
    entries = h.entries()
    up = uploads(h)
    lat = [e["rx"] - (e["t"] / 1000 + 60) for e in entries]
    worst_server = max(lat)
    h.check("2", "end of minute -> on the server (F8 target 16 min)", "<= 16 min 00 s", "max %s, mean %s (n=%d)" % (hm(worst_server), hm(sum(lat) / len(lat)), len(lat)),
            worst_server <= 960, ("F8",))
    last_up = max(r["now"] for r in up)
    newest = max(e["t"] for e in entries) // 1000
    if not h.browser:
        h.skip("3", "dashboard part of the latency", "browser not available", ("F9",))
        h.end()
        return
    res = h.dash([{"name": "screen", "now": last_up + 60},
                  {"name": "auto", "now": last_up + 60, "extra": "&refresh=15", "watchMs": 20000}])
    page = res.get("screen") or {}
    refresh = ((page.get("defaults") or {}).get("refreshS")) or 60
    today = dash_rows(page).get(dayname(0), {})
    n_today = sum(1 for e in entries if am.utc_day(e["t"]) == dayname(0))
    h.check("3", "dashboard opened one refresh period (%d s) after the upload shows every uploaded sample of today" % refresh, "Samples = %d, status 'fresh'" % n_today,
            "Samples = %s, status '%s', page ready in %.1f s" % (today.get("n"), page.get("fresh"), page.get("loadMs", 0) / 1000.0),
            page_ok(page) and same(num(today.get("n")), float(n_today)) and page.get("fresh") == "fresh", ("F9",))
    auto = res.get("auto") or {}
    h.check("4", "dashboard refreshes by itself (page left open 20 s with refresh=15 s)", ">= 1 automatic reload of the data", "%s data requests after the first load" %
            (auto.get("watch") or {}).get("feedRequests"), ((auto.get("watch") or {}).get("feedRequests") or 0) >= 1, ("F9",))
    load_s = page.get("loadMs", 0) / 1000.0
    total = worst_server + refresh + load_s
    h.check("5", "worst-case sample-to-screen = server latency + refresh period + page update", "<= 17 min 00 s",
            "%s = %s + %d s + %.1f s" % (hm(total), hm(worst_server), refresh, load_s), total <= 1020, ("F9",))
    h.check("6", "newest uploaded sample (on screen, see check 3) when the dashboard is opened 60 s after an upload", "sample of 12:14, age <= 3 min", "sample of %s, age %s" %
            (iso(newest)[11:16], hm(last_up + 60 - (newest + 60))), newest == T(0, 12, 14) and last_up + 60 - (newest + 60) <= 180, ("F9",))
    h.end()


SCENARIOS = [("S1", s1), ("S2", s2), ("S3", s3), ("S4", s4), ("S5", s5), ("S5b", s5b), ("S6", s6), ("S7", s7), ("S8", s8),
             ("S9", s9), ("S10", s10), ("S11", s11), ("S12", s12)]

PROVES = {
    "S1": "normal operation: 1 sample/min, contract formats, 15-minute uploads, nightly summary, dashboard table",
    "S2": "a 6 h Wi-Fi outage loses nothing; fault 9 is raised; back-fill is quick",
    "S3": "a 7.5-day outage fits the 8-day buffer; the nightly job repairs the late days it still rewrites",
    "S4": "a crash between 'request sent' and 'acknowledgement stored' loses nothing; duplicates are removed by seq",
    "S5": "lost HTTP responses create duplicates in channel 1 that do not change any metric",
    "S5b": "same as S5 when the server drops entries whose timestamp already exists (assumed real ThingSpeak behaviour)",
    "S6": "server errors: retry waits 1, 2, 4, 8, 15 min, fault 9 after 60 min, nothing lost",
    "S7": "a large backlog is sent in batches of <= 960 at >= 16 s spacing, so ThingSpeak never answers 429",
    "S8": "an outage longer than the buffer loses the oldest data visibly: fault 6, sequence gap, completeness",
    "S9": "after the RTC lost time nothing is logged with a wrong timestamp; fault 4 is reported",
    "S10": "dashboard, nightly job and Python reference agree on every complete day; two phones; CSV export; read-only keys",
    "S11": "completeness >= 99 % on normal days",
    "S12": "a sample is on the screen <= 17 min after its minute ends",
}


# ---------------------------------------------------------------- reports

def md_escape(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def write_reports(h, started, selected):
    os.makedirs(RESULTS, exist_ok=True)
    counts = {k: sum(1 for c in h.checks if c["status"] == k) for k in ("PASS", "FAIL", "SKIP")}
    octave_walls = [n["wall_s"] for n in h.nightly_runs]
    meta = {
        "generated_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "contract": am.CONTRACT_VERSION,
        "scenarios_run": selected,
        "device": h.device_label, "device_kind": h.device_kind,
        "nightly": h.nightly_label, "nightly_kind": h.nightly_kind,
        "browser": h.browser_label,
        "server": "tools/mock_thingspeak.py (mock; HTTP on 127.0.0.1; virtual time)",
        "reference": "analysis/py/ast_metrics.py",
        "virtual_start": iso(T0),
        "wall_seconds": round(time.time() - started, 1),
        "scenario_wall_seconds": h.timing,
        "device_invocations": h.device_runs, "nightly_invocations": len(h.nightly_runs), "dashboard_pages_opened": h.browser_pages,
        "nightly_wall_s_max": max(octave_walls) if octave_walls else None,
        "counts": counts,
    }
    out = {"meta": meta, "checks": h.checks, "findings": h.findings, "nightly_runs": h.nightly_runs}
    with open(os.path.join(RESULTS, "e2e_results.json"), "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1)
    lines = ["# AST data link - end-to-end test results", "",
             "Generated %s by `tests/e2e/run_e2e.py` (contract v%s). Virtual time starts at %s." % (meta["generated_utc"], meta["contract"], iso(T0)), "",
             "| Component | Used in this run |", "|---|---|",
             "| Device | %s |" % h.device_label, "| Server | %s |" % meta["server"], "| Nightly job | %s |" % h.nightly_label,
             "| Reference | %s |" % meta["reference"], "| Dashboard | %s |" % h.browser_label, "",
             "**%d checks: %d PASS, %d FAIL, %d not run.** Wall time %.0f s (%d device runs, %d nightly runs%s, %d dashboard pages)." %
             (len(h.checks), counts["PASS"], counts["FAIL"], counts["SKIP"], meta["wall_seconds"], h.device_runs, len(h.nightly_runs),
              (", slowest %.1f s" % max(octave_walls)) if octave_walls else "", h.browser_pages), ""]
    for sid in selected:
        cs = [c for c in h.checks if c["scenario"] == sid]
        if not cs:
            continue
        verdict = "FAIL" if any(c["status"] == "FAIL" for c in cs) else "PASS"
        lines += ["## %s - %s: %s" % (sid, cs[0]["title"], verdict), "", "Proves: %s." % PROVES.get(sid, ""), "",
                  "| Check | What | Expected | Measured | Result |", "|---|---|---|---|---|"]
        for c in cs:
            lines.append("| %s | %s | %s | %s | %s |" % (c["id"], md_escape(c["check"]), md_escape(c["expected"]), md_escape(c["measured"]), c["status"]))
        lines.append("")
    if h.findings:
        lines += ["## Findings", ""]
        for f in h.findings:
            lines.append("- **%s** (%s): %s" % (f["component"], f["scenario"], f["text"]))
        lines.append("")
    with open(os.path.join(RESULTS, "e2e_summary.md"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    return meta


VV_ROWS = [
    # (requirement, acceptance target (contract 11), method, check ids, bench note)
    ("F7", ">= 99 % of expected samples per day reach channel 1",
     "S1 and S11: device run for 3 virtual days (S11 with a 45 min Wi-Fi drop); samples counted per UTC day in channel 1, by the Python reference, by the nightly job (ch2 field7) and on the dashboard",
     ["S1.2", "S11.3", "S11.4", "S11.5"], None),
    ("F7", "No loss of logged samples for a 7-day outage (8-day buffer) or a power cut; the minute being averaged when the power fails is lost (contract 11)",
     "S3: Wi-Fi off for 7.5 virtual days, then every minute of the 8 days looked up in channel 1; S2 (6 h), S4 (crash during upload), S7 (3 days) the same way",
     ["S3.2", "S3.3", "S2.3", "S4.3", "S7.5"], None),
    ("F7", "Loss beyond the buffer is visible, never silent (contract 7.2)",
     "S8: Wi-Fi off for 9 virtual days; missing minutes counted from timestamps and from the sequence-number gap; fault 6 looked for in channel 1 and on the dashboard",
     ["S8.2", "S8.3", "S8.4", "S8.7", "S8.9"], None),
    ("F7", "Timestamps within 2 s of UTC",
     "Simulation can only show the logic: every created_at is a whole UTC minute (S1) and nothing is logged while the RTC is invalid (S9). The 2 s figure needs the real DS3231 and NTP",
     ["S1.4", "S9.2", "S9.3", "S9.5"], "BENCH: procedure B3 below"),
    ("F8", "Uploads every 15 min",
     "S1: time between consecutive bulk updates in the server log over 3 virtual days",
     ["S1.6", "S1.7", "S1.9"], None),
    ("F8", "A sample is on ThingSpeak <= 16 min after its minute ends (when online)",
     "S1 and S12: server receive time minus end of the sample's minute, for every sample",
     ["S1.8", "S12.2"], "virtual time: add the real Wi-Fi + TLS time measured on the bench (B1)"),
    ("F8", "ThingSpeak limits respected (960 entries per bulk update, 15 s between writes) and retry behaviour (contract 7.3)",
     "S7: 3-day backlog, server log checked for HTTP 429, request sizes and spacing; S6: eight HTTP 500 answers injected, waits between attempts measured",
     ["S7.2", "S7.3", "S7.4", "S6.2", "S6.5"], None),
    ("F8", "HTTPS only",
     "Not testable against the mock (plain HTTP on 127.0.0.1). The firmware uses WiFiClientSecure and verifies the server against the esp32 core's CA bundle; setInsecure() is never used (contract 7.3)",
     [], "BENCH: procedure B1 below (and code review of the firmware transport)"),
    ("F8", "Write key never leaves the device",
     "S10: every network request made by the dashboard recorded in the browser; searched for the write keys and for non-GET methods",
     ["S10.9"], "plus repository check: no key in git (secrets.h is git-ignored)"),
    ("F9", "Dashboard and MATLAB agree (rule 6.5) on every complete day",
     "S10: five different days (clear, cloudy with duplicates, overcast with an outage, dark, calibration run); nightly job writes channel 2; dashboard 'MATLAB check' column, table and KPI cards compared with the Python reference. Also after every other scenario (S1, S2, S4, S5, S11)",
     ["S10.2a", "S10.2d", "S10.2t", "S10.5", "S1.11d", "S5.6d", "S3.6"], None),
    ("F9", "Late (back-filled) days are corrected by the next nightly run",
     "S3: channel-2 rows compared with the reference before and after the next run",
     ["S3.6", "S3.7", "S3.8"], None),
    ("F9", "Two phones show the same figures",
     "S10: the same page opened with two phone profiles (iPhone 13, Pixel 7) in headless Chrome; KPI values and daily table compared",
     ["S10.6"], "BENCH: procedure B4 below with two real phones"),
    ("F9", "CSV export equals the device log",
     "S10: 'Raw samples' export taken from the dashboard and compared value by value with channel 1 and with the device's sample log",
     ["S10.7", "S10.8"], "BENCH: repeat against the ESP32 serial log (B2)"),
    ("F9", "Sample-to-screen <= 17 min",
     "S12: worst server latency + dashboard refresh period + measured page update time; dashboard opened 60 s after an upload must show every uploaded sample",
     ["S12.3", "S12.4", "S12.5"], "virtual time: confirm with a stopwatch on the bench (B1)"),
]

BENCH = """### Hardware bench procedures still to do (when the ESP32 arrives)

These cannot be shown by simulation. Each has a pass criterion from contract section 11.

**B1 - Hotspot idle test (F8: 15-minute uploads, <= 16 min to ThingSpeak, HTTPS; F9: <= 17 min to screen).**
1. Flash the firmware with `sim off`, real sensors connected, phone hotspot on 2.4 GHz, `keys` set from the serial CLI.
2. Leave it for at least 3 h. Note the serial log lines `upload_ok` with their times.
3. On ThingSpeak (channel 1, private view) export the feed; for 12 consecutive uploads compute
   (server `created_at` of the newest entry in the batch + 60 s) against the serial `upload_ok` time, and count entries per hour.
4. With a stopwatch: note when a minute ends on the serial log and when that sample appears on the dashboard on a phone.
5. HTTPS: capture the hotspot traffic for one upload (phone packet capture or a laptop sharing its connection):
   only TLS to api.thingspeak.com:443, no plain-HTTP request, write key not readable.
Pass: 12 of 12 uploads 15 min +/- 30 s apart; 60 entries per hour (>= 99 %); every sample on ThingSpeak <= 16 min and on screen <= 17 min after its minute ends; no clear-text request.

**B2 - Real outage test (F7: no loss for an outage inside the buffer).**
1. With the device logging (serial log saved to a file), switch the phone hotspot off for 6 h (overnight), then on again.
   If time allows repeat with the hotspot off for a whole weekend (about 60 h).
2. After reconnection wait 30 min, then on the dashboard choose 'Raw samples' CSV for the days concerned.
3. Compare with the serial log: every `seq` logged during the outage is in the CSV exactly once after de-duplication, with the same minute.
4. Check the dashboard: completeness 100 % for complete days, fault 9 listed for the outage, no fault 6.
Pass: 0 missing sequence numbers; back-fill finished within 16 min of reconnection (6 h case); serial log shows no HTTP 429.

**B3 - DMM calibration check and clock check (measurement validity for F7-F9; F7: timestamps within 2 s).**
1. Run `cal` (two-point calibration of the power sensors, INA226 or INA219, auto-detected) for each of the three power channels against the bench DMM and a bench supply/load.
2. Then apply five loads across the range (for example 0.1, 0.5, 1, 3, 5 W) and write down DMM power and the 60 s mean reported on the dashboard.
3. Clock: with NTP reachable, type `time` on the serial CLI next to a phone showing time.is; then unplug Wi-Fi for 24 h (RTC free-running) and repeat.
4. Remove the RTC battery with the board off, power on without Wi-Fi: `status` must show fault 4 and no samples are logged; enable the hotspot: logging starts within 2 min.
Pass: dashboard within +/- 2 % of the DMM at every load (or the tolerance agreed with the power-system lead); clock error <= 2 s in both readings; fault 4 behaviour as S9.

**B4 - Two-phone check (F9).**
1. Open the dashboard link on two different phones (one iOS, one Android if possible), one on Wi-Fi and one on mobile data.
2. Press 'Refresh now' on both within the same minute; photograph both screens.
3. Compare the eight key figures, the newest row of the daily table and the 'MATLAB check' column; export 'Raw samples' on both and compare the files (same number of rows, same last line).
Pass: all figures identical; both CSV files identical.
"""


def write_vv(h, meta):
    by_id = {c["id"]: c for c in h.checks}
    lines = ["<!-- BEGIN E2E (generated by tests/e2e/run_e2e.py; edit the script, not this block) -->",
             "## Verification of F7, F8, F9 by end-to-end simulation", "",
             "Run of %s, contract v%s. All times are virtual (the device clock and the server clock are simulated), so several days run in minutes." % (meta["generated_utc"], meta["contract"]), "",
             "What was real and what was a stand-in in this run:", "",
             "| Part | Used |", "|---|---|",
             "| Device | %s |" % h.device_label,
             "| ThingSpeak | %s |" % meta["server"],
             "| Nightly job | %s |" % h.nightly_label,
             "| Dashboard | %s |" % h.browser_label,
             "| Reference numbers | %s |" % meta["reference"], "",
             "Totals: %d checks, %d PASS, %d FAIL, %d not run. Full table: `tests/e2e/results/e2e_summary.md`." %
             (len(h.checks), meta["counts"]["PASS"], meta["counts"]["FAIL"], meta["counts"]["SKIP"]), "",
             "Every sample in these tests is simulated (`s=1`). A PASS here shows that the logic of the data link meets the target; "
             "the bench procedures below must still be done on the hardware.", "",
             "| Req. | Acceptance target (contract 11) | Method | Measured | Result |", "|---|---|---|---|---|"]
    for req, target, method, ids, bench in VV_ROWS:
        got = [by_id[i] for i in ids if i in by_id]
        if got:
            measured = "<br>".join("%s: %s" % (c["id"], md_escape(c["measured"])) for c in got)
            if any(c["status"] == "FAIL" for c in got):
                verdict = "FAIL (%s)" % ", ".join(c["id"] for c in got if c["status"] == "FAIL")
            elif all(c["status"] == "SKIP" for c in got):
                verdict = "NOT RUN"
            elif bench and bench.startswith("BENCH"):
                verdict = "PASS for the logic (simulation); bench test still to do"
            else:
                verdict = "PASS (simulation)"
        else:
            measured = "not measured in simulation" if not ids else "scenario not run"
            verdict = "NOT TESTED" if not ids else "NOT RUN"
        if bench:
            measured += "<br>*%s*" % bench
            if not ids:
                verdict = "TO DO on bench"
        lines.append("| %s | %s | %s | %s | %s |" % (req, md_escape(target), md_escape(method), measured, verdict))
    lines.append("")
    if h.findings:
        lines += ["### Findings from this run", ""]
        for f in h.findings:
            lines.append("- **%s** (%s): %s" % (f["component"], f["scenario"], f["text"]))
        lines.append("")
    lines += [BENCH.rstrip(), "<!-- END E2E -->"]
    block = "\n".join(lines) + "\n"
    if os.path.exists(VV_FILE):
        with open(VV_FILE, "r", encoding="utf-8") as fh:
            old = fh.read()
        if "<!-- BEGIN E2E" in old and "<!-- END E2E -->" in old:
            new = old[:old.index("<!-- BEGIN E2E")] + block.rstrip("\n") + old[old.index("<!-- END E2E -->") + len("<!-- END E2E -->"):]
        else:
            new = old.rstrip("\n") + "\n\n" + block
    else:
        new = "# AST data link - verification and validation results (F7, F8, F9)\n\n" + block
    with open(VV_FILE, "w", encoding="utf-8") as fh:
        fh.write(new)


def main(argv=None):
    ap = argparse.ArgumentParser(description="AST data-link end-to-end tests")
    ap.add_argument("--scenarios", default="all", help="comma-separated, e.g. S1,S6 (default all)")
    ap.add_argument("--device", choices=("auto", "host", "sim"), default="auto")
    ap.add_argument("--nightly", choices=("auto", "octave", "python"), default="auto")
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--wait-host", type=int, default=0, help="seconds to wait for the firmware host build to appear")
    ap.add_argument("--keep", action="store_true", help="keep the temporary directory (device state and logs)")
    ap.add_argument("--no-vv", action="store_true", help="do not rewrite docs/VV_RESULTS.md")
    args = ap.parse_args(argv)
    names = [n for n, _ in SCENARIOS]
    selected = names if args.scenarios == "all" else [s.strip() for s in args.scenarios.split(",") if s.strip()]
    unknown = [s for s in selected if s not in names]
    if unknown:
        ap.error("unknown scenario(s): %s (known: %s)" % (", ".join(unknown), ", ".join(names)))
    h = Harness(args)
    started = time.time()
    try:
        h.start()
        for name, fn in SCENARIOS:
            if name not in selected:
                continue
            try:
                fn(h)
            except Exception as exc:                       # a broken scenario is a failed check, not a lost run
                import traceback
                traceback.print_exc()
                h.check("X", "scenario ran to the end", "no exception", "%s: %s" % (type(exc).__name__, str(exc)[:200]), False)
                h.end()
        meta = write_reports(h, started, selected)
        if not args.no_vv and args.scenarios == "all":
            write_vv(h, meta)
    finally:
        h.stop()
    c = meta["counts"]
    print("\n%d checks: %d PASS, %d FAIL, %d not run. %.0f s. Reports in %s" % (len(h.checks), c["PASS"], c["FAIL"], c["SKIP"], meta["wall_seconds"], RESULTS))
    if args.keep:
        print("temporary files kept in %s" % h.tmp)
    return 1 if c["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
