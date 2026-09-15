# Porting our scenarios onto John's scenario host

This document maps the scenario surface our Survival map and our 90 test
scripts were written against — the old `src/server/scenario.c` host, where a
`game` table was handed to every hook — onto the host that is on main:
`src/scenario/`, documented in [docs/SCENARIO_API.md](docs/SCENARIO_API.md).

The old host is dead. No C from it is ported. What moves is content, and this
document is the key that lets the content move.

Three verdicts are used throughout.

- **direct** — John has the same row under the same name, and the call needs
  no help beyond the hook-shape change every file needs.
- **prelude** — the call is expressible on John's rows, and the shim that
  expresses it lives in `tests/scenario/scenario_compat.lua`, which the gate
  runner concatenates in front of a script. No script body changes.
- **gap** — John has no row that expresses it. The entry names the smallest
  host op that would close the gap. **No C is added by this port**; these are
  Andrew's to decide.

---

## The one change every file needs

Our hooks took the `game` table as their first argument:

```lua
function on_setup(game)   ... end
function on_tick(g, tick) ... end
function on_choose_start(g, p) ... end
```

John's hooks take no such argument and read a global `game`:

```lua
function on_setup()       ... end
function on_tick(tick)    ... end
function on_choose_start(p) ... end
```

91 of our 92 scenario files are affected. Rewriting 92 files by hand for this
would be 92 chances to make a typo, so the compat prelude does it instead. The
prelude is concatenated **in front of** the script and installs a metatable on
`_G` that catches the moment a hook name is assigned, keeps the function the
script wrote, and puts a wrapper of John's shape in its place:

```lua
_G.on_tick = function(tick) return user_on_tick(game, tick) end
```

The sandbox has no `require` and no `dofile` (#339), so a script cannot load
the prelude itself. Concatenation by the runner is the only way in, and it is
also the only way the file the server reads stays one file, which is what the
`<map>.scenario.lua` discovery expects.

---

## Hooks

| ours | John | verdict |
|---|---|---|
| `on_setup(game)` | `on_setup()` | prelude (shape) |
| `on_start(game)` | `on_start()` | prelude (shape) |
| `on_tick(game, tick)` | `on_tick(tick)` | prelude (shape) |
| `on_choose_start(game, p)` | `on_choose_start(p)` | prelude (shape) |
| `allow_base_win(game)` | `allow_base_win()` | prelude (shape) |
| `on_lobby(game)` | `on_lobby(p, scripted)` | prelude (shape) — see note |
| `bot_mode(game, team)` | — | **gap** |
| `enemy_bots(game)` | — | **gap** |
| `show_add_team_button(game)` | `allow_extra_teams()` | prelude (shape), near equivalent |

**`on_lobby` is not the same hook.** Ours was called once when the lobby
settled and asked the script to arrange it. John's is called every time a
lobby seat changes, and is handed the seat. Only Survival declares it, and
Survival's use of it is arranging seats — which John's `scenario.lobby` table
declares instead, without a hook. Survival's `on_lobby` body is therefore
ported by hand into `scenario.lobby`, not shimmed.

---

## `game` rows we call

29 distinct rows, in call-count order across all 92 files.

| ours | uses | John | verdict |
|---|---|---|---|
| `message(text)` | 234 | `message(text[, target])` | direct |
| `pill(n)` | 193 | `pill(n)` | direct |
| `set_tile(x, y, t)` | 150 | `set_tile(x, y, t)` | direct, narrower square range |
| `num_pills()` | 143 | `num_pills()` | direct |
| `set_pill_owner(n, p)` | 136 | `set_pill_owner(n, p)` | prelude — `nil`/`-1` means neutral |
| `tank(p)` | 132 | `tank(p)` | direct, John's table has more fields |
| `num_bases()` | 105 | `num_bases()` | direct |
| `set_team(p, t)` | 104 | `set_team(p, t)` | prelude — refused in `on_setup` |
| `set_base_owner(n, p)` | 91 | `set_base_owner(n, p[, keep_stock])` | prelude — `nil`/`-1` means neutral |
| `set_base_stock(n, a, s, m)` | 80 | `set_base_stock(n, armour, shells, mines)` | direct |
| `base(n)` | 79 | `base(n)` | direct |
| `spawn_bot(name, brain, team, mode, init, start)` | 60 | `spawn_bot{ ... }` | prelude — positional to table |
| `set_pill_armour(n, a)` | 54 | `set_pill_armour(n, a)` | direct |
| `hide_pill(n)` | 48 | — | prelude on `remove_pill` |
| `NEUTRAL` | 38 | `NEUTRAL` | direct, both 255 |
| `map_tile(x, y)` | 24 | `map_tile(x, y)` | direct |
| `show_pill(n[, x, y])` | 21 | — | prelude on `add_pill` |
| `kill_lgm(p)` | 21 | `kill_lgm(p[, killer])` | direct |
| `give_pill(p, n)` | 18 | `give_pill(p, n)` | direct |
| `num_starts()` | 8 | `num_starts()` | direct |
| `end_round([text])` | 8 | `end_round([text[, winner_team]])` | direct |
| `max_tanks()` | 7 | `max_tanks()` | direct |
| `remove_bot(p)` | 6 | `remove_bot(p)` | prelude — refused in `on_setup` |
| `lobby_slot(p)` | 6 | `lobby_slot(p)` | direct, John's table has more fields |
| `add_pill(x, y, ...)` | 4 | `add_pill(x, y[, owner[, armour[, speed]]])` | direct |
| `log(text)` | 3 | `log(text)` | direct, 128-byte line cap |
| `tick()` | 2 | `tick()` | direct |
| `start(n)` | 1 | `start(n)` | direct |
| `newswire_mute(on)` | 1 | — | prelude on the `announce()` policy |
| `lobby_remove_bot(p)` | 1 | `lobby_remove_bot(p)` | prelude — refused in `on_setup` |
| `enemy_team_size()` | 1 | `team_size(2)` | prelude |

---

## The shims, in detail

### Neutral by `nil`

Our host read a missing or negative owner as `NEUTRAL`. John's host raises on a
missing argument, because a missing argument is a mistake in the script rather
than a value the world will not take. The prelude coerces:

```lua
local function owner(p)
  if p == nil or p < 0 then return game.NEUTRAL end
  return p
end
```

and wraps `set_pill_owner` and `set_base_owner` in it.

### `spawn_bot`, positional to table

Ours: `spawn_bot([name][, brain][, team][, mode][, init][, start])`, where
`init` is one **string** of `key=value;key=value` tokens that reaches the brain
as the `BRAIN_INIT_ARG` global.

John's: one table, with `loadout` where ours said `mode`, and an `init` that is
a **flat table** of at most 16 pairs.

The two inits meet exactly. `scnTableParse` in `src/bolo/scenario_table.c`
turns `k=v;k=v` into the table John wants and `scnTableFormat` turns it back
into the string the brain reads, so the prelude splits our string on `;` and
hands John the pairs. A bare token with no `=` becomes the value `"1"`, which
is what the C parser does with one too.

The prelude also accepts a table as the first argument and passes it straight
through, so a ported file may be written in John's own shape.

### `hide_pill` / `show_pill`

Ours took a pillbox off the map with no carrier, remembering where it was, and
put it back dead. The point of it is that a hidden pill cannot be seen, shot,
driven over, repaired or captured — which matters because the bots' own goal
search wants a dead neutral pill on the ground, and half the `kill_me` and
`capture_lgm` arenas exist to measure exactly what the bots do when one is or
is not there. Parking a pill in a map corner is therefore **not** equivalent:
the brain would still route to it.

John has `remove_pill(n)` and `add_pill(x, y, owner, armour, speed)`, which
together do the right thing except for one detail — the slot. `pillsAddItem`
gives out **the lowest free slot**, so putting one pill back while two are
hidden can hand it a number that is not the one it had, and our scripts name
pills by number.

The prelude therefore forces the slot. To show pill `n`:

1. re-add every still-hidden pill whose number is below `n`, in ascending
   order, each at its own remembered square — each lands on its own slot,
   because slots fill lowest-first;
2. add `n`, which now lands on `n`;
3. remove again the ones that were only put back to hold their place.

All of this happens inside one hook call, and John's `add_pill` and
`remove_pill` both apply immediately rather than queueing, so no tick passes
and no client ever sees the placeholders.

The prelude keeps the remembered record — `x`, `y`, `owner`, `armour`,
`speed` — so `show_pill(n)` with no square lands it back where it was, dead,
exactly as ours did.

### `newswire_mute`

Ours silenced the engine newswire — joins, quits, captures — everywhere, while
leaving script messages and chat alone. John has the `announce(kind, subject,
actor)` policy, which is asked before every newswire line and keeps the line
off every client when it answers `false`. The prelude holds a flag and installs
an `announce` that answers `false` while the flag is up. It is the better shape:
ours was a global switch and John's is per line.

The kinds John's policy covers — `joined`, `left`, `base_captured`,
`pill_captured`, `builder_lost`, `name_changed`, `alliance`, `vote` — are the
lines ours silenced.

### `enemy_team_size`

Ours answered the current size of lobby team 2. John's `team_size(t)` answers
any team's, so the prelude is `game.team_size(2)`.

### Roster ops inside `on_setup`

John refuses `spawn_bot`, `remove_bot`, `set_team`, `lobby_add_bot`,
`lobby_remove_bot` and `lobby_set_team` inside `on_setup` with
`SCN_OP_WRONG_STATE`: the round is still being built there and a roster edit
would re-enter the machinery building it. Ours allowed them, and 104 of our
`set_team` calls are in `on_setup` — the line that puts our tank on its own
team so the scripted opponents are hostile to it.

The prelude queues a roster op issued from `on_setup` and flushes the queue at
the top of `on_start`, before the script's own `on_start` runs. The cost is
that the teams settle on the round's first running tick rather than before it.
No test we have reads a team before its first tick, but this is the one
behaviour difference the port introduces on purpose, and it is written here so
a test that starts behaving oddly is looked at here first.

### Square range on a write

John refuses a write naming a square outside 21..235 on either axis — the sea
frame the map is drawn in. Ours allowed the whole 0..255. Every arena we ship
works well inside the range and Survival's island is centred on (128, 128) with
a radius under 30, so nothing is lost. A future generated map that wants to
write the frame cannot.

---

## Gaps

These are the rows and hooks that John's host has no arm for. Nothing in this
port adds C. Each entry names the smallest op that would close it.

### 1. `bot_mode(game, team)` — a per-team brain mode

Ours asked the script, for each team, which brain **mode** and **difficulty**
key that team's bots run, and the answer reached the brain as the
`mode=` and `difficulty=` `BRAIN_INIT_ARG` tokens. Survival uses it to make
the horde play one way and the defenders another without two brain files.

John reaches the same place for a bot the **script** spawns —
`spawn_bot{ init = { mode = "...", difficulty = "..." } }` — so Survival's
horde is covered. What is not covered is a bot the **lobby** seats
(`scenario.lobby.teams[].bots` with `fielded = true`): those are built by the
server with no init at all.

**Smallest op that closes it:** an `init` field on `scenario.lobby.teams[]`,
the same flat table `spawn_bot` takes, handed to every bot the lobby seats for
that team. It needs no new call and no new hook — one more field read where the
team table is already parsed.

**What the port does instead:** Survival seats its horde with `fielded = false`
and fields every one of them through `spawn_bot`, which carries the init. The
defenders are human seats and never needed a mode.

### 2. `enemy_bots(game)` — how many bots the lobby seats for the enemy

Ours was a query hook: the lobby asked the script how many AI seats the enemy
side should get, and the script answered from its own wave table.

John's `scenario.lobby.teams[].bots` is a **number in a table**, decided when
the chunk runs, not a function the lobby calls. For Survival the number is a
constant, so the table expresses it.

**Smallest op that closes it:** allow `bots` to be a function as well as a
number, called once as the lobby is built. Not needed by anything we ship.

**What the port does instead:** the constant goes in the table.

### 3. `show_add_team_button(game)` — whether the lobby offers a new team

John's `allow_extra_teams()` policy is asked the same question at the moment a
host would put a seat on a team nobody is on, and `false` refuses it. The names
differ and the moment differs slightly — ours decided whether to draw a button,
John's decides whether to honour the press — but the effect a host sees is the
same, so this is a prelude entry and not a gap. It is listed here because the
two are not identical and a reviewer should know which was chosen.

### 4. A bot's `init` table — CLOSED, brain-side

This was the gap that cost the most, and it is fixed. `spawn_bot{ init = {...} }`
is carried through the roster op and handed to the new brain as the
`BRAIN_INIT` global, a table; the GoalHunter brains read `BRAIN_INIT_ARG`, a
string of `k=v;k=v` tokens, and read nothing else. Every token a scenario
meant for one bot arrived somewhere the brain never looked.

**Closed by `202ea93d`**, in the brain rather than in the host:
`brains/GoalHunter_1.7/init.lua` flattens the table into that string before
either parse block runs. Keys sorted so every bot builds the same string from
the same table; a value of `"1"` becomes the bare flag word the tick-1 parser
matches (`noblitz`, `suicider`, `noclaimdead`); `"0"` is dropped, which is how
a flag is switched off; everything else stays `k=v`. A string already in
`BRAIN_INIT_ARG` — the command-line path — keeps its place and the table's
tokens follow it.

Two things about the shape are worth knowing, because they are not obvious
and the test arenas all hit them. A Lua table has one key named `cfg`, so it
carries one `cfg=` pin; and a table value is 64 bytes, where a driver's pin
set runs to three hundred. Both fall to the same observation: the brain
**splits the flattened string on `;`** and ignores a token it does not
recognise, so a value that opens with a `;` puts its own `key=` on one side of
a split, where nothing reads it, and whole tokens on the other.
`game.init_tokens(...)` in `tests/scenario/scenario_compat.lua` does that
chunking, so an arena hands a bot its driver's token line unchanged.

**Still open, and Andrew's to decide:** there is no way to hand a RUNNING bot
new data. `init` is spawn-time only. The planned op is
`game.bot_init(p, table)` — update that bot's `BRAIN_INIT` table in its own
Lua state and call `Brain.on_init(table)` where the brain defines one, with
GoalHunter re-parsing its tokens on that call.

### 5. A pill that is hidden rather than removed

The `remove_pill` + slot-forcing `add_pill` shim above is exact for everything
our scripts do with `hide_pill`, but it is not free: it costs three ops where
ours cost one, and it depends on `pillsAddItem` handing out the lowest free
slot, which is an implementation detail rather than a documented promise.

**Smallest op that closes it:** `game.set_pill_hidden(n, on)` — one op that
clears the pill's active flag while keeping its record and its slot, which is
what our host's `hide_pill` did. It would make the shim three lines instead of
thirty and would not depend on slot allocation order.

**What the port does instead:** the shim, with a `VERDICT` line from any
script whose slot forcing does not come back with the number it asked for, so
a drift is a test failure rather than a silent wrong answer.

### 6. Nothing can see a bot think

This is what stops the arenas that stay skipped, and there are a dozen of
them. The old python drivers read the brain's own printed reasoning — the
goal pool for a tick with what each candidate was priced at, the builder
pool's `BP_DISPATCH` and `BP_DENY <reason>` rows, `BLITZ_GO` and
`BLITZ_CONTESTED`, `HEAT_PILL` / `HEAT_SHOT` / `HEAT_EXIT`, `SEA_RELEASE`,
`REFUEL_P1`'s chips, `PLACE_PILL_GATE`'s forced flag. None of that reaches a
scenario, and in most of those arenas the world ends up the same shape
whichever way the brain decided — which is exactly why the arena was built
around the reasoning rather than around the outcome.

**Smallest op that would close most of it:** one read that answers a named
bot's goal pool for the tick it is asked on — each candidate, its goal name
and its final price. That one reading is what the refuel, place-pin, farm
sectors, builder pool and blitz arenas all want. It is a read, not a write,
and it is the brain's own state rather than the sim's, so it would have to be
fetched from the bot's Lua state the way a bot hint would be pushed into it.

Two smaller ones would close the rest: a `on_pill_placed` that carries the
goal that ordered the placement (or a `can_build` told whether the order is
the emergency one), which is all `place_pin_near` wants; and a way to tell
which of two code paths repaired a pill.

Whether any of this should exist is a real question — a scenario reading a
brain's mind is a different thing from a scenario arranging a world — and it
is Andrew's to answer.

---

## Where the content lands

| what | where |
|---|---|
| `Survival.map` + `Survival.scenario.lua` | `data/maps/` — beside the map, which is where discovery looks |
| `RespawnLoadoutTest.map` + script | `data/maps/` |
| the 90 test scripts and their maps | `tests/scenario/` |
| the compat prelude | `tests/scenario/scenario_compat.lua` |
| the gate runner | `tests/scenario/run_gate.py` |

`tests/roost/` is not created: it does not exist on main, it exists on the
bot-commands branch only, and these tests are not the ROOST set.
