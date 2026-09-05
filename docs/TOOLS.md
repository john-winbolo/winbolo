# Developer tools

Helpers that exist in the repo but aren't part of a normal build, and so are
easy to miss. Everything here is for working *on* WinBolo — the shipping
binaries (`WinBolo`, `WinBoloDS`, `LogViewer`, `MapEditor`, `WinBoloHeadless`,
`BrainTest`, `winbolo_gym`) are covered by
[BUILDING.md](BUILDING.md#build-targets) instead.

| Tool | What it's for |
|------|---------------|
| [`MakeTileTestMap`](#maketiletestmap) | Writes a `.map` exercising the terrain-shape lookups |
| [`tools/dump_lang_en.py`](#toolsdump_lang_enpy) | Regenerates `data/lang/en.txt` and `lang_names.inc` |
| [`tools/validate_lang.py`](#toolsvalidate_langpy) | Checks a translation against the English source |
| [`tools/test_lang_roundtrip.py`](#toolstest_lang_roundtrippy) | Proves `en.txt` round-trips against `langTable[]` |
| [`tools/gen_countries.py`](#toolsgen_countriespy) | One-shot generator for the country-name strings |
| [`scripts/analyze_optimize_log.py`](#scriptsanalyze_optimize_logpy) | Summarises GoalHunter brain timings from `optimize.log` |
| [Fixture capture entries](#fixture-capture-entries) | Regenerate committed test fixtures on demand |
| [Headless test harness](#headless-test-harness) | Scripted server + bot scenario tests |

---

## MakeTileTestMap

`tools/make_tile_test_map.c` — writes a small `.map` that puts the terrain-shape
lookups in `src/bolo/screencalc.c` next to reference versions of the same
layout, so their edge cases can be eyeballed in the game or the map editor.

The layouts these functions get wrong are specific and rare, which makes them
hard to hit by hand on a real map. Each demo is a matched pair: a river mouth at
the coast with and without a bridge on the mouth square, a boat beside water and
beside a bridge, and a river cross with and without a road on its centre. Both
halves of a pair are drawn from the same shape, so anywhere they differ is the
lookup reading a road as dry land.

```bash
cmake --build build --target MakeTileTestMap
./build/MakeTileTestMap tile_test.map
```

`EXCLUDE_FROM_ALL`, so it never builds unless named. It resolves every marked
square through the real `screenCalc*` functions and prints the tile each one
gives, so the comparison can be made without launching anything, then reads the
file back through `mapRead` to check it parses.

Two things to know when reading its output:

- **`mapRead` recentres the map**, so the coordinates you see in game are not
  the ones the program wrote. It works the shift out from the land bounding box
  and prints the in-game coordinates.
- **Not every reported difference is a defect.** In the stock tile sheet all
  nine `DEEP_SEA_*` tiles are the same flat water, differing only in where a
  handful of speckles sit (7–19 pixels of 256), so a difference there is
  invisible in practice. The `RIVER_*` family does have genuinely distinct
  shapes, and a difference there is real.

To add a case, paint it with `paint()` using the ASCII notation at the top of
the file (`R` river, `O` road, `D` deep sea, `B` boat, `G` grass) and add a
`report()` line.

It links `tests/unit/test_stubs.c` for the frontend / winbolonet / natPortMap
defaults, the same way `WinBoloUnitTests` does — `bolo_static`'s map TU reaches
back into the server sim, so lighter link sets don't resolve.

## tools/dump_lang_en.py

Generates the canonical English translation file and the parallel C lookup
table from the two sources of truth:

- `src/gui/lang.h` — symbolic ID to integer enum
- `src/gui/sdl3/lang.c` — integer enum to English string

Outputs `data/lang/en.txt` and `src/gui/sdl3/lang_names.inc`. Both are
deterministic and idempotent: re-running with no source changes produces
byte-identical files.

```bash
python3 tools/dump_lang_en.py            # regenerate
python3 tools/dump_lang_en.py --check    # verify without writing
```

Run it after adding or renaming a `STR_*` id. The `unit.lang_name_table` test
guards the generated table — it once shipped with `K_LANG_NAME_TABLE_SIZE` one
larger than the real array, which walked `resolveName`'s `bsearch` off the end
and crashed at startup for every non-English user.

## tools/validate_lang.py

Validates a translation file against the canonical English source. Catches what
a translator is likely to introduce:

- unknown ID (a typo of a `STR_`/`MESSAGE_` name) — **fail**
- duplicate ID — **fail**
- placeholder-token multiset mismatch — **fail**
- missing ID — **warn** (the runtime falls back to English, so it isn't fatal)

```bash
python3 tools/validate_lang.py data/lang/de.txt
python3 tools/validate_lang.py --en data/lang/en.txt data/lang/de.txt
```

## tools/test_lang_roundtrip.py

Verifies `data/lang/en.txt` round-trips byte-for-byte against `langTable[]` in
`src/gui/sdl3/lang.c`, mirroring the C loader's per-line parse rules.

```bash
python3 tools/test_lang_roundtrip.py
```

It exists for one specific failure: several real labels end in a space (`"Map
Name: "`), so a loader that strips trailing whitespace corrupts them silently.
Any whitespace tampering shows up here immediately.

## tools/gen_countries.py

One-shot generator for the `STR_COUNTRY_*` strings, keyed by the two-letter
flag basenames in `data/flags/`. From `data/flags/countries.csv` it rewrites
marked regions in `src/gui/lang.h` and `src/gui/sdl3/lang.c`, and regenerates
`src/gui/sdl3/countries.inc` in full.

```bash
python3 tools/gen_countries.py
```

Only needed when the flag set changes. It edits inside marked regions, so keep
those markers intact.

## scripts/analyze_optimize_log.py

Summarises the `optimize.log` the GoalHunter brain appends to, per BrainTest
session: tick-total distribution (worst, mean, p95), per-phase timings
(`threat.update`, `update_pool_cache`, `dij sched`, …) and pool-6 cache-miss
diagnostics.

```bash
python3 scripts/analyze_optimize_log.py                 # latest session
python3 scripts/analyze_optimize_log.py --session 0     # oldest
python3 scripts/analyze_optimize_log.py --all
python3 scripts/analyze_optimize_log.py path/to/optimize.log
```

Its module docstring advertises a `--top N` flag that the argument parser
doesn't define; passing it is an error.

The log is append-only, so sessions accumulate; boundaries are found by
`===TICK 1===` blocks and the latest is analysed by default.

## Fixture capture entries

Three entries in `WinBoloUnitTests` regenerate committed fixtures rather than
assert anything. They are deliberately **omitted from CTest** and run only by
name — running them rewrites files under `tests/fixtures/`, so check the diff
afterwards.

```bash
./build/WinBoloUnitTests --test wire_corpus_capture      # tests/fixtures/wire/
./build/WinBoloUnitTests --test wbv_v2_capture           # tests/fixtures/wbv/spectator_v2.wbv
./build/WinBoloUnitTests --test spectator_seed_capture   # tests/fixtures/wbv/spectator_seed.bin
```

`wire_corpus_capture` runs a loopback session to regenerate the wire corpus the
differential codec test compares against. `wbv_v2_capture` drives the dedicated
server's writer to rewrite the `.wbv` reader gate's v2 fixture.
`spectator_seed_capture` captures a real ring keyframe and needs
`WB_WBV_FIXTURE_DIR` set.

Regenerating a fixture makes the test that reads it pass by construction, so
only do it when the format genuinely changed, and say so in the commit.

## Headless test harness

`tests/` holds a Python harness that drives a real `WinBoloDS` plus one or more
`WinBoloHeadless` clients running Lua brains, then checks the outcome — bot
aim, diagonal corner-cutting, LGM wall shots, tree seeking and so on, each with
a `generate_*_map.py` that builds its arena.

`run_test.py` is the multi-scenario driver. Its `--test` takes one of a fixed
set of names (`connect`, `movement`, `terrain_speed`, `shell_fire`,
`late_join`, … plus numbered ids like `2.10`), not a free-form string:

```bash
python3 tests/run_test.py --build-dir build-linux --test all
python3 tests/run_test.py --test terrain_speed --ticks 400 --verbose
```

The bot-behaviour scenarios are **separate standalone scripts**, not `--test`
names, each with its own hand-rolled `--ticks` / `--build` arguments:

```bash
python3 tests/corner_cut_test.py --ticks 4000 --build build-linux
python3 tests/aim_test.py
python3 tests/seek_trees_test.py
python3 tests/lgm_wallshot_test.py
python3 tests/boat_diagonal_test.py
```

Each has a matching `tests/generate_*_map.py` that builds its arena, and a
committed `.map` beside it.

See **[tests/README.md](../tests/README.md)** for the headless client's own
arguments, the brain API and how to add a scenario — that document is the
reference; this is only a pointer to it.

---

## Not covered here

- **Shipping build targets, optional features, release builds** —
  [BUILDING.md](BUILDING.md#build-targets).
- **macOS signing and packaging** (`scripts/sign_macos.sh`,
  `scripts/package_macos.sh`, and the `sign_macos` / `package_macos` targets) —
  [BUILDING.md](BUILDING.md#signing-and-notarization-macos).
- **`scripts/upload-sentry-symbols.sh`** — uploads debug symbols for
  symbolicated crash reports; see the usage block at the top of the script and
  the debug-symbols section of [BUILDING.md](BUILDING.md#release-builds-and-debug-symbols).
- **Skins and tile sheets** — [SKINS.md](SKINS.md).
- **Replay logging and the `.wbv` format** — [logging.md](logging.md).
