#!/usr/bin/env bash
# Regenerate the tier-1 (wire_codec) seed corpus from the committed wire
# fixtures. Each non-empty line of tests/fixtures/wire/*.hex is one captured
# record (real session bytes dumped by Cluster A's WIRE_CORPUS_TAP); we decode
# the hex to a raw binary seed file libFuzzer can load directly.
#
# Idempotent: safe to re-run after the fixtures change. Seeds are committed so
# the corpus-replay ctest (fuzz.wire_codec) is meaningful without this script.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
fixtures="$repo/tests/fixtures/wire"
out="$here/corpus/wire_codec"

mkdir -p "$out"
for f in "$fixtures"/*.hex; do
    base="$(basename "$f" .hex)"
    n=0
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        printf '%s' "$line" | xxd -r -p > "$out/${base}_${n}.bin"
        n=$((n + 1))
    done < "$f"
done
echo "wrote seeds to $out"
