# ROOST — bot-command tests

A ROOST test asks one question about what a bot does when it is told
something. It is a whole round: a real map, real brains, a dedicated server,
and a scenario script that gives the order and then decides whether the bot
obeyed it.

These are behaviour tests, not unit tests. The unit tests in `tests/unit`
prove a call does what it says at the C level; a ROOST test proves the brain
on the other end of it changed its mind.

## The convention

One test is two files in this directory, named after the test:

    <name>.map            the map the round is played on
    <name>.scenario.lua   the script that drives it and decides the verdict
    <name>.args           optional: the server arguments, replacing the default

The server finds the script on its own — a scenario script is discovered
beside the map it is named after — so nothing has to point at it.

The script says how the round went by ending it with a line that begins
`PASS` or `FAIL`, and by writing the same line to the server's console:

```lua
local function finish(text)
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end
```

Both calls matter. `game.end_round` is the verdict the round itself carries,
and `game.log` is the only way that text reaches the console: the end text
goes to the lobby, and a headless round has nobody in one. The runner reads
the `ROOST VERDICT` line.

A verdict should say the numbers it decided on, not just the word: a FAIL
that reads "turned 175 times (control 155)" tells you what happened, and one
that reads "the bot did not obey" does not.

## Running them

    C:\Python310\python.exe tests/roost/run_roost.py
    C:\Python310\python.exe tests/roost/run_roost.py say_stop_halts_bot
    C:\Python310\python.exe tests/roost/run_roost.py --exe build/WinBoloDS.exe

The runner starts one server per test, on its own port, with its own
timeout, prints a PASS/FAIL table and exits non-zero if anything failed.
`--keep-output` writes each round's console output to `<name>.out` when a
failure needs reading. `ROOST_VERBOSE=1` prints the passing verdicts too.

It looks for `build-own/WinBoloDS.exe` and then `build/WinBoloDS.exe` unless
`--exe` names one. The server is run from the repository root, so the brain
path in the default arguments resolves.

The runner is the entry point; there is no `ctest` registration. CMake here
knows nothing about Python, and teaching it would put a new dependency in
everybody's configure step for one test directory.

## What the default server arguments are, and why

    -gametype tournament -ai yes -nolobby -notracker -nowinbolonet
    -dontsendlog -noinput -bots 2 -allybots 1 -threads 4
    -brain brains/GoalHunter_1.7/init.lua -seed 42 -asap

Two allied GoalHunter bots, because the smallest interesting question about
a bot is what one of them does that the other does not: give the order to
one and the other is the control, same brain and same seed.

`-nolobby` is not optional. A lobby never starts a round with no human in it
to ready up (`serverSimLobbyCheckAllReady` refuses a human-less lobby on
purpose), so a headless round has to skip the lobby altogether. `-ai yes` is
not optional either: without it the server refuses to seat a brain at all.

A test that needs something else — four bots, two teams, another brain —
writes one line of flags into `<name>.args`, which replaces the list above
rather than adding to it. `-map`, `-port` and `-ticks` are always supplied
by the runner.

## Two things to know before writing one

**A bot only hears chat, and only the kinds a lobby-less round delivers.**
`game.message` is the server talking; it never enters a brain's inbox.
`game.say(p, text)` is a seat talking, and that is what a brain reads. With
no target `say` writes team chat, which is right on a real server — but
under `-nolobby` no lobby-slot event is ever published, so each bot's own
copy of the roster has team 0 and its receiver-side team filter drops the
line. Say `game.say(p, text, "all")`, or name a seat, in a ROOST test.

**A bot's reply does not come back.** GoalHunter answers a command by
sending its reply to its own seat (`msg_dest = 1 << player_number`), which
is a local echo: nothing is published, so `on_chat` never sees it and no
round can wait for it. Decide the verdict on what the bot *does*, not on
what it says.

## The tests

| Test | What it proves |
|---|---|
| `say_stop_halts_bot` | A chat line from a seat reaches a bot's brain and it obeys. One of two identical bots is told `stop` over broadcast chat; after that its tank never turns again and the other's turns constantly. Facing is the signal rather than position, because a Bolo tank handed no key keeps the speed it had and coasts straight. |
