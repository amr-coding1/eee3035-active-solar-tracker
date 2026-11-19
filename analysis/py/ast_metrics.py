#!/usr/bin/env python3
"""
ast_metrics.py - Python reference for the AST data-link metrics.

Implements docs/CONTRACT.md v1.5, section 6 (and the parsing rules of sections
3, 4 and 5 that the metrics depend on). The dashboard (dashboard/metrics.js)
and the nightly MATLAB job (analysis/matlab/ast_nightly.m) must give the same
numbers; analysis/tests/run_tests.sh checks MATLAB against this file.

Standard library only. Times are integer milliseconds since the Unix epoch
(the same unit as the dashboard), powers are mW, energies Wh, angles degrees.

Section map
    6.1 de-duplication ............ dedupe()
    6.2 energy and gain ........... summarise()
    6.3 pointing error ............ pointing_error(), hist_index(), summarise()
    6.4 completeness and gaps ..... expected_for_day(), summarise()
    6.5 agreement rule ............ agreement()
    3.2 channel-2 row format ...... format_ch2_row(), ch2_rows()
    4   status grammar ............ parse_status()
    5   runs and panels ........... run_kind(), run_summaries(),
                                    panel_summaries(), calibration_summary()

De-duplication scope (decision, see analysis/README.md): duplicates are removed
inside the set that is being summarised (one UTC day, one run, ...). The result
for a day therefore never depends on how many other days were loaded, which is
what lets the dashboard and MATLAB agree whatever date range each one reads.

Command line:  python3 ast_metrics.py raw_export.csv   -> daily table
"""
from __future__ import annotations

import csv
import math
import re
import sys
from datetime import datetime, timezone
from decimal import ROUND_HALF_UP, Decimal

CONTRACT_VERSION = "1.5"

DT_S = 60                                  # nominal sample interval [s]
MIN_MS = 60_000
DAY_MS = 86_400_000
WH_PER_MW_SAMPLE = DT_S / 3600 / 1000      # one sample of 1 mW = this many Wh
HIST_LO = -30
HIST_HI = 30
HIST_BINS = HIST_HI - HIST_LO + 2          # underflow + 60 + overflow = 62
TRACKING_STATES = ("TRK", "MOV", "HLD")    # states that count for pointing error
CAL_RUN_MIN = 900                          # run ids 900-999 = calibration runs
CAL_RUN_MAX = 999
NA = "n/a"

RAW_CSV_HEADER = [
    "created_at_utc", "time_uk", "entry_id", "seq", "p_trk_mw", "p_fix_mw", "p_use_mw", "ang_panel_deg",
    "ang_sun_deg", "temp_c", "v_supply_v", "state", "run", "panel", "fault", "sim", "status",
]

# ------------------------------------------------------------------ parsing

_NUM_RE = re.compile(r"^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$")
_INT_RE = re.compile(r"^-?\d+$")
_TIME_RE = re.compile(
    r"^(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2}):(\d{2})(?:\.\d+)?\s*(Z|UTC|[+-]\d{2}:?\d{2})?$"
)


def parse_number(v):
    """Field text -> float, or None for null / empty / not a plain decimal number.

    ThingSpeak returns field values as strings. Accepted: optional sign, digits
    with an optional decimal point, optional exponent, surrounding white space.
    Everything else (including 'NaN' and 'Infinity') is None, as on the dashboard.
    """
    if v is None or isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return float(v) if math.isfinite(v) else None
    s = str(v).strip()
    if not _NUM_RE.match(s):
        return None
    x = float(s)
    return x if math.isfinite(x) else None


def _int_or_0(v):
    return int(v) if _INT_RE.match(v) else 0


def parse_status(s):
    """'TRK;r=12;p=A;e=0;s=1' -> {'state','run','panel','fault','sim'} (section 4).

    Unknown keys are ignored; a repeated key takes its last value; a missing or
    malformed integer is 0; `sim` is True only for exactly 's=1'.
    """
    out = {"state": None, "run": 0, "panel": None, "fault": 0, "sim": False}
    if not s:
        return out
    parts = str(s).split(";")
    out["state"] = parts[0] or None
    for p in parts[1:]:
        i = p.find("=")
        if i < 0:
            continue
        k, v = p[:i], p[i + 1:]
        if k == "r":
            out["run"] = _int_or_0(v)
        elif k == "p":
            out["panel"] = v or None
        elif k == "e":
            out["fault"] = _int_or_0(v)
        elif k == "s":
            out["sim"] = v == "1"
    return out


def parse_time(text):
    """ThingSpeak / contract time text -> ms since the epoch (UTC), or None.

    Accepts 'YYYY-MM-DDTHH:MM:SSZ', '...+00:00', 'YYYY-MM-DD HH:MM:SS +0000',
    '... UTC' and no zone at all (taken as UTC).
    """
    if text is None:
        return None
    m = _TIME_RE.match(str(text).strip())
    if not m:
        return None
    y, mo, d, h, mi, s = (int(g) for g in m.groups()[:6])
    try:
        t = datetime(y, mo, d, h, mi, s, tzinfo=timezone.utc).timestamp()
    except ValueError:
        return None
    z = m.group(7)
    if z and z not in ("Z", "UTC"):
        sign = -1 if z[0] == "-" else 1
        digits = z[1:].replace(":", "")
        t -= sign * (int(digits[:2]) * 3600 + int(digits[2:]) * 60)
    return int(round(t * 1000))


def parse_feed(f):
    """One ThingSpeak channel-1 feed object -> entry dict used by every metric."""
    st = parse_status(f.get("status"))
    seq = parse_number(f.get("field8"))
    eid = f.get("entry_id")
    return {
        "t": parse_time(f.get("created_at")),
        "entry_id": int(eid) if eid is not None and str(eid).strip() != "" else None,
        "p_trk": parse_number(f.get("field1")),
        "p_fix": parse_number(f.get("field2")),
        "p_use": parse_number(f.get("field3")),
        "ang_panel": parse_number(f.get("field4")),
        "ang_sun": parse_number(f.get("field5")),
        "temp": parse_number(f.get("field6")),
        "v_supply": parse_number(f.get("field7")),
        "seq": None if seq is None else math.trunc(seq),
        "state": st["state"],
        "run": st["run"],
        "panel": st["panel"],
        "fault": st["fault"],
        "sim": st["sim"],
        "status": f.get("status"),
    }


def parse_feeds(feeds):
    """List of feed objects -> list of entries; entries without a readable time are dropped."""
    out = []
    for f in feeds:
        e = parse_feed(f)
        if e["t"] is not None:
            out.append(e)
    return out


def read_raw_csv(path):
    """Dashboard raw-sample CSV export (header in docs/PLAN.md) -> list of entries."""
    out = []
    with open(path, newline="", encoding="utf-8") as fh:
        rd = csv.DictReader(fh)
        missing = [c for c in RAW_CSV_HEADER if c not in (rd.fieldnames or [])]
        if missing:
            raise ValueError("not a dashboard raw export, missing columns: %s" % ", ".join(missing))
        for r in rd:
            t = parse_time(r["created_at_utc"])
            if t is None:
                continue
            seq = parse_number(r["seq"])
            eid = parse_number(r["entry_id"])
            out.append({
                "t": t,
                "entry_id": None if eid is None else int(eid),
                "p_trk": parse_number(r["p_trk_mw"]),
                "p_fix": parse_number(r["p_fix_mw"]),
                "p_use": parse_number(r["p_use_mw"]),
                "ang_panel": parse_number(r["ang_panel_deg"]),
                "ang_sun": parse_number(r["ang_sun_deg"]),
                "temp": parse_number(r["temp_c"]),
                "v_supply": parse_number(r["v_supply_v"]),
                "seq": None if seq is None else math.trunc(seq),
                "state": r["state"] or None,
                "run": _int_or_0(r["run"]),
                "panel": r["panel"] or None,
                "fault": _int_or_0(r["fault"]),
                "sim": r["sim"] == "1",
                "status": r["status"] or None,
            })
    return out


# -------------------------------------------------------------------- time

def utc_day(ms):
    """ms epoch -> 'YYYY-MM-DD' of the UTC day containing it."""
    return datetime.fromtimestamp(ms // 1000, timezone.utc).strftime("%Y-%m-%d")


def day_start_ms(day):
    """'YYYY-MM-DD' -> ms epoch of 00:00:00 UTC on that day."""
    y, m, d = (int(x) for x in day.split("-"))
    return int(datetime(y, m, d, tzinfo=timezone.utc).timestamp()) * 1000


def add_days(day, n):
    return utc_day(day_start_ms(day) + n * DAY_MS)


def _round_half_up(x):
    """JavaScript Math.round: halves go towards +infinity."""
    return math.floor(x + 0.5)


def expected_for_day(day, now_ms, newest_ms_of_day=None):
    """Expected entries for one UTC day (section 6.4).

    1440 for a past day; for the current day the whole minutes from 00:00 UTC
    to the newest received entry of that day inclusive (0 when none yet); 0 for
    a day that has not started.
    """
    start = day_start_ms(day)
    if now_ms >= start + DAY_MS:
        return 1440
    if now_ms < start:
        return 0
    if newest_ms_of_day is None or newest_ms_of_day < start:
        return 0
    return min(1440, (newest_ms_of_day - start) // MIN_MS + 1)


def run_kind(run):
    """Run type from the run id: 900-999 calibration, any other id above 0 a tracking test."""
    if not (run and run > 0):
        return None
    return "calibration" if CAL_RUN_MIN <= run <= CAL_RUN_MAX else "test"


# ----------------------------------------------------------------- metrics

def dedupe(entries):
    """Section 6.1: a duplicate has the same `seq` AND the same created_at (a
    re-sent record); keep the first received copy of each.

    First received = lowest entry_id (an entry without entry_id sorts last),
    then input order. The same seq at another minute is a different sample (the
    device's seq restarted, e.g. after a flash format) and is kept. Entries with
    no seq are all kept. Returns (unique entries in time order, duplicates dropped).
    """
    best = {}
    keep = []
    for i, e in enumerate(entries):
        if e["seq"] is None:
            keep.append((i, e))
            continue
        rank = (math.inf if e["entry_id"] is None else e["entry_id"], e["t"], i)
        key = (e["seq"], e["t"])
        cur = best.get(key)
        if cur is None or rank < cur[0]:
            best[key] = (rank, i, e)
    keep.extend((i, e) for _, i, e in best.values())
    keep.sort(key=lambda p: (p[1]["t"], math.inf if p[1]["entry_id"] is None else p[1]["entry_id"], p[0]))
    unique = [e for _, e in keep]
    return unique, len(entries) - len(unique)


def hist_index(err):
    """0 = underflow (< -30), 1..60 = [-30+k-1, -30+k), 61 = overflow (>= 30)."""
    if err < HIST_LO:
        return 0
    if err >= HIST_HI:
        return HIST_BINS - 1
    return math.floor(err - HIST_LO) + 1


def hist_labels():
    return ["<%d" % HIST_LO] + ["[%d,%d)" % (b, b + 1) for b in range(HIST_LO, HIST_HI)] + [">=%d" % HIST_HI]


def pointing_error(e):
    """theta_sun - theta_panel, or None where the contract says not to count it (6.3)."""
    if e["ang_sun"] is None or e["ang_panel"] is None or e["state"] not in TRACKING_STATES:
        return None
    return e["ang_sun"] - e["ang_panel"]


def summarise(entries, expected=None):
    """Summarise a set of entries (sections 6.1 to 6.4).

    entries  : parsed entries in any order, duplicates included
    expected : minute slots in the period; None = first to last entry inclusive
               (rule for a run or an arbitrary period)

    Keys: n (received), n_unique (N), duplicates, expected, e_trk, e_fix, e_use
    [Wh], g_gross, g_net [%], mae [deg], complete [%], n_pointing, hist (62),
    moves, gaps, longest_gap_min, missing_min, missing_power_fields,
    negative_power, n_sim, first, last [ms]. Undefined values are None.
    """
    unique, dropped = dedupe(entries)
    s = {
        "n": len(entries), "n_unique": len(unique), "duplicates": dropped, "expected": expected or 0,
        "e_trk": 0.0, "e_fix": 0.0, "e_use": 0.0,
        "g_gross": None, "g_net": None, "mae": None, "complete": None,
        "moves": 0, "gaps": 0, "longest_gap_min": 0, "missing_min": 0,
        "missing_power_fields": 0, "negative_power": 0, "n_sim": 0, "n_pointing": 0,
        "hist": [0] * HIST_BINS, "first": None, "last": None,
    }
    if not unique:
        s["complete"] = 0.0 if expected else None
        return s
    s["first"] = unique[0]["t"]
    s["last"] = unique[-1]["t"]

    sum_trk = sum_fix = sum_use = sum_abs_err = 0.0
    for e in unique:
        powers = (e["p_trk"], e["p_fix"], e["p_use"])
        if any(p is None for p in powers):
            s["missing_power_fields"] += 1
        if any(p is not None and p < 0 for p in powers):
            s["negative_power"] += 1          # counted as a quality note; still summed as received
        if e["sim"]:
            s["n_sim"] += 1
        sum_trk += e["p_trk"] or 0.0          # a null power counts as 0 (6.2)
        sum_fix += e["p_fix"] or 0.0
        sum_use += e["p_use"] or 0.0
        if e["state"] == "MOV":
            s["moves"] += 1
        err = pointing_error(e)
        if err is not None:
            sum_abs_err += abs(err)
            s["n_pointing"] += 1
            s["hist"][hist_index(err)] += 1
    s["e_trk"] = sum_trk * WH_PER_MW_SAMPLE
    s["e_fix"] = sum_fix * WH_PER_MW_SAMPLE
    s["e_use"] = sum_use * WH_PER_MW_SAMPLE
    if s["e_fix"] > 0:
        s["g_gross"] = 100 * (s["e_trk"] - s["e_fix"]) / s["e_fix"]
        s["g_net"] = 100 * (s["e_trk"] - s["e_fix"] - s["e_use"]) / s["e_fix"]
    if s["n_pointing"]:
        s["mae"] = sum_abs_err / s["n_pointing"]

    # Gaps: missing minutes between consecutive unique timestamps (6.4).
    prev = None
    for e in unique:
        if prev is not None and e["t"] != prev:
            miss = _round_half_up((e["t"] - prev) / MIN_MS) - 1
            if miss > 0:
                s["gaps"] += 1
                s["missing_min"] += miss
                s["longest_gap_min"] = max(s["longest_gap_min"], miss)
        prev = e["t"]
    if expected is None:
        s["expected"] = _round_half_up((s["last"] - s["first"]) / MIN_MS) + 1
    s["complete"] = 100 * s["n_unique"] / s["expected"] if s["expected"] else None
    return s


def group_by_day(entries):
    """dict 'YYYY-MM-DD' -> entries of that UTC day, days in ascending order."""
    m = {}
    for e in entries:
        m.setdefault(utc_day(e["t"]), []).append(e)
    return dict(sorted(m.items()))


def daily_summaries(entries, now_ms, days=None):
    """Per-UTC-day summaries, oldest first: [{'day': ..., **summary}].

    days=None lists the days that have entries (dashboard behaviour). Pass an
    explicit list of 'YYYY-MM-DD' to get a row for every listed day, including
    days with no entries (nightly job behaviour: N = 0, complete = 0).
    """
    groups = group_by_day(entries)
    wanted = sorted(groups) if days is None else list(days)
    out = []
    for day in wanted:
        es = groups.get(day, [])
        newest = max((e["t"] for e in es), default=None)
        out.append({"day": day, **summarise(es, expected_for_day(day, now_ms, newest))})
    return out


def run_summaries(entries):
    """Per-run summaries for run ids > 0, grouped by (run, panel), sorted by run then panel.

    A run that (wrongly) spans a panel swap shows as two rows. `kind` is 'test'
    or 'calibration'. Completeness uses first-to-last entry inclusive, so a run
    may span midnight.
    """
    groups = {}
    for e in entries:
        if not (e["run"] and e["run"] > 0):
            continue
        groups.setdefault((e["run"], e["panel"]), []).append(e)
    out = []
    for (run, panel) in sorted(groups, key=lambda k: (k[0], k[1] or "")):
        out.append({"run": run, "panel": panel, "kind": run_kind(run), **summarise(groups[(run, panel)])})
    return out


def _gains(e_trk, e_fix, e_use):
    if e_fix > 0:
        return 100 * (e_trk - e_fix) / e_fix, 100 * (e_trk - e_fix - e_use) / e_fix
    return None, None


def panel_summaries(entries):
    """Totals over the tracking-test entries (runs 1-899) per panel letter: the panel-swap check.

    Calibration runs are left out. Entries are de-duplicated first.
    """
    unique, _ = dedupe(entries)
    groups = {}
    for e in unique:
        if run_kind(e["run"]) != "test":
            continue
        g = groups.setdefault(e["panel"] or "?", {"runs": set(), "n": 0, "trk": 0.0, "fix": 0.0, "use": 0.0})
        g["runs"].add(e["run"])
        g["n"] += 1
        g["trk"] += e["p_trk"] or 0.0
        g["fix"] += e["p_fix"] or 0.0
        g["use"] += e["p_use"] or 0.0
    out = []
    for panel in sorted(groups):
        g = groups[panel]
        e_trk, e_fix, e_use = (g[k] * WH_PER_MW_SAMPLE for k in ("trk", "fix", "use"))
        gross, net = _gains(e_trk, e_fix, e_use)
        out.append({"panel": panel, "runs": len(g["runs"]), "n": g["n"], "e_trk": e_trk, "e_fix": e_fix,
                    "e_use": e_use, "g_gross": gross, "g_net": net})
    return out


def calibration_summary(entries):
    """Calibration runs (ids 900-999, both panels fixed side by side), section 5.

    mismatch = 100 (E_trk - E_fix) / E_fix over all calibration entries; it is
    a difference between the panels, never a gain. `by_panel` gives the same
    figure per panel letter on the tracker mount. None when there are no
    calibration entries. Entries are de-duplicated first.
    """
    unique, _ = dedupe(entries)

    def blank():
        return {"runs": set(), "n": 0, "n_sim": 0, "trk": 0.0, "fix": 0.0}

    total = blank()
    groups = {}
    for e in unique:
        if run_kind(e["run"]) != "calibration":
            continue
        for g in (total, groups.setdefault(e["panel"] or "?", blank())):
            g["runs"].add(e["run"])
            g["n"] += 1
            g["n_sim"] += 1 if e["sim"] else 0
            g["trk"] += e["p_trk"] or 0.0
            g["fix"] += e["p_fix"] or 0.0
    if not total["n"]:
        return None

    def done(g):
        e_trk, e_fix = g["trk"] * WH_PER_MW_SAMPLE, g["fix"] * WH_PER_MW_SAMPLE
        return {"runs": len(g["runs"]), "n": g["n"], "n_sim": g["n_sim"], "e_trk": e_trk, "e_fix": e_fix,
                "mismatch": 100 * (e_trk - e_fix) / e_fix if e_fix > 0 else None}

    return {**done(total), "by_panel": [{"panel": p, **done(groups[p])} for p in sorted(groups)]}


def cumulative_series(entries):
    """Running totals through a period, one point per unique entry (time order)."""
    unique, _ = dedupe(entries)
    trk = fix = use = 0.0
    out = []
    for e in unique:
        trk += (e["p_trk"] or 0.0) * WH_PER_MW_SAMPLE
        fix += (e["p_fix"] or 0.0) * WH_PER_MW_SAMPLE
        use += (e["p_use"] or 0.0) * WH_PER_MW_SAMPLE
        out.append({"t": e["t"], "e_trk": trk, "e_fix": fix, "e_trk_net": trk - use,
                    "g_net": 100 * (trk - fix - use) / fix if fix > 0 else None})
    return out


# ------------------------------------------- channel 2 (MATLAB results) side

def parse_result_feed(f):
    """Channel-2 feed -> one MATLAB summary row; `date` is None unless 'period=day;date=YYYY-MM-DD'."""
    kv = {}
    for p in str(f.get("status") or "").split(";"):
        i = p.find("=")
        if i > 0:
            kv[p[:i]] = p[i + 1:]
    date = kv.get("date", "")
    is_day = kv.get("period") == "day" and re.match(r"^\d{4}-\d{2}-\d{2}$", date) is not None
    n = parse_number(f.get("field8"))
    eid = f.get("entry_id")
    return {
        "t": parse_time(f.get("created_at")),
        "entry_id": int(eid) if eid is not None else None,
        "period": kv.get("period") or "day",
        "date": date if is_day else None,
        "version": kv.get("v"),
        "n_sim": int(kv["ns"]) if _INT_RE.match(kv.get("ns", "")) else None,
        "duplicates": int(kv["nd"]) if _INT_RE.match(kv.get("nd", "")) else None,
        "e_trk": parse_number(f.get("field1")),
        "e_fix": parse_number(f.get("field2")),
        "e_use": parse_number(f.get("field3")),
        "g_gross": parse_number(f.get("field4")),
        "g_net": parse_number(f.get("field5")),
        "mae": parse_number(f.get("field6")),
        "complete": parse_number(f.get("field7")),
        "n": n,
    }


def latest_results_by_date(rows):
    """Section 3.2: for each summarised date keep the row with the newest created_at.

    Rows that are not 'period=day;date=...' are ignored. A later row in the
    list wins a created_at tie (feeds arrive oldest first).
    """
    m = {}
    for r in rows:
        if not r["date"] or r["t"] is None:
            continue
        prev = m.get(r["date"])
        if prev is None or r["t"] >= prev["t"]:
            m[r["date"]] = r
    return m


def _js_to_fixed(x, dp):
    """Number.prototype.toFixed: exact binary value, ties away from zero."""
    return format(Decimal(x).quantize(Decimal(1).scaleb(-dp), rounding=ROUND_HALF_UP), "f")


def fmt_sig(x, sig=3):
    """Same text as the dashboard's fmtSig: 3 significant figures, no exponent ('12.3', '0.457', '1230')."""
    if x is None or not math.isfinite(x):
        return NA
    if x == 0:
        return _js_to_fixed(0.0, sig - 1)
    d = Decimal(x)
    mag = math.floor(math.log10(abs(x)))
    rounded = float(d.quantize(Decimal(1).scaleb(d.adjusted() - sig + 1), rounding=ROUND_HALF_UP))
    mag2 = math.floor(math.log10(abs(rounded)))        # 9.996 -> 10.0
    return _js_to_fixed(rounded, max(0, sig - 1 - max(mag, mag2)))


def fmt_fixed(x, dp=1):
    """Same text as the dashboard's fmtFixed: fixed decimals, never '-0.0'."""
    if x is None or not math.isfinite(x):
        return NA
    s = _js_to_fixed(x, dp)
    return _js_to_fixed(0.0, dp) if float(s) == 0 else s


_AGREEMENT = (
    ("e_trk", "tracked Wh", fmt_sig),
    ("e_fix", "fixed Wh", fmt_sig),
    ("e_use", "own use Wh", fmt_sig),
    ("g_gross", "gross gain %", fmt_fixed),
    ("g_net", "net gain %", fmt_fixed),
    ("mae", "pointing error deg", fmt_fixed),
    ("complete", "complete %", fmt_fixed),
)


def agreement(local, matlab):
    """Section 6.5. local = summarise() of a day, matlab = parse_result_feed() row or None.

    Energies equal at 3 significant figures; gross, net, MAE and completeness
    equal at 1 dp (both undefined also matches); N equal exactly.
    Returns (status, reason): status 'agree' | 'differ' | 'missing'; reason
    names the first quantity that differs.
    """
    if not matlab:
        return "missing", ""
    for key, name, fmt in _AGREEMENT:
        a, b = fmt(local[key]), fmt(matlab[key])
        if a != b:
            return "differ", "%s: reference %s, MATLAB %s" % (name, a, b)
    if matlab["n"] is None or local["n_unique"] != matlab["n"]:
        return "differ", "samples: reference %s, MATLAB %s" % (local["n_unique"], matlab["n"])
    return "agree", ""


# ------------------------------------------------- channel-2 row formatting

def fmt_6sf(x):
    """Energy text for channel 2: 6 significant figures, plain decimal, with the tie guard.

    Built from the digits of '%.5e' so that MATLAB, Octave and Python produce
    the same characters. Tie guard: the reader rounds this text again to 3
    significant figures (6.5). If the 6-figure value lands exactly on a
    3-figure rounding boundary (digits 4-6 = '500') the second rounding could
    go the other way from rounding the unrounded value, so the last digit is
    moved one step back towards the unrounded value ('...499' or '...501').
    The written number is then still within 1 unit of the 6th figure.
    """
    if x == 0:
        return "0"
    sign = "-" if x < 0 else ""
    a = abs(x)

    def plain(digits, e):
        if e >= 5:
            return digits + "0" * (e - 5)
        if e >= 0:
            return digits[: e + 1] + "." + digits[e + 1:]
        return "0." + "0" * (-e - 1) + digits

    s = "%.5e" % a
    digits, e = s[0] + s[2:7], int(s[8:])
    if digits[3:] == "500":
        v = float(plain(digits, e))
        if a < v:
            digits = digits[:3] + "499"
        elif a > v:
            digits = digits[:3] + "501"
    return sign + plain(digits, e)


def fmt_4dp(x):
    """Percentage / angle text for channel 2: 4 decimal places, with the tie guard.

    The reader rounds again to 1 dp (6.5); a 4-dp value ending in '500' is
    moved 0.0001 back towards the unrounded value (see fmt_6sf).
    """
    s = "%.4f" % x
    if s.endswith("500"):
        v = float(s)
        if x < v:
            s = "%.4f" % (v - 0.0001)
        elif x > v:
            s = "%.4f" % (v + 0.0001)
    return "0.0000" if s == "-0.0000" else s


def format_ch2_row(day, s):
    """Summary of one UTC day -> channel-2 update fields (section 3.2), without created_at."""
    row = {
        "field1": fmt_6sf(s["e_trk"]),
        "field2": fmt_6sf(s["e_fix"]),
        "field3": fmt_6sf(s["e_use"]),
    }
    if s["g_gross"] is not None:
        row["field4"] = fmt_4dp(s["g_gross"])
        row["field5"] = fmt_4dp(s["g_net"])
    if s["mae"] is not None:
        row["field6"] = fmt_4dp(s["mae"])
    row["field7"] = fmt_4dp(s["complete"] or 0.0)
    row["field8"] = "%d" % s["n_unique"]
    row["status"] = "period=day;date=%s;v=%s;ns=%d;nd=%d" % (day, CONTRACT_VERSION, s["n_sim"], s["duplicates"])
    return row


NIGHTLY_DAYS = 9   # contract v1.5 section 3.2: the nightly job rewrites the last 9 complete UTC days


def nightly_days(now_s, ndays=NIGHTLY_DAYS):
    """The last `ndays` COMPLETE UTC days before `now_s` (POSIX seconds), oldest first."""
    today = utc_day(int(now_s) * 1000)
    return [add_days(today, -k) for k in range(ndays, 0, -1)]


def ch2_rows(entries, now_s, ndays=NIGHTLY_DAYS):
    """What the nightly job must write: one update per complete day, oldest first.

    created_at = run time + k seconds, 'YYYY-MM-DD HH:MM:SS +0000'.
    """
    now_s = int(now_s)
    days = nightly_days(now_s, ndays)
    rows = []
    for k, d in enumerate(daily_summaries(entries, now_s * 1000, days)):
        created = datetime.fromtimestamp(now_s + k, timezone.utc).strftime("%Y-%m-%d %H:%M:%S +0000")
        rows.append({"created_at": created, **format_ch2_row(d["day"], d)})
    return rows


# --------------------------------------------------------------------- CLI

def _main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    entries = read_raw_csv(argv[1])
    now_ms = max((e["t"] for e in entries), default=0) + DAY_MS   # treat every day in the file as past
    print("date        N     dup  sim   E_trk Wh  E_fix Wh  E_use Wh  gross %  net %   MAE deg  complete %")
    for d in daily_summaries(entries, now_ms):
        print("%s  %-5d %-4d %-5d %-9s %-9s %-9s %-8s %-7s %-8s %s" % (
            d["day"], d["n_unique"], d["duplicates"], d["n_sim"], fmt_sig(d["e_trk"]), fmt_sig(d["e_fix"]),
            fmt_sig(d["e_use"]), fmt_fixed(d["g_gross"]), fmt_fixed(d["g_net"]), fmt_fixed(d["mae"]),
            fmt_fixed(d["complete"])))
    for r in run_summaries(entries):
        print("run %d panel %s (%s): N=%d net %s %% gross %s %%" % (
            r["run"], r["panel"], r["kind"], r["n_unique"], fmt_fixed(r["g_net"]), fmt_fixed(r["g_gross"])))
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
