# Tutorial scenario: decisions

One entry per decision the brief left open, per change from the brief, per
number checked in the code, and per knob added. Each entry says what, why,
and how to undo it.

## Engine: `game.popup`

- **What:** a new op `SCN_OP_POPUP` and a new control event `CTRL_SCN_POPUP`,
  appended at the end of the control event enum.
  **Why:** a client that does not know the event skips it, so the wire stays
  compatible without a version bump.
  **Undo:** remove the op row, the server arm and the event.
- **What:** the popup text limit is 511 bytes (`SCN_POPUP_TEXT_MAX`).
  **Why:** Station 2's game-type popup needs more than the 128 bytes a line
  of text gets elsewhere.
  **Undo:** lower the define; the codec and the Lua row read it.
- **What:** a client keeps at most 4 popups waiting (`SCN_POPUP_QUEUE_MAX`);
  a fifth is dropped.
  **Why:** a script that sends popups faster than a player closes them must
  not grow memory.
- **What:** a popup is not replayed to a player who joins later, is not sent
  to spectators, and writes no replay or log record.
  **Why:** a popup is a moment, not state; the log record's text field is
  limited to 255 bytes.
- **What:** a popup counts against the 8 messages a frame.
  **Why:** same budget as `message` and `say`.
- **What:** the popup is shown in the tutorial overlay. It pauses the game
  only where the overlay already pauses: a solo session.
  **Why:** the brief asked for `windowTutorialPause`; that function already
  refuses to pause a game with other people in it.

## Engine: tokens

- **What:** new tokens `{QUICK_TREE}`, `{QUICK_ROAD}`, `{QUICK_WALL}`,
  `{QUICK_PILL}`, `{QUICK_MINE}` read the `kiQuick*` key bindings.
  A controller shows the build-tool glyph; a touch screen shows the words
  "the build buttons" (new string `STR_TUTORIAL_QUICK_BUILD_TOUCH`, 2775; 2763-2774 are taken by the open server-name and mod-meta branches).
  **Why:** neither a controller nor a touch screen has a quick-build key.
- **What:** the token table moved to `src/gui/sdl3/tutorial_tokens.c`, a file
  with no SDL glyph code.
  **Why:** the unit tests can link it.

## Engine: `game.wall_shots`

- **What:** a new read, `game.wall_shots(x, y)`: the shells a wall square
  still takes before it is rubble (5 for an untouched classic wall, what is
  left for a damaged one, 0 for anything else).
  **Why:** Station 3's panel shows the shots left on its target wall, and
  the map only says "wall" or "damaged wall"; the count lives in the sim's
  building list. Counting the player's shells from the script would be a
  guess.
  **Undo:** remove the row in `scenario_lua.c`, `serverSimWallShotsLeft`, and
  the test; Station 3 would then show only "intact / damaged / down".

## Engine: `set_tile` drops wall damage

- **What:** `scenarioWriteTile` (every scripted `set_tile` and fill) now
  removes the square's building list record, as it already removed a mine.
  **Why:** the record holds the shells a hit wall has left. Without this
  change, a wall the script rebuilds (the RESET walls in Stations 5B and 7,
  the Station 3 target wall) keeps the old count and falls early. A record
  for a square the script turned into something else would also follow the
  next wall built there.
  **Undo:** remove the `buildingRemovePos` line and the unit test
  `scenario_funnel_set_tile_resets_wall_damage`.

## Engine: `builder_parachute(p, x, y, from)`

- **What:** `game.builder_parachute` takes a fourth argument, `from`: the
  new man starts that many squares short of his landing square, on the line
  to the nearest map edge, instead of at the edge. 0 (the default) is the
  old behaviour; the range is 0 to 32 (`SCN_PARACHUTE_FROM_MAX`); a `from`
  that is past the edge falls back to the edge.
  **Why:** a dead man's replacement flies in from a random start
  (`lgmKill` uses `startsGetRandStart`) at 3 world units a step, which on a
  long map is most of a minute. The tutorial calls
  `builder_parachute(p, nil, nil, 2)` from `on_lgm_died` for every seat, so
  a new man is back in two to four seconds. The `tutorial_new_man` arena
  measured three deaths: first seen 2 squares from the tank each time, back
  in the tank after 3.38, 2.76 and 3.74 seconds. The fly speed
  (`lgm_helicopter_speed`) is not changed: it is a rule every map shares,
  and with `from = 2` the flight is already short.
  **Undo:** drop the argument from the call; remove `from` from
  `ScnOpLgmParachute`, the Lua row and `scenarioOpLgmParachute`.

## Client: a popup never stays under the round-end screen

- **What:** when the round ends, the client closes the tutorial overlay,
  lets go of its pause, and does not show any popup still waiting
  (`clientSimIsRoundOver`, `tutorialOverlayClose`; in `sdl3imgui.cpp` and at
  the lobby change in `winbolo.c`). Unit test `scenario_popup_round_over`.
  **Why:** the old tutorial ended the round 30 seconds after the win. The
  round-end screen then covered the congratulations popup, and the paused
  popup could not be closed: the game looked frozen.
- **What:** the tutorial no longer ends the round at all. After the win it
  shows the congratulations popup and the status line "Tutorial complete.
  Leave from the menu when you are ready." The player leaves when they want.
  **Why:** the last message must be readable and closable; a round end
  gives the player nothing the menu does not.
  **Undo:** call `game.end_round` from `check_win` again; the client fix
  keeps that safe.

## Map: `data/maps/Tutorial.map`

- **What:** the map is generated by `tools/make_tutorial_map.py`, which also
  rewrites the `LAYOUT` block in the script. Every
  `tests/scenario/maps/tutorial_*.map` is a copy of it.
  **Why:** the script and the map then always name the same squares.
  **Undo:** edit the map by hand and keep the block in step yourself.
- **What:** the seven stations are stacked up one column, Station 1 at the
  south and Station 7 at the north, joined by one road through a forest
  band between each pair.
  **Why:** one long road is easy to follow; the bands keep each station's
  view to itself.
- **What:** there is no deep-water trench. Each checkpoint start is a small
  dock of deep water on the main road at the station's south entrance,
  walled on three sides; a tank respawns on its boat and drives north out
  of it. A player coming up the road goes round the dock on a short road to
  the east.
  **Why:** a start must be on deep sea and every spawn comes on a boat. The
  trench was a long detour west and took the player off the road. Station
  1's popup still warns about deep water ("Deep water sinks tanks. If you
  die anywhere here, you come back at your last checkpoint.").
- **What:** everything the player uses is on or right beside the road.
  The two exceptions are the Station 7 battle (the whole island) and the
  Station 5A island, which the player only watches from the road.
  **Why:** the player should never have to look for where to go next.
- **What:** there are no reset pads. Stations 5 and 7 each have a walled
  RESET pen: the word RESET in walls, a pen with a road entrance and a
  pad, and a road arrow pointing at it; a cyan marker sits on the pad.
  Entering the pen resets the station and rebuilds the pen's walls and
  arrow. Every other station re-arms by itself (see the script section).
  **Why:** small pads were easy to drive onto by mistake; most stations
  need no reset when missing goals are put back.
- **What:** the terrain's box is centred on square 126, so `mapRead` does not
  move it.
  **Why:** the script uses the generator's squares as they are.
- **What:** all pills, bases and starts are written neutral; the script sets
  every owner at the start of the game.
  **Why:** the owners depend on which seats the human and the bots have.
- **What:** Station 1 has five 3x3 patches beside the road: forest, swamp,
  rubble, craters, shallow river. It is done after three.
- **What:** Station 4B/4C is one pillbox east of the road inside a full ring
  of forest, with a 5x5 clearing round the pillbox. The ring covers every
  square within the pillbox's range plus 1.5 squares, so every way in is
  through forest. The ring's west edge touches the road's east edge; the
  road does not cross the ring, and the road is ten squares from the
  pillbox, out of its range.
  **Why:** the player must enter range in forest to learn hiding; a road
  through the ring would be a way in that is not forest. The friendly base
  4C (`b4c`, 127,132) is on the road at the station's north end, 14 squares
  from the pillbox, out of range.
- **What:** Station 5A is an island (the moat stays) with the demo bot's
  start on it; the player watches from the road strip `watch5a`, out of
  range of both pills. Station 5B is west of the road.
- **What:** Station 6 is a parked bot east of the road; its man walks one
  row (`path6`) west across the road and a short span of craters
  (`craters6`, beside the main road) to one tree (`tree6`, 121,80) west of
  the road, and back.
- **What:** Station 7 is a small Chew Toy-like island with no moat and no
  gate: the main road runs straight in from the south through a gap in a
  wall round the island. The player holds three corners (two live pills and
  two bases each in NW and SE; two dead pills and two bases in SW); the bot
  holds the north-east corner: two bases and no pills.
  **Why:** the gate (a teleport) and the moat are gone at the brief's
  request. No enemy pills: the lesson is taking bases with the player's
  pillboxes, and a pill in the bot's corner is one more thing the Easy bot
  defends.
- **What:** friendly bases sit on the road's centre column where the
  player shoots a lot: 2A, 3, 4C, 5 and 6. The status line says "Low on
  shells? Stop on the base by the road to refill." when the tank is low.

## Verified numbers (read in the code)

- **Wall:** 5 shells. The first hit turns a wall into a damaged wall with
  `building_life` (4) left; each later hit takes one; at 0 it is rubble.
  (`shells.c`, `building.c buildingAddItem`.)
- **Pillbox armour:** 15 (`pill_max_armour`), and each shell takes 1
  (`pill_shell_damage`), so 15 shells kill a full pillbox.
- **Pillbox range:** 2048 world units, which is 8 squares (`pill_range`).
- **Pillbox firing:** every 100 pill ticks when calm (`pill_attack_ticks`),
  about 2 seconds. A hit divides the interval by 2 (`pill_angry_divisor`)
  down to 6 ticks (`pill_attack_min_ticks`); every 32 ticks it eases back
  by one.
- **Neutral pillboxes fire** at every tank: the target loop skips only the
  owner and the owner's allies, and a neutral pillbox has neither.
- **Pillboxes do not heal by themselves.** A builder repairs them with
  trees: 1 tree buys 4 armour (`pill_repair_amount`).
- **Forest hiding:** a tank is hidden when its square and the squares it
  overlaps are all forest, and the viewer is at least 3 squares away on
  x or y (`tree_hide_distance` 768). Firing shows the tank for 101 ticks,
  about 2 seconds (`just_fired_ticks`).
- **Base:** full armour 90, a shell takes 5 (`shell_damage`). A base can be
  driven onto and taken once its armour is 9 or less
  (`base_capture_armour`). From full that is 17 shells (90 - 17*5 = 5).
  An enemy shell stops hitting a base at 4 or less (`base_hit_armour`).
  A base adds 1 armour, 1 shell and 1 mine every 1000 ticks
  (`base_regen_ticks`, about 20 seconds). Neutral bases cannot be shot:
  `basesCanHit` refuses a neutral owner.
- **Deep sea:** a tank with no boat drowns the tick it reaches deep sea
  (`tank_deep_sea_safe` 0).
- **Tank:** full armour 40, full shells 40 (`tank_full_*`).

## Script: `data/maps/Tutorial.scenario.lua`

### Player and screens

- **The player** is the first human seat, or the seat named in the global
  `TUTORIAL_PLAYER` (set only by the gate arenas). A player who joins late
  is adopted in `on_player_join`. A player seated on team 2 is moved to
  team 1. *Undo:* drop `TUTORIAL_PLAYER` from `is_player_seat`.
- **Welcome popup:** the first popup, shown at Station 1 before the driving
  popup: what Bolo is, who wrote it, and what pillboxes, bases and the man
  are.
- **Popup timing:** a popup that follows a player action (a goal ticked, a
  station done) waits 3 seconds (`POPUP_DELAY`, 150 frames) so the player
  sees what happened first. It shows at once when the player has already
  moved on to another station. The 4B popup comes 3 seconds after the 4A
  pillbox is built.
- **One marker at a time:** the map shows one marker, on the goal the
  player is on. A marker is removed the moment its goal is ticked. 4B shows
  the ring of red range markers; Stations 5 and 7 also show the cyan pen
  marker.
- **Status line:** a white line says the next step in one short sentence,
  with key tokens (`{FIRE}`, `{QUICK_PILL}`, ...) so it reads right on
  keyboard, controller and touch. The wording says "click", the desktop
  word. Live numbers: shots left on the 2C base, seconds hidden in 4B, bases
  owned in Station 7.
- **Pillbox build wording** is the brief's text with `{QUICK_PILL}`: the 4A
  popup carries all of it; the 4A, 5B and 7 status lines carry "Press the
  pillbox build button or {QUICK_PILL}, then click ...".
- **Station 4B popup** ends exactly "Firing reveals your position."
- **Station 4C popup:** "This pillbox is already badly damaged: one more
  shot kills it." The repair item is gone. The 15-shot fact is in the 4A
  popup.
- **Station 6 popup** says a man cannot be run over (the engine has no
  run-over: a man dies only from shells, mines and blasts), how the
  crosshair works with `{GUN_DOWN}` and `{GUN_UP}`, and to aim at the
  craters. The status line: "Put your crosshair on the craters, then fire
  as the man crosses them."
- **Gun-range reminder:** after the Station 6 kill, if the player's gun
  sight is short of `gunsight_max`, a popup and the status line ask for
  `{GUN_UP}` until the crosshair is at its longest. The reminder clears
  when the sight is at the maximum.
- **The win** is the player owning all eight Station 7 bases. The script
  shows the congratulations popup and the status line "Tutorial complete.
  Leave from the menu when you are ready." It does not end the round (see
  the client entry above). `allow_base_win` is false, so owning every base
  on the map is not a win by itself.

### Checkpoints and loadouts

- **Checkpoint** is the furthest station driven into (`S.reached`);
  driving back south does not lower it. Respawns are at that station's
  dock.
- **First-entry fill:** the first time the player drives into a station
  the tank gets that station's loadout: full, except Stations 1 to 3 carry
  no trees (so Station 3's harvest is needed) and Station 2 starts low (5
  shells, 0 mines, 15 armour) until its refill is done. Respawns get the
  same loadout.
- **2A ticks on a full tank:** the moment the tank reaches the 2A base,
  the `s2stay` popup says to stay on it while the stocks flow in. "Refill
  at your base" ticks only when the tank is full on that base (about 6 to
  10 seconds from the low start), so the 2B popup does not pull the player
  off the base half way. The popup and
  status line say to STOP on the base and wait; the panel says "STOP on the
  base: wait" while the tank moves on a friendly base, then "Refilling:
  wait here", then "Full".
- **2C shot count** comes from the rules: `base_shots(armour)` is the
  shells to bring the base down to `base_capture_armour`; 17 from full
  classic. The status line counts the shots left.

### Re-arming

- **What:** once a second the station the player is in puts back whatever
  a goal still needs while that goal is not ticked: the 2B base neutral and
  the 2C base the Station 6 bot's; the Station 3 target wall and grove; the
  4A dead pillbox home and dead (and picked up again if the tank died
  carrying it); the 4B/4C pillbox home at 1 armour; the 5B target home and
  the 5B blocker in the player's tank, or rebuilt 10 seconds after it died
  once built. The friendly road bases (2A, 3, 4C, 5, 6) are set back to the
  player's and full whenever they are not.
  **Why:** the reset pads are gone; a player who loses a goal's object
  must never be stuck.
- **RESET pens** (Stations 5 and 7) put the whole station back: pills,
  bases, the pen walls and arrow road, the checklist (5A "watch" is kept),
  the loadout; Station 7 also sends the bot to its start with a full tank.
  The `tutorial_reset` arena checks Station 4 re-arming and both pens.

### Bots

- **Reserved seats, late fielding.** The lobby holds one bot seat on team 1
  (the 5A demo bot, the player's ally) and two on team 2 (the Station 6 bot
  and the Station 7 bot), all `fielded = false`. As the round starts the
  script picks out a seat for each role (`reserve_seats`), so a seat can
  own things before its bot is fielded: the 2C base and the 5B target
  belong to the Station 6 bot's seat from the start. Each bot is fielded
  with `game.spawn_bot` the first time the player drives into its station.
  Where the lobby held no seats (a host trimmed them, or a `-nolobby`
  server), a free seat is used with the role's team.
  **Note:** a held, unfielded seat has no tank: `game.tank(p)` is nil and
  nothing of its is on the map until it is fielded. The `tutorial_late_bots` arena
  checks all of this.
- **"bot3" in the brief** is the Station 6 bot.
- **The 5A demo bot** is on team 1, the player's ally. `can_hit` stops tank
  shells between it and the player both ways. `can_build` keeps its man on
  `island5a`, and `can_capture` lets it pick up only the two 5A pills.
  After each take the script puts the target back, gives the bot its
  blocker pillbox again and sends it home; a watchdog restarts a take that
  has stalled for 150 seconds. It plays at Hard, as a demonstration.
- **The blocker take (5A), and a conflict with the brief.** The brief asked
  for the blocker orthogonally next to the target, in line, with the tank
  shooting past it. In this engine a tank shell hits any live pillbox on
  its square, its own side's too. Probe runs of `tutorial_take5a` with the
  script moving the bot's blocker to the square in line (147,102): the bot
  parked at (151,106) and its own shells killed the blocker (15 to 0) while
  the target lost 2 armour; then the bot died beside the target. Left to
  itself, GoalHunter builds its blocker next to the target, mostly
  diagonal at (148,102) and sometimes in line at (147,102), parks at
  (150,108), and kills the target in about 4 seconds while the blocker
  soaks up the target's fire (15 to 0); the first take came 21.9 seconds
  after fielding. **Decision:** the demo bot builds its blocker itself, on
  a square of its own choosing next to the target; the script no longer
  moves it. The arena logs where it was built, every hit on the target
  with the bot's square, and passes when the bot built its blocker next to
  the target, the blocker took fire, and the take came within 150 seconds.
  The 5B player's blocker stays in line at (117,102), with the park square
  (119,104) diagonal to it, where the player's shots at the target's side
  pass the blocker by.
- **The Station 6 bot** has `peace=<player seat>` so it does not fight.
  It is parked: teleported to `bot6_park` with `set_modifiers{speed = 1,
  accel = 1, turn = 1}`, and every frame put back on the square's centre if
  it moved at all. (A modifier of 0 is read as the classic 100; 1 still
  creeps.) Its tank cannot die (`can_die`). The `tutorial_station6` arena
  measured 6 world units at most per engine step and 0 after every tick
  over 2.5 minutes.
- **The Station 6 man** walks to one tree west of the road and back. While
  he is in the tank the script puts the tree back, empties the tank's trees
  and orders him to that tree; every second it also rebuilds his row
  (road, and the crater span beside the main road). The arena logged 13
  trips in 2.5 minutes, all over the same squares in the same order, all
  across the craters, none off the row.
- **Station 6 kills** count a death of the Station 6 man with the player
  as the killer.
- **The Station 7 bot** is an Easy GoalHunter on team 2. It may build only
  inside `island7` and never a boat. With no moat or gate, it is sent back
  to its start the frame its tank leaves `island7` (checked every frame;
  once a second let it get out through the road gap). The
  `tutorial_bot7_region` arena watches it for two minutes.
- **The Station 7 bot never kills the player's man.** GoalHunter has no
  knob that stops it shooting men only: `peace=` spares the tank and the
  man together, which would leave no opponent. So the bot gets
  `cfg=CAPTURE_LGM_HUNT=false` (no hunt for a man building near a pill it
  wants), and `can_die("builder")` refuses any death of the player's man
  with the Station 7 bot as the killer. The `tutorial_bot7_man` arena kept
  the player's man working beside the bot for three minutes: 29 deaths
  refused, 0 by the bot.
- **New man close by:** `on_lgm_died` calls `builder_parachute(p, nil,
  nil, 2)` for every seat (see the engine entry).

### Other

- **5A "watch"** ticks when a take happens while the player is in
  Station 5.
- **5B:** 3 seconds after the player picks up the enemy pillbox it is put
  back, owned by the Station 6 bot's seat, at full armour, with an
  announcement.
- **4B hiding** mimics the engine's rule: the tank's square and the squares
  it overlaps all forest, and 3 or more squares from the pillbox on x or
  y; 5 seconds hidden in range ticks the item. Firing is not tracked: the
  popup says firing reveals the tank, and the pillbox then shoots it. The pillbox starts at
  1 armour, and `can_die` refuses its death until "Sit hidden in range" is
  ticked ("Hide in the forest first, then kill it.").
- **A pillbox taken out of its station** is sent home when the player
  enters another station carrying it, and one put down outside its station
  is sent home at once. *Why:* stations must not affect each other.
- **The Station 2 game-type popup** says a little more than the brief for
  Tournament: "the more bases are taken, the fewer shells a tank respawns
  with; once all bases are taken, tanks respawn without shells". *Why:*
  `gameTypeGetItems` gives 2 x 40 x (neutral bases / bases) shells, so the
  drop is gradual. *Undo:* cut the first clause in `POP.s2d`.
- **"Pick up both dead pills"** counts each Station 7 dead pillbox once it
  has been picked up, so building the first before picking up the second
  still ticks the item.
- **Gate arenas** hold the player's seat (a GoalHunter in the gate) still
  with `set_modifiers{speed = 1, turn = 1}` and teleport it about. Eight
  arenas: `tutorial_checkpoint`, `tutorial_reset`, `tutorial_take5a`,
  `tutorial_late_bots`, `tutorial_station6`, `tutorial_bot7_region`,
  `tutorial_bot7_man`, `tutorial_new_man`. The Station 2 steal is not
  covered: a held-still tank cannot drive onto a base.
