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

**A verdict has to fit in one short line.** `game.log` and `game.end_round`
both refuse text of 129 bytes or more, and a refusal is a return value rather
than an error — so a verdict that runs long is not truncated, it is never
said, and the runner reports a round that gave no verdict. Say the numbers in
as few words as they need.

**An ORDER's reply does come back; an operator command's does not.** The old
operator commands (`!cp:5`, `!attack:5`, `!stop`; the leading `!` is required) answer to the bot's own seat
(`msg_dest = 1 << player_number`), a local echo that is never published, so
`on_chat` never sees one. The bot-command lines in `orders.lua` are the other
way round: acks, "released", "Busy (...)" and the group line go to the ALLIES
mask so a human can read them, and a scenario's `on_chat` sees every one. The
order tests below are read off those lines.

**Every chat line reaches `on_chat` twice.** One line said by a seat arrives
as two identical `on_chat` calls on the same tick — the server fans it down
two paths. A round that counts answers has to treat the same sender, text and
tick as one line, which every order test does with a small `fresh()` helper.

**A bot's line can be starved.** A brain has ONE message slot per think, and
the internal channel (the state slate, the known-world digest, the order
verbs) takes it first; the order say-queue only gets a turn when nothing else
went out. On a map with two pillboxes to keep allies posted about, the slate
took every slot and a bot's ack and "Leaving ..." line were never said at all.
Keep a round's world small, and do not build a verdict on a line that only
gets said when the channel is quiet.

**A bot only knows a pillbox it or an ally has SEEN.** An order naming one
nobody has seen is answered "pill #0 not known yet" and goes nowhere — and
only the team's SPEAKING bot (the lowest player number) says that line, so a
round whose only listener is not the speaker gets silence instead. Twelve to
twenty squares is known in these rounds; forty-six is not.

**A bot's chat line is only read as an order if it starts with `!`.** A
headless round has no human in it, so the speaker is another seated bot, and
`orders.lua` drops an unforced line from a bot on purpose — otherwise one
bot's ack would be read as the next bot's order.

## What cannot be tested from here yet

A HUMAN TEAM-MATE. Every seat in a headless round is a bot: the server stamps
`PLAYER_FLAG_BOT` as it seats one (`server_sim_bots.c`), the flag is
deliberately trusted and server-only, and no scenario op clears it — the
roster ops in `docs/SCENARIO_API.md` seat bots and move them between teams,
and `remove_bot` refuses a human seat rather than making one. So a rule that
turns on "a HUMAN ally is within N squares", such as
`ORDER_HUMAN_NEAR_SUICIDE_TILES`, has no round that can set it up. The
measuring function behind it (`util.human_ally_near`) is pinned in
`test_orders.lua` instead, including the case that matters most here: a BOT
team-mate standing on top of the bot must not read as a human.

The bot-command PING — a tile ping that carries a verb, and the caution ping
that cancels or retreats — has no scenario op behind it. `game.say` puts a
chat line on the wire and `game.shell_expired` posts a shell expiry, but
nothing in `docs/SCENARIO_API.md` publishes an `EVENT_PING`, so a script
cannot make a seat ping a square. Every ping path in `orders.lua` — the verb
table by tile, select-by-ping, the repeat ping that adds a bot, the caution
ping — is therefore untested here, and will stay that way until the API grows
a ping op. The chat forms of the same orders are what these rounds cover.
The one exception is the decoy hold: GoalHunter 1.7 reads `ping = "1"` on a
`goto` hint as a ping order, and `decoy_getaway` uses that.

## The arena the order tests share

`order_*.map` is one generated map: a 64-square grass island from (96,96) to
(159,159), two neutral bases out to the west at (104,112) and (104,144), no
pillboxes of its own, and four starts in the sea either side. It is symmetric
about y = 127, so `mapCenter` shifts nothing and every square in a script is
the square the round uses. Each round teleports its three seats onto named
squares, recalls their builders, fills their stocks and places the pillboxes
it needs, so the geometry at the moment of the order is exact.

Two of them differ: `order_cancel` uses the same map, and `order_enemy_ignored`
uses `-teams 2,1` in its `.args` to seat two bots on team 1 and one on team 2.

## The tests

| Test | What it proves |
|---|---|
| `three_shots_go_there` | Three shells that run their full range onto one open square are an order, and a bot obeys it — but only when the burst stands alone, and only a second after the third shot was FIRED. The scenario posts shells through `game.shell_expired`, giving each the tick it left the gun — a script cannot make a seat fire a gun — from the seat of one bot, so the OTHER bot is the only one that hears the line (a sender never receives its own). The shooter is teleported beside the listener first, because the range rule is the shooter's own screen. Phase A fires three and then a fourth half a second later: the listener must never answer. Phase B fires three clean ones: the listener must not answer before the quiet second is up, must then get within two squares of the target, and must still be near it three seconds later. |
| `say_stop_halts_bot` | A chat line from a seat reaches a bot's brain and it obeys. One of two identical bots is told `stop` over broadcast chat; after that its tank never turns again and the other's turns constantly. Facing is the signal rather than position, because a Bolo tank handed no key keeps the speed it had and coasts straight. |
| `order_attack_pill` | `!attack 0`, with no who-word, is taken by exactly ONE bot, and that bot goes. Three seats, the pill twelve squares from one of them and twenty from the others; the speaker never hears its own line, so two bots bid and one wins. Arrival is read at eight squares rather than four: attack_pill is a shooting goal and stands off at five to seven. |
| `order_all_attack` | `!all attack 0` puts every free bot on the same pill with no auction, and the lowest player number among them says ONE line for the group — "2 on pill #0" — instead of one ack each. Both bots have to reach the standoff ring. |
| `order_nearby` | `!nearby attack 0` reaches only bots within ten squares OF THE TARGET. One seat is six squares out and takes it; the seat that hears it from twenty-four squares away answers nothing and is still nineteen squares out twelve hundred ticks later. A dead pillbox is put beside the far seat so that staying away is a choice it had something else to do with. |
| `order_by_name` | A bot's name in front of the verb addresses that bot alone, and the name may be a PREFIX: the round works out the shortest prefix of seat 0's name no other bot shares and orders that. Seat 0 is the FAR bot, so an auction would have given the job to seat 1 — the ack from seat 0 and silence from seat 1 can only be the name. |
| `order_cancel` | `!cancel all` reaches the bot holding an order and it answers "released", which `orders.lua` says from one place only: `release_held`, which is also the only code that empties the order slot. Where the tank goes afterwards is logged and not asserted — the design says goal selection simply reruns, and on a map with one pillbox a released bot heads for it again of its own accord. |
| `order_enemy_ignored` | The team check is on the SENDER. With `-teams 2,1` the enemy seat says `!attack 0` to everyone and gets nothing back — no ack, no "didn't understand". Then an ALLY says the same line in the same words and it is taken in twenty-four ticks, so the silence was the team check rather than a line nobody could act on. |
| `order_retreat_busy` | `!<name> retreat` reaches the named bot and it answers with the goal the verb really turns into: "take_cover #", with no number because the goal has nothing to number. A bot that was already busy would answer "Busy (<reason>)" instead, and none of busy's reasons — man out of the tank, a capture in its last phase — can be set up from outside a round, so this one asks only whether a named retreat lands. |
| `order_death_clears_hold` | A bot killed while HOLDING a place order comes back able to drive. The hold parks the tank — steering takes the throttle away — and nothing used to clear the order slot on a death, so a respawned bot sat frozen on its spawn square for the rest of the hold. Seat 1 says `!goto 128 128`, seat 0 goes and says "holding", the script kills it at once, and the new life has to get two squares from where it came back inside four hundred ticks. The kill is twenty ticks after arriving rather than two hundred because the respawn itself takes five hundred and the whole window has to fit inside the thousand-tick hold. Measured: four squares with the fix, none without it. |
| `order_count_two` | A NUMBER in front of the verb says how many bots go. `!2 attack 0` on order_all_attack's arena, seats and seed: two bots take it and the group line carries the count — "2 on pill #0" — where a plain `!attack 0` on the same squares (`order_attack_pill`) gives one bot and a solo ack. The count is not a new way of choosing bots; it raises how many of the auction's bids win from one to two, so the two that go are the two nearest the pill. |
| `order_attack_closest` | `closest` is measured from the SENDER, not from the bot. Two pills: #0 two squares from the speaker and twelve from the bots, #1 four squares from the bots. A bot resolving `closest` against its own tank takes #1; against the sender it takes #0. The ack carries the pill number, so it says outright which rule ran, and the bot then drives past the near pill to the far one's standoff ring. |
| `order_bot_chat_off` | `bot chat off` stops the bots TALKING, not the bots WORKING. Half A is the control: with chat on, `!attack 0` is acked — which proves the seats, the pill and the message channel all work, so silence in half B is the latch and not a starved slot. Half B cancels, says `!bot chat off` and orders a DIFFERENT pill: not one line comes back, and a bot is at that pill's standoff ring anyway. |
| `order_enemy_stop_ignored` | An operator command from the other team is not a command. An ally's `!stop` freezes seat 0 (the baseline), the enemy's `!start` has to leave it frozen, and the ally's `!start` has to start it again (without which the round proves nothing). **This one passes on both sides of the Lua fix**: the engine already keeps an enemy's chat out of a bot's inbox — the same seat saying the same word freezes this bot under `-allybots 1` and does not under `-teams 2,1` — so the round guards both layers rather than reproducing the bug. The Lua half is covered by `commands.lua -- only an ally may give an operator command` in `test_orders.lua`, which does fail before the fix. |
| `decoy_getaway` | A decoy steps out of a pill's line while a team-mate shoots the pill. The attacker is a suicider, ordered `!<name> attack` from the decoy's seat before the ping, and hits the pill before or soon after the decoy's first hit; the scenario refills the pill's armour so the hold does not end. A `goto` hint with `ping = "1"` puts the decoy on a square six south of the pill, beside a row of walls with the engine's `building_life` (4). It must say "decoying". The first step needs a hit. After it, the BLOCKER STEP (`DECOY_GETAWAY_BLOCKER_STEP`) moves it on with no hit when the LAST blocker on the pill's shell line to its square has 2 shots left or fewer (`DECOY_GETAWAY_BLOCKER_SHOTS`); with 2 or more blockers it holds. The line runs to the square it PARKED on, not the tank's square, so a knock off that square keeps the count, and the step then drives to the next square from where the tank is. The brain counts the shells that stop on that wall: it hears each wall hit (an `EVENT_SOUND` shotBuildingNear on the wall's square), and a full wall is `building_life` + 1 = 5 less the hits, so it moves after 3 hits. The round cannot hear those hits: it logs each wall change with its tick (damaged = hit 1, gone = hit 5). It judges a step when the tank STARTS to move (a square takes 80 to 100 ticks to cross, and a shell can hit it on the way). So it must move only within 400 ticks of a hit, or, after the first step, from a square with no wall on the line or one wall that is not full. A blocker step from a square whose one wall was full at park must come while that wall still stands and at least 12 ticks (two more shells at the fastest fire rate) after it turned damaged. Its first stop off the square must be (129,126), the first square of the chain `DECOY_GETAWAY_SCAN` logs, facing it when hit; if a blocker step takes it on before it stops there, the square that made ring 1 must be (129,126). Each new ring needs a hit since the last one or a blocker step. A DRIFT is no departure: a square change with no stop since the last one that goes no further out is the same move (a tank at speed slides on past the square it aimed for). The decoy is NEVER refilled (the attacker is, below 25 armour, and the pill below 5). It starts with 40 armour and a pill shell takes 5: 8 hits leave it alive at 0 armour, the 9th kills it. A death ends the round on that tick with armour lost 40, and the kill counts as a hit. The verdict line is `PASS decoy_getaway: lost N/40, H hits, dead tT` (or `alive, hold end tT`), then the step count and each step's trigger (`hit` or `blk`). PASS or FAIL judges the step rules only; a death, the number of steps and the armour lost are results, not checks. The log lists every hit (tick@square) and the wall changes. Under the angry pill a step is slow: the knocks push the tank back while it turns, so after the first step the watch counts rings, not squares. |
| `decoy_getaway_keel` | The same round with `cfg=DECOY_GETAWAY=false` handed to the decoy by `bot_init`: it takes hits and never steps. The knocks push it straight south down its column, a few squares under the angry pill; it must never stop off that column (a step goes sideways, behind the walls). It keeps the same `building_life` (4), so the results compare, and gives the same result line (with `no step, knocked to ring N` for the steps). |
| `removed_pill_order` | A pill removed while a bot holds an order on it ends the order. The engine tells a brain which pills are still on the map (`info.pills_on_map`), and the brain drops a removed pill from its own list even when it cannot see the pill go. Seat 2 says `!attack 0` from ten squares out; seats 0 and 1 hear it from twenty-two and twenty-six squares out, past what a tank sees, and the pill is removed on the tick the ack is heard. The holder must say "order lapsed" inside 400 ticks, and no bot may spend a shell in the 1500 ticks after the removal. Without the fix the holder never let go. |
| `removed_base_order` | The base twin: `!capture base N` on the base at (104,112), removed on the ack. The holder must say "order lapsed" inside 400 ticks, and no tank may drive onto the removed base's square in 1500 ticks. Without the fix a bot drove onto the empty square; with only the brain's list fixed, the order still never lapsed, because a base order ended only on the base turning friendly. |
| `removed_dead_pill_pickup` | A dead pill removed while a bot is on its way to pick it up is left alone. `!attack 0` on a dead pill is a pick-up; the pill is removed on the ack. The holder must say "order lapsed", and no builder may be sent to the square and no tank may drive onto it in 1500 ticks. Without the fix a bot drove onto the square. |
| `removed_pill_in_view` | A pill removed while the bots shoot at it is let go at once. Both map bases are removed at setup, so the live pill at (128,128) is the only target; it is removed fifty ticks after the first shell. After a 100-tick grace no bot may spend a shell for 1500 ticks. Without the fix nobody shot at the pill at all: the bots drove to the two removed bases they still had in their lists. |
| `removed_carried_pill` | A pill removed while a tank carries it is not placed and does not come back. The engine refuses to remove a carried pill, so the script drops seat 0's pill on a far square and removes it on the same tick. Seat 0's tank must carry no pill after that, its builder must never go out on a "pill" job in 3000 ticks, and the pill must not come back when the tank is killed. This one passes on both sides of the fix; it guards the engine side. |
