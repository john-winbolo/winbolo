# Packet-parser fuzzing (hardening plan §1.2)

Both sides of the wire hand-parse binary with manual bounds checks. With
open-source self-compilable clients, hostile packets are a certainty, and one
missed bound is a remote crash on the dedicated server. These fuzz targets feed
attacker-controlled bytes straight into the parsers, exactly as a hostile
datagram arrives; AddressSanitizer turns any out-of-bounds read into a hard
abort with the triggering input.

## Platform: Linux + Clang

The targets build with **Clang** and `-fsanitize=fuzzer,address` on Linux. The
pure C parsers compile fine there and the normal MSVC/GCC builds are untouched —
`WB_FUZZ` is OFF by default and gates the targets, the corpus ctests, and the
`transport_udp_server.c` dispatcher seam out of every shipping build.

**AFL++ fallback:** if requiring Clang/libFuzzer is unwanted, the same target
functions (`LLVMFuzzerTestOneInput`) compile under `afl-clang-fast` with an
AFL persistent-mode shim; the seed corpus below transfers unchanged. libFuzzer
is the default because it needs no external driver.

## Targets

| Target | Surface |
|---|---|
| `fuzz_wire_codec` (tier 1) | The bounded generated `unpack*` codecs + `commandCodecDecode` — the single bounds-checked path Cluster A codegen produced. Pure, no context. Doubles as the architecture plan's A.5 differential guard. |
| `fuzz_server_dispatch` (tier 2) | The full `serverProcessPacket` switch and the hand-written count-loops / chunk reassembly codegen did **not** touch (COMMAND_TICK, MAP_ACK, the reliable event loops, lobby/map handlers) — the highest remaining over-read surface. Runs socket-free and thread-free via the `WB_FUZZ`-gated seam. |

## Build

```sh
cmake -B build-fuzz -DWB_FUZZ=ON \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
# Build just the fuzz targets (pulls only their library deps, not the whole game):
cmake --build build-fuzz --target fuzz_wire_codec fuzz_server_dispatch
```

`WB_FUZZ` is a **dedicated build config** — when ON the whole build is
instrumented with ASan + coverage. Keep it in its own build dir.

## Two run modes

**1. Corpus replay — the per-commit net (ctest).** Each target is run over its
committed corpus + crash set with `-runs=0` (load and execute each input once,
no mutation): bounded, deterministic, pass/fail. This is the permanent safety
net — "no known-hostile packet crashes the parser."

```sh
ctest --test-dir build-fuzz -R '^fuzz\.'
```

**2. Discovery — time-boxed exploration (NOT a ctest).** Let the engine explore
with a budget; a crash is minimized and dropped into `crashes/`, where mode 1
then guards it forever.

```sh
tests/fuzz/run_discovery.sh wire_codec 300 build-fuzz
tests/fuzz/run_discovery.sh server_dispatch 300 build-fuzz
```

## Corpus

- `corpus/wire_codec/` — seeded from the committed wire fixtures
  (`tests/fixtures/wire/*.hex`, real session bytes captured by Cluster A's
  `WIRE_CORPUS_TAP`). Regenerate with `tests/fuzz/seed_from_fixtures.sh`.
- `corpus/server_dispatch/` — hand-seeded minimal datagrams with valid
  `'W''B'`-magic headers (ping, join, map-ack, quit, command-tick, input).
- `crashes/` — minimized regressions. A crash found in discovery lands here and
  is replayed by the ctest on every build. Commit new crash inputs alongside
  the fix.

## Known lead this harness is built to catch

`transport_udp_client.c:~1550` guards the reliable-event loop with
`if (pos + 1 > len) break;` (one byte) but then calls `unpackGameEvent`, which
`memcpy`s up to `gameEventDataSize(type)` bytes with no `avail` bound — a
truncated event over-reads. That loop is reached through the snapshot handler,
not the leaf in isolation; surfacing it cleanly is a motivating case for
extending tier 2 with a client-snapshot dispatcher target.
