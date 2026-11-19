"""
Unit tests for tools/mock_thingspeak.py (CONTRACT.md sections 3, 7.3, 8, 10).

    python3 -m pytest tools/test_mock_thingspeak.py -q
"""
import http.client
import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_thingspeak as mt  # noqa: E402

CH1, W1, R1 = 1001, "WRITEKEY1001", "READKEY1001"
CH2, W2, R2 = 1002, "WRITEKEY1002", "READKEY1002"
PUB, WP = 1003, "WRITEKEY1003"
T0 = 1795132800  # 2026-11-20T00:00:00Z


class Client:
    def __init__(self, base):
        self.base = base

    def req(self, method, path, body=None, headers=None, timeout=10):
        data = None
        hdrs = dict(headers or {})
        if body is not None:
            if isinstance(body, (dict, list)):
                data = json.dumps(body).encode()
                hdrs.setdefault("Content-Type", "application/json")
            else:
                data = body if isinstance(body, bytes) else body.encode()
        r = urllib.request.Request(self.base + path, data=data, method=method, headers=hdrs)
        try:
            with urllib.request.urlopen(r, timeout=timeout) as resp:
                return resp.status, resp.read().decode(), dict(resp.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode(), dict(e.headers)

    def get(self, path, **kw):
        return self.req("GET", path, **kw)

    def post(self, path, body=None, **kw):
        return self.req("POST", path, body if body is not None else {}, **kw)

    def bulk(self, updates, ch=CH1, key=W1, sim=None, headers=None):
        h = dict(headers or {})
        if sim is not None:
            h["X-Sim-Time"] = str(sim)
        return self.post("/channels/%d/bulk_update.json" % ch, {"write_api_key": key, "updates": updates}, headers=h)

    def feeds(self, ch=CH1, key=R1, **params):
        q = dict(params)
        if key:
            q["api_key"] = key
        s, b, h = self.get("/channels/%d/feeds.json?%s" % (ch, urllib.parse.urlencode(q, quote_via=urllib.parse.quote)))
        return s, (json.loads(b) if b.strip().startswith("{") else b), h


def upd(minute, seq=None, t0=T0, **extra):
    ts = time.strftime("%Y-%m-%d %H:%M:%S +0000", time.gmtime(t0 + 60 * minute))
    u = {"created_at": ts, "field1": 1000.5 + minute, "field2": 800.0, "field3": 120.0, "field4": -10.0,
         "field7": 5.012, "field8": seq if seq is not None else minute + 1, "status": "TRK;r=1;p=A;e=0;s=1"}
    u.update(extra)
    return u


@pytest.fixture()
def srv():
    server = mt.make_server(["%d:%s:%s" % (CH1, W1, R1), "%d:%s:%s" % (CH2, W2, R2), "%d:%s:" % (PUB, WP)], port=0)
    th = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
    th.start()
    c = Client("http://127.0.0.1:%d" % server.server_address[1])
    c.server = server
    yield c
    server.shutdown()
    server.server_close()


# ------------------------------------------------------------ time parsing

@pytest.mark.parametrize("text,expected", [
    ("2026-11-20 00:00:00 +0000", T0),
    ("2026-11-20T00:00:00Z", T0),
    ("2026-11-20 00:00:00", T0),
    ("2026-11-20T01:00:00+01:00", T0),
    ("2026-11-20 01:00:00 +0100", T0),
    ("2026-11-19 19:00:00 -0500", T0),
    ("2026-11-20 00:00:00 UTC", T0),
    ("2026-11-20T00:00:00.000Z", T0),
    (str(T0), T0),
    (T0, T0),
])
def test_parse_time_formats(text, expected):
    assert mt.parse_time(text) == expected


@pytest.mark.parametrize("text", ["", "yesterday", "2026-13-01 00:00:00", "2026-11-31 00:00:00", "20/11/2026 10:00"])
def test_parse_time_rejects(text):
    with pytest.raises(ValueError):
        mt.parse_time(text)


def test_iso_z():
    assert mt.iso_z(T0) == "2026-11-20T00:00:00Z"


# ------------------------------------------------------------ bulk write

def test_bulk_success_202_and_entry_ids(srv):
    s, b, h = srv.bulk([upd(i) for i in range(5)], sim=T0 + 1000)
    assert s == 202
    assert json.loads(b) == {"success": True}
    assert h["Access-Control-Allow-Origin"] == "*"
    s, b, _ = srv.get("/__admin/dump/%d" % CH1)
    d = json.loads(b)
    assert [e["entry_id"] for e in d["entries"]] == [1, 2, 3, 4, 5]
    assert d["entries"][0]["created_at"] == "2026-11-20T00:00:00Z"
    assert d["entries"][0]["field8"] == "1"
    assert d["entries"][0]["status"] == "TRK;r=1;p=A;e=0;s=1"
    s, b, _ = srv.bulk([upd(5)], sim=T0 + 1100)
    assert s == 202
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["last_entry_id"] == 6


def test_bulk_wrong_key_401_json(srv):
    s, b, _ = srv.bulk([upd(0)], key="NOPE")
    assert s == 401
    body = json.loads(b)
    assert body["status"] == "401" and body["error"]["error_code"] == "error_auth_required"
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 0


def test_bulk_requires_json_content_type(srv):
    payload = json.dumps({"write_api_key": W1, "updates": [upd(0)]})
    for ctype in ("application/x-www-form-urlencoded", "text/plain", None):
        hdr = {"Content-Type": ctype} if ctype else {"Content-Type": ""}
        s, b, _ = srv.req("POST", "/channels/%d/bulk_update.json" % CH1, payload.encode(), headers=hdr)
        assert s == 401, ctype
        assert json.loads(b)["status"] == "401"
    s, _, _ = srv.req("POST", "/channels/%d/bulk_update.json" % CH1, payload.encode(),
                      headers={"Content-Type": "application/json; charset=utf-8"})
    assert s == 202


def test_bulk_unknown_channel_404(srv):
    s, b, _ = srv.bulk([upd(0)], ch=4242)
    assert s == 404 and json.loads(b)["status"] == "404"


def test_bulk_960_limit(srv):
    s, b, _ = srv.bulk([upd(i) for i in range(961)], sim=T0)
    assert s == 400
    assert json.loads(b)["error"]["error_code"] == "error_bad_request"
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 0
    s, _, _ = srv.bulk([upd(i) for i in range(960)], sim=T0)
    assert s == 202
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 960


def test_bulk_bad_requests_400(srv):
    assert srv.req("POST", "/channels/%d/bulk_update.json" % CH1, b"{not json",
                   headers={"Content-Type": "application/json"})[0] == 400
    assert srv.bulk([])[0] == 400
    assert srv.bulk([{"created_at": "garbage", "field1": 1}])[0] == 400
    assert srv.bulk([{"field1": 1}])[0] == 400
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 0


def test_bulk_created_at_formats_and_delta_t(srv):
    ups = [
        {"created_at": "2026-11-20 00:00:00 +0000", "field1": 1},
        {"created_at": "2026-11-20T00:01:00Z", "field1": 2},
        {"created_at": "2026-11-20 01:02:00 +0100", "field1": 3},
        {"created_at": "2026-11-20T00:03:00", "field1": 4},
        {"created_at": T0 + 240, "field1": 5},
    ]
    assert srv.bulk(ups, sim=T0 + 600)[0] == 202
    s, body, _ = srv.feeds(results=10)
    assert [f["created_at"] for f in body["feeds"]] == [
        "2026-11-20T00:00:00Z", "2026-11-20T00:01:00Z", "2026-11-20T00:02:00Z",
        "2026-11-20T00:03:00Z", "2026-11-20T00:04:00Z"]
    # delta_t: the last update lands on "now", earlier ones are spaced back
    assert srv.bulk([{"delta_t": 0, "field1": 1}, {"delta_t": 60, "field1": 2}, {"delta_t": 30, "field1": 3}],
                    ch=CH2, key=W2, sim=T0 + 1000)[0] == 202
    s, body, _ = srv.feeds(ch=CH2, key=R2, results=10)
    assert [f["created_at"] for f in body["feeds"]] == [
        mt.iso_z(T0 + 1000 - 90), mt.iso_z(T0 + 1000 - 30), mt.iso_z(T0 + 1000)]


def test_spacing_rule_sim_time(srv):
    assert srv.bulk([upd(0)], sim=T0 + 100)[0] == 202
    s, b, _ = srv.bulk([upd(1)], sim=T0 + 114)
    assert s == 429
    assert json.loads(b)["error"]["error_code"] == "error_too_many_requests"
    assert srv.bulk([upd(1)], sim=T0 + 114.9)[0] == 429
    # a refused request does not restart the 15 s window
    assert srv.bulk([upd(1)], sim=T0 + 115)[0] == 202
    assert srv.bulk([upd(2)], sim=T0 + 131)[0] == 202
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 3


def test_spacing_rule_is_per_channel(srv):
    assert srv.bulk([upd(0)], sim=T0)[0] == 202
    assert srv.bulk([upd(0)], ch=CH2, key=W2, sim=T0 + 1)[0] == 202
    assert srv.bulk([upd(1)], sim=T0 + 2)[0] == 429


def test_spacing_rule_wall_clock(srv):
    assert srv.bulk([upd(0)])[0] == 202
    assert srv.bulk([upd(1)])[0] == 429


def test_spacing_rule_admin_clock(srv):
    s, b, _ = srv.post("/__admin/clock", {"now": T0, "freeze": True})
    assert s == 200 and json.loads(b)["now"] == T0
    assert srv.bulk([upd(0)])[0] == 202
    assert srv.bulk([upd(1)])[0] == 429
    srv.post("/__admin/clock", {"now": "2026-11-20T00:00:20Z", "freeze": True})
    assert srv.bulk([upd(1)])[0] == 202
    # failed authentication is checked before, and never counts for, spacing
    assert srv.bulk([upd(2)], key="bad")[0] == 401


def test_duplicate_created_at_stored_by_default(srv):
    assert srv.bulk([upd(0, seq=1), upd(1, seq=2)], sim=T0)[0] == 202
    assert srv.bulk([upd(0, seq=1), upd(1, seq=2), upd(2, seq=3)], sim=T0 + 20)[0] == 202
    d = json.loads(srv.get("/__admin/dump/%d" % CH1)[1])
    assert d["count"] == 5
    assert [e["field8"] for e in d["entries"]] == ["1", "2", "1", "2", "3"]
    s, body, _ = srv.feeds(results=100)
    assert [f["entry_id"] for f in body["feeds"]] == [1, 3, 2, 4, 5]  # by created_at, then entry_id
    log = json.loads(srv.get("/__admin/log")[1])["log"]
    bulk = [r for r in log if r["kind"] == "bulk"]
    assert bulk[1]["dup_ts"] == 2 and bulk[1]["stored"] == 3


def test_duplicate_created_at_rejected_when_configured():
    server = mt.make_server(["%d:%s:%s" % (CH1, W1, R1)], port=0, reject_dup_ts=True)
    th = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
    th.start()
    try:
        c = Client("http://127.0.0.1:%d" % server.server_address[1])
        assert c.bulk([upd(0, seq=1), upd(1, seq=2)], sim=T0)[0] == 202
        assert c.bulk([upd(0, seq=1), upd(1, seq=2), upd(2, seq=3)], sim=T0 + 20)[0] == 202
        d = json.loads(c.get("/__admin/dump/%d" % CH1)[1])
        assert [e["field8"] for e in d["entries"]] == ["1", "2", "3"]
        assert [e["entry_id"] for e in d["entries"]] == [1, 2, 3]
    finally:
        server.shutdown()
        server.server_close()


def test_numbers_as_strings_missing_as_null(srv):
    raw = ('{"write_api_key":"%s","updates":[{"created_at":"2026-11-20 00:00:00 +0000",'
           '"field1":1234.5,"field2":0.0,"field7":5.010,"field8":7,"status":"IDL;r=0;p=A;e=0"}]}' % W1)
    assert srv.req("POST", "/channels/%d/bulk_update.json" % CH1, raw.encode(),
                   headers={"Content-Type": "application/json"})[0] == 202
    s, body, _ = srv.feeds(results=1, status="true")
    f = body["feeds"][0]
    assert f["field1"] == "1234.5" and f["field2"] == "0.0"
    assert f["field7"] == "5.010"          # exact text preserved
    assert f["field8"] == "7"
    assert f["field3"] is None and f["field5"] is None and f["field6"] is None
    assert f["status"] == "IDL;r=0;p=A;e=0"
    assert f["entry_id"] == 1 and isinstance(f["entry_id"], int)


# ------------------------------------------------------------ read API

def fill(srv, n, t0=T0, ch=CH1, key=W1):
    sim = t0 + n * 60
    for i in range(0, n, 960):
        s, b, _ = srv.bulk([upd(j, t0=t0) for j in range(i, min(n, i + 960))], ch=ch, key=key, sim=sim)
        assert s == 202, b
        sim += 16


def test_feeds_read_key(srv):
    fill(srv, 3)
    for key in ("WRONG", ""):
        s, b, h = srv.get("/channels/%d/feeds.json?results=5%s" % (CH1, "&api_key=" + key if key else ""))
        assert s == 400 and b.strip() == "-1"
        assert h["Access-Control-Allow-Origin"] == "*"
    # the write key is not a read key
    assert srv.feeds(key=W1)[1].strip() == "-1"
    s, body, _ = srv.feeds()
    assert s == 200 and len(body["feeds"]) == 3
    assert srv.get("/channels/%d/feeds/last.json" % CH1)[1].strip() == "-1"
    assert srv.get("/channels/%d/fields/8/last.json?api_key=bad" % CH1)[1].strip() == "-1"


def test_feeds_public_channel_needs_no_key(srv):
    fill(srv, 2, ch=PUB, key=WP)
    s, body, _ = srv.feeds(ch=PUB, key=None)
    assert s == 200 and len(body["feeds"]) == 2


def test_feeds_unknown_channel_404(srv):
    s, b, _ = srv.get("/channels/999/feeds.json")
    assert s == 404 and json.loads(b) == {"status": "404", "error": "Not Found"}


def test_feeds_channel_metadata_and_shape(srv):
    fill(srv, 2)
    s, body, _ = srv.feeds(results=1)
    ch = body["channel"]
    assert ch["id"] == CH1 and ch["last_entry_id"] == 2
    assert all(("field%d" % i) in ch for i in range(1, 9))
    assert ch["created_at"].endswith("Z") and ch["updated_at"].endswith("Z")
    f = body["feeds"][0]
    assert "status" not in f                       # only with status=true
    assert set(f) == {"created_at", "entry_id"} | {"field%d" % i for i in range(1, 9)}


def test_feeds_default_100_and_max_8000_newest_oldest_first(srv):
    fill(srv, 8200)
    s, body, _ = srv.feeds()
    assert len(body["feeds"]) == 100
    assert body["feeds"][-1]["created_at"] == mt.iso_z(T0 + 8199 * 60)
    assert body["feeds"][0]["created_at"] == mt.iso_z(T0 + 8100 * 60)
    s, body, _ = srv.feeds(results=100000)
    feeds = body["feeds"]
    assert len(feeds) == 8000
    assert feeds[0]["created_at"] == mt.iso_z(T0 + 200 * 60)      # the OLDEST 200 are cut off
    assert feeds[-1]["created_at"] == mt.iso_z(T0 + 8199 * 60)
    times = [f["created_at"] for f in feeds]
    assert times == sorted(times)
    s, body, _ = srv.feeds(results=0)
    assert body["feeds"] == []


def test_feeds_window_inclusive_and_newest_inside_window(srv):
    fill(srv, 600)
    s, body, _ = srv.feeds(start="2026-11-20 01:00:00", end="2026-11-20 02:00:00", timezone="Etc/UTC", results=8000)
    feeds = body["feeds"]
    assert len(feeds) == 61
    assert feeds[0]["created_at"] == "2026-11-20T01:00:00Z" and feeds[-1]["created_at"] == "2026-11-20T02:00:00Z"
    s, body, _ = srv.feeds(start="2026-11-20 01:00:00", end="2026-11-20 02:00:00", timezone="UTC", results=10)
    assert [f["created_at"] for f in body["feeds"]][0] == "2026-11-20T01:51:00Z"
    assert len(body["feeds"]) == 10
    # only start / only end
    assert len(srv.feeds(start="2026-11-20 09:50:00", results=8000)[1]["feeds"]) == 10
    assert len(srv.feeds(end="2026-11-20 00:09:00", results=8000)[1]["feeds"]) == 10
    # default timezone is UTC
    assert len(srv.feeds(start="2026-11-20 01:00:00", end="2026-11-20 01:00:59", results=8000)[1]["feeds"]) == 1


def test_feeds_percent20_and_plus_spaces(srv):
    fill(srv, 10)
    for sep in ("%20", "+"):
        s, b, _ = srv.get("/channels/%d/feeds.json?api_key=%s&start=2026-11-20%s00:02:00&end=2026-11-20%s00:04:00"
                          "&results=8000" % (CH1, R1, sep, sep))
        assert s == 200 and len(json.loads(b)["feeds"]) == 3


def test_feeds_timezone_param(srv):
    fill(srv, 180)
    # 01:00 in Paris (UTC+1 in November) is 00:00 UTC
    s, body, _ = srv.feeds(start="2026-11-20 01:00:00", end="2026-11-20 01:04:00", timezone="Europe/Paris", results=8000)
    assert len(body["feeds"]) == 5
    assert body["feeds"][0]["created_at"] == "2026-11-20T01:00:00+01:00"
    s, b, _ = srv.feeds(timezone="Mars/Olympus")
    assert s == 400
    s, b, _ = srv.feeds(start="not a time")
    assert s == 400


def test_feeds_backfill_sorted_by_time(srv):
    # newer minutes arrive first, the older backlog later (device catching up)
    assert srv.bulk([upd(10, seq=11), upd(11, seq=12)], sim=T0 + 1000)[0] == 202
    assert srv.bulk([upd(0, seq=1), upd(1, seq=2)], sim=T0 + 1020)[0] == 202
    s, body, _ = srv.feeds(results=8000)
    assert [f["field8"] for f in body["feeds"]] == ["1", "2", "11", "12"]
    assert [f["entry_id"] for f in body["feeds"]] == [3, 4, 1, 2]
    # "last" is the most recently received entry
    assert json.loads(srv.get("/channels/%d/feeds/last.json?api_key=%s" % (CH1, R1))[1])["entry_id"] == 4


def test_last_endpoints(srv):
    s, b, _ = srv.get("/channels/%d/feeds/last.json?api_key=%s" % (CH1, R1))
    assert s == 200 and b.strip() == "-1"            # empty channel
    assert srv.bulk([upd(0, seq=41), upd(1, seq=42)], sim=T0)[0] == 202
    assert srv.bulk([{"created_at": "2026-11-20 00:02:00 +0000", "field1": 5, "status": "IDL"}], sim=T0 + 20)[0] == 202
    last = json.loads(srv.get("/channels/%d/feeds/last.json?api_key=%s&status=true" % (CH1, R1))[1])
    assert last["entry_id"] == 3 and last["field1"] == "5" and last["field8"] is None and last["status"] == "IDL"
    f8 = json.loads(srv.get("/channels/%d/fields/8/last.json?api_key=%s" % (CH1, R1))[1])
    assert f8 == {"created_at": "2026-11-20T00:01:00Z", "entry_id": 2, "field8": "42"}
    assert srv.get("/channels/%d/fields/8/last.txt?api_key=%s" % (CH1, R1))[1] == "42"
    s, b, _ = srv.get("/channels/%d/fields/9/last.json?api_key=%s" % (CH1, R1))
    assert s == 404


def test_single_update(srv):
    s, b, _ = srv.get("/update?api_key=%s&field1=12.5&field8=3&status=TRK&created_at=2026-11-20%%2000:00:00" % W1,
                      headers={"X-Sim-Time": str(T0)})
    assert s == 200 and b == "1"
    s, b, _ = srv.post("/update.json", {"api_key": W1, "field1": 13.5}, headers={"X-Sim-Time": str(T0 + 5)})
    assert s == 429 and b == "0"
    s, b, _ = srv.post("/update.json", {"api_key": W1, "field1": 13.5}, headers={"X-Sim-Time": str(T0 + 15)})
    assert s == 200
    e = json.loads(b)
    assert e["entry_id"] == 2 and e["field1"] == "13.5" and e["channel_id"] == CH1
    assert e["created_at"] == mt.iso_z(T0 + 15)
    s, b, _ = srv.req("POST", "/update", "api_key=%s&field2=7" % W1,
                      headers={"Content-Type": "application/x-www-form-urlencoded", "X-Sim-Time": str(T0 + 40)})
    assert s == 200 and b == "3"
    s, b, _ = srv.get("/update?api_key=WRONG&field1=1")
    assert s == 400 and b == "0"


# ------------------------------------------------------------ CORS

def test_cors_preflight_and_headers(srv):
    s, b, h = srv.req("OPTIONS", "/channels/%d/feeds.json" % CH1,
                      headers={"Origin": "http://127.0.0.1:8000", "Access-Control-Request-Method": "GET"})
    assert s == 200
    assert h["Access-Control-Allow-Origin"] == "*"
    assert "GET" in h["Access-Control-Allow-Methods"] and "POST" in h["Access-Control-Allow-Methods"]
    assert h["Access-Control-Allow-Headers"] == "origin, content-type, X-Requested-With"   # as the real service
    assert h["Access-Control-Allow-Methods"] == "GET, POST, PUT, OPTIONS, DELETE, PATCH"
    for path in ("/channels/%d/feeds.json?api_key=%s" % (CH1, R1), "/channels/999/feeds.json", "/nothing",
                 "/__admin/log", "/__admin/health"):
        assert srv.get(path)[2]["Access-Control-Allow-Origin"] == "*", path
    assert srv.bulk([upd(0)], key="bad")[2]["Access-Control-Allow-Origin"] == "*"


# ------------------------------------------------------------ faults / admin

def test_fault_http500_next_n(srv):
    assert srv.post("/__admin/faults", {"mode": "http500", "count": 2})[0] == 200
    for k in range(2):
        s, b, _ = srv.bulk([upd(0)], sim=T0 + k)
        assert s == 500 and json.loads(b)["status"] == "500"
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 0
    assert srv.bulk([upd(0)], sim=T0 + 3)[0] == 202     # failures did not start the 15 s window
    assert json.loads(srv.get("/__admin/faults")[1])["faults"] == []
    # reads are not affected by write faults
    srv.post("/__admin/faults", {"mode": "http500", "count": 1})
    assert srv.feeds()[0] == 200


def test_fault_drop_response_stores_but_no_reply(srv):
    srv.post("/__admin/faults", {"mode": "drop_response", "count": 1})
    conn = http.client.HTTPConnection("127.0.0.1", srv.server.server_address[1], timeout=5)
    conn.request("POST", "/channels/%d/bulk_update.json" % CH1,
                 json.dumps({"write_api_key": W1, "updates": [upd(0), upd(1)]}),
                 {"Content-Type": "application/json", "X-Sim-Time": str(T0)})
    with pytest.raises((http.client.RemoteDisconnected, ConnectionResetError, http.client.BadStatusLine)):
        conn.getresponse()
    conn.close()
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 2      # the write happened
    # the stored write counts for spacing; the retry after 16 s succeeds and duplicates
    assert srv.bulk([upd(0), upd(1)], sim=T0 + 5)[0] == 429
    assert srv.bulk([upd(0), upd(1)], sim=T0 + 16)[0] == 202
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 4
    log = [r for r in json.loads(srv.get("/__admin/log")[1])["log"] if r["kind"] == "bulk"]
    assert log[0]["status"] == 0 and log[0]["fault"] == "drop_response" and log[0]["stored"] == 2


def test_fault_delay(srv):
    srv.post("/__admin/faults", {"mode": "delay", "ms": 300, "count": 1})
    t = time.time()
    assert srv.feeds()[0] == 200
    assert time.time() - t >= 0.28
    t = time.time()
    assert srv.feeds()[0] == 200
    assert time.time() - t < 0.25


def test_fault_reject_all_until_cleared(srv):
    srv.post("/__admin/faults", {"mode": "reject_all"})
    for k in range(3):
        s, b, _ = srv.bulk([upd(k)], sim=T0 + 100 * k)
        assert s == 429
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 0
    srv.post("/__admin/faults", {"mode": "clear"})
    assert srv.bulk([upd(0)], sim=T0 + 1000)[0] == 202
    srv.post("/__admin/faults", {"mode": "reject_all", "status": 503, "count": 1})
    assert srv.bulk([upd(1)], sim=T0 + 2000)[0] == 503
    assert srv.bulk([upd(1)], sim=T0 + 2001)[0] == 202


def test_fault_channel_filter_and_unknown_mode(srv):
    srv.post("/__admin/faults", {"mode": "http500", "count": 1, "channel": CH2})
    assert srv.bulk([upd(0)], sim=T0)[0] == 202
    assert srv.bulk([upd(0)], ch=CH2, key=W2, sim=T0)[0] == 500
    assert srv.post("/__admin/faults", {"mode": "explode"})[0] == 400


def test_admin_log_records_writes_and_reads(srv):
    srv.bulk([upd(i, seq=100 + i) for i in range(3)], sim=T0 + 500)
    srv.bulk([upd(3)], sim=T0 + 505)
    srv.feeds(results=2)
    log = json.loads(srv.get("/__admin/log")[1])["log"]
    assert [r["kind"] for r in log] == ["bulk", "bulk", "feeds"]
    a, b, c = log
    assert a["status"] == 202 and a["updates"] == 3 and a["stored"] == 3 and a["sim"] == T0 + 500
    assert a["seq_min"] == 100 and a["seq_max"] == 102
    assert a["first_created_at"] == "2026-11-20T00:00:00Z" and a["last_created_at"] == "2026-11-20T00:02:00Z"
    assert b["status"] == 429 and b["spacing_s"] == 5
    assert c["status"] == 200 and c["returned"] == 2 and c["in_window"] == 3
    assert json.loads(srv.get("/__admin/log?since=2")[1])["log"][0]["kind"] == "feeds"


def test_admin_reset(srv):
    fill(srv, 5)
    fill(srv, 5, ch=CH2, key=W2)
    srv.post("/__admin/faults", {"mode": "reject_all"})
    assert srv.post("/__admin/reset", {"channels": [CH1], "keep_log": True})[0] == 200
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 0
    assert json.loads(srv.get("/__admin/dump/%d" % CH2)[1])["count"] == 5
    assert len(json.loads(srv.get("/__admin/log")[1])["log"]) == 2
    assert json.loads(srv.get("/__admin/faults")[1])["faults"] == []
    assert srv.post("/__admin/reset")[0] == 200
    assert json.loads(srv.get("/__admin/dump/%d" % CH2)[1])["count"] == 0
    assert json.loads(srv.get("/__admin/log")[1])["log"] == []
    # entry ids restart and the spacing memory is gone
    assert srv.bulk([upd(0)], sim=T0)[0] == 202
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["entries"][0]["entry_id"] == 1


def test_concurrent_writes_one_wins_spacing(srv):
    results = []

    def worker(k):
        results.append(srv.bulk([upd(k, seq=k + 1)], sim=T0 + 50)[0])

    threads = [threading.Thread(target=worker, args=(k,)) for k in range(8)]
    [t.start() for t in threads]
    [t.join() for t in threads]
    assert sorted(results) == [202] + [429] * 7
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 1


def test_existing_timestamp_policy_reject_runtime_switch(srv):
    assert json.loads(srv.get("/__admin/config")[1])["existing_timestamp_policy"] == "store"
    assert srv.post("/__admin/config", {"existing_timestamp_policy": "reject"})[0] == 200
    assert srv.bulk([upd(0, seq=1), upd(1, seq=2)], sim=T0)[0] == 202
    # timestamps already in the channel are dropped one by one, the rest is stored, answer still 202
    s, b, _ = srv.bulk([upd(0, seq=1), upd(1, seq=2), upd(2, seq=3)], sim=T0 + 20)
    assert s == 202 and json.loads(b) == {"success": True}
    d = json.loads(srv.get("/__admin/dump/%d" % CH1)[1])
    assert [e["field8"] for e in d["entries"]] == ["1", "2", "3"]
    # the same created_at twice inside ONE request: whole request refused, nothing stored
    s, b, _ = srv.bulk([upd(5, seq=6), upd(5, seq=7), upd(6, seq=8)], sim=T0 + 40)
    assert s == 400 and json.loads(b)["error"]["error_code"] == "error_bad_request"
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 3
    assert srv.bulk([upd(5, seq=6)], sim=T0 + 41)[0] == 202      # the refused request did not start the 15 s window
    assert srv.post("/__admin/config", {"existing_timestamp_policy": "maybe"})[0] == 400
    srv.post("/__admin/config", {"existing_timestamp_policy": "store"})
    assert srv.bulk([upd(5, seq=6), upd(5, seq=6)], sim=T0 + 100)[0] == 202
    assert json.loads(srv.get("/__admin/dump/%d" % CH1)[1])["count"] == 6


def test_cli_existing_timestamp_policy_flag():
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_thingspeak.py")
    for flags in (["--existing-timestamp-policy", "reject"], ["--reject-duplicate-timestamps"]):
        p = subprocess.Popen([sys.executable, script, "--port", "0", "--channel", "%d:%s:%s" % (CH1, W1, R1)] + flags,
                             stdout=subprocess.PIPE, text=True)
        try:
            c = Client(p.stdout.readline().split()[1])
            assert json.loads(c.get("/__admin/config")[1])["existing_timestamp_policy"] == "reject"
        finally:
            p.terminate()
            p.wait(timeout=10)


# ------------------------------------------------------------ CLI + persistence

def test_cli_state_persistence(tmp_path):
    state = str(tmp_path / "state.json")
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_thingspeak.py")

    def start():
        p = subprocess.Popen([sys.executable, script, "--port", "0", "--channel", "%d:%s:%s" % (CH1, W1, R1),
                              "--state", state], stdout=subprocess.PIPE, text=True)
        line = p.stdout.readline().strip()
        assert line.startswith("LISTENING http://127.0.0.1:")
        return p, Client(line.split()[1])

    p, c = start()
    try:
        assert c.bulk([upd(0), upd(1)], sim=T0)[0] == 202
    finally:
        p.terminate()
        p.wait(timeout=10)
    assert os.path.exists(state)
    p, c = start()
    try:
        d = json.loads(c.get("/__admin/dump/%d" % CH1)[1])
        assert d["count"] == 2 and d["last_entry_id"] == 2
        assert c.bulk([upd(2)], sim=T0 + 5)[0] == 429        # spacing memory persisted too
        assert c.bulk([upd(2)], sim=T0 + 20)[0] == 202
        assert json.loads(c.get("/__admin/dump/%d" % CH1)[1])["entries"][-1]["entry_id"] == 3
    finally:
        p.terminate()
        p.wait(timeout=10)


def test_cli_requires_channel():
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_thingspeak.py")
    r = subprocess.run([sys.executable, script, "--port", "0"], capture_output=True, text=True)
    assert r.returncode == 2
