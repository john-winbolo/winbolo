#!/usr/bin/env bash
# Water-stuck regression test (GoalHunter escape_water deadlock).
#
# Repro history: on river-maze maps (Wild Bleeding Chickens), bots that lose
# their boat inside a walled water channel used to pin against a wall for
# 5-8 minutes: find_dry_land() picked the geometrically nearest dry tile with
# no reachability check, escape_water steering beelines at it with no A*, and
# the 3-second stuck handler's block was never consulted by find_dry_land, so
# the same unreachable tile was re-picked every tick. Fixed in GoalHunter_1.6
# (pathfinder.lua beeline_clear + blocked-list rotation, init.lua water-aware
# stuck threshold). This test guards that fix.
#
# Detection: a bot repeatedly logging "STUCK_BLOCK ... pos=(same) ...
# goal=escape_water" is pinned. With the water stuck threshold at 450 ticks
# (~9 s), MAX_REPEAT=5 means ~45+ seconds pinned at one tile => regression.
# The pre-fix runs logged 100-239 repeats; post-fix runs log 0.
#
# Usage:
#   tests/bots/water_stuck_regression.sh [WinBoloDS.exe] [duration_seconds]
#       Runs a live 6-bot game on Wild Bleeding Chickens (default binary
#       build/WinBoloDS.exe, default 360 s), then analyzes the recording.
#       Run from the repo root or pass an absolute binary path; the server
#       is started in the binary's directory so data/maps and Brains resolve.
#
#   tests/bots/water_stuck_regression.sh --analyze <debug_session_dir>
#       Skips the live run and analyzes an existing -brain-debug session
#       (any dir containing print2_bot*.log). Handy for triaging a
#       recording a community member sent in.
#
# Exit 0 = pass, 1 = regression detected, 2 = setup/usage error.
#
# CAUTION: live-run cleanup falls back to killing by image name
# (taskkill/pkill WinBoloDS), so don't run the live mode while an unrelated
# WinBoloDS server is running on this machine. --analyze mode is always safe.

set -u

MAX_REPEAT=5     # same-pos escape_water stuck events per bot before failing
MAX_TOTAL=15     # total escape_water stuck events per bot before failing

analyze() {
  local session="$1"
  local fail=0
  local found_logs=0
  shopt -s nullglob
  for f in "$session"/print2_bot*.log; do
    found_logs=1
    local bot
    bot=$(basename "$f" .log)
    local total worst worst_pos
    total=$(grep -c 'STUCK_BLOCK.*goal=escape_water' "$f" || true)
    if [ "$total" -gt 0 ]; then
      worst_line=$(grep 'STUCK_BLOCK' "$f" | grep 'goal=escape_water' \
        | sed -E 's/.*pos=\(([0-9]+,[0-9]+)\).*/\1/' \
        | sort | uniq -c | sort -rn | head -1)
      worst=$(echo "$worst_line" | awk '{print $1}')
      worst_pos=$(echo "$worst_line" | awk '{print $2}')
    else
      worst=0; worst_pos="-"
    fi
    echo "$bot: escape_water_stuck total=$total worst_same_pos=$worst @($worst_pos)"
    if [ "$worst" -ge "$MAX_REPEAT" ] || [ "$total" -ge "$MAX_TOTAL" ]; then
      echo "  FAIL: bot pinned in water (worst_same_pos=$worst >= $MAX_REPEAT or total=$total >= $MAX_TOTAL)"
      fail=1
    fi
  done
  # A crashed brain idles too — count any crash report as a failure.
  for c in "$session"/brain_crash_*.log; do
    echo "FAIL: brain crash report present: $(basename "$c")"
    fail=1
  done
  if [ "$found_logs" -eq 0 ]; then
    echo "ERROR: no print2_bot*.log in '$session' (need a -brain-debug session recorded with the base, non-opt brain)"
    return 2
  fi
  if [ "$fail" -eq 0 ]; then
    echo "PASS: no water-stuck pinning detected"
  fi
  return $fail
}

if [ "${1:-}" = "--analyze" ]; then
  [ -d "${2:-}" ] || { echo "usage: $0 --analyze <debug_session_dir>"; exit 2; }
  analyze "$2"
  exit $?
fi

BIN="${1:-build/WinBoloDS.exe}"
DURATION="${2:-360}"
[ -x "$BIN" ] || [ -f "$BIN" ] || { echo "ERROR: server binary '$BIN' not found"; exit 2; }
BIN_DIR=$(cd "$(dirname "$BIN")" && pwd)
BIN_NAME=$(basename "$BIN")
MAP="data/maps/Wild Bleeding Chickens.map"
[ -f "$BIN_DIR/$MAP" ] || { echo "ERROR: '$MAP' not found next to the binary"; exit 2; }

LABEL="waterstuck_$$"
echo "Running $BIN_NAME on Wild Bleeding Chickens for ${DURATION}s (6 bots, label $LABEL)..."
(
  cd "$BIN_DIR" || exit 2
  WINBOLO_BRAINDBG_LABEL="$LABEL" "./$BIN_NAME" \
    -map "$MAP" -port 27599 -gametype Open \
    -bots 6 -brain "Brains/GoalHunter_1.7/init.lua" -ai yes \
    -nolobby -notracker -brain-debug -bd-nopool -bd-noviz \
    > /dev/null 2>&1
) &
SRV=$!
sleep "$DURATION"
kill "$SRV" 2> /dev/null
# The subshell wraps the exe; kill the exe too if it survived.
taskkill //F //IM "$BIN_NAME" > /dev/null 2>&1 || pkill -f "$BIN_NAME" 2> /dev/null || true
sleep 2

SESSION=$(ls -td "$BIN_DIR"/debug_sessions/*_"$LABEL" 2>/dev/null | head -1)
[ -n "$SESSION" ] || { echo "ERROR: no debug session dir with label $LABEL found"; exit 2; }
echo "Analyzing $SESSION"
analyze "$SESSION"
exit $?
