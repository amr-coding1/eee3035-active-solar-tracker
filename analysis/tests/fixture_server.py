#!/usr/bin/env python3
"""
fixture_server.py - minimal ThingSpeak stand-in owned by the analysis tests.

The shared mock (tools/mock_thingspeak.py) always answers with tidy feeds:
every key present, every value a string. This server instead returns channel-1
feeds EXACTLY as given in a JSON file, so the tests can feed ast_nightly.m
awkward but legal JSON (keys missing from some feeds, JSON numbers, '+00:00'
time zone text, null status). It is also the fallback if the shared mock is
not available.

    python3 fixture_server.py --feeds feeds.json [--port 0] [--read-key K] [--write-key K]

    GET  /channels/1/feeds.json?start=..&end=..&results=N[&api_key=K]
         feeds of the file inside [start, end] (inclusive, UTC), newest N, oldest first
    POST /channels/2/bulk_update.json   JSON body {"write_api_key":..,"updates":[..]} -> 202
    GET  /channels/2/feeds.json         what was written (entry ids 1,2,3..)
    GET  /__requests                    log of requests
First stdout line: "LISTENING http://127.0.0.1:<port>".
"""
import argparse
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "py"))
import ast_metrics as am  # noqa: E402

STATE = {"feeds": [], "ch2": [], "requests": [], "read_key": "", "write_key": ""}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send(self, code, obj, ctype="application/json"):
        body = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        STATE["requests"].append({"method": "GET", "path": url.path, "query": q})
        if url.path == "/__requests":
            return self._send(200, STATE["requests"])
        if url.path == "/channels/2/feeds.json":
            return self._send(200, {"channel": {"id": 2}, "feeds": STATE["ch2"]})
        if url.path != "/channels/1/feeds.json":
            return self._send(404, {"status": "404", "error": "Not Found"})
        if STATE["read_key"] and q.get("api_key") != STATE["read_key"]:
            return self._send(400, b"-1", "text/plain")
        lo = am.parse_time(q["start"]) if "start" in q else None
        hi = am.parse_time(q["end"]) if "end" in q else None
        sel = []
        for f in STATE["feeds"]:
            t = am.parse_time(f.get("created_at"))
            if t is None or ((lo is None or t >= lo) and (hi is None or t <= hi)):
                sel.append(f)                      # unreadable times are passed through on purpose
        n = min(8000, int(q.get("results", 100)))
        self._send(200, {"channel": {"id": 1}, "feeds": sel[-n:] if n else []})

    def do_POST(self):
        url = urlparse(self.path)
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        STATE["requests"].append({"method": "POST", "path": url.path, "content_type": ctype, "bytes": len(raw)})
        if url.path != "/channels/2/bulk_update.json":
            return self._send(404, {"status": "404", "error": "Not Found"})
        if ctype != "application/json":
            return self._send(401, {"status": "401", "error": "content type"})
        try:
            body = json.loads(raw.decode("utf-8"), parse_float=str, parse_int=str)
        except ValueError:
            return self._send(400, {"status": "400", "error": "bad JSON"})
        if body.get("write_api_key") != STATE["write_key"]:
            return self._send(401, {"status": "401", "error": "write key"})
        ups = body.get("updates")
        if not isinstance(ups, list) or not ups or len(ups) > 960:
            return self._send(400, {"status": "400", "error": "updates"})
        for u in ups:
            t = am.parse_time(u.get("created_at"))
            if t is None:
                return self._send(400, {"status": "400", "error": "created_at"})
            row = {"created_at": am.utc_day(t) + "T" + "%02d:%02d:%02dZ" % (t // 3600000 % 24, t // 60000 % 60, t // 1000 % 60),
                   "entry_id": len(STATE["ch2"]) + 1}
            for i in range(1, 9):
                v = u.get("field%d" % i)
                row["field%d" % i] = None if v is None else str(v)
            row["status"] = u.get("status")
            STATE["ch2"].append(row)
        self._send(202, {"success": True})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--feeds", required=True)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--read-key", default="")
    ap.add_argument("--write-key", default="FIXTUREWRITEKEY")
    a = ap.parse_args()
    with open(a.feeds, encoding="utf-8") as fh:
        STATE["feeds"] = json.load(fh)
    STATE["read_key"], STATE["write_key"] = a.read_key, a.write_key
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print("LISTENING http://127.0.0.1:%d" % srv.server_address[1], flush=True)
    try:
        srv.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
