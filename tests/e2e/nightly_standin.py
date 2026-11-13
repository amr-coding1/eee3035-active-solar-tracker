#!/usr/bin/env python3
"""
nightly_standin.py - PYTHON STAND-IN for analysis/matlab/ast_nightly.m.

NOT the MATLAB job. run_e2e.py uses it only when GNU Octave or the .m script is
not available (or with --nightly python), and says so in every report. It does
what contract v1.5 section 3.2 asks of the nightly job, using the Python
reference analysis/py/ast_metrics.py for the numbers:

  * read channel 1 for the last N complete UTC days through feeds.json in
    windows of at most 4 days, splitting a window that comes back full (sec. 8)
  * one row per day (section 3.2 formats), created_at = run time + k seconds
  * one bulk update to channel 2

  python3 nightly_standin.py --base http://127.0.0.1:18766 --ch1 1001 --rk1 READ \
      --ch2 1002 --wk2 WRITE --now 2026-11-23T00:20:00Z [--days 7]
"""
import argparse
import json
import os
import sys
import urllib.parse
import urllib.request
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "analysis", "py"))
import ast_metrics as am  # noqa: E402

MAX_RESULTS = 8000
WINDOW_S = 4 * 86400


def ts(t):
    return datetime.fromtimestamp(t, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def read_range(base, ch, key, start, end):
    """All feeds with start <= created_at <= end (unix seconds, inclusive)."""
    feeds, requests = [], 0
    queue = [(s, min(s + WINDOW_S, end + 1) - 1) for s in range(start, end + 1, WINDOW_S)]
    while queue:
        a, b = queue.pop(0)
        q = {"start": ts(a), "end": ts(b), "timezone": "Etc/UTC", "status": "true", "results": str(MAX_RESULTS)}
        if key:
            q["api_key"] = key
        url = "%s/channels/%s/feeds.json?%s" % (base, ch, urllib.parse.urlencode(q, quote_via=urllib.parse.quote))
        with urllib.request.urlopen(url, timeout=30) as resp:
            text = resp.read().decode("utf-8")
        requests += 1
        if text.strip() == "-1":
            raise SystemExit("read key rejected for channel %s" % ch)
        got = json.loads(text)["feeds"]
        if len(got) >= MAX_RESULTS and b - a > 120:
            mid = a + (b - a) // 2
            queue[:0] = [(a, mid), (mid + 1, b)]
            continue
        feeds.extend(got)
    return feeds, requests


def main(argv=None):
    ap = argparse.ArgumentParser(description="Python STAND-IN for ast_nightly.m (tests only)")
    ap.add_argument("--base", required=True)
    ap.add_argument("--ch1", required=True)
    ap.add_argument("--rk1", default="")
    ap.add_argument("--ch2", required=True)
    ap.add_argument("--wk2", required=True)
    ap.add_argument("--now", required=True, help="ISO-8601 UTC time of the run")
    ap.add_argument("--days", type=int, default=7)
    a = ap.parse_args(argv)
    now = int(datetime.fromisoformat(a.now.replace("Z", "+00:00")).timestamp())
    today0 = now // 86400 * 86400
    feeds, nreq = read_range(a.base.rstrip("/"), a.ch1, a.rk1, today0 - a.days * 86400, today0 - 1)
    rows = am.ch2_rows(am.parse_feeds(feeds), now, a.days)
    body = json.dumps({"write_api_key": a.wk2, "updates": rows}).encode()
    req = urllib.request.Request("%s/channels/%s/bulk_update.json" % (a.base.rstrip("/"), a.ch2), data=body,
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=30) as resp:
        ok = resp.status in (200, 202) and '"success":true' in resp.read().decode().replace(" ", "")
    print("PYTHON STAND-IN nightly: %d entries read in %d requests, %d rows written to channel %s: %s"
          % (len(feeds), nreq, len(rows), a.ch2, "ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
