#!/usr/bin/env bash
# Regression-test harness.
#
# Single-bot section: runs winboloheadless against a fixed set of brain
# scenarios across several maps, captures --log-state JSON, diffs vs expected.
#
# Multi-bot section: runs winbolods (dedicated server) with N bots driven by
# tests/brains/sit_and_log.lua, captures the brain's stdout, diffs vs expected.
#
# Exits 0 if every scenario matches its expected output, 1 otherwise.
#
# Usage:
#   tests/baseline/run.sh                                       # defaults below
#   tests/baseline/run.sh /path/to/winboloheadless
#   tests/baseline/run.sh /path/to/winboloheadless /path/to/winbolods
#
# argv[2] (dedicated-server binary) defaults to a same-directory neighbor of
# argv[1] (WinBoloDS / winbolods, matching argv[1]'s case).

set -e

BIN="${1:-$HOME/linux-build/WinBoloHeadless}"
case "$(basename "$BIN")" in
  winboloheadless) BIN_DS_DEFAULT="$(dirname "$BIN")/winbolods" ;;
  *)               BIN_DS_DEFAULT="$(dirname "$BIN")/WinBoloDS" ;;
esac
BIN_DS="${2:-$BIN_DS_DEFAULT}"
DIR="$(cd "$(dirname "$0")" && pwd)"
BRAINS="$(cd "$DIR/../brains" && pwd)"
MAPS="$DIR/maps"
EXPECTED="$DIR/expected"
ACTUAL="$DIR/actual"

mkdir -p "$ACTUAL"

COMMANDS="$DIR/commands"

# Diff two JSONL files after normalizing UDP wall-clock jitter:
# every line has its tick field stripped before the lexical sort, so
# a CTRL_MAP_SKIP_STATE that lands two ticks earlier or later than
# the golden run still matches. The set of events (and their order
# within a tick, modulo lex sort) is the regression target — the
# precise tick is run-to-run noise on the UDP path. Used only by
# UDP scenarios; --fast scenarios diff unsorted/unstripped because
# the in-process pipe is fully deterministic.
diff_sorted() {
  local expected="$1"
  local actual="$2"
  diff -u \
    <(sed -E 's/"tick":[0-9]+,//' "$expected" | sort) \
    <(sed -E 's/"tick":[0-9]+,//' "$actual"   | sort)
}

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

run_ds() {
  local name="$1"
  local bots="$2"
  local ally="$3"
  echo -n "  $name ... "
  local args=( -map "$MAPS/Everard Island.map" -port 50001 -nolobby
               -gametype open
               -bots "$bots" -brain "$BRAINS/sit_and_log.lua"
               -seed 42 -ticks 500
               -nowinbolonet -quiet -threads 1
               -logfile "$ACTUAL/$name.log" )
  if [ -n "$ally" ]; then
    args+=( -allybots "$ally" )
  fi
  "$BIN_DS" "${args[@]}" \
      > "$ACTUAL/$name.out" 2> "$ACTUAL/$name.err" || { echo "CRASH"; return 1; }
  if diff -q "$EXPECTED/$name.out" "$ACTUAL/$name.out" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff -u "$EXPECTED/$name.out" "$ACTUAL/$name.out" 2>&1 | head -40
    return 1
  fi
}

# Run WinBoloHeadless --fast with --log-events; diff the resulting JSONL.
run_events_fast() {
  local name="$1"
  local map="$2"
  local brain="$3"
  echo -n "  $name ... "
  "$BIN" --fast --map "$map" --brain "$brain" \
      --ticks 500 --seed 42 \
      --log-events "$ACTUAL/$name.jsonl" --quiet \
      > "$ACTUAL/$name.out" 2> "$ACTUAL/$name.err" || { echo "CRASH"; return 1; }
  if diff -q "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff -u "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" 2>&1 | head -40
    return 1
  fi
}

# Run WinBoloDS in the background and connect WinBoloHeadless --server to it,
# capturing the headless's --log-events stream. The DS is unlimited (-ticks
# omitted) and torn down via an EXIT trap so a crash in the headless still
# leaves no orphaned server. Port 50002 is chosen to avoid the run_ds 50001.
run_events_udp() {
  local name="$1"
  local map="$2"
  local brain="$3"
  local port=50002
  echo -n "  $name ... "

  "$BIN_DS" -map "$map" -port "$port" -nolobby \
            -gametype open \
            -bots 1 -brain "$brain" \
            -seed 42 \
            -nowinbolonet -quiet -threads 1 \
            -logfile "$ACTUAL/$name.dslog" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!

  # Best-effort cleanup: fire on every return path, including the OK case
  # below where we trap - EXIT before returning.
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  # Brief sleep for the UDP socket to bind. The headless's join retry
  # tolerates a slower startup, but a short delay avoids the first packet
  # going to an unbound port.
  sleep 0.5

  local rc=0
  "$BIN" --server 127.0.0.1 --port "$port" --brain "$brain" \
         --ticks 500 --seed 42 \
         --log-events "$ACTUAL/$name.jsonl" --quiet \
         > "$ACTUAL/$name.out" 2> "$ACTUAL/$name.err" || rc=$?

  kill "$ds_pid" 2>/dev/null || true
  wait "$ds_pid" 2>/dev/null || true
  trap - EXIT

  if [ "$rc" -ne 0 ]; then
    echo "CRASH"
    return 1
  fi
  if diff -q "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff -u "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" 2>&1 | head -40
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

echo "Dedicated server (Everard Island):"
run_ds "ds_4bot_melee" 4 ""  || fail=1
run_ds "ds_2v2_team"   4 "1" || fail=1

echo "Control-event capture (Everard Island):"
run_events_fast "centralize_events_fast" \
                "$MAPS/Everard Island.map" \
                "$BRAINS/sit_and_log.lua" || fail=1
run_events_udp  "centralize_events_udp" \
                "$MAPS/Everard Island.map" \
                "$BRAINS/sit_and_log.lua" || fail=1

# ---------------------------------------------------------------
# Scripted control-event scenarios (--cmd-stdin / -cmd-stdin).
#
# Each scenario drives a deterministic command stream so we can
# gate the centralize migration on event variants that the
# brain-driven sit_and_log harness doesn't naturally fire.
#
# --fast helpers diff unsorted (the in-process pipe ordering is
# itself a regression target). UDP helpers diff sorted, neutralizing
# wire jitter.
# ---------------------------------------------------------------

# WinBoloHeadless --fast with --cmd-stdin. No brain — the cmd
# stream drives everything.
run_events_cmd_fast() {
  local name="$1"
  local map="$2"
  local cmd_file="$3"
  echo -n "  $name ... "
  "$BIN" --fast --map "$map" --cmd-stdin "$cmd_file" \
      --ticks 500 --seed 42 \
      --log-events "$ACTUAL/$name.jsonl" --quiet \
      > "$ACTUAL/$name.out" 2> "$ACTUAL/$name.err" || { echo "CRASH"; return 1; }
  if diff -q "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff -u "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" 2>&1 | head -40
    return 1
  fi
}

# WinBoloHeadless --server with --cmd-stdin, connected to a
# WinBoloDS instance. The server side may optionally take its
# own -cmd-stdin file (4th arg, "" for none). Compares sorted to
# absorb wire ordering jitter.
run_events_cmd_udp() {
  local name="$1"
  local map="$2"
  local client_cmd="$3"
  local server_cmd="${4:-}"
  local port=50003
  echo -n "  $name ... "

  local ds_args=( -map "$map" -port "$port" -gametype open
                  -nowinbolonet -quiet -threads 1
                  -logfile "$ACTUAL/$name.dslog" )
  if [ -n "$server_cmd" ]; then
    ds_args+=( -cmd-stdin "$server_cmd" )
  else
    ds_args+=( -nolobby )
  fi

  "$BIN_DS" "${ds_args[@]}" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  sleep 0.5

  local rc=0
  "$BIN" --server 127.0.0.1 --port "$port" \
         --cmd-stdin "$client_cmd" \
         --ticks 500 --seed 42 \
         --log-events "$ACTUAL/$name.jsonl" --quiet \
         > "$ACTUAL/$name.out" 2> "$ACTUAL/$name.err" || rc=$?

  kill "$ds_pid" 2>/dev/null || true
  wait "$ds_pid" 2>/dev/null || true
  trap - EXIT

  if [ "$rc" -ne 0 ]; then
    echo "CRASH"
    return 1
  fi
  if diff_sorted "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff_sorted "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" 2>&1 | head -40
    return 1
  fi
}

# UDP-only scenario where the server runs without a client cmd
# stream and shuts itself down via its own -cmd-stdin. The
# headless just listens for CTRL_SERVER_SHUTDOWN; --ticks gives
# it a generous wall-clock budget to receive the packet before
# its own loop exits.
run_events_cmd_udp_server_only() {
  local name="$1"
  local map="$2"
  local server_cmd="$3"
  local port=50004
  echo -n "  $name ... "

  "$BIN_DS" -map "$map" -port "$port" -gametype open \
            -nolobby \
            -cmd-stdin "$server_cmd" \
            -nowinbolonet -quiet -threads 1 \
            -logfile "$ACTUAL/$name.dslog" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  sleep 0.5

  local rc=0
  "$BIN" --server 127.0.0.1 --port "$port" \
         --ticks 500 --seed 42 \
         --log-events "$ACTUAL/$name.jsonl" --quiet \
         > "$ACTUAL/$name.out" 2> "$ACTUAL/$name.err" || rc=$?

  kill "$ds_pid" 2>/dev/null || true
  wait "$ds_pid" 2>/dev/null || true
  trap - EXIT

  if [ "$rc" -ne 0 ]; then
    echo "CRASH"
    return 1
  fi
  if diff_sorted "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff_sorted "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" 2>&1 | head -40
    return 1
  fi
}

echo "Scripted control-event scenarios (Everard Island):"
run_events_cmd_fast "centralize_events_teams_fast" \
                    "$MAPS/Everard Island.map" \
                    "$COMMANDS/centralize_events_teams.client.jsonl" || fail=1
run_events_cmd_fast "centralize_events_alliance_fast" \
                    "$MAPS/Everard Island.map" \
                    "$COMMANDS/centralize_events_alliance.client.jsonl" || fail=1
run_events_cmd_fast "centralize_events_name_change_fast" \
                    "$MAPS/Everard Island.map" \
                    "$COMMANDS/centralize_events_name_change.client.jsonl" || fail=1
run_events_cmd_fast "centralize_events_map_skip_fast" \
                    "$MAPS/Everard Island.map" \
                    "$COMMANDS/centralize_events_map_skip.client.jsonl" || fail=1
run_events_cmd_udp  "centralize_events_name_change_udp" \
                    "$MAPS/Everard Island.map" \
                    "$COMMANDS/centralize_events_name_change.client.jsonl" || fail=1
run_events_cmd_udp  "centralize_events_map_skip_udp" \
                    "$MAPS/Everard Island.map" \
                    "$COMMANDS/centralize_events_map_skip.client.jsonl" || fail=1
run_events_cmd_udp  "centralize_events_alliance_leave_udp" \
                    "$MAPS/Everard Island.map" \
                    "$COMMANDS/centralize_events_alliance_leave.client.jsonl" || fail=1
run_events_cmd_udp_server_only \
                    "centralize_events_shutdown_udp" \
                    "$MAPS/Everard Island.map" \
                    "$COMMANDS/centralize_events_shutdown.server.jsonl" || fail=1

exit $fail
