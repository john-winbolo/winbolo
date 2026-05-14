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

# --scenario <name> runs exactly one scenario (used by CTest, one entry per
# scenario). Without it the script runs every scenario with section headers
# for manual use.
SCENARIO=""
if [ "${1:-}" = "--scenario" ]; then
  SCENARIO="$2"
  shift 2
fi

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

# Two WinBoloHeadless --server clients connected to one WinBoloDS,
# each reading its own -cmd-stdin and writing its own log-events.
# The DS runs with -nolobby so the game is in RUNNING phase from
# tick 0 — the headless cmd-stdin pump in --server mode keys off
# the last observed server tick, which only advances once game
# snapshots flow, so lobby-mode scenarios would deadlock. Diffs
# each client's capture against its own baseline, sorted to absorb
# wire ordering jitter.
run_events_cmd_udp_two_clients() {
  local name="$1"
  local map="$2"
  local c1_cmd="$3"
  local c2_cmd="$4"
  local port=50005
  echo -n "  $name ... "

  "$BIN_DS" -map "$map" -port "$port" -gametype open -nolobby \
            -nowinbolonet -quiet -threads 1 \
            -logfile "$ACTUAL/$name.dslog" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  sleep 0.5

  # Client 1 first, then a brief delay so it lands in slot 0
  # deterministically before client 2 joins into slot 1. Distinct
  # --name args so the server doesn't reject c2 as a duplicate.
  "$BIN" --server 127.0.0.1 --port "$port" --name HeadlessBot1 \
         --cmd-stdin "$c1_cmd" \
         --ticks 500 --seed 42 \
         --log-events "$ACTUAL/${name}_c1.jsonl" --quiet \
         > "$ACTUAL/${name}_c1.out" 2> "$ACTUAL/${name}_c1.err" &
  local c1_pid=$!
  sleep 0.3
  "$BIN" --server 127.0.0.1 --port "$port" --name HeadlessBot2 \
         --cmd-stdin "$c2_cmd" \
         --ticks 500 --seed 43 \
         --log-events "$ACTUAL/${name}_c2.jsonl" --quiet \
         > "$ACTUAL/${name}_c2.out" 2> "$ACTUAL/${name}_c2.err" &
  local c2_pid=$!

  trap 'kill "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true; \
        wait "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true' EXIT

  local c1_rc=0 c2_rc=0
  wait "$c1_pid" || c1_rc=$?
  wait "$c2_pid" || c2_rc=$?

  kill "$ds_pid" 2>/dev/null || true
  wait "$ds_pid" 2>/dev/null || true
  trap - EXIT

  if [ "$c1_rc" -ne 0 ] || [ "$c2_rc" -ne 0 ]; then
    echo "CRASH (c1=$c1_rc c2=$c2_rc)"
    return 1
  fi

  local fail=0
  for which in c1 c2; do
    if diff_sorted "$EXPECTED/${name}_${which}.jsonl" \
                   "$ACTUAL/${name}_${which}.jsonl" >/dev/null 2>&1; then
      :
    else
      [ "$fail" -eq 0 ] && echo "DIFF"
      diff_sorted "$EXPECTED/${name}_${which}.jsonl" \
                  "$ACTUAL/${name}_${which}.jsonl" 2>&1 | head -40
      fail=1
    fi
  done
  if [ "$fail" -eq 0 ]; then
    echo "OK"
    return 0
  fi
  return 1
}

# Two passive WinBoloHeadless --server clients connected to a WinBoloDS
# that is configured (via -ticklimit) to end the running game at a fixed
# tick. The clients use a brain (no cmd-stdin) so they sit idle; their
# event logs capture the GAME_OVER family of events as the server-driven
# game-end fires. Server uses -nolobby for the same tick-pump-reference
# reason as run_events_cmd_udp_two_clients.
run_events_udp_two_clients_ticklimit() {
  local name="$1"
  local map="$2"
  local brain="$3"
  local ticklimit="$4"
  local port=50006
  echo -n "  $name ... "

  "$BIN_DS" -map "$map" -port "$port" -gametype open -nolobby \
            -ticklimit "$ticklimit" \
            -nowinbolonet -quiet -threads 1 \
            -logfile "$ACTUAL/$name.dslog" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  sleep 0.5

  "$BIN" --server 127.0.0.1 --port "$port" --name HeadlessBot1 \
         --brain "$brain" \
         --ticks 500 --seed 42 \
         --log-events "$ACTUAL/${name}_c1.jsonl" --quiet \
         > "$ACTUAL/${name}_c1.out" 2> "$ACTUAL/${name}_c1.err" &
  local c1_pid=$!
  sleep 0.3
  "$BIN" --server 127.0.0.1 --port "$port" --name HeadlessBot2 \
         --brain "$brain" \
         --ticks 500 --seed 43 \
         --log-events "$ACTUAL/${name}_c2.jsonl" --quiet \
         > "$ACTUAL/${name}_c2.out" 2> "$ACTUAL/${name}_c2.err" &
  local c2_pid=$!

  trap 'kill "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true; \
        wait "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true' EXIT

  local c1_rc=0 c2_rc=0
  wait "$c1_pid" || c1_rc=$?
  wait "$c2_pid" || c2_rc=$?

  kill "$ds_pid" 2>/dev/null || true
  wait "$ds_pid" 2>/dev/null || true
  trap - EXIT

  if [ "$c1_rc" -ne 0 ] || [ "$c2_rc" -ne 0 ]; then
    echo "CRASH (c1=$c1_rc c2=$c2_rc)"
    return 1
  fi

  local fail=0
  for which in c1 c2; do
    if diff_sorted "$EXPECTED/${name}_${which}.jsonl" \
                   "$ACTUAL/${name}_${which}.jsonl" >/dev/null 2>&1; then
      :
    else
      [ "$fail" -eq 0 ] && echo "DIFF"
      diff_sorted "$EXPECTED/${name}_${which}.jsonl" \
                  "$ACTUAL/${name}_${which}.jsonl" 2>&1 | head -40
      fail=1
    fi
  done
  if [ "$fail" -eq 0 ]; then
    echo "OK"
    return 0
  fi
  return 1
}

# Scenario name → helper invocation. The set of names here must stay in
# sync with CMakeLists.txt's baseline.${name} CTest entries.
EVERARD_MAP="$MAPS/Everard Island.map"
FOREST_MAP="$MAPS/Forest Rig.map"
SLUGFEST_MAP="$MAPS/Slugfest IV.map"

dispatch_scenario() {
  local name="$1"
  case "$name" in
    everard_island_1bot_idle)  run "$name" "$EVERARD_MAP"  "$BRAINS/idle.lua"          ;;
    everard_island_1bot_sit)   run "$name" "$EVERARD_MAP"  "$BRAINS/sit_and_log.lua"   ;;
    everard_island_1bot_drive) run "$name" "$EVERARD_MAP"  "$BRAINS/drive_forward.lua" ;;
    everard_island_1bot_shoot) run "$name" "$EVERARD_MAP"  "$BRAINS/shoot_and_log.lua" ;;
    everard_island_1bot_watch) run "$name" "$EVERARD_MAP"  "$BRAINS/watch_objects.lua" ;;
    forest_rig_1bot_idle)      run "$name" "$FOREST_MAP"   "$BRAINS/idle.lua"          ;;
    forest_rig_1bot_sit)       run "$name" "$FOREST_MAP"   "$BRAINS/sit_and_log.lua"   ;;
    forest_rig_1bot_drive)     run "$name" "$FOREST_MAP"   "$BRAINS/drive_forward.lua" ;;
    forest_rig_1bot_shoot)     run "$name" "$FOREST_MAP"   "$BRAINS/shoot_and_log.lua" ;;
    forest_rig_1bot_watch)     run "$name" "$FOREST_MAP"   "$BRAINS/watch_objects.lua" ;;
    slugfest_iv_1bot_idle)     run "$name" "$SLUGFEST_MAP" "$BRAINS/idle.lua"          ;;
    slugfest_iv_1bot_sit)      run "$name" "$SLUGFEST_MAP" "$BRAINS/sit_and_log.lua"   ;;
    slugfest_iv_1bot_drive)    run "$name" "$SLUGFEST_MAP" "$BRAINS/drive_forward.lua" ;;
    slugfest_iv_1bot_shoot)    run "$name" "$SLUGFEST_MAP" "$BRAINS/shoot_and_log.lua" ;;
    slugfest_iv_1bot_watch)    run "$name" "$SLUGFEST_MAP" "$BRAINS/watch_objects.lua" ;;

    ds_4bot_melee)             run_ds "$name" 4 ""  ;;
    ds_2v2_team)               run_ds "$name" 4 "1" ;;

    centralize_events_fast)    run_events_fast "$name" "$EVERARD_MAP" "$BRAINS/sit_and_log.lua" ;;
    centralize_events_udp)     run_events_udp  "$name" "$EVERARD_MAP" "$BRAINS/sit_and_log.lua" ;;

    centralize_events_teams_fast)
      run_events_cmd_fast "$name" "$EVERARD_MAP" \
                          "$COMMANDS/centralize_events_teams.client.jsonl" ;;
    centralize_events_alliance_fast)
      run_events_cmd_fast "$name" "$EVERARD_MAP" \
                          "$COMMANDS/centralize_events_alliance.client.jsonl" ;;
    centralize_events_name_change_fast)
      run_events_cmd_fast "$name" "$EVERARD_MAP" \
                          "$COMMANDS/centralize_events_name_change.client.jsonl" ;;
    centralize_events_map_skip_fast)
      run_events_cmd_fast "$name" "$EVERARD_MAP" \
                          "$COMMANDS/centralize_events_map_skip.client.jsonl" ;;
    centralize_events_name_change_udp)
      run_events_cmd_udp "$name" "$EVERARD_MAP" \
                         "$COMMANDS/centralize_events_name_change.client.jsonl" ;;
    centralize_events_map_skip_udp)
      run_events_cmd_udp "$name" "$EVERARD_MAP" \
                         "$COMMANDS/centralize_events_map_skip.client.jsonl" ;;
    centralize_events_alliance_leave_udp)
      run_events_cmd_udp "$name" "$EVERARD_MAP" \
                         "$COMMANDS/centralize_events_alliance_leave.client.jsonl" ;;
    centralize_events_shutdown_udp)
      run_events_cmd_udp_server_only "$name" "$EVERARD_MAP" \
                         "$COMMANDS/centralize_events_shutdown.server.jsonl" ;;
    centralize_events_alliance_2client_udp)
      run_events_cmd_udp_two_clients "$name" "$EVERARD_MAP" \
                         "$COMMANDS/centralize_events_alliance_2client.c1.jsonl" \
                         "$COMMANDS/centralize_events_alliance_2client.c2.jsonl" ;;
    centralize_events_game_over_udp)
      run_events_udp_two_clients_ticklimit "$name" "$EVERARD_MAP" \
                         "$BRAINS/sit_and_log.lua" 200 ;;

    *) echo "unknown scenario: $name" >&2; return 2 ;;
  esac
}

# CTest path: dispatch one scenario and propagate its exit code.
if [ -n "$SCENARIO" ]; then
  rc=0
  dispatch_scenario "$SCENARIO" || rc=$?
  exit $rc
fi

# Manual path: every scenario, grouped under section headers.
fail=0

echo "Everard Island:"
for n in everard_island_1bot_idle everard_island_1bot_sit \
         everard_island_1bot_drive everard_island_1bot_shoot \
         everard_island_1bot_watch; do
  dispatch_scenario "$n" || fail=1
done

echo "Forest Rig:"
for n in forest_rig_1bot_idle forest_rig_1bot_sit \
         forest_rig_1bot_drive forest_rig_1bot_shoot \
         forest_rig_1bot_watch; do
  dispatch_scenario "$n" || fail=1
done

echo "Slugfest IV:"
for n in slugfest_iv_1bot_idle slugfest_iv_1bot_sit \
         slugfest_iv_1bot_drive slugfest_iv_1bot_shoot \
         slugfest_iv_1bot_watch; do
  dispatch_scenario "$n" || fail=1
done

echo "Dedicated server (Everard Island):"
dispatch_scenario ds_4bot_melee || fail=1
dispatch_scenario ds_2v2_team   || fail=1

echo "Control-event capture (Everard Island):"
dispatch_scenario centralize_events_fast || fail=1
dispatch_scenario centralize_events_udp  || fail=1

echo "Scripted control-event scenarios (Everard Island):"
for n in centralize_events_teams_fast \
         centralize_events_alliance_fast \
         centralize_events_name_change_fast \
         centralize_events_map_skip_fast \
         centralize_events_name_change_udp \
         centralize_events_map_skip_udp \
         centralize_events_alliance_leave_udp \
         centralize_events_shutdown_udp \
         centralize_events_alliance_2client_udp \
         centralize_events_game_over_udp; do
  dispatch_scenario "$n" || fail=1
done

exit $fail
