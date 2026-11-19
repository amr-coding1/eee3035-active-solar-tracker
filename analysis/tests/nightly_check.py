#!/usr/bin/env python3
"""
nightly_check.py - run analysis/matlab/ast_nightly.m in GNU Octave against a local
ThingSpeak stand-in and check every channel-2 row against the Python reference.

    python3 nightly_check.py            exit 0 = all checks passed

Scenarios
  A  shared mock (tools/mock_thingspeak.py), private channels, NDAYS = 9 days:
     normal day, day with a 2.5 h gap + missing fields, day with re-sent
     batches (duplicates), empty days, simulated (s=1) day, night-only day
     (E_fix = 0, no pointing samples, entries without seq, a value on a
     rounding boundary), a day where the device's seq restarts (flash
     formatted: same seq at other minutes must be kept), a day re-sent 5 more
     times so that its read window
     holds more than 8000 entries (window split), entries before the first
     day and after midnight of "today" (must be ignored).
  B  same server one night later after the gap was back-filled: the day is
     rewritten and readers must take the newest row per date.
  C  wrong read key: the script must stop with an error and write nothing.
  E  DRY_RUN = true with the write-key placeholder: rows are printed, nothing
     is written; DRY_RUN = 'auto' (the default) with the placeholder is a dry
     run too; with DRY_RUN forced off the placeholder key is refused.
  D  fixture server (tests/fixture_server.py), public channel (no read key):
     awkward JSON - keys missing from some feeds (jsondecode then returns a
     cell array instead of a struct array), JSON numbers, '+00:00' times,
     null status, junk values.

Checks per row: written text identical to ast_metrics.ch2_rows(); the agreement
rule (contract 6.5) against the unrounded reference summary; numeric
differences inside the rounding of the row format; plus request counts, the
single bulk write and the run time.
"""
import copy
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, "..", "py"))
import ast_metrics as am  # noqa: E402
import ast_sim  # noqa: E402

NIGHTLY = os.path.join(HERE, "..", "matlab", "ast_nightly.m")
SHIMS = os.path.join(HERE, "octave_shims")
MOCK = os.path.join(ROOT, "tools", "mock_thingspeak.py")
FIXTURE = os.path.join(HERE, "fixture_server.py")

CH1, CH1_WRITE, CH1_READ = 1001, "TESTWRITEKEY1001", "TESTREADKEY1001"
CH2, CH2_WRITE, CH2_READ = 1002, "TESTWRITEKEY1002", "TESTREADKEY1002"
NOW_A = ast_sim.day_start_s("2026-11-27") + 20 * 60          # 2026-11-27 00:20:00 UTC: days 18-26 Nov
NDAYS = am.NIGHTLY_DAYS                                      # 9 (contract v1.5): 8-day device buffer + 1
TIME_LIMIT_S = 20.0                                          # ThingSpeak MATLAB limit
REL_TOL_ENERGY = 1.0e-5       # 6 s.f. (5e-6) plus at most one tie-guard step
ABS_TOL_PCT = 1.0e-4          # 4 dp (5e-5) plus at most one tie-guard step

_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
failures = []
notes = []


def check(cond, text):
    if not cond:
        failures.append(text)
        print("  FAIL: " + text)
    return cond


def http(method, url, body=None, headers=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    if data is not None:
        req.add_header("Content-Type", "application/json")
    with _opener.open(req, timeout=30) as r:
        return json.loads(r.read().decode())


def start_server(cmd):
    p = subprocess.Popen([sys.executable] + cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    line = p.stdout.readline().strip()
    m = re.match(r"LISTENING (http://\S+)", line)
    if not m:
        p.kill()
        raise RuntimeError("server did not start: %r" % line)
    return p, m.group(1)


def stop_server(p):
    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()


def run_octave(base, now, ch1, read_key, ch2, write_key, extra=""):
    """Run ast_nightly.m with AST_* overrides. Returns (exit code, output, wall seconds, script seconds)."""
    code = ("addpath('%s'); AST_BASE_URL='%s'; AST_CH1_ID=%d; AST_CH1_READ_KEY='%s'; AST_CH2_ID=%d; "
            "AST_CH2_WRITE_KEY='%s'; AST_NOW_UTC=%d; %s run('%s');"
            % (SHIMS, base, ch1, read_key, ch2, write_key, now, extra, NIGHTLY))
    env = dict(os.environ, no_proxy="127.0.0.1,localhost", NO_PROXY="127.0.0.1,localhost")
    t0 = time.time()
    p = subprocess.run(["octave", "--no-gui", "-q", "--no-init-file", "--eval", code], capture_output=True,
                       text=True, timeout=180, env=env)
    wall = time.time() - t0
    out = p.stdout + "\n".join(l for l in p.stderr.splitlines()
                               if "X11 DISPLAY" not in l and "disabling GUI" not in l and "shadows a core" not in l)
    m = re.search(r"Elapsed ([0-9.]+) s", out)
    return p.returncode, out, wall, float(m.group(1)) if m else None


# ---------------------------------------------------------------- datasets

def build_dataset_a():
    """Returns (initial arrival list, back-fill arrival list) of channel-1 updates."""
    seq = 1
    first, later = [], []

    ups, seq = ast_sim.simulate_day("2026-11-17", seq, "clear", 0, "A", seed=1, minutes=range(1340, 1440))
    first += ups                                             # before the 9 days: must be ignored

    # 2026-11-18: nothing (empty day at the old end of the window)

    # 2026-11-19: the device's flash was formatted at 11:00 and seq restarted at 1.
    # Same seq at another minute is a different sample: all 120 are kept (review finding 3).
    t19 = ast_sim.day_start_s("2026-11-19")
    for m in range(600, 720):
        first.append({"created_at": ast_sim.created_at(t19 + m * 60), "field1": "500.0", "field2": "450.0",
                      "field3": "50.0", "field4": "10.0", "field5": "11.0", "field7": "5.020",
                      "field8": "%d" % ((m - 600) % 60 + 1), "status": "TRK;r=1;p=A;e=0"})

    day20, seq = ast_sim.simulate_day("2026-11-20", seq, "clear", 1, "A", seed=1, sim=False)
    first += day20                                           # normal day of REAL samples (no s=1)

    day21, seq = ast_sim.simulate_day("2026-11-21", seq, "cloudy", 2, "A", seed=1, sim=False)
    for m in (500, 501, 502):
        del day21[m]["field1"]                               # tracked-panel power missing
    for m in range(800, 830):
        day21[m].pop("field5", None)                         # sun angle unknown
    day21[560]["field2"] = "-4.0"                            # negative power is summed as received
    day21[561]["status"] += ";z=9"                           # unknown key is ignored
    day21[562]["status"] = "TRK;s=1;r=2;p=A;s=0"             # last s wins: not simulated
    first += day21[:600] + day21[750:]                       # device offline 10:00-12:29 ...
    later += day21[600:750]                                  # ... back-filled the next day

    day22, seq = ast_sim.simulate_day("2026-11-22", seq, "clear", 3, "A", seed=1, sim=False)
    first += day22
    dups22 = copy.deepcopy(day22[300:600])                   # batch re-sent after a lost response
    altered = copy.deepcopy(day22[700])
    altered["field1"] = "9999.0"                             # later copy with a different value: must lose
    dups22.append(altered)

    # 2026-11-23: nothing at all (empty day)

    day24, seq = ast_sim.simulate_day("2026-11-24", seq, "overcast", 4, "B", seed=1, sim=True)
    first += day24                                           # every sample simulated (ns = 1440)

    day25 = []                                               # night only: E_fix = 0, no pointing samples
    t25 = ast_sim.day_start_s("2026-11-25")
    for m in range(200):
        day25.append({"created_at": ast_sim.created_at(t25 + m * 60), "field1": "0.0", "field2": "0.0",
                      "field3": "0.0", "field4": "0.0", "field7": "3.801", "field8": "%d" % seq,
                      "status": "IDL;r=0;p=B;e=0"})
        seq += 1
    day25[17]["field3"] = "74.09999"                         # E_use = 0.00123499983 Wh: 3 s.f. boundary
    for m in (300, 301, 301):                                # no seq: all kept, even at the same minute
        day25.append({"created_at": ast_sim.created_at(t25 + m * 60), "field1": "0.0", "field2": "0.0",
                      "field3": "0.0", "field4": "0.0", "field7": "3.800", "status": "IDL;r=0;p=B;e=0"})
    first += day25

    day26, seq = ast_sim.simulate_day("2026-11-26", seq, "clear", 5, "B", seed=1, sim=True)
    first += day26
    dups26 = copy.deepcopy(day26) * 5                        # 7200 duplicates: its one-day read window holds > 8000 entries

    day27, seq = ast_sim.simulate_day("2026-11-27", seq, "cloudy", 5, "B", seed=1, sim=True)
    first += day27[:16]                                      # "today" 00:00-00:15: run 5 spans midnight
    later += day27[16:]

    return first + dups22 + dups26, later


def seed(base, channel, key, updates, sim_time):
    """Bulk-write in batches of 960 with 16 virtual seconds between requests (15 s rule)."""
    for i in range(0, len(updates), 960):
        r = http("POST", "%s/channels/%d/bulk_update.json" % (base, channel),
                 {"write_api_key": key, "updates": updates[i:i + 960]}, {"X-Sim-Time": str(sim_time)})
        assert r.get("success") is True, r
        sim_time += 16
    return sim_time


# ------------------------------------------------------------- comparison

stats = {"rows": 0, "max_rel_energy": 0.0, "max_abs_pct": 0.0, "text_mismatch": 0, "agree": 0}


def compare(label, ch1_feeds, ch2_feeds, now, ndays=NDAYS):
    """Check the rows written at `now` against the reference built from the same channel-1 feeds."""
    entries = am.parse_feeds(ch1_feeds)
    expect = am.ch2_rows(entries, now, ndays)
    days = am.daily_summaries(entries, now * 1000, am.nightly_days(now, ndays))
    written = [f for f in ch2_feeds if now * 1000 <= am.parse_time(f["created_at"]) < (now + ndays) * 1000]
    check(len(written) == ndays, "%s: %d rows written at this run, expected %d" % (label, len(written), ndays))
    latest = am.latest_results_by_date([am.parse_result_feed(f) for f in ch2_feeds])
    print("  %-10s %-5s %-5s %-4s %-5s %-11s %-11s %-11s %-9s %-9s %-7s %-9s %s" % (
        "date", "n", "N", "dup", "sim", "E_trk", "E_fix", "E_use", "gross", "net", "MAE", "complete", "check"))
    for exp, day, got in zip(expect, days, written):
        stats["rows"] += 1
        same = True
        for k in ["field%d" % i for i in range(1, 9)] + ["status"]:
            if exp.get(k) != got.get(k):
                same = False
                check(False, "%s %s: %s written %r, reference %r" % (label, day["day"], k, got.get(k), exp.get(k)))
        check(am.parse_time(exp["created_at"]) == am.parse_time(got["created_at"]),
              "%s %s: created_at %s, reference %s" % (label, day["day"], got["created_at"], exp["created_at"]))
        stats["text_mismatch"] += 0 if same else 1
        row = latest.get(day["day"])
        check(row is not None and row["t"] == am.parse_time(got["created_at"]),
              "%s %s: newest channel-2 row for the date is not the one just written" % (label, day["day"]))
        status, reason = am.agreement(day, row)
        check(status == "agree", "%s %s: agreement rule: %s %s" % (label, day["day"], status, reason))
        stats["agree"] += status == "agree"
        if row:
            check(row["version"] == "1.5" and row["n_sim"] == day["n_sim"] and row["duplicates"] == day["duplicates"],
                  "%s %s: status %r" % (label, day["day"], got.get("status")))
            check(row["n"] == day["n_unique"], "%s %s: N %s vs %s" % (label, day["day"], row["n"], day["n_unique"]))
            for k in ("e_trk", "e_fix", "e_use"):
                ref, val = day[k], row[k]
                rel = 0.0 if ref == 0 and val == 0 else abs(val - ref) / abs(ref) if ref else float("inf")
                stats["max_rel_energy"] = max(stats["max_rel_energy"], rel)
                check(rel <= REL_TOL_ENERGY, "%s %s: %s %r vs %r (rel %.2e)" % (label, day["day"], k, val, ref, rel))
            for k in ("g_gross", "g_net", "mae", "complete"):
                ref, val = day[k], row[k]
                if ref is None or val is None:
                    check(ref is None and val is None, "%s %s: %s %r vs %r" % (label, day["day"], k, val, ref))
                    continue
                stats["max_abs_pct"] = max(stats["max_abs_pct"], abs(val - ref))
                check(abs(val - ref) <= ABS_TOL_PCT, "%s %s: %s %r vs %r" % (label, day["day"], k, val, ref))
        print("  %-10s %-5d %-5d %-4d %-5d %-11s %-11s %-11s %-9s %-9s %-7s %-9s %s" % (
            day["day"], day["n"], day["n_unique"], day["duplicates"], day["n_sim"], got.get("field1"),
            got.get("field2"), got.get("field3"), got.get("field4", "-"), got.get("field5", "-"),
            got.get("field6", "-"), got.get("field7"), "ok" if same and status == "agree" else "DIFFERENT"))
    return days


def timing(label, rc, out, wall, inner):
    check(rc == 0, "%s: Octave exit code %d\n%s" % (label, rc, out))
    check(inner is not None and inner < TIME_LIMIT_S, "%s: script time %s s (limit %g s)" % (label, inner, TIME_LIMIT_S))
    print("  run time: %.2f s inside the script, %.2f s wall including Octave start-up" % (inner or -1, wall))
    notes.append("%s: script %.2f s, wall %.2f s" % (label, inner or -1, wall))


# ---------------------------------------------------------------- scenarios

def scenarios_abc():
    if not os.path.exists(MOCK):
        notes.append("shared mock not found: scenarios A-C skipped (fixture server scenario D still runs)")
        print("shared mock not found, skipping scenarios A-C")
        return
    srv, base = start_server([MOCK, "--port", "0", "--channel", "%d:%s:%s" % (CH1, CH1_WRITE, CH1_READ),
                              "--channel", "%d:%s:%s" % (CH2, CH2_WRITE, CH2_READ)])
    try:
        first, later = build_dataset_a()
        sim_t = seed(base, CH1, CH1_WRITE, first, NOW_A - 6 * 3600)
        dump = lambda ch: http("GET", "%s/__admin/dump/%d" % (base, ch))["entries"]
        read2 = lambda: http("GET", "%s/channels/%d/feeds.json?api_key=%s&results=8000&status=true&timezone=Etc/UTC"
                             % (base, CH2, CH2_READ))["feeds"]

        print("\nScenario A: shared mock, %d channel-1 entries, now = %s" % (len(first), ast_sim.created_at(NOW_A)))
        http("POST", base + "/__admin/clock", {"now": NOW_A, "freeze": True})
        log0 = http("GET", base + "/__admin/log")["total"]
        rc, out, wall, inner = run_octave(base, NOW_A, CH1, CH1_READ, CH2, CH2_WRITE)
        print("".join("  | %s\n" % l for l in out.strip().splitlines()))
        timing("A", rc, out, wall, inner)
        days = compare("A", dump(CH1), read2(), NOW_A)
        by = {d["day"]: d for d in days}
        check(by["2026-11-20"]["n_unique"] == 1440 and by["2026-11-20"]["n_sim"] == 0, "A: normal day")
        check(by["2026-11-21"]["n_unique"] == 1290 and by["2026-11-21"]["missing_min"] == 150, "A: gap day")
        check(by["2026-11-22"]["duplicates"] == 301 and by["2026-11-22"]["n_unique"] == 1440, "A: duplicates day")
        check(by["2026-11-23"]["n"] == 0, "A: empty day")
        check(by["2026-11-24"]["n_sim"] == 1440, "A: simulated day")
        check(by["2026-11-25"]["n_unique"] == 203 and by["2026-11-25"]["g_net"] is None
              and by["2026-11-25"]["mae"] is None, "A: night-only day")
        check(by["2026-11-18"]["n"] == 0, "A: empty day at the old end; 17 Nov ignored")
        check(by["2026-11-19"]["n_unique"] == 120 and by["2026-11-19"]["duplicates"] == 0,
              "A: seq restart: all 120 samples kept (got N=%s, dup=%s)" % (by["2026-11-19"]["n_unique"], by["2026-11-19"]["duplicates"]))
        check(by["2026-11-26"]["duplicates"] == 7200 and by["2026-11-26"]["n_unique"] == 1440, "A: re-sent day")
        log = [r for r in http("GET", base + "/__admin/log?since=%d" % log0)["log"]]
        reads = [r for r in log if r.get("channel") == CH1 and r.get("method") == "GET"]
        writes = [r for r in log if r.get("channel") == CH2 and r.get("method") == "POST"]
        full = [r for r in reads if r.get("returned") == 8000]
        check(len(writes) == 1 and writes[0].get("updates") == NDAYS and writes[0].get("status") == 202,
              "A: exactly one bulk update of %d rows (got %r)" % (NDAYS, [(w.get("updates"), w.get("status")) for w in writes]))
        check(len(full) >= 1, "A: a read window came back full (8000) so the split path ran")
        check(sum(r.get("returned", 0) for r in reads if r.get("returned") != 8000)
              == sum(d["n"] for d in days), "A: every entry of the %d days was read exactly once after splitting" % NDAYS)
        m = re.search(r"(\d+) request\(s\), (\d+) window split\(s\)", out)
        check(bool(m) and int(m.group(2)) >= 1, "A: log line reports the split")
        notes.append("A: %d channel-1 reads (%d came back full and were split), 1 channel-2 write"
                     % (len(reads), len(full)))
        # tie guard really exercised: E_use of the night-only day
        e_use_txt = [f for f in read2() if "date=2026-11-25" in f["status"]][-1]["field3"]
        check(e_use_txt == "0.00123499", "A: tie guard wrote %r for E_use = 0.00123499983 Wh" % e_use_txt)

        now_b = NOW_A + 86400
        print("\nScenario B: gap back-filled (%d late entries), next night, now = %s" % (len(later), ast_sim.created_at(now_b)))
        seed(base, CH1, CH1_WRITE, later, sim_t + 3600)
        http("POST", base + "/__admin/clock", {"now": now_b, "freeze": True})
        rc, out, wall, inner = run_octave(base, now_b, CH1, CH1_READ, CH2, CH2_WRITE)
        timing("B", rc, out, wall, inner)
        ch2 = read2()
        days = compare("B", dump(CH1), ch2, now_b)
        by = {d["day"]: d for d in days}
        check(len(ch2) == 2 * NDAYS, "B: channel 2 holds %d rows after two nights (got %d)" % (2 * NDAYS, len(ch2)))
        check(by["2026-11-21"]["n_unique"] == 1440 and by["2026-11-21"]["gaps"] == 0, "B: back-filled day is whole")
        check(by["2026-11-27"]["n_unique"] == 1440, "B: the day that was 'today' is now summarised")
        rows21 = [f for f in ch2 if "date=2026-11-21" in f["status"]]
        check([r["field8"] for r in rows21] == ["1290", "1440"], "B: old and corrected row for the back-filled day")

        print("\nScenario C: wrong read key")
        before = len(read2())
        rc, out, wall, inner = run_octave(base, now_b + 60, CH1, "WRONGKEY", CH2, CH2_WRITE)
        check(rc != 0, "C: script must fail with a wrong read key")
        check("Reading channel" in out and "CH1_READ_KEY" in out, "C: error message names the read key:\n" + out)
        check(len(read2()) == before, "C: nothing written to channel 2")
        print("  script stopped as expected: %s" % next(l for l in out.splitlines() if "Reading channel" in l).strip()[:150])

        print("\nScenario E: DRY_RUN with the write-key placeholder")
        rc, out, wall, inner = run_octave(base, now_b, CH1, CH1_READ, CH2, "PASTE_CH2_WRITE_KEY", "AST_DRY_RUN=true;")
        timing("E", rc, out, wall, inner)
        check("DRY RUN: nothing written" in out and re.search(r"JSON body \d+ characters", out) is not None,
              "E: dry-run line with the body length:\n" + out)
        check(len(read2()) == before, "E: dry run wrote nothing to channel 2")
        expect = am.ch2_rows(am.parse_feeds(dump(CH1)), now_b, NDAYS)
        printed = [l.split() for l in out.splitlines() if re.match(r"^\d{4}-\d{2}-\d{2}  ", l)]
        want = [[r["status"].split(";")[1][5:], r["field1"], r["field2"], r["field3"], r.get("field4", "-"),
                 r.get("field5", "-"), r.get("field6", "-"), r["field7"], r["field8"]] for r in expect]
        got = [[c[0]] + c[5:12] + [c[2]] for c in printed]
        check(got == want, "E: printed rows equal the reference rows")
        rc, out, wall, inner = run_octave(base, now_b, CH1, CH1_READ, CH2, "PASTE_CH2_WRITE_KEY")
        check(rc == 0 and "No channel-2 write key yet: dry run" in out and "DRY RUN: nothing written" in out,
              "E: default DRY_RUN = 'auto' with the placeholder key is a dry run:\n" + out)
        rc, out, wall, inner = run_octave(base, now_b, CH1, CH1_READ, CH2, "PASTE_CH2_WRITE_KEY", "AST_DRY_RUN=false;")
        check(rc != 0 and "CH2_WRITE_KEY" in out, "E: with DRY_RUN forced off the placeholder key is refused")
        check(len(read2()) == before, "E: placeholder key wrote nothing")
        print("  dry run printed %d rows identical to the reference; placeholder key refused when DRY_RUN is off" % len(got))
    finally:
        stop_server(srv)


def build_fixture_feeds():
    """Channel-1 feeds as ThingSpeak-like JSON objects, deliberately untidy."""
    feeds = []
    eid = 0
    seq = 1
    for day, scen in (("2026-11-25", "clear"), ("2026-11-26", "cloudy")):
        ups, seq = ast_sim.simulate_day(day, seq, scen, 7, "A", seed=3, minutes=range(400, 1000))
        for i, u in enumerate(ups):
            eid += 1
            f = {"created_at": u["created_at"][:10] + "T" + u["created_at"][11:19] + "Z", "entry_id": eid}
            for k in range(1, 9):
                f["field%d" % k] = u.get("field%d" % k)
            f["status"] = u["status"]
            if i % 7 == 0:
                del f["field6"]                                # key absent (field not enabled / older entries)
            if i % 11 == 0:
                f["field1"] = float(f["field1"])               # JSON number instead of text
                f["field8"] = int(f["field8"])
            if i % 13 == 0:
                f["created_at"] = f["created_at"][:-1] + "+00:00"
            if i % 17 == 0:
                f["status"] = None
            if i % 19 == 0:
                del f["status"]
            if i % 23 == 0:
                f["field2"] = " %s " % f["field2"]             # white space round a number
            if i % 29 == 0:
                f["field3"] = "abc"                            # junk -> treated as missing
            if i % 31 == 0:
                f["field5"] = "NaN"
            if i % 37 == 0:
                del f["entry_id"]
            if i % 41 == 0:
                f["field4"] = "1e1"                            # exponent form = 10
            feeds.append(f)
    feeds.append({"created_at": "not-a-time", "entry_id": eid + 1, "field1": "5000.0", "status": "TRK"})
    feeds.append({"created_at": "2026-11-26T18:00:00+01:00", "entry_id": eid + 2, "field1": "33.3", "field2": "30.0",
                  "field3": "1.0", "field8": "999999", "status": "TRK;r=7;p=A;e=0"})   # = 17:00:00Z
    return feeds


def scenario_d():
    feeds = build_fixture_feeds()
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
        json.dump(feeds, fh)
    srv, base = start_server([FIXTURE, "--feeds", fh.name, "--write-key", "FIXTUREWRITEKEY"])
    try:
        now = ast_sim.day_start_s("2026-11-27") + 20 * 60 + 5
        print("\nScenario D: fixture server, public channel, untidy JSON (%d feeds)" % len(feeds))
        rc, out, wall, inner = run_octave(base, now, 1, "", 2, "FIXTUREWRITEKEY")
        timing("D", rc, out, wall, inner)
        if rc != 0:
            print(out)
        ch2 = http("GET", base + "/channels/2/feeds.json")
        days = compare("D", feeds, ch2["feeds"], now)
        check(sum(d["n"] for d in days) == 1201, "D: 1201 entries with a readable time (got %d)" % sum(d["n"] for d in days))
        m = re.search(r"(\d+) without a readable time", out)      # the fixture returns it in every window
        check(bool(m) and int(m.group(1)) >= 1, "D: unreadable created_at is counted in the log")
        reqs = http("GET", base + "/__requests")
        reads = [r for r in reqs if r["method"] == "GET" and r["path"] == "/channels/1/feeds.json"]
        check(reads and all("api_key" not in r["query"] for r in reads), "D: empty read key -> no api_key parameter")
        check(all(r["query"].get("timezone") == "Etc/UTC" and r["query"].get("status") == "true"
                  and r["query"].get("results") == "8000" for r in reads), "D: read parameters of contract section 8")
        spans = [(am.parse_time(r["query"]["end"]) - am.parse_time(r["query"]["start"])) / 86400000 for r in reads]
        check(max(spans) <= 4.0, "D: read windows of at most 4 days (got %.2f)" % max(spans))
        posts = [r for r in reqs if r["method"] == "POST"]
        check(len(posts) == 1 and posts[0]["content_type"] == "application/json", "D: one JSON bulk update")
    finally:
        stop_server(srv)
        os.unlink(fh.name)


def main():
    v = subprocess.run(["octave", "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    print("ast_nightly.m against the Python reference, using %s" % v)
    scenarios_abc()
    scenario_d()
    print("\nSummary: %d rows checked, %d identical text, %d agree (rule 6.5); max relative energy difference "
          "%.2e (limit %.0e), max percentage/angle difference %.2e (limit %.0e)"
          % (stats["rows"], stats["rows"] - stats["text_mismatch"], stats["agree"], stats["max_rel_energy"],
             REL_TOL_ENERGY, stats["max_abs_pct"], ABS_TOL_PCT))
    for n in notes:
        print("  " + n)
    if failures:
        print("\nNIGHTLY CHECK FAILED: %d problem(s)" % len(failures))
        return 1
    print("\nNIGHTLY CHECK PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
