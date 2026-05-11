#!/usr/bin/env bash
# Single-bot regression-test harness.
#
# Runs winboloheadless against a fixed set of brain scenarios across several
# maps, captures the --log-state JSON output, and diffs it against committed
# expected files. Exits 0 if every scenario matches its expected output, 1
# otherwise.
#
# Usage:
#   tests/baseline/run.sh                        # uses build-linux/winboloheadless
#   tests/baseline/run.sh /path/to/winboloheadless

set -e

BIN="${1:-$HOME/linux-build/WinBoloHeadless}"
DIR="$(cd "$(dirname "$0")" && pwd)"
BRAINS="$(cd "$DIR/../brains" && pwd)"
MAPS="$DIR/maps"
EXPECTED="$DIR/expected"
ACTUAL="$DIR/actual"

mkdir -p "$ACTUAL"

run() {
  local name="$1"
  local map="$2"
  local brain="$3"
  echo -n "  $name ... "
  "$BIN" --fast --map "$map" --brain "$brain" \
      --ticks 500 --seed 42 \
      --log-state "$ACTUAL/$name.json" --quiet \
      > "$ACTUAL/$name.stdout" 2>&1 || { echo "CRASH"; return 1; }
  if diff -q "$EXPECTED/$name.json" "$ACTUAL/$name.json" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff -u "$EXPECTED/$name.json" "$ACTUAL/$name.json" 2>&1 | head -40
    return 1
  fi
}

run_all_brains() {
  local tag="$1"
  local map="$2"
  if [ ! -f "$map" ]; then
    echo "  ERROR: missing map $map" >&2
    fail=1
    return 0
  fi
  run "${tag}_1bot_idle"  "$map" "$BRAINS/idle.lua"            || fail=1
  run "${tag}_1bot_sit"   "$map" "$BRAINS/sit_and_log.lua"     || fail=1
  run "${tag}_1bot_drive" "$map" "$BRAINS/drive_forward.lua"   || fail=1
  run "${tag}_1bot_shoot" "$map" "$BRAINS/shoot_and_log.lua"   || fail=1
  run "${tag}_1bot_watch" "$map" "$BRAINS/watch_objects.lua"   || fail=1
}

fail=0

echo "Everard Island:"
run_all_brains "everard_island" "$MAPS/Everard Island.map"

echo "Forest Rig:"
run_all_brains "forest_rig"     "$MAPS/Forest Rig.map"

echo "Slugfest IV:"
run_all_brains "slugfest_iv"    "$MAPS/Slugfest IV.map"

exit $fail
