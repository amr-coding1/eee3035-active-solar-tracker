#!/usr/bin/env bash
# Smoke test: ast_host_device (the real firmware core, PC build) against the
# mock ThingSpeak server of contract section 10 (tools/mock_thingspeak.py).
# Skips (exit 0) when the mock or the build is not there yet.
#   firmware/host/smoke_mock.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DEV="$HERE/build/ast_host_device"
MOCK="$ROOT/tools/mock_thingspeak.py"
if [ ! -f "$MOCK" ]; then echo "SKIP: $MOCK not found (the mock server is not written yet)"; exit 0; fi
if [ ! -x "$DEV" ]; then echo "SKIP: $DEV not built (cmake -S firmware/host -B firmware/host/build && cmake --build firmware/host/build -j2)"; exit 0; fi
command -v python3 >/dev/null || { echo "SKIP: python3 not found"; exit 0; }

TMP="$(mktemp -d /tmp/ast_smoke.XXXXXX)"
MOCK_PID=""
cleanup() { [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

python3 "$MOCK" --port 0 --channel 1001:WRITEKEY1001:READKEY1001 >"$TMP/mock.out" 2>"$TMP/mock.err" &
MOCK_PID=$!
BASE=""
for _ in $(seq 1 50); do
  BASE="$(sed -n 's/^LISTENING \(http:[^ ]*\).*/\1/p' "$TMP/mock.out" | head -1)"
  [ -n "$BASE" ] && break
  sleep 0.1
done
if [ -z "$BASE" ]; then echo "FAIL: mock did not start"; cat "$TMP/mock.err"; exit 1; fi
echo "mock at $BASE"

fail=0
check() {  # check "description" actual expected
  if [ "$2" = "$3" ]; then echo "  ok   $1 ($2)"; else echo "  FAIL $1: got $2, expected $3"; fail=1; fi
}
count_entries() { python3 -c 'import json,sys,urllib.request
d=json.load(urllib.request.urlopen(sys.argv[1]+"/__admin/dump/1001"))
e=d.get("entries", d.get("feeds", d)) if isinstance(d, dict) else d
print(len(e))' "$BASE"; }
admin() { python3 -c 'import sys,urllib.request
urllib.request.urlopen(urllib.request.Request(sys.argv[1]+sys.argv[2], data=sys.argv[3].encode(), headers={"Content-Type":"application/json"})).read()' "$BASE" "$1" "$2"; }
events() { grep -c "\"ev\":\"$1\"" "$TMP/dev.jsonl" || true; }

COMMON=(--base "$BASE" --ch1 1001 --write-key WRITEKEY1001 --read-key READKEY1001 --state-dir "$TMP/state" --virtual --log "$TMP/dev.jsonl" --run 12 --panel A)

echo "1) 2 h clear day, uploads every 15 min"
"$DEV" "${COMMON[@]}" --start 2026-11-20T10:00:00Z --minutes 120 --scenario clear --seed 1 || fail=1
check "entries on the server (8 uploads x 15; the last 15 minutes wait for the next slot)" "$(count_entries)" 105
check "upload_ok events" "$(events upload_ok)" 7

echo "2) restart, lost response (in doubt) + read-back, then HTTP 500 twice"
admin /__admin/faults '{"mode":"drop_response","count":1}'
"$DEV" "${COMMON[@]}" --minutes 16 --scenario clear --seed 1 || fail=1
admin /__admin/faults '{"mode":"http500","count":2}'
"$DEV" "${COMMON[@]}" --minutes 44 --scenario clear --seed 1 --drain || fail=1
check "in_doubt events" "$(events in_doubt)" 1
check "entries on the server after drain (3 h = 180 samples, none duplicated thanks to the read-back)" "$(count_entries)" 180
check "upload_fail events (HTTP 500)" "$(events upload_fail)" 2

echo "3) simulated crash and restart: seq continues"
"$DEV" "${COMMON[@]}" --minutes 60 --crash-at 2026-11-20T13:20:30Z >/dev/null; rc=$?
check "exit code of the crashed run" "$rc" 3
"$DEV" "${COMMON[@]}" --minutes 40 --drain || fail=1
python3 - "$TMP/dev.jsonl" <<'PY' || fail=1
import json, sys
seqs = [json.loads(l)["seq"] for l in open(sys.argv[1]) if '"ev":"sample"' in l]
ok = seqs == list(range(1, len(seqs) + 1))
print("  %s seq of logged samples 1..%d consecutive, never reused" % ("ok  " if ok else "FAIL", len(seqs)))
sys.exit(0 if ok else 1)
PY

if [ "$fail" = 0 ]; then echo "SMOKE PASS"; else echo "SMOKE FAIL"; echo "--- device log tail"; tail -20 "$TMP/dev.jsonl"; fi
exit $fail
