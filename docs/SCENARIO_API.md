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
- [Packaging](#packaging)
- [Mods](#mods)
- [The script list](#the-script-list)
- [Uploads](#uploads)
- [Sharing: Save a copy](#sharing-save-a-copy)
- [The Steam Workshop](#the-steam-workshop)
- [Recordings and the game finder](#recordings-and-the-game-finder)
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
- [Settings](#settings)
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

A script reaches a round in one of five ways:

- **Beside the map.** `Wave Defense.map` is played with
  `Wave Defense.scenario.lua` beside it. This is the file you edit.
- **Inside the map.** A `.map` can carry its scenario packed into the file
  itself, so the scenario goes wherever the map goes. See
  [Packaging](#packaging).
- **As a `.scenario` file** in one of the server's mod directories, which
  plays over whatever map is committed. See [Mods](#mods).
- **On the lobby's list.** The host picks a scenario and up to nine mods in
  the lobby's chooser, and the chooser sends that list to the server. See
  [The script list](#the-script-list).
- **From the map editor**, which writes the script beside the map, packs it
  into the map, or saves it as a `.scenario` file. See
  [Packaging](#packaging).

A map with no script beside it and none inside it is an ordinary map. Copying
a map to a new name and writing a script for the copy is still how you attach
a scenario without changing how the original map plays.

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

**A scenario and its mods share the state but not the libraries.** A round
may run a scenario with mods behind it, every script on the list in the one
state, each with its own globals. In the sandbox each script also has its own
copies of the standard libraries and of `game`, so changing them — setting
`string.find` to `nil`, or putting a row of your own on `game` — affects only
that script. Method calls on strings, such as `s:find(p)`, still share the one
string library. Under `-allow-unsafe-scripts` the scripts on one list share
one set of libraries and one `game` table.

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
Control characters in printed text, newlines included, come out as spaces, so
each `print` is one line. Tabs are kept.

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

**All of one tick's calls share two million instructions** — every hook and
timer the tick runs, and any policy asked from inside one of those calls (a
hook's op that asks a policy), added together. A policy the engine asks while
it steps the world, such as `damage_scale` on a hit, has only its own call's
budget and is not part of the total. The call that passes the
total fails with an error, and the rest of that tick's calls are skipped: a
timer skipped this way runs next tick, but an event or a region change skipped
this way is not delivered again. `on_end` is not part of the total and always
runs. The tick costs your script one error however many calls it skipped, and
`pcall` cannot catch it any more than it can catch the per-call budget, so a
script that spends the whole total every tick is switched off after twenty
ticks.

A million is a great deal — an `on_tick` that reads a few dozen tanks and
decides something spends a few thousand. If you are near it, you are looping
over the map rather than over what changed.

The count cannot see inside the library's C functions, so seven of them are
bounded by length instead: `string.rep`, `string.format` and `table.concat`
refuse to build a string over 64 KiB, and `string.find`, `string.match`,
`string.gmatch` and `string.gsub` refuse to search one. That refusal is an
ordinary error, unlike the budget's — a `pcall` catches it and your script
carries on. A longer string built with `..` is not refused here; the memory
cap is what counts that.

`string.find`, `string.match`, `string.gmatch` and `string.gsub` follow Lua
5.4's pattern rules on every host, LuaJIT included: `%g`, `gmatch`'s third
argument, 5.4's handling of empty matches, and the error for a `%` in a `gsub`
replacement that is not `%0`–`%9` or `%%` are the same everywhere. A pattern
that runs too long uses up the call's instruction budget like any loop and is
stopped the same way, however short the string it runs over — and `pcall`
cannot keep that error either.

**Your script runs interpreted** on every host: LuaJIT's compiler is never
switched on for scenario scripts, and that is what lets the budget hold, since
compiled code would slip past the count. On a debug build the same code ran
about nine times faster compiled, and counting adds about two thirds on top of
the interpreter. A typical `on_tick` still costs well under a microsecond, but
a script that spends its whole two-million-instruction tick total takes a few
milliseconds of a 20 ms frame on LuaJIT, and over half of one on the Lua 5.4
build. Under `-allow-unsafe-scripts` the compiler is on and none of this
applies.

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

**Scripts that come from players.** A player can send a server a mod or a
scenario from the lobby, and a `.map` a player uploads can have a scenario
packed into it. A server that keeps an uploaded map on disk keeps the bytes it
was sent whole, so committing that map later runs the script that came with
it. Either kind runs under exactly the same sandbox as a script on the
operator's own disk — the same library, the same memory cap, the same
instruction budget — and, as above, at the server's own privilege. What a
server does with them is its script upload policy, a narrower switch than
turning scripts off altogether:

- `-scriptuploads off|allow|persist` on the dedicated server. One dash.
- `--scriptuploads off|allow|persist` on the headless runner. Two.
- **Scripts from Players**, the desktop client's hosting preference, under the
  one above: Off, Allow this session or Allow and keep. Greyed while scripts
  are off altogether.

All three default to allow, which the desktop calls Allow this session. Off
refuses every script a player sends, and
strips the script from an uploaded map: a map whose file sits in the server's
map uploads directory attaches neither a packed scenario nor a loose script
beside it, the console names the upload that was turned down, and the map
chooser does not tag it as scripted. Every other map is unaffected. On the
desktop that half takes effect the moment the preference changes. Allow and
persist take the scripts players send, for the session or for good (see
[Uploads](#uploads)), and run an uploaded map's own scenario; one console line
names each uploaded map whose packed scenario is what runs, so the operator
can see when a round is being played by a script a player sent.

**Running scripts with nothing held back.** For an operator whose own content
needs more than the sandbox allows, there is a switch that lifts it, named
after `-allow-unsafe-brains`. Each host takes both dash forms:

- `-allow-unsafe-scripts` on the dedicated server.
- `--allow-unsafe-scripts` on the headless runner.
- `--allow-unsafe-scripts` on the desktop client's command line. There is no
  preference for it.

With it on, every scenario state the process boots — a script beside a map,
one packed into an uploaded map, a mod, and the check `-validate` makes — is
plain Lua:

- the whole standard library, `io`, `os`, `package` and `require`, `debug`,
  `load`, `dofile` and `string.dump` included, with `loadstring`, `ffi`,
  `jit` and `bit` on LuaJIT, where the compiler is on as well;
- no memory cap, no instruction budget per call and none per tick;
- the VM's own `string` functions, patterns, `pcall`, `xpcall` and coroutines,
  with none of the length caps above;
- precompiled chunks accepted, as well as text;
- `math.randomseed` left in place. The host still seeds `math.random` at
  each boot;
- the scripts on one list share the standard libraries and `game` rather
  than each having copies of its own, so a change one script makes to them
  reaches every other script on the list.

`print` still goes to the server console, where the operator and the desktop
client look, with no limit on how many lines.

It applies to uploaded maps' scripts and to the scripts players send as well.
With the script upload policy set to off, an uploaded map's script is still
refused outright, and so is every script a player sends. The host prints a
note at startup saying the switch is on. It exists for content the operator
trusts as their own; a server open to uploads from strangers should not run
this way.

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

## Packaging

A scenario is kept in one of three shapes, and a server reads all three:

- **A loose script**, plain Lua: `X.scenario.lua` beside `X.map`, or a `.lua`
  file in a mod directory.
- **A chunk in the map.** The scenario's manifest and its script, framed as a
  WBSC container and appended to the `.map` file after the map itself. The
  scenario then goes wherever the file is copied, uploaded or published. This
  is the shape for a scenario with `bound` true.
- **A `.scenario` file.** The same container as a file of its own, which is
  how a mod, or a scenario that plays over any map, is kept in a mod
  directory.

The manifest is the `scenario` table written down as data, with the keys
[The `scenario` table](#the-scenario-table) lists.

**The map editor is the ordinary way to pack.** Its scenario panel writes all
three:

- **Save** writes the loose script beside the map, and checks it.
- **Pack into Map** writes the chunk on to the map file, replacing any chunk
  already there. A manifest with `bound` false is refused: a chunk on a map is
  that map's scenario, and one that plays over any map is saved as a
  `.scenario` instead.
- **Save as Mod…** writes a `.scenario` file with `bound` false and no tags or
  regions, whatever the forms hold, because those belong to a map a mod has
  never seen.

Both packs check the script first, and a script with problems is not written;
the panel's Script view lists them. Saving a map that opened with a chunk puts
that chunk back on the file, so editing a packed map's terrain does not lose
its scenario. Pack into Map is what writes the forms' changes.

**`-pack` does the same from the command line.**

```
WinBoloDS -pack "maps/Wave Defense.map"
```

writes the script beside the map into the map file and exits without starting
a server. The manifest comes from the script's own `scenario` table, and a
chunk already on the map is replaced. A script with problems is not packed;
run `-validate` on the map to see them. The command exits 0 when the map was
packed and 1 when it was not.

**Write it loose, ship it packed.** A loose script beside a map wins over the
chunk inside it. That is the loop a scenario is written in: keep
`X.scenario.lua` beside the packed `X.map`, edit it, `reload` or press
**Reload script**, and play, with no packing between rounds. While both are
there the console says once that the loose script is what runs and the packed
one is not. Delete the loose script and the next reload hands the map back to
its own chunk. When you are done, pack it and take the loose script away: a
server that has both runs the loose one.

---

## Mods

A mod is a file whose table says `kind = "mod"`: it changes how the game
plays and leaves winning and losing alone. The `kind` row of
[The `scenario` table](#the-scenario-table) has the whole rule. In short, a
mod may not:

- name a `game` type, or declare a trigger action that calls `end_round`,
  `set_game_time`, `add_game_time` or `score`. The file is refused as it
  loads;
- call any of those four from its code. Each raises when a mod calls it;
- decide the round through `allow_base_win`. The server does not read a mod's.

A mod further down the list than the round's scenario, or than the first
script of a round with no scenario, may not declare a `lobby` block or
`fill_to_caps` either, because those belong to the script at the head of the
round. A list that breaks this plays no script, and the console names the file
and the key.

**Where mods are found.** A server offers every `.scenario` file and every
loose `.lua` in these directories, merged into one list, highest precedence
first:

1. The directory this host was given: `-moddir` on the dedicated server
   (default `data/scenarios`), or the desktop client's **Mod Directory**
   hosting preference (default `<prefpath>Mods`).
2. `<prefpath>Mods`, the player's own.
3. `<prefpath>Workshop`, where subscribed Workshop items are copied. See
   [The Steam Workshop](#the-steam-workshop).
4. `data/mods` beside the executable: the mods that ship with the game.
5. The uploads directory, where scripts players send land. See
   [Uploads](#uploads).

A file name two directories both hold is the higher directory's. The lower
copy is not listed and never loads. A directory named twice is read once, and
one that is not there is not an error: it means that directory offers nothing.

`<prefpath>` is the game's own writable folder, the one it reads `Brains` from
as well: `~/Library/Application Support/WinBolo/WinBolo/` on macOS, and SDL's
preferences path on other systems. The dedicated server reads the same
folders a desktop host does.

- `-moddir <Dir>` on the dedicated server. One dash. `-scenariodir` is its old
  name and is still taken; `-moddir` wins when both are given.
- **Mod Directory**, the desktop client's hosting preference. It is read when
  a game is hosted, so a change reaches the next hosted game. The client makes
  the folder if it is not there.

**The shipped mods** live in `data/mods`, found from where the executable is
rather than from where the server was started, so every host offers them
whatever `-moddir` says.

---

## The script list

A round runs an ordered list of scripts: at most one scenario, the file that
decides the round, and mods behind it, ten scripts in all. A map that brings a
scenario of its own counts that scenario as one of the ten.

**The chooser.** The host opens it with the **Details** button on the lobby's
Mods row. The left column, **This server offers**, lists every script the
server's directories hold, with a filter box and a kind filter. The right
column, **This round runs**, is the list, top first, with **Load earlier** and
**Load later** to move a row and a button to drop one. The arrow on a left row
adds it: a mod goes on the end, and a scenario takes the place of the scenario
already on the list. **OK** sends the whole list to the server in one command.
Any script's name opens its details: its description, its rules and its
settings.

The host, an admin, or anyone while Open Host is on may change the list. For
everyone else, spectators included, the chooser is read-only.

**Order is precedence.** The top of the list wins. A rule two scripts both set
is the higher script's, and the details dialog marks the lower one's row
*Overridden by …*. A region two scripts both name is kept twice, once for
each, and each script's own lookups find its own. Tags are the union of every
script's, and every script's triggers are kept, in list order. Hooks run down
the list, top first, except `on_tick`, which runs up it: the bottom script's
first and the top script's last. A write such as `game.set_modifiers` or
`game.set_rule` replaces what was there, so in a frame the last write stands,
and the top script's `on_tick` write is the one that stands. Note: when a
tick spends the shared two-million-instruction budget (see above), the rest
of that tick's calls are dropped, and for `on_tick` those are the scripts
nearer the top, which run last. So in that tick the top script's `on_tick`
is the one most likely not to run, and a lower script's write stands. The round needs
bots when any script on the list says `needs_bots`, whatever its place (see
`needs_bots` in [The `scenario` table](#the-scenario-table)).

**A picked scenario replaces the map's own.** Two scripts that can each end the
round cannot both play, so a scenario the host picks plays in place of the
scenario the map brought. A mod picked on a scenario map plays beside the
map's scenario. The map's own row can sit anywhere on the list, which is how a
host puts a mod's rules ahead of the map's.

**What the server refuses.** A list with two scenarios, the same file twice, a
name none of its directories holds, or another map's bound scenario is refused
whole, and the list that was playing goes on playing. So is a list sent while a
round is running, a second list inside a second of the last, and any list but
an empty one while the lobby is ranked. A script on an accepted list that will
not load plays no script at all rather than the rest of the list without it,
and the console says which file.

**Mods/Scenario Enabled.** The host's Mods row in the lobby has this checkbox,
on by default. Off, the round composes none of the host's picks: no mod and no
picked scenario. The map's own scenario still plays. The list is kept as the
host wrote it, so turning the box back on brings the same scripts back in the
same order, and the lobby marks each pick that is switched off. An operator
locks the box with `-lock mods` on the dedicated server.

---

## Uploads

A player with a mod or a scenario on their own machine can send it to the
server whose lobby they are in. The server's script upload policy, set by the
switches under [Where a scenario lives](#where-a-scenario-lives), decides what
happens to it:

| Policy | A script a player sends | An uploaded map's own script |
|---|---|---|
| off | Refused before any bytes are sent. | Not run. |
| allow (the default) | Kept for the session, in the Session directory. | Run. |
| persist | Kept for good, in the Scripts directory. | Run. |

It is a policy of its own, apart from the one for map uploads.

**Where they land.**

- On the dedicated server, persist writes to `<map root>/Uploads/Scripts`, or
  to the directory `-scriptuploaddir` names, and allow writes to
  `<map root>/Uploads/Session-<port>`. The port is in the name so that two
  servers sharing a map directory keep separate sessions.
- On a desktop host, persist writes to the **Script Upload Directory**
  preference, which defaults to `<prefpath>uploads/Scripts`, and allow writes
  to `<prefpath>uploads/Session`.

The directory in force is the last and lowest entry of the mod list (see
[Mods](#mods)), so an uploaded file is offered beside the server's own and
marked *uploaded*.

**The Session directory is emptied** when the server starts, when it shuts
down, and when the last player leaves and the lobby resets to its defaults:
the group that brought the files is the session. A desktop host going back to
the menu is a shutdown. At the reset, every pick on the list that named a file
in the directory is dropped first, and the console says how many. Only a host
that takes players from other machines empties it; single player never does.

**Name rules.** A script's file name ends in `.scenario` or `.lua` and is at
most 127 bytes. It may not start with a dot, hold a path separator, a colon or
a control character, have a dot or a space just before the extension, or be a
Windows reserved name such as `CON` or `LPT1`. The file may be up to 4 MiB.

**What the server checks.** At the start of an upload, in order: that the
sender has not asked too soon after their last request, that they may change
the round (the host, an admin, or anyone while Open Host is on), that the
policy is not off, that no other player's upload is in flight, the size and
the name, that the name is not taken, and under persist, the caps. Once the
bytes have arrived the server reads the file the way the mod list would: a
`.scenario`'s manifest must parse and a `.lua` must load and declare a
`scenario` table. A file that asks for a newer `api`, declares a `kind` this
server does not know, or is bound to a map is refused as well; a bound
scenario arrives as its map instead. The player is told why, and a script
with a syntax error says the line.

**A name already taken** in a higher directory — `-moddir`, the Mods or
Workshop folder, or the shipped mods — is refused at the start with reject
code 8, `LOBBY_REJECT_NAME_TAKEN`, because a pick of that name would load the
higher file and never the upload. The match ignores case. Sending a file the
uploads directory already holds is not refused: the new one replaces it.

**The caps** hold under persist only, since the Session directory is emptied
instead. Only the `.scenario` and `.lua` files directly in the Scripts
directory count, and a file an upload replaces counts by its change in size.
An upload past either cap is refused at the start, and again when its bytes
arrive if another upload has landed since. A value outside the range is
clamped, with a warning.

- `-scriptuploads off|allow|persist` on the dedicated server, and
  `--scriptuploads` on the headless runner, which takes none of the switches
  below.
- `-scriptuploaddir <Dir>`: where persist writes. Default
  `<map root>/Uploads/Scripts`.
- `-scriptuploadmaxfiles <N>`: 1 to 255 files, default 32.
- `-scriptuploadmaxstorage <MB>`: 1 to 4095 MB, default 64.
- On the desktop, **Script Upload Directory**, **Max Files** and **Max Storage
  (MB)** under **Scripts from Players**, drawn when Allow and keep is chosen,
  with the same ranges and defaults. Its preferences file keeps them under
  `HOSTING` as `Script Upload Policy` (`Off`, `Allow` or `Persist`),
  `Script Upload Dir`, `Script Upload Max Files` and
  `Script Upload Max Storage`.

**Map uploads** have their own policy and caps: `-uploadpolicy off|allow|persist`,
`-uploaddir <Dir>` for where a persisted map is written (default
`<map root>/Uploads`), `-uploadmaxfiles <N>` 1 to 255 (default 64) and
`-uploadmaxstorage <MB>` 1 to 4095 (default 8). A persisted map upload is a
file in that directory, which is what the script upload policy's off looks
for. A map upload under allow plays from memory and has no file, so a scenario
packed into it does not run under any script policy.

**In the chooser.** On a server in another process, the left column also lists
what this computer holds in `<prefpath>Mods` and `<prefpath>Workshop`. A row
is matched to the server's by Workshop item id when both carry one, and by
file name, ignoring case, otherwise. Each row says where it is:

- *server*: only the server has it;
- a tick and *on this computer*: both have it;
- *on this computer* and a **Send to server** button in place of the add
  arrow: only you have it.

A row the server holds because a player sent it also says *uploaded*.

Send draws a progress bar under the row while it runs. When the file lands,
the chooser asks the server for its list again and the row becomes one both
hold; for a player who may change the round, the file is also added to the
list they are building, and the round gets it when they press OK. A refusal
shows its reason under the row for five seconds.

Send is drawn for every player, and greyed with the reason on hover for a
player who may not change the round, when the server's policy is off, and
while any upload is in flight. In single player or on your own desktop host
every script on this computer is already one the server reads, so no row is
on this computer alone and there is nothing to send.

---

## Sharing: Save a copy

A row only the server has carries a **Save a copy** button, a down arrow whose
tooltip reads *Save a copy to your Mods folder*. It asks the server for the
file and writes it to `<prefpath>Mods` under the same name, and the row then
reads as one both hold. The round never needs the copy, since every script
runs on the server; it is for hosting the script yourself later. A loose
`.lua` arrives as its source and lands as a `.lua`.

The button is offered only against a server in another process. It is greyed
for a spectator, when the server does not share, and while another copy is on
its way. A file your Mods or Workshop folder already holds under that name is
not asked for. A file the server no longer has, one too large to send, and a
request too soon after the last are refused, and each shows its reason under
the row.

Sharing is on by default:

- `-noscriptsharing` on the dedicated server turns it off. One dash.
- **Let players copy this server's scripts**, the desktop client's hosting
  preference, kept as `Share Scripts` (`Yes` or `No`). Greyed with the rest
  while scripts are off.

**A bound scenario is not served this way.** The map's own scenario lives in
the map file, and the map a joining player downloads is the map alone,
without its chunk. See [What is not here yet](#what-is-not-here-yet).

**A Workshop row** also carries **Open in Workshop**, which opens the item's
page in Steam when Steam is running. Subscribing there is the better copy,
because Steam keeps it up to date.

---

## The Steam Workshop

Mods and scenarios are published to and subscribed from the Steam Workshop,
the way skins are. All of it needs the Steam client running; without Steam
none of it is drawn.

**Subscribing.** Subscribe on the item's Workshop page. The game copies each
installed item's one `.map`, `.scenario` or `.lua` file into
`<prefpath>Workshop` when it starts, and again whenever Steam says an item has
finished installing, so a subscription reaches the chooser without a restart.
An item that holds a skin is left to the skin picker, and one that holds none
of the three files, or more than one, is skipped with a line in the log. Two
items with the same file name keep the lower item id's.
`<prefpath>Workshop/workshop.json` records which file came from which item,
with the source's size and modify time, so each pass copies only what changed
and removes only the files it put there, once you unsubscribe.

**Publishing** is on the **Workshop** tab of the Settings dialog, which is
there only while Steam is running. Two buttons at the top switch between its
two views, **Subscribed** and **Publish**, beside **Refresh**, which runs the
copy again, and **Browse Workshop**.

- *Subscribed* lists what you are subscribed to: each item's name, whether it
  is a mod, a scenario, a map or a skin, its state (*Installed*, downloading,
  or *Not usable* for an item the copy could not use), and
  **Open in Workshop**. Skins are listed here and still read in place by the
  skin picker.
- *Publish* lists every script in your own `<prefpath>Mods` folder and every
  map under `data/maps` that carries a scenario, each with **Publish**, or
  **Update** when the file already names an item you published. Either opens
  the publish window the skin picker uses.

A loose `.lua` is packed before it is published: into `<name>.scenario` beside
it in the Mods folder, with the `.lua` moved into `Mods/Sources`, where the
mod list does not look. The package is what is sent. A loose script that is
bound to a map is not offered, because it ships inside its map.

**Tags.** An item is tagged `Mod` or `Scenario` from the file's `kind`, as a
skin is tagged `Skin`. The copy does not read the tag: what an item is comes
from the file it holds, so a plain map somebody published lands in
`<prefpath>Workshop` as well.

**Provenance.** When a publish succeeds, the game writes the item's id and
your SteamID64 into the file that was sent, as the manifest's `workshop_id`
and `workshop_author` (see [The `scenario` table](#the-scenario-table)): into
a `.scenario`'s manifest, or into the chunk on a `.map`. That is what makes
the next press **Update** rather than a second item, and what lets the lobby
match a server's copy of an item to yours whatever the two files are called.
The map editor shows both, read-only.

**The Workshop folder in the map chooser.** A `.map` copied into
`<prefpath>Workshop` is offered in the desktop client's map chooser as a
folder named **Workshop** at the top of the map list, the way `Uploads` is. A
real folder called `Workshop` in the map directory is shown in its place.

**In the lobby**, a row whose file names a Workshop item wears a *Workshop*
chip after its kind and, with Steam running, an **Open in Workshop** button.
The map panel's script lines wear the chip too.

**A dedicated server has no Steam.** It publishes nothing and copies nothing,
and its map list has no Workshop folder. It reads the same mod directories a
desktop host does, `<prefpath>Workshop` among them, so an operator who wants a
Workshop mod on a server copies the file into `-moddir` or into one of those
folders.

---

## Recordings and the game finder

**A recording keeps the round's scripts.** The `.wbv` of a round that ran
scripts holds a `scripts.json` member saying what they were; a plain round's
has none. [docs/replay-format.md](replay-format.md#scriptsjson) describes it.

**The finder names them.** A server registered with WinBolo.net sends three
keys about its scripts when it registers and with every lobby update:

- `scenario`: the name of the scenario that decides the round;
- `scenario_max_players`: that scenario's cap on human players, 0 for none;
- `mods`: the names of the mods that run, `[]` when none do.

A round no scenario decides, a plain one or one only mods change, sends
neither of the first two. The game finder puts the scenario's name after the
map on a server's row, then the one mod's name or *+N mods*, and counts humans
against the scenario's cap when it has one. The details pane names the
scenario, with its description once the server itself has answered, and lists
the mods. That answer is the info reply, whose bytes are in
[docs/info_packet_wire.md](info_packet_wire.md#script-bytes-after-the-packet).

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

**Some settings are refused while a scenario is attached.** Any game-type
change and ranked on, and — only when a script on the list says
`needs_bots = true` — the no-bots AI policy. They are refused the same way
and the host is shown the same line — *That setting is fixed by the map's
scenario* — rather than the request failing silently. A mod leaves the game
type to the host.

**Committing a scripted map moves settings out of the way**, because the
ones above cannot stand beside a scenario: ranked goes off, and — only when a
script on the list says `needs_bots = true` — an AI policy set to no bots
moves up to one that runs them. A list that does not say it leaves the AI
policy to the host, so a lobby with no bots stays with no bots. Committing a
plain map afterwards gives them back — the game type, the ranked flag, and
the AI policy the lobby was on before a `needs_bots` script moved it.

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
  author       = "Jo Bloggs",
  updated      = "2026-10-03T20:47Z",
  kind         = "scenario",
  game         = "open",
  bound        = true,
  fill_to_caps = false,
  needs_bots   = false,
  lobby        = { ... },
  rules        = { ... },
  tags         = { ... },
  regions      = { ... },
  triggers     = { ... },
  callbacks    = { ... },
}
```

| Field | Type | Meaning |
|---|---|---|
| `name` | string | What the scenario is called. |
| `description` | string | One or two sentences for a host reading a list. |
| `api` | number | The API version you wrote against. Defaults to 1. A server older than the version you name refuses the scenario rather than running it half-understood. |
| `kind` | string | What the file is allowed to decide: `"scenario"` (the default) or `"mod"`. A scenario decides the win condition — it can end the round and say who won. A mod changes how the game plays and leaves winning and losing where the map and the server's rules left them, so `game.end_round`, `game.set_game_time`, `game.add_game_time` and `game.score` all raise when a mod calls them, a mod that names `game` or declares a trigger action calling one of those four is refused as the file loads, and a mod's `allow_base_win` is not read. Left out, the file is a scenario, which is what every file written before this key existed is. A word that is neither is reported and read as a scenario. |
| `game` | string | The game type the scenario asks for: `"open"`, `"tournament"` or `"strict"`. The round plays under it — the lobby and both game finders read the type as Scripted, and every part of the engine that picks behaviour from the game type resolves that to the word named here. Left out, the round plays strict tournament. A word that is none of the three also plays strict tournament, and `-validate` reports it by name. Write `game = "open"` for an open round: a scenario that says nothing is not read as asking for one. |
| `bound` | boolean | True (the default) when the scenario is tied to its map. A scenario that names tags or regions is tied to its map by definition, because tags and regions are the map's own squares and entities. A server can also offer scenarios of its own, which play over whichever map a host has committed; a scenario with `bound` true is not one of those and a host picking it is refused, because over another map its tags, its regions and its entity indices name items that are not there. |
| `triggers` | array | What the scenario does without a line of Lua: hooks to listen on, tests against what each hook is handed, and calls to make when every test holds. A scenario may carry triggers, a script, or both. See [Triggers](#triggers). |
| `fill_to_caps` | boolean | False by default. True starts every pillbox and base on the map at the caps your `rules` table leaves in force rather than at the numbers the map file holds. A map file states a number for each pill's armour and each base's stocks and has no way of stating "full", so a scenario that raises `base_full_armour` or `pill_max_armour` would otherwise open with the map's own smaller numbers and climb to the new ones over the round. Raising only: anything already at or above a cap is left where it is, and anything above one is brought down by the rules themselves. A pill's firing rate is not touched. |
| `needs_bots` | boolean | False by default. True says the script needs the lobby to allow bots: it fields its own, through a `lobby` team with `bots` above 0 or with held seats, through `game.spawn_bot`, or through `game.lobby_add_bot`, and a lobby set to no computer tanks refuses every one of those. With it, attaching the script moves a lobby set to no bots up to one that runs them, the host cannot set no bots while the script is attached, and the lobby goes back to no bots once the last script goes. Without it the script leaves the host's bot setting alone, and a script that only works with the bots a host adds — retuning them with `game.bot_init`, say — does not need it. A mod may say it as well as a scenario, and a list needs bots when any script on it says so. A `lobby` team with `bots = 0` does not say it. A file that fields bots and leaves it out works in a lobby that allows bots and fails in one that does not, so `-validate` reports it and packing or publishing the file is refused: a `lobby` team with `bots` above 0, or a call to `game.spawn_bot` or `game.lobby_add_bot` anywhere in the code (a comment or a string that only names one is not a call). |
| `callbacks` | table | What each of the script's callbacks does, one sentence each for a player, keyed by the callback's name: `callbacks = { on_start = "Lines the teams up.", can_die = "Builders cannot be killed." }`. The lobby's details dialog shows them under the rules table, headed "What this mod implements:" or "What this scenario implements:", as a table of Method (the callback's name), Type and High-level overview (the sentence). Type is Event for a hook whose return the engine ignores, Query for a policy whose answer it uses, and Trigger for a hook only a trigger's `when` defines. A script with no block shows no such section. Optional, and it changes nothing about how the round plays. At most 40 rows, each sentence cut at 159 bytes (at a UTF-8 character boundary), and 2048 bytes for the whole block packed (a type byte and two length bytes per row plus the name and the sentence, and one count byte) — about twenty lines of eighty letters. The load warns, and never refuses the file, for each callback the script defines that the block does not describe, for each name that is no callback the engine calls, and for each name the script never defines; those last two rows are dropped. The warnings go to the server console and `-validate` prints them. The names are the ones in the hook and policy tables below; a trigger's `when` counts as defining its hook. |
| `workshop_id` | string | The Steam Workshop item the file was published as, written as a string of decimal digits (`"3301234567"`), because a Lua number cannot hold every digit of a 64-bit id. The game writes it into the file's manifest when you publish it; it is not meant to be set by hand. A script that declares its own `scenario` table need not repeat it: it is compared with the manifest only when the table states it, and a table that states a different id is refused. Anything that is not a string of digits is reported and read as none. |
| `workshop_author` | string | The SteamID64 of the account that published the file, a string of decimal digits like `workshop_id`, and written by the game at the same time. The same rules apply: not meant to be set by hand, need not be repeated, and refused only when the table states a different one. |
| `author` | string | Who wrote the file, as the lobby and the server log show it: up to 47 bytes of UTF-8. See [`author` and `updated`](#author-and-updated). |
| `updated` | string | When the file's content last changed, in UTC to the minute: `"YYYY-MM-DDTHH:MMZ"`. See [`author` and `updated`](#author-and-updated). |
| `settings` | table | Choices the host makes in the lobby details dialog, one dropdown each, read by the script with `game.setting(id)`. See [`scenario.settings`](#scenariosettings). Optional; a script with none shows no Settings section. |

### `author` and `updated`

Two editions of a mod can share a name. The author and the time the file last
changed are what tell them apart, so the lobby shows both: on each row of the
script chooser as a tooltip, "by Jo Bloggs · updated 2026-10-03 20:47 UTC",
and on the same line under the file name in the details dialog. The server
prints them on the line it writes for each script a round loads:

```
scenario: Mac Bolo Rules by WinBolo, updated 2026-10-03 (MacBoloRules.scenario.lua) loaded
```

`updated` is written in one form only, an ISO 8601 time in UTC to the minute
with a `Z`: `"2026-10-03T20:47Z"`. No seconds, no offset, no other separator.
It is the time the content changed, stated in the file, and not the file's
modified time on disk, which a copy or a download moves. The form is checked
as a real date: `"2026-02-30T10:00Z"` is not one.

`author` is free text, cleaned as it is read: control characters are dropped,
and so are invisible format characters (bidi controls, line and paragraph
separators, zero-width characters), spaces at either end are trimmed, and
the text is cut at 47 bytes on a UTF-8 character boundary. Invalid UTF-8 is
dropped byte by byte. An author that is empty once cleaned reads as unknown.
The lobby draws it as plain text, never as a format string.

Both are optional. A file that leaves one out, or states one that is not a
string, or an `updated` that is not in the form, reads as "unknown" there,
and the file still loads and plays. Each of those is a warning, not a
problem: `-validate` prints it as `file: warning: key: message` and exits 0
all the same, and the round loads without a word. A cleaned author is a
warning too, naming the text that was used.

A package's `manifest.json` carries the same two keys, `"author"` and
`"updated"`, with the same rules. A script that declares its own `scenario`
table need not repeat them; when it does, they must match the manifest's, as
`workshop_id` must.

A server from before these keys sends no identity, and a newer lobby then
shows nothing rather than "unknown". An older lobby ignores what a newer
server sends.

The shipped scripts (`data/mods/*.lua` and `data/maps/*.scenario.lua`) say
`author = "WinBolo"`. `tools/stamp_mod_updated.py` keeps their `updated`
current: run it before committing a change to one, and it stamps every
shipped file that differs from git HEAD with the current UTC time.
`--check` fails a shipped file whose last change did not also change its
`updated` line; see [TOOLS.md](TOOLS.md#toolsstamp_mod_updatedpy).

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
| `brain` | string | The brain this team's bots run, **named**: the directory under the server's `brains/`, such as `GoalHunter`. Empty means the server's own. A name this server does not have leaves the team's seats on the server's own brain, says one line on the console, and is reported by `-validate` before a round is ever started. A value with `/` or `\` in it is a path, not a name, and is refused as such — a scenario shared with a server knows nothing of that server's layout, which is why it names the brain and lets the server find it. `package:NAME`, a brain carried inside the scenario, is still refused with `SCN_OP_NOT_FOUND`. |
| `mode` | string | The brain mode this team's bots play in, by the key the brain's own `modes.txt` lists — `"default"`, say. Left out, the seats keep whatever mode the lobby would have given them. |
| `difficulty` | string | The level inside that mode, by the key the same file lists — `"hard"`. Left out, the seats keep the lobby's level, except that a team naming a `mode` and no `difficulty` lands on that mode's own default level. |
| `init` | table | A flat table of names to strings or numbers, handed to this team's bots when their VM is built. A `spawn_bot` that names one of these seats and carries no `init` of its own gets this one. |

The server's own `-brain` switch is still a path, and deliberately: that is an
operator naming a file on their own machine, where the layout is theirs to know.
A `brain` in a scenario is content that travels with the map to servers that
have never seen it, so it names what it wants and the server resolves the name
against its own `brains/` — the same directories the lobby's bot list is built
from. Ship a scenario that needs `GoalHunter` and every server that has
`GoalHunter` runs it; one that does not gets a reported problem and a
playable round.

**`mode` and `difficulty` are the half the lobby reads.** They are matched
against the brain's `modes.txt` — case does not matter — and written into the
seat's own config, which is three things at once: the difficulty chips on the
seat's row in the players list, the mode tag beside its name, and the `mode=`
/ `difficulty=` tokens the server builds the brain's init string from. A seat
whose team says nothing here carries whatever the lobby gave it, which is the
behaviour every scenario written before these two fields had.

A key neither file lists is a problem `-validate` reports by name. The seating
does not refuse the seat over it: the seat is made and keeps the lobby's own
mode and level, with a line in the server log saying which key was not
recognised. The bot ops below are stricter, because there the refusal costs
nothing: `spawn_bot` and `lobby_add_bot` answer `SCN_OP_NO_SUCH_ITEM` rather
than seating a bot at a level nobody asked for.

**The host's dropdown still works on these seats.** A host may change a
scripted seat's mode and difficulty from the players list for as long as that
lobby lasts, the same way the host may trim the seats the template asked for.
The template's values are re-applied when the lobby is built from the map and
again when a lobby comes back from a round — which is the rule `bots` and
`max_bots` already follow: what a host does inside one lobby stands, and the
template describes the lobby each round opens with.

**Add Bot on one of these teams reads the template too.** A seat the host
adds to a team the template named a `mode` or a `difficulty` on is not a
plain lobby bot: the server resolves it the same way the seating does, so
Survival's horde grows in the mode its template names however the lobby's
own default is set. The level is the first of these that exists:

1. the most recent difficulty a **person** set on a seat of **that team**,
   through the difficulty dropdown, this lobby session;
2. the team's own `difficulty`;
3. the default level of the mode the team named;
4. Hard.

The mode is always the template's — a host who moves one horde seat into
another mode has moved that seat and nothing else, and the next Add Bot on
that team is back in the template's mode. A team the template does not
name, or names neither key on, keeps the ordinary lobby rule, where the last
pair a person chose by hand sets the mode as well as the level.

The memory is per team and lasts one lobby session. It is cleared with every
other lobby-session memory when a round ends and when the last person leaves,
so the lobby a map opens with is the template's and not last round's. The
resolution is done on the server for both roads in — the host's Add Bot over
the wire, and single player's own — so the two always agree.

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
the same ones `game.rule` and `game.set_rule` take. A name that spells no rule
is reported and dropped, and the rest of the table still applies. A value that
is not a number is dropped without a report, so `"40"` in quotes sets nothing.
At round start the table is checked and applied whole: a value outside its
rule's range, or two values that break a pair between them, applies none of the
table's rules, and the console says why.

```lua
rules = {
  tank_death_ticks  = 150,
  tank_reload_ticks = 10,
},
```

### `scenario.settings`

Choices the host makes in the lobby, before the round, without editing the
script. Each one is a dropdown in the script's details dialog. The script
reads the value with [`game.setting(id)`](#settings).

```lua
settings = {
  { id = "round_minutes", label = "Round length (minutes)", type = "int",
    min = 1, max = 10, step = 1, default = 4 },
  { id = "rounds", label = "Rounds", type = "int",
    min = 1, max = 5, step = 1, default = 5 },
  { id = "sudden_death", label = "Sudden death", type = "bool",
    default = false },
  { id = "teams", label = "Teams", type = "choice",
    choices = { "Free For All", "Use Lobby Teams" },
    default = "Free For All" },
},
```

| Field | What it is |
|---|---|
| `id` | The name `game.setting` takes. 1 to 31 letters, digits and `_`, and unique in the script. |
| `label` | What the dialog shows beside the dropdown, up to 63 bytes. This is the script's own text; it is not translated. |
| `type` | `"int"`, a whole number, which is also what a missing type means. `"bool"`, on or off, drawn as a dropdown of On and Off. `"choice"`, one of a list of words, drawn as a dropdown of the words. |
| `min`, `max` | The range, both ends in it. Required for `"int"`; a `"bool"` or `"choice"` row must not give them. |
| `step` | The gap between entries, above 0. Optional for `"int"`, 1 when missing; a `"bool"` or `"choice"` row must not give it. |
| `choices` | For `"choice"` only, and required there: a list of 2 to 8 different words, each 1 to 31 bytes, in the order the dropdown shows them. Like the label, they are the script's own text and are not translated. |
| `default` | The value when the host picks nothing. Required. For `"int"`, a whole number inside the range and on the step; for `"bool"`, `true` or `false`; for `"choice"`, one of its words, written exactly as in `choices`. |

A script may declare up to 16 settings, and a setting may offer up to 100
entries (`(max - min) / step + 1`). A row that breaks a rule, a duplicate id,
and a row past the sixteenth are reported and dropped; the rest still apply.
`-validate` prints the same reports. A package's `manifest.json`
carries the same list under `"settings"` with the same fields, a `"bool"`
row's `"default"` being JSON `true` or `false` and a `"choice"` row's
`"choices"` a JSON array of strings.

A `"choice"` row travels to the lobby as its id, label and words in no more
bytes than the longest `"int"` row takes (113), so a long id and a long label
leave less room for the words. A row whose id, label and words do not fit is
reported and dropped. A client or server from before `"choice"` existed skips
the row: its dialog does not show it, and an older server does not offer it
at all, so `game.setting` for it raises there as it does for any id that is
not declared.

The server reads the declaration without running the script, the same way it
reads `rules`. A value the host picks is held for the lobby session, per
script file.

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
| `on_tick(tick)` | Once per frame, fifty times a second. The `tick` it is handed goes up by **2** each time, not by 1 — see [Three clocks](#three-clocks). With more than one script on the list, the scripts' `on_tick` run bottom to top, so the top script writes last and its `set_modifiers`, `set_rule` and other writes stand (see [The script list](#the-script-list)). A tick that spends the shared per-tick budget drops the rest of its calls, and for `on_tick` those are the top scripts. **Prefer not to declare this.** Timers and the hooks below cover nearly everything, and a handler that runs fifty times a second is a handler that has to be cheap. |
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

The table is applied as a whole. Every value goes in before the check reads
any of them, so a table that raises `pill_max_armour` and `pill_repair_amount`
together is taken in whatever order it lists them. A table the check refuses
applies none of its rules: the round plays the classic table, and the console
names the reason, as `-validate` does.

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
| `on_tank_hit(victim, attacker, cause, amount, pill, scripted)` | Every shell and every mine a tank takes, after the damage is worked out. `attacker` is the seat that fired the shell or laid the mine, or `game.NEUTRAL` for a pillbox's shell and a mine nobody owns. `cause` is `"shell"` or `"mine"`. `amount` is the armour the tank actually lost, which can be `0` — a hit `damage_scale` priced at nothing, or one landing on a tank already at zero. `pill` is the pillbox number that fired the shell, and `nil` for anything else. A shell `can_hit` let through never hit, so it is not reported here. A hit that kills raises this and then `on_tank_killed`. |
| `on_lgm_died(p, killer, mx, my, scripted)` | The square is where the man died, captured before the respawn moves him. |
| `on_lgm_landed(p, mx, my, scripted)` | The builder `on_lgm_died` reported has finished his flight back and touched down. `mx`, `my` are the square he reached — where his tank stood when he died, unless `builder_parachute` aimed him somewhere else — and he walks to the tank from there rather than arriving in it. |
| `on_base_captured(n, old, new, scripted)` | `old` and `new` are owners: `old` is `game.NEUTRAL` for a base nobody held, and `new` is always a seat, because a base changing to nobody's raises `on_base_neutralized` below instead. Neither hook fires when a base passes to an ally or falls to nobody because its owner left — that hand-over raises no event at all. |
| `on_base_neutralized(n, old, scripted)` | A base that changed to nobody's. No play reaches it: the engine's own path for a base going neutral raises no event, the same silence `on_base_captured` describes above, so the only thing that fires this hook is `game.set_base_owner` clearing one — which makes `scripted` always true. |
| `on_pill_captured(n, old, new, scripted)` | Every change of a pillbox's owner, with `game.NEUTRAL` as `new` where nobody took it. |
| `on_pill_placed(n, p, armour, scripted)` | The pillbox first, then who placed it. Fires for every way a carried pillbox reaches the map, not only a builder finishing the job: a tank sinking or being destroyed puts its cargo down, a builder dying puts the one in his hands down, and a player leaving does both. `armour` is what tells them apart — a built pillbox arrives at the sim's cap, every other route arrives dead at `0` — so test it, not `p`, before treating one as a live gun. `p` is whoever was carrying it, which on a leave is a slot on its way out. |
| `on_pill_picked_up(n, p, scripted)` | The same order. |
| `on_pill_killed(n, by, scripted)` | `by` is the seat credited with the blow, or `game.NEUTRAL` where none can be: a blast that caught the pillbox, and a shell another pillbox fired, both name nobody. `can_die` and `pill_damage_scale` are told whose tank the blast came from, but nobody is credited with it, so this hook still names nobody for one. |
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
| `can_hit(attacker, kind, n, pill)` | When a shell reaches a `"tank"` or a `"pill"` it would hit. `n` is the seat for a tank and the pillbox number for a pill. `attacker` is the seat that fired it, or `game.NEUTRAL` for a pillbox's shell, and `pill` is the pillbox that fired it, `nil` for a tank's shell — so a pillbox's side is `game.pill(pill).owner`. Pillbox shells reaching pillboxes are asked too. A shell is asked once about each target: once let through, it passes that target for the rest of its flight without asking again. | `false` lets the shell fly on as if the target were not there: no damage, no knockback, no boat lost, no angry pillbox and no hit sound, and it can still hit whatever is behind. Ordinary rule: yes. |
| `can_die(kind, n, killer, cause, pill)` | When a blow would destroy a `"tank"`, a `"builder"` or a `"pill"`. `n` is the seat for a tank or its builder and the pillbox number for a pill. `cause` is `"shell"`, `"mine"`, `"deep_sea"` or `"script"` for a tank; `"shell"`, `"mine"` or `"explosion"` for a builder; `"shell"` or `"explosion"` for a pill; and `nil` when the engine could not name it. `"explosion"` is a dying tank's blast. `killer` is the seat that fired the shell (`game.NEUTRAL` for a pillbox's); for a builder caught by a mine it is the seat that laid it, and for a builder or a pill caught in a blast it is the seat whose tank blew up. Those two are named here and credited with nothing: `on_lgm_died` and `on_pill_killed` still name nobody for them. `pill` is the pillbox whose shell it was, and `nil` for anything else. | `false` leaves a tank at zero armour and alive, a builder untouched, a pillbox at one armour. Ordinary rule: yes. |
| `can_ally(p, q)` | When seat `p` asks seat `q` for an alliance, and again when `q` accepts, so an answer that changed in between is the one that counts. `p` is always the seat that asked. Bots ask and accept the same way people do. A script's own `set_team`, a bot it spawns onto a team, and the seating in `scenario.lobby` are the script's decisions and are not asked. | `false` refuses the request, so `q` is never shown it, or refuses the accept. Ordinary rule: yes. |
| `on_choose_start(p)` | When the engine is about to pick a start for seat `p`, at a spawn, a respawn or a teleport with no start named. A start the script named in the op itself is not asked about. | A start number, counted from 1 as `game.start` counts. A number that names no live start is reported and the engine picks. `nil` lets the engine pick. |
| `spawn_loadout(p)` | When seat `p`'s tank is created, unless the op that spawned it named a `loadout` of its own. A named one outranks the policy and is taken as it is read, so the policy is not asked for that tank and the named amounts are spent on it rather than held for the seat's next life. | `"open"`, `"tournament"` or `"strict"` for that game type's loadout, or a table of all four amounts, `{ shells = , mines = , armour = , trees = }`, each 0 to 255. A table short of one is reported and the ordinary loadout stands. |
| `damage_scale(attacker, victim, cause, pill)` | On every hit a tank takes, with `cause` as `can_die` spells it for a tank. `attacker` is `game.NEUTRAL` for a pillbox's shell, and `pill` is that pillbox's number, `nil` for anything else. | A percent, 0 to 10000. 100 is the ordinary amount and 0 is a hit that costs nothing. Out of range is reported and 100 stands. |
| `pill_damage_scale(attacker, n, cause, pill)` | On every blow pillbox `n` takes: a shell, with `cause` `"shell"`, and a dying tank's blast, with `cause` `"explosion"`. `attacker` is the seat that fired the shell (`game.NEUTRAL` for a pillbox's) or the seat whose tank blew up, and `pill` is the pillbox that fired the shell, `nil` for anything else. | A percent, 0 to 10000, of the armour the pillbox loses. 100 is the ordinary amount and 0 costs it nothing. Only the armour is scaled: the shell still stops there and the pillbox still turns angry. Out of range is reported and 100 stands. |

`scenario.lobby.max_players` is the one decision that is a number rather than
a function; it is applied by the lobby without asking.

**Switching off friendly fire.** A pillbox's shells carry no seat, so a script
reads the side from the pillbox that fired them:

```lua
local function side(attacker, pill)
  if pill ~= nil then
    local p = game.pill(pill)
    return p and p.owner or game.NEUTRAL
  end
  return attacker
end

function can_hit(attacker, kind, n, pill)
  local from = side(attacker, pill)
  if from == game.NEUTRAL then return nil end
  local to = n
  if kind == "pill" then
    local p = game.pill(n)
    to = p and p.owner or game.NEUTRAL
  end
  if to ~= game.NEUTRAL and (to == from or game.allied(from, to)) then
    return false
  end
  return nil
end
```

**What a player sees of a shell let through.** The policies run on the server
alone. A client draws its own shells ahead of the server and stops drawing one
where it meets another tank or a pillbox, because that is where a shell
ordinarily ends. A shell `can_hit` lets through therefore disappears from its
firer's screen at the tank or pillbox it passed, and the explosion where it
really lands is drawn when the server reports it. Nobody sees a hit that did
not happen: there is no hit sound, no damage and no knockback. Other players'
views of the same shell pick it up again from the next update once it is past.

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

**A script may send 256 writes a frame, and 8 of them may be messages or
sounds** — `message`, `say` and `sound`. A write past either count does not
apply and answers `nil, "SCN_OP_RATE"` instead; the allowance comes back on the
next frame. This is an answer, not an error, so it does not count toward
switching your script off. `game.log` counts toward the 256 but not the 8. A
write refused because it came from inside a policy costs nothing, and neither
do the rules in your `scenario` table, which the server applies itself.

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
| `game.lobby_slot(p)` | `{ connected, bot, team, name, ready, fielded, alive, team_pool }`, or `nil` for an empty seat. **`fielded` is the field that tells a held seat from one on the field.** `team_pool` is the label of the bot naming pool the seat's team draws bot names from, such as `"Famous Painters"`: the name the lobby shows in that team's pool dropdown. It is absent when the seat is on no team (0) or the team has no pool. A team has a pool once it is set up in the lobby; teams 1 and 2 start with the first pool, and a team the host never set up has none, so its seats read `nil` even when they hold bots. A dedicated server's `-bots` names its bots from one random pool without setting any team's pool, so there `team_pool` is the first pool's label, not the pool the bot names came from. The label comes from the server's own pools, so every client reads the same word. A script can label a team of bots only by it (see below). |
| `game.allied(a, b)` | Whether seats `a` and `b` are on the same side in the game: `true` or `false`, `true` for a seat and itself, and `nil` when either seat is empty. This is the alliance table every game rule reads — who a pillbox fires at, which bases refuel whom — including alliances players made and left in play. **It can differ from `game.lobby_slot(p).team`:** two teammates where one has left the alliance share a team but are not allied, and players on different teams who allied share a side but not a team. |

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

A team with no human on it can be named after its bots' pool, as Joust and
Pillbox Tag do on their panels:

```lua
local function team_label(t)
  local pool, human = nil, false
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    if slot and slot.team == t then
      pool = pool or slot.team_pool
      human = human or not slot.bot
    end
  end
  return (not human and pool) or ("Team " .. t)
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

Timers due on the same frame run oldest first, and one of them may cancel
another that has not run yet: the cancelled one does not run, and the cancel
answers `true`.

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
| `game.set_modifiers(p, t)` | Replaces a tank's `speed`, `accel`, `turn`, `reload`, `dealt` and `taken` percentages. A field the table leaves out goes back to the classic tank: the whole set is replaced, not merged. `speed` goes from 0 to 2000 and the others from 0 to 255; past that the call is refused with `SCN_OP_RANGE`. `speed` scales the ground's speed cap, so 534 puts a river's 3 at a road's 16. However high it is set, the engine holds the tank's scaled cap to 160 world units a frame, the most the byte-sized modifier could give on the fastest speed rule. |

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
| `game.set_tile(x, y, t)` | Writes one square's terrain, by a `game.TERRAIN` code. `set_tile` and `fill_rect` share 256 changed squares a frame; a `set_tile` past that answers `nil, "SCN_OP_RATE"` and writes nothing. |
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
| `game.bot_init(p, t)` | Hands a bot a new init table. A seat that has been fielded takes one whether it is on the field now or not: a seat taken off keeps its brain, and the table is waiting in it when the seat is fielded again. A human seat is refused with `SCN_OP_IS_HUMAN`, an empty one with `SCN_OP_NO_SUCH_PLAYER`, and a held seat that has never been fielded — which has no brain yet to hold the table — with `SCN_OP_NO_RUNNER`. |
| `game.lobby_add_bot(t)` | Seats a bot in the lobby and answers which seat it took. |
| `game.lobby_remove_bot(p)` | Takes a bot out of the lobby. A human seat is refused. |
| `game.lobby_set_team(p, t)` | Moves a lobby seat to another team. |

`spawn_bot` takes one table, all of whose fields are optional:

| Field | Meaning |
|---|---|
| `slot` | The seat to take. Left out, the first free seat is taken, and which one that is is decided as the spawn lands rather than as it is queued. A seat held for a bot that is not on the field is the one occupied seat a spawn may name — fielding it is what the seat is for. A seat that already has somebody on the field is refused with `SCN_OP_ALREADY`. |
| `name` | The bot's name. A seat that is already held keeps the name it was seated with, whatever this says. |
| `brain` | The brain to run, named the way a team's is: the directory under the server's `brains/`, such as `GoalHunter`. Left out, the seat's own brain is used — the one its team was written with — and failing that the server's. A name this server does not have, a value with `/` or `\` in it, and `package:NAME` are each refused with `SCN_OP_NOT_FOUND`. |
| `team` | The team to join. A held seat keeps the team it was seated with. |
| `start` | The start to come in on, 1-based. Left out, the engine chooses. |
| `loadout` | What this one bot comes in with: `"open"`, `"tournament"` or `"strict"` for that game type's amounts. A word that is none of the three stops the call the way any bad argument does. It outranks `spawn_loadout`, which is not asked about this tank at all, and it is spent on the tank the spawn builds — the bot's next life is fuelled the way every other tank's is. Left out, `spawn_loadout` answers, and failing that the round's own game type. |
| `mode` | The brain mode this bot plays in, by the key the brain's own `modes.txt` lists. Left out, the seat's config stands — which for a seat the template made is what the template gave it. A key the brain does not list is refused with `SCN_OP_NO_SUCH_ITEM`. |
| `difficulty` | The level inside that mode, by the key the same file lists. Left out with a `mode` named, the new mode's own default level is taken; left out with no `mode` named, the seat's level stands. A key the mode does not list is refused with `SCN_OP_NO_SUCH_ITEM`. |
| `init` | A flat table of names to strings or numbers, handed to the brain at its first breath. Left out, the seat's own is used — the one its team was written with. |

`lobby_add_bot` takes `name`, `brain`, `team`, `slot`, `fielded`, `mode` and
`difficulty`, where `fielded = false` asks for the seat without the bot. A
held seat takes `mode` and `difficulty` too: no brain loads for it yet, but
the seat's row shows them from the moment it appears and the spawn that
fields it later reads them off the seat.

**`mode` and `difficulty` land in the seat's config, not in the init table.**
That is what puts them on the lobby row and what the server turns into the
brain's `mode=` / `difficulty=` tokens. A scenario may also write the same
pair into its `init` table, which reaches the brain by a different road: the
brain flattens the table onto the end of the same token string and takes the
last write. Writing both is not wrong — the two roads serve different bots,
the template's pair being what a seat the lobby shows carries and the table's
what a bot spawned into a seat no template described gets. Two identical
values are one value applied twice. Two different ones are not: the table's
wins at the brain, and the lobby row still shows the seat's.

Survival writes them on the **template only**. Every seat it fields is one of
the template's own held seats, so the seat's config already carries the pair
by the time a VM is built for it, and a wave's table would be a second copy
of a value nothing had changed. There is a stronger reason not to put them in
a wave's table: a wave table goes to a brain through `bot_init`, which reaches
a brain that is **already running**, and `mode=` and `difficulty=` are refused
at runtime — so they would buy nothing and cost one "unsupported" line per bot
per wave in the log.

**Changing a bot's orders while it plays.** `bot_init` takes the same table
`spawn_bot`'s `init` field takes — flat, names to strings or numbers, at most
16 pairs — and hands it to a bot that has been fielded:

```lua
game.bot_init(p, { noblitz = "1", cfg = "PILL_REPOSITION_ENABLED=false" })
```

The table **replaces** the bot's, whole. What the spawn's table said and this
one does not say is gone, because the brain's table is rebuilt rather than
merged into. Values are text and numbers, as a spawn's are, so a flag a brain
reads as on or off is written `"1"` and `"0"` rather than `true` and `false`.
GoalHunter treats only its known flag words that way (`noblitz`, `blitzonly`,
`suicider`, `nosuicider`, `noclaimdead`, `normal`, `ammoless`, `survivor`, `horde`); a valued token such as
`blitzsuiciders = "1"` keeps its value.

What the bot does with it is the brain's business, and there are two levels
to it:

- **Every brain** gets its `BRAIN_INIT` global rebuilt from the table. A brain
  that reads the global somewhere other than at its first breath reads the new
  pairs from the next time it looks.
- **A brain that has written a `Brain.on_init(t)`** is called with that same
  table as well, which is how a brain acts on the change rather than waiting
  to be asked. The call is made between ticks, never while the brain is
  thinking. A brain whose `on_init` raises has the error logged on the server
  and carries on playing — a scenario changing a bot's orders cannot kill it.

GoalHunter, the brain that ships with the server, writes one: it re-reads the
whole token string, so `cfg=NAME=VALUE` and `preset=` change its constants
there and then, and the bare flags (`noblitz`, `blitzonly`, `suicider`,
`nosuicider`, `noclaimdead`, `normal`, `ammoless`, `survivor`, `horde`) change the bot's behaviour from the next
tick. `difficulty=` and `mode=` are **not** applied at runtime — those choose a
whole bundle of values at load and a second bundle cannot unset the first — so
the brain logs them as unsupported and leaves them. It also says one line,
`init updated: <n> tokens`, to the other bots on its team (never to a human:
a scenario that retunes its bots often would fill the newswire), so a script's
`on_chat` can see the change land when another bot is on the same side.

A bot whose brain is not running yet still keeps the table: the record is what
its next brain is built from, so nothing the script asked for is lost.

A seat that is off the field takes the table the same way. An unfielded seat
keeps the brain it was fielded with rather than throwing it away, so the
global is rebuilt where the brain waits and the seat comes back onto the field
already carrying the new pairs. This is how a wave-based scenario retunes its
seats between waves, and it costs the next wave nothing: the seat is fielded
again on the brain it already had. Only a held seat that has never been
fielded is refused, with `SCN_OP_NO_RUNNER` — there is no brain there yet to
hold the table, so field it first and then write into it.

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
line in the server log saying which of the two differed. A **spawn's** `init`
table is therefore not the place for orders that change from one wave to the
next — varying it is what buys the rebuild. To retune a held seat between
waves, spawn it with the same table every time and write the new one with
`bot_init` while it is off the field: what a spawn is matched against is the
table its VM was built with, which `bot_init` does not touch, so the seat
still comes back onto the runner it already had. `game.hint` below is the
other way to tell a bot something mid-round.

The roster ops are the six above, `set_team` and `lobby_set_team` included.
All of them are refused inside `on_setup`: the round is still being built
there, and a roster edit would re-enter the machinery that is building it.
`bot_init` is refused there with them, for the other half of the same reason
— inside a start the bots and their brains are still being built, so there is
no settled brain to write into. Field your first wave, move seats between
teams, and change a bot's orders from `on_start` onwards.

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

GoalHunter also reads `ping = "1"` on a `goto`: the order is filed as if
a bot-command ping had given it, so a square a hostile pillbox can shoot turns
the hold into the decoy hold. No scenario op places a ping, so this key is how
a script reaches the decoy hold (the `decoy_getaway` ROOST test uses it).

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
| `game.set_voice_everyone(on)` | With `true`, voice in the running round goes to every player rather than to the talker's allies alone. `false` puts it back. |
| `game.voice_everyone()` | Whether voice is going to everyone: what `set_voice_everyone` last set this round, and `false` on a server with voice off. |
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

`set_voice_everyone` is for a round where sides do not fit voice. In a round,
the server sends a player's voice only to that player's allies, so a round
where every tank is on a team of its own has nobody hearing anybody. With voice
to everyone on, the alliance check is skipped and every connected player hears
every talker. Nothing else changes: a player's mutes still hold, a player still
hears at most four talkers at a time, and a spectator still hears nobody.
Outside a round everyone hears everyone already, so the setting has no effect
in the lobby.

Every player is told when the setting changes in a running round: a line in
the newswire, in the player's own language, and while it is on the outline of
the microphone in the game view is drawn amber rather than white. A setting already on when
the round starts is told as it starts, and a player who joins mid-round is
told as they join. Setting the value it already holds tells nobody anything,
so a script may set it every tick. The replay records each change.

The setting is the server's own and lasts for one round. Every round starts
with it off, and it goes off again when the round returns to the lobby and
when the scenario is taken off the server, so a later round without the script
has voice to allies only. Call it from `on_setup` or later; a call in the lobby
is cleared when the round starts. A server with voice off forwards no voice at
all, so `set_voice_everyone(true)` is refused there (`SCN_OP_WRONG_STATE`);
`set_voice_everyone(false)` is always taken.

---

## Showing things on a client

A scenario never draws on a client. It sends what it wants shown as data, and
each frontend draws that with its own 2D calls, so one scenario looks right on
the desktop game, in a browser and on a phone without knowing that any of them
exist.

| Call | What it does |
|---|---|
| `game.panel(id, list[, target])` | Draws panel `id` from a list of primitives. An empty list clears it. |
| `game.score(target, value[, label])` | The scenario's own score for one seat with a number, or for a team with `{ team = t }`. `label` is the short word shown beside it, up to 15 bytes. A round that scores both teams and seats gets a grouped recap table: a row for each scored team, best team score first, with its members (by lobby team) under it, best own score first; seats on no scored team follow. A round that scores only one of the two keeps the plain table of players. |
| `game.announce(text[, seconds[, target[, position]]])` | A big line across the game view for that many seconds. Left out, `position` puts the line where it has always gone, centred in the upper third of the view. Give `"top"`, `"center"` or `{ x = , y = }` to put the centre of the line somewhere else. Empty text takes the line away. |
| `game.status(text[, countdown_to[, target]])` | The status line: one line at the very top of the game view, centred, that stays until it is changed or cleared. `countdown_to` is a tick on `game.tick()`'s clock; the client shows the time left to it after the text. Empty text takes the line away. |
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

**Where an announcement goes.** With no `position` the line is centred across
the game view, with the top of its letters 0.28 of the way down, in the upper
third and clear of the player's own tank in the middle. No panel, window or
status line moves it. On the main view that is where every announcement has
always gone. On the full screen map the same rule is applied to the map, so
the line sits 0.28 of the way down the map and not at its old spot, which was
worked out from the main view's rectangle. In tablet mode the rule is
applied to tablet mode's own game view; before, tablet mode drew no
announcement at all.

`position` puts the centre of the line at a point on the game view. Give a
table `{ x = across, y = down }`, each from 0 to 1: `{ x = 0, y = 0 }` is the
top-left corner, `{ x = 1, y = 1 }` the bottom-right one and
`{ x = 0.5, y = 0.5 }` the middle. Two words are short names for the points
scripts use most:

| Word | Same as | Use it for |
|---|---|---|
| `"top"` | `{ x = 0.5, y = 0 }` | Lines that come often, such as kill lines: up out of the way |
| `"center"` (or `"centre"`) | `{ x = 0.5, y = 0.5 }` | Important lines in a quiet moment, such as the round start or the winner |

The line is moved the least it takes to keep all of it inside the view, so
`y = 0` puts it as high as it goes and no edge is ever cut off. A line wider
than the view is centred across it. When a status line is up, its row counts
as a band the full width of the view, from the top to the bottom of the status
line's box. A line that would come within one gap of that band drops to one
gap below it, wherever it sits across the view, even where the status line is
short and nowhere near it. The gap is the scenario panel's own inset from the
corner of the view. A line with a position does not move
for the scenario panel or other windows, and `"center"` sits on the player's
own tank: the script chose the spot, so use it when there is little else to
watch.

The position is sent as two bytes, so it lands on one of 255 steps across and
down. A share that is not a number, a table without both `x` and `y`, or a
word other than these stops the script. A share below 0, above 1, or NaN is
refused with `SCN_OP_RANGE`.

A line with no position is up to 128 bytes. A line with a position is up to
125 bytes, and a longer one is refused with `SCN_OP_TOO_BIG`. A client older
than the position draws every line in the usual place, and it would drop a
positioned line longer than 125 bytes and show nothing. Both lines are drawn
on the desktop view, on the full screen map and in tablet mode, each over its
own game view.

On a server older than the position, `game.announce` reads only its first
three arguments, so a script that passes a position there still shows the
line, in the usual place.

**The status line.** `game.status` holds one line at the top of the view for
as long as the round wants it, such as "Wave 3/10". With `countdown_to`, the
client adds the time left to that tick, in minutes and seconds, four spaces
after the text ("Wave 3/10    2:44"), and counts it down on its own clock, so
a countdown is one call and not one a second. It stops at 0:00. The text is
up to 128 bytes, and a longer one is refused with `SCN_OP_TOO_BIG`. A player
sees the last line written to them, whether it went to everyone, to their team
or to their seat, and an empty line to any of those takes it away. Calling it
again with the same text, countdown and target sends nothing when no later
line was written to any of those players, so a script can restate its line
every second without cost. A late joiner is given the lines in the order they
were written, so it ends on the last line written to it, the same line the
players around it see. A replay shows it too. A `countdown_to` below 0 or
past 4294967294, or a seat or team outside the game, is refused with
`SCN_OP_RANGE`.

```lua
game.status(string.format("Wave %d/%d", wave, waves), wave_ends_at)
game.announce("Wave 3 - they come from the north", 5, nil, "center")
game.announce(name .. " got a kill", 2, nil, "top")
game.announce("Red has the flag", 3, { team = 2 }, { x = 0.5, y = 0.8 })
game.announce("Go!", 2)      -- no position: where announcements always go
game.status("")              -- take the status line away
```

**Who sees it.** The last argument of `panel`, `status`, `announce` and the three marker
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

`size` is `"small"`, `"normal"` or `"large"`, 8, 11 and 16 panel units high.
A client from before `"large"` draws a large item at normal size. `align` is
`"left"`, `"centre"` or `"right"`, and says which way the text sits about its `x`. A timer's `mode` is
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
that, with each large item counted twice, or if the whole list comes to more
than the 1017 bytes one update carries. A primitive written wrong — a name that spells no primitive, an
operand missing, an operand that is not a number — stops the script, like any
other call written wrong. A value the simulation will not take, such as a
colour outside the palette, is refused as an answer the script can read. A
`rect` with colour `none`, width and height 0 at `x` 2, `y` 0 is refused too:
it draws nothing, and on the wire it is the mark that makes an item large.

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

## Settings

| Call | What it does |
|---|---|
| `game.setting(id)` | The value the host picked in the lobby for one of this script's own [`settings`](#scenariosettings), or its declared default when the host picked nothing: a number for an `"int"` setting, `true` or `false` for a `"bool"` one, and for a `"choice"` one the chosen word as a string, spelled exactly as the script wrote it in `choices`. An id the script does not declare **raises**, for the reason an unknown rule name does. |

The value is fixed for the round. Read it in `on_init`, or at the top of the
file, and keep it in a local:

```lua
local WAVES        = game.setting("rounds")
local WAVE_LIMIT_S = game.setting("round_minutes") * 60
local SUDDEN_DEATH = game.setting("sudden_death")   -- true or false
local TEAMS        = game.setting("teams") == "Use Lobby Teams"
```

A script reads only its own settings: the id is looked up in the declaration
of the file that makes the call. The server checks every value the host
sends. A value below the range becomes the lowest entry, one above it the
highest, and one inside the range but off the step the default. A
`"bool"` setting takes only on or off, and a `"choice"` setting only one of
its words; anything else is refused. The server and the lobby keep a choice
as the index of its word, so a script that reorders or renames its words
moves a pick the host made before the change. An older
server, or a client that cannot send a pick, leaves every setting at its
default, so a script must play correctly on its defaults alone.

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
| `mine_fatal_divisor` | 3 | 1 to 255 | A mine blow big enough to kill has one part in this many taken back off it, so the tank keeps facing the rest, rounded up. The classic 3 is the two-thirds a fatal mine hit deals. Bigger is harsher, not softer: 255 leaves a fatal blow at almost its full size, and 1 takes the whole thing off and leaves a fatal mine dealing nothing at all. |
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
| `tank_slide_step` | 32 | 0 to 63 | How far a knocked tank slides per step. |
| `tank_slide_mac` | 0 | 0 to 1 | Whether a shell hit pushes a tank the Mac Bolo way. 0, the classic table, is the WinBolo push: `tank_slide_step` at every armour level, moved and decayed by `tank_bump_decay_shift` each tick. 1 is the Mac Bolo push: `tank_slide_step` plus the armour bonus below, with the step and `tank_bump_decay_shift` read per 40 ms, so a push travels step × 2^shift in all, in a slide that moves every tick with its fractions carried. Mac Bolo plays it with a step of 28 and a shift of 2. |
| `tank_slide_armour_bonus` | 32 | 0 to 255 | Extra push step at zero armour, added to `tank_slide_step` in proportion to the armour missing before the hit: nothing at full armour, the whole bonus at none. Only read while `tank_slide_mac` is 1. A `tank_slide_step` of 0 is no push at all, bonus included. |
| `tank_wall_glide` | 0.0 | 0.0 to 1.0 | 0 slides a tank along a wall it hits; 1 lets it glide off free. Not read while `tank_collision_mac` is 1. |
| `tank_collision_mac` | 0 | 0 to 1 | Whether a tank collides with walls, live pillboxes and hostile bases the Mac Bolo way. 0, the classic table, is the WinBolo circle: a tank of radius `tank_hit_radius` is pushed out of each solid square and slid along it by `tank_wall_glide`. 1 is Mac Bolo's sixteen direction-dependent tank boxes, nudged one pixel at a time, with a corner pushing on both axes and a tank wedged between opposing walls left where it is. It lets a tank pass closer beside a pillbox than the circle does, which is what puts it inside the bad-lead range of `pill_aim_mac`. |
| `tank_deep_sea_safe` | 0 | 0 to 1 | Whether a tank with no boat can drive over deep sea. 0, the classic table, drowns it the tick it reaches deep sea. 1 lets it drive there at `speed_deep_sea` and stay alive; a tank on a boat keeps its boat either way. Drowning is tested every tick, so a tank still out on deep sea with no boat when this goes back to 0 drowns on the next tick, the same death as driving in. |

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
| `man_bless_tile_terrain_speed` | 0 | 0 to 1 | Which speed the builder has on the square he is going to build on, while he walks up to its centre. 0, the classic table, is the WinBolo walk: on that square he always moves at `man_speed_refuel_base`, whatever its terrain, so a wall put up on swamp is reached at full speed. 1 is the Mac Bolo walk: he crosses it at its own cap from this table, as he does every other square, so the last stretch to a wall on swamp goes at `man_speed_swamp`. A square whose cap is 0 (a river under a road or a boat, a wall to repair, a live pillbox) keeps `man_speed_refuel_base`, so he is never left standing still. The man has a build square only for a road on a river, a boat, a wall and a pillbox, so a tree, a plain road or a mine is not changed. Nor is the walk back to the tank, or a man leaving a tank on a boat. |

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
| `pill_base_defend_range` | 9 | 0 to 255 | How near a base being shot at has to be to anger an allied pillbox, in map squares. `pill_base_defend_shape` says how it is measured. |
| `pill_base_defend_shape` | 0 | 0 to 1 | How `pill_base_defend_range` is measured. 0, the classic WinBolo table, is a square: a pillbox up to the range away on each axis is angered, so 0 range is a pillbox that answers only for a base on its own square. 1 is a circle, as Mac Bolo measures it: the range is a radius and a pillbox exactly that far off is not angered, so 0 range is a pillbox that never answers for a base. Mac Bolo plays it as a circle of 7. |
| `pill_aim_iterations` | 200 | 1 to 65535 | The step budget the aim solver gets to lead a moving target. 1 is a pillbox that never leads and fires at where the target is standing now. |
| `pill_massage_range` | 0 | 0 to 65535 | How near a tank has to be for a pillbox to aim with the legacy forward prediction rather than the solver, an approximation of the Mac Bolo pill massage which misses a tank circling it. Zero, the classic table, is a pillbox that always leads its target properly; 384 is a square and a half, the distance the old build-time switch used. Not read while `pill_aim_mac` is 1, which replaces this approximation with the original calculation. |
| `pill_massage_cosine` | 0.5 | 0.0 to 1.0 | How straight at a close pillbox a tank has to be driving to be aimed at properly anyway, as the cosine of the angle between its heading and the line to the pillbox. One aims sloppily at every tank inside `pill_massage_range`, zero at none of them. Does nothing while that rule is zero or `pill_aim_mac` is 1. |
| `pill_aim_mac` | 0 | 0 to 1 | Whether a pillbox aims the Mac Bolo way. 0, the classic table, is the WinBolo solver, or the massage approximation above inside `pill_massage_range`. 1 is Mac Bolo's integer distance and lead calculation: a moving, unobstructed tank inside about one square makes the unsigned lead wrap, so the pillbox aims roughly 32 squares along the tank's heading and misses; a stopped or blocked tank is aimed at directly; beyond that distance the same formula leads normally. `pill_massage_range`, `pill_massage_cosine` and `pill_aim_iterations` are not read while this is 1. |
| `pill_shell_cap` | 0 | 0 or 1 | Whether pillboxes limit how many of their shells can be in the air at one tank, to `pill_max_shells_at_tank`. 0, the classic table, lets every pillbox fire at its nearest target however many shells are already on the way. |
| `pill_max_shells_at_tank` | 12 | 1 to 255 | The most pillbox shells that can be in the air at one tank. A pillbox whose nearest target already has this many coming fires at the next nearest enemy in range instead, or holds its shot until one lands. Only read while `pill_shell_cap` is 1. |

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

**Hearing.** How far a sound carries, in map squares, tested on each axis so
the bands are boxes rather than circles. Inside the soft range it is played
near, at or past the none range it is not played at all, and between the two
it is played far.

| Rule | Classic | Range | Description |
|---|---|---|---|
| `sound_soft_range` | 15 | 0 to 255 | Inside this a sound is played near. |
| `sound_none_range` | 40 | 0 to 255 | At or past this a sound is not played at all. |

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

Three things only the round finds out, so keep them in mind. **A `call` naming a
function your script never defined is skipped**, not reported — the check has
no way to know what the chunk will define, since a script may define a
function under a condition. A region a test names may be one
`game.define_region` makes during the round, so an unknown region name is not
refused either. And **an `announce` position word is checked only when the
trigger fires**: the check before the round holds a word to being text, not
to the words an op takes, so `{ "announce", "Go", 3, "all", "middle" }` passes
it. When that trigger fires, the word raises inside `announce`, the same way
a literal of the wrong kind does above.

A trigger can only write a number, a string, a boolean or a `{ field = }`, so
the position of an `announce` action is `"top"`, `"center"` or `"centre"`. The
`{ x = , y = }` table is out of reach; a script that wants one calls
`game.announce` from Lua. A positioned line is held to 125 bytes, so an
`announce` action with a position and a longer line is refused with
`SCN_OP_TOO_BIG` when it fires and shows nothing.

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
| `SCN_OP_NO_RUNNER` | The seat is a bot's, but no brain is behind it: a held seat that has never been fielded. A seat off the field keeps its brain and is not this. |
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
| A bot's `init` table | 16 pairs, whether it comes from `spawn_bot` or `bot_init` |
| A hint table | 16 pairs; a name 23 bytes, a value 63 |
| Events queued for one frame | 256 |
| Roster changes outstanding at once | 32 |
| Tiles `fill_rect` and `set_tile` may change in one tick | 256 |
| Writes a script may send in one tick | 256 |
| Messages and sounds among them | 8 |
| Errors in a row before the scenario is switched off | 20 |
| `scenario.name` | 63 bytes |
| `scenario.description` | 255 bytes |
| `scenario.game` | 23 bytes |
| A team's `brain` name | 255 bytes |

Going past one of the counts is reported and refused. Spawns, removals and
`bot_init` share the one roster queue and the sim drains one a tick, so a
script that asks for ten bots gets them over ten ticks; the one past the last is refused
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
for one that is not. Warnings, such as a missing `author` or `updated`, are
printed after the problems as `file: warning: key: message`, and do not change
the exit code. A `.lua` file named directly, a mod say, is checked the same way
with no map:

```
WinBoloDS -validate data/mods/MacBoloRules.scenario.lua
```

A `.lua` named directly that cannot be read, a misspelt path say, exits 1, so
a script or a hook that checks a mod by name does not pass on a typo. A map
with no script beside it still exits 0.

Problems the parse itself finds — a rule name that
spells nothing, a tag past what the map holds — are also written to standard
output as the server would log them, so a run that captures one stream sees
half the report. The map has to load before the script is looked at.

The brains a scenario's teams name are checked against the ones this server
has, so `-validate` on the server you are about to run is what tells you a map
wants `GoalHunter` and this machine has not got it. That is a problem
rather than a refusal: the map still plays, with those seats on the server's
own brain.

No round is run and no bot loads, but the file's top level does run, the same
way it would at a round start — so a file you would not run is a file you
should not check, with `-validate` or with the map editor's **Validate**. The
editor makes the same check on the text in its script pane when you press
Validate, whenever it saves the script, and before it packs one, with the
manifest its forms hold standing in for the one a package would carry. It has
no server behind it, so it does not hold tags against the map, and says so.
The stub `game` table answers `nil` to every call, so a
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
- **A copy of a bound scenario.** Save a copy does not serve the map's own
  scenario, because its file is the map, and the map a player downloads is
  the map without its chunk. Serving it would take a map download that keeps
  the chunk, which is not built. Share the `.map` file itself, or publish it
  to the Workshop.
