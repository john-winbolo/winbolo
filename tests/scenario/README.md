# The scenario arenas

A hundred and forty-one small maps, each with a scenario script beside it.
Most are built to make one bot decision happen on purpose and then say whether
the bot made it. Thirty-four check a shipped mod instead: the twenty-three
`pilltag_*` arenas, the seven `rule_roulette_*` arenas and the four `virus_*`
arenas. `deep_sea_safe_rule` checks one gameplay rule. The four `tutorial_*`
arenas check the shipped Tutorial map's script: the checkpoint respawn, the
station resets, the Station 5A demo take and the Station 7 bot keeping to
its island.

They came from the branch `survival-scenario-bot-improvements-merged`, where
they ran on a server-side scenario host that no longer exists. They run here
on the host that is on main — `src/scenario/`, documented in
[../../docs/SCENARIO_API.md](../../docs/SCENARIO_API.md).

## What is here

| | |
|---|---|
| `<name>.scenario.lua` | one arena: the set-up, and the verdict it says |
| `maps/<name>.map` | the map that arena is played on |
| `scenario_compat.lua` | the prelude that lets an arena keep the shape it was written in |
| `run_gate.py` | runs them all and reports |

## Running them

From the repository root, with `<dir>` the build directory `WinBoloDS` was
built into — left out, `--build` looks in `build-own`:

```
python3 tests/scenario/run_gate.py --build <dir>
```

and to run one family, or one arena:

```
python3 tests/scenario/run_gate.py --build <dir> --only heat_pill
python3 tests/scenario/run_gate.py --build <dir> --only water_pills --jobs 1
```

`--jobs N` caps how many run at once. Each arena is its own private server
on a port the machine picks, which the server prints as `[UDP SERVER]
listening on UDP port N` in that arena's own log. Per-arena logs land in
`<build>/scenario_gate_logs/`. The runner exits 0 when everything that was
meant to pass passed, and an arena marked `expect=fail` that passes counts
against it.

### Playing Survival headless, outside the gate

Not an arena — the shipped map and its script, run by hand to watch a whole
round go past. From the build directory:

```
WinBoloDS.exe -map ../data/maps/Survival.map -port 0 -gametype open \
  -nolobby -bots 4 -allybots 1 -brain ../brains/GoalHunter/init.lua \
  -ai yes -seed 42 -ticks 6000 -asap -brain-no-budget-kill \
  -brain-lua-seed 42 -nowinbolonet -threads 12
```

`-port 0` lets the machine pick one. The port to join is the N in the
server's `[UDP SERVER] listening on UDP port N` line.

**`-allybots 1` is not optional.** `-nolobby` seats its `-bots` on no team at
all, and team 0 is no team: the script's own test for a defender asks which
team a seat is on, finds nobody on team 1, and the round opens with an empty
keep — no pills dealt, no bot dug in, and the waves storming an island nobody
is holding. `-allybots 1` puts all four on team 1, which is the team the
script's lobby template calls the defenders'. A lobby run needs none of this:
the host seats the template and the Add Bot button puts each bot on a team.

The map and the script both have to be beside the binary, so copy them over
after editing either: `data/maps/Survival.map` and
`data/maps/Survival.scenario.lua` into the build directory's own
`data/maps/`.

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

An arena that checks a shipped script names it, from the repository root:

```
-- GATE: include=data/mods/RuleRoulette.scenario.lua
```

The runner writes that file, unchanged, between the prelude's head and the
arena. The arena is in the same chunk, so the script's top-level locals are in
scope: the `rule_roulette_*` arenas fill the `queue` of Rule Roulette's
`TANK` and `BUILDER` tracks to force the mode order, read each track's `mode`
to know which one is in force, and call the script's own `command` (the
`!roulette` chat commands) and `build_panel`. The arena then has to
put a `scenario` table of its own over the script's (a mod may not end the
round, and the verdict does) and take over the script's hooks, which take no
`game` argument, so the tail does not wrap them a second time.

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

## Known gaps

**A bot the lobby seats gets no `init`.** The old host asked a script for each
team's brain mode and difficulty. The host here reaches the same place for a
bot a script spawns — `spawn_bot{ init = { mode = ..., difficulty = ... } }` —
but a bot the lobby seats (`scenario.lobby.teams[].bots` with
`fielded = true`) is built with no init at all. Survival works around it by
seating its horde `fielded = false` and fielding every one of them through
`spawn_bot`, which carries the init.

**How many bots the enemy side gets is a constant.** The old host called a
script to ask how many AI seats the enemy side should have.
`scenario.lobby.teams[].bots` here is a number in a table, decided when the
chunk runs, so what is shipped is a constant.

**Nothing can see a bot think.** The old python drivers asserted on the
brain's own printed reasoning — the goal pool for a tick and what each
candidate was priced at, the builder pool's dispatch and denial rows, the
blitz and heat lines. None of that reaches a scenario, which is why a dozen
arenas stay marked `skip`.
