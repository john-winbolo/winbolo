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
| `fuzz_channel_frame` (tier 1) | `channelRecvFrame` — the one variable-length frame parser the channel-mux rework collapsed every reliable queue, the `CONTROL_TICK`/`_ACK` carrier, and the three bulk chunkers onto. Every reliable byte the transport receives now passes through it. Pure, no context; reinitialises a `ChannelMux` per input and drains every channel afterward to exercise in-order delivery + stream reassembly, not just the parse. |
| `fuzz_voice_segment` (tier 1) | `voiceSegmentUnpackUp` / `voiceSegmentUnpackDown` — the parse sites for everything carried on `CHANNEL_VOICE`. Voice is best-effort, so a segment arrives with nothing upstream vouching for it, and each parse hands back an opus pointer into the caller's buffer plus a length that is passed straight on. Pure, no context; every accepted payload is summed into a volatile sink so ASan sees an over-long `opusLen` rather than a silent walk off the end. |
| `fuzz_bulk_transfer` (tier 1) | The bulk-transfer receive state machine (`bulkParseStreamHeader` + `bulkReceiverFeed`) that reassembles map download / resync / upload / preview blobs off `CHANNEL_BULK`. The stream header (`kind`/`gen`/`totalSize`/`pathLen`/`path`) and body length are wire-supplied and untrusted. The harness feeds the input in split fragments through a bounding sink that allocates `dst` to exactly `totalSize`, so ASan catches any body-fill write past the buffer. |
| `fuzz_server_dispatch` (tier 2) | The full `serverProcessPacket` switch and the hand-written count-loops / chunk reassembly codegen did **not** touch (COMMAND_TICK, MAP_ACK, the reliable event loops, lobby/map handlers) — the highest remaining over-read surface. Runs socket-free and thread-free via the `WB_FUZZ`-gated seam. |
| `fuzz_client_snapshot` | The client `PACKET_STATE_SNAPSHOT` decoder — specifically the count-driven reliable-event loop and its `unpackGameEvent` call (the suspected over-read at `transport_udp_client.c:~1550`). Input is the snapshot body; the seam frames a header and feeds it on an exact-size, ASan-guarded buffer. |
| `fuzz_join_accept` | The client `PACKET_JOIN_ACCEPT` handler — the join handshake (server-assigned slot, mapSize/connId parsing), a path the snapshot target can't reach (it starts already CONNECTED). Input is the accept body. |
| `fuzz_map_load` | `mapLoadCompressedMap` — the parser every map that arrives over the network lands in: three fixed-size struct memcpys for the bases, pillboxes and starts, then an LZW decode of the terrain into the 256x256 array. Input is the whole compressed blob, copied onto an exact-size heap buffer so ASan catches an over-read past `inputLen`. Covers the LZW decoder and the reject paths; it almost never reaches a successful load, because a mutated stream rarely decodes to exactly 65536 bytes. |
| `fuzz_map_fields` | The same loader, past the LZW gate. Takes the struct region straight from the fuzzer, fills the terrain from the rest of the input, compresses it with the writer's own `lzwencoding`, and hands the result to the real `mapLoadCompressedMap`, so every input loads. This is what exercises `basesValidate` / `pillsValidate` / `startsValidate` and the terrain values the nibble-packed file format cannot express. Each input costs a full compress and decompress, so it runs far slower than `fuzz_map_load` and saturates within seconds. |

## Build

```sh
cmake -B build-fuzz -DWB_FUZZ=ON \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
# Build just the fuzz targets (pulls only their library deps, not the whole game):
cmake --build build-fuzz --target fuzz_wire_codec fuzz_channel_frame fuzz_voice_segment fuzz_bulk_transfer fuzz_server_dispatch fuzz_client_snapshot fuzz_join_accept fuzz_map_load fuzz_map_fields
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

The fuzz build strips the app's own logging (`WINBOLO_LOG_LEVEL=OFF` plus the
`WB_FUZZ`-gated hot-path `fprintf`s), so the only output is libFuzzer's — the
log stays small over a multi-hour run and still reports the total executions.
`run_discovery.sh` passes `-print_final_stats=1`; the count is on the
`stat::number_of_executed_units:` line, and libFuzzer also prints periodic
`#<n> ... exec/s` pulses and a closing `Done <n> runs`.

### If a target crawls at single-digit exec/s

The sanitizer symbolizes each newly covered function by starting
`llvm-symbolizer`. Where only a versioned `llvm-symbolizer-N` is on PATH that
start fails, the reply pipe never reaches EOF, and the run stalls about 90
seconds per attempt — `Done 22 runs in 90 second(s)` at 0% CPU, with the main
thread in `pipe_read`. It hits the targets linking the most code hardest.

`run_discovery.sh` detects this and sets `ASAN_OPTIONS=symbolize=0`. macOS is
not affected, since its runtime falls back to `atos`, so the guard leaves it
alone. Setting `ASAN_SYMBOLIZER_PATH` does *not* help, and neither does
libFuzzer's `-symbolize=0`. Replaying an artifact symbolizes fine, so leave
the variable unset when you replay one and you still get named frames.

### Long unattended runs (parallel)

`run_discovery.sh` is single-process. For an overnight run, drive the binary
directly with `-jobs`/`-workers` so every core fuzzes; each worker writes a
`fuzz-<id>.log`, and the total executions is the sum of their final stats:

```sh
mkdir -p ~/fuzz-build/fuzz-corpus-client_snapshot
( cd ~/fuzz-build && nohup ./fuzz_client_snapshot \
    -max_total_time=21600 -jobs=$(nproc) -workers=$(nproc) -print_final_stats=1 \
    -artifact_prefix=<repo>/tests/fuzz/crashes/ \
    fuzz-corpus-client_snapshot <repo>/tests/fuzz/corpus/client_snapshot \
    >/dev/null 2>&1 & )

# In the morning: total executions across workers, and any new crashes
awk '/number_of_executed_units/{s+=$2} END{print s" total execs"}' ~/fuzz-build/fuzz-*.log
ls <repo>/tests/fuzz/crashes/
```

## Corpus

- `corpus/wire_codec/` — seeded from the committed wire fixtures
  (`tests/fixtures/wire/*.hex`, real session bytes captured by Cluster A's
  `WIRE_CORPUS_TAP`). Regenerate with `tests/fuzz/seed_from_fixtures.sh`.
- `corpus/channel_frame/` — hand-authored well-formed channel frames covering
  the codec's shapes: the empty "go quiet" frame, an ack-only frame, a single
  in-order game segment, a combined ack-list + multi-channel segment-list frame,
  a bulk stream segment (drives reassembly), and an out-of-order segment the
  reorder buffer must hold. All are non-crashing so the replay ctest stays green;
  discovery mutates from them into truncated / oversized / bad-id frames.
- `corpus/voice_segment/` — hand-authored well-formed voice segments in both
  directions: the shortest client→server segment that carries audio, a typical
  one, one with the end-of-utterance flag set, and server→client segments from
  the first and last tank slot. Each direction also gets a maximum-length
  payload (`VOICE_SEG_MAX_OPUS`), the seed nearest an over-read. All are
  non-crashing so the replay ctest stays green; discovery mutates them into
  header-only, oversized and out-of-range-`fromPlayer` segments.
- `corpus/bulk_transfer/` — hand-authored well-formed bulk streams (header +
  body): a small download, an upload, a preview carrying a path, a resync, two
  back-to-back transfers (tail of one + head of the next), and a header that
  announces more body than it provides (stays mid-body). All non-crashing so
  the replay ctest stays green; discovery mutates them into truncated headers,
  oversized `totalSize`, and bad `pathLen` values.
- `corpus/server_dispatch/` — hand-seeded minimal datagrams with valid
  `'W''B'`-magic headers (ping, join, map-ack, quit, command-tick, input).
  The seam's `LLVMFuzzerInitialize` drives one real cookie-gated JOIN to
  completion (`fuzzServerWarmJoin`), so the dispatcher starts with a client
  connected at the seam's fixed peer address. Without this the post-JOIN
  handlers — COMMAND_TICK, INPUT, CONTROL_ACK, the reliable event loops and
  map reassembly, i.e. the hand-written count-loops this target exists to
  reach — all bail at their `serverFindClient() < 0` guard, leaving coverage
  pinned near the JOIN gate. To enrich these seeds with real post-JOIN traffic,
  capture the client→server datagrams a loopback session produces and drop them
  here as whole datagrams (the seam replays one datagram per input).
- `corpus/map_load/` — the real Everard Island blob (the first 5097 bytes of
  `E_MAP` in `everard_map.h`, the length every caller passes), a struct region
  with no terrain at the exact minimum the length guard accepts, one byte under
  that minimum, and a blob with all three counts at their maxima. Everard is
  the only seed that loads; the rest pin the reject paths.
- `corpus/map_fields/` — struct-region-plus-terrain inputs for the compressing
  target: a plain map with no objects and uniform terrain, one with all three
  counts at their maxima and every base field set to 200 (past the owner and
  stock clamps) over terrain of 200, and a terrain mix spanning legal tiles,
  `DEEP_SEA` (255) and nothing in between. All load, by construction.
- `crashes/` — minimized regressions. A crash found in discovery lands here and
  is replayed by the ctest on every build. Commit new crash inputs alongside
  the fix.

## Known lead this harness is built to catch

`transport_udp_client.c:~1550` guards the reliable-event loop with
`if (pos + 1 > len) break;` (one byte) but then calls `unpackGameEvent`, which
`memcpy`s up to `gameEventDataSize(type)` bytes (`GAME_EVENT_MAX_DATA` = 8 for
unknown types) with no `avail` bound — a truncated final event over-reads the
datagram. `fuzz_client_snapshot` exists to reach exactly this loop; discovery
should find the truncated-event input that trips it. When it does, drop the
minimized artifact into `crashes/` so the replay ctest guards the fix.
