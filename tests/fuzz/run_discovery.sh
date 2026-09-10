#!/usr/bin/env bash
# Discovery mode (NOT a ctest): let libFuzzer explore for a time budget,
# coverage-mutating from the committed corpus. Any crash is minimized and
# written under tests/fuzz/crashes/, where the corpus-replay ctest
# (fuzz.<target>) then guards it forever. Open-ended exploration has no clean
# pass/fail, so it deliberately lives outside ctest.
#
# Usage:
#   tests/fuzz/run_discovery.sh <wire_codec|channel_frame|voice_segment|bulk_transfer|server_dispatch|client_snapshot|join_accept> [seconds] [build-dir]
#
# Example:
#   tests/fuzz/run_discovery.sh wire_codec 300 build-fuzz
set -euo pipefail

target="${1:?usage: run_discovery.sh <wire_codec|channel_frame|voice_segment|bulk_transfer|server_dispatch|client_snapshot|join_accept> [seconds] [build-dir]}"
seconds="${2:-300}"
builddir="${3:-build-fuzz}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="$builddir/fuzz_${target}"
corpus="$here/corpus/${target}"
crashes="$here/crashes"

if [ ! -x "$bin" ]; then
    echo "fuzz binary not found: $bin" >&2
    echo "build first: cmake -B $builddir -DWB_FUZZ=ON -DCMAKE_C_COMPILER=clang \\" >&2
    echo "             -DCMAKE_CXX_COMPILER=clang++ && cmake --build $builddir --target fuzz_${target}" >&2
    exit 1
fi

# libFuzzer writes newly-discovered inputs into the FIRST corpus dir it is
# given and treats the rest as read-only seeds. Point the writable dir at the
# build tree so discovery never pollutes the committed seed corpus; promote
# anything worth keeping into tests/fuzz/corpus/<target>/ deliberately.
work="$builddir/fuzz-corpus-${target}"
mkdir -p "$work" "$crashes"

# The sanitizer symbolizes each newly covered function by starting
# llvm-symbolizer. Where only a versioned llvm-symbolizer-N is installed that
# start fails, the reply pipe never reaches EOF and the run stalls ~90 seconds
# per attempt — single-digit exec/s instead of six figures. Prefer
# unsymbolized output to stalling; replay an artifact without this to get
# named frames back. macOS is exempt: its runtime falls back to atos, which
# works, so only a host with neither tool gets symbolization turned off.
if ! command -v llvm-symbolizer >/dev/null 2>&1 && \
   ! command -v atos >/dev/null 2>&1; then
    export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}symbolize=0"
    echo "note: llvm-symbolizer not on PATH — running with symbolize=0" >&2
fi
# -print_final_stats reports the total executed inputs at exit
# (stat::number_of_executed_units) alongside libFuzzer's periodic
# "#<n> ... exec/s" lines — the run count stays visible even with the app's
# own logging stripped (WB_FUZZ build).
exec "$bin" \
    -max_total_time="$seconds" \
    -print_final_stats=1 \
    -artifact_prefix="$crashes/" \
    "$work" "$corpus"
