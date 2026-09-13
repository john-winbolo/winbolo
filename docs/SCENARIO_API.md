# The WinBolo scenario API

**API version 1.**

A scenario is a Lua file that sits beside a map and changes what a round on
that map is. It can seat the lobby before anyone joins, set the numbers the
simulation runs on, move tanks and pillboxes about, put bots on the field in
waves, talk to the players, and decide when the round is over and who won.

This document describes every call, every hook and every field the server
offers a scenario. It is written against the server's own binding table, so
where this document and the server disagree, the server is right and this is a
bug worth reporting.

---

## Contents

- [Where a scenario lives](#where-a-scenario-lives)
- [A worked example: Wave Defense](#a-worked-example-wave-defense)
- [The `scenario` table](#the-scenario-table)
- [Hooks](#hooks)
- [How the `game` table behaves](#how-the-game-table-behaves)
- [Reading the world](#reading-the-world)
- [Timers](#timers)
- [Tags and regions](#tags-and-regions)
- [Changing the world](#changing-the-world)
- [Bots and seats](#bots-and-seats)
- [Talking to players, and ending the round](#talking-to-players-and-ending-the-round)
- [Rules](#rules)
- [Constants](#constants)
- [Terrain codes](#terrain-codes)
- [Sounds](#sounds)
- [Refusal codes](#refusal-codes)
- [Limits](#limits)
- [Checking a scenario before you run it](#checking-a-scenario-before-you-run-it)
- [What is not here yet](#what-is-not-here-yet)

---

## Where a scenario lives

A scenario is found by the map's file name. `Wave Defense.map` is played with
`Wave Defense.scenario.lua` beside it; a map with no such file beside it is an
ordinary map. Nothing else points the server at a scenario, so copying a map
to a new name and writing a sidecar for the copy is how you attach a scenario
without changing how the original map plays.

The server reads the file once when the map is loaded and runs it again at the
start of every round, each time in a Lua state of its own. That means:

- **Your chunk's top level runs before any round.** Declare your functions and
  your `scenario` table there. Do not call into `game` at the top level and
  expect an answer about the world: there is no round yet.
- **Globals do not survive a round.** Every round gets a fresh state, so
  nothing the last round left behind is reachable from this one. Counters you
  want to keep across a round belong in the file's own locals, which are set
  again each time the chunk runs.
- **A sidecar that will not load is skipped.** The round is played as a plain
  map and the operator is told why.

The file must be under 1 MiB.

---

## A worked example: Wave Defense

`tests/baseline/maps/Wave Defense.scenario.lua` is the reference scenario.
Read it alongside this document. Its shape is the shape most scenarios take:

```lua
local DEFENDERS = 1
local RAIDERS   = 2
local WAVES     = { 2, 4, 6 }

local wave, standing, seats, wave_timer, over = 0, 0, {}, nil, false
```

**The lobby is declared, not built.** The `scenario` table asks for six seats
on team 2 with `fielded = false`. The server puts them in the roster before
anyone joins, where a host can see and trim them, and loads no brain for any
of them until something fields one:

```lua
lobby = {
  max_players = 4,
  teams = {
    { id = DEFENDERS, bots = 0, max_bots = 0 },
    { id = RAIDERS,   bots = 6, max_bots = 8, fielded = false },
  },
},
```

**The round is driven by hooks and timers, never by polling.** `on_start` runs
on the first tick of the round; from there each wave sets a timer for the next
step, and the hooks the server calls report what happened:

```lua
function on_start()
  seats = horde_seats()
  hand_over_the_keep(enlist_the_defenders())
  game.message("Wave Defense: hold the keep through 3 waves.")
  game.timer(2, next_wave)
end
```

**A held seat is fielded by naming it.** `spawn_bot` with a `slot` puts a bot
into a seat the lobby was holding; the seat carries the name and the team it
was seated with, so the spawn need not restate either:

```lua
game.spawn_bot{ slot = seats[i], start = i }
```

**A seat goes back when the wave ends.** `remove_bot` on a seat a scenario
fielded returns it to being held rather than emptying it, so the next wave
uses the same seats again.

**The scenario decides the ending.** `end_round` stops the round then and
there, with the line the lobby will show:

```lua
game.end_round("The keep held.", DEFENDERS)
```

If you write one scenario, write this one first and change it.

---

## The `scenario` table

A global table named `scenario`, declared at the chunk's top level. Every field
is optional except in the sense that a scenario with no `name` is a scenario
nobody can identify.

```lua
scenario = {
  name        = "Wave Defense",
  description = "Hold the four pillboxes at the centre of the map.",
  api         = 1,
  game        = "open",
  bound       = true,
  lobby       = { ... },
  rules       = { ... },
  tags        = { ... },
  regions     = { ... },
}
```

| Field | Type | Meaning |
|---|---|---|
| `name` | string | What the scenario is called. |
| `description` | string | One or two sentences for a host reading a list. |
| `api` | number | The API version you wrote against. Defaults to 1. A server older than the version you name refuses the scenario rather than running it half-understood. |
| `game` | string | The game type the scenario asks for: `"open"`, `"tournament"` or `"strict"`. |
| `bound` | boolean | True (the default) when the scenario is tied to its map. A scenario that names tags or regions is tied to its map by definition, because tags and regions are the map's own squares and entities. |

### `scenario.lobby`

The lobby the map opens with.

| Field | Type | Meaning |
|---|---|---|
| `max_players` | number | The cap on human players, 1 to 16. 0 (the default) leaves the server's own cap. |
| `extra_teams` | boolean | Whether a host may add teams beyond the ones listed. Defaults to false. |
| `teams` | array | One entry per team, in order. |

Each team:

| Field | Type | Meaning |
|---|---|---|
| `id` | number | The team number, 1 to 15. Two teams may not share one. |
| `bots` | number | How many seats to seat for this team when the lobby is built. A host who trims them gets the trimmed number back next round: the point of seating them where a host can see them is that the host may change them. |
| `max_bots` | number | The ceiling a host may raise `bots` to. 0 means no ceiling stated, which is not the same as no bots allowed. |
| `fielded` | boolean | True (the default) puts a bot in the seat at the start of the round. False holds the seat without one: it is in the roster, it takes no tank, and no brain loads until a `spawn_bot` names it. |
| `brain` | string | The brain this team's bots run. A path, or `package:NAME` for a brain carried by the scenario's own package. Empty means the server's own. |

### `scenario.rules`

A table of rule name to value. Names are the rule catalogue's own spellings —
the same ones `game.rule` and `game.set_rule` take. A name that spells no rule,
or a value outside a rule's range, is reported and the rest of the table still
applies.

```lua
rules = {
  tank_death_ticks  = 150,
  tank_reload_ticks = 10,
},
```

### `scenario.tags`

Names you put on the map's own entities, so the script never has to carry
their numbers. Keyed by kind, then by the entity's 1-based index, with either
one string or a list of them.

```lua
tags = {
  pills  = { [7] = "keep", [8] = "keep", [9] = { "keep", "north" } },
  bases  = { [1] = "depot" },
  starts = { [3] = "beach" },
},
```

An entity carries at most four tags; a fifth is reported and dropped. An index
past what the map holds is reported.

### `scenario.regions`

Named rectangles of map squares, by name, with an inclusive top-left corner
and a size.

```lua
regions = {
  keep = { x = 124, y = 124, w = 10, h = 10 },
},
```

A region is half-open on both axes — `x` to `x + w - 1` — so a rectangle with
no width or no height holds nothing. At most 64 regions exist at once, counting
both these and any `define_region` makes during the round.

---

## Hooks

A hook is a global function the server calls. Declare the ones you want and
leave out the rest; a hook you do not declare costs nothing.

### The round's own moments

| Hook | When |
|---|---|
| `on_setup()` | Once, after the scenario's rules are applied and before the round's first tick. Every write is available except the ones that add or remove a seat. This is where the map gets ready. |
| `on_start()` | The round's first running tick. The tanks exist and the roster has settled, so this is the first moment a scenario can ask who is playing. |
| `on_tick(tick)` | Every running tick. **Prefer not to declare this.** Timers and the hooks below cover nearly everything, and a handler that runs a hundred times a second is a handler that has to be cheap. |
| `on_end()` | The round has just ended, for any reason. |

### The roster and the lobby

| Hook | Arguments |
|---|---|
| `on_lobby(p, scripted)` | A lobby seat changed. `p` is the seat. |
| `on_player_join(p, scripted)` | |
| `on_player_leave(p, scripted)` | |
| `on_team_changed(p, team, scripted)` | `team` is the seat's new team. |
| `on_chat(p, text, scripted)` | A player said something. |

### What happens in the round

| Hook | Arguments |
|---|---|
| `on_tank_spawned(p, mx, my, respawn, scripted)` | `respawn` is false the first time a seat takes the field. |
| `on_tank_killed(victim, killer, cause, scripted)` | The victim comes first: it is the subject, and the killer is what happened to it. `cause` is `"shell"`, `"mine"`, `"deep_sea"` or `"script"`. |
| `on_lgm_died(p, killer, mx, my, scripted)` | The square is where the man died, captured before the respawn moves him. |
| `on_lgm_landed(p, mx, my, scripted)` | |
| `on_base_captured(n, old, new, scripted)` | |
| `on_base_neutralized(n, old, scripted)` | A base that changed to nobody's. |
| `on_pill_captured(n, old, new, scripted)` | Every change of a pillbox's owner, with `game.NEUTRAL` as `new` where nobody took it. |
| `on_pill_placed(n, p, scripted)` | The pillbox first, then who placed it. |
| `on_pill_picked_up(n, p, scripted)` | The same order. |
| `on_pill_killed(n, by, scripted)` | |
| `on_built(p, action, x, y, scripted)` | `action` is `"trees"`, `"road"`, `"building"`, `"repair"`, `"mine"` or `"boat"`. A build of kind pill on this hook is always a repair — a new pillbox going down is `on_pill_placed`. |
| `on_mine_laid(p, mx, my, scripted)` | |
| `on_mine_explosion(mx, my, layer, scripted)` | |

`scripted` is true when the scenario's own op caused the event, so a handler
that should ignore its own edits opens with `if scripted then return end`.

### Regions

| Hook | Arguments |
|---|---|
| `on_enter_region(p, name)` | |
| `on_leave_region(p, name)` | |

These two take **no** `scripted` flag. Between one sample and the next a tank
can be both moved by the script and driven under its own power, and there is no
honest answer to give a handler that asks which this was.

A tank that stops having a place on the map leaves everything it was in, so
every enter is followed by exactly one leave. That is what makes counting who
is inside a region safe.

### Policies

| Function | Returns |
|---|---|
| `allow_extra_teams()` | Whether a host may put a seat on a team no other seat is on. Absent, or returning `nil`, means yes. |

An op issued from inside a policy is refused with `SCN_OP_IN_POLICY`: a policy
is a question the engine asks mid-operation, and it answers and nothing else.

---

## How the `game` table behaves

One global table, `game`, is installed on every state before your chunk runs.
The same rules apply to every row on it.

**Players are 0-based. Pills, bases and starts are 1-based.** A seat is `p`,
from 0 to `game.max_tanks() - 1`. An entity is `n`, from 1 to
`game.num_pills()` and its siblings — which is also Lua's own numbering, so
`for n = 1, game.num_pills() do` visits every slot.

**Teams are 1 to 15.** 0 means no team.

**Owners are a seat or `game.NEUTRAL`.** Where an owner may be left out
entirely, leaving it out means neutral.

**Reads answer a table, or `nil` for something that is not there.** An index
past the end, an empty seat and a slot no live entity holds all read as `nil`,
and no read raises for it. `game.num_pills()` is the count of slots, not of
live pillboxes, so a loop over it has to skip the `nil`s.

**Writes answer one of three things:**

- `true` when the change applied.
- `true, "queued"` when the change was accepted and lands on a later tick.
  Spawns, removals and large terrain fills answer this way.
- `nil, code, detail` when it was refused. `code` is the refusal's name as a
  string — `"SCN_OP_NO_SUCH_PLAYER"` — and `detail` is one short sentence with
  the number that mattered.

Rows that choose something answer it instead of `true`: `add_pill`, `add_base`
and `add_start` answer the index they took, and `spawn_bot` and `lobby_add_bot`
answer the seat.

```lua
local ok, code, why = game.teleport(p, 300, 4)
if not ok then
  game.log(code .. ": " .. why)     -- SCN_OP_BAD_SQUARE: square (300, 4) is off the map
end
```

**A refusal is an answer; a mistake raises.** A missing argument, an argument
of the wrong type and a word that names nothing are errors in the script and
raise. A value the world will not take is refused and returned. Test the
return; do not wrap calls in `pcall`.

**Text is bytes.** A string past its limit is refused with the limit in the
detail, never cut short.

---

## Reading the world

### Counts and the clock

| Call | Answers |
|---|---|
| `game.tick()` | The tick the round is on. |
| `game.max_tanks()` | How many seats a game has. |
| `game.num_players()` | How many seats are playing the round, bots included. |
| `game.num_humans()` | How many of those are people. |
| `game.team_size(t)` | How many seats sit on team `t`, playing the round or not. |
| `game.game_type()` | The rules the humans play under: `"open"`, `"tournament"` or `"strict"`. |

### The map

| Call | Answers |
|---|---|
| `game.map_name()` | What the map is called. |
| `game.map_tile(x, y)` | The terrain code at a square, or `nil` for a square off the map. |
| `game.is_mine(x, y)` | Whether a square holds a mine. |
| `game.terrain()` | Every square as one 65,536-byte string, the square at `(x, y)` at byte `y * 256 + x + 1`. One call instead of 65,536. |

### Entities

| Call | Answers |
|---|---|
| `game.num_pills()` | How many pill slots the map has, live or not. |
| `game.pill(n)` | `{ x, y, owner, armour, speed, in_tank }`, or `nil` for a slot no live pill holds. |
| `game.num_bases()` | How many base slots the map has. |
| `game.base(n)` | `{ x, y, owner, armour, shells, mines }`, or `nil`. |
| `game.num_starts()` | How many start slots the map has. |
| `game.start(n)` | `{ x, y, dir }`, or `nil`. |

### Seats

| Call | Answers |
|---|---|
| `game.tank(p)` | `{ mx, my, wx, wy, dir, armour, shells, mines, trees, pills, boat, dead, name, bot, kills, deaths, mods }`, or `nil` when the seat is empty or has no tank. `mx, my` are map squares and `wx, wy` world coordinates; `dir` is the full 0-255 facing, which is what `teleport` takes back. `mods` is `{ speed, accel, turn, reload, dealt, taken }`. |
| `game.builder(p)` | `{ state, mx, my, wx, wy, job, trees, mines }`, or `nil` when the seat has none. `state` is `"in_tank"`, `"going"`, `"returning"`, `"parachuting"` or `"dead"`; `job` is an action word, or absent when he is on none. The square is tracked while he is out of the tank — in the tank he is wherever his tank is, which is why the state comes first. |
| `game.lobby_slot(p)` | `{ connected, bot, team, name, ready, fielded, alive }`, or `nil` for an empty seat. **`fielded` is the field that tells a held seat from one on the field.** |

A seat the lobby is holding for a bot reads as connected, a bot, on its team,
and `fielded = false`. That is how Wave Defense finds its horde:

```lua
for p = 0, game.max_tanks() - 1 do
  local slot = game.lobby_slot(p)
  if slot and slot.bot and slot.team == RAIDERS then
    out[#out + 1] = p
  end
end
```

---

## Timers

| Call | Answers |
|---|---|
| `game.timer(seconds, fn)` | Runs `fn` once, on the first tick at or after `seconds` from now. Answers an id. |
| `game.cancel_timer(id)` | Stops a timer that has not run yet. `false` when the id names none, which is what an id that has already run names. |

At most 64 timers wait at a time, and none outlives its round.

A timer that sets another timer moves one call per tick however short the
delay: a run that feeds itself cannot spin inside a single tick.

Ids are never reused, so a stale id is safe to cancel — it matches nothing
rather than matching whatever has since taken its place.

```lua
wave_timer = game.timer(3, wave_over)
...
game.cancel_timer(wave_timer)      -- the wave was cleared early
wave_timer = nil
```

---

## Tags and regions

| Call | Answers |
|---|---|
| `game.tags(kind, n)` | The tags the scenario put on a `"pill"`, `"base"` or `"start"`, as an array of strings. |
| `game.tagged(tag[, kind])` | Everything carrying a tag, as an array of `{ kind, n }`, or of `n` when a kind is named. Pills first, then bases, then starts. |
| `game.region(name)` | A declared rectangle as `{ x, y, w, h }`, or `nil` when nothing is declared by that name. |
| `game.regions()` | The name of every declared region, in name order. |
| `game.in_region(name, mx, my)` | Whether a square is inside a named region. |
| `game.define_region(name, x, y, w, h)` | Names a rectangle for the rest of the round, replacing one of that name. It shares the 64 the `scenario` table's own regions come out of. |

`game.in_region` and the `on_enter_region` hook answer from the same test, so a
script cannot be told it is inside a region the hook disagrees about.

---

## Changing the world

### Tanks

| Call | What it does |
|---|---|
| `game.set_stocks(p, t)` | Sets any of `t.shells`, `t.mines`, `t.armour` and `t.trees`; a stock the table leaves out is left alone. |
| `game.add_stocks(p, t)` | The same four as amounts to add, negative to take away. Each is held at the cap and at zero rather than refused. |
| `game.kill_tank(p[, killer])` | Kills a tank. `killer` is a seat; without one the death is the scenario's own and the cause is `"script"`. |
| `game.teleport(p, x, y[, dir])` | Puts a tank on a square, facing `dir` from 0 to 255. Without `dir` it keeps the way it faces. |
| `game.teleport_to_start(p[, n])` | Puts a tank on start `n`, or on the one the engine would have chosen. |
| `game.set_boat(p, on)` | Puts a tank on a boat or takes it off one. The square under it has to be water. |
| `game.give_pill(p, n)` | Puts a pillbox into a tank, however armoured and whoever held it. |
| `game.drop_pill(p, n[, x, y])` | Puts a carried pillbox back on the map, on a square or under the tank. |
| `game.set_modifiers(p, t)` | Replaces a tank's `speed`, `accel`, `turn`, `reload`, `dealt` and `taken` percentages. A field the table leaves out goes back to the classic tank: the whole set is replaced, not merged. |

### The builder

| Call | What it does |
|---|---|
| `game.builder_order(p, action, x, y)` | Sends a builder out to do one of `"trees"`, `"road"`, `"building"`, `"pill"`, `"mine"` or `"boat"` on a square. The engine repairs rather than builds where the square already holds one. |
| `game.builder_recall(p)` | Calls a builder back to the tank. |
| `game.kill_lgm(p[, killer])` | Kills a builder. `killer` is a seat. |
| `game.builder_parachute(p[, x, y])` | Drops a dead builder back in, on a square or at the tank. |
| `game.set_builder_carried(p[, trees[, mines]])` | What a builder is carrying. A count left out is left alone. |

### Pillboxes and bases

| Call | What it does |
|---|---|
| `game.set_pill_owner(n, p)` | Hands a pillbox to a seat, or to nobody with `game.NEUTRAL`. A pillbox in a tank answers to whoever is carrying it, so this is refused with `SCN_OP_CARRIED` until it is dropped. |
| `game.set_pill_armour(n, a)` | How much a pillbox has left. 0 is a dead pillbox on the ground. |
| `game.set_pill_speed(n, s)` | The ticks between a pillbox's shots. |
| `game.move_pill(n, x, y)` | Puts a pillbox on another square. |
| `game.set_base_owner(n, p[, keep_stock])` | Hands a base to a seat, or to nobody with `game.NEUTRAL`. `keep_stock` leaves what it holds; without it a base changing hands is emptied, as it is in play. |
| `game.set_base_stock(n, armour, shells, mines)` | What a base holds. A stock left out is left alone, and one past the cap is held there. |

A pillbox answers to a **seat**, not to a team. Handing the keep to the
defenders means handing it to one of them:

```lua
for _, n in ipairs(game.tagged("keep", "pill")) do
  game.set_pill_owner(n, defender)
end
```

### Adding and removing entities

| Call | What it does |
|---|---|
| `game.add_pill(x, y[, owner[, armour[, speed]]])` | Puts a new pillbox on the map and answers which one it is. Nobody's, dead, and firing at the round's own rate unless told otherwise. |
| `game.remove_pill(n)` | Takes a pillbox off the map. The slot stays, so the pillboxes above it keep their numbers. |
| `game.add_base(x, y[, owner[, armour, shells, mines]])` | Puts a new base on the map and answers which one it is. Nobody's and empty unless told otherwise. |
| `game.remove_base(n)` | Takes a base off the map. The slot stays. |
| `game.add_start(x, y, dir)` | Puts a new start on a deep-sea square, facing `dir` from 0 to 15. Answers which one it is. |
| `game.remove_start(n)` | Takes a start off the map. The last one is refused — a map with no start has nowhere to put anybody. |

A map holds 16 of each at once; the 17th is refused with `SCN_OP_FULL`.

### Terrain and mines

| Call | What it does |
|---|---|
| `game.set_tile(x, y, t)` | Writes one square's terrain, by a `game.TERRAIN` code. |
| `game.fill_rect(x0, y0, x1, y1, t)` | Writes a rectangle of terrain. One too big for a tick's budget answers `true, "queued"` and finishes over the ticks after it. |
| `game.place_mine(x, y[, owner[, visible]])` | Lays a mine on a square. `visible` shows it to everyone rather than to its owner's side. |
| `game.remove_mine(x, y)` | Takes a mine off a square without setting it off. |

---

## Bots and seats

| Call | What it does |
|---|---|
| `game.spawn_bot(t)` | Puts a bot into the running round. Answers the seat and `"queued"`. |
| `game.remove_bot(p)` | Takes a bot out of the running round. A human seat is refused with `SCN_OP_IS_HUMAN`. |
| `game.set_team(p, t)` | Moves a seat to another team mid-round. |
| `game.lobby_add_bot(t)` | Seats a bot in the lobby and answers which seat it took. |
| `game.lobby_remove_bot(p)` | Takes a bot out of the lobby. A human seat is refused. |
| `game.lobby_set_team(p, t)` | Moves a lobby seat to another team. |

`spawn_bot` takes one table, all of whose fields are optional:

| Field | Meaning |
|---|---|
| `slot` | The seat to take. Left out, the first free seat is taken, and which one that is is decided as the spawn lands rather than as it is queued. A seat held for a bot that is not on the field is the one occupied seat a spawn may name — fielding it is what the seat is for. A seat that already has somebody on the field is refused with `SCN_OP_ALREADY`. |
| `name` | The bot's name. A seat that is already held keeps the name it was seated with, whatever this says. |
| `brain` | The brain to run. Left out, the seat's own brain is used — the one its team was written with — and failing that the server's. |
| `team` | The team to join. A held seat keeps the team it was seated with. |
| `start` | The start to come in on, 1-based. Left out, the engine chooses. |
| `init` | A flat table of names to strings or numbers, handed to the brain at its first breath. |

`lobby_add_bot` takes `name`, `brain`, `team`, `slot` and `fielded`, where
`fielded = false` asks for the seat without the bot.

**The seat cycle.** A seat the `scenario` table seated and a wave fielded goes
back to being held when `remove_bot` names it, rather than being emptied — so
the next wave has it again. A seat that was not the template's is emptied, as a
removal has always done.

The roster ops are the six above. All of them are refused inside `on_setup`:
the round is still being built there, and a roster edit would re-enter the
machinery that is building it. Field your first wave from `on_start`.

---

## Talking to players, and ending the round

| Call | What it does |
|---|---|
| `game.message(text[, target])` | A line to everyone, to one seat with a number, or to a team with `{ team = t }`. `nil` and `"all"` both mean everyone. |
| `game.sound(name[, x, y])` | Plays one of the server's sounds, at a square or everywhere. |
| `game.log(text)` | Writes a line to the server's console. No player sees it. |
| `game.end_round([text[, winner_team]])` | Ends the round now, with the line the lobby shows and the team that won it. |
| `game.set_game_time(ticks)` | How long the round has left. |
| `game.add_game_time(ticks)` | Adds to what the round has left, or takes away with a negative. A round with no time limit has nothing to add to, so give it a length first. |

`end_round` is how a scenario wins or loses a round. It stops play there and
then, and the line it carries is shown in the lobby exactly as written — the
server adds no verdict of its own.

---

## Rules

Every gameplay number the simulation runs on is a named rule. `game.rule(name)`
reads one and `game.set_rule(name, value)` writes one; the `scenario` table's
`rules` block writes a set of them before the round starts.

| Call | What it does |
|---|---|
| `game.rule(name)` | What a rule is set to. A name that spells no rule **raises** — every other `nil` on this surface is an entity that is not there, and a rule that is not in the table is a misspelling in the script. |
| `game.set_rule(name, value)` | Writes one rule. A name that spells no rule raises; a value the table will not take is refused. |

Two things a rule value can be refused for:

- `SCN_OP_RANGE` — the value is outside the rule's own bounds. The detail names
  the rule and both bounds: `tank_reload_ticks is 999, outside 0..255`.
- `SCN_OP_PAIR` — the value is inside its own bounds but breaks an invariant it
  shares with another rule. The detail names both:
  `base_min_shells is 20, above base_full_shells 10`.

A pair can be broken across two rules that are each fine alone, so a `rules`
block is checked as a whole and not one line at a time.

---

## Constants

| Name | Value | Meaning |
|---|---|---|
| `game.api_version` | 1 | The surface this server implements. A script states the one it was written against as `scenario.api`. |
| `game.NEUTRAL` | 255 | The owner a pillbox or base carries when no player holds it. |

---

## Terrain codes

`game.TERRAIN` names the codes `game.map_tile` answers and `game.set_tile` and
`game.fill_rect` take, so a script compares against a word rather than against
a number of its own.

`deep_sea`, `building`, `river`, `swamp`, `crater`, `road`, `forest`, `rubble`,
`grass`, `half_building`, `boat`, `mine_swamp`, `mine_crater`, `mine_road`,
`mine_forest`, `mine_rubble`, `mine_grass`.

```lua
if game.map_tile(x, y) == game.TERRAIN.deep_sea then ... end
```

---

## Sounds

The names `game.sound` takes. The list stops where the server stops taking one
from a scenario: the lobby's own sounds are the last it carries, and anything
past them belongs to the client.

`shoot_self`, `shoot_near`, `shoot_far`, `shot_tree_near`, `shot_tree_far`,
`shot_building_near`, `shot_building_far`, `hit_tank_near`, `hit_tank_far`,
`hit_tank_self`, `bubbles`, `tank_sink_near`, `tank_sink_far`,
`big_explosion_near`, `big_explosion_far`, `farming_tree_near`,
`farming_tree_far`, `man_building_near`, `man_building_far`, `man_dying_near`,
`man_dying_far`, `man_laying_mine_near`, `mine_explosion_near`,
`mine_explosion_far`, `lobby_chat_received`, `lobby_ready`, `lobby_unready`,
`lobby_countdown`, `lobby_game_start`, `lobby_player_join`,
`lobby_player_leave`.

---

## Refusal codes

The `code` a refused write answers, as a string.

| Code | Means |
|---|---|
| `SCN_OP_QUEUED` | Accepted; it applies on a later tick. Answered beside `true`, not as a refusal. |
| `SCN_OP_UNSUPPORTED` | The server has no arm for this yet. |
| `SCN_OP_IN_POLICY` | Issued from inside a policy, which may only answer. |
| `SCN_OP_WRONG_STATE` | A lobby write while the round runs, a round write while in the lobby, or a roster write during the setup that opens a round. |
| `SCN_OP_NO_SUCH_PLAYER` | The seat is empty, or has no tank when the write needs one. |
| `SCN_OP_TANK_DEAD` | The write needs a live tank. |
| `SCN_OP_IS_HUMAN` | A removal aimed at a person. |
| `SCN_OP_NO_SUCH_ITEM` | A pill, base, start or region index that is out of range or holds nothing live. |
| `SCN_OP_BAD_SQUARE` | `x` or `y` off the map. |
| `SCN_OP_BAD_TERRAIN` | The square cannot hold this: a base on deep sea, a mine on a building. |
| `SCN_OP_RANGE` | A number outside what the field or the rule allows. |
| `SCN_OP_PAIR` | A rule value that breaks an invariant it shares with another rule. |
| `SCN_OP_CARRIED` | The pillbox is in a tank; drop it first. |
| `SCN_OP_FULL` | 16 live entities, 16 seats, or a queue with no room. |
| `SCN_OP_ALREADY` | An add of something already there, a give of a carried pillbox, a spawn into a seat already on the field. |
| `SCN_OP_TOO_BIG` | A list or a line past its buffer. |
| `SCN_OP_RATE` | A budget for the tick is spent. |
| `SCN_OP_NOT_FOUND` | A brain path or a package name that does not resolve. |
| `SCN_OP_NO_STOCK` | A builder order the tank cannot pay for. |
| `SCN_OP_BAD_CALL` | The call itself is malformed. |

---

## Limits

| | |
|---|---|
| Sidecar file | 1 MiB |
| Rules in the `scenario` table | 128 |
| Tags per entity | 4, each 31 bytes |
| Regions | 64, declared and defined together; names 31 bytes |
| Timers waiting at once | 64 |
| A line of text | 128 bytes |
| A bot's `init` table | 16 pairs |
| Events queued for one tick | 256 |

Going past one of these is reported and refused, never silently cut.

---

## Checking a scenario before you run it

```
WinBoloDS -validate "maps/Wave Defense.map"
```

reads the sidecar beside the map in a Lua state with a `game` table that
answers nothing, runs the chunk's top level, and checks what the `scenario`
table says against the map, the lobby and the rule catalogue. Each problem is
printed as

```
maps/Wave Defense.scenario.lua:14: rules.tank_reload_ticks: tank_reload_ticks is 999, outside 0..255
```

and the command exits 0 for a map that is playable and 1 for one that is not.
No round is run and no bot loads, so this is safe to run on anything.

The line number is found by looking for the key's own name in the source, so
two things can lead it astray: a name written earlier in a comment or a string
takes the line, and a key two tables both carry — a second team's `bots` —
takes the first one's line.

---

## What is not here yet

Named so you do not spend an afternoon looking for them:

- **Policies beyond `allow_extra_teams`.** Choosing a start, deciding a
  loadout, scaling damage, allowing a capture, vetoing a death: none of these
  are asked of a scenario today.
- **`spawn_bot`'s `loadout`.** The field is read and any value for it is
  refused; a spawning bot is handed what the round's own policy hands it.
- **Presentation.** A panel, a score line, a newswire line and a map marker
  have no calls yet.
- **Triggers.** A `scenario.triggers` table is not read, and a sidecar that
  carries one is neither parsed nor refused for it.
