# The scenario arenas

Ninety small maps, each with a scenario script beside it, each built to make
one bot decision happen on purpose and then say whether the bot made it.

They came from the branch `survival-scenario-bot-improvements-merged`, where
they ran on a server-side scenario host that no longer exists. They run here
on the host that is on main — `src/scenario/`, documented in
[../../docs/SCENARIO_API.md](../../docs/SCENARIO_API.md).
[../../PORT_MAP.md](../../PORT_MAP.md) is the key that moved them.

## What is here

| | |
|---|---|
| `<name>.scenario.lua` | one arena: the set-up, and the verdict it says |
| `maps/<name>.map` | the map that arena is played on |
| `scenario_compat.lua` | the prelude that lets an arena keep the shape it was written in |
| `run_gate.py` | runs them all and reports |

## Running them

From the repository root, with `build-own` built:

```
C:\Python310\python.exe tests/scenario/run_gate.py
```

and to run one family, or one arena:

```
C:\Python310\python.exe tests/scenario/run_gate.py --only heat_pill
C:\Python310\python.exe tests/scenario/run_gate.py --only water_pills --jobs 1
```

`--build <dir>` points it at a different build directory, `--jobs N` caps how
many run at once, and `--port-base N` moves the private server ports if
something else on the machine wants them. Per-arena logs land in
`<build>/scenario_gate_logs/`. The runner exits 0 when everything that was
meant to pass passed.

## How one arena runs

The runner makes a working directory, copies the map into it, and writes the
scenario file beside it as three pieces joined into one:

1. the prelude's **head** — the old `game` rows, the verdict helpers,
2. the **arena's own text**, unchanged,
3. the prelude's **tail** — which takes each hook the arena declared and puts
   a wrapper of the host's shape in its place.

One file, because the map's discovery reads one file, and because the sandbox
a scenario runs in has no `require` and no `dofile` to load a second.

Then it runs `WinBoloDS` on that map and reads the verdict off standard
output.

## The verdict

`game.end_round`'s text goes to the lobby and never to the console, so an
arena says its answer with `game.log`:

```
VERDICT PASS <name> <why>
VERDICT FAIL <name> <why>
```

`game.log` drops a line past 128 bytes without saying so, so the prelude cuts
one rather than losing it. An arena that says nothing before its tick limit
fails with "no verdict".

Most arenas state a `VERDICT_CHECK` and let the prelude ask it just before the
round is cut off. That is where the python drivers these came from used to
read the final state, so it is the same moment and the same reading.

## The GATE line

An arena states what the server needs on one comment line:

```
-- GATE: ticks=24000 bots=1 ai=yesfull gametype=open
```

Anything left out takes `ticks=6000 bots=1 gametype=open ai=yesfull limit=20
seed=42`.

Two other forms mark a debt, and each carries its reason:

```
-- GATE: expect=fail <why this is known to fail>
-- GATE: skip=<why this cannot run>
```

An arena marked `expect=fail` that starts passing is reported as `UPASS`, so a
mark that has gone stale is noticed rather than hiding a fix.

## Traps, all of them paid for once already

**`io` is not in the sandbox**, and neither is `os.getenv`. Every arena that
came across opened a trace file for its python driver to read; that raises on
the first tick and the scenario is switched off after twenty errors, so the
arena fails at the door. Keep what the driver wanted from the file in a local
instead.

**`spawn_bot` with no `slot` answers `true, "queued"`**, where the old host
answered the seat. Name a slot, and remember `on_choose_start` fires during
the spawn, so set anything that hook reads first.

**`game.tick()` holds one parity for a whole round.** It steps by 2, so
`tick % PERIOD == 0` with an even period fires on half the rounds and never on
the rest. Count down to a due tick instead.

**The hook clock doubled.** Any number inside an arena meant as a duration
means half the time it used to. The drivers' own `-ticks` budgets were already
in the same unit and did not need doubling.

**Team 0 means no team.** Two bots on team 0 are not allies, so an arena
measuring what they say to each other measures nothing.

**`hide_pill` is a real `remove_pill`**, so `game.pill(n)` stops answering for
a hidden pill where the old host still answered. An arena that resolves its
pill list twice finds nothing the second time.

**Two pillboxes may not share a square**, where the old host allowed it. An
arena that refills a corpse onto an occupied square is refused.

**A tank pockets a dead pillbox by driving over it**, goal or no goal. Pricing
capture out of the goal pool only stops the bot routing to a corpse; the
tracks still take it. An arena whose errand is a corpse wants `can_capture`
answering `false`.

**The console's `Bot N: brain init arg '...'` line does not show a scenario's
init.** That line is the command-line string; a spawn's `init` table arrives
separately as `BRAIN_INIT` and the brain flattens it in Lua afterwards.

**A token whose value is `"0"` is dropped by the flattening**, which is right
for a flag being switched off and wrong for a brain that wants a number of
zero. Hand a bot its pins with `game.init_tokens("k=v;k=v")` rather than as a
table of pairs and the question does not arise.

## Two things to know before writing one

**The clock doubled.** The old host's `game.tick()` counted 50 a second. This
one counts 100, and the `tick` a hook is handed goes up by 2 each frame. A
tick number carried over from the old host means half the time it used to.

**A scenario cannot see the brain think.** Several of the old python drivers
asserted on brain debug output — goal traces, decision lines a brain printed.
None of that reaches a scenario, so those assertions did not move. Each arena
that lost one says which one, in its own comments, and is marked `skip`.
