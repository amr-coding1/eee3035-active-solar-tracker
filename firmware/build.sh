#!/usr/bin/env bash
# Builds everything and runs every automatic check of the firmware.
#   firmware/build.sh                         host tests (ASan/UBSan + TSan) and both ESP32 compiles
#   firmware/build.sh host                    host build and tests only
#   firmware/build.sh esp32                   ESP32 compile only (tracker + data-link-only sketch)
#   firmware/build.sh upload /dev/ttyACM0     compile and flash the TRACKER firmware (Windows: COM5)
#   firmware/build.sh upload-datalink <port>  compile and flash the data-link-only sketch (bench B1-B4)
# Machine note: 2 CPUs shared -> never more than -j2.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
WHAT="${1:-all}"
FQBN="esp32:esp32:nano_nora:PartitionScheme=spiffs"   # contract 7.1: scheme with SPIFFS/LittleFS space
BUILD_ROOT="${AST_BUILD_DIR:-/tmp/ast_build}"

host() {
  cmake -S "$HERE/host" -B "$HERE/host/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
  cmake --build "$HERE/host/build" -j2
  "$HERE/host/build/ast_tests"
  "$HERE/host/build/ast_tsan_test"
  "$HERE/host/build/ast_host_device" --selftest
  "$HERE/host/smoke_mock.sh"
}

compile_sketch() {   # $1 = sketch folder name
  LOG="$(mktemp /tmp/ast_compile.XXXXXX.log)"
  arduino-cli compile --fqbn "$FQBN" --warnings all --jobs 2 --build-path "$BUILD_ROOT/$1" "$HERE/$1" 2>&1 | tee "$LOG"
  # Fail on any warning that comes from our own files (not from the core).
  if grep -E "warning:" "$LOG" | grep -E "firmware/$1/|/sketch/" ; then echo "ERROR: warnings in our own files ($1)"; exit 1; fi
  echo "ESP32 compile ok ($1): $(arduino-cli core list | awk '$1=="esp32:esp32"{print $1"@"$2}'), $FQBN"
}

esp32() {
  "$HERE/sync_core.sh"
  "$HERE/sync_core.sh" --check
  arduino-cli core list | grep -q '^esp32:esp32 ' || { echo "esp32:esp32 core not installed (arduino-cli core install esp32:esp32@3.3.12)"; exit 1; }
  compile_sketch tracker
  compile_sketch ast_datalink
}

case "$WHAT" in
  host) host ;;
  esp32) esp32 ;;
  upload)
    PORT="${2:?usage: build.sh upload <serial port>}"
    "$HERE/sync_core.sh" && compile_sketch tracker
    arduino-cli upload --fqbn "$FQBN" --port "$PORT" --input-dir "$BUILD_ROOT/tracker" "$HERE/tracker" ;;
  upload-datalink)
    PORT="${2:?usage: build.sh upload-datalink <serial port>}"
    "$HERE/sync_core.sh" && compile_sketch ast_datalink
    arduino-cli upload --fqbn "$FQBN" --port "$PORT" --input-dir "$BUILD_ROOT/ast_datalink" "$HERE/ast_datalink" ;;
  all) host; esp32 ;;
  *) echo "usage: build.sh [all|host|esp32|upload <port>|upload-datalink <port>]"; exit 2 ;;
esac
