#!/bin/bash
# Advertise a dedicated server on this Mac with -mdns and look it up with the
# system's dns-sd tool, which goes through the same Bonjour service the iOS
# client's LAN search uses (src/gui/ios/ios_discovery.m): browse for
# _winbolo._udp, resolve the instance to its port and TXT record, and look up
# the host's address. See docs/IOS.md.
#
# Usage: tools/ios/mdns_check.sh [--ds PATH] [--port P] [--out DIR]
#                                [-- extra WinBoloDS arguments]
#   --ds          default cmake-build-debug/WinBoloDS
#   --port        the server's port (default 27520)
#   --out         where the server and dns-sd output go; default a new
#                 directory under cmake-build-debug/tool-runs/
set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)

usage() {
  sed -n '8,13p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

DS="$REPO_ROOT/cmake-build-debug/WinBoloDS"
PORT=27520
OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --ds)   DS=${2:-}; shift 2 ;;
    --port) PORT=${2:-}; shift 2 ;;
    --out)  OUT=${2:-}; shift 2 ;;
    --) shift; break ;;
    -h|--help) usage ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
done

# A relative --ds is taken from where the script was run, since the server is
# started from inside the output directory.
case "$DS" in /*) ;; *) DS="$PWD/$DS" ;; esac
[ -x "$DS" ] || { echo "no WinBoloDS at $DS" >&2; exit 1; }
if [ -z "$OUT" ]; then
  OUT="$REPO_ROOT/cmake-build-debug/tool-runs/mdns_check-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$OUT" || { echo "cannot create $OUT" >&2; exit 1; }
OUT=$(cd "$OUT" && pwd)

# Every background process, killed on the way out however the script ends.
PIDS=""
cleanup() {
  local p
  for p in $PIDS; do
    kill "$p" 2>/dev/null
    wait "$p" 2>/dev/null
  done
  PIDS=""
}
trap cleanup EXIT INT TERM

# Runs a dns-sd query for SECS seconds into FILE, then stops it (dns-sd keeps
# listening until killed).
dnssd_for() {
  local secs=$1 file=$2 pid
  shift 2
  dns-sd "$@" > "$file" 2>&1 &
  pid=$!
  PIDS="$PIDS $pid"
  sleep "$secs"
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
}

echo "--- server on port $PORT, output in $OUT"
(cd "$OUT" && exec "$DS" -mdns -inbuilt -port "$PORT" -gametype Open \
   -nowinbolonet -notracker -quiet "$@") > "$OUT/server.log" 2>&1 &
PIDS="$PIDS $!"
sleep 4

dnssd_for 5 "$OUT/browse.log" -B _winbolo._udp local.
echo "--- browse"
grep -vE '^[[:space:]]*$' "$OUT/browse.log" | tail -4
INST=$(grep -oE 'WinBolo-[0-9]+-[0-9]+-[0-9]+-[0-9]+-[0-9]+' "$OUT/browse.log" | head -1)
echo "instance: ${INST:-none}"
if [ -z "$INST" ]; then
  echo "no _winbolo._udp instance found" >&2
  exit 1
fi

dnssd_for 5 "$OUT/resolve.log" -L "$INST" _winbolo._udp local.
echo "--- resolve"
grep -vE '^[[:space:]]*$' "$OUT/resolve.log" | tail -4 | cut -c1-200
HOST=$(grep -oE 'can be reached at [^ ]+' "$OUT/resolve.log" | head -1 | awk '{print $5}' | cut -d: -f1)
if [ -n "$HOST" ]; then
  dnssd_for 4 "$OUT/address.log" -G v4 "$HOST"
  echo "--- address of $HOST"
  grep -vE '^[[:space:]]*$' "$OUT/address.log" | tail -2
fi
echo "--- written to $OUT"
