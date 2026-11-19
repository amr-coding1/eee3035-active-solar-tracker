#!/usr/bin/env python3
"""
ast_sim.py - small deterministic simulator of channel-1 data (tests and example figures only).

NOT a physical model of the group's tracker: a plausible shape (sun arc, clouds,
a stepping tracker, servo consumption) so that the analysis code has something
realistic to chew on. Every sample it makes is marked simulated ('s=1' in the
status, contract section 4) unless the caller asks otherwise for a parser test.

simulate_day() returns ThingSpeak-style update objects (contract section 3.1):
{"created_at": "YYYY-MM-DD HH:MM:00 +0000", "field1".."field8": text, "status": text},
with unknown values left out.
"""
import math
import random
from datetime import datetime, timezone

SUNRISE_MIN = 7 * 60 + 20        # 07:20 UTC, Guildford in late November
SUNSET_MIN = 16 * 60 + 5         # 16:05 UTC
P_MAX_MW = 1500.0                # panel output at full simulated sun
CLOUD = {"clear": (1.0, 0.03), "cloudy": (0.55, 0.35), "overcast": (0.12, 0.05)}   # mean level, flicker
DIFFUSE = {"clear": 0.10, "cloudy": 0.45, "overcast": 0.95}                        # share that ignores angle


def day_start_s(day):
    y, m, d = (int(x) for x in day.split("-"))
    return int(datetime(y, m, d, tzinfo=timezone.utc).timestamp())


def created_at(t_s):
    return datetime.fromtimestamp(t_s, timezone.utc).strftime("%Y-%m-%d %H:%M:%S +0000")


def simulate_day(day, seq_start=1, scenario="clear", run=0, panel="A", seed=0, sim=True,
                 minutes=range(1440), calibration=False, mismatch=0.015):
    """One UTC day (or the listed minutes of it) of one-minute samples.

    calibration=True: both panels fixed side by side (run ids 900-999), the
    "tracked" channel reads `mismatch` higher than the fixed one.
    Returns (updates, next_seq).
    """
    rnd = random.Random("%s|%s|%d" % (day, scenario, seed))
    level, flicker = CLOUD[scenario]
    diffuse = DIFFUSE[scenario]
    t0 = day_start_s(day)
    wanted = set(minutes)
    out = []
    seq = seq_start
    panel_angle = 0.0
    cloud = level
    rnd_v = random.Random("%s|%s|%d|vsup" % (day, scenario, seed))   # own stream: other columns keep their values
    for m in range(1440):
        daylight = SUNRISE_MIN <= m < SUNSET_MIN
        moved = False
        sun = None
        if daylight:
            x = (m - SUNRISE_MIN) / (SUNSET_MIN - SUNRISE_MIN)
            sun = -75.0 + 150.0 * x + rnd.gauss(0, 0.4)
            cloud = min(1.0, max(0.03, cloud + 0.25 * (level - cloud) + rnd.gauss(0, flicker * 0.3)))
            g = math.sin(math.pi * x) ** 1.2 * cloud
            if calibration:
                state = "STW"
            else:
                target = max(-60.0, min(60.0, sun))
                if abs(target - panel_angle) > 3.0:          # dead band, then one step per minute
                    panel_angle += max(-6.0, min(6.0, target - panel_angle))
                    moved = True
                state = "MOV" if moved else ("HLD" if scenario == "overcast" and rnd.random() < 0.3 else "TRK")
            direct = (1 - diffuse) * g
            p_fix = P_MAX_MW * (direct * max(0.0, math.cos(math.radians(sun))) + diffuse * g)
            p_trk = P_MAX_MW * (direct * max(0.0, math.cos(math.radians(sun - panel_angle))) + diffuse * g)
            if calibration:
                p_trk = p_fix * (1 + mismatch)
            p_trk *= 1 + rnd.gauss(0, 0.008)
            p_fix *= 1 + rnd.gauss(0, 0.008)
        else:
            state = "IDL"
            if not calibration and panel_angle != 0.0:       # return to level after sunset
                panel_angle += max(-6.0, min(6.0, -panel_angle))
                moved = True
                state = "MOV"
            p_trk = p_fix = 0.0
        p_use = 0.0 if calibration else 11.0 + rnd.gauss(0, 0.4) + (95.0 if moved else 0.0)
        # field7 V_supply: 5 V feed to the tracker, sagging slightly with the load (as firmware/core/ast_sim.cpp)
        v_supply = 5.05 - 0.00015 * max(0.0, p_use) + 0.004 * rnd_v.uniform(-1.0, 1.0)
        if m not in wanted:
            continue                                         # not logged: seq does not advance
        u = {
            "created_at": created_at(t0 + m * 60),
            "field1": "%.1f" % max(0.0, p_trk),
            "field2": "%.1f" % max(0.0, p_fix),
            "field3": "%.1f" % max(0.0, p_use),
            "field4": "%.1f" % panel_angle,
            "field7": "%.3f" % v_supply,
            "field8": "%d" % seq,
            "status": "%s;r=%d;p=%s;e=0%s" % (state, run, panel, ";s=1" if sim else ""),
        }
        if sun is not None:
            u["field5"] = "%.1f" % sun
        out.append(u)
        seq += 1
    return out, seq
