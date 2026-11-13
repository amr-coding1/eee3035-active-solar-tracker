#!/usr/bin/env python3
"""
vis_check.py - run the five ThingSpeak visualisation scripts and the report-figure
script in GNU Octave (off screen) and check that each one produces a picture.

    python3 vis_check.py            exit 0 = every script ran and wrote its PNG

The vis_*.m scripts read a local mock ThingSpeak that is seeded with simulated
data: channel 1 gets one day with a gap and a re-sent batch, channel 2 gets 14
daily rows written by the Python reference (gains, losses, an empty day, a
partial day). Each script is also run against EMPTY channels to check the
"No data" note. Pictures go to analysis/tests/out/ for a visual check; this
script only checks that they exist and are not blank.
Needs: octave, xvfb-run (no display on the test machine), the shared mock.
"""
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "py"))
import ast_metrics as am  # noqa: E402
import ast_sim  # noqa: E402
import nightly_check as nc  # noqa: E402  (server and HTTP helpers)

MATLAB_DIR = os.path.normpath(os.path.join(HERE, "..", "matlab"))
OUT = os.path.join(HERE, "out")
VIS = ["vis_power_today", "vis_energy_today", "vis_angles_today", "vis_status_today", "vis_daily_net_gain"]
TIME_LIMIT_S = 20.0

OCTAVE_DRIVER = r"""
graphics_toolkit('qt');
set(0, 'defaultfigurevisible', 'off');
zz_names = {%(names)s};        %% zz_ names: the scripts share this workspace
for zz_i = 1:numel(zz_names)
  for zz_pass = 1:2
    clear -x zz_*
    AST_BASE_URL = '%(base)s';
    if zz_pass == 1
      AST_CH1_ID = %(ch1)d; AST_CH1_READ_KEY = '%(k1)s'; AST_CH2_ID = %(ch2)d; AST_CH2_READ_KEY = '%(k2)s'; zz_tag = '';
    else
      AST_CH1_ID = %(e1)d; AST_CH1_READ_KEY = ''; AST_CH2_ID = %(e2)d; AST_CH2_READ_KEY = ''; zz_tag = '_nodata';
    end
    close all
    zz_t0 = tic;
    try
      run(fullfile('%(mdir)s', [zz_names{zz_i} '.m']));
      zz_secs = toc(zz_t0);
      print(gcf, '-dpng', '-r100', fullfile('%(out)s', [zz_names{zz_i} zz_tag '.png']));
      printf('RESULT %%s%%s ok %%.2f figures=%%d\n', zz_names{zz_i}, zz_tag, zz_secs, numel(findall(0, 'type', 'figure')));
    catch zz_err
      printf('RESULT %%s%%s ERROR %%s\n', zz_names{zz_i}, zz_tag, strrep(zz_err.message, char(10), ' '));
    end
  end
end
"""


def seed_channels(base):
    day = "2026-11-26"
    ups, seq = ast_sim.simulate_day(day, 5001, "cloudy", 5, "B", seed=4,
                                    minutes=[m for m in range(1440) if not 690 <= m < 735 and m < 1300])
    for u in ups[200:230]:
        u["status"] = u["status"].replace("IDL", "STW")
    for u in ups[900:906]:
        u["status"] = u["status"].replace("TRK", "FLT").replace("e=0", "e=8")
    t = nc.seed(base, nc.CH1, nc.CH1_WRITE, ups + ups[400:520], 1_800_000_000)      # one batch sent twice
    # channel 2: 14 nights summarised by the reference from simulated days
    plan = ["clear", "cloudy", "overcast", "clear", None, "cloudy", "clear", "overcast", "cloudy", "clear",
            "clear", "partial", "overcast", "cloudy"]
    feeds, seq, d = [], 1, "2026-11-13"
    for i, scen in enumerate(plan):
        if scen is not None:
            mins = range(600, 1100) if scen == "partial" else range(1440)
            u, seq = ast_sim.simulate_day(d, seq, "clear" if scen == "partial" else scen, 1 + i, "A", seed=2,
                                          sim=(i % 3 != 0), minutes=mins)
            feeds += u
        d = am.add_days(d, 1)
    for i, f in enumerate(feeds, 1):
        f["entry_id"] = i
    now = ast_sim.day_start_s("2026-11-27") + 20 * 60
    rows = am.ch2_rows(am.parse_feeds(feeds), now, 14)
    old = [dict(r, created_at=ast_sim.created_at(now - 86400 + k), field5="55.5555") for k, r in enumerate(rows[:5])]
    nc.seed(base, nc.CH2, nc.CH2_WRITE, old, t + 100)        # stale rows that must lose to the newer ones
    nc.seed(base, nc.CH2, nc.CH2_WRITE, rows, t + 200)
    return rows


def main():
    for tool in ("octave", "xvfb-run"):
        if shutil.which(tool) is None:
            print("SKIPPED: %s not found; the visualisations were not executed" % tool)
            return 0
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    srv, base = nc.start_server([nc.MOCK, "--port", "0", "--channel", "%d:%s:%s" % (nc.CH1, nc.CH1_WRITE, nc.CH1_READ),
                                 "--channel", "%d:%s:%s" % (nc.CH2, nc.CH2_WRITE, nc.CH2_READ),
                                 "--channel", "2001:W2001:", "--channel", "2002:W2002:"])    # empty, public
    failures = 0
    try:
        seed_channels(base)
        code = OCTAVE_DRIVER % {"names": ", ".join("'%s'" % v for v in VIS), "base": base, "ch1": nc.CH1,
                                "k1": nc.CH1_READ, "ch2": nc.CH2, "k2": nc.CH2_READ, "e1": 2001, "e2": 2002,
                                "mdir": MATLAB_DIR, "out": OUT}
        env = dict(os.environ, no_proxy="127.0.0.1,localhost", NO_PROXY="127.0.0.1,localhost")
        t0 = time.time()
        p = subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24", "octave", "--no-gui", "-q",
                            "--no-init-file", "--eval", code], capture_output=True, text=True, timeout=300, env=env)
        print("Octave session: %.1f s, exit code %d" % (time.time() - t0, p.returncode))
        results = dict((m.group(1), m.group(2)) for m in re.finditer(r"^RESULT (\S+) (.*)$", p.stdout, re.M))
        for v in VIS:
            for tag in ("", "_nodata"):
                r = results.get(v + tag, "ERROR did not run")
                png = os.path.join(OUT, v + tag + ".png")
                size = os.path.getsize(png) if os.path.exists(png) else 0
                secs = float(r.split()[1]) if r.startswith("ok") else None
                good = r.startswith("ok") and size > 3000 and secs < TIME_LIMIT_S and r.endswith("figures=1")
                failures += 0 if good else 1
                print("  %-28s %-4s %s  png %d bytes" % (v + tag, "ok" if good else "FAIL", r, size))
        if p.returncode != 0 or failures:
            print(p.stdout[-2000:])
            print(p.stderr[-3000:])
    finally:
        nc.stop_server(srv)
    print("VIS CHECK %s (%d of %d runs good); pictures in %s" % ("FAILED" if failures else "PASSED",
                                                                 2 * len(VIS) - failures, 2 * len(VIS), OUT))
    report_failures = report_check()
    return 1 if failures or report_failures else 0


REPORT_FIGS = ["fig1_power_2026-11-21", "fig2_energy_2026-11-21", "fig3_daily_net_gain", "fig4_pointing_histogram",
               "fig5_energy_budget", "fig6_calibration_mismatch", "fig7_completeness"]


def report_check():
    """ast_report_figures.m on a fresh simulated CSV; its summary table against the Python reference."""
    import csv
    import math
    out = os.path.join(OUT, "report")
    os.makedirs(out)
    raw = os.path.join(out, "sample_raw_SIMULATED.csv")
    subprocess.run([sys.executable, os.path.join(HERE, "..", "py", "make_sample_csv.py"), raw], check=True,
                   stdout=subprocess.DEVNULL)
    code = "graphics_toolkit('qt'); addpath('%s'); ast_report_figures('%s', '%s');" % (MATLAB_DIR, raw, out)
    t0 = time.time()
    p = subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24", "octave", "--no-gui", "-q",
                        "--no-init-file", "--eval", code], capture_output=True, text=True, timeout=300)
    bad = []
    if p.returncode != 0:
        bad.append("Octave exit code %d: %s" % (p.returncode, p.stderr[-1500:]))
    for f in REPORT_FIGS:
        for ext in (".png", ".pdf"):
            path = os.path.join(out, f + ext)
            if not os.path.exists(path) or os.path.getsize(path) < 5000:
                bad.append("missing or empty " + f + ext)
    # reference: per day, duplicates removed inside the day, calibration samples left out of the gains
    entries = am.read_raw_csv(raw)
    now_ms = max(e["t"] for e in entries) + am.DAY_MS
    all_days = {d["day"]: d for d in am.daily_summaries(entries, now_ms)}
    tests = [e for e in entries if am.run_kind(e["run"]) != "calibration"]
    test_days = {d["day"]: d for d in am.daily_summaries(tests, now_ms)}
    worst = 0.0
    rows = []
    table = os.path.join(out, "report_daily_summary.csv")
    if os.path.exists(table):
        with open(table, newline="") as fh:
            rows = list(csv.DictReader(fh))
    if len(rows) != len(all_days):
        bad.append("summary has %d days, reference %d" % (len(rows), len(all_days)))
    for r in rows:
        a = all_days[r["date"]]
        t = test_days.get(r["date"]) or am.summarise([], 1440)
        if (int(r["n_received"]), int(r["n_unique"]), int(r["n_sim"]), int(r["n_test"])) != \
                (a["n"], a["n_unique"], a["n_sim"], t["n_unique"]):
            bad.append("%s: counts %r" % (r["date"], r))
        pairs = [(r["complete_pct"], a["complete"], 1e-4), (r["e_trk_wh"], t["e_trk"], 1e-6), (r["e_fix_wh"], t["e_fix"], 1e-6),
                 (r["e_use_wh"], t["e_use"], 1e-6), (r["gain_gross_pct"], t["g_gross"], 1e-5),
                 (r["gain_net_pct"], t["g_net"], 1e-5), (r["mae_point_deg"], t["mae"], 1e-5)]
        if t["g_net"] is not None:      # uncertainty: independent +/- 2 % on each power channel (see the .m header)
            rr, u = t["e_trk"] / t["e_fix"], t["e_use"] / t["e_fix"]
            pairs.append((r["gain_net_unc_pct"], 100 * math.sqrt((rr * 0.02) ** 2 + (u * 0.02) ** 2 + ((rr - u) * 0.02) ** 2), 1e-5))
        for txt, ref, tol in pairs:
            if ref is None or txt == "":
                if not (ref is None and txt == ""):
                    bad.append("%s: %r vs %r" % (r["date"], txt, ref))
                continue
            worst = max(worst, abs(float(txt) - ref))
            if abs(float(txt) - ref) > tol:
                bad.append("%s: %r vs %r" % (r["date"], txt, ref))
    for b in bad:
        print("  FAIL: " + b)
    print("REPORT CHECK %s: %d figures x (PNG, PDF) in %.1f s, %d daily rows against the reference, "
          "largest difference %.1e; output in %s" % ("FAILED" if bad else "PASSED", len(REPORT_FIGS),
                                                     time.time() - t0, len(rows), worst, out))
    return len(bad)


if __name__ == "__main__":
    sys.exit(main())
