#!/bin/bash
# Boot an iOS simulator without opening its window, install and launch
# WinBoloIOS, keep its console for a number of seconds, take a screenshot,
# then stop the app and shut the simulator down. See docs/IOS.md.
#
# Usage: tools/ios/sim_run.sh [--device NAME] [--app PATH] [--out DIR]
#                             [--seconds N]
#   SIM_UDID      simulator to use; otherwise --device NAME picks the first
#                 available one of that name, otherwise the first iPhone
#   WINBOLO_LOG   passed to the app (default '*=info')
#   --app         default build-ios-sim/WinBoloIOS.app
#   --out         where console.log and screenshot.png go; default a new
#                 directory under build-ios-sim/tool-runs/
#   --seconds     how long the app runs before the screenshot (default 20)
set -u
. "$(dirname "$0")/sim_common.sh"

usage() {
  sed -n '6,14p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

DEVICE=""
APP="$SIM_REPO_ROOT/build-ios-sim/WinBoloIOS.app"
OUT=""
SECS=20
while [ $# -gt 0 ]; do
  case "$1" in
    --device)  DEVICE=${2:-}; shift 2 ;;
    --app)     APP=${2:-}; shift 2 ;;
    --out)     OUT=${2:-}; shift 2 ;;
    --seconds) SECS=${2:-}; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
done

[ -d "$APP" ] || { echo "no app at $APP" >&2; exit 1; }
UDID=$(sim_pick_udid "$DEVICE") || { echo "no simulator found${DEVICE:+ called '$DEVICE'}" >&2; exit 1; }
OUT=$(sim_out_dir "$OUT" sim_run) || { echo "cannot create the output directory" >&2; exit 1; }
LOG_SPEC=${WINBOLO_LOG:-'*=info'}

LPID=""
cleanup() {
  if [ -n "$LPID" ]; then
    xcrun simctl terminate "$UDID" "$SIM_BUNDLE_ID" >/dev/null 2>&1
    kill "$LPID" 2>/dev/null
    wait "$LPID" 2>/dev/null
    LPID=""
  fi
  xcrun simctl shutdown "$UDID" >/dev/null 2>&1
}
trap cleanup EXIT INT TERM

echo "--- simulator $UDID, output in $OUT"
sim_boot "$UDID"
echo "--- install $APP"
xcrun simctl install "$UDID" "$APP" || exit 1
echo "--- launch (console for ${SECS}s)"
SIMCTL_CHILD_WINBOLO_LOG="$LOG_SPEC" \
  xcrun simctl launch --console "$UDID" "$SIM_BUNDLE_ID" > "$OUT/console.log" 2>&1 &
LPID=$!
sleep "$SECS"
echo "--- screenshot"
xcrun simctl io "$UDID" screenshot "$OUT/screenshot.png" >/dev/null 2>&1
cleanup
trap - EXIT INT TERM
echo "--- console tail"
grep -vE '^[[:space:]]*$' "$OUT/console.log" | tail -40
echo "--- written: $OUT/console.log $OUT/screenshot.png"
