#!/usr/bin/env bash
# The portable core (firmware/core) is the single source. arduino-cli only
# compiles files inside the sketch folder, so an exact copy is kept in
# firmware/ast_datalink/src/core. NEVER edit the copy.
#   firmware/sync_core.sh           copy core -> sketch
#   firmware/sync_core.sh --check   exit 1 if the copy differs from the core
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/core"
DST="$HERE/ast_datalink/src/core"
if [ "${1:-}" = "--check" ]; then
  if diff -rq -x README.txt "$SRC" "$DST" >/dev/null 2>&1; then echo "core copy is identical"; exit 0; fi
  echo "ERROR: $DST differs from $SRC - run firmware/sync_core.sh"; diff -rq -x README.txt "$SRC" "$DST" || true; exit 1
fi
mkdir -p "$DST"
# Remove files that no longer exist in the core (":?" stops the script if DST were empty).
for f in "${DST:?}"/*.h "${DST:?}"/*.cpp; do
  [ -e "$f" ] || continue
  [ -e "$SRC/$(basename "$f")" ] || rm -f -- "$f"
done
cp "$SRC"/*.h "$SRC"/*.cpp "$DST"/
cat > "$DST/README.txt" <<'TXT'
GENERATED COPY of firmware/core (made by firmware/sync_core.sh).
Do not edit these files: edit firmware/core and run the script again.
firmware/build.sh checks that both directories are identical before compiling.
TXT
echo "copied $(ls "$SRC" | wc -l) files to $DST"
