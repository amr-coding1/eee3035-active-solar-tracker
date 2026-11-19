#!/usr/bin/env python3
"""
sim_device.py - Python stand-in for the AST tracker's data link (tests, demos).

It is NOT the firmware. It implements the same externally visible behaviour
(CONTRACT.md v1.5 sections 3.1, 4, 5, 7.1-7.3, 7.5) so the mock server, the
nightly job and the dashboard can be exercised before the ESP32 exists, and it
fills a channel with realistic demonstration data. EVERY sample it produces is
marked `s=1` (simulated) in the status string.

Modes
  run      same command line as firmware/host/build/ast_host_device (PLAN.md):
           a virtual-time device with a persistent state directory
  history  generate N past days and upload them with created_at (demo data),
           960 per request, >= 16 s between requests
  csv      write a ThingSpeak Data-Import CSV for N days ending at a given time
           (header created_at,field1,...,field8,status; created_at as
           'YYYY-MM-DDTHH:MM:SS+00:00'); needs no key and no network. The file is
           imported through the ThingSpeak web page (Channel > Data Import/Export)
  live     run in real time: one sample per minute, upload every upload_min
           minutes at second 20; Ctrl-C stops cleanly and keeps the state

Examples
  # against the mock
  python3 tools/sim_device.py run --base http://127.0.0.1:18766 --ch1 1001 \
      --write-key WRITEKEY1001 --state-dir /tmp/dev --start 2026-11-20T00:00:00Z \
      --minutes 4320 --scenario cloudy --virtual --log /tmp/dev.jsonl
  # against the real ThingSpeak (keys from the environment, never from files)
  export TS_CH1=...  TS_WRITE_KEY=...
  python3 tools/sim_device.py history --days 3 --scenario mixed
  python3 tools/sim_device.py live --scenario cloudy
  # file for the web-page import (no keys involved)
  python3 tools/sim_device.py csv --days 3 --end 2026-11-13T00:00:00Z --scenario mixed --out ast_import.csv

Protocol (contract 7.3)
  * POST <base>/channels/<ch1>/bulk_update.json, Content-Type application/json,
    {"write_api_key":K,"updates":[...]} with up to 960 unacknowledged records,
    oldest first. Success = HTTP 200/202 and a body containing "success":true.
  * regular uploads at second 20 of every upload_min-th minute; a backlog is
    sent in further batches, never less than 16 s after the previous request.
  * failure (no connection, HTTP >= 400, timeout): retry after 1, 2, 4, 8, 15,
    15 ... minutes; records stay in the buffer; regular slots are ignored while
    backing off. Fault 9 when no upload has succeeded for 60 min with records
    pending.
  * lost response: the batch is "in doubt" and is re-sent with the next
    attempt; with --read-key the device first reads fields/8/last.json and
    skips records the server already has.
  * buffer: 11,520 records (8 days). When full the oldest unsent record is
    overwritten and fault 6 is raised on the next sample (7.2).
  * --rtc-lost: no samples are logged until NTP (= the first moment Wi-Fi is
    up); fault 4 is reported on the first sample logged afterwards (7.1).
  * `--crash-at T`: the process stops dead at virtual time T (exit code 3). If T
    is exactly an upload instant the request is sent and the process dies
    before the acknowledgement is stored (worst case: duplicates on restart).

Simulated plant (contract 7.5 says only "sun model + clouds + tracker"; the
numbers below are this tool's assumptions, chosen to look like the real rig):
  * site Guildford 51.24 N, 0.57 W; --season auto (calendar date) | nov | summer
  * single tracking axis tilted 35 deg towards the south; the fixed panel lies
    in the same plane at angle 0. Angles: east negative, west positive, +-60.
  * clear-sky beam + diffuse model, clouds as smooth pseudo-random noise;
    scenarios clear | cloudy | overcast | night | lamp | mixed (per-day choice).
    `lamp` = indoor halogen lamp swept by hand through +-50 deg every 40 min.
  * panels rated 5.4 W at 1000 W/m2; panel B is 2 % weaker than panel A, so
    the A/B swap test has something to find.
  * tracker re-aims every 20 minutes, half an interval ahead of the sun (state
    MOV in that minute), holds (HLD)
    when the sun sensor sees no direct light, idles level (IDL) in the dark.
  * own use about 105 mW plus servo bursts (about 2.8 W while moving).
  * runs 900-999 (calibration): both panels fixed at 0, state STW.
The data are a pure function of (seed, time, scenario, run, panel), so a
restarted device reproduces the same values for the same minute.
"""

import argparse
import collections
import hashlib
import http.client
import json
import math
import os
import signal
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

CONTRACT = "1.5"
CAPACITY = 11520            # records, 8 days at 1/min (contract 7.2)
MAX_BATCH = 960             # contract 3 / 7.3
BATCH_SPACING_S = 16        # contract 7.3
BACKOFF_MIN = (1, 2, 4, 8, 15)
FAULT9_AFTER_S = 3600
HTTP_TIMEOUT_S = 30
UPLOAD_SECOND = 20
LAT, LON = 51.24, -0.57     # Guildford
AXIS_TILT = 35.0            # deg, tracking axis tilted towards the south
ANGLE_LIMIT = 60.0
RETARGET_S = 1200           # tracker re-aims every 20 min
P_RATED_MW = 5400.0
PANEL_FACTOR = {"A": 1.00, "B": 0.98}
SCENARIOS = ("clear", "cloudy", "overcast", "night", "lamp", "mixed")


# ----------------------------------------------------------------- helpers

def parse_iso(text):
    s = text.strip().replace(" ", "T")
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    dt = datetime.fromisoformat(s)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return int(dt.timestamp())


def iso(ts):
    return datetime.fromtimestamp(int(ts), timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def ts_created_at(ts):
    return datetime.fromtimestamp(int(ts), timezone.utc).strftime("%Y-%m-%d %H:%M:%S +0000")


def _hash01(*parts):
    """Deterministic uniform number in [0, 1) from the given parts."""
    h = hashlib.blake2b(repr(parts).encode(), digest_size=8).digest()
    return int.from_bytes(h, "big") / 2.0 ** 64


def _gauss(*parts):
    u1 = max(1e-12, _hash01("g1", *parts))
    u2 = _hash01("g2", *parts)
    return math.sqrt(-2.0 * math.log(u1)) * math.cos(2 * math.pi * u2)


def _smooth_noise(seed, tag, t, period_s):
    """Value noise in [0, 1): smooth interpolation between hashed knots."""
    x = t / float(period_s)
    i = math.floor(x)
    f = x - i
    f = f * f * (3 - 2 * f)
    return _hash01(seed, tag, i) * (1 - f) + _hash01(seed, tag, i + 1) * f


# ----------------------------------------------------------------- world

class World:
    """Simulated sun, clouds, panels and tracker: sample(t) for the minute [t, t+60)."""

    def __init__(self, seed=1, scenario="clear", season="auto", run=0, panel="A", upload_min=15):
        self.seed = int(seed)
        self.scenario = scenario
        self.season = season
        self.run = int(run)
        self.panel = panel
        self.upload_min = upload_min
        self._targets = {}

    # -- sun geometry
    def _day_of_year(self, t):
        if self.season == "nov":
            return 324      # 20 November
        if self.season == "summer":
            return 172      # 21 June
        return datetime.fromtimestamp(t, timezone.utc).timetuple().tm_yday

    def sun(self, t):
        """(theta_sun about the axis [deg], elevation [deg], s_up, s_e) at unix time t."""
        n = self._day_of_year(t)
        decl = math.radians(23.44) * math.sin(2 * math.pi * (284 + n) / 365.0)
        b = 2 * math.pi * (n - 81) / 364.0
        eot_min = 9.87 * math.sin(2 * b) - 7.53 * math.cos(b) - 1.5 * math.sin(b)
        solar_h = ((t % 86400) / 3600.0) + LON / 15.0 + eot_min / 60.0
        ha = math.radians(15.0 * (solar_h - 12.0))
        lat = math.radians(LAT)
        s_u = math.sin(lat) * math.sin(decl) + math.cos(lat) * math.cos(decl) * math.cos(ha)
        s_e = -math.cos(decl) * math.sin(ha)
        s_n = math.cos(lat) * math.sin(decl) - math.sin(lat) * math.cos(decl) * math.cos(ha)
        beta = math.radians(AXIS_TILT)
        s_up = -s_n * math.sin(beta) + s_u * math.cos(beta)     # along the level-panel normal
        theta = math.degrees(math.atan2(-s_e, s_up))
        return theta, math.degrees(math.asin(max(-1.0, min(1.0, s_u)))), s_up, s_e

    def _scenario_at(self, t):
        if self.scenario != "mixed":
            return self.scenario
        r = _hash01(self.seed, "day", int(t // 86400))
        return "clear" if r < 0.35 else "cloudy" if r < 0.8 else "overcast"

    def conditions(self, t):
        """dict: theta (deg or None when below the horizon), known (sun sensor sees
        the sun), dark, beam (W/m2 on a surface facing the sun), diffuse (W/m2),
        s_up, s_e (unit sun vector components in the panel frame)."""
        sc = self._scenario_at(t)
        if sc == "lamp":
            period = 2400.0
            x = (t % period) / period
            tri = 4 * abs(x - 0.5) - 1          # +1 .. -1 .. +1
            theta = -50.0 * tri
            beam = 300.0 * (1 + 0.02 * _gauss(self.seed, "lamp", int(t // 60)))
            th = math.radians(theta)
            return {"theta": theta, "known": True, "dark": False, "beam": beam, "diffuse": 15.0,
                    "s_up": math.cos(th), "s_e": -math.sin(th)}
        theta, el, s_up, s_e = self.sun(t)
        if sc == "night" or el < 3.0:
            return {"theta": None, "known": False, "dark": True, "beam": 0.0,
                    "diffuse": 0.0 if (sc == "night" or el < -4) else 4.0 * (el + 4) / 7.0,
                    "s_up": s_up, "s_e": s_e}
        sin_el = math.sin(math.radians(el))
        air_mass = 1.0 / (sin_el + 0.50572 * (el + 6.07995) ** -1.6364)
        dni = 1361.0 * 0.7 ** (air_mass ** 0.678)
        dhi = 0.11 * dni * sin_el + 12.0 * math.sqrt(sin_el)
        ghi = dni * sin_el + dhi
        if sc == "clear":
            v = _smooth_noise(self.seed, "c1", t, 1800)
            tau = 1.0 if v < 0.82 else 1.0 - 0.5 * (v - 0.82) / 0.18
        elif sc == "cloudy":
            v = 0.65 * _smooth_noise(self.seed, "c1", t, 1500) + 0.35 * _smooth_noise(self.seed, "c2", t, 240)
            x = min(1.0, max(0.0, (v - 0.35) / 0.3))
            tau = 0.06 + 0.94 * x * x * (3 - 2 * x)
        else:  # overcast
            tau = 0.03 + 0.05 * _smooth_noise(self.seed, "c1", t, 1200)
        beam = dni * tau
        diffuse = dhi * tau + (1 - tau) * 0.30 * ghi
        return {"theta": theta, "known": beam > 80.0, "dark": False, "beam": beam, "diffuse": diffuse,
                "s_up": s_up, "s_e": s_e}

    # -- tracker
    def _calibration(self):
        return 900 <= self.run <= 999

    def target(self, mark):
        """Panel angle commanded at 20-minute mark number `mark` (pure function of time)."""
        if self._calibration():
            return 0.0
        chain = []
        m = mark
        while True:
            if m in self._targets:
                value = self._targets[m]
                break
            c = self.conditions(m * RETARGET_S)
            if c["dark"]:
                value = 0.0
            elif c["known"]:
                # aim where the sun will be half an interval ahead (halves the mean error)
                ahead = self.conditions(m * RETARGET_S + RETARGET_S // 2)["theta"]
                aim = c["theta"] if ahead is None else c["theta"] + (ahead - c["theta"])
                value = max(-ANGLE_LIMIT, min(ANGLE_LIMIT, aim + 0.4 * _gauss(self.seed, "aim", m)))
                value = round(value, 2)
            elif mark - m >= 72:
                value = 0.0
            else:
                chain.append(m)
                m -= 1
                continue
            self._targets[m] = value
            break
        for k in chain:
            self._targets[k] = value
        if len(self._targets) > 5000:
            for k in sorted(self._targets)[:2500]:
                del self._targets[k]
        return value

    def sample(self, t):
        """Values for the logged minute [t, t+60): powers = minute mean (mid-minute
        value), angles and state = end of minute."""
        mark = int(t // RETARGET_S)
        angle = self.target(mark)
        moved = 0.0
        if t % RETARGET_S == 0:
            moved = abs(angle - self.target(mark - 1))
            if moved <= 0.5:
                moved = 0.0
        mid = self.conditions(t + 30)
        end = self.conditions(t + 60)
        phi = math.radians(angle)
        cos_trk = max(0.0, math.cos(phi) * mid["s_up"] - math.sin(phi) * mid["s_e"])
        cos_fix = max(0.0, mid["s_up"])
        tilt_loss = 0.5 * (1 + math.cos(phi))            # tilted panel sees less sky
        g_trk = mid["beam"] * cos_trk + mid["diffuse"] * (0.85 + 0.15 * tilt_loss)
        g_fix = mid["beam"] * cos_fix + mid["diffuse"]
        minute = int(t // 60)
        ambient = (20.0 if self._warm(t) else 8.0) + 3.0 * math.sin(2 * math.pi * ((t % 86400) / 86400.0 - 0.375))
        if self._scenario_at(t) == "lamp":
            ambient = 21.0
        temp = ambient + 0.028 * g_trk + 0.2 * _gauss(self.seed, "T", minute)
        derate = 1.0 - 0.004 * max(0.0, temp - 25.0)
        on_trk, on_fix = (self.panel, "B" if self.panel == "A" else "A")
        p_trk = P_RATED_MW * g_trk / 1000.0 * derate * PANEL_FACTOR[on_trk] * (1 + 0.004 * _gauss(self.seed, "pt", minute))
        p_fix = P_RATED_MW * g_fix / 1000.0 * derate * PANEL_FACTOR[on_fix] * (1 + 0.004 * _gauss(self.seed, "pf", minute))
        p_use = (96.0 if end["dark"] else 106.0) + 5.0 * _gauss(self.seed, "pu", minute)
        if moved:
            p_use += 2800.0 * (moved / 4.0 + 0.5) / 60.0   # servo at 4 deg/s
        if minute % self.upload_min == 0:
            p_use += 28.0                                  # Wi-Fi burst for the upload
        if self._calibration():
            state = "STW"
        elif moved:
            state = "MOV"
        elif end["dark"]:
            state = "IDL"
        elif end["known"]:
            state = "TRK"
        else:
            state = "HLD"
        sun_angle = None
        if end["known"] and end["theta"] is not None:
            sun_angle = end["theta"] + 0.3 * _gauss(self.seed, "sa", minute)
        # field7 V_supply: 5 V feed to the tracker, sagging slightly with the load (as firmware/core/ast_sim.cpp)
        v_supply = 5.05 - 0.00015 * max(0.0, p_use) + 0.002 * _gauss(self.seed, "vb", minute)
        return {
            "t": int(t),
            "p_trk": max(0.0, p_trk), "p_fix": max(0.0, p_fix), "p_use": max(0.0, p_use),
            "ang_panel": angle, "ang_sun": sun_angle, "temp": temp, "v_supply": v_supply, "state": state,
        }

    def _warm(self, t):
        return 100 <= self._day_of_year(t) <= 280


def record_json(rec):
    """One element of the bulk-update "updates" array, in the contract 3.1 formats."""
    parts = ['"created_at":"%s"' % ts_created_at(rec["t"]),
             '"field1":%.1f' % rec["p_trk"], '"field2":%.1f' % rec["p_fix"], '"field3":%.1f' % rec["p_use"],
             '"field4":%.1f' % rec["ang_panel"]]
    if rec.get("ang_sun") is not None:
        parts.append('"field5":%.1f' % rec["ang_sun"])
    if rec.get("temp") is not None:
        parts.append('"field6":%.1f' % rec["temp"])
    parts.append('"field7":%.3f' % rec["v_supply"])
    parts.append('"field8":%d' % rec["seq"])
    parts.append('"status":%s' % json.dumps(rec["status"]))
    return "{" + ",".join(parts) + "}"


def status_string(state, run, panel, fault):
    return "%s;r=%d;p=%s;e=%d;s=1" % (state, run, panel, fault)


# ----------------------------------------------------------------- transport

class Transport:
    """HTTP(S) client. The write key is never printed or logged."""

    def __init__(self, base, channel, write_key, read_key=None, virtual=False, timeout=HTTP_TIMEOUT_S):
        self.base = base.rstrip("/")
        self.channel = channel
        self.write_key = write_key
        self.read_key = read_key
        self.virtual = virtual
        self.timeout = timeout

    def _headers(self, now):
        h = {"User-Agent": "ast-sim-device/" + CONTRACT}
        if self.virtual:
            h["X-Sim-Time"] = str(int(now))
        return h

    def bulk(self, records, now):
        """-> (outcome, http_status, detail); outcome is 'ok', 'fail' or 'in_doubt'."""
        body = '{"write_api_key":%s,"updates":[%s]}' % (json.dumps(self.write_key),
                                                         ",".join(record_json(r) for r in records))
        hdr = self._headers(now)
        hdr["Content-Type"] = "application/json"
        req = urllib.request.Request("%s/channels/%s/bulk_update.json" % (self.base, self.channel),
                                     data=body.encode("utf-8"), headers=hdr, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                text = resp.read().decode("utf-8", "replace")
                ok = resp.status in (200, 202) and '"success":true' in text.replace(" ", "")
                return ("ok" if ok else "fail"), resp.status, ("" if ok else text[:120])
        except urllib.error.HTTPError as exc:
            return "fail", exc.code, exc.read().decode("utf-8", "replace")[:120]
        except urllib.error.URLError as exc:          # could not connect / TLS / DNS: nothing was sent
            return "fail", 0, "connect: %s" % (exc.reason,)
        except (http.client.HTTPException, ConnectionError, TimeoutError, OSError) as exc:
            return "in_doubt", 0, "no response: %s" % (type(exc).__name__,)

    def last_seq(self, now):
        """seq of the entry the server received last (fields/8/last.json), or None."""
        if not self.read_key:
            return None
        url = "%s/channels/%s/fields/8/last.json?api_key=%s" % (self.base, self.channel, self.read_key)
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=self._headers(now)),
                                        timeout=self.timeout) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                return int(float(data["field8"])) if isinstance(data, dict) and data.get("field8") else None
        except Exception:
            return None


# ----------------------------------------------------------------- device

class Crash(Exception):
    pass


class Device:
    def __init__(self, world, transport, state_dir=None, upload_min=15, capacity=CAPACITY, log_path=None,
                 wifi_down=(), rtc_lost=False, run=0, panel="A", quiet=False):
        self.world = world
        self.tx = transport
        self.state_dir = state_dir
        self.upload_s = int(upload_min) * 60
        self.capacity = capacity
        self.wifi_down = list(wifi_down)
        self.run, self.panel = int(run), panel
        self.quiet = quiet
        self.pending = collections.deque()
        self.seq = 0
        self.ack_seq = 0
        self.clock = None
        self.nfail = 0
        self.next_attempt = None
        self.last_request = None
        self.last_ok = None            # time of the last successful upload
        self.pending_since = None      # when records first waited without a success
        self.in_doubt = False
        self.time_valid = not rtc_lost
        self.flag4 = bool(rtc_lost)    # fault 4 to report on the next logged sample
        self.flag6 = False
        self.fault9_enabled = True
        self.stats = collections.Counter()
        self._journal = None
        self._journal_lines = 0
        self._log = open(log_path, "a", encoding="utf-8") if log_path else None
        if state_dir:
            os.makedirs(state_dir, exist_ok=True)
            self._load()

    # -- persistence ("flash")
    def _meta_path(self):
        return os.path.join(self.state_dir, "meta.json")

    def _journal_path(self):
        return os.path.join(self.state_dir, "buffer.jsonl")

    def has_clock(self):
        return self.clock is not None

    def _rtc_path(self):
        return os.path.join(self.state_dir, "rtc.json")

    def save_rtc(self, now):
        """The battery-backed RTC keeps running through a crash (it is not flash)."""
        if self.state_dir:
            with open(self._rtc_path(), "w", encoding="utf-8") as fh:
                json.dump({"clock": int(now)}, fh)

    def _load(self):
        if os.path.exists(self._rtc_path()):
            try:
                with open(self._rtc_path(), "r", encoding="utf-8") as fh:
                    self.clock = json.load(fh)["clock"]
            except (ValueError, KeyError):
                pass
        if os.path.exists(self._meta_path()):
            with open(self._meta_path(), "r", encoding="utf-8") as fh:
                m = json.load(fh)
            self.seq, self.ack_seq = m["seq"], m["ack_seq"]
            self.clock = max(m["clock"], self.clock or 0)
            self.nfail = 0                       # backoff restarts after a reboot
            self.last_ok = m.get("last_ok")
            self.pending_since = m.get("pending_since")
            self.flag6 = m.get("flag6", False)
            self.stats.update(m.get("stats", {}))
        recs = []
        if os.path.exists(self._journal_path()):
            with open(self._journal_path(), "r", encoding="utf-8") as fh:
                for line in fh:
                    try:
                        r = json.loads(line)
                    except ValueError:
                        continue                 # torn last line after a crash
                    if r["seq"] > self.ack_seq:
                        recs.append(r)
        recs = recs[-self.capacity:]
        self.pending = collections.deque(recs)
        if recs:
            self.seq = max(self.seq, recs[-1]["seq"])
        self._compact()

    def _compact(self):
        if not self.state_dir:
            return
        if self._journal:
            self._journal.close()
        tmp = self._journal_path() + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            for r in self.pending:
                fh.write(json.dumps(r, separators=(",", ":")) + "\n")
        os.replace(tmp, self._journal_path())
        self._journal = open(self._journal_path(), "a", encoding="utf-8")
        self._journal_lines = len(self.pending)

    def _persist(self):
        if not self.state_dir:
            return
        m = {"version": 1, "contract": CONTRACT, "seq": self.seq, "ack_seq": self.ack_seq, "clock": self.clock,
             "last_ok": self.last_ok, "pending_since": self.pending_since, "flag6": self.flag6,
             "stats": dict(self.stats)}
        tmp = self._meta_path() + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(m, fh)
        os.replace(tmp, self._meta_path())

    def close(self):
        self._persist()
        if self._journal:
            self._journal.close()
            self._journal = None
        if self._log:
            self._log.close()
            self._log = None

    def event(self, t, ev, **kw):
        if self._log:
            rec = {"t": iso(t), "ev": ev}
            rec.update(kw)
            self._log.write(json.dumps(rec, separators=(",", ":")) + "\n")
            self._log.flush()

    # -- environment
    def wifi_up(self, now):
        return not any(a <= now < b for a, b in self.wifi_down)

    def fault_code(self, now):
        if self.flag4:
            return 4
        if self.flag6:
            return 6
        # 9: records are waiting and nothing has been accepted for over 60 min
        ref = self.last_ok if self.last_ok is not None else self.pending_since
        if self.fault9_enabled and self.pending and ref is not None and now - ref > FAULT9_AFTER_S:
            return 9
        return 0

    # -- sampling
    def try_ntp(self, now):
        if not self.time_valid and self.wifi_up(now):
            self.time_valid = True
            self.event(now, "ntp", note="time valid again")

    def on_minute(self, boundary):
        """Called at a minute boundary: log the sample for [boundary-60, boundary)."""
        was_valid = self.time_valid
        self.try_ntp(boundary)
        if not was_valid:
            # also skips the minute in which the time became valid: its 1 Hz readings
            # were taken without a trustworthy clock
            if not self.stats["rtc_skipped"]:
                self.event(boundary, "fault", e=4, note="RTC lost time; samples are not logged")
            self.stats["rtc_skipped"] += 1
            return None
        t = boundary - 60
        s = self.world.sample(t)
        fault = self.fault_code(boundary)
        state = s["state"]
        self.seq += 1
        rec = {"seq": self.seq, "t": t, "p_trk": round(s["p_trk"], 1), "p_fix": round(s["p_fix"], 1),
               "p_use": round(s["p_use"], 1), "ang_panel": round(s["ang_panel"], 1),
               "ang_sun": None if s["ang_sun"] is None else round(s["ang_sun"], 1),
               "temp": round(s["temp"], 1), "v_supply": round(s["v_supply"], 3),
               "status": status_string(state, self.run, self.panel, fault)}
        if fault:
            self.event(boundary, "fault", e=fault, seq=self.seq)
            self.stats["fault%d_samples" % fault] += 1
        self.flag4 = False
        self.flag6 = False
        if len(self.pending) >= self.capacity:
            lost = self.pending.popleft()
            self.stats["overwritten"] += 1
            self.flag6 = True
            self.event(boundary, "overflow", lost_seq=lost["seq"], lost_t=iso(lost["t"]))
        self.pending.append(rec)
        if self.pending_since is None:
            self.pending_since = boundary
        if self._journal:
            self._journal.write(json.dumps(rec, separators=(",", ":")) + "\n")
            self._journal.flush()
            self._journal_lines += 1
            if self._journal_lines > 3 * self.capacity:
                self._compact()
        self.stats["samples"] += 1
        if self._log:       # same shape as ast_host_device: the entry exactly as it will be uploaded
            self.event(boundary, "sample", seq=self.seq, pending=len(self.pending), entry=json.loads(record_json(rec)))
        return rec

    # -- uploading
    def next_slot(self, after):
        """First regular upload instant strictly after `after` (second 20 of an upload minute)."""
        k = (after - UPLOAD_SECOND) // self.upload_s + 1
        return k * self.upload_s + UPLOAD_SECOND

    def _spaced(self, t):
        return t if self.last_request is None else max(t, self.last_request + BATCH_SPACING_S)

    def attempt(self, now, crash_before_ack=False):
        """One upload attempt at time `now`; sets self.next_attempt."""
        if not self.pending:
            self.next_attempt = self.next_slot(now)
            return "idle"
        if not self.wifi_up(now):
            return self._failed(now, "fail", 0, "wifi down", sent=False)
        if self.in_doubt and self.tx.read_key:
            last = self.tx.last_seq(now)
            if last is not None:
                skipped = 0
                while self.pending and self.pending[0]["seq"] <= last:
                    self.pending.popleft()
                    skipped += 1
                if skipped:
                    self.ack_seq = max(self.ack_seq, last)
                    self.stats["skipped_after_doubt"] += skipped
                    self.event(now, "doubt_resolved", last_seq=last, skipped=skipped)
                    self._persist()
            self.in_doubt = False
            if not self.pending:
                self._ok_bookkeeping(now)
                self.next_attempt = self.next_slot(now)
                return "ok"
        batch = [self.pending[i] for i in range(min(MAX_BATCH, len(self.pending)))]
        self.last_request = now
        self.stats["requests"] += 1
        outcome, status, detail = self.tx.bulk(batch, now)
        if crash_before_ack:
            self.event(now, "crash", note="request sent, died before the acknowledgement was stored",
                       n=len(batch), seq_first=batch[0]["seq"], seq_last=batch[-1]["seq"])
            raise Crash()
        if outcome == "ok":
            for _ in batch:
                self.pending.popleft()
            self.ack_seq = batch[-1]["seq"]
            self.stats["uploaded"] += len(batch)
            self.stats["batches_ok"] += 1
            self.in_doubt = False
            self._ok_bookkeeping(now)
            self.event(now, "upload_ok", n=len(batch), seq_first=batch[0]["seq"], seq_last=batch[-1]["seq"],
                       http=status, pending=len(self.pending))
            self._persist()
            self.next_attempt = self._spaced(now + BATCH_SPACING_S) if self.pending else self.next_slot(now)
            return "ok"
        return self._failed(now, outcome, status, detail, sent=True, n=len(batch),
                            seq_first=batch[0]["seq"], seq_last=batch[-1]["seq"])

    def _ok_bookkeeping(self, now):
        self.nfail = 0
        self.last_ok = now
        self.pending_since = now if self.pending else None

    def _failed(self, now, outcome, status, detail, sent, **kw):
        if outcome == "in_doubt":
            self.in_doubt = True
            self.stats["in_doubt"] += 1
            self.event(now, "in_doubt", detail=detail, **kw)
        else:
            self.stats["failures"] += 1
            self.event(now, "upload_fail", http=status, detail=detail, sent=sent, **kw)
        wait_min = BACKOFF_MIN[min(self.nfail, len(BACKOFF_MIN) - 1)]
        self.nfail += 1
        self.next_attempt = self._spaced(now + wait_min * 60)
        self.event(now, "backoff", minutes=wait_min, failures=self.nfail, next=iso(self.next_attempt))
        return outcome

    # -- main loop
    def run_loop(self, start, end, sleeper=None, crash_at=None, should_stop=lambda: False):
        """Run from `start` (unix s) until `end` (None = for ever). `sleeper(t)` waits
        until real time t (None = virtual time, jump)."""
        now = self.clock if self.clock is not None else start
        self.clock = now
        self.event(now, "boot", contract=CONTRACT, seq=self.seq, pending=len(self.pending),
                   time_valid=self.time_valid)
        self.try_ntp(now)
        next_minute = (now // 60 + 1) * 60
        # first upload: the next regular slot; after a reboot with records waiting, not
        # sooner than 16 s after boot (the spacing to the pre-reboot request is unknown)
        self.next_attempt = self.next_slot(now - 1)
        if self.pending and self.next_attempt < now + BATCH_SPACING_S:
            self.next_attempt = now + BATCH_SPACING_S
        while not should_stop():
            t = min(next_minute, self.next_attempt)
            if crash_at is not None and crash_at <= t:
                self.clock = crash_at
                if crash_at == t == self.next_attempt and t != next_minute:
                    self.attempt(t, crash_before_ack=True)     # raises Crash once the request is out
                self.event(crash_at, "crash")
                raise Crash()
            if end is not None and t > end:
                self.clock = end
                break
            if sleeper:
                sleeper(t)
                if should_stop():
                    break
            self.clock = t
            if t == next_minute:
                self.on_minute(t)
                next_minute += 60
                self._persist()
            else:
                self.attempt(t)
        self._persist()
        return self.clock


# ----------------------------------------------------------------- CLI

def parse_window(text):
    a, _, b = text.partition("/")
    if not b:
        raise argparse.ArgumentTypeError("window must be START/END in ISO-8601 UTC")
    return parse_iso(a), parse_iso(b)


def common_args(p):
    p.add_argument("--base", default=os.environ.get("TS_BASE", "https://api.thingspeak.com"),
                   help="API root; default https://api.thingspeak.com (or $TS_BASE)")
    p.add_argument("--ch1", default=os.environ.get("TS_CH1"), help="channel 1 id (default $TS_CH1)")
    p.add_argument("--write-key", default=os.environ.get("TS_WRITE_KEY"),
                   help="write key (default $TS_WRITE_KEY; never store it in the repository)")
    p.add_argument("--read-key", default=os.environ.get("TS_READ_KEY"), help="optional ch1 read key ($TS_READ_KEY)")
    p.add_argument("--upload-min", type=int, default=15, help="upload interval, 5-60 minutes")
    p.add_argument("--scenario", choices=SCENARIOS, default="clear")
    p.add_argument("--season", choices=("auto", "nov", "summer"), default="auto")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--run", type=int, default=0, help="run id 0-999 (900-999 = calibration)")
    p.add_argument("--panel", choices=("A", "B"), default="A")
    p.add_argument("--state-dir", help='persistent "flash" directory')
    p.add_argument("--log", help="JSON-lines event log")
    p.add_argument("--quiet", action="store_true")


def build(args, virtual, capacity=CAPACITY, wifi_down=(), rtc_lost=False):
    if not args.ch1 or not args.write_key:
        sys.stderr.write("error: channel id and write key are required (--ch1/--write-key or TS_CH1/TS_WRITE_KEY)\n")
        sys.exit(2)
    if not 5 <= args.upload_min <= 60:
        sys.stderr.write("error: --upload-min must be 5-60\n")
        sys.exit(2)
    if not 0 <= args.run <= 999:
        sys.stderr.write("error: --run must be 0-999\n")
        sys.exit(2)
    if args.base.startswith("http://") and "127.0.0.1" not in args.base and "localhost" not in args.base:
        sys.stderr.write("warning: plain HTTP to a non-local host; the contract requires HTTPS (F8)\n")
    world = World(args.seed, args.scenario, args.season, args.run, args.panel, args.upload_min)
    tx = Transport(args.base, args.ch1, args.write_key, args.read_key, virtual=virtual)
    return Device(world, tx, args.state_dir, args.upload_min, capacity, args.log, wifi_down, rtc_lost,
                  args.run, args.panel, args.quiet)


def summary(dev, extra=None):
    out = {"device": "sim_device.py (Python stand-in, all samples s=1)", "contract": CONTRACT, "clock": iso(dev.clock),
           "seq": dev.seq, "ack_seq": dev.ack_seq, "pending": len(dev.pending)}
    out.update({k: dev.stats[k] for k in sorted(dev.stats)})
    out.update(extra or {})
    return out


def real_sleeper(stop_flag):
    def sleep_until(t):
        while not stop_flag["stop"]:
            d = t - time.time()
            if d <= 0:
                return
            time.sleep(min(d, 0.5))
    return sleep_until


def cmd_run(args):
    dev = build(args, args.virtual, wifi_down=args.wifi_down, rtc_lost=args.rtc_lost)
    stop = {"stop": False}
    signal.signal(signal.SIGINT, lambda *_: stop.update(stop=True))
    signal.signal(signal.SIGTERM, lambda *_: stop.update(stop=True))
    if args.virtual:
        start = parse_iso(args.start) if args.start else int(time.time()) // 60 * 60
        if dev.has_clock() and not args.set_time:
            start = dev.clock
        else:
            dev.clock = start
        sleeper = None
    else:
        start = int(time.time())
        dev.clock = start
        sleeper = real_sleeper(stop)
    end = start + args.minutes * 60 if args.minutes is not None else None
    crash_at = parse_iso(args.crash_at) if args.crash_at else None
    try:
        dev.run_loop(start, end, sleeper, crash_at, lambda: stop["stop"])
    except Crash:
        # abrupt stop: nothing is flushed beyond what was already persisted
        dev.save_rtc(dev.clock)
        if not args.quiet:
            print(json.dumps(summary(dev, {"exit": "crash"})), flush=True)
        os._exit(3)
    dev.close()
    if not args.quiet:
        print(json.dumps(summary(dev, {"exit": "ok"})), flush=True)
    return 0


def cmd_history(args):
    end = (parse_iso(args.end) if args.end else int(time.time())) // 60 * 60
    n = int(args.days * 1440)
    dev = build(args, args.virtual, capacity=max(CAPACITY, n))
    if dev.seq == 0 and args.seq_start > 1:
        dev.seq = dev.ack_seq = args.seq_start - 1
    stop = {"stop": False}
    signal.signal(signal.SIGINT, lambda *_: stop.update(stop=True))
    first = end - n * 60
    dev.fault9_enabled = False      # generated history is not an upload outage
    for k in range(n):
        dev.on_minute(first + (k + 1) * 60)
    if not args.quiet:
        print("generated %d samples %s .. %s; uploading in batches of %d, %d s apart" %
              (n, iso(first), iso(end - 60), MAX_BATCH, BATCH_SPACING_S), file=sys.stderr, flush=True)
    now = end + UPLOAD_SECOND if args.virtual else time.time()
    sleeper = None if args.virtual else real_sleeper(stop)
    attempts = 0
    while dev.pending and not stop["stop"]:
        dev.clock = int(now)
        outcome = dev.attempt(now)
        attempts += 1
        if not args.quiet:
            print("  request %d: %s, %d pending" % (attempts, outcome, len(dev.pending)), file=sys.stderr, flush=True)
        if outcome != "ok" and dev.nfail > args.max_failures:
            break
        if dev.pending:
            now = dev.next_attempt
            if sleeper:
                sleeper(now)
    left = len(dev.pending)
    dev.close()
    if not args.quiet:
        print(json.dumps(summary(dev, {"exit": "ok" if not left else "incomplete", "first": iso(first),
                                       "last": iso(end - 60)})), flush=True)
    return 0 if not left else 1


def csv_row(rec):
    """One Data-Import row in the contract 3.1 formats; unknown values are empty cells."""
    when = datetime.fromtimestamp(rec["t"], timezone.utc).strftime("%Y-%m-%dT%H:%M:%S+00:00")
    cells = [when, "%.1f" % rec["p_trk"], "%.1f" % rec["p_fix"], "%.1f" % rec["p_use"], "%.1f" % rec["ang_panel"],
             "" if rec.get("ang_sun") is None else "%.1f" % rec["ang_sun"],
             "" if rec.get("temp") is None else "%.1f" % rec["temp"],
             "%.3f" % rec["v_supply"], "%d" % rec["seq"], rec["status"]]
    return ",".join(cells)       # no cell contains a comma or a quote (status uses ';' and '=')


def cmd_csv(args):
    end = (parse_iso(args.end) if args.end else int(time.time())) // 60 * 60
    n = int(args.days * 1440)
    if n <= 0:
        sys.stderr.write("error: --days must be positive\n")
        return 2
    if not 0 <= args.run <= 999:
        sys.stderr.write("error: --run must be 0-999\n")
        return 2
    world = World(args.seed, args.scenario, args.season, args.run, args.panel, args.upload_min)
    dev = Device(world, None, None, args.upload_min, max(CAPACITY, n), None, (), False, args.run, args.panel)
    dev.fault9_enabled = False
    dev.seq = args.seq_start - 1
    first = end - n * 60
    out = sys.stdout if args.out in (None, "-") else open(args.out, "w", encoding="utf-8", newline="")
    try:
        out.write("created_at,field1,field2,field3,field4,field5,field6,field7,field8,status\n")
        for k in range(n):
            out.write(csv_row(dev.on_minute(first + (k + 1) * 60)) + "\n")
    finally:
        if out is not sys.stdout:
            out.close()
    sys.stderr.write("wrote %d rows %s .. %s (seq %d..%d, every row s=1)%s\n" % (
        n, iso(first), iso(end - 60), args.seq_start, dev.seq, "" if args.out in (None, "-") else " to " + args.out))
    if n > 8000:
        sys.stderr.write("note: %d rows; check the row/size limit of the ThingSpeak import page and split by day if needed\n" % n)
    return 0


def cmd_live(args):
    args.virtual = False
    args.start = None
    args.set_time = True
    args.minutes = None
    args.crash_at = None
    args.wifi_down = []
    args.rtc_lost = False
    if not args.state_dir:
        import tempfile
        args.state_dir = os.path.join(tempfile.gettempdir(), "ast_sim_device_%s" % args.ch1)
    if not args.quiet:
        print("live: one simulated sample per minute, upload every %d min; state in %s; Ctrl-C to stop" %
              (args.upload_min, args.state_dir), file=sys.stderr, flush=True)
    return cmd_run(args)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0].startswith("--") and argv[0] not in ("-h", "--help"):
        argv.insert(0, "run")       # same command line as ast_host_device
    ap = argparse.ArgumentParser(description="AST device simulator (Python stand-in; every sample is s=1)")
    sub = ap.add_subparsers(dest="mode", required=True)

    r = sub.add_parser("run", help="ast_host_device-compatible virtual device")
    common_args(r)
    r.add_argument("--start", help="virtual UTC start, e.g. 2026-11-20T00:00:00Z (ignored when the state dir holds a clock)")
    r.add_argument("--set-time", action="store_true", help="use --start even when the state dir holds a clock")
    r.add_argument("--minutes", type=int, help="run N minutes then exit 0")
    r.add_argument("--wifi-down", action="append", type=parse_window, default=[], metavar="START/END")
    r.add_argument("--crash-at", metavar="ISO")
    r.add_argument("--rtc-lost", action="store_true")
    r.add_argument("--virtual", action="store_true", help="no sleeping; send X-Sim-Time with every request")
    r.set_defaults(func=cmd_run)

    h = sub.add_parser("history", help="generate N past days and upload them")
    common_args(h)
    h.add_argument("--days", type=float, required=True)
    h.add_argument("--end", help="end of the generated period (default: now)")
    h.add_argument("--seq-start", type=int, default=1, help="first sequence number when the state dir is new")
    h.add_argument("--virtual", action="store_true", help="do not wait 16 s between requests (mock only)")
    h.add_argument("--max-failures", type=int, default=6)
    h.set_defaults(func=cmd_history)

    c = sub.add_parser("csv", help="write a ThingSpeak Data-Import CSV (no keys, no network)")
    c.add_argument("--days", type=float, required=True)
    c.add_argument("--end", help="end of the period, ISO-8601 UTC (default: now); the last row is the minute before it")
    c.add_argument("--out", help="output file (default: stdout)")
    c.add_argument("--seq-start", type=int, default=1, help="sequence number of the first row")
    c.add_argument("--upload-min", type=int, default=15)
    c.add_argument("--scenario", choices=SCENARIOS, default="clear")
    c.add_argument("--season", choices=("auto", "nov", "summer"), default="auto")
    c.add_argument("--seed", type=int, default=1)
    c.add_argument("--run", type=int, default=0)
    c.add_argument("--panel", choices=("A", "B"), default="A")
    c.set_defaults(func=cmd_csv)

    lv = sub.add_parser("live", help="real-time device")
    common_args(lv)
    lv.set_defaults(func=cmd_live)

    try:
        args = ap.parse_args(argv)
    except SystemExit as exc:
        return 2 if exc.code not in (0, None) else 0
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
