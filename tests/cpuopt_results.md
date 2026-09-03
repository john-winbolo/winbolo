# GoalHunter 1.7 CPU optimisation — measurements

All numbers from `tests/brain_cpu_profile.py`, DH-Oil Rig, 4 bots, `-threads 4`,
`-seed 4242 -brain-lua-seed 42 -brain-no-budget-kill -brain-tier 10`, 12000 game
ticks (6000 thinks per bot). Per-think times are the host's `brain.lastThinkMs`
sampled by the harness's `[[GHPROBE]]` snippet, over all four bots.

**Read this first.** The machine drifts ~20% over an hour of back-to-back
measurement, which is larger than most of the effects below. Every comparison
here is therefore **interleaved**: A and B alternate inside one block, minutes
apart, and only numbers from the same block are compared. That is only
practical because of `-asap` (see below), which cuts a 12000-tick game from
120 s to ~12 s.

Trees used:

| name | what it is |
|---|---|
| `base` | `git archive HEAD brains/GoalHunter_1.7` — the pristine starting point |
| `full` | this branch's tree |
| `nomark` | `base` with the `opt(...)` statements deleted from `opt/` by `lua_strip --strip "opt("` |
| `+itemN` / `−itemN` | `base` or `full` with one item's files swapped in/out and `opt/` regenerated |

---

## Headline

**prod** (`opt/` brain, no debug streams) — 3 interleaved rounds, medians:

| | mean | p50 | p90 | p99 |
|---|---|---|---|---|
| before | 1.12 | 1.07 | 1.80 | 2.72 |
| after | 0.64 | 0.57 | 1.06 | 1.91 |
| | **−43%** | **−47%** | **−41%** | **−30%** |

**debug** (root brain, `-brain-debug`, all four streams) — 2 interleaved rounds:

| | mean | p50 | p90 | p99 |
|---|---|---|---|---|
| before | 3.69 | 3.57 | 5.13 | 8.52 |
| after | 2.88 | 2.73 | 4.24 | 7.29 |
| | **−22%** | **−24%** | **−17%** | **−14%** |

**allocation** (`brain_alloc_stats` via `-brain-profile-log`, 2 interleaved
rounds, mean over 4 bots):

| | KB per think |
|---|---|
| before | 108.8 |
| after | 93.1 |
| | **−14%** |

**print2 bytes** (12000-tick `-brain-debug` game = 120 s of game time):

| | per bot per game | per bot per hour | 4 bots per hour |
|---|---|---|---|
| before | 30.0 MB | 901 MB | 3.5 GB |
| after | 13.4 MB | 402 MB | 1.6 GB |
| | **−55%** | | |

---

## Per-item attribution (prod p50, ms)

Each row is from a block where that tree and its reference ran back to back.

| tree | p50 | vs its reference |
|---|---|---|
| `base` | 1.07 | — |
| `base + item 1` (guard the markers, gate the profile-only `clock_us`) | 1.04 | −3% |
| `base + item 1 + item 3 (init.lua dead preambles)` | 0.64 | −40% vs `base` |
| `base + all init.lua changes` (items 1, 3, 4, 7, 9) | 0.58 | −46% vs `base` |
| `base + everything except init.lua` (items 2, 3, 4) | 0.68 | −36% vs `base` |
| `full` | 0.57 | −47% vs `base` |
| `full − item 2` (builder_pool formula eager again) | 0.57 | 0% |
| `full − the GC call` | 0.60 | +7% |
| `nomark` (markers **deleted** from `base`, not guarded) | 0.94 | −15% vs `base` (1.10 same block) |

Three things worth knowing:

1. **Item 3 — removing lua_strip's dead preambles — is the whole story.**
   Guarding the 78 profiler markers buys 3%; taking the KWDIAG token tables and
   the `goal_str` block out of `Brain.think` buys 40%. Neither block costs
   anything obvious on paper (KWDIAG runs every 8th tick, `goal_str` is one
   `string.format`). The size of the win points at `Brain.think` being a ~7000
   line function and LuaJIT's trace recorder, not at the arithmetic: dead code
   in the middle of it is expensive out of proportion to what it does.

2. **The two halves are not additive.** init.lua alone gets `base` to 0.58 and
   everything-but-init alone gets it to 0.68, but together they only reach 0.57.
   They are competing for the same bottleneck.

3. **`nomark` beats `base + item 1`** (0.94 vs 1.04): deleting a marker is worth
   five times more than guarding it, for the same reason as (1) — the guarded
   form still leaves the `string.format` bytecode inside `Brain.think`.
   `lua_strip --strip "opt("` would collect the rest, but it also removes the
   ability to profile the production brain at all (`-brain-profile-log` runs the
   `opt/` tree; the harness's `prodprof` mode depends on it), so this is left as
   a decision rather than taken.

## LuaJIT GC parameters

`opt/init.lua` called `collectgarbage("generational", 10, 100)`, which the 5.4→5.1
compat shim in `luabrainshandler.c` swallows — the brain ran on LuaJIT's stock
settings. Replaced with `setpause`/`setstepmul` (`C.GC_PAUSE` / `C.GC_STEPMUL`).
Sweep, prod, all bots (timer-paced, sequential — see the drift warning; the
repeats at the bottom are the ones that calibrate it):

| pause/stepmul | mean | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| 200/200 (LuaJIT default) | 0.80 | 0.74 | 1.26 | 2.12 | 10.45 |
| 150/200 | 0.82 | 0.76 | 1.30 | 2.20 | 9.33 |
| 300/200 | 0.81 | 0.75 | 1.29 | 2.14 | 12.38 |
| 150/400 | 0.79 | 0.73 | 1.27 | 2.25 | 7.55 |
| 200/400 | 0.77 | 0.71 | 1.25 | 2.11 | 10.27 |
| 250/400 | 0.78 | 0.72 | 1.22 | 2.15 | 9.60 |
| 200/600 | 0.76 | 0.70 | 1.22 | 2.20 | 8.21 |
| 200/200 (repeat) | 0.76 | 0.70 | 1.22 | 2.12 | 8.40 |
| 200/400 (repeat) | 0.75 | 0.69 | 1.21 | 2.12 | 9.78 |
| 200/800 (repeat) | 0.77 | 0.70 | 1.22 | 2.31 | 7.86 |

The repeats move by 0.04 ms, which is the size of the whole sweep: the knobs do
not move this workload. 200/400 is kept (a hair ahead on p50/p90 in both
samples, behind in neither). Making the call at all is still worth 0.60 → 0.56
in the interleaved table above, because what it replaced did nothing.

---

## `-asap`

New WinBoloDS flag: run game ticks back-to-back instead of one per 20 ms of wall
clock. The timer is not armed; a dedicated thread runs the `serverGameTimer`
body without the elapsed-time gate, under the same `g_serverTickLock` and the
same shutdown handshake, incrementing the same `ticks` counter.

| run | timer-paced | `-asap` | speed-up |
|---|---|---|---|
| 12000-tick prod (opt/, 2v2 1.6 vs 1.7) | 120 s | 12 s | **10.0x** |
| 12000-tick debug (`-brain-debug`) | 120 s | 67 s | **1.8x** |

Identity: both runs' `-snapjson -snapinterval 1` series are **byte-identical** to
the timer-paced baseline. The debug run's `print2_bot*.log` differ only in the
pre-existing `pairs()`-order noise described below.

---

## Proof of zero behaviour change

Recipe (run from the worktree root, `build-wt/WinBoloDS.exe`):

```
WinBoloDS -map "data/maps/DH-Oil Rig.map" -gametype tournament -ai yes -nolobby \
  -notracker -nowinbolonet -dontsendlog -noinput -bots 4 -threads 4 -teams 2,2 \
  -brain-no-budget-kill -brain-lua-seed 42 -brain-tier 10 -allow-unsafe-brains \
  -seed 4242 -ticks 12000 -snapinterval 1 -asap -snapjson <out.jsonl> \
  -brain    brains/GoalHunter_1.7[/opt]/init.lua \
  -bot-init "0-1=brains/GoalHunter_1.6[/opt]/init.lua,2-3=brains/GoalHunter_1.7[/opt]/init.lua"
```

(add `-brain-debug` and drop the `/opt` for the debug run)

| check | result |
|---|---|
| prod `-snapjson` (12001 lines, 31 MB), before vs after | **byte-identical** |
| debug `-snapjson` (31 MB), before vs after | **byte-identical** |
| `WinBoloUnitTests` | 443 pass, same 2 pre-existing failures as the untouched main build (`attribution_reader_old_wbv`, `lang_name_table`) |
| `builder_pool_test`, `take_cover_test`, `defend_repair_test`, `refuel_stickiness_test`, `spawn_escape_test`, `harvest_follow_through_test` | all pass (`--build build-wt`) |

### print2 logs

Bots 0 and 1 in the identity game run the **unmodified** GoalHunter_1.6 brain,
so they are a control for what two runs of identical code look like. After
normalizing away the `[x.xxms]` os.clock prefix, lines whose existence is gated
on a measured duration, durations embedded in other lines, and the source
line-number column (editing a .lua file shifts it):

| | non-SYNC_P6 first divergence | SYNC_P6 lines | SYNC_P6 redundant repeats | co-printed ticks disagreeing |
|---|---|---|---|---|
| bot0 (control, 1.6) | line 42861, a `CAPTURE_EVICT` pair in the other order | 162730 vs 163690 | 162053 | 7 (0.004%) |
| bot1 (control, 1.6) | line 38771, same | 142738 vs 144082 | 141244 | 5 (0.004%) |
| bot2 (1.7) | line 67027, same | **1328 vs 147395 (−99.1%)** | **0** | 1 (0.075%) |
| bot3 (1.7) | tail length only | **2381 vs 126834 (−98.1%)** | **0** | 13 (0.546%) |

Reading that table:

* The **control bots show the same two divergence classes as the changed bots**,
  so both are pre-existing run-to-run noise, not something this branch caused.
  Both come from walking a table with `pairs()`: `CAPTURE_EVICT` emits its lines
  in hash order, and the `SYNC_P6` entry dump prints `_reject_in` as it stands
  when the sweep reaches that entry, which the visit order can change. LuaJIT
  hash order depends on allocation addresses, so it differs between runs of
  identical code. None of it reaches a decision — that is what the byte-identical
  `-snapjson` proves.
* **`redundant repeats: 0`** on the changed bots is the edge trigger's own
  guarantee, checked inside a single log where no cross-run noise can reach it:
  no key ever prints the same text twice in a row.
* The disagreement rate is measured only on ticks where **both** logs printed the
  key, so edge-triggering cannot inflate it. It sits at the control's noise
  floor, on a sample 100x smaller.

Tail length varies by a few hundred lines between any two runs (the control
bots too): the run ends mid-batch and print2 hands off on a wall-clock timer.
