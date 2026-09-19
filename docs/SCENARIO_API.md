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
- [What a scripted lobby looks like](#what-a-scripted-lobby-looks-like)
- [A worked example: Wave Defense](#a-worked-example-wave-defense)
- [The `scenario` table](#the-scenario-table)
- [Hooks](#hooks)
- [How the `game` table behaves](#how-the-game-table-behaves)
- [Reading the world](#reading-the-world)
  - [Three clocks](#three-clocks)
- [Timers](#timers)
- [Tags and regions](#tags-and-regions)
- [Changing the world](#changing-the-world)
- [Bots and seats](#bots-and-seats)
  - [Hints: telling one bot what to do](#hints-telling-one-bot-what-to-do)
- [Talking to players, and ending the round](#talking-to-players-and-ending-the-round)
- [Showing things on a client](#showing-things-on-a-client)
  - [The panel](#the-panel)
- [Rules](#rules)
- [Triggers](#triggers)
  - [The fields a hook offers](#the-fields-a-hook-offers)
  - [Writing triggers in the map editor](#writing-triggers-in-the-map-editor)
- [Test hooks](#test-hooks)
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
to a new name and writing a script for the copy is how you attach a scenario
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
- **A script that will not load is skipped.** The round is played as a plain
  map and the operator is told why.

The file may be up to 1 MiB.

**What a script is given.** The state a scenario runs in opens `string`,
`table`, `math`, `os`, `coroutine` and a named list of base functions, and
nothing else. The base list is `assert`, `collectgarbage`, `error`,
`getmetatable`, `ipairs`, `next`, `pairs`, `pcall`, `print`, `rawequal`,
`rawget`, `rawlen`, `rawset`, `select`, `setmetatable`, `tonumber`,
`tostring`, `type`, `unpack` and `xpcall`, plus `_G` and `_VERSION` — a base
function this list does not name is not there, whatever your host's Lua
carries. `unpack` is 5.1's and `rawlen` is 5.2's, so a host has the one its
own Lua has. `getfenv` and `setfenv` are not on the list, so do not go looking
for them on a LuaJIT host.
There is no `io`, so a script cannot read or write a file. There is no
`package` and no `require`, so it cannot load another module. There is no
`debug`, and on a LuaJIT host no `ffi`, no `jit` and no `bit`. `load`,
`loadstring`, `dofile`, `loadfile` and `string.dump` are gone with them, and
`os` keeps only `time`, `date`, `clock` and `difftime`. `os.date` takes the
portable conversion characters — `%a %A %b %B %c %C %d %D %e %F %g %G %h %H
%I %j %m %M %n %p %r %R %S %t %T %u %U %V %w %W %x %X %y %Y %z %Z %%`, with
the leading `!` for UTC and `*t` for a table — and raises on anything else,
naming the specifier it would not take: hosts do not agree on what their C
library accepts, and one of them ends the process over a format it does not
like rather than complaining about it. The `E` and `O` modifiers are among
what it will not take, and a format may be 256 bytes at most. `math.random`
works and is seeded for you; `math.randomseed` is not there to reseed it.
`collectgarbage` answers every option but `"stop"`, which raises. `utf8` is
present only on hosts built against PUC-Lua — a LuaJIT host has none, so a
script that wants it must ask. Your file is loaded as text: a precompiled
chunk is refused at the door, so ship the source. `print` goes to the server
console rather than to the host's stdout, which is how a script says
something to the operator. It joins its arguments with tabs as stock `print`
does, and cuts the finished line at 1024 bytes — a line longer than that is
shortened rather than dropped, so an operator still sees what it was about.

How many lines it will take is bounded twice: 64 from any one call — a hook,
a timer or a policy answer — and 64 across everything a single tick's calls
print between them. Past either, lines are dropped, and the first one dropped
says so, once, so an operator is not left wondering where the output went.
Both allowances come back: the per-call one at the next call and the per-tick
one at the next tick, so a script that says a line or two as things happen
never meets them. What meets them is a script tracing every tick, and `print`
is not for that — it is for telling an operator something. If you want to
watch your script work, print at the moment that interests you rather than on
the way past.

**Your state holds at most 32 MB.** Every allocation it makes is counted, and
the one that would take it past the cap is refused: Lua raises an
out-of-memory error at the point that asked for it, exactly as it would raise
any other. The hook or policy call it happened in fails, the server counts one
error against the script, and the round carries on — a script that keeps
failing is switched off for the rest of the round, as it is for any other
repeated error. The state itself survives, so whatever the failed call had
abandoned is collected and the next call starts with room again. 32 MB is far
more than a scenario needs; if you are near it you are keeping something you
meant to let go of.

**One call may run for a million instructions.** Every hook, timer and policy
answer is counted as it runs, and a call that passes the budget is stopped
where it stands: Lua raises an error at that line, the call fails, the server
counts one error against the script, and the round carries on — again, a
script that keeps failing is switched off for the rest of the round.

You cannot catch that error and carry on. `pcall`, `xpcall` and
`coroutine.resume` raise it again rather than answering with it, so a `pcall`
around slow work does not buy the work more instructions, and a loop that
catches and tries again is stopped on the next turn rather than running on —
the instructions are spent whether or not something caught the error. A
coroutine is no way around it either: stopping what a `coroutine.resume` was
running stops the call that resumed it. Errors of your own are unaffected; a
catcher hands those back exactly as it always did.

The budget is per call, not per round and not per tick. Each call starts again
at zero, so nine hundred thousand instructions in this `on_tick` leaves the
next one its own full million. What it rules out is looping inside a single
call: `while true do` in a hook takes your scenario off the round and nothing
else, but it does take it off. Work that cannot finish in one call belongs
spread across `on_tick` calls, keeping its place in a local between them.

A million is a great deal — an `on_tick` that reads a few dozen tanks and
decides something spends a few thousand. If you are near it, you are looping
over the map rather than over what changed.

The count is taken by a Lua debug hook, and a state that carries one does not
use LuaJIT's compiler, so **your script runs interpreted** on a LuaJIT host.
That is worth knowing before you time anything: what you measure here is not
what the same code would do in a brain.

**A scenario is still trusted the way a brain is.** A file beside a map is run
by whoever hosts that map, at the server's own privilege, so run only scripts
you would run as a program — the library, the memory cap and the budget above
narrow what a script can reach and how long it can hold the tick, not who is
answerable for it. Every host that opens a map from a file looks beside it:
the dedicated server, the desktop client hosting a single-player or LAN game,
and the headless runner.

**Switching scripts off.** Each of the three hosts can be told not to load
one, and each spells it its own way:

- `-noscenarios` on the dedicated server. One dash.
- `--noscenarios` on the headless runner. Two.
- **Run map scripts when hosting**, the desktop client's hosting preference.
  On by default, and it takes effect the moment it changes rather than from
  the next hosted game.

With any of them off a map that has a script beside it plays plainly, the
operator is told which script was skipped, and the map chooser does not tag
the map as scripted.

**Switching off only the scripts inside uploaded maps.** A `.map` a player
uploads to a server can have a scenario packed into the file, and the server
keeps the bytes it was sent whole, so committing that map later runs the
script that came with it. An uploaded map's script runs under exactly the
same sandbox as one on the operator's own disk — the same library, the same
memory cap, the same instruction budget — and, as above, at the server's own
privilege. A server that would rather not take a script from a player has a
narrower switch than turning scripts off altogether:

- `-nouploadscripts` on the dedicated server. One dash.
- `--nouploadscripts` on the headless runner. Two.
- **Run scripts in uploaded maps**, the desktop client's hosting preference,
  under the one above. On by default, and it takes effect the moment it
  changes.

With it off, a map whose file sits in the server's uploads directory attaches
neither a packed scenario nor a loose script beside it, the console names the
upload that was turned down, and the map chooser does not tag it as scripted.
Every other map is unaffected. With it on, one console line names each
uploaded map whose packed scenario is what runs, so the operator can see when
a round is being played by a script a player sent.

**Reloading after an edit.** Two ways in. On a dedicated server the console
command `reload` reads the file again; in a lobby, the host has a **Reload
script** button beside the scenario's name. The button is the host's alone
and only while the lobby is up, and at most one reload a second is accepted —
reading and checking the file is real work on the thread the command arrives
on.

Either way the file is read and checked before anything is swapped, so one
that will not load leaves the old one in place and says why, naming the file
and the line. A reload swaps the script's bytes and nothing else: the next
round to start boots the new rules and hooks, while the lobby seats the
template asks for, the scenario's name and description, and the game it
declares are the map commit's and change only when the map is committed
again. The round in progress keeps what it started with.

---

## What a scripted lobby looks like

Worth knowing before you write one, because some of it is not yours to
change from the script.

**The game type reads Scripted.** In the lobby and in both game finders, a
round with a scenario attached shows its type as Scripted rather than as the
game the scenario declared. `scenario.game` is what the round is actually
played by; Scripted is what the round is called. A scenario that declares no
`game`, or one the server has no behaviour for, is played as strict
tournament — the lobby does not keep the type it was on, so a mod that came
to change one rule and named no game still moves the round to strict.

**The scenario names itself under the map.** The lobby draws the scenario's
name below the map lines, and the description under that in a dimmer shade.
A scenario whose table left `name` empty is shown by its script's file name
instead, because a scenario that named itself nothing still came from a file.
Any control character in the three strings arrives as a space, so a
description with a newline in it does not take the lobby apart.

**The map chooser tags a scripted map.** Every map the lister finds is asked
whether a script sits beside it, and the ones that do carry a Scripted tag.
With scripts switched off nothing is tagged.

**Three settings are refused while a scenario is attached.** Any game-type
change, ranked on, and the no-bots AI policy. All three are refused the same
way and the host is shown the same line — *That setting is fixed by the map's
scenario* — rather than the request failing silently.

**Committing a scripted map moves two settings out of the way**, because the
three above cannot stand beside a scenario: ranked goes off, and an AI policy
set to no bots moves up to one that runs them. Committing a plain map
afterwards gives all three back — the game type, the ranked flag and the AI
policy the lobby was on before the scenario arrived.

**A lobby everyone leaves keeps the scenario's settings.** The reset that
empties a lobby puts the operator's own game type, ranked flag and AI policy
back and then applies the scenario's over them again, so the lobby the next
player walks into is the one the scenario asked for.

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
anyone joins, where a host can see and trim them, and puts no bot on the
field in any of them until something fields one:

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
  seats = held_seats()
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
is optional. Leaving out `name` costs less than it looks: the lobby falls back
to the script's own file name, so the scenario is still identified — but by
the name you gave the file rather than by the one you would have chosen.

```lua
scenario = {
  name         = "Wave Defense",
  description  = "Hold the four pillboxes at the centre of the map.",
  api          = 1,
  game         = "open",
  bound        = true,
  fill_to_caps = false,
  lobby        = { ... },
  rules        = { ... },
  tags         = { ... },
  regions      = { ... },
  triggers     = { ... },
}
```

| Field | Type | Meaning |
|---|---|---|
| `name` | string | What the scenario is called. |
| `description` | string | One or two sentences for a host reading a list. |
| `api` | number | The API version you wrote against. Defaults to 1. A server older than the version you name refuses the scenario rather than running it half-understood. |
| `game` | string | The game type the scenario asks for: `"open"`, `"tournament"` or `"strict"`. The round plays under it — the lobby and both game finders read the type as Scripted, and every part of the engine that picks behaviour from the game type resolves that to the word named here. Left out, the round plays strict tournament. A word that is none of the three also plays strict tournament, and `-validate` reports it by name. Write `game = "open"` for an open round: a scenario that says nothing is not read as asking for one. |
| `bound` | boolean | True (the default) when the scenario is tied to its map. A scenario that names tags or regions is tied to its map by definition, because tags and regions are the map's own squares and entities. A server can also offer scenarios of its own, which play over whichever map a host has committed; a scenario with `bound` true is not one of those and a host picking it is refused, because over another map its tags, its regions and its entity indices name items that are not there. |
| `triggers` | array | What the scenario does without a line of Lua: hooks to listen on, tests against what each hook is handed, and calls to make when every test holds. A scenario may carry triggers, a script, or both. See [Triggers](#triggers). |
| `fill_to_caps` | boolean | False by default. True starts every pillbox and base on the map at the caps your `rules` table leaves in force rather than at the numbers the map file holds. A map file states a number for each pill's armour and each base's stocks and has no way of stating "full", so a scenario that raises `base_full_armour` or `pill_max_armour` would otherwise open with the map's own smaller numbers and climb to the new ones over the round. Raising only: anything already at or above a cap is left where it is, and anything above one is brought down by the rules themselves. A pill's firing rate is not touched. |

### `scenario.lobby`

The lobby the map opens with.

| Field | Type | Meaning |
|---|---|---|
| `max_players` | number | The cap on human players, 1 to 16. 0 (the default) leaves the server's own cap. |
| `extra_teams` | boolean | Read, and sent to every client on the lobby settings event, but nothing acts on it yet. Whether a host may put a seat on a team nobody is on is decided by the `allow_extra_teams()` policy below, and a scenario that declares neither allows it. |
| `teams` | array | One entry per team, in order. |

Each team:

| Field | Type | Meaning |
|---|---|---|
| `id` | number | The team number, 1 to 15. Two teams may not share one. |
| `bots` | number | How many seats to seat for this team when the lobby is built. A host who trims them gets the trimmed number back next round: the point of seating them where a host can see them is that the host may change them. |
| `max_bots` | number | The ceiling a host may raise `bots` to. 0 means no ceiling stated, which is not the same as no bots allowed. |
| `fielded` | boolean | True (the default) puts a bot in the seat at the start of the round. False holds the seat without one: it is in the roster, it takes no tank, and no bot plays in it until a `spawn_bot` names it. Its runner is built ahead of the round, as the two paragraphs below this table describe. |
| `brain` | string | The brain this team's bots run, **named**: the directory under the server's `brains/`, such as `GoalHunter_1.7`. Empty means the server's own. A name this server does not have leaves the team's seats on the server's own brain, says one line on the console, and is reported by `-validate` before a round is ever started. A value with `/` or `\` in it is a path, not a name, and is refused as such — a scenario shared with a server knows nothing of that server's layout, which is why it names the brain and lets the server find it. `package:NAME`, a brain carried inside the scenario, is still refused with `SCN_OP_NOT_FOUND`. |
| `init` | table | A flat table of names to strings or numbers, handed to this team's bots when their VM is built. A `spawn_bot` that names one of these seats and carries no `init` of its own gets this one. |

The server's own `-brain` switch is still a path, and deliberately: that is an
operator naming a file on their own machine, where the layout is theirs to know.
A `brain` in a scenario is content that travels with the map to servers that
have never seen it, so it names what it wants and the server resolves the name
against its own `brains/` — the same directories the lobby's bot list is built
from. Ship a scenario that needs `GoalHunter_1.7` and every server that has
`GoalHunter_1.7` runs it; one that does not gets a reported problem and a
playable round.

`max_bots` is a memory ceiling as well as a seating one. A seat keeps the runner
behind it — one ClientSim and one brain VM — across the unfielding that takes
its bot off the field, so the next wave is handed that runner rather than
building another.

Those runners are built before the round is played: one a frame across the
countdown, for every seat the template is holding. So a round holds one runner
per held seat from the moment it begins, rather than from the first wave that
needs one, and `max_bots` bounds that. They are built ahead only for a round
that starts from the lobby's countdown: a first single-player round, a server
started without a lobby, and a map rotation each build a seat's runner at the
first wave that fields it instead. A seat whose team gives it an `init` here is
warmed with that table, so a spawn naming the seat with no table of its own, or
with the same one, is a resume from the first wave on. A spawn carrying a
different table builds its runner then, because the table is read when the VM is
built, and the server log says the two differed. A seat whose brain will not
load is skipped, with a line in the server log, and the wave that fields it
builds its runner then as it always did. A countdown abandoned before the round
starts releases the runners it had built, and every runner is released when the
round ends.

### `scenario.rules`

A table of rule name to value. Names are the ones in [Rules](#rules) —
the same ones `game.rule` and `game.set_rule` take. A name that spells no rule,
or a value outside a rule's range, is reported and the rest of the table still
applies. A value that is not a number is dropped without a report, so `"40"`
in quotes sets nothing.

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
past what the map holds is reported. A tag is at most 31 bytes, and one longer
than that is cut to 31 without a report, so `game.tagged` with the full name
would then find nothing.

**The map editor counts these from 0 and this table counts from 1.** The
editor's pill, base and start lists are its own, numbered the way the rest of
the editor numbers them, so the pillbox it shows as **pill 0** is `[1]` here.
Tagging through the editor's own tags view is safe — it converts, and the
number you see is the number you meant. Writing the table by hand from a
number read off the editor is where this bites, and it bites quietly: a tag on
the wrong entity is still a valid table, so nothing is reported and the
trigger or the `game.tagged` call that wanted it simply never finds it. Count
from 1 here, and from 1 in every `game.*` call that takes an `n`.

### `scenario.regions`

Named rectangles of map squares, by name, with an inclusive top-left corner
and a size.

```lua
regions = {
  keep = { x = 124, y = 124, w = 10, h = 10 },
},
```

A region covers `x` to `x + w - 1` and `y` to `y + h - 1`, both ends
included, so a rectangle with no width or no height holds nothing. At most 64
regions exist at once, counting both these and any `define_region` makes
during the round. A name is at most 31 bytes; a longer one in this table is
cut to 31 without a report, where `define_region` refuses it. A coordinate or
size is a byte: a value past 255 is not reported here and lands wrapped, so
keep to the map.

### `scenario.triggers`

What the scenario does, stated as data rather than written as Lua: a list of
hooks to listen on, tests against what each hook is handed, and calls to make
when every test holds. A scenario can carry triggers, a script, or both.

```lua
triggers = {
  { when    = "on_pill_captured",
    where   = { { "tag", "in", "keep" } },
    actions = { { "announce", "The keep has fallen", 5 } } },
},
```

Set out in full under [Triggers](#triggers).

---

## Hooks

A hook is a global function the server calls, or a function of the same name
kept as a field of the `scenario` table; where both exist the global wins.
Declare the ones you want and leave out the rest; a hook you do not declare
costs nothing.

**A hook's return value means one thing, and only where the scenario declares
triggers.** The engine ignores what a hook returns. But a scenario with a
`triggers` table has a router installed over each hook it listens on, and that
router calls your own handler first: return **`false`** from it and the
triggers on that hook do not run for that event. Anything else — a number, a
string, `nil`, or falling off the end of the function, which is what most
handlers do — lets them run. A scenario with no triggers has no router, so
there is nothing for a return to reach. See [Triggers](#triggers).

### The round's own moments

| Hook | When |
|---|---|
| `on_setup()` | Once, after the scenario's rules are applied and before the round's first tick. Every write is available except the six roster ops — `spawn_bot`, `remove_bot`, `set_team` and the three `lobby_*` calls — which answer `SCN_OP_WRONG_STATE` here. This is where the map gets ready. |
| `on_start()` | The round's first running tick. The tanks exist and the roster has settled, so this is the first moment a scenario can ask who is playing. |
| `on_tick(tick)` | Once per frame, fifty times a second. The `tick` it is handed goes up by **2** each time, not by 1 — see [Three clocks](#three-clocks). **Prefer not to declare this.** Timers and the hooks below cover nearly everything, and a handler that runs fifty times a second is a handler that has to be cheap. |
| `on_end()` | The round has just ended, for any reason. |

**What a setup arranges, and what it does not.** The window that lets writes
through is open across two calls, not one: the chunk's own top level at the
round's boot and `on_setup` after the tanks are built. Both are inside a
round start, and three things follow from that.

What a setup arranges is the world the round begins in, not something that
happened in it. It rides the opening snapshot's own tank, base and pill lists,
and writes no newswire line: a setup that deals every base on the map hands
each client sixteen owners rather than sixteen captures. For the same reason
none of those captures is credited to anyone — the round's scoreboard and its
timeline both start empty, whatever a setup arranged.

Terrain a setup edits does reach every client. A `set_tile` or a `fill_rect`
at setup is sent the same way a mid-round one is, so the round opens on the
map the script wrote rather than on the one the file held.

**The rules table, round by round.** Every round opens on the classic table,
whatever the round before it played by. A scenario writes its own over that at
the boot, before a start is picked or a tank is built, so the opening tanks are
built under the numbers the file asked for. Two rounds therefore play classic
without asking for it: the round after a scenario is detached, and a round
whose script failed to boot — neither has a table of its own to write, and
neither inherits the last script's.

The table is applied a rule at a time. A rule the file names that the sim
refuses is reported by name, with the reason, and the rest of the table is
applied around it; one bad row does not cost a scenario its other rules.

### The roster and the lobby

| Hook | Arguments |
|---|---|
| `on_lobby(p, scripted)` | A lobby seat changed. `p` is the seat. |
| `on_player_join(p, scripted)` | Fires for a bot seat as it does for a person, and a bot a script seats reaches it with `scripted` true. A seat on the roster is not yet a tank on the field: that is `on_tank_spawned`. |
| `on_player_leave(p, scripted)` | By the time the handler runs the seat is empty — `game.tank(p)`, `game.builder(p)` and `game.lobby_slot(p)` all answer `nil`. Whatever a handler needs to know about the player has to have been kept from an earlier hook. |
| `on_team_changed(p, team, scripted)` | `team` is the seat's new team. |
| `on_chat(p, text, scripted)` | A player said something. |
| `on_ping(p, kind, mx, my, scripted)` | Seat `p` put a smart ping on the map. `kind` is 0 standard, 1 caution, 2 assist, 3 attack, 4 on my way, 5 bot command; `mx`, `my` are the map square it landed on. A seated bot brain places its own pings through the same path, so this fires for a bot as it does for a person. The hook only watches: nothing it returns changes the marker, and there is no call that places one. |

### What happens in the round

| Hook | Arguments |
|---|---|
| `on_tank_spawned(p, mx, my, respawn, scripted)` | `respawn` is false the first time a seat takes the field. |
| `on_tank_killed(victim, killer, cause, scripted)` | The victim comes first: it is the subject, and the killer is what happened to it. `cause` is `"shell"`, `"mine"`, `"deep_sea"` or `"script"`. |
| `on_lgm_died(p, killer, mx, my, scripted)` | The square is where the man died, captured before the respawn moves him. |
| `on_lgm_landed(p, mx, my, scripted)` | The builder `on_lgm_died` reported has finished his flight back and touched down. `mx`, `my` are the square he reached — where his tank stood when he died, unless `builder_parachute` aimed him somewhere else — and he walks to the tank from there rather than arriving in it. |
| `on_base_captured(n, old, new, scripted)` | `old` and `new` are owners: `old` is `game.NEUTRAL` for a base nobody held, and `new` is always a seat, because a base changing to nobody's raises `on_base_neutralized` below instead. Neither hook fires when a base passes to an ally or falls to nobody because its owner left — that hand-over raises no event at all. |
| `on_base_neutralized(n, old, scripted)` | A base that changed to nobody's. No play reaches it: the engine's own path for a base going neutral raises no event, the same silence `on_base_captured` describes above, so the only thing that fires this hook is `game.set_base_owner` clearing one — which makes `scripted` always true. |
| `on_pill_captured(n, old, new, scripted)` | Every change of a pillbox's owner, with `game.NEUTRAL` as `new` where nobody took it. |
| `on_pill_placed(n, p, armour, scripted)` | The pillbox first, then who placed it. Fires for every way a carried pillbox reaches the map, not only a builder finishing the job: a tank sinking or being destroyed puts its cargo down, a builder dying puts the one in his hands down, and a player leaving does both. `armour` is what tells them apart — a built pillbox arrives at the sim's cap, every other route arrives dead at `0` — so test it, not `p`, before treating one as a live gun. `p` is whoever was carrying it, which on a leave is a slot on its way out. |
| `on_pill_picked_up(n, p, scripted)` | The same order. |
| `on_pill_killed(n, by, scripted)` | `by` is the seat credited with the blow, or `game.NEUTRAL` where none can be: a blast that caught the pillbox, and a shell another pillbox fired, both name nobody. |
| `on_built(p, action, x, y, scripted)` | `action` is `"trees"`, `"road"`, `"building"`, `"repair"` or `"boat"`. A build of kind pill on this hook is always a repair — a new pillbox going down is `on_pill_placed`. `"mine"` is a word the surface spells and this hook never sends: laying one raises `on_mine_laid` instead, so a test for it here never holds. |
| `on_mine_laid(p, mx, my, scripted)` | `p` is the seat whose tank dropped the mine or whose builder laid it. A scenario's own `place_mine` does not reach this hook, so nothing here is `scripted`. |
| `on_mine_explosion(mx, my, layer, scripted)` | `layer` is who laid the mine, not a layer of the map: the seat that put it down, or `game.NEUTRAL` for a mine the map came with or one whose owner has since left. It is read before the mine leaves the square, and it is kept off the wire — no client is told whose minefield it drove into. |

`scripted` is true when the scenario's own op caused the event, so a handler
that should ignore its own edits opens with `if scripted then return end`.

> **`on_pill_placed` changed.** It used to be `on_pill_placed(n, p, scripted)`
> and to fire only when a builder finished the job. A handler written against
> that form needs two things. Take the new argument, or the third one it reads
> as `scripted` is now a number:
>
> ```lua
> function on_pill_placed(n, p, armour, scripted)
> ```
>
> And decide what it means by the armour, because the hook now also fires for
> every pillbox a corpse drops — a tank sunk or destroyed, a builder killed
> holding one, a player quitting doing both, and a scenario's own `drop_pill`.
> A handler that meant "somebody put a gun up" keeps that meaning with a guard:
>
> ```lua
> function on_pill_placed(n, p, armour, scripted)
>   if armour == 0 then return end   -- dropped, not built: it is dead
>   ...
> end
> ```
>
> Without the guard it will fire on drops it never used to see. `p` is whoever
> was carrying the pillbox, which on a quit is a slot on its way out of the
> game, so it is not a safe stand-in for "the player who built this".

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

A policy is a question the engine asks in the middle of doing something, and
the answer decides what it does. Unlike a hook, which is told about a fact
after it has happened, a policy is asked before, and it answers and nothing
else: an op issued from inside one is refused with `SCN_OP_IN_POLICY`.

Every policy is a plain global function, looked up by name at each call.
Leave one out and the game plays by its ordinary rule. Four things all mean
"no opinion" and give that same rule: no function of the name, a scenario
switched off for the round, a `nil` return, and a call that raises. Only the
raise counts toward the error limit. An answer the engine cannot use — a
start that is not on the map, a loadout table missing an amount, a percent
out of range — also gives the ordinary rule and also counts, and is named on
the console, so a script answering unusably every time is switched off
rather than left to misbehave quietly.

| Function | Asked | Answer |
|---|---|---|
| `allow_extra_teams()` | When a host would put a seat on a team no other seat is on. | `true` to allow. Ordinary rule: yes. |
| `allow_base_win()` | Before the round checks whether one side owns every base. | `false` takes that ending out of the round and leaves every other ending alone. Ordinary rule: yes. |
| `can_respawn(p)` | On the last tick of a dead tank's wait. | `false` holds the tank where it is, and the question is asked again next tick. Ordinary rule: yes. |
| `can_build(p, action, x, y, n)` | Before a builder order goes ahead. `action` is the order as given — `"trees"`, `"road"`, `"building"`, `"pill"`, `"mine"` or `"boat"` — before the engine decides whether the square makes it a repair. `n` is the pillbox for a pill order and `nil` otherwise. | `false` refuses the order. Ordinary rule: yes. |
| `can_capture(kind, n, p)` | When seat `p` would take `"pill"` or `"base"` number `n`. | `false` leaves it where it is and does not stop the tank. Ordinary rule: yes. |
| `announce(kind, subject, actor)` | Before a newswire line is shown. `kind` is `"joined"`, `"left"`, `"base_captured"`, `"pill_captured"`, `"builder_lost"`, `"name_changed"`, `"alliance"` or `"vote"`. `subject` is the base or pillbox number for a capture and a seat otherwise; `actor` is the seat that did it. | `false` keeps the line off every client's newswire. The fact still happens. Ordinary rule: shown. |
| `can_die(kind, n, killer, cause)` | When a blow would destroy a `"tank"`, a `"builder"` or a `"pill"`. `n` is the seat for a tank or its builder and the pillbox number for a pill. `cause` is `"shell"`, `"mine"`, `"deep_sea"` or `"script"` for a tank, `"shell"` or `"mine"` for the other two, and `nil` when the engine could not name it. | `false` leaves a tank at zero armour and alive, a builder untouched, a pillbox at one armour. Ordinary rule: yes. |
| `on_choose_start(p)` | When the engine is about to pick a start for seat `p`, at a spawn, a respawn or a teleport with no start named. A start the script named in the op itself is not asked about. | A start number, counted from 1 as `game.start` counts. A number that names no live start is reported and the engine picks. `nil` lets the engine pick. |
| `spawn_loadout(p)` | When seat `p`'s tank is created, unless the op that spawned it named a `loadout` of its own. A named one outranks the policy and is taken as it is read, so the policy is not asked for that tank and the named amounts are spent on it rather than held for the seat's next life. | `"open"`, `"tournament"` or `"strict"` for that game type's loadout, or a table of all four amounts, `{ shells = , mines = , armour = , trees = }`, each 0 to 255. A table short of one is reported and the ordinary loadout stands. |
| `damage_scale(attacker, victim, cause)` | On every hit a tank takes, with `cause` as `can_die` spells it for a tank. | A percent, 0 to 10000. 100 is the ordinary amount and 0 is a hit that costs nothing. Out of range is reported and 100 stands. |

`scenario.lobby.max_players` is the one decision that is a number rather than
a function; it is applied by the lobby without asking.

**Which round answers.** A round's own state is booted before the round places
anything, so `on_choose_start` and `spawn_loadout` for the seats already in the
round are answered by the state that is about to play it, with that round's
globals, and the opening tanks are built under the rules the same file asked
for. A seat that joins or respawns later is answered by the same state.

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
and `add_start` answer the index they took, and `lobby_add_bot` answers the
seat. `spawn_bot` answers the seat only when the call named one with `slot`; a
spawn that left the seat to the server is decided when it lands, so the call
answers `true, "queued"` and the seat is learned from `on_tank_spawned`.

**Where a square is optional, 255 for both coordinates means the same as
leaving them out**, and where a count is optional, 255 means keep it. That is
how `drop_pill`, `builder_parachute`, `set_builder_carried` and `sound` read
an absent argument.

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
detail, never cut short. A string with a NUL byte inside it ends at the NUL as
far as the server is concerned.

---

## Reading the world

### Counts and the clock

| Call | Answers |
|---|---|
| `game.tick()` | The tick the round is on. It counts at 100 a second and goes up by 2 between one `on_tick` and the next. |
| `game.max_tanks()` | How many seats a game has. |
| `game.num_players()` | How many seats are playing the round, bots included. |
| `game.num_humans()` | How many of those are people. |
| `game.team_size(t)` | How many seats sit on team `t`, playing the round or not. |
| `game.game_type()` | `"open"`, `"tournament"` or `"strict"` — the game the round is being played by. It answers `scenario.game` when the table sets one of those three, and the game the round resolves to otherwise, so a table that named no game or a word the server has no behaviour for reads `"strict"`. It never answers `"scripted"`: that is what the lobby calls the round, not a set of rules anything plays by. |

### Three clocks

The word "tick" means three different things, and a scenario meets all three.
They are worth twenty seconds of your time now.

**The simulation runs 50 frames a second.** Every frame is 20 milliseconds of
play. Everything below is counted against that.

**1. `game.tick()` counts twice a frame — 100 a second.** A frame is two
half-steps and the counter goes up on each, so between one `on_tick` and the
next it has moved by 2. It is always even or always odd for the whole of a
round, never both.

That matters the moment you test it for a multiple. Half the values never
arrive, so a test for one of them lands on half the multiples it reads as, or
on none:

```lua
function on_tick(tick)
  -- 100 is even, and this round's ticks may all be odd, in which case this
  -- is never true. Where they are even it is true once a second.
  if tick % 100 == 0 then ... end

  -- Every other multiple of 99 is odd, so whichever parity the round has,
  -- this fires half as often as it looks like it will.
  if tick % 99 == 0 then ... end
end
```

Compare against a count of your own, or use `game.timer`, which takes seconds
and sidesteps the question:

```lua
game.timer(1, every_second)         -- and set it again from inside itself
```

**2. Your hooks run once a frame — 50 a second.** `on_tick` is called after
both half-steps, so a handler sees every second value of `game.tick()`.
Timers, the event hooks and the region hooks are all on this clock too: a
timer set for 3 seconds comes due 150 frames later, and it is checked once a
frame, so a delay shorter than 20 milliseconds still waits a whole frame.

**3. A rule whose name ends `_ticks` is on neither of those.** Those counters
belong to the thing they are counting for. A pillbox's step once a frame. A
tank's step once per input the server applies to it, and of the two a client
sends each frame only one moves the tank. Either way it comes out at about 50
a second, and **not** at the rate `game.tick()` moves. Read a `_ticks` rule as
roughly **fifty to the second**:

| Value | About |
|---|---|
| 50 | 1 second |
| 255 | 5 seconds — the classic `tank_death_ticks` |
| 1500 | 30 seconds |

So `tank_death_ticks = 1500` keeps a dead tank waiting half a minute, and
`tank_death_ticks = 150` — the value Wave Defense sets — is about three
seconds.

**The game clock is the exception.** `set_game_time` and `add_game_time` are
counted in `game.tick()`'s own units, 100 a second, because the round's time
limit comes down on every half-step. A minute is 6000, not 3000.

**Before the round runs, the clock is slower.** In the lobby the counter
moves once a frame rather than twice, and timers are measured against it, so
a timer set from `on_lobby`, `on_player_join` or `on_chat` before the round
starts comes due after about twice the seconds it asked for.

### The map

| Call | Answers |
|---|---|
| `game.map_name()` | What the map is called. |
| `game.map_tile(x, y)` | The terrain code at a square, or `nil` for a square off the map. |
| `game.is_mine(x, y)` | Whether a square holds a mine. |
| `game.terrain()` | Every square as one 65,536-byte string, the square at `(x, y)` at byte `y * 256 + x + 1`. One call instead of 65,536 — and a fresh 64 KiB string each time, so read it where you need the whole map, not from `on_tick`. |

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
and `fielded = false`. That is how Wave Defense finds its held seats:

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

A timer that sets another timer moves one call per frame however short the
delay: a run that feeds itself cannot spin inside a single frame.

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

**A write's square is 21 to 235 on both axes.** The reads cover the whole
map, 0 to 255, but the outer twenty squares are the sea the map is framed
with and nothing may be written there: a write naming one of those squares is
refused with `SCN_OP_BAD_SQUARE`, the same answer as a square off the map.
`fill_rect` over the whole map is `fill_rect(21, 21, 235, 235, t)`.

### Tanks

| Call | What it does |
|---|---|
| `game.set_stocks(p, t)` | Sets any of `t.shells`, `t.mines`, `t.armour` and `t.trees`; a stock the table leaves out is left alone. A value past the rule's cap (`tank_full_shells` and its siblings) is refused with `SCN_OP_RANGE`, not held at the cap. |
| `game.add_stocks(p, t)` | The same four as amounts to add, negative to take away. Each is held at the cap and at zero rather than refused. |
| `game.kill_tank(p[, killer])` | Kills a tank. `killer` is a seat, and has to be one that is connected. The cause is always `"script"`. Without a killer the death is the scenario's own, and `on_tank_killed` then sees the victim as its own killer, the way a drowning reads — so a handler counting suicides has to look at `cause` as well. |
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
| `game.set_pill_owner(n[, p])` | Hands a pillbox to a seat, or to nobody with `game.NEUTRAL` or with no seat named. A pillbox in a tank answers to whoever is carrying it, so this is refused with `SCN_OP_CARRIED` until it is dropped. |
| `game.set_pill_armour(n, a)` | How much a pillbox has left. 0 is a dead pillbox on the ground. |
| `game.set_pill_speed(n, s)` | The ticks between a pillbox's shots, counted the way a `_ticks` rule is — about fifty to the second, so 50 is a shot a second. Kept between the rules `pill_attack_min_ticks` and `pill_attack_ticks`; outside them it is refused with `SCN_OP_RANGE`. |
| `game.move_pill(n, x, y)` | Puts a pillbox on another square. |
| `game.set_base_owner(n[, p[, keep_stock]])` | Hands a base to a seat, or to nobody with `game.NEUTRAL` or with no seat named. `keep_stock` leaves what it holds; without it a base changing hands is emptied, as it is in play. |
| `game.set_base_stock(n[, armour[, shells[, mines]]])` | What a base holds. A stock left out is left alone, and one past the cap is held there. |

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
| `game.add_pill(x, y[, owner[, armour[, speed]]])` | Puts a new pillbox on the map and answers which one it is. Nobody's, dead, and firing at the round's own `pill_attack_ticks` unless told otherwise. `armour` is kept to `pill_max_armour` and `speed` between `pill_attack_min_ticks` and `pill_attack_ticks`, refused with `SCN_OP_RANGE` outside. |
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
| `game.fill_rect(x0, y0, x1, y1, t)` | Writes a rectangle of terrain. One too big for a frame's budget answers `true, "queued"` and finishes over the frames after it. Only one fill may be in progress: a second asked for while one is still landing is refused with `SCN_OP_RATE`, not queued behind it, so test the answer when you write several. |
| `game.place_mine(x, y[, owner[, visible]])` | Lays a mine on a square. `visible` shows it to everyone rather than to its owner's side. |
| `game.remove_mine(x, y)` | Takes a mine off a square without setting it off. |

---

## Bots and seats

| Call | What it does |
|---|---|
| `game.spawn_bot(t)` | Puts a bot into the running round. Answers the seat and `"queued"` when the call named a seat, and `true, "queued"` when the server is choosing one. |
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
| `brain` | The brain to run, named the way a team's is: the directory under the server's `brains/`, such as `GoalHunter_1.7`. Left out, the seat's own brain is used — the one its team was written with — and failing that the server's. A name this server does not have, a value with `/` or `\` in it, and `package:NAME` are each refused with `SCN_OP_NOT_FOUND`. |
| `team` | The team to join. A held seat keeps the team it was seated with. |
| `start` | The start to come in on, 1-based. Left out, the engine chooses. |
| `loadout` | What this one bot comes in with: `"open"`, `"tournament"` or `"strict"` for that game type's amounts. A word that is none of the three stops the call the way any bad argument does. It outranks `spawn_loadout`, which is not asked about this tank at all, and it is spent on the tank the spawn builds — the bot's next life is fuelled the way every other tank's is. Left out, `spawn_loadout` answers, and failing that the round's own game type. |
| `init` | A flat table of names to strings or numbers, handed to the brain at its first breath. Left out, the seat's own is used — the one its team was written with. |

`lobby_add_bot` takes `name`, `brain`, `team`, `slot` and `fielded`, where
`fielded = false` asks for the seat without the bot.

**The seat cycle.** A seat the `scenario` table seated and a wave fielded goes
back to being held when `remove_bot` names it, rather than being emptied — so
the next wave has it again. A seat that was not the template's is emptied, as a
removal has always done.

Give a seat the same `brain` and the same `init` table every wave, and the
`scenario` table's team block is the place to give it once rather than on every
spawn: a seat whose team names a `brain` and an `init` there is seated with
both, and a `spawn_bot` that leaves them out gets them. The runner behind a
held seat is kept across the unfielding, and it can only be handed to a spawn
naming the brain it is already running and the configuration its VM read at its
first breath. The brain's own state goes with it: a bot fielded for the second
wave keeps whatever its script stored during the first, and is told the tank is
new the way a respawn is. A spawn naming either differently gets a runner built
for it instead, which is what a wave transition costs when it is paid, and a
line in the server log saying which of the two differed. The `init` table is
read once, when the VM is built, so it is not the place for orders that change
from one wave to the next; `game.hint` below is.

The roster ops are the six above, `set_team` and `lobby_set_team` included.
All of them are refused inside `on_setup`: the round is still being built
there, and a roster edit would re-enter the machinery that is building it.
Field your first wave, and move seats between teams, from `on_start`.

---

### Hints: telling one bot what to do

| Call | What it does |
|---|---|
| `game.hint(p, t)` | Hands bot `p`'s brain an order: one flat table with a `verb` in it. |

A hint goes to **one bot's brain**, not to the world. The engine marshals the
table and never reads it: what a key means is a contract between the script
and the brain it was written for.

`t` is a flat table — names to values, nothing nested. A key is at most 23
bytes, a value at most 63, and there are at most 16 pairs; past any of those
the call is refused with `SCN_OP_TOO_BIG`. Values may be strings, numbers or
`true`/`false`, and **every one of them reaches the brain as text**, so

```lua
game.hint(p, { verb = "defend", base = 3, tight = true })
```

arrives as `t.verb == "defend"`, `t.base == "3"` and `t.tight == "true"`. A
brain reads a number back with `tonumber`.

`verb` is required: a table without one, or with an empty one, stops the call
the way any bad argument does. Everything else is the script's business.

**What the brain has to do.** The server calls the global `on_scenario_hint(t)`
on that bot's Lua state, if the brain defines one:

```lua
function on_scenario_hint(t)
  if t.verb == "goto" then ... end
end
```

A brain that does not define it ignores the hint and the call still answers
`true` — a scenario names a seat and cannot know which brain a server runs it
with. `game.hint` is refused for a seat with nobody in it
(`SCN_OP_NO_SUCH_PLAYER`) and for a human (`SCN_OP_IS_HUMAN`), and nothing
else: a bot that is dead or waiting to come in is handed the order anyway,
because its brain is still running.

**The seven standard verbs.** These are the words a brain that takes hints is
expected to know. Everything past them passes through untouched, so a brain
author and a script author can agree on words of their own.

| `verb` | Keys | Means |
|---|---|---|
| `goto` | `x`, `y`, and optionally `w`, `h` | Go to that square, or to the middle of that rectangle, and hold there |
| `attack` | `player` | Attack that seat's tank |
| `defend` | `pill` or `base` | Hold that pillbox or that base |
| `hold` | `x`, `y`, both optional | Stand still — on that square, or where the bot already is |
| `patrol` | `x1`, `y1`, `x2`, `y2`, … up to `x7`, `y7` | Walk the points in order, round and round |
| `escort` | `player`, and optionally `distance` | Stay with that seat, within `distance` squares of it (3 by default) |
| `avoid` | `x`, `y`, `w`, `h` | Keep out of that rectangle until told otherwise |

There is no `region` key. A brain has no region table to look a name up in, so
a script that wants a bot in a region reads the rectangle itself and sends the
numbers:

```lua
local r = game.region("keep")
game.hint(p, { verb = "goto", x = r.x, y = r.y, w = r.w, h = r.h })
```

**What GoalHunter does with them.** The brain routes a hint into the same
order machinery a chat line from a human ally reaches, so a hinted bot acks
it, holds it for the same sixty seconds, and drops it for anything a person
says afterwards. A player can call a scripted order off with `cancel all` or
by naming the bot — a bare `cancel` cannot, because that one releases only
the speaker's own order and a hint's sender is the scenario.

Three of the seven are as near as the brain's existing goals get. `defend` on
a **base** stands on the base, because there is no defend-a-base goal — a base
is not held the way a pillbox is. `avoid` is remembered rather than fed to the
pathfinder: a bot standing inside the rectangle leaves it, and a later `goto`
or `hold` inside it is turned down, but a route may still pass through.
`escort` follows a seat whose position the brain knows, which is an ally
**bot**; a human ally broadcasts nothing the brain can follow, so a bot told
to escort a person stays where it is.

---

## Talking to players, and ending the round

| Call | What it does |
|---|---|
| `game.message(text[, target])` | A line to everyone, to one seat with a number, or to a team with `{ team = t }`. `nil` and `"all"` both mean everyone. |
| `game.say(p, text[, target])` | A chat line seat `p` says, exactly as a player typing would: to its own team with no target, to everyone with `"all"`, or to one seat with a number. |
| `game.sound(name[, x, y])` | Plays one of the server's sounds, at a square or everywhere. |
| `game.log(text)` | Writes a line to the server's console. No player sees it. |
| `game.end_round([text[, winner_team]])` | Ends the round now, with the line the lobby shows and the team that won it. |
| `game.set_game_time(ticks)` | How long the round has left, in `game.tick()`'s own units: 100 a second, so a minute is 6000. |
| `game.add_game_time(ticks)` | Adds to what the round has left, or takes away with a negative, in the same units. A round with no time limit has nothing to add to, so give it a length first. |

`message` and `say` are two different voices, and the difference decides
whether a bot hears the line at all.

- **`message` is the server talking.** It writes a server line: the newswire
  a player reads, with no sender beside it. A brain never sees it — a brain's
  inbox is fed from chat alone — and `on_chat` does not fire for it.
- **`say` is a seat talking.** It writes that seat's own chat, down the same
  path a typed line takes, so it lands in the receiving brains' inboxes as
  `info.messages` with `sender` set to `p`, and `on_chat(p, text, true)`
  fires. This is how a scripted round hands a bot the order a human ally
  would have typed.

`say` has the three destinations a player has and no more. No target is the
seat's own team, `"all"` is everyone, and a seat number is that seat. A team
the sender is not on is not among them, because the chat path refuses a line
addressed to one whoever sends it. A sender never receives its own line, so
a scenario can tell one bot something without telling the one it spoke
through.

`say` is refused when `p`, or a named target, is a seat with nobody in it
(`SCN_OP_NO_SUCH_PLAYER`), when a team line comes from a seat on no team and
so has nobody to say it to (`SCN_OP_RANGE`), when the line is empty
(`SCN_OP_BAD_CALL`, since every receiver drops a chat body of no length), and
when the line is longer than a chat line can be (`SCN_OP_TOO_BIG`). A seat
held without a bot in it — `fielded = false` — is a seat for this purpose, so
a scenario can keep one back purely to speak from.

One caution about team chat. A receiver decides whether a team line is for it
from its own copy of the roster, which it builds from lobby-slot events. A
round started with no lobby at all — `-nolobby`, which is how a headless test
starts a round with no human to ready up — publishes none of those, so every
seat's copy says team 0 and the team filter drops the line. The op is
accepted, because the server's own roster does carry the team; it is the
receiver that never sees it. Use `"all"` or a seat number there.

`end_round` is how a scenario wins or loses a round. It stops play there and
then, and the line it carries is shown in the lobby exactly as written — the
server adds no verdict of its own.

---

## Showing things on a client

A scenario never draws on a client. It sends what it wants shown as data, and
each frontend draws that with its own 2D calls, so one scenario looks right on
the desktop game, in a browser and on a phone without knowing that any of them
exist.

| Call | What it does |
|---|---|
| `game.panel(id, list[, target])` | Draws panel `id` from a list of primitives. An empty list clears it. |
| `game.score(target, value[, label])` | The scenario's own score for one seat with a number, or for a team with `{ team = t }`. `label` is the short word shown beside it, up to 15 bytes. |
| `game.announce(text[, seconds[, target]])` | A line across the centre of the screen for that many seconds. Empty text takes the line away. |
| `game.marker(id, x, y[, colour[, target]])` | Puts mark `id` on a map square. |
| `game.marker_follow(id, p[, colour[, target]])` | Puts mark `id` on seat `p`, where it rides the tank rather than the ground. |
| `game.clear_marker(id[, target])` | Takes mark `id` off the map. |

There is one panel, id 0 — the square over the game view — and any other id is
refused with `SCN_OP_RANGE`. There are sixteen markers, numbered 0 to 15. A
marker id holds one mark: putting a second one on an id replaces the first,
and `clear_marker` takes it off. A colour left out of a marker call is
`yellow`.

`announce` is counted in seconds and the client counts it down in ticks, at
100 a second — the same rate `game.timer` converts at, and the one
[Three clocks](#three-clocks) describes. Two seconds is 200 ticks, and the
longest a line can be asked to stay up is 65535 ticks, about 655 seconds. A
line with something in it and a time of zero is refused, and one written with
no time at all stops the script; an empty line is the clear, and it needs
neither. In the lobby the clock moves at half that
rate, so a line put up before the round starts stays up about twice as long as
it asked for.

**Who sees it.** The last argument of `panel`, `announce` and the three marker
calls is the same target every other call takes: left out, or `"all"`, for
everyone; a number for one seat; `{ team = t }` for one team. A late joiner is
given whatever the panels hold at the moment it arrives, so a panel put up in
the first tick of a round is still there for somebody who connects in the
tenth minute.

`score` is the exception. Its target says *whose* score it is, not who sees
it, so a score is shown to everyone. There is no everyone's score: a call with
no target, or with `"all"`, is the call written wrong and stops the script.

**One update per panel per audience per tick.** Every update replaces the
whole list, so a second update to the same panel for the same audience in the
same tick is refused with `SCN_OP_RATE` rather than queued — the one it would
have replaced was never going to be seen. Giving each of sixteen players their
own copy of panel 0 in one tick is fine: the audience is part of the key.

### The panel

A panel is a square of 128 logical units on a side, origin top-left, with `x`
running right and `y` running down. Each frontend maps that square into a slot
in its own layout and scales it with the game's zoom, so a script never knows
a pixel size. Coordinates are bytes: a primitive may start inside the square
and run past its edge, and the frontend clips it.

A list is an array of primitives. Each primitive is an array whose first
element is its name, with the operands after it in the order below:

| Primitive | Written as | Draws |
|---|---|---|
| `rect` | `{ "rect", x, y, w, h, colour, fill }` | A rectangle, outlined or filled. `fill` is `true` or `false`. |
| `line` | `{ "line", x0, y0, x1, y1, colour }` | A one-unit line. |
| `text` | `{ "text", x, y, colour, size, align, s }` | `s` in the frontend's own font, up to 48 bytes, and only bytes a panel draws — no newlines or escapes. |
| `name` | `{ "name", x, y, colour, size, align, p }` | The name seat `p` is playing under, so a script never sends names and a rename shows through. |
| `sprite` | `{ "sprite", x, y, tile }` | One tile from the skin's own sheet, by its `tilenum.h` id: terrain, a pillbox, a base, a tank frame. |
| `bar` | `{ "bar", x, y, w, h, colour, value, max }` | A horizontal bar filled to `value` over `max`, outlined. Both are 16-bit, so a bar can show a real total. |
| `timer` | `{ "timer", x, y, colour, size, align, mode, tick }` | Minutes and seconds counting down to, or up from, a game tick. The client works it out against its own clock, so a countdown is one message rather than one a tick. |

`size` is `"small"` or `"normal"`. `align` is `"left"`, `"centre"` or
`"right"`, and says which way the text sits about its `x`. A timer's `mode` is
`"down"` or `"up"`, and its `tick` is a tick on `game.tick()`'s clock.

```lua
game.panel(0, {
  { "rect",  0, 0, 128, 20, "grey_dark", true },
  { "text",  64, 4, "white", "normal", "centre", "Wave 3" },
  { "bar",   4, 24, 120, 8, "green", holding, 20 },
  { "timer", 64, 40, "yellow", "normal", "centre", "down", ends_at },
})

game.panel(0, {})            -- take it away again
```

A list holds up to 128 primitives and is refused with `SCN_OP_TOO_BIG` past
that, or if the whole list comes to more than the 1017 bytes one update
carries. A primitive written wrong — a name that spells no primitive, an
operand missing, an operand that is not a number — stops the script, like any
other call written wrong. A value the simulation will not take, such as a
colour outside the palette, is refused as an answer the script can read.

### Colours

Every colour is an index into a palette of sixteen, so a skin or a dark mode
maps the sixteen rather than a script choosing RGB. A colour is written as its
name or as its number:

`none`, `black`, `white`, `grey`, `grey_dark`, `red`, `green`, `blue`,
`yellow`, `orange`, `cyan`, `magenta`, `reserved_12`, `reserved_13`,
`reserved_14`, `reserved_15`.

`none` draws nothing, and the four reserved entries draw nothing until a skin
gives them a colour. The names are also on the `game` table as
`game.COLOUR.red` and so on, beside `game.SIZE`, `game.ALIGN` and
`game.TIMER_MODE`, for a script that computes one rather than writing it.

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

**A rule whose name ends `_ticks` counts at about fifty to the second**, which
is not the rate `game.tick()` moves at. 50 is a second, 255 is five, 1500 is
half a minute. [Three clocks](#three-clocks) says why.

### The catalogue

Every rule, with the value the unmodified game plays under and the range a
value is accepted in. A rule listed as "and up" has a floor and no ceiling of
its own; several of those are bounded instead by another rule, and the pairs
below the tables say which.

**Tank.**

| Rule | Classic | Range | Description |
|---|---|---|---|
| `tank_reload_ticks` | 13 | 0 to 255 | Ticks a tank waits between shots. |
| `tank_full_shells` | 40 | 0 to 255 | The most shells a tank can hold. |
| `tank_full_mines` | 40 | 0 to 255 | The most mines a tank can hold. |
| `tank_full_trees` | 40 | 0 to 255 | The most trees a tank can hold. |
| `tank_full_armour` | 40 | 0 to 255 | The most armour a tank can hold, and what it starts a life with. |
| `tank_death_ticks` | 255 | 0 to 65535 | Ticks a destroyed tank waits before it comes back. |
| `tank_water_ticks` | 15 | 1 to 255 | Ticks a tank wades in a river before the water costs it a shell and a mine. |
| `shell_damage` | 5 | 1 to 255 | Armour a shell takes off the tank or base it hits. |
| `mine_damage` | 15 | 1 to 255 | Armour a mine takes off the tank that sets it off. |
| `mine_damage_range` | 256 | 0 to 65535 | How far off the mine's centre a tank is still caught, in world units, tested on each axis so it is a box rather than a circle. The classic 256 is one map square each way; 0 is a mine that catches nobody. |
| `mine_fatal_divisor` | 3 | 1 to 255 | A mine blow big enough to kill has one part in this many taken back off it, so the tank keeps facing the rest, rounded in its favour. The classic 3 is the two-thirds a fatal mine hit deals. Bigger is harsher, not softer: 255 leaves a fatal blow at almost its full size, and 1 takes the whole thing off and leaves a fatal mine dealing nothing at all. |
| `water_loss_shells` | 1 | 0 to 255 | Shells a wading tank loses each `tank_water_ticks`. |
| `water_loss_mines` | 1 | 0 to 255 | Mines a wading tank loses each `tank_water_ticks`. |
| `just_fired_ticks` | 101 | 0 to 255 | Ticks a tank stays visible in the trees after firing. |
| `tree_hide_distance` | 768 | 0 to 65535 | How far off a tank sitting in forest stops being drawn for somebody else, in world units — the classic 768 is three squares. 0 is a wood that hides nobody. |
| `gunsight_min` | 2 | 1 to 255 | The shortest the gunsight range winds down to. |
| `gunsight_max` | 14 | 1 to 255 | The longest the gunsight range winds out to. |
| `tank_accel_rate` | 0.25 | 0.01 to 16.0 | Speed a tank gains each tick while the accelerate key is held. |
| `tank_decel_rate` | 0.25 | 0.01 to 16.0 | Speed a tank loses each tick while it is above the terrain's cap. |
| `tank_brake_rate` | 0.25 | 0.01 to 16.0 | Speed a tank loses each tick while the slow key is held. |
| `tank_autoslow_rate` | 0.25 | 0.01 to 16.0 | Speed a tank loses each tick when auto-slowdown is on and no key is held. |
| `tank_min_move` | 6 | 0 to 255 | Speed a tank has to build up before it moves a step. |

**Tank collision geometry.** What a tank is, as a shape, to a shell and to
the world it drives through. Distances here are world units, 256 to the map
square.

| Rule | Classic | Range | Description |
|---|---|---|---|
| `tank_hit_radius` | 112 | 1 to 255 | The circle a shell has to reach, and the one the building resolver pushes a tank out of. Squared where it is used, which is why it stops at 255. |
| `tank_collision_distance` | 256 | 0 to 65535 | How far apart two tanks have to get for the shoving to stop, measured as the x gap plus the y gap rather than as a straight line. 0 is tanks that pass through one another. |
| `tank_nudge_threshold` | 96 | 0 to 65535 | Which way a shove goes. Two tanks are pushed apart along x when their x gap is over this and along y otherwise, so a smaller number sends more shoves sideways. |
| `tank_nudge_amount` | 16 | 1 to 255 | How far one shove moves a tank. |
| `tank_nudge_iterations` | 5 | 1 to 255 | How many shoves are tried in a tick before the pair is left where it is. |
| `tank_bump_decay_shift` | 2 | 0 to 31 | A right shift: a knockback loses that fraction of itself each tick, plus one. A bigger number is a slower decay, not a faster one — 2 takes off a quarter a tick, 31 takes off only the one. |
| `tank_pill_pickup_inset` | 16 | 0 to 255 | How far off its centre a tank reaches to pick a dead pillbox up. |
| `tank_boat_exit_inset` | 64 | 0 to 255 | How far inside the bank a boat is held when a tank leaves one. |
| `tank_slide_step` | 32 | 0 to 255 | How far a knocked tank slides per step. |
| `tank_wall_glide` | 0.0 | 0.0 to 1.0 | 0 slides a tank along a wall it hits; 1 lets it glide off free. |

**Terrain: the cap a tank's speed clamps to.**

| Rule | Classic | Range | Description |
|---|---|---|---|
| `speed_road` | 16 | 0 to 63 | The fastest a tank may drive on a road. |
| `speed_grass` | 12 | 0 to 63 | The fastest a tank may drive on grass. |
| `speed_forest` | 6 | 0 to 63 | The fastest a tank may drive through forest. |
| `speed_river` | 3 | 0 to 63 | The fastest a tank may drive through a river, and the speed at or below which it wades. |
| `speed_swamp` | 3 | 0 to 63 | The fastest a tank may drive through swamp. |
| `speed_crater` | 3 | 0 to 63 | The fastest a tank may drive through a crater. |
| `speed_rubble` | 3 | 0 to 63 | The fastest a tank may drive over rubble. |
| `speed_boat` | 16 | 0 to 63 | The fastest a boat carries a tank, and the speed it leaves the boat with. |
| `speed_deep_sea` | 3 | 0 to 63 | The fastest a tank may drive on deep sea. |
| `speed_refuel_base` | 16 | 0 to 63 | The fastest a tank may drive over a base it is allowed onto. |

**Terrain: bradians turned per tick.**

| Rule | Classic | Range | Description |
|---|---|---|---|
| `turn_road` | 1 | 0.0 to 16.0 | How fast a tank turns on a road. |
| `turn_grass` | 1 | 0.0 to 16.0 | How fast a tank turns on grass. |
| `turn_forest` | 0.5 | 0.0 to 16.0 | How fast a tank turns in forest. |
| `turn_river` | 0.25 | 0.0 to 16.0 | How fast a tank turns in a river. |
| `turn_swamp` | 0.25 | 0.0 to 16.0 | How fast a tank turns in swamp. |
| `turn_crater` | 0.25 | 0.0 to 16.0 | How fast a tank turns in a crater. |
| `turn_rubble` | 0.25 | 0.0 to 16.0 | How fast a tank turns on rubble. |
| `turn_boat` | 1 | 0.0 to 16.0 | How fast a tank turns while it is on a boat. |
| `turn_deep_sea` | 0.5 | 0.0 to 16.0 | How fast a tank turns on deep sea. |
| `turn_refuel_base` | 1 | 0.0 to 16.0 | How fast a tank turns on a base it is allowed onto. |

**Terrain: the cap the builder's walk clamps to.** A man is on his own table,
not the tank's: he crosses swamp and rubble faster than a tank does and
cannot cross a river at all. Building, half-building and pillbox are absent
for the reason they are absent above — they are impassable by terrain type
rather than by having no speed.

| Rule | Classic | Range | Description |
|---|---|---|---|
| `man_speed_road` | 16 | 0 to 63 | The cap the builder's walk clamps to on a road. |
| `man_speed_grass` | 16 | 0 to 63 | The cap on grass. |
| `man_speed_forest` | 8 | 0 to 63 | The cap in forest. |
| `man_speed_river` | 0 | 0 to 63 | The cap in a river; 0 is the classic man, who cannot wade one. |
| `man_speed_swamp` | 4 | 0 to 63 | The cap in swamp. |
| `man_speed_crater` | 4 | 0 to 63 | The cap in a crater. |
| `man_speed_rubble` | 4 | 0 to 63 | The cap on rubble. |
| `man_speed_boat` | 16 | 0 to 63 | The cap on a boat. |
| `man_speed_deep_sea` | 0 | 0 to 63 | The cap in deep sea. |
| `man_speed_refuel_base` | 16 | 0 to 63 | The cap on a refuelling base. |

**Shells.**

| Rule | Classic | Range | Description |
|---|---|---|---|
| `shell_life` | 8 | 1 to 255 | Ticks a shell flies for each unit of gunsight range. |
| `shell_speed` | 32 | 1 to 255 | How far a shell travels each tick. |
| `shell_start_add` | 5 | 0 and up | How far ahead of the tank a shell starts, counted in ticks of its own travel. |

**Builder.**

| Rule | Classic | Range | Description |
|---|---|---|---|
| `lgm_build_ticks` | 20 | 0 to 255 | Ticks the builder spends at the square doing a job. |
| `lgm_cost_road` | 2 | 0 and up | Trees the builder spends to lay a road. |
| `lgm_cost_building` | 2 | 0 and up | Trees the builder spends to put up a wall. |
| `lgm_cost_repair_building` | 1 | 0 and up | Trees the builder spends to mend a damaged wall. |
| `lgm_cost_pill_repair` | 1 | 0 and up | Trees one unit of pillbox repair costs. |
| `lgm_cost_boat` | 20 | 0 and up | Trees the builder spends to build a boat. |
| `lgm_cost_pill_new` | 4 | 0 and up | Trees the builder spends to place a pillbox the tank is carrying. |
| `lgm_cost_mine` | 1 | 0 and up | Mines the builder spends to lay a mine. |
| `lgm_pill_repair_load` | 4 | 1 to 255 | Units of pillbox repair the builder carries in one trip. |
| `lgm_gather_trees` | 4 | 1 to 255 | Trees the builder brings back from one square of forest. |
| `lgm_helicopter_speed` | 3 | 1 to 255 | How far the builder travels each tick while he parachutes in, so it is also how long he is in the air: the drop is the distance over this. The rule's name is the one the code has always carried for the drop; everything a player meets calls it a parachute, `game.builder_parachute` included. See the note below the table before you raise it. |
| `lgm_arrive_tolerance` | 16 | 1 to 255 | How near his goal counts as arrived, as a half-width taken either way round it on both axes. The job itself is done on the square that was ordered, not the one he stopped on. |
| `lgm_return_tolerance` | 128 | 1 to 65535 | The same, coming back to the tank. |
| `lgm_pill_drop_search` | 10 | 1 to 255 | How many squares of a column the search for somewhere to put a pillbox down walks. |
| `lgm_boat_leave_offset` | 144 | 0 to 255 | How far out he steps when he boards a boat. |
| `lgm_boat_return_offset` | 160 | 0 to 255 | How far in he steps coming off one. |

**Raising `lgm_helicopter_speed` means raising `lgm_arrive_tolerance` with
it.** The drop steps straight at the tank and counts as landed once both axes
are inside the arrival tolerance, so a step wider than that window steps over
it and the man circles his tank without ever touching down. At the classic
tolerance of 16, a speed of 90 fails to land on more than half its flights. A
tolerance at or above the step lands every one of them.

**Pillbox.**

| Rule | Classic | Range | Description |
|---|---|---|---|
| `pill_max_armour` | 15 | 1 to 255 | The most armour a pillbox holds, and what a newly built one starts with. |
| `pill_attack_ticks` | 100 | 1 to 255 | Ticks between shots from a pillbox nobody has hit. |
| `pill_attack_min_ticks` | 6 | 1 and up | The shortest the interval between a hurt pillbox's shots falls to. |
| `pill_cooldown_ticks` | 32 | 0 to 255 | Ticks an angry pillbox waits before its interval eases back by one. |
| `pill_repair_amount` | 4 | 1 and up | Armour a pillbox gains from one unit of repair. |
| `pill_range` | 2048 | 0 to 65535 | How far a pillbox looks for a tank to shoot at. |
| `pill_shell_damage` | 1 | 1 and up, at most `pill_max_armour` | What one shell takes off a pillbox. Setting it to the cap is a pillbox killed by a single hit. |
| `pill_angry_divisor` | 2 | 1 to 255 | An angered pillbox divides its firing interval by this and is held at `pill_attack_min_ticks`, so the classic 2 is the twice as fast it fires when hurt and 1 is a pillbox that never gets angry. |
| `pill_fire_length` | 8.5 | 0.5 to 127.0 | How far a pillbox's shell flies, in half map squares, the way the gunsight rows are counted. |
| `pill_base_defend_range` | 7 | 0 to 255 | How near a base being shot at has to be to anger an allied pillbox, as a radius in map squares measured as a circle and taken strictly: a pillbox exactly this far off is not angered. 0 is a pillbox that never answers for a base at all. |
| `pill_aim_iterations` | 200 | 1 to 65535 | The step budget the aim solver gets to lead a moving target. 1 is a pillbox that never leads and fires at where the target is standing now. |
| `pill_massage_range` | 0 | 0 to 65535 | How near a tank has to be for a pillbox to aim with the original forward prediction rather than the solver, which misses a tank circling it. Zero, the classic table, is a pillbox that always leads its target properly; 384 is a square and a half, the distance the old build-time switch used. |
| `pill_massage_cosine` | 0.5 | 0.0 to 1.0 | How straight at a close pillbox a tank has to be driving to be aimed at properly anyway, as the cosine of the angle between its heading and the line to the pillbox. One aims sloppily at every tank inside `pill_massage_range`, zero at none of them. Does nothing while that rule is zero. |

**Base.**

| Rule | Classic | Range | Description |
|---|---|---|---|
| `base_full_armour` | 90 | 0 to 255 | The most armour a base holds. |
| `base_full_shells` | 90 | 0 to 255 | The most shells a base holds. |
| `base_full_mines` | 90 | 0 to 255 | The most mines a base holds. |
| `base_capture_armour` | 9 | 0 and up | The armour at or below which a base is taken by the next tank to drive onto it. |
| `base_hit_armour` | 4 | 0 and up | The armour a base has to be above before an enemy shell can hit it. |
| `base_min_armour` | 10 | 0 and up | Armour a base keeps back rather than hand out. |
| `base_min_shells` | 0 | 0 and up | Shells a base keeps back rather than hand out. |
| `base_min_mines` | 0 | 0 and up | Mines a base keeps back rather than hand out. |
| `base_armour_give` | 5 | 0 and up | Armour a base hands a tank each time it refuels one. |
| `base_shells_give` | 1 | 0 and up | Shells a base hands a tank each time it refuels one. |
| `base_mines_give` | 1 | 0 and up | Mines a base hands a tank each time it refuels one. |
| `base_refuel_armour_ticks` | 46 | 1 to 255 | Ticks a base waits between handing out one lot of armour and the next. |
| `base_refuel_shells_ticks` | 7.5 | 0.5 to 255.0 | Ticks a base waits between handing out one lot of shells and the next. |
| `base_refuel_mines_ticks` | 7.5 | 0.5 to 255.0 | Ticks a base waits between handing out one lot of mines and the next. |
| `base_regen_ticks` | 1000 | 1 and up | Ticks between a base adding one armour, one shell and one mine to its own stock. |
| `base_status_range` | 1792 | 0 to 65535 | How near a base has to be for a player to be shown what it is holding, in world units — the classic 1792 is seven squares. |
| `base_reveal_range` | 1024 | 0 to 65535 | How near before a base's armour is worth predicting for the player, in world units and measured as a circle — the classic 1024 is four squares. |

**Terrain destruction and explosions.**

| Rule | Classic | Range | Description |
|---|---|---|---|
| `building_life` | 4 | 1 to 255 | Shell hits a wall stands before it falls to rubble. |
| `rubble_life` | 4 | 1 to 255 | Shell hits rubble stands before it washes away to river. |
| `grass_life` | 4 | 1 to 255 | Shell hits grass stands before it turns to swamp. |
| `swamp_life` | 3 | 1 to 255 | Shell hits swamp stands before it turns to river. |
| `mine_fuse_ticks` | 10 | 1 to 255 | Ticks between a mine being set off and it going up. |
| `big_explosion_threshold` | 20 | 0 to 510 | Shells and mines a dying tank has to be carrying to go up in a big explosion. |
| `tank_explosion_damage` | 5 | 0 and up, at most `pill_max_armour` | The splash a dying tank deals a pillbox it goes up next to. 0 is a wreck that scorches nothing. |
| `tank_explosion_length` | 40 | 1 to 255 | How many steps the wreck travels before it stops. |
| `tank_explosion_move` | 48 | 0 to 255 | How far the wreck moves in one of those steps, in world units. |
| `tank_explosion_update_ticks` | 2 | 1 to 255 | Ticks between the steps. |
| `tank_explosion_width` | 24 | 0 to 255 | Half the wreck's collision box across, in world units. |
| `tank_explosion_height` | 32 | 0 to 255 | Half the wreck's collision box down, in world units. |

**Spawning.** How a respawn picks its start. The three ranges are what counts
as too near a tank, a pillbox or a base, all in map squares.

| Rule | Classic | Range | Description |
|---|---|---|---|
| `start_tank_range` | 1 | 0 to 255 | How near a live tank is too near to start on. 0 on any of the three is a spawn that does not care what is standing there. |
| `start_pill_range` | 9 | 0 to 255 | How near an enemy pillbox is too near. |
| `start_base_range` | 9 | 0 to 255 | How near an enemy base is too near. |
| `start_spawn_separation` | 2 | 0 to 255 | How far a scattered spawn keeps from another live tank. |
| `start_scatter_max` | 1000 | 1 to 65535 | How long the spiral search looks before it gives up and takes what it has. |
| `start_neutral_threshold_pct` | 20 | 0 to 100 | The share of bases still neutral below which a player's own base is preferred to a neutral one. |

**Hearing.** How far a sound carries, in map squares. Inside the soft range it
is played near, past the none range it is not played at all, and between the
two it is played far.

| Rule | Classic | Range | Description |
|---|---|---|---|
| `sound_soft_range` | 15 | 0 to 255 | Inside this a sound is played near. |
| `sound_none_range` | 40 | 0 to 255 | Past this a sound is not played at all. |

**Terrain flooding.**

| Rule | Classic | Range | Description |
|---|---|---|---|
| `flood_fill_ticks` | 16 | 1 to 255 | Ticks water takes to claim a square next to it. |

**Tree growth.**

| Rule | Classic | Range | Description |
|---|---|---|---|
| `tree_grow_ticks` | 3000 | 1 and up | Ticks before the best square found so far grows its tree. |
| `tree_grow_initial_ticks` | 30000 | 1 and up | Ticks before the first tree of a round grows. |
| `tree_grow_initial_score` | -10000 | -32768 to 32767 | What the weighted draw below starts and resets from, so how long a map waits for its first tree. Negative by design, and it shares its window with the weights. |
| `tree_weight_forest` | 100 | -32768 to 32767 | What forest counts for in a square's tree growing score. |
| `tree_weight_grass` | 25 | -32768 to 32767 | What grass counts for in a square's tree growing score. |
| `tree_weight_river` | 2 | -32768 to 32767 | What a river counts for in a square's tree growing score. |
| `tree_weight_boat` | 1 | -32768 to 32767 | What a boat counts for in a square's tree growing score. |
| `tree_weight_deep_sea` | 0 | -32768 to 32767 | What deep sea counts for in a square's tree growing score. |
| `tree_weight_swamp` | 2 | -32768 to 32767 | What swamp counts for in a square's tree growing score. |
| `tree_weight_rubble` | -2 | -32768 to 32767 | What rubble counts for in a square's tree growing score. |
| `tree_weight_building` | -20 | -32768 to 32767 | What a wall counts for in a square's tree growing score. |
| `tree_weight_half_building` | -15 | -32768 to 32767 | What a damaged wall counts for in a square's tree growing score. |
| `tree_weight_crater` | -2 | -32768 to 32767 | What a crater counts for in a square's tree growing score. |
| `tree_weight_road` | -100 | -32768 to 32767 | What a road counts for in a square's tree growing score; a pill or base square counts the same. |
| `tree_weight_mine` | -7 | -32768 to 32767 | What a laid mine adds to a square's tree growing score, on top of the terrain under it. |

**Pairs.** A value inside its own range can still be refused with
`SCN_OP_PAIR` when it breaks one of these:

- `pill_attack_min_ticks` at most `pill_attack_ticks`.
- `base_capture_armour` at most `base_full_armour`; `base_hit_armour` at most
  `base_capture_armour`.
- `base_min_armour`, `base_min_shells` and `base_min_mines` at most the
  matching `base_full_*`.
- `base_armour_give`, `base_shells_give` and `base_mines_give` at most the
  matching `base_full_*` less the matching `base_min_*`.
- `lgm_cost_road`, `lgm_cost_building`, `lgm_cost_repair_building`,
  `lgm_cost_boat` and `lgm_cost_pill_new` at most `tank_full_trees`;
  `lgm_cost_pill_repair` times `lgm_pill_repair_load` at most
  `tank_full_trees`; `lgm_cost_mine` at most `tank_full_mines`.
- `lgm_pill_repair_load` times `pill_repair_amount` at least `pill_max_armour`,
  and `pill_repair_amount` at most `pill_max_armour`.
- `pill_shell_damage` and `tank_explosion_damage` at most `pill_max_armour`.
- `sound_soft_range` at most `sound_none_range`, so there is a far band left
  for a sound to land in.
- `gunsight_min` at most `gunsight_max`; `shell_start_add` at most half of
  `shell_life` times `gunsight_min`, so the shortest shot still travels; and
  half of `shell_life` times `gunsight_max`, less `shell_start_add`, plus 1,
  at most 255, so the longest shot's life still fits its byte.

The detail a refusal carries names both sides with their numbers.

---

## Triggers

A trigger is a hook to listen on, some tests against what that hook is handed,
and some calls to make when every test holds. It is data in the `scenario`
table rather than a function you write, so a scenario that only wants to say
"when the keep falls, announce it" needs no Lua at all.

```lua
scenario = {
  name = "Hold the Keep",
  tags = { pills = { [1] = "keep", [2] = "keep" } },

  triggers = {
    { when    = "on_pill_captured",
      where   = { { "tag", "in", "keep" } },
      actions = { { "announce", "The keep has fallen", 5 },
                  { "sound", "big_explosion_near" } } },

    { when    = "on_tank_killed",
      -- killer is an owner, and a drowning has none: the test on its team
      -- is what keeps the owner who is nobody out of score's seat.
      where   = { { "victim_team", "eq", 1 },
                  { "killer_team", "gte", 0 } },
      actions = { { "score", { field = "killer" }, 10 } } },
  },
}
```

Three keys, all optional but `when`:

| Key | What it is |
|---|---|
| `when` | The hook to run on, by name. A trigger that names none, or names a policy, never runs. |
| `where` | The tests, as `{ field, operator, value }` rows. **Every one has to hold**, so no `where` at all means the trigger fires every time its hook does. |
| `actions` | The calls, as `{ op, argument... }` rows, run in the order written. |

A trigger the router never sees says so at load. A `when` the build has no
hook for — a typo, or a policy — is left out when the triggers are handed
over, and a `where` that is there but is not an array takes its trigger with
it as the table is read: one kept with tests that could not be read would
fire every time its hook did. Each gets a line on the console under its own
position, `triggers[3]`, so a table written by hand says what it lost without
waiting for `-validate`. Positions count from zero, as the file writes them:
`triggers[0]` is the first trigger in the table, and `triggers[0].actions[1]`
is the second action of it. That is one less than the index Lua gives the same
entry, so `triggers[3]` is the fourth row of your table, not the third.

Triggers run in the order the table writes them, and the actions inside one
run in the order that trigger writes them. A trigger whose tests do not hold
is skipped and the next is tried; nothing stops at the first miss.

### The fields a hook offers

A `where` row names a **field**, which is either one of the hook's own
parameters or one derived from it. The parameters are the ones in the
[Hooks](#hooks) tables, by the names written there. On top of those:

| Derived field | Off | What it answers |
|---|---|---|
| `<name>_team` | a seat or an owner | That seat's team. `on_tank_killed` offers `victim_team` and `killer_team`. |
| `tag` | a pillbox or a base | The tags on that entity — a **set**, not one value. |
| `region` | a square `x` and the `y` beside it | The regions that square is inside — a **set**. |

So `on_tank_killed(victim, killer, cause)` offers `victim`, `killer`, `cause`,
`victim_team` and `killer_team`, and `on_pill_captured(n, old, new)` offers
`n`, `old`, `new`, `tag`, `old_team` and `new_team`.

Three hooks are handed nothing and so offer no field: `on_setup`, `on_start`
and `on_end`. A trigger on one of those runs every time, and a `where` row
against it is refused.

A hook's `scripted` flag is **not** a field. It is handed to your own
handler, but it is added after the declared parameters and the fields come
from those, so a trigger fires whether the event was the script's doing or the
round's.

**Seven operators**, and which of them a field takes depends on whether it
answers one value or a set:

| | Operators |
|---|---|
| A field that answers one value | `eq`, `ne`, `lt`, `lte`, `gt`, `gte` |
| A set — `tag` and `region` | `in` for "holds this name", `ne` for "does not" |

`in` on a single value and `eq` on a set are both refused before the round
starts, because neither asks a question the other side can answer. The one
that looks like an exception is not: `on_enter_region`'s own `name` parameter
is a region, but the hook hands it over as the one name it is rather than as a
set, so `eq` on it is right and `in` on it is refused.

**A value** is one of four things:

| Written | Means |
|---|---|
| `12`, `0.5` | a number |
| `"keep"` | a string — a tag, a region, or one of the words a field takes |
| `true`, `false` | a boolean |
| `{ field = "killer" }` | whatever that field of the **same hook** holds when the trigger fires |

The last one is how two halves of the same event are compared:
`{ "victim_team", "eq", { field = "killer_team" } }` is a team killing its own.

Values of different kinds compare `false` rather than raising — `ne` included,
so a row that cannot be worked out never holds either way round. A field that
reads nothing also does not hold: an owner who is nobody has no team, and a
seat the roster has nothing in has none either.

**An action** names an op and gives its arguments positionally, using the same
four kinds of value. It can name any `game.*` call that changes something and
takes flat arguments — which leaves out the reads, since an action's answer
goes nowhere, and the handful that take a table or a function, which a row
cannot write. `game.panel` is the one worth naming: its `list` is a table, so
it is a script's to call, not a trigger's.

**Who an action can address.** An op's `target` takes a seat number or one of
the words the surface names — `"all"` is the one — and from a trigger nothing
else: the third form the ops themselves take, `{ team = t }`, is a table, and
a trigger's argument is only a number, a string, a boolean or a `{ field = }`
reference. A trigger that has to address a team does it through `call`, whose
function is your own Lua and can write the table. `game.score` is the one
`target` that refuses `"all"`, a score being one seat's or one team's, so a
trigger's score is a seat's.

A `target` is one of the two arguments that take either a number or a word.
The other is the `colour` on `marker` and `marker_follow`, which is the
palette's word or the number behind it: both spellings of red are the same
argument.

The one action that is not a `game.*` row is **`call`**, which runs a
top-level function of your own script:

```lua
{ when    = "on_base_captured",
  actions = { { "call", "base_taken", { field = "new" }, { field = "n" } } } },
```

Its first argument is the function's name and the rest are passed to it. The
function reaches `game.*` exactly as the rest of your script does, and an
error in it counts against the scenario like any other. Nothing checks what
arguments it takes — they are yours.

**Your own handler runs first.** Where a scenario declares both a handler and
a trigger on the same hook, the handler is called before the triggers, and
returning `false` from it stops them for that event. That is the only meaning
a hook's return value has. A handler written as a field of the `scenario`
table — `scenario = { on_player_join = function(p) … end }` — is picked up
the same way a global of that name is, and where both are written the global
is the one that runs.

**What is refused, and what is merely skipped.** Everything a table can be
held to is checked before a round starts — by `-validate`, and by the map
editor as you type — and reported against the trigger's position, so
`triggers[3].actions[0]` names the row you are looking at.

Refused for what a row names: a hook that names nothing or names a policy, a
test that names no field, an action that names no op — a name that is there
but empty counts as none — a field the hook has not got, an operator that is
none of the seven, an operator the field cannot answer, an argument count the
op does not take, and a `call` that names no function at all or reads the
name off the payload, which would let whatever the hook was handed pick which
of your functions runs.

Refused for what a value in it holds: a tag nothing on the map carries, a
literal of the wrong kind for the argument it sits in — a number where text
is wanted — a `{ field = }` naming a set, which is neither the one value a
test compares against nor the one an argument passes, and a `{ field = }`
naming a team where the op reads that argument as a seat, which would reach
the seat numbered like that team rather than the player meant.

At run time the router is silent instead: a row it cannot make sense of does
not hold, and an action it cannot make does not run. An action is not made in
part: an argument that reads nothing takes the whole action with it rather
than reaching the op as a nil, and `{ field = "killer_team" }` on a kill
nobody did is the case to keep in mind, since an owner who is nobody has no
team. Raising there would spend one of the scenario's errors on a fault
nobody can fix mid-round, which is why the check before the round is the one
that talks.

A literal of the wrong kind is the fault that does not go quietly, and it is
what that check earns its keep on. Reaching a round through a table nothing
checked, it raises inside the op that reads it, and the raise takes the rest
of that trigger's actions and every trigger after it on that hook for that
event. The server counts one error against the script, and a script that
keeps failing is switched off for the rest of the round.

Two things only the round finds out, so keep them in mind. **A `call` naming a
function your script never defined is skipped**, not reported — the check has
no way to know what the chunk will define, since a script may define a
function under a condition. And a region a test names may be one
`game.define_region` makes during the round, so an unknown region name is not
refused either.

**Limits.** 64 triggers in a scenario, 4 tests and 4 actions on one trigger, 6
arguments on one action. Going past any of them drops what is past it and says
so, the way a table with too many regions does. One argument of an action may
be a line of text up to 128 bytes; every other string is 31. A string past
either length is cut to fit and says so as well, under the slot it was
written in.

A key inside a trigger that is none of the three is not kept. The rest of a
manifest keeps what it did not read, but a trigger is written back out of
what was read, so anything else you put in one is gone after a trip through
the editor or a pack.

### Writing triggers in the map editor

The map editor's scenario panel has a **Triggers** view that writes this table
for you: the hook off a list, the field and operator off what the hook offers,
and the value drawn as whatever the field holds — a tag picker, a region
picker, a team, or the map's own pillboxes and bases by the numbers the editor
shows them under.

The lists are built from the catalogue, so the names and the operators are
right by construction: a field combo offers that hook's own fields, an
operator combo offers the ones that field can answer, and an op combo offers
the ops an action can use rather than every row of the `game` table. What it
cannot settle is what you put in the values — a tag nothing carries, a
literal of the wrong kind for the argument it sits in — and those are
reported in the panel's own issues list as you type, under the same
`triggers[3].actions[0]` key `-validate` uses. A number of the right kind but
outside the op's own range is not among them: a range is the op's business,
and is answered when the action runs.

It also converts entity numbers, so the pillbox it lists as **pill 0** is
written out as `[1]` — see [`scenario.tags`](#scenariotags) for why that is
the one thing worth checking on a table you wrote by hand.

---

## Test hooks

One call, and it is here because a test needs a server path that a round
cannot be steered into.

| Call | What it does |
|---|---|
| `game.shell_expired(p, x, y [, fire_tick])` | Posts one of seat `p`'s shells as having run its full range and died over square `(x, y)` with nothing hit. `fire_tick` is the tick the shell LEFT THE GUN, which is the tick every timing rule in the three-shot detector reads; left out, the shell counts as fired now. Nothing else about the shell happens: no explosion, no sound, no shell. |

**Three shots = go there.** Three full-range shells from one player that die
on the SAME open square are an order to that player's bots: one of them goes
there and holds, for the same sixty seconds every other order runs for. Open
means grass, road, swamp, crater, rubble, river, shallow water or deep sea, a
mined square counting as whatever is under the mine, and never forest, a
wall, a building, a pillbox or a base.

The three have to be FIRED inside two seconds of each other, and the burst
has to stand alone: nothing of that player's fired in the second before the
first of them, and nothing fired in the second after the third. The order is
therefore not sent when the third shell lands. It is armed, and the server
sends it one second after the third shot was fired; a shell fired inside that
second takes it away again.

Which bots hear it is the SHOOTER's own screen: the line carries the
shooter's square as well as the target's, and only a bot within fourteen
squares of the shooter, measured the way a screen is, bids for the job.

A script cannot make a seat pull a trigger, and three full-range shells
landing on a chosen square is not something a round arrives at by accident,
so `shell_expired` is how a test gives that order. A real shell is about half
a second in the air, so a test that cares about the quiet seconds says when
each shell was fired:

```lua
game.shell_expired(SHOOTER, 40, 40, t)
game.shell_expired(SHOOTER, 40, 40, t + 40)
game.shell_expired(SHOOTER, 40, 40, t + 80)   -- the order goes out at t + 180
```

The op is refused with `SCN_OP_WRONG_STATE` outside a running round, with
`SCN_OP_NO_SUCH_PLAYER` for a seat nobody is in, and with `SCN_OP_RANGE` for
a negative `fire_tick`.

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
| `SCN_OP_BAD_SQUARE` | `x` or `y` off the map, or in the twenty-square sea frame a write may not touch (see [Changing the world](#changing-the-world)). |
| `SCN_OP_BAD_TERRAIN` | The square cannot hold this: a base on deep sea, a mine on a building. |
| `SCN_OP_RANGE` | A number outside what the field or the rule allows. |
| `SCN_OP_PAIR` | A rule value that breaks an invariant it shares with another rule. |
| `SCN_OP_CARRIED` | The pillbox is in a tank; drop it first. |
| `SCN_OP_FULL` | 16 live entities, 16 seats, or a queue with no room. |
| `SCN_OP_ALREADY` | An add of something already there, a give of a carried pillbox, a spawn into a seat already on the field. |
| `SCN_OP_TOO_BIG` | A list or a line past its buffer. |
| `SCN_OP_RATE` | A budget for the tick is spent, or a second `fill_rect` was asked for while one is still landing. |
| `SCN_OP_NOT_FOUND` | A brain that does not resolve: a name this server does not have, a path written where a name belongs, or a `package:NAME`. |
| `SCN_OP_NO_STOCK` | A builder order the tank cannot pay for. |
| `SCN_OP_BAD_CALL` | The call itself is malformed. |

---

## Limits

| | |
|---|---|
| Script file | 1 MiB |
| Rules in the `scenario` table | 256 |
| Tags per entity | 4, each 31 bytes |
| Regions | 64, declared and defined together; names 31 bytes |
| Timers waiting at once | 64 |
| A line of text | 128 bytes |
| Primitives in one panel list | 128 |
| Bytes in one panel list | 1017 |
| A panel text primitive | 48 bytes |
| A score label | 15 bytes |
| A bot's `init` table | 16 pairs |
| A hint table | 16 pairs; a name 23 bytes, a value 63 |
| Events queued for one frame | 256 |
| Roster changes outstanding at once | 32 |
| Tiles a fill may change in one tick | 256 |
| Errors in a row before the scenario is switched off | 20 |
| `scenario.name` | 63 bytes |
| `scenario.description` | 255 bytes |
| `scenario.game` | 23 bytes |
| A team's `brain` name | 255 bytes |

Going past one of the counts is reported and refused. Spawns and removals
share the one roster queue and the sim drains one a tick, so a script that
asks for ten bots gets them over ten ticks; the one past the last is refused
rather than displacing something already accepted. A rectangle bigger than a
tick's tile budget applies what the budget allows and carries the rest on
later ticks, one budget each — a whole-map fill takes 256 of them.

The lengths are the exception, and they are cut rather than refused. A tag or
a region name longer than 31 bytes is cut to 31 when it comes from the
`scenario` table, where `define_region` refuses one instead. The four string
fields above are cut the same way and just as quietly, so a description
written as prose arrives at the lobby ending mid-word rather than not at all.

---

## Checking a scenario before you run it

```
WinBoloDS -validate "maps/Wave Defense.map"
```

reads the script beside the map in a Lua state with a `game` table that
answers nothing, runs the chunk's top level, and checks what the `scenario`
table says against the map, the lobby and the rule catalogue. Each problem is
printed as

```
maps/Wave Defense.scenario.lua:14: rules.tank_reload_ticks: tank_reload_ticks is 999, outside 0..255
```

on standard error, and the command exits 0 for a map that is playable and 1
for one that is not. Problems the parse itself finds — a rule name that
spells nothing, a tag past what the map holds — are also written to standard
output as the server would log them, so a run that captures one stream sees
half the report. The map has to load before the script is looked at.

The brains a scenario's teams name are checked against the ones this server
has, so `-validate` on the server you are about to run is what tells you a map
wants `GoalHunter_1.7` and this machine has not got it. That is a problem
rather than a refusal: the map still plays, with those seats on the server's
own brain.

No round is run and no bot loads, but the file's top level does run, the same
way it would at a round start — so a file you would not run is a file you
should not check. The stub `game` table answers `nil` to every call, so a
script that reads the world at its top level, which it should not, raises
here and not on the server.

The line number is found by looking for the key's own name in the source, so
two things can lead it astray: a name written earlier in a comment or a string
takes the line, and a key two tables both carry — a second team's `bots` —
takes the first one's line.

---

## What is not here yet

Named so you do not spend an afternoon looking for them:

- **`scenario.lobby.extra_teams`.** It is read, and it is sent to every
  client on the lobby settings event, but nothing acts on it: whether a host
  may put a seat on a team nobody is on is still decided by the
  `allow_extra_teams()` policy alone.
- **Packaged brains.** `package:NAME` is refused wherever a brain is named.
- **A trigger's own `scripted` flag.** An event hook is handed one, but it is
  not a field a `where` row can test: the fields come from the hook's declared
  parameters and the flag is added after them. A trigger fires whether the
  event was the script's doing or the round's. Test it in a handler of your
  own if it matters.
