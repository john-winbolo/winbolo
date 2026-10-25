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

# ── Launching the binaries ─────────────────────────────────────────
# Start every DS and headless through these two, never through "$BIN_DS"
# or "$BIN" directly, so the -nocrashreporting below cannot be forgotten
# at a new call site.
#
# Without it, sentryInit (src/common/sentry_integration.c) opens a crash
# database at SDL_GetPrefPath("WinBolo","WinBolo")/.sentry-native whenever
# the build carries a SENTRY_DSN. That is ONE directory per user, shared by
# the DS and every client of every scenario at once, and shared again with
# every other worktree and every concurrent run on the machine — the one
# piece of machine-global state the harness still touched after the ports
# went ephemeral. It also costs a crash-handler install per process and, on
# a crash, network I/O inside the scenario's 60 s budget.
#
# sentryInit scans argv for the flag itself, before either binary parses its
# arguments. WinBoloDS ignores an argument it does not recognise; WinBoloHeadless
# rejects one, so its parser accepts -nocrashreporting and does nothing with it
# (headless_main.c). Inert on a build with no DSN, harmless everywhere else.
#
# Both wrappers exec once they are already running in a child of the script,
# which is where `ds_bin ... &` puts them. Without that, the pid a launch
# records is the wrapper's own subshell and not the game process. The subshell
# lives exactly as long as its child, so waiting on it still returns the right
# status and nothing looks wrong — but every kill in this file then lands on
# the wrapper and leaves the server or client it meant to stop running. A
# scenario whose DS does not stop itself leaks one per run, which a suite
# multiplies into a machine full of servers holding ports: the opposite of
# what the rest of this work is for.
#
# $BASH_SUBSHELL is 0 at the top level of the script and greater than 0 in
# any subshell, which includes a backgrounded `ds_bin ... &`. So this execs for
# every backgrounded launch and runs the binary as an ordinary child for the
# foreground ones, where exec would replace the harness itself.
#
# Not $BASHPID: that variable arrived in bash 4.0, and macOS ships 3.2 as
# /bin/bash, which is what `bash run.sh` under ctest resolves to there. On 3.2
# it expands to nothing, the test is always true, and every foreground launch
# execs the harness away — the scenario then "passes" with whatever status
# the binary exits with, no diff ever runs, and the DS a single-client UDP
# scenario started is orphaned. $BASH_SUBSHELL has been in bash since 3.0.
ds_bin() {
  if [ "${BASH_SUBSHELL:-0}" -gt 0 ]; then exec "$BIN_DS" "$@" -nocrashreporting; fi
  "$BIN_DS" "$@" -nocrashreporting
}
headless_bin() {
  if [ "${BASH_SUBSHELL:-0}" -gt 0 ]; then exec "$BIN" "$@" -nocrashreporting; fi
  "$BIN" "$@" -nocrashreporting
}

DIR="$(cd "$(dirname "$0")" && pwd)"
BRAINS="$(cd "$DIR/../brains" && pwd)"
MAPS="$DIR/maps"
EXPECTED="$DIR/expected"
# Captures go to a private directory, not straight to tests/baseline/actual.
# Two runs of this suite at once — a second ctest, or a developer running
# alongside CI — otherwise write the same $name.jsonl and $name.ds.err and
# overwrite each other's output mid-diff, which fails scenarios in both runs
# for no reason connected to the code. Per-run ports fixed the servers
# colliding; this fixes their output colliding.
#
# The directory is copied to tests/baseline/actual/ on the way out, so the
# post-mortem convention still holds. Last writer wins there, which costs
# nothing: every diff has already been taken against the private copy.
ACTUAL_PUBLISH="$DIR/actual"
ACTUAL="$(mktemp -d "${TMPDIR:-/tmp}/wb-baseline.XXXXXX")"

publish_actual() {
  mkdir -p "$ACTUAL_PUBLISH"
  cp -Rf "$ACTUAL"/. "$ACTUAL_PUBLISH"/ 2>/dev/null || true
  rm -rf "$ACTUAL"
}

mkdir -p "$ACTUAL"

COMMANDS="$DIR/commands"

# Fields that aren't part of the regression target and need to be
# neutralized before any baseline diff:
#   tick        — UDP wall-clock jitter shifts events by a few ticks.
#   pingMs      — RTT varies by a millisecond between runs.
#   countryCode — single-player and DS sessions now stamp the local
#                 player's country (default "XX", or the resolved WBN
#                 country code on machines that have made a fetch),
#                 so the value depends on where the test is running.
#                 The bare "country" field carried by CTRL_PLAYER_JOIN
#                 and CTRL_PLAYER_LEAVE has the same problem.
#   clientType  — bolo_detect_client_type() reports the compile-time
#                 platform of the headless client (Windows=1, Linux=2,
#                 macOS=3), so the value depends on where the test is
#                 built. The fixtures were recorded on Linux (=2); the
#                 runner's platform isn't a regression target.
NORMALIZE_EVENTS_SED='s/"tick":[0-9]+,//; s/"pingMs":[0-9]+/"pingMs":0/; s/"countryCode":"[^"]*"/"countryCode":"??"/g; s/"country":"[^"]*"/"country":"??"/g; s/"clientType":[0-9]+/"clientType":2/g'

# Windows text streams write CRLF; committed fixtures use LF. Compare all
# other bytes as before. JSON strings encode carriage returns as escapes.
diff_text() {
  local mode="$1"
  diff "$mode" <(tr -d '\r' < "$2") <(tr -d '\r' < "$3")
}

# Diff two JSONL files after the field normalization above, with a
# lexical sort. sort -u collapses duplicate (untickled) lines because
# lobby-mode scenarios broadcast the lobby state on a wall-clock
# cadence, so the multiplicity of identical lobby-state lines varies
# run-to-run as the pump-tick fallback crosses different positions in
# the broadcast cycle. Non-lobby UDP scenarios produce one line per
# event so sort -u is equivalent to sort there. Used by UDP scenarios.
diff_sorted() {
  local expected="$1"
  local actual="$2"
  diff -u \
    <(sed -E "$NORMALIZE_EVENTS_SED" "$expected" | sort -u) \
    <(sed -E "$NORMALIZE_EVENTS_SED" "$actual"   | sort -u)
}

# Diff two JSONL files after field normalization but with no sort —
# the line ordering itself is a regression target. Used by --fast
# scenarios (in-process pipe is fully deterministic) and single-bot
# UDP scenarios where the wire order is also stable.
diff_norm() {
  local expected="$1"
  local actual="$2"
  diff -u \
    <(sed -E "$NORMALIZE_EVENTS_SED" "$expected") \
    <(sed -E "$NORMALIZE_EVENTS_SED" "$actual")
}

# Two-client lobby scenarios race at teardown in three ways that are not
# the scenario's regression target (join + rename roster state is):
#   1. Client disconnect — CTRL_PLAYER_LEAVE, its "<name> has left."
#      CTRL_SERVER_TEXT, and the CTRL_ALLIANCE_LEAVE a departure publishes
#      (serverSimRemovePlayer). Whether one client logs the other's leave
#      before it is itself killed varies run-to-run. Dedicated leave /
#      shutdown scenarios cover disconnect events deterministically. These
#      scenarios ask for no alliance change, so every alliance leave in
#      them is a departure's.
#   2. The rename auto-unready transient — a "ready":false CTRL_LOBBY_SLOT
#      broadcast emitted between the rename and the client re-readying.
#      Whether it lands in the capture window before teardown is racy;
#      sort -u can't fold it because it differs from the "ready":true
#      line only in the ready flag.
#   3. The game-end lobby-return settings — when a teardown disconnect ends
#      a running game, the server broadcasts a CTRL_LOBBY_SETTINGS carrying
#      netStat:3 + hasLobby:true (back-to-lobby). Whether it lands in the
#      capture window is racy; it can't fold because it differs in fields
#      from the deterministic lobby settings (netStat:0) and the running
#      settings (hasLobby:false).
# Drop the leave/has-left/alliance-leave and game-end-return settings lines and fold ready
# to a constant so the transients collapse, leaving the deterministic
# join/rename roster.
LOBBY_TEARDOWN_SED='/"type":"CTRL_PLAYER_LEAVE"/d; /"type":"CTRL_ALLIANCE_LEAVE"/d; /"type":"CTRL_SERVER_TEXT","text":"[^"]*has left/d; /"type":"CTRL_LOBBY_SETTINGS".*"netStat":3,"hasLobby":true/d; s/"ready":(true|false)/"ready":false/g'

diff_sorted_lobby() {
  local expected="$1"
  local actual="$2"
  diff -u \
    <(sed -E "$NORMALIZE_EVENTS_SED" "$expected" | sed -E "$LOBBY_TEARDOWN_SED" | sort -u) \
    <(sed -E "$NORMALIZE_EVENTS_SED" "$actual"   | sed -E "$LOBBY_TEARDOWN_SED" | sort -u)
}

# ── Server port ────────────────────────────────────────────────────
# Every UDP scenario launches its DS with `-port 0` and reads back the port
# the OS actually gave it, which the DS prints on stderr as
# "[UDP SERVER] listening on UDP port N" (transport_udp_server.c).
#
# This is why there is no fixed-port bookkeeping here any more. Fixed ports
# meant two copies of the suite contended for the same nine numbers, an
# interrupted run left a DS squatting one (its EXIT trap never fires when
# the harness is SIGKILLed), and a unit test's loopback harness binding
# 127.0.0.1:0 could be handed one out from under a scenario about to start.
# All three were the same bug — a port this harness does not control — and
# a port nobody else can name cannot be taken by any of them. The reaper
# that used to kill stale DSs by name-plus-port, and the RESOURCE_LOCK
# groups in CMakeLists.txt that serialised scenarios sharing a port, both
# went with it.

# Wait for a just-launched DS to report the port it bound, and echo it.
# $1 = the DS's captured stderr, $2 = its pid.
#
# This doubles as the readiness check: the line is printed after bind(), so
# seeing it means the socket is up and there is no need to sleep and hope.
# Fails if the DS dies or never prints one, which is what a bad map, a
# missing brain or any other startup failure looks like from here.
#
# Readiness is not the whole story, though, which is why every caller still
# sleeps a beat after this returns. The port line is printed from bind(),
# which is early: the spectator ring, the upload config and the rest of
# serverInstanceStartup still follow it. These captures are event-ordered and
# were recorded against a client that connected a beat after the server
# settled, so keep that beat. Without it the alliance scenarios pick up an
# extra CTRL_ALLIANCE_LEAVE.
await_ds_port() {
  local errfile="$1"
  local ds_pid="$2"
  local waited=0
  local p=""
  while [ "$waited" -lt 200 ]; do
    p=$(sed -n 's/.*listening on UDP port \([0-9][0-9]*\).*/\1/p' \
            "$errfile" 2>/dev/null | head -1)
    if [ -n "$p" ]; then
      echo "$p"
      return 0
    fi
    kill -0 "$ds_pid" 2>/dev/null || break
    sleep 0.05
    waited=$((waited + 1))
  done
  echo "DS FAILED TO START (never reported a listening port)" >&2
  return 1
}

# How long a scenario waits on one of its clients before calling it hung. Only
# has to be shorter than the CTest timeout (60s); a healthy client finishes in
# a few seconds. Override for a quicker check.
CLIENT_WAIT_LIMIT="${CLIENT_WAIT_LIMIT:-40}"

# Wait for a scenario client to exit, but not forever.
#
# A client that stops making progress otherwise parks the scenario in `wait`
# until CTest's timeout kills the whole tree, and that failure arrives with no
# output whatsoever: the scenario name has been echoed without a newline, so
# the partial line dies in the buffer, and the log says neither which process
# stopped nor where. Kill it at the deadline and name it instead. Returns 124,
# the usual timed-out status, so the caller's CRASH line carries it.
await_client() {
  local pid="$1"
  local label="$2"
  local waited=0
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$waited" -ge "$CLIENT_WAIT_LIMIT" ]; then
      echo
      echo "  HUNG: $label did not exit within ${CLIENT_WAIT_LIMIT}s; killing it"
      kill -9 "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      return 124
    fi
    sleep 1
    waited=$((waited + 1))
  done
  wait "$pid"
}

run() {
  local name="$1"
  local map="$2"
  local brain="$3"
  echo -n "  $name ... "
  headless_bin --fast --map "$map" --brain "$brain" \
      --ticks 500 --seed 42 \
      --log-state "$ACTUAL/$name.json" --quiet \
      > "$ACTUAL/$name.stdout" 2>&1 || { echo "CRASH"; return 1; }
  if diff_text -q "$EXPECTED/$name.json" "$ACTUAL/$name.json" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff_text -u "$EXPECTED/$name.json" "$ACTUAL/$name.json" 2>&1 | head -40
    return 1
  fi
}

# Run WinBoloHeadless --fast with --log-changes and diff the JSONL byte for
# byte. Arguments: name, map, brain, game type, game ticks, a record flag
# (when non-empty the run is also recorded to $ACTUAL/$name.wbv) and a
# terrain flag (when non-empty each record also names the map squares whose
# terrain moved since the last one). The terrain flag is off by default: it
# adds a field, and the goldens captured before it existed are byte for byte
# what they were without it.
run_changes() {
  local name="$1"
  local map="$2"
  local brain="$3"
  local gametype="$4"
  local ticks="$5"
  local record="$6"
  local terrain="$7"
  echo -n "  $name ... "
  local args=( --fast --map "$map" --brain "$brain" --gametype "$gametype"
               --ticks "$ticks" --seed 42
               --log-changes "$ACTUAL/$name.jsonl" --quiet )
  if [ -n "$record" ]; then
    args+=( --record "$ACTUAL/$name.wbv" )
  fi
  if [ -n "$terrain" ]; then
    args+=( --log-terrain )
  fi
  headless_bin "${args[@]}" \
      > "$ACTUAL/$name.stdout" 2>&1 || { echo "CRASH"; return 1; }
  if diff_text -q "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff_text -u "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" 2>&1 | head -40
    return 1
  fi
}

run_ds() {
  local name="$1"
  local bots="$2"
  local ally="$3"
  echo -n "  $name ... "
  local args=( -map "$MAPS/Everard Island.map" -port 0 -nolobby
               -gametype open
               -bots "$bots" -brain "$BRAINS/sit_and_log.lua"
               -seed 42 -ticks 500
               -nowinbolonet -quiet -threads 1
               -logfile "$ACTUAL/$name.log" )
  if [ -n "$ally" ]; then
    args+=( -allybots "$ally" )
  fi
  ds_bin "${args[@]}" \
      > "$ACTUAL/$name.out" 2> "$ACTUAL/$name.err" || {
        echo "CRASH"; return 1; }
  if diff_text -q "$EXPECTED/$name.out" "$ACTUAL/$name.out" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff_text -u "$EXPECTED/$name.out" "$ACTUAL/$name.out" 2>&1 | head -40
    return 1
  fi
}

# Run WinBoloHeadless --fast with --log-events; diff the resulting JSONL.
run_events_fast() {
  local name="$1"
  local map="$2"
  local brain="$3"
  echo -n "  $name ... "
  headless_bin --fast --map "$map" --brain "$brain" \
      --ticks 500 --seed 42 \
      --log-events "$ACTUAL/$name.jsonl" --quiet \
      > "$ACTUAL/$name.out" 2> "$ACTUAL/$name.err" || { echo "CRASH"; return 1; }
  if diff_norm "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff_norm "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" 2>&1 | head -40
    return 1
  fi
}

# Run WinBoloDS in the background and connect WinBoloHeadless --server to it,
# capturing the headless's --log-events stream. The DS is unlimited (-ticks
# omitted) and torn down via an EXIT trap so a crash in the headless still
# leaves no orphaned server. The port is whatever the OS gave the DS (await_ds_port).
run_events_udp() {
  local name="$1"
  local map="$2"
  local brain="$3"
  local port
  echo -n "  $name ... "

  ds_bin -map "$map" -port 0 -nolobby \
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

  port=$(await_ds_port "$ACTUAL/$name.ds.err" "$ds_pid") || return 1
  sleep 0.5  # settle; see await_ds_port

  local rc=0
  headless_bin --server 127.0.0.1 --port "$port" --brain "$brain" \
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
  if diff_norm "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff_norm "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" 2>&1 | head -40
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
  headless_bin --fast --map "$map" --cmd-stdin "$cmd_file" \
      --ticks 500 --seed 42 \
      --log-events "$ACTUAL/$name.jsonl" --quiet \
      > "$ACTUAL/$name.out" 2> "$ACTUAL/$name.err" || { echo "CRASH"; return 1; }
  if diff_norm "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" >/dev/null 2>&1; then
    echo "OK"
  else
    echo "DIFF"
    diff_norm "$EXPECTED/$name.jsonl" "$ACTUAL/$name.jsonl" 2>&1 | head -40
    return 1
  fi
}

# Run WinBoloHeadless --fast with a command stream and a scenario script
# beside the map, and look for the line the scenario ends the round with.
# Arguments: name, map, command file, bot brain, ticks, the line to find.
#
# The check is the round's own outcome rather than a recorded log. A scenario
# says what happened when it ends the round, so the line is the result; a run
# that stalls, or one whose waves never field, never says it. --bot-brain is
# what the held seats load when a wave fields one — not --brain, which is
# this process's own player and is left out here.
run_scenario_fast() {
  local name="$1"
  local map="$2"
  local cmd_file="$3"
  local brain="$4"
  local ticks="$5"
  local want="$6"
  echo -n "  $name ... "
  headless_bin --fast --map "$map" --cmd-stdin "$cmd_file" \
      --bot-brain "$brain" \
      --ticks "$ticks" --seed 42 \
      --log-events "$ACTUAL/$name.jsonl" --quiet \
      > "$ACTUAL/$name.out" 2> "$ACTUAL/$name.err" || { echo "CRASH"; return 1; }
  if grep -qF "$want" "$ACTUAL/$name.jsonl"; then
    echo "OK"
  else
    echo "NO END"
    echo "    the round never said: $want"
    # The attach line and any complaint about the script go to stderr; the
    # scenario's own console lines go to stdout.
    tail -10 "$ACTUAL/$name.err"
    tail -10 "$ACTUAL/$name.out"
    return 1
  fi
}

# The same scenario over the wire: one WinBoloHeadless --server against a
# WinBoloDS running the scripted map, checked by grep rather than by diff.
#
# Three things differ from every other UDP helper here, each for its own
# reason.
#
# The lobby stays live. The raiders are a team of held seats and the scenario's
# lobby template is what seats them, so -nolobby would leave the waves with
# nothing to field. The seats are taken before anyone joins and
# serverSimFindFreeSlot hands out the first free slot, so the held seats take
# the low slots and the one human lands above them.
#
# The client only readies. start_game is refused in --server mode, so the
# round is started by the lobby's own all-ready check: one ready human plus
# the template's seats, which are seated ready as every bot seat is.
#
# Nothing on a clock ends this run. The client will not: when the scenario
# ends the round its net status goes back to lobby and its game-tick counter
# stops, so --ticks is never reached and is only a ceiling that must not
# truncate the round. A scheduled server command will not either, at any
# value. The server's tick restarts at 0 when a round starts and again when
# the round hands back to the lobby — serverSimReturnToLobby runs the same
# world reset — while the command pump compares against that counter in every
# state. So a tick at or below the round's own end fires mid-round and cuts it
# short, and a tick above it is not reached until the returned lobby, which
# starts again from 0 and counts at 50/s against the round's 100/s: idling
# there for a number past the round's end costs more than the client wait
# below allows.
#
# So the run ends on the round's own word. The helper watches the client's
# event log for the line the scenario ends with, then tells the server to
# quit on its console, which publishes the shutdown the client leaves on.
#
# -ai yesfull is what lets a wave field a seat at all: the spawn arm refuses
# on a server that runs no bots, and the dedicated server's default is none.
# -brain is the DS spelling of what run_scenario_fast passes as --bot-brain —
# the scenario's lobby names no brain of its own, so the held seats fall
# back to the server's.
#
# Arguments: name, map, client command file, bot brain, client tick budget.
run_scenario_swap_udp() {
  local name="$1"
  local map="$2"
  local client_cmd="$3"
  local brain="$4"
  local ticks="$5"
  local port
  echo -n "  $name ... "

  # A held seat leaving the roster is a CTRL_LOBBY_SLOT for one of the low
  # slots carrying connected:false. The six seats hold 0-5, so the human's own
  # slot and the empty ones a join replay reports are outside this.
  local lost_seat='"playerNum":[0-5],"slot":[{]"connected":false'
  # The line the scenario ends the round with, which the wait below watches
  # for, and the line its last wave announces itself with, which is checked
  # after the run. Both are needed and neither covers the other.
  #
  # The end line alone would pass on a round that never had seats: next_wave
  # clamps what it asks for to the number of seats it found, so with no seats
  # it fields nothing, times each wave out, and reaches the same end. The wave
  # line is the scenario's own count of what it actually put on the field —
  # only spawns that returned true are counted — so the third wave naming six
  # says the template seated six, that all three waves ran, and that the seats
  # handed back after waves one and two came round again. That last part is
  # the swap this entry exists to exercise.
  local held="The keep held."
  local fielded="Wave 3 of 3: 6 raiders."
  # The overflow disconnect and the runner lines checked at the end are both
  # written through wb_log, which is silent unless WINBOLO_LOG names a
  # category. Without this those checks would be reading a file that could
  # never hold their lines, and would always pass. net=error carries the
  # overflow disconnect; sim=info carries what bot_manager.c says each time a
  # wave takes a seat's parked runner back or builds it a new one. The spec is
  # a comma-separated list of category=level pairs, applied left to right
  # (apply_env_log_spec in src/common/wb_log.c).
  #
  # Saved and put back rather than unset: this file is also run by hand, and a
  # developer who set WINBOLO_LOG for the rest of the run should still have
  # their own value after this launch.
  local had_log=0
  local old_log=""
  if [ -n "${WINBOLO_LOG+x}" ]; then
    had_log=1
    old_log="$WINBOLO_LOG"
  fi
  export WINBOLO_LOG="net=error,sim=info"

  # The server is given a live stdin and is not launched -quiet, which puts
  # processKeys on the branch that reads the console. That is the one road to
  # a clean stop that works on every platform this suite runs on. A SIGINT
  # sent from Git-bash to a native Windows process is never delivered, so the
  # interrupt flag the console loop breaks on is never set there. A "quit"
  # line breaks the same loop, and the shutdown that follows publishes
  # CTRL_SERVER_SHUTDOWN with its broadcast - which is what the client leaves
  # on, about a second later.
  #
  # The writer says nothing until the teardown below creates the sentinel
  # file, and holds the pipe open either way: were it to end first, the
  # server's reader thread would see EOF and no later line could reach it.
  # Its own bound is 600 turns of 0.2s, which is 120s and past every wait in
  # this helper. On a run that ends early - a NO END - the sentinel is never
  # written, so left alone the writer would sit in that loop for the rest of
  # the two minutes and then take a SIGPIPE on a line nobody is reading. The
  # teardown stops it by name instead, beside the server and the client.
  #
  # $! after a background pipeline is the last process in it, which is the
  # server, and ds_bin execs when it runs in a subshell - so this is the
  # server's own pid, not a shell wrapping it. The writer is the FIRST process
  # of that pipeline, and its pid is the one `jobs -p` reports for the job, so
  # the two are picked up separately. A shell that reports nothing leaves
  # w_pid empty, and kill walks past an empty argument to the pids after it.
  local quit_file="$ACTUAL/$name.quit"
  rm -f "$quit_file"
  { qw=0
    while [ ! -f "$quit_file" ] && [ "$qw" -lt 600 ]; do
      sleep 0.2
      qw=$((qw + 1))
    done
    echo quit
    sleep 5
  } | ds_bin -map "$map" -port 0 -gametype open \
            -ai yesfull -brain "$brain" \
            -nowinbolonet -threads 1 \
            -logfile "$ACTUAL/$name.dslog" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  local w_pid
  w_pid=$(jobs -p %% 2>/dev/null || true)
  if [ "$had_log" -eq 1 ]; then
    export WINBOLO_LOG="$old_log"
  else
    unset WINBOLO_LOG
  fi
  trap 'kill "$w_pid" "$ds_pid" 2>/dev/null || true; \
        wait "$w_pid" "$ds_pid" 2>/dev/null || true' EXIT

  port=$(await_ds_port "$ACTUAL/$name.ds.err" "$ds_pid") || return 1
  sleep 0.5  # settle; see await_ds_port

  headless_bin --server 127.0.0.1 --port "$port" \
         --cmd-stdin "$client_cmd" \
         --ticks "$ticks" --seed 42 \
         --log-events "$ACTUAL/$name.jsonl" --quiet \
         > "$ACTUAL/$name.out" 2> "$ACTUAL/$name.err" &
  local c_pid=$!
  trap 'kill "$w_pid" "$ds_pid" "$c_pid" 2>/dev/null || true; \
        wait "$w_pid" "$ds_pid" "$c_pid" 2>/dev/null || true' EXIT

  # Wait for the round to say it is over. The client flushes its event log
  # after every event, so the line lands there as soon as it is delivered.
  # Bounded, because a round that never ends must not sit here until CTest
  # kills the tree: a healthy run reaches this in about twenty seconds.
  local end_limit=35
  local waited=0
  local ended=0
  local gone=0
  while :; do
    if grep -qF "$held" "$ACTUAL/$name.jsonl" 2>/dev/null; then
      ended=1
      break
    fi
    # The client going early is the other way out of this loop; await_client
    # below reports it, as it reports any other way the client stops.
    if ! kill -0 "$c_pid" 2>/dev/null; then
      gone=1
      break
    fi
    if [ "$waited" -ge "$end_limit" ]; then
      break
    fi
    sleep 1
    waited=$((waited + 1))
  done

  if [ "$ended" -eq 0 ] && [ "$gone" -eq 0 ]; then
    kill "$w_pid" "$ds_pid" "$c_pid" 2>/dev/null || true
    wait "$w_pid" "$ds_pid" "$c_pid" 2>/dev/null || true
    trap - EXIT
    echo "NO END"
    echo "    the round never said: $held"
    tail -10 "$ACTUAL/$name.ds.err"
    tail -10 "$ACTUAL/$name.err"
    return 1
  fi

  if [ "$ended" -eq 1 ]; then
    # Ask the server to quit. The line goes down the pipe opened at launch,
    # the console loop breaks on it, and the shutdown that follows broadcasts
    # CTRL_SERVER_SHUTDOWN, which is what the client leaves on.
    : > "$quit_file"
    local quitWaited=0
    while kill -0 "$ds_pid" 2>/dev/null && [ "$quitWaited" -lt 15 ]; do
      sleep 1
      quitWaited=$((quitWaited + 1))
    done
    # Last resort, and SIGKILL rather than SIGTERM: a server still up after
    # that is one nothing gentler will stop either, and a SIGTERM that follows
    # a signal Windows never delivered is swallowed outright. The client then
    # leaves on the dead link instead, which is slower but still ends the run.
    # Every check this entry makes has already been written by the time either
    # road is taken - how the server stopped is teardown, not what is being
    # measured.
    kill -9 "$ds_pid" 2>/dev/null || true
  fi

  # Sized for the slower of the two roads out. A client leaving on the
  # shutdown broadcast takes about a second, and that is the road the quit
  # above takes. One leaving on a dead link instead waits
  # CLIENT_TIMEOUT_TICKS, which is 1000 of its own ticks and not a wall-clock
  # span: ten seconds on an idle machine, and considerably longer on a busy
  # one, because a headless sharing a machine with the rest of a -j run pumps
  # slower. 60s covers the first road many times over and gives the second a
  # chance, and this entry carries a CTest timeout of its own to fit it (see
  # the set_tests_properties beside its add_test). await_client reads this by
  # name when it is called, so the caller's local is the value it uses.
  local CLIENT_WAIT_LIMIT=60
  local rc=0
  await_client "$c_pid" "client" || rc=$?

  kill "$w_pid" "$ds_pid" 2>/dev/null || true
  wait "$w_pid" "$ds_pid" 2>/dev/null || true
  trap - EXIT

  if [ "$rc" -ne 0 ]; then
    echo "CRASH ($rc)"
    tail -10 "$ACTUAL/$name.ds.err"
    tail -10 "$ACTUAL/$name.err"
    return 1
  fi

  # Only the branch that drops a client for a full control channel. Three
  # other lines start the same way — the baseline and bulk resets sent around
  # a join and a map transfer — and they fire nowhere near a wave transition,
  # so matching them would fail this entry for something it does not measure.
  local overflow='control channel overflow for slot [0-9]+, deferring disconnect'
  if grep -qE "$overflow" "$ACTUAL/$name.ds.err"; then
    echo "OVERFLOW"
    echo "    the control channel filled and the server dropped a client"
    grep -nE "$overflow" "$ACTUAL/$name.ds.err" | head -5
    tail -10 "$ACTUAL/$name.ds.err"
    tail -10 "$ACTUAL/$name.err"
    return 1
  fi
  if grep -qE "$lost_seat" "$ACTUAL/$name.jsonl"; then
    echo "SEAT LOST"
    echo "    a held seat left the roster over the swap"
    grep -nE "$lost_seat" "$ACTUAL/$name.jsonl" | head -5
    tail -10 "$ACTUAL/$name.ds.err"
    tail -10 "$ACTUAL/$name.err"
    return 1
  fi
  # The poll is what waits for the end line, so there is no second grep for it
  # on the ordinary path. This only catches the other way out of that loop: a
  # client that stopped on its own before the round ended and stopped for a
  # reason await_client reads as clean — a server that died under it, say.
  # Without this the wave check below could pass on a run that got as far as
  # the third wave and no further.
  if [ "$ended" -eq 0 ]; then
    echo "NO END"
    echo "    the client stopped before the round said: $held"
    tail -10 "$ACTUAL/$name.ds.err"
    tail -10 "$ACTUAL/$name.err"
    return 1
  fi
  if ! grep -qF "$fielded" "$ACTUAL/$name.jsonl"; then
    echo "NO WAVE"
    echo "    the round ended, but never said: $fielded"
    # What it did say, which names the wave it got to and how many it put on
    # the field: none at all means the template seated no bots.
    grep -o "Wave [0-9]* of [0-9]*: [0-9]* raiders." "$ACTUAL/$name.jsonl" | head -5
    tail -10 "$ACTUAL/$name.ds.err"
    tail -10 "$ACTUAL/$name.err"
    return 1
  fi

  # Every fielding in this round has to take a seat's parked runner back
  # rather than build a new one, which is the work this entry is here to
  # watch. bot_manager.c writes one line per resume, at INFO in the sim
  # category, which is what the sim=info above turns on.
  #
  # Twelve of them in a healthy run: Wave Defense fields 2 raiders on wave
  # one, 4 on wave two and 6 on wave three (WAVES in the scenario, each
  # clamped to the six seats the lobby template holds), and the countdown
  # warms all six seats, so even a seat's first fielding finds a runner parked
  # for it. Fewer than that means some fielding went another way.
  local resume='botManager: bot [0-9]+ back on the field on its parked runner'
  local resumes
  resumes=$(grep -cE "$resume" "$ACTUAL/$name.ds.err" 2>/dev/null || true)
  if [ "$resumes" -lt 12 ]; then
    echo "NO RESUME"
    echo "    $resumes fieldings took a parked runner back; expected 12"
    grep -nE "$resume" "$ACTUAL/$name.ds.err" | head -5
    tail -10 "$ACTUAL/$name.ds.err"
    tail -10 "$ACTUAL/$name.err"
    return 1
  fi
  # And none of them built a runner. A Wave Defense spawn names no brain and
  # carries no init table, so every seat is fielded on exactly what its parked
  # runner was built with; a build line here is a fielding paying for a Lua VM
  # it did not need.
  local rebuilt='building a fresh runner|building its runner now'
  if grep -qE "$rebuilt" "$ACTUAL/$name.ds.err"; then
    echo "REBUILT"
    echo "    a fielding built a runner instead of taking the parked one"
    grep -nE "$rebuilt" "$ACTUAL/$name.ds.err" | head -5
    tail -10 "$ACTUAL/$name.ds.err"
    tail -10 "$ACTUAL/$name.err"
    return 1
  fi
  echo "OK ($resumes resumes)"
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
  local port
  echo -n "  $name ... "

  local ds_args=( -map "$map" -port 0 -gametype open
                  -nowinbolonet -quiet -threads 1
                  -logfile "$ACTUAL/$name.dslog" )
  if [ -n "$server_cmd" ]; then
    ds_args+=( -cmd-stdin "$server_cmd" )
  else
    ds_args+=( -nolobby )
  fi

  ds_bin "${ds_args[@]}" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  port=$(await_ds_port "$ACTUAL/$name.ds.err" "$ds_pid") || return 1
  sleep 0.5  # settle; see await_ds_port

  local rc=0
  headless_bin --server 127.0.0.1 --port "$port" \
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
  local port
  echo -n "  $name ... "

  ds_bin -map "$map" -port 0 -gametype open \
            -nolobby \
            -cmd-stdin "$server_cmd" \
            -nowinbolonet -quiet -threads 1 \
            -logfile "$ACTUAL/$name.dslog" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  port=$(await_ds_port "$ACTUAL/$name.ds.err" "$ds_pid") || return 1
  sleep 0.5  # settle; see await_ds_port

  local rc=0
  headless_bin --server 127.0.0.1 --port "$port" \
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
  local port
  echo -n "  $name ... "

  ds_bin -map "$map" -port 0 -gametype open -nolobby \
            -nowinbolonet -quiet -threads 1 \
            -logfile "$ACTUAL/$name.dslog" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  port=$(await_ds_port "$ACTUAL/$name.ds.err" "$ds_pid") || return 1
  sleep 0.5  # settle; see await_ds_port

  # Client 1 first, then a brief delay so it lands in slot 0
  # deterministically before client 2 joins into slot 1. Distinct
  # --name args so the server doesn't reject c2 as a duplicate.
  headless_bin --server 127.0.0.1 --port "$port" --name HeadlessBot1 \
         --cmd-stdin "$c1_cmd" \
         --ticks 500 --seed 42 \
         --log-events "$ACTUAL/${name}_c1.jsonl" --quiet \
         > "$ACTUAL/${name}_c1.out" 2> "$ACTUAL/${name}_c1.err" &
  local c1_pid=$!
  sleep 0.3
  headless_bin --server 127.0.0.1 --port "$port" --name HeadlessBot2 \
         --cmd-stdin "$c2_cmd" \
         --ticks 500 --seed 43 \
         --log-events "$ACTUAL/${name}_c2.jsonl" --quiet \
         > "$ACTUAL/${name}_c2.out" 2> "$ACTUAL/${name}_c2.err" &
  local c2_pid=$!

  trap 'kill "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true; \
        wait "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true' EXIT

  local c1_rc=0 c2_rc=0
  await_client "$c1_pid" "client 1" || c1_rc=$?
  await_client "$c2_pid" "client 2" || c2_rc=$?

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

# Two WinBoloHeadless --server clients connected to one WinBoloDS
# running with the lobby state machine live (no -nolobby). Each
# client drives its own cmd-stdin to set its team and ready in the
# lobby; the server auto-starts the countdown once all connected
# players are ready and naturally transitions to RUNNING when the
# countdown expires. Scheduled cmd-stdin ops fire against the
# headless's per-iteration pump-tick fallback while serverTick is
# still 0 (lobby phase, no game-state snapshots flowing).
run_events_cmd_udp_two_clients_lobby() {
  local name="$1"
  local map="$2"
  local c1_cmd="$3"
  local c2_cmd="$4"
  local port
  echo -n "  $name ... "

  ds_bin -map "$map" -port 0 -gametype open \
            -nowinbolonet -quiet -threads 1 \
            -logfile "$ACTUAL/$name.dslog" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  port=$(await_ds_port "$ACTUAL/$name.ds.err" "$ds_pid") || return 1
  sleep 0.5  # settle; see await_ds_port

  # Client 1 first, then a brief delay so it lands in slot 0
  # deterministically before client 2 joins into slot 1. Distinct
  # --name args so the server doesn't reject c2 as a duplicate.
  headless_bin --server 127.0.0.1 --port "$port" --name HeadlessBot1 \
         --cmd-stdin "$c1_cmd" \
         --ticks 500 --seed 42 \
         --log-events "$ACTUAL/${name}_c1.jsonl" --quiet \
         > "$ACTUAL/${name}_c1.out" 2> "$ACTUAL/${name}_c1.err" &
  local c1_pid=$!
  sleep 0.3
  headless_bin --server 127.0.0.1 --port "$port" --name HeadlessBot2 \
         --cmd-stdin "$c2_cmd" \
         --ticks 500 --seed 43 \
         --log-events "$ACTUAL/${name}_c2.jsonl" --quiet \
         > "$ACTUAL/${name}_c2.out" 2> "$ACTUAL/${name}_c2.err" &
  local c2_pid=$!

  trap 'kill "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true; \
        wait "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true' EXIT

  local c1_rc=0 c2_rc=0
  await_client "$c1_pid" "client 1" || c1_rc=$?
  await_client "$c2_pid" "client 2" || c2_rc=$?

  kill "$ds_pid" 2>/dev/null || true
  wait "$ds_pid" 2>/dev/null || true
  trap - EXIT

  if [ "$c1_rc" -ne 0 ] || [ "$c2_rc" -ne 0 ]; then
    echo "CRASH (c1=$c1_rc c2=$c2_rc)"
    return 1
  fi

  local fail=0
  for which in c1 c2; do
    if diff_sorted_lobby "$EXPECTED/${name}_${which}.jsonl" \
                         "$ACTUAL/${name}_${which}.jsonl" >/dev/null 2>&1; then
      :
    else
      [ "$fail" -eq 0 ] && echo "DIFF"
      diff_sorted_lobby "$EXPECTED/${name}_${which}.jsonl" \
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

# Three WinBoloHeadless --server clients connected to one WinBoloDS,
# each reading its own -cmd-stdin and writing its own log-events.
# Same shape as run_events_cmd_udp_two_clients but with one more
# join stagger so client 3 lands in slot 2 deterministically. The
# DS runs with -nolobby so the game is in RUNNING phase from tick 0.
# Used by the chat scenarios that need a passive third party to
# verify it never receives unicast messages addressed elsewhere.
run_events_cmd_udp_three_clients() {
  local name="$1"
  local map="$2"
  local c1_cmd="$3"
  local c2_cmd="$4"
  local c3_cmd="$5"
  local port
  echo -n "  $name ... "

  ds_bin -map "$map" -port 0 -gametype open -nolobby \
            -nowinbolonet -quiet -threads 1 \
            -logfile "$ACTUAL/$name.dslog" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  port=$(await_ds_port "$ACTUAL/$name.ds.err" "$ds_pid") || return 1
  sleep 0.5  # settle; see await_ds_port

  # Stagger joins by 0.3s each so slot assignment is deterministic
  # (c1→0, c2→1, c3→2). Distinct --name args so the server doesn't
  # reject a duplicate.
  headless_bin --server 127.0.0.1 --port "$port" --name HeadlessBot1 \
         --cmd-stdin "$c1_cmd" \
         --ticks 500 --seed 42 \
         --log-events "$ACTUAL/${name}_c1.jsonl" --quiet \
         > "$ACTUAL/${name}_c1.out" 2> "$ACTUAL/${name}_c1.err" &
  local c1_pid=$!
  sleep 0.3
  headless_bin --server 127.0.0.1 --port "$port" --name HeadlessBot2 \
         --cmd-stdin "$c2_cmd" \
         --ticks 500 --seed 43 \
         --log-events "$ACTUAL/${name}_c2.jsonl" --quiet \
         > "$ACTUAL/${name}_c2.out" 2> "$ACTUAL/${name}_c2.err" &
  local c2_pid=$!
  sleep 0.3
  headless_bin --server 127.0.0.1 --port "$port" --name HeadlessBot3 \
         --cmd-stdin "$c3_cmd" \
         --ticks 500 --seed 44 \
         --log-events "$ACTUAL/${name}_c3.jsonl" --quiet \
         > "$ACTUAL/${name}_c3.out" 2> "$ACTUAL/${name}_c3.err" &
  local c3_pid=$!

  trap 'kill "$ds_pid" "$c1_pid" "$c2_pid" "$c3_pid" 2>/dev/null || true; \
        wait "$ds_pid" "$c1_pid" "$c2_pid" "$c3_pid" 2>/dev/null || true' EXIT

  local c1_rc=0 c2_rc=0 c3_rc=0
  await_client "$c1_pid" "client 1" || c1_rc=$?
  await_client "$c2_pid" "client 2" || c2_rc=$?
  await_client "$c3_pid" "client 3" || c3_rc=$?

  kill "$ds_pid" 2>/dev/null || true
  wait "$ds_pid" 2>/dev/null || true
  trap - EXIT

  if [ "$c1_rc" -ne 0 ] || [ "$c2_rc" -ne 0 ] || [ "$c3_rc" -ne 0 ]; then
    echo "CRASH (c1=$c1_rc c2=$c2_rc c3=$c3_rc)"
    return 1
  fi

  local fail=0
  for which in c1 c2 c3; do
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
  local port
  echo -n "  $name ... "

  ds_bin -map "$map" -port 0 -gametype open -nolobby \
            -ticklimit "$ticklimit" \
            -nowinbolonet -quiet -threads 1 \
            -logfile "$ACTUAL/$name.dslog" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  port=$(await_ds_port "$ACTUAL/$name.ds.err" "$ds_pid") || return 1
  sleep 0.5  # settle; see await_ds_port

  headless_bin --server 127.0.0.1 --port "$port" --name HeadlessBot1 \
         --brain "$brain" \
         --ticks 500 --seed 42 \
         --log-events "$ACTUAL/${name}_c1.jsonl" --quiet \
         > "$ACTUAL/${name}_c1.out" 2> "$ACTUAL/${name}_c1.err" &
  local c1_pid=$!
  sleep 0.3
  headless_bin --server 127.0.0.1 --port "$port" --name HeadlessBot2 \
         --brain "$brain" \
         --ticks 500 --seed 43 \
         --log-events "$ACTUAL/${name}_c2.jsonl" --quiet \
         > "$ACTUAL/${name}_c2.out" 2> "$ACTUAL/${name}_c2.err" &
  local c2_pid=$!

  trap 'kill "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true; \
        wait "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true' EXIT

  local c1_rc=0 c2_rc=0
  await_client "$c1_pid" "client 1" || c1_rc=$?
  await_client "$c2_pid" "client 2" || c2_rc=$?

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

# One WinBoloDS and two WinBoloHeadless --server clients, both running the
# same brain and each diffed on what its own brain wrote to stdout. The brain
# decides from the square it spawned on whether to drive the road or to watch
# it, so one client takes the neutral items standing on it and the other only
# ever hears about them, which is the point: a capture in a --fast run never
# leaves the process, and here it has to cross the wire to be reported at all.
#
# The DS takes -seed 42, which the other multi-client helpers do not need. The
# start a joining tank is given is drawn from the sim's random stream
# (startsGetStartOpen), and on a map with one start to drive from and one to
# watch from, that draw decides which client does which. Joins are kept off
# that stream on purpose (serverNextConnId has its own), so the draw lands the
# same way every run.
#
# The brain writes a line only when something happens and never writes the
# same one twice, so neither capture file carries a tick or a count of ticks
# and the wire's timing cannot reach it. Nothing is sorted or normalized here.
run_captures_udp_two_clients() {
  local name="$1"
  local map="$2"
  local brain="$3"
  local ticks="$4"
  local port
  echo -n "  $name ... "

  ds_bin -map "$map" -port 0 -gametype open -nolobby \
            -seed 42 \
            -nowinbolonet -quiet -threads 1 \
            -logfile "$ACTUAL/$name.dslog" \
            > "$ACTUAL/$name.ds.out" 2> "$ACTUAL/$name.ds.err" &
  local ds_pid=$!
  trap 'kill "$ds_pid" 2>/dev/null || true; wait "$ds_pid" 2>/dev/null || true' EXIT

  port=$(await_ds_port "$ACTUAL/$name.ds.err" "$ds_pid") || return 1
  sleep 0.5  # settle; see await_ds_port

  # Client 1 first, then a brief delay so it lands in slot 0 deterministically
  # before client 2 joins into slot 1. Distinct --name args so the server
  # doesn't reject c2 as a duplicate.
  headless_bin --server 127.0.0.1 --port "$port" --name HeadlessBot1 \
         --brain "$brain" \
         --ticks "$ticks" --seed 42 --quiet \
         > "$ACTUAL/${name}_c1.out" 2> "$ACTUAL/${name}_c1.err" &
  local c1_pid=$!
  sleep 0.3
  headless_bin --server 127.0.0.1 --port "$port" --name HeadlessBot2 \
         --brain "$brain" \
         --ticks "$ticks" --seed 43 --quiet \
         > "$ACTUAL/${name}_c2.out" 2> "$ACTUAL/${name}_c2.err" &
  local c2_pid=$!

  trap 'kill "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true; \
        wait "$ds_pid" "$c1_pid" "$c2_pid" 2>/dev/null || true' EXIT

  local c1_rc=0 c2_rc=0
  await_client "$c1_pid" "client 1" || c1_rc=$?
  await_client "$c2_pid" "client 2" || c2_rc=$?

  kill "$ds_pid" 2>/dev/null || true
  wait "$ds_pid" 2>/dev/null || true
  trap - EXIT

  if [ "$c1_rc" -ne 0 ] || [ "$c2_rc" -ne 0 ]; then
    echo "CRASH (c1=$c1_rc c2=$c2_rc)"
    return 1
  fi

  local fail=0
  for which in c1 c2; do
    if diff_text -q "$EXPECTED/${name}_${which}.out" \
               "$ACTUAL/${name}_${which}.out" >/dev/null 2>&1; then
      :
    else
      [ "$fail" -eq 0 ] && echo "DIFF"
      diff_text -u "$EXPECTED/${name}_${which}.out" \
              "$ACTUAL/${name}_${which}.out" 2>&1 | head -40
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
ROAD_SPIT_MAP="$MAPS/Road Spit Minefield.map"
BOAT_BANK_MAP="$MAPS/Boat Bank.map"
BUILDER_YARD_MAP="$MAPS/Builder Yard.map"
BASE_YARD_MAP="$MAPS/Base Yard.map"
PILL_YARD_MAP="$MAPS/Pill Yard.map"
GRASS_FLAT_MAP="$MAPS/Grass Flat.map"
WATCH_ROAD_MAP="$MAPS/Watch Road.map"
# A copy of Slugfest IV with a scenario script beside it. The copy is what
# keeps the script off the Slugfest cases: a scenario is found by the map's
# own file name.
WAVE_DEFENSE_MAP="$MAPS/Wave Defense.map"

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

    # Tank deaths and respawns on the purpose-built spit: a parked tank
    # shelled to death by the neutral pill under each game type, a tank
    # that lays a mine and drives into the minefield, and a tank that
    # drives off the road into deep sea. Each tick budget covers the
    # death, the whole respawn wait and the respawn loadout with a margin
    # after it. The open shell run is also recorded; its .wbv is the
    # source of the committed tests/fixtures/wbv/road_spit_shell_open.wbv.
    road_spit_shell_open)
      run_changes "$name" "$ROAD_SPIT_MAP" "$BRAINS/park_in_pill_range.lua" open 1440 record ;;
    road_spit_shell_strict)
      run_changes "$name" "$ROAD_SPIT_MAP" "$BRAINS/park_in_pill_range.lua" strict 1440 "" ;;
    road_spit_shell_tournament)
      run_changes "$name" "$ROAD_SPIT_MAP" "$BRAINS/park_in_pill_range.lua" tournament 1440 "" ;;
    road_spit_mine_open)
      run_changes "$name" "$ROAD_SPIT_MAP" "$BRAINS/lay_mine_and_drive_over.lua" open 900 "" ;;
    road_spit_drown_open)
      run_changes "$name" "$ROAD_SPIT_MAP" "$BRAINS/drive_into_deep_sea.lua" open 560 "" ;;

    # Getting out of a boat and back into one, on the purpose-built Boat
    # Bank. The exit rule the engine applies turns on the speed the boat
    # reaches the land at, whatever the land is: road and grass alike only
    # land the tank at the boat's top speed, and below that speed the boat
    # is held a quarter square short of the bank instead, as in the
    # original WinBolo, WinBolo 1.17 and Mac Bolo. Entry is different:
    # driving onto a parked boat takes it at any speed and off any ground,
    # which the fast road run shows from the road and the grass run from
    # the grass. The tick budgets cover the last boat change in each run
    # plus the stop after it; the slow runs never land. Both slow runs
    # settle against the bank and log nothing more until their last tick:
    # the road run settles against the bank square 124 at tick 155, and
    # the grass run is cut shortly after it settles.
    # The fast road run is also recorded, and its .wbv is the source of the
    # committed tests/fixtures/wbv/boat_bank_road_fast.wbv: the summary's
    # terrain hash covers the boat moving from one map square to another,
    # which the change log has no field for.
    boat_bank_road_slow)
      run_changes "$name" "$BOAT_BANK_MAP" "$BRAINS/boat_exit_road_slow.lua" open 330 "" ;;
    boat_bank_road_fast)
      run_changes "$name" "$BOAT_BANK_MAP" "$BRAINS/boat_exit_road_fast.lua" open 260 record ;;
    boat_bank_grass_slow)
      run_changes "$name" "$BOAT_BANK_MAP" "$BRAINS/boat_exit_grass_slow.lua" open 240 "" ;;
    boat_bank_grass_fast)
      run_changes "$name" "$BOAT_BANK_MAP" "$BRAINS/boat_exit_grass_fast.lua" open 260 "" ;;

    # The builder at work on the purpose-built Builder Yard. Each run comes
    # ashore, stops on road square 122 and sends the man out to the work
    # squares in the apron beside it; the pillbox runs drive further east
    # first. What each job costs is charged when the order is accepted, not
    # when the man arrives, so the tank's stock moves a step ahead of the
    # square. These runs pass the terrain flag, because almost everything
    # the builder does is a change to the map and the other fields say
    # nothing about it: a road laid or a wall raised moves no stock beyond
    # the charge, and neither shows in the counts.
    #
    # The farm run is under strict rules, where a tank starts with no trees
    # at all, so each load the man carries home lands in the stock whole
    # instead of vanishing into the 40-tree cap.
    #
    # The tick budgets cover the last job of each run and the man's climb
    # back into the tank. The pill-placing run is also recorded, and its
    # .wbv is the source of the committed
    # tests/fixtures/wbv/builder_yard_pill_place.wbv: its summary carries
    # the moved pillbox and the terrain the run left behind.
    builder_yard_farm)
      run_changes "$name" "$BUILDER_YARD_MAP" "$BRAINS/farm_two_forest_squares.lua" strict 350 "" terrain ;;
    builder_yard_road)
      run_changes "$name" "$BUILDER_YARD_MAP" "$BRAINS/build_road_on_grass_and_swamp.lua" open 290 "" terrain ;;
    builder_yard_building)
      run_changes "$name" "$BUILDER_YARD_MAP" "$BRAINS/build_wall_then_repair_wall.lua" open 320 "" terrain ;;
    builder_yard_mine)
      run_changes "$name" "$BUILDER_YARD_MAP" "$BRAINS/lay_mine_beside_road.lua" open 220 "" terrain ;;
    builder_yard_pill_place)
      run_changes "$name" "$BUILDER_YARD_MAP" "$BRAINS/place_carried_pillbox.lua" open 290 record terrain ;;
    builder_yard_pill_repair)
      run_changes "$name" "$BUILDER_YARD_MAP" "$BRAINS/repair_own_pillbox.lua" open 350 "" terrain ;;

    # Bases on the purpose-built Base Yard, where three of them stand on one
    # road. What a base is worth to the tank that takes it turns on who held
    # it before: a neutral base hands its stocks over whole, and a base taken
    # from a live owner is emptied of all three the moment it changes hands.
    #
    # The capture run stops on the first base under open rules, where the
    # tank is already full, so the base has nothing to give and its stocks
    # stand still for a whole refuel interval after it changes hands.
    #
    # The refuel run is under strict rules, where a tank starts with no
    # shells and no mines at all. A base gives armour first, then shells,
    # then mines, and only moves on when the tank is full or the base is
    # out; the base it parks on holds three shells, so the run reaches the
    # mines without waiting out forty gives of shells.
    #
    # The steal run drives into the base at the end of the road, which is
    # solid until it is shelled under MIN_ARMOUR_CAPTURE, leans on it with
    # the trigger held and rolls in under the third shell.
    #
    # Both of those runs cross the mined road square, which is the only
    # reason either tank is short of armour, and both pass the terrain flag:
    # nothing else in the record says what became of that square, which
    # craters under the mine and then floods.
    #
    # The tick budgets cover the last thing each run does and the stop after
    # it; the capture run's covers a refuel interval on the base with nothing
    # to hand over.
    base_yard_capture)
      run_changes "$name" "$BASE_YARD_MAP" "$BRAINS/drive_onto_neutral_base.lua" open 250 "" ;;
    base_yard_refuel)
      run_changes "$name" "$BASE_YARD_MAP" "$BRAINS/refuel_on_neutral_base.lua" strict 490 "" terrain ;;
    base_yard_steal)
      run_changes "$name" "$BASE_YARD_MAP" "$BRAINS/shell_and_take_enemy_base.lua" open 430 "" terrain ;;

    # Pillboxes on the purpose-built Pill Yard. The capture run flattens the
    # neutral pill standing in the road and drives over it, which is how a
    # pill changes hands; it is shot at on the way in, because a pill with
    # armour left shoots at anything that is not its own.
    #
    # The anger run works on the pill the tank owns, which never shoots back.
    # Four shells halve its firing interval each time, from
    # PILLBOX_ATTACK_NORMAL down to the PILLBOX_MAX_FIRERATE floor, and the
    # run then sits still long enough to watch the interval climb back one
    # step at a time, PILLBOX_COOLDOWN_TIME apart. Its budget covers three of
    # those steps, which is where the cadence is established.
    pill_yard_capture)
      run_changes "$name" "$PILL_YARD_MAP" "$BRAINS/shell_and_take_neutral_pillbox.lua" open 400 "" ;;
    pill_yard_anger)
      run_changes "$name" "$PILL_YARD_MAP" "$BRAINS/shell_own_pillbox_four_times.lua" open 420 "" ;;

    # Tree growth on the purpose-built Grass Flat, where a tank idles offshore
    # and the only thing that moves in the whole run is one square of the
    # plain turning to forest. treeGrowUpdate runs once per tank per world
    # update: it samples one map square and takes one off the TREEGROW_TIME
    # countdown of 3000. The fast loop runs a server frame on its keys pass as
    # well as its game pass, so two of those updates land on every tick the
    # log counts and the countdown is 1500 ticks rather than 3000. The budget
    # covers the growth, the tick after it where the terrain field is gone
    # again, and a margin past that. The terrain flag is what makes the run
    # legible: a grown tree is a map square and nothing else, and the forest
    # count beside it only says how many there are.
    grass_flat_growth)
      run_changes "$name" "$GRASS_FLAT_MAP" "$BRAINS/idle.lua" open 3040 "" terrain ;;

    # The scenario beside Wave Defense.map, played to the end it writes for
    # itself. The lobby holds six seats for the raiders, the command stream
    # readies the one human and starts the round, and three waves go in on
    # timers. The raiders run the idle brain, so a wave ends on its own time
    # rather than on anything a bot does, which is what makes the run the same
    # every time.
    #
    # The scenario's timers add up to fourteen seconds, which is 1400 of the
    # simulation's ticks. The budget here is counted in game ticks, and the
    # loop runs a server frame on its keys pass as well as its game pass, so
    # it buys at least that many again.
    wave_defense_fast)
      run_scenario_fast "$name" "$WAVE_DEFENSE_MAP" \
                        "$COMMANDS/wave_defense.client.jsonl" \
                        "$BRAINS/idle.lua" 1800 "The keep held." ;;

    # The same three waves over the wire, which is where a wave transition
    # costs something a client can feel: each swap tears down six runners and
    # builds six more inside consecutive ticks, and each add and removal fans
    # control events out to every client. This entry fails if that drops a
    # client for a full control channel, if a seat is lost, if the round never
    # reaches its own end, or if the last wave did not field six raiders.
    #
    # The last two checks are both needed. The scenario ends the round the same
    # way whether or not it ever had seats — next_wave clamps what it asks
    # for to the seats it found, so with none it fields nothing, times each
    # wave out and still reaches the end. So the end line alone would pass on a
    # run where the lobby template seated nothing, which is the one failure
    # that would make this entry worthless. The wave line is the scenario's own
    # count of what it put on the field, and the third wave naming six says the
    # seats were there, that all three waves ran, and that the seats given back
    # after the first two came round again.
    #
    # Nothing here is on a clock, and that is deliberate. A server command
    # scheduled on a tick cannot end this run at any value. The server's
    # counter restarts at 0 when the round starts and again when the round
    # hands back to the lobby — serverSimReturnToLobby runs the same world
    # reset — and the command pump compares against it in every state. A tick
    # low enough to be reached lands inside the round and cuts it short; one
    # high enough to clear the round's own end is only reached in the returned
    # lobby, which starts again from 0 and counts at 50/s where the round
    # counted at 100/s, so waiting there costs more idle time than the client
    # wait allows. Both were tried and both failed that way.
    #
    # So the helper waits for the line the scenario ends with and then tells
    # the server to quit on its console, and the client leaves on the shutdown
    # that follows. The round decides the length of the run. A signal cannot
    # do that job here: on Windows MSYS turns the few signals it will deliver
    # to a native process into TerminateProcess and drops the rest, so a
    # SIGINT never sets the interrupt flag the console loop breaks on, and a
    # SIGTERM sent after one is swallowed as well. A "quit" line on stdin
    # breaks the same loop and needs no signal at all, so both platforms take
    # the same road and the client leaves the same way on each.
    #
    # End to end: about 3s for the client to ready, a 5s countdown, 14s of
    # scenario, and the shutdown round trip — roughly 27s.
    #
    # The worst case matters as much as the healthy one, because a run CTest
    # kills prints nothing: the entry's own line is half-written and still in
    # the buffer. The helper's waits are the whole of it — a 0.5s settle after
    # the server reports its port, then up to 35s for the round's end line,
    # then up to 15s for the quitting server to go, then up to 60s for the
    # client to leave. That is 110.5s at the outside, so even a run that hits
    # every bound reports its own failure with its logs inside the 150s the
    # entry is given.
    #
    # The client's --ticks is a ceiling, not the exit. The headless alternates
    # a keys pass and a game pass at GAME_TICK_LENGTH 10ms, so its game-tick
    # counter runs at 50/s and a 14s round is 700 of them. 2000 is 40s at that
    # rate, far past the round, so it is never the thing that stops the run.
    wave_swap_udp)
      run_scenario_swap_udp "$name" "$WAVE_DEFENSE_MAP" \
                        "$COMMANDS/wave_swap.client.jsonl" \
                        "$BRAINS/idle.lua" 2000 ;;

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
    centralize_events_lobby_smoke_udp)
      run_events_cmd_udp_two_clients_lobby "$name" "$EVERARD_MAP" \
                         "$COMMANDS/centralize_events_lobby_smoke.c1.jsonl" \
                         "$COMMANDS/centralize_events_lobby_smoke.c2.jsonl" ;;
    centralize_events_lobby_name_change_udp)
      run_events_cmd_udp_two_clients_lobby "$name" "$EVERARD_MAP" \
                         "$COMMANDS/centralize_events_lobby_name_change.c1.jsonl" \
                         "$COMMANDS/centralize_events_lobby_name_change.c2.jsonl" ;;
    centralize_events_game_over_udp)
      run_events_udp_two_clients_ticklimit "$name" "$EVERARD_MAP" \
                         "$BRAINS/sit_and_log.lua" 200 ;;
    centralize_events_chat_unicast_3client_udp)
      run_events_cmd_udp_three_clients "$name" "$EVERARD_MAP" \
                         "$COMMANDS/centralize_events_chat_unicast.c1.jsonl" \
                         "$COMMANDS/centralize_events_chat_unicast.c2.jsonl" \
                         "$COMMANDS/centralize_events_chat_unicast.c3.jsonl" ;;
    centralize_events_chat_alliance_3client_udp)
      run_events_cmd_udp_three_clients "$name" "$EVERARD_MAP" \
                         "$COMMANDS/centralize_events_chat_alliance.c1.jsonl" \
                         "$COMMANDS/centralize_events_chat_alliance.c2.jsonl" \
                         "$COMMANDS/centralize_events_chat_alliance.c3.jsonl" ;;

    # A pillbox and a base taken on the purpose-built Watch Road with a second
    # client watching, over a real socket. Every other capture scenario runs
    # --fast, where the sim the log reads is the one in the same process; this
    # one has the capture reach a client that did not make it. Both clients
    # report the same new owner from the events they were sent, and each
    # reports what the base on the map became from where it stands: the tank
    # that took it calls it friendly and the tank watching calls it hostile.
    # The budget covers the drive, both captures and the stop after them.
    watch_road_capture_2client_udp)
      run_captures_udp_two_clients "$name" "$WATCH_ROAD_MAP" \
                         "$BRAINS/take_pill_and_base_watched.lua" 400 ;;

    *) echo "unknown scenario: $name" >&2; return 2 ;;
  esac
}

# CTest path: dispatch one scenario and propagate its exit code.
if [ -n "$SCENARIO" ]; then
  rc=0
  dispatch_scenario "$SCENARIO" || rc=$?
  publish_actual
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

echo "Road Spit Minefield:"
for n in road_spit_shell_open road_spit_shell_strict \
         road_spit_shell_tournament road_spit_mine_open \
         road_spit_drown_open; do
  dispatch_scenario "$n" || fail=1
done

echo "Boat Bank:"
for n in boat_bank_road_slow boat_bank_road_fast \
         boat_bank_grass_slow boat_bank_grass_fast; do
  dispatch_scenario "$n" || fail=1
done

echo "Builder Yard:"
for n in builder_yard_farm builder_yard_road builder_yard_building \
         builder_yard_mine builder_yard_pill_place \
         builder_yard_pill_repair; do
  dispatch_scenario "$n" || fail=1
done

echo "Base Yard:"
for n in base_yard_capture base_yard_refuel base_yard_steal; do
  dispatch_scenario "$n" || fail=1
done

echo "Pill Yard:"
for n in pill_yard_capture pill_yard_anger; do
  dispatch_scenario "$n" || fail=1
done

echo "Grass Flat:"
dispatch_scenario grass_flat_growth || fail=1

echo "Wave Defense:"
dispatch_scenario wave_defense_fast || fail=1
dispatch_scenario wave_swap_udp    || fail=1

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
         centralize_events_lobby_smoke_udp \
         centralize_events_lobby_name_change_udp \
         centralize_events_game_over_udp \
         centralize_events_chat_unicast_3client_udp \
         centralize_events_chat_alliance_3client_udp; do
  dispatch_scenario "$n" || fail=1
done

echo "Capture over the wire (Watch Road):"
dispatch_scenario watch_road_capture_2client_udp || fail=1

publish_actual
exit $fail
