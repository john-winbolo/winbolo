#!/usr/bin/env bash
# Discovery mode (NOT a ctest): let libFuzzer explore for a time budget,
# coverage-mutating from the committed corpus. Any crash is minimized and
# written under tests/fuzz/crashes/, where the corpus-replay ctest
# (fuzz.<target>) then guards it forever. Open-ended exploration has no clean
# pass/fail, so it deliberately lives outside ctest.
#
# Usage:
#   tests/fuzz/run_discovery.sh <wire_codec|server_dispatch> [seconds] [build-dir]
#
# Example:
#   tests/fuzz/run_discovery.sh wire_codec 300 build-fuzz
set -euo pipefail

target="${1:?usage: run_discovery.sh <wire_codec|server_dispatch> [seconds] [build-dir]}"
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
exec "$bin" \
    -max_total_time="$seconds" \
    -artifact_prefix="$crashes/" \
    "$work" "$corpus"
