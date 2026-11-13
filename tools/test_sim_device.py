"""
Unit tests for tools/sim_device.py (contract 3.1, 4, 7.1-7.3, 7.5).

    python3 -m pytest tools/test_sim_device.py -q
"""
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sim_device as sd  # noqa: E402

T0 = sd.parse_iso("2026-11-20T00:00:00Z")


class FakeTx:
    """Transport double: scripted outcomes, records every request."""

    def __init__(self, script=()):
        self.script = list(script)
        self.calls = []
        self.read_key = None
        self.server_seq = 0

    def bulk(self, records, now):
        outcome = self.script.pop(0) if self.script else "ok"
        self.calls.append((now, len(records), records[0]["seq"], records[-1]["seq"], outcome))
        if outcome in ("ok", "in_doubt"):
            self.server_seq = max(self.server_seq, records[-1]["seq"])
        return outcome, {"ok": 202, "fail": 500, "in_doubt": 0}[outcome], ""

    def last_seq(self, now):
        return self.server_seq or None


def device(tx, **kw):
    world = sd.World(seed=3, scenario=kw.pop("scenario", "cloudy"))
    return sd.Device(world, tx, **kw)


def test_record_json_formats_and_omitted_fields():
    rec = {"t": T0, "p_trk": 12.0, "p_fix": 0.04, "p_use": 105.25, "ang_panel": -3.0, "ang_sun": None, "temp": 8.0,
           "vbat": 3.9, "seq": 7, "status": "IDL;r=0;p=A;e=0;s=1"}
    obj = json.loads(sd.record_json(rec))
    assert obj["created_at"] == "2026-11-20 00:00:00 +0000"
    assert "field5" not in obj                       # unknown value is omitted, never written as 0
    text = sd.record_json(rec)
    assert '"field1":12.0,' in text and '"field2":0.0,' in text and '"field3":105.2' in text
    assert '"field7":3.900,' in text and '"field8":7,' in text
    rec["ang_sun"] = 1.26
    assert '"field5":1.3' in sd.record_json(rec)


def test_every_sample_is_marked_simulated_and_status_grammar():
    d = device(FakeTx())
    for k in range(1, 1441):
        r = d.on_minute(T0 + 60 * k)
        assert re.match(r"^(TRK|MOV|HLD|IDL|STW);r=0;p=A;e=\d;s=1$", r["status"])
    assert d.seq == 1440


def test_world_is_deterministic_and_plausible():
    a, b = sd.World(seed=5, scenario="clear"), sd.World(seed=5, scenario="clear")
    noon, night = T0 + 12 * 3600, T0 + 2 * 3600
    assert a.sample(noon) == b.sample(noon)
    assert a.sample(night)["p_trk"] == 0 and a.sample(night)["state"] == "IDL" and a.sample(night)["ang_sun"] is None
    s = a.sample(noon)
    assert 500 < s["p_fix"] <= 5400 and 500 < s["p_trk"] <= 5400 and s["ang_sun"] is not None
    day = [a.sample(T0 + 60 * k) for k in range(1440)]
    e_trk, e_fix, e_use = (sum(x[k] for x in day) / 60000 for k in ("p_trk", "p_fix", "p_use"))
    assert e_trk > e_fix > 0                         # tracking gains on a clear day
    assert 100 <= sum(x["p_use"] for x in day) / 1440 <= 150      # own use 100-150 mW average
    moves = [x for x in day if x["state"] == "MOV"]
    assert 10 <= len(moves) <= 40 and all(x["t"] % 1200 == 0 for x in moves)   # re-aim every 20 min in daylight
    assert all(-60.0 <= x["ang_panel"] <= 60.0 for x in day)


def test_summer_has_more_energy_than_november_and_lamp_is_indoor():
    nov = sd.World(seed=1, scenario="clear", season="nov")
    summer = sd.World(seed=1, scenario="clear", season="summer")
    e = lambda w: sum(w.sample(T0 + 60 * k)["p_fix"] for k in range(1440))
    assert e(summer) > 2 * e(nov)
    lamp = sd.World(seed=1, scenario="lamp")
    s = lamp.sample(T0 + 3 * 3600)                   # 03:00: the lamp does not care about the time of day
    assert s["p_trk"] > 300 and s["ang_sun"] is not None and s["p_trk"] < 2000


def test_calibration_run_keeps_both_panels_fixed_and_panel_swap_changes_mismatch():
    cal_a = sd.World(seed=1, scenario="clear", run=900, panel="A")
    cal_b = sd.World(seed=1, scenario="clear", run=900, panel="B")
    t = T0 + 12 * 3600
    assert cal_a.sample(t)["ang_panel"] == 0.0 and cal_a.sample(t)["state"] == "STW"
    ra = cal_a.sample(t)["p_trk"] / cal_a.sample(t)["p_fix"]
    rb = cal_b.sample(t)["p_trk"] / cal_b.sample(t)["p_fix"]
    assert ra > 1.0 > rb                             # panel B is the weaker panel


def test_next_slot_is_second_20_of_every_upload_minute():
    d = device(FakeTx())
    assert d.next_slot(T0) == T0 + 20
    assert d.next_slot(T0 + 20) == T0 + 920
    assert d.next_slot(T0 + 21) == T0 + 920


def test_normal_uploads_every_15_min_with_15_records():
    tx = FakeTx()
    d = device(tx)
    d.run_loop(T0, T0 + 3600)
    assert [c[0] - T0 for c in tx.calls] == [920, 1820, 2720]
    assert [c[1] for c in tx.calls] == [15, 15, 15]
    assert tx.calls[0][2:4] == (1, 15)


def test_backoff_1_2_4_8_15_then_cap_and_fault9():
    tx = FakeTx(["fail"] * 7)
    d = device(tx)
    d.run_loop(T0, T0 + 3 * 3600)
    t = [c[0] for c in tx.calls]
    assert [(b - a) // 60 for a, b in zip(t, t[1:])][:7] == [1, 2, 4, 8, 15, 15, 15]
    assert tx.calls[7][4] == "ok" and tx.calls[7][2] == 1         # nothing lost, oldest first
    assert d.stats["fault9_samples"] > 0 and d.stats["failures"] == 7
    assert d.fault_code(T0 + 3 * 3600) == 0                       # cleared after the successful upload


def test_backlog_batches_of_960_spaced_16_s():
    tx = FakeTx()
    d = device(tx, wifi_down=[(T0, T0 + 2 * 86400)])
    d.run_loop(T0, T0 + 2 * 86400 + 1800)
    sent = [c for c in tx.calls if c[1] > 100]
    assert [c[1] for c in sent] == [960, 960, 960]
    assert all(b[0] - a[0] >= 16 for a, b in zip(tx.calls, tx.calls[1:]))
    assert max(c[1] for c in tx.calls) <= 960
    assert d.ack_seq >= 2880


def test_in_doubt_batch_is_resent_and_read_key_skips_it():
    tx = FakeTx(["in_doubt"])
    d = device(tx)
    d.run_loop(T0, T0 + 1800)
    assert tx.calls[0][2:5] == (1, 15, "in_doubt") and tx.calls[1][2] == 1      # same records again
    tx2 = FakeTx(["in_doubt"])
    tx2.read_key = "R"
    d2 = device(tx2)
    d2.run_loop(T0, T0 + 1800)
    assert tx2.calls[1][2] == 16 and d2.stats["skipped_after_doubt"] == 15     # server already had 1..15


def test_overflow_drops_oldest_and_raises_fault6_on_next_sample():
    d = device(FakeTx(), capacity=10, wifi_down=[(T0, T0 + 10 ** 9)])
    recs = [d.on_minute(T0 + 60 * k) for k in range(1, 14)]
    assert [r["seq"] for r in d.pending] == list(range(4, 14))
    assert d.stats["overwritten"] == 3
    faults = [int(re.search(r"e=(\d)", r["status"]).group(1)) for r in recs]
    assert faults[:10] == [0] * 10 and faults[10] == 0 and faults[11:] == [6, 6]


def test_rtc_lost_logs_nothing_until_ntp_then_reports_fault4_once():
    d = device(FakeTx(), wifi_down=[(T0, T0 + 600)], rtc_lost=True)
    out = [d.on_minute(T0 + 60 * k) for k in range(1, 14)]
    assert out[:10] == [None] * 10                   # minutes 1..9 invalid, minute 10 = the one NTP arrived in
    assert out[10]["t"] == T0 + 600 and "e=4" in out[10]["status"] and out[10]["seq"] == 1
    assert "e=0" in out[11]["status"]


def test_state_survives_restart_and_crash(tmp_path):
    state = str(tmp_path / "s")
    tx = FakeTx()
    d = device(tx, state_dir=state)
    try:
        d.run_loop(T0, T0 + 7200, crash_at=T0 + 3600 + 920)       # dies while the 01:15:20 request is out
        assert False, "should have crashed"
    except sd.Crash:
        d.save_rtc(d.clock)
    assert tx.calls[-1][0] == T0 + 3600 + 920
    lost_ack = tx.calls[-1][2:4]
    tx2 = FakeTx()
    d2 = device(tx2, state_dir=state)
    assert d2.clock == T0 + 3600 + 920 and d2.seq == lost_ack[1]
    assert [r["seq"] for r in d2.pending] == list(range(lost_ack[0], lost_ack[1] + 1))
    d2.run_loop(d2.clock, d2.clock + 1800)
    assert tx2.calls[0][2] == lost_ack[0]            # the unacknowledged batch goes out again
    assert tx2.calls[0][0] >= T0 + 3600 + 920 + 16   # not sooner than 16 s after the reboot


def test_cli_csv_mode(tmp_path):
    out = str(tmp_path / "import.csv")
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sim_device.py")
    r = subprocess.run([sys.executable, script, "csv", "--days", "1", "--end", "2026-11-13T00:00:00Z", "--out", out,
                        "--seq-start", "101", "--run", "5", "--panel", "B"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    lines = open(out).read().splitlines()
    assert lines[0] == "created_at,field1,field2,field3,field4,field5,field6,field7,field8,status"
    assert len(lines) == 1441
    first, last = lines[1].split(","), lines[-1].split(",")
    assert first[0] == "2026-11-12T00:00:00+00:00" and last[0] == "2026-11-12T23:59:00+00:00"
    assert first[8] == "101" and last[8] == "1540"
    assert all(len(l.split(",")) == 10 and l.endswith(";s=1") and ";r=5;p=B;" in l for l in lines[1:])
    assert first[5] == ""                            # no sun angle at midnight: empty cell, not 0


def test_cli_usage_errors_exit_2():
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sim_device.py")
    env = {k: v for k, v in os.environ.items() if not k.startswith("TS_")}
    assert subprocess.run([sys.executable, script, "run", "--minutes", "1", "--virtual"], env=env,
                          capture_output=True).returncode == 2
    assert subprocess.run([sys.executable, script, "--bogus"], env=env, capture_output=True).returncode == 2
