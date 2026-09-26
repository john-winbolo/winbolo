#!/usr/bin/env bash
# Build the fuzz_replay seed corpus from the committed recordings and one
# scripted round.
#
# A seed is what fuzz_replay reads: [u16 big-endian scriptsLen][scripts.json]
# [log.dat], with scriptsLen 0 for a recording that has no scripts.json. Each
# tests/fixtures/wbv/*.wbv becomes corpus/replay/<name>.bin. The scripted round
# comes from the lv_presentation_scripted_round unit case, which keeps a copy
# of its recording at $WB_KEEP_WBV; it carries scripts.json and the panel,
# score and announcement records none of the fixtures have.
#
# Usage:
#   tests/fuzz/seed_replay.sh <dir holding WinBoloUnitTests>
#
# Example:
#   tests/fuzz/seed_replay.sh ~/viewer-build
#
# Idempotent: re-run it after the fixtures or the writer change, then commit
# tests/fuzz/corpus/replay/.
set -euo pipefail

bindir="${1:?usage: seed_replay.sh <dir holding WinBoloUnitTests>}"
unittests="$bindir/WinBoloUnitTests"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
fixtures="$repo/tests/fixtures/wbv"
out="$here/corpus/replay"

if [ ! -x "$unittests" ]; then
    echo "unit-test binary not found: $unittests" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# One .wbv to one seed.
to_seed() {
    python3 - "$1" "$2" <<'EOF'
import struct
import sys
import zipfile

src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as z:
    log = z.read("log.dat")
    scripts = z.read("scripts.json") if "scripts.json" in z.namelist() else b""
if len(scripts) > 0xFFFF:
    sys.exit("%s: scripts.json is %d bytes, more than a seed's length holds"
             % (src, len(scripts)))
with open(dst, "wb") as f:
    f.write(struct.pack(">H", len(scripts)) + scripts + log)
EOF
}

mkdir -p "$out"
for f in "$fixtures"/*.wbv; do
    to_seed "$f" "$out/$(basename "$f" .wbv).bin"
done

WB_KEEP_WBV="$work/scripted_presentation_round.wbv" \
    "$unittests" --test lv_presentation_scripted_round
if [ ! -s "$work/scripted_presentation_round.wbv" ]; then
    echo "lv_presentation_scripted_round left no recording" >&2
    exit 1
fi
to_seed "$work/scripted_presentation_round.wbv" \
    "$out/scripted_presentation_round.bin"

echo "wrote seeds to $out"
