#!/usr/bin/env python3
"""
mock_thingspeak.py - local stand-in for the ThingSpeak REST API (tests only).

Implements CONTRACT.md v1.5 section 10: the subset of ThingSpeak used by the
AST data link (sections 3, 7.3 and 8), on plain HTTP, Python stdlib only.

    python3 tools/mock_thingspeak.py --port 18766 \
        --channel 1001:WRITEKEY1001:READKEY1001 \
        --channel 1002:WRITEKEY1002:READKEY1002 [--state FILE]

    --port 0                         pick a free port; the first stdout line is
                                     "LISTENING http://127.0.0.1:<port>"
    --channel ID:WRITEKEY:READKEY    repeatable. Empty READKEY = public channel.
    --state FILE                     load at start, save after every stored write
    --existing-timestamp-policy store|reject
                                     store (default): an update whose created_at
                                     already exists in the channel is stored
                                     again, so re-sent batches exercise the
                                     de-duplication by seq (contract 6.1).
                                     reject: what the MathWorks CSV-import
                                     documentation describes, assumed to hold for
                                     bulk_update.json too: a request that
                                     contains the same created_at twice is
                                     refused as a whole (HTTP 400); updates whose
                                     created_at is already in the channel are
                                     dropped one by one, the rest are stored and
                                     the answer is still 202.
    --reject-duplicate-timestamps    same as --existing-timestamp-policy reject
    --verbose                        one line per request on stderr

Write API (device, MATLAB)
  POST /channels/<id>/bulk_update.json
       Content-Type must be application/json, otherwise 401 (as the real
       service answered on 13 Nov 2026). Body:
       {"write_api_key":"K","updates":[{"created_at":..., "field1":..,
        ... "field8":.., "status":"..."}]}
       created_at: 'YYYY-MM-DD HH:MM:SS +0000' (any +hhmm / +hh:mm offset),
       'YYYY-MM-DDTHH:MM:SSZ', no offset (= UTC), or unix seconds; or delta_t
       (seconds since the previous update of the array; the first update is
       placed so that the last one lands on "now").
       202 {"success":true}             stored, entry_id assigned 1,2,3...
       401 {"status":"401","error":..}  wrong write key / wrong content type
       400 {"status":"400","error":..}  bad JSON, no updates, > 960 updates,
                                        unreadable created_at
       404 {"status":"404","error":"Not Found"}   unknown channel
       429 {"status":"429","error":..}  less than 15 s since the previous
                                        accepted write to this channel
  POST /update.json | /update   and   GET /update?api_key=K&field1=..
       single write; answers the entry JSON (.json) or the entry_id as text;
       "0" with HTTP 400 on a bad key, "0" with HTTP 429 inside the 15 s rule.

  "now" for the 15 s rule and for missing created_at is, in this order: the
  request header X-Sim-Time (unix seconds), the admin clock, the wall clock.

Read API (dashboard, MATLAB)
  GET /channels/<id>/feeds.json?api_key=READ&start=YYYY-MM-DD%20HH:MM:SS
        &end=...&timezone=Etc/UTC&status=true&results=8000
       start/end are inclusive and read in `timezone` (default UTC; Etc/UTC,
       UTC and any IANA zone known to this Python are accepted). `results`
       defaults to 100 and is capped at 8000. The NEWEST `results` entries in
       the window are returned, oldest first (ordered by created_at, then
       entry_id). `days=N` and `minutes=N` (relative to "now") are accepted.
       Values come back as strings, missing values as null, created_at as
       'YYYY-MM-DDTHH:MM:SSZ'. `status=true` adds "status" to every feed.
       Wrong or missing read key on a private channel: body -1 (sent with
       HTTP 400 text/plain, as api.thingspeak.com answered from this machine
       on 13 Nov 2026; clients must test the BODY, not the status code).
  GET /channels/<id>/feeds/last.json        newest entry (highest entry_id)
  GET /channels/<id>/fields/<n>/last.json   newest entry with field n not null
  GET /channels/<id>/fields/<n>/last.txt    its value as plain text
  GET /channels/<id>/fields/<n>.json        like feeds.json with one field

Admin (JSON; never on the real service)
  POST /__admin/reset     {}  or {"channels":[1001], "keep_log":true}
                          deletes entries, entry ids, spacing memory, faults,
                          the admin clock and (unless keep_log) the log
  POST /__admin/faults    {"mode":"http500","count":N[,"status":503]}
                              next N write requests fail with HTTP 500, nothing stored
                          {"mode":"drop_response","count":N}
                              next N write requests are processed and STORED, then
                              the socket is closed without any reply (lost response)
                          {"mode":"delay","ms":M[,"count":N]}
                              every API request (or the next N) waits M ms first
                          {"mode":"reject_all"[,"count":N][,"status":429]}
                              every write (or the next N) is refused, nothing stored
                          {"mode":"clear"}   remove all faults
                          optional "channel": ID limits a fault to one channel.
  GET  /__admin/faults    active faults
  POST /__admin/clock     {"now": unix_seconds}  virtual clock (advances in real
                          time from that value; {"now":..,"freeze":true} holds it;
                          {"now":null} returns to the wall clock)
  POST /__admin/config    {"existing_timestamp_policy":"store"|"reject"}  switch the
                          policy at run time (GET shows it; reset does not change it)
  GET  /__admin/dump/<id> everything stored for a channel, in entry_id order
                          (?since_entry=N for entries with entry_id > N)
  GET  /__admin/log       one record per API request (?since=N to skip N records)
  GET  /__admin/health    {"ok":true,...}

CORS: Access-Control-Allow-Origin: * on every response, OPTIONS preflight
answered with 200.

Known differences from the real service (deliberate, documented):
  * every channel has all eight fields defined; * `results` defaults to 100
  even when start/end are given (clients must pass results explicitly);
  * no message-per-year quota; * HTTP only.
"""

import argparse
import json
import os
import re
import socket
import sys
import threading
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

try:
    from zoneinfo import ZoneInfo
except Exception:  # pragma: no cover
    ZoneInfo = None

MAX_BULK = 960          # contract section 3: entries per bulk update
MAX_RESULTS = 8000      # contract section 3 / 8: entries per read
DEFAULT_RESULTS = 100
MIN_SPACING_S = 15      # contract section 3: one write per channel every 15 s
LOG_CAP = 200000

ERRORS = {
    400: ("error_bad_request", "Bad Request", "The request cannot be fulfilled due to bad syntax."),
    401: ("error_auth_required", "Authorization Required", "Please provide proper authentication details."),
    413: ("error_request_too_large", "Request Too Large", "Your request is too large. Please reduce the size."),
    429: ("error_too_many_requests", "Too Many Requests", "Please wait before making another request."),
    500: ("error_internal_server", "Internal Server Error", "An unexpected error occurred."),
    503: ("error_service_unavailable", "Service Unavailable", "The server is currently unavailable."),
}


def error_body(status, details=None):
    code, message, default = ERRORS.get(status, ("error", "Error", ""))
    return {"status": str(status), "error": {"error_code": code, "message": message, "details": details or default}}


# ------------------------------------------------------------------ time

_TS_RE = re.compile(
    r"^\s*(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2})(?::(\d{2})(?:\.\d+)?)?\s*"
    r"(Z|UTC|[+-]\d{2}:?\d{2}|[+-]\d{2})?\s*$",
    re.IGNORECASE,
)


def parse_time(text, default_tz=timezone.utc):
    """Parse a ThingSpeak timestamp to unix seconds (int). Raises ValueError."""
    if isinstance(text, (int, float)) and not isinstance(text, bool):
        return int(text)
    s = str(text)
    if re.fullmatch(r"\s*\d{9,11}\s*", s):
        return int(s)
    m = _TS_RE.match(s)
    if not m:
        raise ValueError("unreadable time %r" % (text,))
    y, mo, d, h, mi = (int(m.group(i)) for i in range(1, 6))
    sec = int(m.group(6) or 0)
    off = m.group(7)
    if off is None:
        tz = default_tz
    elif off.upper() in ("Z", "UTC"):
        tz = timezone.utc
    else:
        sign = -1 if off[0] == "-" else 1
        digits = off[1:].replace(":", "")
        hh = int(digits[:2])
        mm = int(digits[2:4] or 0)
        if hh > 14 or mm > 59:
            raise ValueError("bad offset in %r" % (text,))
        tz = timezone(sign * timedelta(hours=hh, minutes=mm))
    dt = datetime(y, mo, d, h, mi, sec, tzinfo=tz)  # ValueError on impossible dates
    return int(dt.timestamp())


def iso_z(ts, tz=None):
    """unix seconds -> 'YYYY-MM-DDTHH:MM:SSZ' (or with offset for a non-UTC zone)."""
    if tz is None or tz is timezone.utc:
        return datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    dt = datetime.fromtimestamp(ts, tz)
    off = dt.utcoffset() or timedelta(0)
    if off == timedelta(0):
        return dt.strftime("%Y-%m-%dT%H:%M:%SZ")
    s = dt.strftime("%z")
    return dt.strftime("%Y-%m-%dT%H:%M:%S") + s[:3] + ":" + s[3:]


def resolve_tz(name):
    if name is None or name == "" or name.upper() in ("UTC", "ETC/UTC", "Z", "GMT", "ETC/GMT"):
        return timezone.utc
    if ZoneInfo is not None:
        try:
            return ZoneInfo(name)
        except Exception:
            pass
    raise ValueError("unknown timezone %r" % (name,))


# ------------------------------------------------------------------ store

def field_text(value):
    """What ThingSpeak stores for a field: text, or None when absent."""
    if value is None:
        return None
    if isinstance(value, bool):
        return "true" if value else "false"
    s = str(value)
    return s if s != "" else None


class Channel:
    def __init__(self, cid, write_key, read_key, name=None):
        self.id = int(cid)
        self.write_key = write_key
        self.read_key = read_key
        self.name = name or ("Channel %d" % self.id)
        self.created = int(time.time())
        self.clear()

    def clear(self):
        self.entries = []          # dicts in entry_id order
        self.last_entry_id = 0
        self.last_write = None     # "now" of the last accepted write
        self.updated = self.created
        self.ts_seen = set()

    def add(self, created_at, fields, status, received):
        self.last_entry_id += 1
        e = {"entry_id": self.last_entry_id, "created_at": int(created_at), "fields": fields,
             "status": status, "received": received}
        self.entries.append(e)
        self.ts_seen.add(int(created_at))
        return e

    def meta(self, only_field=None):
        m = {"id": self.id, "name": self.name, "latitude": "0.0", "longitude": "0.0"}
        for i in range(1, 9):
            if only_field is None or only_field == i:
                m["field%d" % i] = "Field %d" % i
        m["created_at"] = iso_z(self.created)
        m["updated_at"] = iso_z(self.updated)
        m["last_entry_id"] = self.last_entry_id if self.last_entry_id else None
        return m

    def to_state(self):
        return {"id": self.id, "last_entry_id": self.last_entry_id, "last_write": self.last_write,
                "entries": [[e["entry_id"], e["created_at"], e["fields"], e["status"], e["received"]]
                            for e in self.entries]}

    def from_state(self, st):
        self.clear()
        for eid, ts, fields, status, received in st.get("entries", []):
            self.entries.append({"entry_id": eid, "created_at": ts, "fields": fields, "status": status,
                                 "received": received})
            self.ts_seen.add(ts)
        self.last_entry_id = st.get("last_entry_id", len(self.entries))
        self.last_write = st.get("last_write")


def feed_json(e, tz=None, with_status=False, only_field=None):
    out = {"created_at": iso_z(e["created_at"], tz), "entry_id": e["entry_id"]}
    for i in range(1, 9):
        if only_field is None or only_field == i:
            out["field%d" % i] = e["fields"][i - 1]
    if with_status:
        out["status"] = e["status"]
    return out


class Store:
    def __init__(self, reject_dup_ts=False, state_file=None):
        self.lock = threading.RLock()
        self.channels = {}
        self.reject_dup_ts = reject_dup_ts
        self.state_file = state_file
        self.faults = []           # dicts: mode, count (None = until cleared), ms, status, channel
        self.log = []
        self.log_dropped = 0
        self.clock = None          # (virtual_now, wall_at_set, frozen)
        self.started = time.time()

    # -- clock
    def now(self, sim_header=None):
        if sim_header is not None:
            return sim_header
        with self.lock:
            if self.clock is not None:
                base, wall0, frozen = self.clock
                return base if frozen else base + (time.time() - wall0)
        return time.time()

    # -- faults
    def take_fault(self, modes, channel_id):
        """Return (and consume one count of) the first active fault among `modes`."""
        with self.lock:
            for f in self.faults:
                if f["mode"] not in modes:
                    continue
                if f.get("channel") is not None and channel_id is not None and int(f["channel"]) != int(channel_id):
                    continue
                if f["count"] is not None:
                    f["count"] -= 1
                    if f["count"] <= 0:
                        self.faults.remove(f)
                return dict(f)
        return None

    def add_log(self, rec):
        with self.lock:
            rec["n"] = len(self.log) + self.log_dropped + 1
            self.log.append(rec)
            if len(self.log) > LOG_CAP:
                del self.log[: LOG_CAP // 10]
                self.log_dropped += LOG_CAP // 10

    # -- persistence
    def load(self):
        if not self.state_file or not os.path.exists(self.state_file):
            return
        with open(self.state_file, "r", encoding="utf-8") as fh:
            st = json.load(fh)
        for cst in st.get("channels", []):
            ch = self.channels.get(int(cst["id"]))
            if ch is not None:
                ch.from_state(cst)

    def save(self):
        if not self.state_file:
            return
        with self.lock:
            st = {"version": 1, "channels": [c.to_state() for c in self.channels.values()]}
            tmp = self.state_file + ".tmp"
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump(st, fh, separators=(",", ":"))
            os.replace(tmp, self.state_file)


# ------------------------------------------------------------------ HTTP

class _Dropped(Exception):
    """Raised to close the socket without sending a reply."""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "mock-thingspeak/1.5"
    store = None      # set by make_server
    verbose = False

    # -- plumbing
    def log_message(self, fmt, *args):
        if self.verbose:
            sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def _send(self, status, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, separators=(",", ":"))
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS, DELETE, PATCH")
        self.send_header("Access-Control-Allow-Headers", "origin, content-type, X-Requested-With")
        self.send_header("Access-Control-Max-Age", "1800")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)
        self._status = status

    def _err(self, status, details=None):
        self._send(status, error_body(status, details))

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n > 0 else b""

    def _sim_time(self):
        v = self.headers.get("X-Sim-Time")
        if v is None:
            return None
        try:
            return float(v)
        except ValueError:
            return None

    def _drop(self):
        self._status = 0
        self.close_connection = True
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    # -- verbs
    def do_OPTIONS(self):
        self._body()
        self._send(200, "", "text/plain; charset=utf-8")

    def do_GET(self):
        self._dispatch("GET")

    def do_HEAD(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def do_PUT(self):
        self._dispatch("POST")

    def _dispatch(self, method):
        url = urlsplit(self.path)
        path = url.path.rstrip("/") or "/"
        query = {k: v[-1] for k, v in parse_qs(url.query, keep_blank_values=True).items()}
        raw = self._body() if self.command in ("POST", "PUT") else b""
        self._status = None
        if path.startswith("/__admin"):
            try:
                self._admin(method, path, query, raw)
            except Exception as exc:  # admin misuse must not kill the thread silently
                self._send(400, {"ok": False, "error": str(exc)})
            return

        st = self.store
        sim = self._sim_time()
        rec = {"wall": round(time.time(), 3), "sim": sim, "method": self.command, "path": url.path,
               "query": url.query, "channel": None, "kind": "other", "status": None, "fault": None}
        m = re.match(r"^/channels/(\d+)(/.*)?$", path)
        cid = int(m.group(1)) if m else None
        rec["channel"] = cid
        try:
            f = st.take_fault(("delay",), cid)
            if f:
                rec["fault"] = "delay"
                time.sleep(max(0, float(f.get("ms") or 0)) / 1000.0)
            if m:
                sub = m.group(2) or ""
                ch = st.channels.get(cid)
                if sub == "/bulk_update.json" and method == "POST":
                    rec["kind"] = "bulk"
                    self._bulk(ch, raw, sim, rec)
                elif ch is None:
                    self._send(404, {"status": "404", "error": "Not Found"})
                elif method == "GET" and sub in ("/feeds.json", "/feed.json", ".json"):
                    rec["kind"] = "feeds"
                    self._feeds(ch, query, rec)
                elif method == "GET" and sub in ("/feeds/last.json", "/feeds/last", "/feed/last.json"):
                    rec["kind"] = "last"
                    self._last(ch, query, None, "json")
                else:
                    fm = re.match(r"^/fields?/([1-8])(?:/(last))?(?:\.(json|txt))?$", sub)
                    if fm and method == "GET":
                        n, last, ext = int(fm.group(1)), fm.group(2), fm.group(3) or "json"
                        if last:
                            rec["kind"] = "field_last"
                            self._last(ch, query, n, ext)
                        else:
                            rec["kind"] = "feeds"
                            self._feeds(ch, query, rec, only_field=n)
                    else:
                        self._send(404, {"status": "404", "error": "Not Found"})
            elif path in ("/update", "/update.json"):
                rec["kind"] = "update"
                self._update(method, path, query, raw, sim, rec)
            else:
                self._send(404, {"status": "404", "error": "Not Found"})
        except _Dropped:
            self._drop()
        except (BrokenPipeError, ConnectionResetError):
            self._status = self._status or 0
            self.close_connection = True
        rec["status"] = self._status
        st.add_log(rec)

    # -- write API
    def _write_faults(self, cid, rec):
        """Returns ('fail', status) / ('drop', None) / (None, None)."""
        st = self.store
        f = st.take_fault(("http500", "reject_all"), cid)
        if f:
            rec["fault"] = f["mode"]
            return "fail", int(f.get("status") or (500 if f["mode"] == "http500" else 429))
        f = st.take_fault(("drop_response",), cid)
        if f:
            rec["fault"] = "drop_response"
            return "drop", None
        return None, None

    def _spacing_ok(self, ch, now):
        return ch.last_write is None or not (0 <= now - ch.last_write < MIN_SPACING_S)

    def _bulk(self, ch, raw, sim, rec):
        st = self.store
        now = st.now(sim)
        rec["now"] = now
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        if ctype != "application/json":
            return self._err(401)
        try:
            # numbers keep the exact text the client wrote ("12.50" stays "12.50")
            body = json.loads(raw.decode("utf-8"), parse_float=str, parse_int=str)
        except Exception:
            return self._err(400, "The request body is not valid JSON.")
        if not isinstance(body, dict):
            return self._err(400, "The request body must be a JSON object.")
        if ch is None:
            return self._send(404, {"status": "404", "error": "Not Found"})
        if body.get("write_api_key") != ch.write_key:
            return self._err(401)
        updates = body.get("updates")
        if not isinstance(updates, list) or not updates:
            return self._err(400, "updates must be a non-empty array.")
        rec["updates"] = len(updates)
        if len(updates) > MAX_BULK:
            return self._err(400, "Too many updates: at most %d per bulk update." % MAX_BULK)
        action, fstatus = self._write_faults(ch.id, rec)
        if action == "fail":
            return self._err(fstatus)
        with st.lock:
            if not self._spacing_ok(ch, now):
                rec["spacing_s"] = round(now - ch.last_write, 3)
                return self._err(429)
            # timestamps
            try:
                times = self._bulk_times(updates, now)
            except ValueError as exc:
                return self._err(400, str(exc))
            if st.reject_dup_ts and len(set(times)) != len(times):
                rec["dup_in_request"] = len(times) - len(set(times))
                return self._err(400, "The request contains the same created_at more than once.")
            rec["spacing_s"] = None if ch.last_write is None else round(now - ch.last_write, 3)
            stored = dup = 0
            seqs = []
            for u, ts in zip(updates, times):
                if st.reject_dup_ts and ts in ch.ts_seen:
                    dup += 1
                    continue
                if ts in ch.ts_seen:
                    dup += 1
                fields = [field_text(u.get("field%d" % i)) for i in range(1, 9)]
                status = u.get("status")
                ch.add(ts, fields, None if status is None else str(status), round(now, 3))
                stored += 1
                if fields[7] is not None:
                    try:
                        seqs.append(int(float(fields[7])))
                    except ValueError:
                        pass
            ch.last_write = now
            ch.updated = int(now)
            rec.update(stored=stored, dup_ts=dup, first_created_at=iso_z(min(times)), last_created_at=iso_z(max(times)),
                       last_entry_id=ch.last_entry_id)
            if seqs:
                rec.update(seq_min=min(seqs), seq_max=max(seqs))
            st.save()
        if action == "drop":
            raise _Dropped()
        self._send(202, {"success": True})

    @staticmethod
    def _bulk_times(updates, now):
        times = []
        if all(isinstance(u, dict) and "created_at" not in u and "delta_t" in u for u in updates):
            deltas = [int(float(u["delta_t"])) for u in updates]
            t = int(now) - sum(deltas[1:])
            for i, d in enumerate(deltas):
                t = t if i == 0 else t + d
                times.append(t)
            return times
        for i, u in enumerate(updates):
            if not isinstance(u, dict):
                raise ValueError("update %d is not an object." % i)
            if "created_at" in u:
                try:
                    times.append(parse_time(u["created_at"]))
                except ValueError:
                    raise ValueError("update %d: created_at %r is not a valid time." % (i, u["created_at"]))
            elif "delta_t" in u:
                times.append((times[-1] if times else int(now)) + int(float(u["delta_t"])))
            else:
                raise ValueError("update %d has neither created_at nor delta_t." % i)
        return times

    def _update(self, method, path, query, raw, sim, rec):
        st = self.store
        params = dict(query)
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        if raw:
            try:
                if ctype == "application/json":
                    params.update(json.loads(raw.decode("utf-8"), parse_float=str, parse_int=str))
                else:
                    params.update({k: v[-1] for k, v in parse_qs(raw.decode("utf-8"), keep_blank_values=True).items()})
            except Exception:
                return self._send(400, "0", "text/plain; charset=utf-8")
        key = params.get("api_key") or self.headers.get("THINGSPEAKAPIKEY") or self.headers.get("X-THINGSPEAKAPIKEY")
        ch = next((c for c in st.channels.values() if c.write_key == key), None)
        if ch is None:
            return self._send(400, "0", "text/plain; charset=utf-8")
        rec["channel"] = ch.id
        rec["updates"] = 1
        action, fstatus = self._write_faults(ch.id, rec)
        if action == "fail":
            return self._send(fstatus, "0", "text/plain; charset=utf-8")
        now = st.now(sim)
        rec["now"] = now
        with st.lock:
            if not self._spacing_ok(ch, now):
                return self._send(429, "0", "text/plain; charset=utf-8")
            try:
                ts = parse_time(params["created_at"]) if params.get("created_at") else int(now)
            except ValueError:
                return self._send(400, "0", "text/plain; charset=utf-8")
            if st.reject_dup_ts and ts in ch.ts_seen:
                return self._send(200, "0", "text/plain; charset=utf-8")
            fields = [field_text(params.get("field%d" % i)) for i in range(1, 9)]
            status = params.get("status")
            e = ch.add(ts, fields, None if status is None else str(status), round(now, 3))
            ch.last_write = now
            ch.updated = int(now)
            rec.update(stored=1, last_entry_id=ch.last_entry_id)
            st.save()
        if action == "drop":
            raise _Dropped()
        if path.endswith(".json"):
            out = feed_json(e, None, True)
            out["channel_id"] = ch.id
            self._send(200, out)
        else:
            self._send(200, str(e["entry_id"]), "text/plain; charset=utf-8")

    # -- read API
    def _read_allowed(self, ch, query):
        if not ch.read_key:
            return True
        key = query.get("api_key") or self.headers.get("THINGSPEAKAPIKEY")
        return key == ch.read_key

    def _feeds(self, ch, query, rec, only_field=None):
        st = self.store
        if not self._read_allowed(ch, query):
            return self._send(400, "-1", "text/plain; charset=utf-8")
        try:
            tz = resolve_tz(query.get("timezone"))
            start = parse_time(query["start"], tz) if query.get("start") else None
            end = parse_time(query["end"], tz) if query.get("end") else None
            now = st.now(self._sim_time())
            if query.get("days") and start is None:
                start = int(now) - int(float(query["days"]) * 86400)
            if query.get("minutes") and start is None:
                start = int(now) - int(float(query["minutes"]) * 60)
            results = int(query["results"]) if query.get("results") not in (None, "") else DEFAULT_RESULTS
        except ValueError as exc:
            return self._err(400, str(exc))
        results = max(0, min(MAX_RESULTS, results))
        with_status = str(query.get("status", "")).lower() == "true"
        with st.lock:
            sel = [e for e in ch.entries
                   if (start is None or e["created_at"] >= start) and (end is None or e["created_at"] <= end)
                   and (only_field is None or e["fields"][only_field - 1] is not None)]
            in_window = len(sel)
            sel.sort(key=lambda e: (e["created_at"], e["entry_id"]))
            sel = sel[-results:] if results > 0 else []
            feeds = [feed_json(e, tz, with_status, only_field) for e in sel]
            meta = ch.meta(only_field)
        rec.update(returned=len(feeds), in_window=in_window, results=results)
        self._send(200, {"channel": meta, "feeds": feeds})

    def _last(self, ch, query, field, ext):
        st = self.store
        if not self._read_allowed(ch, query):
            return self._send(400, "-1", "text/plain; charset=utf-8")
        try:
            tz = resolve_tz(query.get("timezone"))
        except ValueError as exc:
            return self._err(400, str(exc))
        with_status = str(query.get("status", "")).lower() == "true"
        with st.lock:
            e = None
            for cand in reversed(ch.entries):
                if field is None or cand["fields"][field - 1] is not None:
                    e = cand
                    break
        if e is None:
            if ext == "txt":
                return self._send(200, "-1", "text/plain; charset=utf-8")
            return self._send(200, "-1", "application/json; charset=utf-8")
        if ext == "txt":
            return self._send(200, e["fields"][field - 1] if field else str(e["entry_id"]), "text/plain; charset=utf-8")
        self._send(200, feed_json(e, tz, with_status, field))

    # -- admin
    def _admin(self, method, path, query, raw):
        st = self.store
        body = json.loads(raw.decode("utf-8")) if raw.strip() else {}
        if path == "/__admin/health":
            with st.lock:
                return self._send(200, {"ok": True, "channels": {str(c.id): len(c.entries) for c in st.channels.values()},
                                        "faults": st.faults, "log": len(st.log) + st.log_dropped,
                                        "reject_duplicate_timestamps": st.reject_dup_ts})
        if path == "/__admin/config":
            if method == "POST" and "existing_timestamp_policy" in body:
                pol = body["existing_timestamp_policy"]
                if pol not in ("store", "reject"):
                    return self._send(400, {"ok": False, "error": "policy must be store or reject"})
                st.reject_dup_ts = pol == "reject"
            return self._send(200, {"ok": True, "existing_timestamp_policy": "reject" if st.reject_dup_ts else "store"})
        if path == "/__admin/reset" and method == "POST":
            with st.lock:
                ids = body.get("channels")
                for c in st.channels.values():
                    if ids is None or c.id in [int(i) for i in ids]:
                        c.clear()
                st.faults = []
                st.clock = None
                if not body.get("keep_log"):
                    st.log = []
                    st.log_dropped = 0
                st.save()
            return self._send(200, {"ok": True})
        if path == "/__admin/faults":
            if method == "POST":
                mode = body.get("mode")
                with st.lock:
                    if mode in ("clear", "none", None):
                        st.faults = []
                    elif mode in ("http500", "drop_response", "delay", "reject_all"):
                        count = body.get("count")
                        if mode in ("http500", "drop_response") and count is None:
                            count = 1
                        if count is not None:
                            count = int(count)
                        if count is None or count > 0:
                            st.faults.append({"mode": mode, "count": count, "ms": body.get("ms"),
                                              "status": body.get("status"), "channel": body.get("channel")})
                    else:
                        return self._send(400, {"ok": False, "error": "unknown fault mode %r" % (mode,)})
            with st.lock:
                return self._send(200, {"ok": True, "faults": st.faults})
        if path == "/__admin/clock":
            if method == "POST":
                with st.lock:
                    now = body.get("now")
                    if now is None:
                        st.clock = None
                    else:
                        now = parse_time(now) if isinstance(now, str) else float(now)
                        st.clock = (float(now), time.time(), bool(body.get("freeze")))
            return self._send(200, {"ok": True, "now": st.now(), "virtual": st.clock is not None})
        m = re.match(r"^/__admin/dump/(\d+)$", path)
        if m:
            ch = st.channels.get(int(m.group(1)))
            if ch is None:
                return self._send(404, {"ok": False, "error": "unknown channel"})
            since = int(query.get("since_entry", 0))
            with st.lock:
                entries = []
                for e in ch.entries:
                    if e["entry_id"] <= since:
                        continue
                    row = feed_json(e, None, True)
                    row["received_at"] = e["received"]
                    entries.append(row)
                out = {"channel": ch.id, "last_entry_id": ch.last_entry_id, "count": len(ch.entries),
                       "last_write": ch.last_write, "entries": entries}
            return self._send(200, out)
        if path == "/__admin/log":
            since = int(query.get("since", 0))
            with st.lock:
                recs = [r for r in st.log if r["n"] > since]
                return self._send(200, {"ok": True, "total": len(st.log) + st.log_dropped, "log": recs})
        self._send(404, {"ok": False, "error": "unknown admin endpoint"})


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 64

    def handle_error(self, request, client_address):  # quiet on client disconnects
        exc = sys.exc_info()[1]
        if isinstance(exc, (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, TimeoutError)):
            return
        super().handle_error(request, client_address)


def parse_channel(spec):
    parts = spec.split(":")
    if len(parts) < 2 or not parts[0].isdigit():
        raise argparse.ArgumentTypeError("channel must be ID:WRITEKEY:READKEY, got %r" % spec)
    cid, wkey = int(parts[0]), parts[1]
    rkey = parts[2] if len(parts) > 2 else ""
    name = ":".join(parts[3:]) if len(parts) > 3 else None
    return Channel(cid, wkey, rkey, name)


def make_server(channels, port=0, host="127.0.0.1", state_file=None, reject_dup_ts=False, verbose=False):
    """Create (not start) a server. `channels`: list of Channel or 'ID:W:R' strings."""
    store = Store(reject_dup_ts=reject_dup_ts, state_file=state_file)
    for c in channels:
        c = parse_channel(c) if isinstance(c, str) else c
        store.channels[c.id] = c
    store.load()
    handler = type("BoundHandler", (Handler,), {"store": store, "verbose": verbose})
    srv = Server((host, port), handler)
    srv.store = store
    return srv


def main(argv=None):
    ap = argparse.ArgumentParser(description="Mock ThingSpeak server (CONTRACT.md section 10)")
    ap.add_argument("--port", type=int, default=18766)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--channel", action="append", type=parse_channel, default=[],
                    metavar="ID:WRITEKEY:READKEY", help="repeatable")
    ap.add_argument("--state", metavar="FILE", help="persist channel contents in this JSON file")
    ap.add_argument("--existing-timestamp-policy", choices=("store", "reject"), default="store",
                    help="what to do with an update whose created_at is already in the channel (default store)")
    ap.add_argument("--reject-duplicate-timestamps", action="store_true",
                    help="same as --existing-timestamp-policy reject")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)
    if not args.channel:
        ap.error("at least one --channel ID:WRITEKEY:READKEY is required")
    reject = args.reject_duplicate_timestamps or args.existing_timestamp_policy == "reject"
    srv = make_server(args.channel, args.port, args.host, args.state, reject, args.verbose)
    print("LISTENING http://%s:%d" % (args.host, srv.server_address[1]), flush=True)
    print("channels: %s" % ", ".join(str(c.id) for c in args.channel), flush=True)
    try:
        srv.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        srv.store.save()
        srv.server_close()
    return 0


if __name__ == "__main__":
    import signal

    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    sys.exit(main())
