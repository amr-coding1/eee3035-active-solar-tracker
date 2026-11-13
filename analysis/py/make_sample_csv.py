#!/usr/bin/env python3
"""
make_sample_csv.py - write a SIMULATED raw-sample CSV in the dashboard's export format.

    python3 make_sample_csv.py [out.csv]      (default: ../figures/sample_raw_SIMULATED.csv)

The file has the header of docs/PLAN.md ("Dashboard CSV export header") and
the same number formatting as dashboard/exports.js. Every row is marked sim=1
(status ...;s=1). It feeds analysis/matlab/ast_report_figures.m so that the
example figures can be made before any real data exists.

Eight days: tracking tests with panel A then panel B on the tracker (clear,
cloudy and overcast days), one day with a 2.5 h gap and a re-sent batch
(duplicates), and two calibration runs (900 with A on the mount, 901 with B).
"""
import os
import sys
from zoneinfo import ZoneInfo
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ast_metrics as am  # noqa: E402
import ast_sim  # noqa: E402

PLAN = [  # day, scenario, run, panel on the tracker mount, calibration, minutes logged
    ("2026-11-20", "clear", 900, "A", True, range(1440)),
    ("2026-11-21", "clear", 1, "A", False, range(1440)),
    ("2026-11-22", "cloudy", 2, "A", False, range(1440)),
    ("2026-11-23", "overcast", 3, "A", False, [m for m in range(1440) if not 600 <= m < 750]),
    ("2026-11-24", "clear", 901, "B", True, range(1440)),
    ("2026-11-25", "clear", 4, "B", False, range(1440)),
    ("2026-11-26", "cloudy", 5, "B", False, range(1430)),
    ("2026-11-27", "overcast", 6, "B", False, range(1440)),
]
LONDON = ZoneInfo("Europe/London")


def csv_num(x, dp=6):
    """dashboard/format.js csvNum: plain decimal, up to dp places, empty for null."""
    if x is None:
        return ""
    s = ("%.*f" % (dp, x)).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def build_feeds():
    """ThingSpeak-style feed objects with entry ids, including one re-sent batch."""
    feeds = []
    seq = 1
    for day, scenario, run, panel, cal, minutes in PLAN:
        # the panel on the tracker mount reads 1.5 % high for A and 1.5 % low for B (same two panels, swapped)
        ups, seq = ast_sim.simulate_day(day, seq, scenario, run, panel, seed=8, sim=True, minutes=minutes,
                                        calibration=cal, mismatch=0.015 if panel == "A" else -0.015)
        feeds.extend(ups)
        if day == "2026-11-22":
            feeds.extend(ups[300:420])          # a batch sent twice after a lost response
    for i, f in enumerate(feeds, 1):
        f["entry_id"] = i
    return feeds


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    out = argv[1] if len(argv) > 1 else os.path.join(here, "..", "figures", "sample_raw_SIMULATED.csv")
    entries = am.parse_feeds(build_feeds())
    assert all(e["sim"] for e in entries)
    lines = [",".join(am.RAW_CSV_HEADER)]
    for e in entries:
        utc = datetime.fromtimestamp(e["t"] // 1000, timezone.utc)
        lines.append(",".join([
            utc.strftime("%Y-%m-%dT%H:%M:%SZ"), utc.astimezone(LONDON).strftime("%Y-%m-%d %H:%M:%S"),
            str(e["entry_id"]), str(e["seq"]), csv_num(e["p_trk"], 3), csv_num(e["p_fix"], 3), csv_num(e["p_use"], 3),
            csv_num(e["ang_panel"], 3), csv_num(e["ang_sun"], 3), csv_num(e["temp"], 3), csv_num(e["vbat"], 3),
            e["state"] or "", str(e["run"]), e["panel"] or "", str(e["fault"]), "1" if e["sim"] else "0",
            e["status"] or "",
        ]))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="") as fh:
        fh.write("\r\n".join(lines) + "\r\n")
    print("wrote %d SIMULATED rows to %s" % (len(entries), os.path.normpath(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
