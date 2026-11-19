"""
pytest for ast_metrics.py (CONTRACT.md v1.5 section 6).

Expected values are worked out by hand in the comments, not by calling the
code under test. Run:  python3 -m pytest -q analysis/py
"""
import math
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ast_metrics as am  # noqa: E402

DAY = "2026-11-20"
T0 = am.day_start_ms(DAY)                 # 2026-11-20 00:00:00 UTC
NEXT_DAY_NOON = T0 + am.DAY_MS + 12 * 3600 * 1000


def iso(ms):
    from datetime import datetime, timezone
    return datetime.fromtimestamp(ms // 1000, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def feed(minute, seq, trk="100.0", fix="80.0", use="5.0", panel="0.0", sun="0.0",
         status="TRK;r=1;p=A;e=0", entry_id=None, day_ms=T0, temp=None, v_supply="5.020"):
    """One ThingSpeak-style channel-1 feed object (strings, None = null)."""
    return {
        "created_at": iso(day_ms + minute * am.MIN_MS),
        "entry_id": entry_id if entry_id is not None else (seq if seq is not None else 10_000 + minute),
        "field1": trk, "field2": fix, "field3": use, "field4": panel, "field5": sun,
        "field6": temp, "field7": v_supply, "field8": None if seq is None else str(seq), "status": status,
    }


def entries(feeds):
    return am.parse_feeds(feeds)


# ------------------------------------------------------------------ parsing

def test_parse_number():
    assert am.parse_number("12.5") == 12.5
    assert am.parse_number(" 7 ") == 7.0
    assert am.parse_number("-0.3") == -0.3
    assert am.parse_number("1e3") == 1000.0
    assert am.parse_number(".5") == 0.5
    assert am.parse_number("5.") == 5.0
    assert am.parse_number(3) == 3.0
    for bad in (None, "", "  ", "abc", "12abc", "NaN", "Infinity", "1,000", "1_000", "--1", True):
        assert am.parse_number(bad) is None, bad


def test_parse_status_grammar():
    assert am.parse_status("TRK;r=12;p=A;e=0") == {"state": "TRK", "run": 12, "panel": "A", "fault": 0, "sim": False}
    assert am.parse_status("MOV;r=12;p=A;e=0;s=1")["sim"] is True
    assert am.parse_status("FLT;e=8") == {"state": "FLT", "run": 0, "panel": None, "fault": 8, "sim": False}
    # unknown keys and parts without '=' are ignored; order does not matter
    assert am.parse_status("HLD;zz=9;junk;p=B;r=901") == {"state": "HLD", "run": 901, "panel": "B", "fault": 0, "sim": False}
    # null / empty status
    assert am.parse_status(None)["state"] is None
    assert am.parse_status("")["state"] is None
    # malformed integers fall back to 0; 's' must be exactly 1; last value of a repeated key wins
    assert am.parse_status("TRK;r=abc;e=1.5")["run"] == 0
    assert am.parse_status("TRK;r=abc;e=1.5")["fault"] == 0
    assert am.parse_status("TRK;s=0")["sim"] is False
    assert am.parse_status("TRK;s=true")["sim"] is False
    assert am.parse_status("TRK;s=1;s=0")["sim"] is False
    assert am.parse_status("TRK;xs=1")["sim"] is False
    assert am.parse_status("TRK;p=")["panel"] is None


def test_parse_time_formats():
    t = 1795132800000                      # 2026-11-20T00:00:00Z = 20777 days * 86400 s
    assert 20777 * 86400 * 1000 == t
    assert am.parse_time("2026-11-20T00:00:00Z") == t
    assert am.parse_time("2026-11-20T00:00:00+00:00") == t
    assert am.parse_time("2026-11-20 00:00:00 +0000") == t
    assert am.parse_time("2026-11-20 00:00:00 UTC") == t
    assert am.parse_time("2026-11-20 00:00:00") == t
    assert am.parse_time("2026-11-20T01:00:00+01:00") == t
    assert am.parse_time("2026-11-19T19:00:00-05:00") == t
    assert am.parse_time("not a time") is None
    assert am.parse_time(None) is None
    assert am.utc_day(t) == "2026-11-20"
    assert am.utc_day(t - 1) == "2026-11-19"
    assert am.day_start_ms("2026-11-20") == t
    assert am.add_days("2026-11-20", -20) == "2026-10-31"
    assert am.add_days("2026-12-31", 1) == "2027-01-01"


def test_parse_feed_fields_and_nulls():
    e = am.parse_feed({"created_at": "2026-11-20T10:05:00Z", "entry_id": 77, "field1": "250.4", "field2": "200.1",
                       "field3": "12.0", "field4": "-12.5", "field5": None, "field6": None, "field7": "5.012",
                       "field8": "4321", "status": "HLD;r=3;p=B;e=5"})
    assert e["t"] == T0 + 605 * am.MIN_MS
    assert (e["p_trk"], e["p_fix"], e["p_use"], e["ang_panel"]) == (250.4, 200.1, 12.0, -12.5)
    assert e["ang_sun"] is None and e["temp"] is None
    assert e["v_supply"] == 5.012 and e["seq"] == 4321 and e["entry_id"] == 77
    assert (e["state"], e["run"], e["panel"], e["fault"], e["sim"]) == ("HLD", 3, "B", 5, False)
    # a channel that does not have field6 enabled does not send the key at all
    e2 = am.parse_feed({"created_at": "2026-11-20T10:05:00Z", "entry_id": 1})
    assert e2["p_trk"] is None and e2["seq"] is None and e2["state"] is None
    # an entry without a readable time is dropped
    assert am.parse_feeds([{"created_at": "garbage", "entry_id": 1}]) == []


# ---------------------------------------------------------- energy and gain

def test_hand_computed_day():
    # 4 samples at 00:00..00:03.
    #   P_trk 120.0 + 130.0 + 110.0 + 140.0 = 500.0 mW  -> E_trk = 500/60000 Wh = 0.008333...
    #   P_fix 100.0 * 4                      = 400.0 mW  -> E_fix = 400/60000    = 0.006666...
    #   P_use   5.0 * 4                      =  20.0 mW  -> E_use =  20/60000    = 0.000333...
    #   gross = 100 * (500-400)/400 = 25 %, net = 100 * (500-400-20)/400 = 20 %
    #   errors (sun - panel): +1.0, -2.0, +0.5, (IDL: not counted) -> MAE = 3.5/3 = 1.16666...
    fs = [
        feed(0, 1, trk="120.0", fix="100.0", panel="10.0", sun="11.0", status="TRK;r=1;p=A;e=0"),
        feed(1, 2, trk="130.0", fix="100.0", panel="10.0", sun="8.0", status="MOV;r=1;p=A;e=0"),
        feed(2, 3, trk="110.0", fix="100.0", panel="10.0", sun="10.5", status="HLD;r=1;p=A;e=0"),
        feed(3, 4, trk="140.0", fix="100.0", panel="10.0", sun="40.0", status="IDL;r=1;p=A;e=0"),
    ]
    s = am.summarise(entries(fs), 1440)
    assert s["n"] == 4 and s["n_unique"] == 4 and s["duplicates"] == 0
    assert s["e_trk"] == pytest.approx(500 / 60000, rel=1e-12)
    assert s["e_fix"] == pytest.approx(400 / 60000, rel=1e-12)
    assert s["e_use"] == pytest.approx(20 / 60000, rel=1e-12)
    assert s["g_gross"] == pytest.approx(25.0, abs=1e-9)
    assert s["g_net"] == pytest.approx(20.0, abs=1e-9)
    assert s["n_pointing"] == 3
    assert s["mae"] == pytest.approx(3.5 / 3, rel=1e-12)
    assert s["complete"] == pytest.approx(100 * 4 / 1440, rel=1e-12)   # 0.27777... %
    assert s["moves"] == 1 and s["gaps"] == 0 and s["n_sim"] == 0
    assert s["first"] == T0 and s["last"] == T0 + 3 * am.MIN_MS
    # histogram: +1.0 -> bin 32, -2.0 -> bin 29, +0.5 -> bin 31
    assert s["hist"][32] == 1 and s["hist"][29] == 1 and s["hist"][31] == 1 and sum(s["hist"]) == 3


def test_empty_day():
    s = am.summarise([], 1440)
    assert s["n"] == 0 and s["n_unique"] == 0
    assert (s["e_trk"], s["e_fix"], s["e_use"]) == (0.0, 0.0, 0.0)
    assert s["g_gross"] is None and s["g_net"] is None and s["mae"] is None
    assert s["complete"] == 0.0
    assert sum(s["hist"]) == 0 and len(s["hist"]) == 62
    # with no expected count there is nothing to be complete against
    assert am.summarise([])["complete"] is None


def test_e_fix_zero_gives_no_gain():
    s = am.summarise(entries([feed(0, 1, fix="0.0"), feed(1, 2, fix="0.0")]), 1440)
    assert s["e_fix"] == 0.0 and s["e_trk"] > 0
    assert s["g_gross"] is None and s["g_net"] is None
    # a null fixed power counts as 0 as well
    s = am.summarise(entries([feed(0, 1, fix=None)]), 1440)
    assert s["g_gross"] is None and s["missing_power_fields"] == 1


def test_missing_fields_count_as_zero_power_and_no_pointing():
    # sample 2 has no P_trk and no sun angle: sum P_trk = 100 + 0 + 100 = 200 mW
    fs = [feed(0, 1, sun="2.0"), feed(1, 2, trk=None, sun=None), feed(2, 3, sun="-2.0", panel=None)]
    s = am.summarise(entries(fs), 1440)
    assert s["e_trk"] == pytest.approx(200 / 60000, rel=1e-12)
    assert s["missing_power_fields"] == 1
    assert s["n_pointing"] == 1 and s["mae"] == pytest.approx(2.0)   # only the first has both angles
    assert s["n_unique"] == 3


def test_negative_power_is_noted_but_summed():
    s = am.summarise(entries([feed(0, 1, trk="-3.0"), feed(1, 2, trk="9.0")]), 1440)
    assert s["negative_power"] == 1
    assert s["e_trk"] == pytest.approx(6 / 60000, rel=1e-12)


# ---------------------------------------------------------- de-duplication

def test_duplicates_keep_lowest_entry_id():
    # seq 2 arrives twice (batch re-sent): entry 12 first, entry 15 later with a DIFFERENT power
    # so the test can tell which copy was used. Kept: 100 + 200 + 100 = 400 mW.
    fs = [
        feed(0, 1, entry_id=11, trk="100.0"),
        feed(1, 2, entry_id=15, trk="999.0"),
        feed(1, 2, entry_id=12, trk="200.0"),
        feed(2, 3, entry_id=13, trk="100.0"),
        feed(2, 3, entry_id=16, trk="100.0"),
    ]
    es = entries(fs)
    unique, dropped = am.dedupe(es)
    assert dropped == 2
    assert [e["entry_id"] for e in unique] == [11, 12, 13]
    s = am.summarise(es, 1440)
    assert s["n"] == 5 and s["n_unique"] == 3 and s["duplicates"] == 2
    assert s["e_trk"] == pytest.approx(400 / 60000, rel=1e-12)
    assert s["complete"] == pytest.approx(100 * 3 / 1440, rel=1e-12)
    assert s["gaps"] == 0


def test_duplicates_without_entry_id_keep_the_first_received():
    a = am.parse_feed(feed(4, 9, entry_id=None, trk="1.0"))
    b = am.parse_feed(feed(4, 9, entry_id=None, trk="2.0"))
    a["entry_id"] = b["entry_id"] = None           # e.g. rows from a CSV without entry ids
    unique, dropped = am.dedupe([a, b])
    assert dropped == 1 and unique[0]["p_trk"] == 1.0


def test_same_seq_at_another_minute_is_a_new_sample():
    # Review finding 3: the device's seq restarts after a flash format, so the
    # same seq at a different minute is a different sample and must be kept;
    # only a copy with the same seq AND the same created_at is a duplicate.
    fs = [feed(0, 1, entry_id=1), feed(1, 2, entry_id=2), feed(5, 1, entry_id=3), feed(6, 2, entry_id=4),
          feed(6, 2, entry_id=5)]
    unique, dropped = am.dedupe(entries(fs))
    assert dropped == 1
    assert [e["entry_id"] for e in unique] == [1, 2, 3, 4]


def test_entries_without_seq_are_all_kept():
    fs = [feed(0, None, entry_id=1), feed(0, None, entry_id=2), feed(1, None, entry_id=3), feed(1, 7, entry_id=4)]
    s = am.summarise(entries(fs), 1440)
    assert s["n_unique"] == 4 and s["duplicates"] == 0


def test_duplicate_does_not_count_twice_in_pointing_or_sim():
    fs = [feed(0, 1, entry_id=1, sun="3.0", status="TRK;r=1;p=A;e=0;s=1"),
          feed(0, 1, entry_id=2, sun="3.0", status="TRK;r=1;p=A;e=0;s=1")]
    s = am.summarise(entries(fs), 1440)
    assert s["n_pointing"] == 1 and s["n_sim"] == 1 and s["hist"][34] == 1


# ------------------------------------------------------------ pointing error

def test_histogram_edges():
    assert am.HIST_BINS == 62 and len(am.hist_labels()) == 62
    assert am.hist_index(-30.1) == 0
    assert am.hist_index(-30.0) == 1          # lower edge belongs to bin 1 = [-30, -29)
    assert am.hist_index(-29.0001) == 1
    assert am.hist_index(-29.0) == 2
    assert am.hist_index(-0.0001) == 30       # [-1, 0)
    assert am.hist_index(0.0) == 31           # [0, 1)
    assert am.hist_index(0.9999) == 31
    assert am.hist_index(29.0) == 60          # [29, 30)
    assert am.hist_index(29.9999) == 60
    assert am.hist_index(30.0) == 61          # upper edge is overflow
    assert am.hist_index(75.0) == 61
    assert am.hist_labels()[0] == "<-30" and am.hist_labels()[1] == "[-30,-29)" and am.hist_labels()[61] == ">=30"


def test_histogram_edges_through_summarise():
    fs = [feed(0, 1, panel="30.0", sun="0.0"),     # err -30 -> bin 1
          feed(1, 2, panel="0.0", sun="30.0"),     # err +30 -> bin 61
          feed(2, 3, panel="30.1", sun="0.0"),     # err -30.1 -> bin 0
          feed(3, 4, panel="0.0", sun="29.9")]     # err +29.9 -> bin 60
    s = am.summarise(entries(fs), 1440)
    assert [s["hist"][i] for i in (0, 1, 60, 61)] == [1, 1, 1, 1]
    assert s["mae"] == pytest.approx((30 + 30 + 30.1 + 29.9) / 4)


def test_states_that_count_for_pointing():
    fs = [feed(i, i + 1, panel="0.0", sun="4.0", status="%s;r=1;p=A;e=0" % st)
          for i, st in enumerate(["TRK", "MOV", "HLD", "IDL", "STW", "FLT"])]
    s = am.summarise(entries(fs), 1440)
    assert s["n_pointing"] == 3 and s["mae"] == pytest.approx(4.0)
    # none counted -> no MAE
    s = am.summarise(entries([feed(0, 1, status="IDL;r=0")]), 1440)
    assert s["mae"] is None and s["n_pointing"] == 0


# -------------------------------------------------------- completeness, gaps

def test_gaps_and_missing_minutes():
    # minutes 0,1,2, then 10 (7 missing), 11, then 14 (2 missing)
    fs = [feed(m, i + 1) for i, m in enumerate([0, 1, 2, 10, 11, 14])]
    s = am.summarise(entries(fs), 1440)
    assert s["gaps"] == 2 and s["missing_min"] == 9 and s["longest_gap_min"] == 7
    # run/period rule: first to last inclusive = 15 minute slots, 6 present
    s = am.summarise(entries(fs))
    assert s["expected"] == 15
    assert s["complete"] == pytest.approx(100 * 6 / 15)


def test_complete_day_is_100_percent():
    fs = [feed(m, m + 1) for m in range(1440)]
    s = am.summarise(entries(fs), 1440)
    assert s["complete"] == 100.0 and s["gaps"] == 0 and s["n_unique"] == 1440
    # 1440 samples of 100 mW = 144000 mW-min = 2.4 Wh
    assert s["e_trk"] == pytest.approx(2.4, rel=1e-12)


def test_expected_for_day_today_rule():
    now_today = T0 + 10 * 3600 * 1000
    newest = T0 + 599 * am.MIN_MS                       # 09:59 -> minutes 0..599 = 600 slots
    assert am.expected_for_day(DAY, now_today, newest) == 600
    assert am.expected_for_day(DAY, now_today, T0) == 1        # only 00:00 received
    assert am.expected_for_day(DAY, now_today, None) == 0      # nothing yet today
    assert am.expected_for_day(DAY, T0 - 1, None) == 0         # day has not started
    assert am.expected_for_day(DAY, T0 + am.DAY_MS, newest) == 1440      # past day (boundary inclusive)
    assert am.expected_for_day(DAY, T0 + am.DAY_MS - 1, T0 + 1439 * am.MIN_MS) == 1440


def test_daily_summaries_today_completeness():
    fs = [feed(m, m + 1) for m in range(0, 600) if m not in (100, 101)]   # 598 of 600
    now_today = T0 + 10 * 3600 * 1000 + 7 * am.MIN_MS
    d = am.daily_summaries(entries(fs), now_today)
    assert len(d) == 1 and d[0]["day"] == DAY and d[0]["expected"] == 600
    assert d[0]["complete"] == pytest.approx(100 * 598 / 600)
    assert d[0]["gaps"] == 1 and d[0]["missing_min"] == 2


def test_day_boundaries():
    # 23:59 on the 20th belongs to the 20th; 00:00 on the 21st belongs to the 21st
    fs = [feed(1439, 1, trk="60.0"), feed(1440, 2, trk="120.0"), feed(1441, 3, trk="120.0")]
    d = am.daily_summaries(entries(fs), T0 + 3 * am.DAY_MS)
    assert [x["day"] for x in d] == ["2026-11-20", "2026-11-21"]
    assert d[0]["n_unique"] == 1 and d[0]["e_trk"] == pytest.approx(60 / 60000)
    assert d[1]["n_unique"] == 2 and d[1]["e_trk"] == pytest.approx(240 / 60000)
    assert d[0]["expected"] == 1440 and d[1]["expected"] == 1440


def test_daily_summaries_with_explicit_days_includes_empty_days():
    fs = [feed(0, 1)]
    d = am.daily_summaries(entries(fs), T0 + 5 * am.DAY_MS, ["2026-11-19", "2026-11-20", "2026-11-21"])
    assert [x["n_unique"] for x in d] == [0, 1, 0]
    assert d[0]["complete"] == 0.0 and d[0]["g_net"] is None


def test_dedup_scope_is_the_day():
    # the same seq on two different days is not a duplicate inside either day
    fs = [feed(0, 5, entry_id=1), feed(1440, 5, entry_id=2)]
    d = am.daily_summaries(entries(fs), T0 + 5 * am.DAY_MS)
    assert [x["n_unique"] for x in d] == [1, 1] and [x["duplicates"] for x in d] == [0, 0]


# ------------------------------------------------------------ runs and panels

def test_run_kind():
    assert am.run_kind(0) is None and am.run_kind(-1) is None
    assert am.run_kind(1) == "test" and am.run_kind(899) == "test"
    assert am.run_kind(900) == "calibration" and am.run_kind(999) == "calibration"
    assert am.run_kind(1000) == "test"


def test_run_summaries_grouping_and_midnight():
    fs = []
    seq = 0
    # run 3 panel A: 23:50 on the 20th to 00:09 on the 21st = 20 minutes across midnight
    for m in range(1430, 1450):
        seq += 1
        fs.append(feed(m, seq, trk="150.0", fix="100.0", use="10.0", status="TRK;r=3;p=A;e=0"))
    # run 4 panel B: 5 minutes, one missing in the middle
    for m in (1500, 1501, 1503, 1504):
        seq += 1
        fs.append(feed(m, seq, trk="90.0", fix="100.0", use="10.0", status="TRK;r=4;p=B;e=0"))
    # run 900 calibration, panel A on the mount
    for m in range(1600, 1610):
        seq += 1
        fs.append(feed(m, seq, trk="102.0", fix="100.0", use="1.0", status="STW;r=900;p=A;e=0"))
    # r=0 samples belong to no run
    fs.append(feed(1700, seq + 1, status="IDL;r=0;p=A;e=0"))
    es = entries(fs)
    runs = am.run_summaries(es)
    assert [(r["run"], r["panel"], r["kind"]) for r in runs] == [(3, "A", "test"), (4, "B", "test"), (900, "A", "calibration")]
    r3, r4, r900 = runs
    assert r3["n_unique"] == 20 and r3["expected"] == 20 and r3["complete"] == 100.0
    assert r3["g_gross"] == pytest.approx(50.0) and r3["g_net"] == pytest.approx(40.0)
    assert r4["n_unique"] == 4 and r4["expected"] == 5 and r4["complete"] == pytest.approx(80.0)
    assert r4["g_gross"] == pytest.approx(-10.0) and r4["g_net"] == pytest.approx(-20.0)
    assert r4["gaps"] == 1 and r4["missing_min"] == 1
    assert r900["g_gross"] == pytest.approx(2.0)
    # the daily view splits run 3 at midnight: 10 samples each side
    d = am.daily_summaries(es, T0 + 5 * am.DAY_MS)
    assert d[0]["day"] == "2026-11-20" and d[0]["n_unique"] == 10
    assert d[1]["day"] == "2026-11-21" and d[1]["n_unique"] == 10 + 4 + 10 + 1


def test_run_spanning_a_panel_swap_gives_two_rows():
    fs = [feed(0, 1, status="TRK;r=7;p=A;e=0"), feed(1, 2, status="TRK;r=7;p=B;e=0"), feed(2, 3, status="TRK;r=7;e=0")]
    runs = am.run_summaries(entries(fs))
    assert [(r["run"], r["panel"]) for r in runs] == [(7, None), (7, "A"), (7, "B")]


def test_panel_summaries_and_calibration():
    fs = []
    seq = 0
    def add(n, start, trk, fix, use, status):
        nonlocal seq
        for m in range(start, start + n):
            seq += 1
            fs.append(feed(m, seq, trk=trk, fix=fix, use=use, status=status))
    add(10, 0, "130.0", "100.0", "5.0", "TRK;r=1;p=A;e=0")     # A: trk 1300 fix 1000 use 50
    add(10, 20, "120.0", "100.0", "5.0", "TRK;r=2;p=A;e=0")    # A: trk 1200 fix 1000 use 50
    add(10, 40, "126.0", "100.0", "6.0", "TRK;r=3;p=B;e=0")    # B: trk 1260 fix 1000 use 60
    add(10, 60, "103.0", "100.0", "1.0", "STW;r=900;p=A;e=0")  # cal A: +3 %
    add(10, 80, "99.0", "100.0", "1.0", "STW;r=901;p=B;e=0;s=1")  # cal B: -1 %
    add(5, 100, "500.0", "100.0", "1.0", "TRK;r=0;p=A;e=0")    # free running: ignored
    es = entries(fs) + entries([feed(0, 1, trk="130.0", status="TRK;r=1;p=A;e=0", entry_id=5000)])  # one duplicate
    ps = am.panel_summaries(es)
    assert [p["panel"] for p in ps] == ["A", "B"]
    a, b = ps
    assert a["runs"] == 2 and a["n"] == 20 and b["runs"] == 1 and b["n"] == 10
    assert a["e_trk"] == pytest.approx(2500 / 60000) and a["e_fix"] == pytest.approx(2000 / 60000)
    assert a["g_gross"] == pytest.approx(25.0) and a["g_net"] == pytest.approx(20.0)     # (2500-2000-100)/2000
    assert b["g_gross"] == pytest.approx(26.0) and b["g_net"] == pytest.approx(20.0)     # (1260-1000-60)/1000
    cal = am.calibration_summary(es)
    assert cal["runs"] == 2 and cal["n"] == 20 and cal["n_sim"] == 10
    assert cal["mismatch"] == pytest.approx(100 * (2020 - 2000) / 2000)                  # +1 %
    assert [(p["panel"], round(p["mismatch"], 9)) for p in cal["by_panel"]] == [("A", 3.0), ("B", -1.0)]
    assert am.calibration_summary(entries([feed(0, 1)])) is None


def test_sim_samples_are_counted_not_excluded():
    fs = [feed(0, 1, status="TRK;r=1;p=A;e=0;s=1"), feed(1, 2, status="TRK;r=1;p=A;e=0"), feed(2, 3, status="IDL;s=1")]
    s = am.summarise(entries(fs), 1440)
    assert s["n_sim"] == 2 and s["n_unique"] == 3
    assert s["e_trk"] == pytest.approx(300 / 60000)


def test_cumulative_series():
    fs = [feed(0, 1, trk="120.0", fix="0.0", use="6.0"), feed(1, 2, trk="120.0", fix="60.0", use="6.0")]
    c = am.cumulative_series(entries(fs))
    assert c[0]["g_net"] is None
    assert c[1]["e_trk"] == pytest.approx(240 / 60000) and c[1]["e_trk_net"] == pytest.approx(228 / 60000)
    assert c[1]["g_net"] == pytest.approx(100 * (240 - 60 - 12) / 60)


# -------------------------------------------------------------- channel 2

def ch2_feed(created, status, f=("1.23457", "1.00000", "0.0500000", "23.4570", "18.4570", "1.2346", "99.9306", "1439"), eid=1):
    out = {"created_at": created, "entry_id": eid, "status": status}
    for i, v in enumerate(f, 1):
        out["field%d" % i] = v
    return out


def test_parse_result_feed():
    r = am.parse_result_feed(ch2_feed("2026-11-21T00:20:00Z", "period=day;date=2026-11-20;v=1.5;ns=3;nd=12"))
    assert r["date"] == "2026-11-20" and r["version"] == "1.5" and r["n_sim"] == 3 and r["duplicates"] == 12
    assert r["e_trk"] == 1.23457 and r["g_net"] == 18.457 and r["n"] == 1439
    # omitted gains come back as null
    r = am.parse_result_feed(ch2_feed("2026-11-21T00:20:00Z", "period=day;date=2026-11-20;v=1.5;ns=0;nd=0",
                                      f=("0", "0", "0", None, None, None, "0.0000", "0")))
    assert r["g_gross"] is None and r["mae"] is None and r["n"] == 0 and r["e_fix"] == 0.0
    # not a day row / malformed date -> date None
    assert am.parse_result_feed(ch2_feed("2026-11-21T00:20:00Z", "period=run;r=3"))["date"] is None
    assert am.parse_result_feed(ch2_feed("2026-11-21T00:20:00Z", "period=day;date=20 Nov"))["date"] is None
    assert am.parse_result_feed(ch2_feed("2026-11-21T00:20:00Z", None))["date"] is None


def test_latest_results_by_date_keeps_newest_row():
    rows = [am.parse_result_feed(f) for f in [
        ch2_feed("2026-11-21T00:20:00Z", "period=day;date=2026-11-20;v=1.5;ns=0;nd=0", eid=1,
                 f=("1.0", "1", "0", "0", "0", "1", "50.0000", "720")),
        ch2_feed("2026-11-21T00:20:01Z", "period=day;date=2026-11-19;v=1.5;ns=0;nd=0", eid=2),
        ch2_feed("2026-11-22T00:20:00Z", "period=day;date=2026-11-20;v=1.5;ns=0;nd=0", eid=3,
                 f=("2.0", "1", "0", "0", "0", "1", "100.0000", "1440")),
        ch2_feed("2026-11-22T00:20:05Z", "period=run;r=4", eid=4),
    ]]
    m = am.latest_results_by_date(rows)
    assert sorted(m) == ["2026-11-19", "2026-11-20"]
    assert m["2026-11-20"]["n"] == 1440 and m["2026-11-20"]["e_trk"] == 2.0
    # order of the input must not matter
    m2 = am.latest_results_by_date(list(reversed(rows)))
    assert m2["2026-11-20"]["n"] == 1440


# --------------------------------------------------------- agreement (6.5)

def test_fmt_sig_matches_dashboard_examples():
    # examples from dashboard/format.js: 12.3, 0.457, 1230, 0.00
    assert am.fmt_sig(12.34) == "12.3"
    assert am.fmt_sig(0.4567) == "0.457"
    assert am.fmt_sig(1234.0) == "1230"
    assert am.fmt_sig(0.0) == "0.00"
    assert am.fmt_sig(9.996) == "10.0"
    assert am.fmt_sig(0.0012345) == "0.00123"
    assert am.fmt_sig(None) == "n/a"
    assert am.fmt_fixed(18.449) == "18.4" and am.fmt_fixed(18.45) == "18.4"   # 18.45 is 18.449999... in binary
    assert am.fmt_fixed(-0.04) == "0.0" and am.fmt_fixed(-3.25) == "-3.3"     # -3.25 is exact: tie away from zero
    assert am.fmt_fixed(None) == "n/a"


def local_summary(**kw):
    s = {"e_trk": 1.234567, "e_fix": 1.0, "e_use": 0.05, "g_gross": 23.4567, "g_net": 18.4567,
         "mae": 1.23456, "complete": 99.930556, "n_unique": 1439}
    s.update(kw)
    return s


def test_agreement_rule():
    row = am.parse_result_feed(ch2_feed("2026-11-21T00:20:00Z", "period=day;date=2026-11-20;v=1.5;ns=0;nd=0"))
    assert am.agreement(local_summary(), row) == ("agree", "")
    assert am.agreement(local_summary(), None) == ("missing", "")
    # 3 s.f. on energy: the row says 1.23457 -> "1.23"; 1.2349 also prints "1.23", 1.2399 prints "1.24"
    assert am.agreement(local_summary(e_trk=1.2349), row)[0] == "agree"
    st, why = am.agreement(local_summary(e_trk=1.2399), row)
    assert st == "differ" and why.startswith("tracked Wh")
    # 1 dp on gains: the row says 18.4570 -> "18.5"; 18.46 also prints "18.5", 18.44 prints "18.4"
    assert am.agreement(local_summary(g_net=18.46), row)[0] == "agree"
    st, why = am.agreement(local_summary(g_net=18.44), row)
    assert st == "differ" and why.startswith("net gain")
    # N exactly
    assert am.agreement(local_summary(n_unique=1440), row)[0] == "differ"
    # both undefined matches; one undefined differs
    row0 = am.parse_result_feed(ch2_feed("2026-11-21T00:20:00Z", "period=day;date=2026-11-20;v=1.5;ns=0;nd=0",
                                         f=("0", "0", "0", None, None, None, "0.0000", "0")))
    empty = am.summarise([], 1440)
    assert am.agreement(empty, row0) == ("agree", "")
    assert am.agreement(local_summary(), row0)[0] == "differ"
    assert am.agreement(empty, row)[0] == "differ"


# ----------------------------------------------------- channel-2 formatting

def test_fmt_6sf():
    assert am.fmt_6sf(0.0) == "0"
    assert am.fmt_6sf(1.23456789) == "1.23457"
    assert am.fmt_6sf(123.456789) == "123.457"
    assert am.fmt_6sf(0.00123456789) == "0.00123457"
    assert am.fmt_6sf(2.4) == "2.40000"
    assert am.fmt_6sf(9.9999996) == "10.0000"          # rounding carries into a new digit
    assert am.fmt_6sf(999999.6) == "1000000"
    assert am.fmt_6sf(1234567.0) == "1234570"
    assert am.fmt_6sf(123456.4) == "123456"
    assert am.fmt_6sf(-0.5) == "-0.500000"
    # every result has exactly 6 significant digits
    for x in (0.000123456, 0.0999999, 1.0, 54.321, 4321.987):
        digits = am.fmt_6sf(x).replace(".", "").lstrip("0")
        assert len(digits) == 6, (x, digits)


def test_fmt_6sf_tie_guard():
    # 1.2349996 rounds to 1.23500 at 6 s.f.; rounding THAT to 3 s.f. could give 1.24 although the
    # unrounded value is 1.23. The guard writes 1.23499 instead.
    assert am.fmt_6sf(1.2349996) == "1.23499"
    assert am.fmt_sig(float(am.fmt_6sf(1.2349996))) == am.fmt_sig(1.2349996) == "1.23"
    assert am.fmt_6sf(1.2350004) == "1.23501"
    assert am.fmt_sig(float(am.fmt_6sf(1.2350004))) == am.fmt_sig(1.2350004) == "1.24"
    assert am.fmt_6sf(0.0012349998) == "0.00123499"
    # not a 3 s.f. boundary: untouched
    assert am.fmt_6sf(1.2345004) == "1.23450"
    assert am.fmt_6sf(1.5) == "1.50000"


def test_fmt_4dp_and_tie_guard():
    assert am.fmt_4dp(18.45678) == "18.4568"
    assert am.fmt_4dp(100.0) == "100.0000"
    assert am.fmt_4dp(-3.21) == "-3.2100"
    assert am.fmt_4dp(-0.00001) == "0.0000"
    assert am.fmt_4dp(18.44996) == "18.4499"            # would be 18.4500 -> reader's 1 dp ambiguous
    assert am.fmt_fixed(float(am.fmt_4dp(18.44996))) == am.fmt_fixed(18.44996) == "18.4"
    assert am.fmt_4dp(18.45004) == "18.4501"
    assert am.fmt_fixed(float(am.fmt_4dp(18.45004))) == am.fmt_fixed(18.45004) == "18.5"
    assert am.fmt_4dp(-2.35004) == "-2.3501"
    assert am.fmt_4dp(18.5) == "18.5000"                # ends in 000, not a 1-dp boundary


def test_tie_guard_keeps_agreement_on_random_values():
    import random
    rnd = random.Random(8)
    for _ in range(20000):
        # values scattered tightly around 3 s.f. / 1 dp rounding boundaries
        base = rnd.choice([0.012345, 1.235, 45.65, 123.5, 0.9995]) * (1 + rnd.uniform(-3e-6, 3e-6))
        assert am.fmt_sig(float(am.fmt_6sf(base))) == am.fmt_sig(base), base
        pct = rnd.choice([18.45, -3.25, 0.05, 99.95, -0.05]) + rnd.uniform(-2e-4, 2e-4)
        assert am.fmt_fixed(float(am.fmt_4dp(pct))) == am.fmt_fixed(pct), pct


def test_format_ch2_row_and_ch2_rows():
    # 2 samples on the 20th: E_trk = 240/60000 = 0.004 Wh, E_fix = 200/60000 = 0.00333333 Wh,
    # E_use = 10/60000 = 0.000166667 Wh, gross 20 %, net 15 %, MAE 1.5 deg, complete 2/1440 = 0.1389 %
    fs = [feed(0, 1, trk="120.0", fix="100.0", use="5.0", panel="0.0", sun="1.0", status="TRK;r=1;p=A;e=0;s=1"),
          feed(1, 2, trk="120.0", fix="100.0", use="5.0", panel="0.0", sun="-2.0"),
          feed(1, 2, trk="120.0", fix="100.0", use="5.0", panel="0.0", sun="-2.0", entry_id=99)]
    now_s = (T0 + am.DAY_MS) // 1000 + 20 * 60            # 00:20:00 UTC on the 21st
    rows = am.ch2_rows(entries(fs), now_s, ndays=3)
    assert [r["status"] for r in rows] == [
        "period=day;date=2026-11-18;v=1.5;ns=0;nd=0",
        "period=day;date=2026-11-19;v=1.5;ns=0;nd=0",
        "period=day;date=2026-11-20;v=1.5;ns=1;nd=1",
    ]
    assert [r["created_at"] for r in rows] == ["2026-11-21 00:20:00 +0000", "2026-11-21 00:20:01 +0000",
                                               "2026-11-21 00:20:02 +0000"]
    assert rows[0] == {"created_at": "2026-11-21 00:20:00 +0000", "field1": "0", "field2": "0", "field3": "0",
                       "field7": "0.0000", "field8": "0", "status": "period=day;date=2026-11-18;v=1.5;ns=0;nd=0"}
    last = rows[2]
    assert last["field1"] == "0.00400000" and last["field2"] == "0.00333333" and last["field3"] == "0.000166667"
    assert last["field4"] == "20.0000" and last["field5"] == "15.0000" and last["field6"] == "1.5000"
    assert last["field7"] == "0.1389" and last["field8"] == "2"
    # the day in progress is never written
    assert am.nightly_days(now_s, 2) == ["2026-11-19", "2026-11-20"]
    assert am.nightly_days(now_s + 86399 - 20 * 60, 1) == ["2026-11-20"]
    # written rows read back and agree with the summaries they came from
    days = am.daily_summaries(entries(fs), now_s * 1000, am.nightly_days(now_s, 3))
    for r, d in zip(rows, days):
        back = am.parse_result_feed({**r, "created_at": r["created_at"], "entry_id": 1})
        assert am.agreement(d, back) == ("agree", "")


# -------------------------------------------------------------------- CSV

def test_read_raw_csv(tmp_path):
    p = tmp_path / "raw.csv"
    p.write_text(
        ",".join(am.RAW_CSV_HEADER) + "\r\n"
        "2026-11-20T00:00:00Z,2026-11-20 00:00:00,11,1,120,100,5,0,1,,5.02,TRK,1,A,0,1,TRK;r=1;p=A;e=0;s=1\r\n"
        "2026-11-20T00:01:00Z,2026-11-20 00:01:00,12,2,120.5,100,5,0,,21.5,5.02,IDL,0,,0,0,IDL;r=0;e=0\r\n",
        encoding="utf-8")
    es = am.read_raw_csv(str(p))
    assert len(es) == 2
    assert es[0]["t"] == T0 and es[0]["seq"] == 1 and es[0]["sim"] is True and es[0]["temp"] is None
    assert es[1]["ang_sun"] is None and es[1]["panel"] is None and es[1]["temp"] == 21.5 and es[1]["p_trk"] == 120.5
    s = am.summarise(es, 1440)
    assert s["e_trk"] == pytest.approx(240.5 / 60000) and s["n_sim"] == 1 and s["n_pointing"] == 1
    bad = tmp_path / "bad.csv"
    bad.write_text("a,b\r\n1,2\r\n", encoding="utf-8")
    with pytest.raises(ValueError):
        am.read_raw_csv(str(bad))


def test_wh_constant():
    assert am.WH_PER_MW_SAMPLE == pytest.approx(1 / 60000)
    assert math.isclose(1440 * 1000 * am.WH_PER_MW_SAMPLE, 24.0)   # 1 W for a day = 24 Wh
