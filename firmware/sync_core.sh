#!/usr/bin/env bash
# The portable core (firmware/core) and the ESP32 data-link adapters
# (firmware/ast_datalink/src/esp_*) are the single sources. arduino-cli and
# PlatformIO only compile files inside a sketch folder, so exact copies are kept in
#   firmware/ast_datalink/src/core          (core only: the adapters live there)
#   firmware/tracker/src/datalink/core      (core)
#   firmware/tracker/src/datalink/esp_*     (adapters)
# NEVER edit the copies.
#   firmware/sync_core.sh           copy the sources into both sketches
#   firmware/sync_core.sh --check   exit 1 if any copy differs from its source
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CORE="$HERE/core"
ADAPT="$HERE/ast_datalink/src"
T_DL="$HERE/tracker/src/datalink"

note() {
  cat > "$1/README.txt" <<TXT
GENERATED COPY made by firmware/sync_core.sh from $2.
Do not edit these files: edit the source and run firmware/sync_core.sh again.
firmware/build.sh checks that the copies are identical before compiling.
TXT
}

if [ "${1:-}" = "--check" ]; then
  bad=0
  diff -rq -x README.txt "$CORE" "$HERE/ast_datalink/src/core" >/dev/null 2>&1 || { echo "ERROR: ast_datalink/src/core differs from core"; bad=1; }
  diff -rq -x README.txt "$CORE" "$T_DL/core" >/dev/null 2>&1 || { echo "ERROR: tracker/src/datalink/core differs from core"; bad=1; }
  for f in "$ADAPT"/esp_*.h "$ADAPT"/esp_*.cpp; do
    cmp -s "$f" "$T_DL/$(basename "$f")" || { echo "ERROR: tracker/src/datalink/$(basename "$f") differs from ast_datalink/src"; bad=1; }
  done
  for f in "$T_DL"/esp_*; do
    [ -e "$ADAPT/$(basename "$f")" ] || { echo "ERROR: tracker/src/datalink/$(basename "$f") has no source"; bad=1; }
  done
  if [ "$bad" = 0 ]; then echo "core and adapter copies are identical"; exit 0; fi
  echo "run firmware/sync_core.sh"; exit 1
fi

copy_core() {   # $1 = destination directory
  mkdir -p "$1"
  # Remove files that no longer exist in the core (":?" stops the script if the path were empty).
  for f in "${1:?}"/*.h "${1:?}"/*.cpp; do
    [ -e "$f" ] || continue
    [ -e "$CORE/$(basename "$f")" ] || rm -f -- "$f"
  done
  cp "$CORE"/*.h "$CORE"/*.cpp "$1"/
  note "$1" "firmware/core"
}

copy_core "$HERE/ast_datalink/src/core"
copy_core "$T_DL/core"
for f in "$T_DL"/esp_*; do
  [ -e "$f" ] || continue
  [ -e "$ADAPT/$(basename "$f")" ] || rm -f -- "$f"
done
cp "$ADAPT"/esp_*.h "$ADAPT"/esp_*.cpp "$T_DL"/
note "$T_DL" "firmware/ast_datalink/src/esp_* (adapters) and firmware/core (core/)"
echo "copied $(ls "$CORE" | wc -l) core files to ast_datalink and tracker, $(ls "$ADAPT"/esp_* | wc -l) adapter files to tracker"
