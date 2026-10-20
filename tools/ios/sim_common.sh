# Shared by tools/ios/sim_run.sh and tools/ios/sim_join.sh. Sourced, not run.
# Written for macOS's /bin/bash 3.2.

# The repository root: two directories above this file.
SIM_REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SIM_BUNDLE_ID=com.winbolo.ios

# sim_pick_udid NAME
# Prints the UDID to use: $SIM_UDID when set, otherwise the first available
# simulator called NAME, otherwise (NAME empty) the first available iPhone.
# Returns 1 when none is found.
sim_pick_udid() {
  local name=$1 line udid
  if [ -n "${SIM_UDID:-}" ]; then
    echo "$SIM_UDID"
    return 0
  fi
  if [ -n "$name" ]; then
    line=$(xcrun simctl list devices available | grep -F "    $name (" | head -1)
  else
    line=$(xcrun simctl list devices available | grep -E '^ +iPhone ' | head -1)
  fi
  udid=$(echo "$line" | grep -oE '[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}' | head -1)
  if [ -z "$udid" ]; then
    return 1
  fi
  echo "$udid"
}

# sim_out_dir DIR TAG
# Prints the output directory, creating it: DIR when given, otherwise a new
# directory under build-ios-sim/tool-runs/ named after TAG and the time.
sim_out_dir() {
  local dir=$1 tag=$2
  if [ -z "$dir" ]; then
    dir="$SIM_REPO_ROOT/build-ios-sim/tool-runs/$tag-$(date +%Y%m%d-%H%M%S)"
  fi
  mkdir -p "$dir" || return 1
  (cd "$dir" && pwd)
}

# 1 once sim_boot has booted the simulator itself, 0 when it was already
# booted or has not been booted. sim_shutdown reads it.
SIM_BOOTED_HERE=0

# sim_is_booted UDID
# Returns 0 when the simulator is booted.
sim_is_booted() {
  xcrun simctl list devices | grep -F "($1) (Booted)" >/dev/null
}

# sim_boot UDID
# Boots the simulator if it is not already, and waits until it has finished.
# Sets SIM_BOOTED_HERE to 1 when this call did the booting.
sim_boot() {
  if ! sim_is_booted "$1"; then
    SIM_BOOTED_HERE=1
  fi
  xcrun simctl boot "$1" 2>&1 | grep -v 'current state: Booted'
  xcrun simctl bootstatus "$1" -b >/dev/null 2>&1
}

# sim_shutdown UDID
# Shuts the simulator down if sim_boot booted it, and leaves it running if
# it was already booted before the script started.
sim_shutdown() {
  if [ "$SIM_BOOTED_HERE" = 1 ]; then
    xcrun simctl shutdown "$1" >/dev/null 2>&1
    SIM_BOOTED_HERE=0
  fi
}
