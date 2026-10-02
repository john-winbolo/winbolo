#!/usr/bin/env bash
# Strips debug calls from GoalHunter Lua sources and writes cleaned
# copies to opt/.  Run this after editing any .lua file when you want
# to refresh the production (non-debug) brain used by WinBolo/WinBoloDS.
#
# Usage: ./strip.sh [path/to/lua_strip]
#   Default lua_strip location: /home/john/build-release/lua_strip
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LUA_STRIP="${1:-/home/john/build-release/lua_strip}"

if [ ! -x "$LUA_STRIP" ]; then
    echo "ERROR: lua_strip not found (or not executable) at $LUA_STRIP"
    echo "Build the lua_strip target first, or pass its path as an argument."
    exit 1
fi

# The shell expands *.lua to the source files, so lua_strip receives real
# paths (unlike cmd.exe, which passed the literal glob and broke strip.bat).
"$LUA_STRIP" \
    --strip print2 \
    --strip "viz." \
    --strip overlay_ \
    --strip-block "if BRAIN_DEBUG_MODE" \
    --exclude los_stamp_cache.lua \
    --exclude shield_stamp_cache.lua \
    "$SCRIPT_DIR/opt" \
    "$SCRIPT_DIR"/*.lua

echo "Done. Stripped files written to $SCRIPT_DIR/opt/"
