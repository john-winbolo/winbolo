#!/bin/bash
# Run a dedicated server on this Mac, launch WinBoloIOS in a simulator with a
# winbolo:// link to it so the app opens the join dialog for that server,
# keep the console for a number of seconds, take a screenshot, then stop the
# app and the server, and shut the simulator down if this script booted it.
#
# Usage: tools/ios/sim_join.sh [--device NAME] [--app PATH] [--ds PATH]
#                              [--port P] [--out DIR] [--seconds N]
#                              [-- extra WinBoloDS arguments]
#   SIM_UDID      simulator to use; otherwise --device NAME picks the first
#                 available one of that name, otherwise the first iPhone
#   WINBOLO_LOG   passed to the app (default '*=info')
#   --app         default build-ios-sim/WinBoloIOS.app
#   --ds          default cmake-build-debug/WinBoloDS
#   --port        the server's port (default 27512)
#   --out         where console.log, server.log and screenshot.png go;
#                 default a new directory under build-ios-sim/tool-runs/
#   --seconds     how long the app runs before the screenshot (default 30)
set -u
. "$(dirname "$0")/sim_common.sh"

usage() {
  sed -n '7,18p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

DEVICE=""
APP="$SIM_REPO_ROOT/build-ios-sim/WinBoloIOS.app"
DS="$SIM_REPO_ROOT/cmake-build-debug/WinBoloDS"
PORT=27512
OUT=""
SECS=30
while [ $# -gt 0 ]; do
  case "$1" in
    --device)  DEVICE=${2:-}; shift 2 ;;
    --app)     APP=${2:-}; shift 2 ;;
    --ds)      DS=${2:-}; shift 2 ;;
    --port)    PORT=${2:-}; shift 2 ;;
    --out)     OUT=${2:-}; shift 2 ;;
    --seconds) SECS=${2:-}; shift 2 ;;
    --) shift; break ;;
    -h|--help) usage ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
done

[ -d "$APP" ] || { echo "no app at $APP" >&2; exit 1; }
# A relative --ds is taken from where the script was run, since the server is
# started from inside the output directory.
case "$DS" in /*) ;; *) DS="$PWD/$DS" ;; esac
[ -x "$DS" ] || { echo "no WinBoloDS at $DS" >&2; exit 1; }
UDID=$(sim_pick_udid "$DEVICE") || { echo "no simulator found${DEVICE:+ called '$DEVICE'}" >&2; exit 1; }
OUT=$(sim_out_dir "$OUT" sim_join) || { echo "cannot create the output directory" >&2; exit 1; }
LOG_SPEC=${WINBOLO_LOG:-'*=info'}

DSPID=""
LPID=""
cleanup() {
  if [ -n "$LPID" ]; then
    xcrun simctl terminate "$UDID" "$SIM_BUNDLE_ID" >/dev/null 2>&1
    kill "$LPID" 2>/dev/null
    wait "$LPID" 2>/dev/null
    LPID=""
  fi
  sim_shutdown "$UDID"
  if [ -n "$DSPID" ]; then
    kill "$DSPID" 2>/dev/null
    wait "$DSPID" 2>/dev/null
    DSPID=""
  fi
}
trap cleanup EXIT INT TERM

echo "--- server on port $PORT, output in $OUT"
# Run from the output directory, so anything the server writes lands there.
(cd "$OUT" && exec "$DS" -inbuilt -port "$PORT" -gametype Open \
   -nowinbolonet -notracker -quiet "$@") > "$OUT/server.log" 2>&1 &
DSPID=$!
sleep 3
if ! kill -0 "$DSPID" 2>/dev/null; then
  echo "the server exited early:" >&2
  head -20 "$OUT/server.log" >&2
  DSPID=""
  exit 1
fi

echo "--- simulator $UDID"
sim_boot "$UDID"
xcrun simctl install "$UDID" "$APP" || exit 1
echo "--- launch with winbolo://127.0.0.1:$PORT (console for ${SECS}s)"
SIMCTL_CHILD_WINBOLO_LOG="$LOG_SPEC" \
  xcrun simctl launch --console "$UDID" "$SIM_BUNDLE_ID" \
  "winbolo://127.0.0.1:$PORT" > "$OUT/console.log" 2>&1 &
LPID=$!
sleep "$SECS"
echo "--- screenshot"
xcrun simctl io "$UDID" screenshot "$OUT/screenshot.png" >/dev/null 2>&1
cleanup
trap - EXIT INT TERM
echo "--- server log tail"
tail -15 "$OUT/server.log"
echo "--- client lines about the join"
grep -iE 'url|join|lobby|download|connect|warn|error|fail|\[iOS\]' "$OUT/console.log" |
  grep -v 'think aborted' | cut -c1-200 | tail -40
echo "--- written: $OUT/console.log $OUT/server.log $OUT/screenshot.png"
