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
- **What (changed 2026-10-10, item 20, from Andrew's play-test; replaces
  the one-square pockets of item 18 below):** every checkpoint start is the
  centre of a 3x3 pool of deep water, and the tank faces north there
  (map-file dir 4; the engine turns that into its own 0, north, so
  `game.start(n).dir` and the respawned tank's `dir` both read 0). Walls
  close the pool on the west, east and south, one square out. On the north
  the pool's middle column opens onto one road square, with a wall on each
  side of it, and that road runs north into the main road. The tool's
  `pool()` makes the pool; `dock()` places one for Stations 2 to 6.
  - cp1 (127,226): directly south of the main road's south end, in line
    with it. The pool takes rows 225..227 and its walls rows 224..228 (228
    is the frame), so the main road stops at row 223 and the pool's road
    square (127,224) runs straight into it.
  - cp2 to cp6: x 123, y 199 / 176 / 155 / 124 / 89, three squares west
    of the main road's west column; the south wall is on the station's
    south row. The road goes north two squares from the pool (rows y-2 and
    y-3), then east along row y-3 into the main road (x 123..125).
    Footprint x 121..125, rows y-3..y+2.
  - cp7 (112,51): the same pool in the island's SW quarter, replacing the
    1x2 water pocket in a forest clump; its road square (112,49) opens onto
    the island's road.
  **Why the road goes north two squares before it turns:** the road square
  next to the pool must be the only way in. A road turning east on the
  first row would run along the top of the pool's east column, and a tank
  could drive into the pool there, away from its boat.
  **No drowning:** the tank respawns on its boat. It can only leave through
  the pool's middle north square, so the boat stays there, on the square a
  tank coming back drives in on; it boards the boat and does not drown.
  **Neighbours checked:**
  - Station 2: cp2's road row 196 touches the 2A base patch (rows 193..195),
    so a respawned tank can drive straight onto its own base. It is
    outside `b2a_area`.
  - Station 3: cp3's road row 173 touches the grove's south edge (row 172);
    the grove is not cut.
  - Station 4: cp4's road row 152 is inside `p4a_area` (its south ring
    row), which the script declares but never reads. The dead 4A pill
    (122,149) is 6 squares from cp4; it never fires (dead at home, or in
    the tank).
  - Station 5: the pool's west wall (x 121) would cut the 5B RESET arrow,
    so `walled_reset` takes an `arrow_dx` and 5B's arrow moves two squares
    west to x 117..120 (still under the pen's road, still pointing at the
    gap). cp5's road (x 123..125, row 121) joins the RESET pen's road (rows
    119..120). The 5B popup region `s5b` now ends at x 122 (was 124) and
    row 122, so the pool and its road lie outside it and a respawn at cp5
    does not count as driving into 5B. The 7 RESET arrow is unchanged.
  - Station 6: nothing within 4 squares; the craters and the man's path
    are on row 80, the base b6 on the main road at row 84.
  - Station 7: the pool's walls (x 110..114, rows 49..53) sit between the
    forest rim and the 2 bases; the RESET strip starts at row 54.
- **What (changed 2026-10-10, item 18):** there is no deep-water trench,
  and the main road (x 126..128) runs straight and clear up the centre of
  the map. Each checkpoint start of Stations 1 to 6 is a one-square pocket
  of deep water three squares west of the main road, one row above the
  station's south edge: x 123, y 226 / 200 / 177 / 156 / 125 / 90 (cp1 to
  cp6). Walls close it on the west, north and south. The start faces east,
  and the pocket has its own road: two squares east to the main road
  (x 124..125), and one road square north-east of its end (x 125, one row
  up), so the merge bends north the way the main road goes. Before, each
  start was a 3x2 pool on the main road itself (x 127), and a player coming
  up the road had to go round it on a bypass to the east; the bypass is
  gone. The Station 7 start (cp7, 112,52) is unchanged: it was already off
  the main road, in a forest pocket on the island next to the island's road.
  **Why one square:** the boat a tank leaves behind stays on the only water
  square, so a tank that drives back in from the road boards it and does
  not drown. **Why one row above the south edge:** in Station 5 the RESET
  pen's road arrow reaches (122,123); a pocket one row higher would wall it
  over. No base, pen, crater row or goal region is within 3 squares of a
  pocket. The 5B popup region `s5b` now ends at row 122 (was 126), so a
  respawn at cp5 does not count as driving into 5B. The only pill within
  range of a pocket is the dead 4A pill (122,149), 7 squares from cp4; it
  never fires (it is dead at home, or in the tank).
  **Why a start at all:** a start must be on deep sea and every spawn
  comes on a boat. The trench was a long detour west and took the player
  off the road. Station
  1's popup still warns about deep water ("Deep water sinks tanks. If you
  die anywhere here, you come back at your last checkpoint.").
- **What:** everything the player uses is on or right beside the road.
  The two exceptions are the Station 7 battle (the whole island) and the
  Station 5A island, which the player only watches from the road.
  **Why:** the player should never have to look for where to go next.
- **What:** there are no reset pads. Stations 5 and 7 each have a walled
  RESET pen: the word RESET in walls, a pen with a road entrance and a
  pad, and a road arrow pointing at it. There is no marker on the pad.
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
- **What (2026-10-10):** Station 5A is a grass island east of the main
  road, x 135..140, y 104..117, inside a wall ring, a one-square moat of
  deep water and a second wall ring. The target T is at (137,105). The
  player watches from `watch5a` (131,111), the end of a short road spur
  off the main road beside the outer west wall: 8.5 squares from T, out
  of its range, with the take on screen. Station 5B is west of the road.
  **Why the moat:** with walls only, GoalHunter's route finder counts a
  wall as a few shots' work, so every goal off the island was in reach.
  Probe runs: the bot shot its way out after the dead pills in Station 7
  (127,70)/(127,68), the 4A pill (122,149), and to drop its blocker in 5B
  (`place_pill_strategic`), and never took T. Deep water without a boat is
  no route, so with the moat nothing off the island is a goal. The inner
  walls keep the bot off the moat's edge, so it cannot drown; the outer
  walls keep the player's tank off it.
- **What:** Station 6 is a parked bot east of the road; its man walks one
  row (`path6`, y 80) west across the road and a span of craters
  (`craters6`, x 121..124) to one tree (`tree6`, 115,80), and back.
  **Changed 2026-10-10:** the tree moved 6 squares west (was 121,80), so
  the craters lie between the road and the tree. The man crosses the same
  craters going out and coming back; there is no other way along the row.
  The craters are 2 to 5 squares west of the road's west edge, so a
  crosshair at its longest range reaches them from the road.
- **What:** Station 7 is a small Chew Toy-like island with no moat and no
  gate: the main road runs straight in from the south through a gap in a
  wall round the island. The player holds three corners (two live pills and
  two bases each in NW and SE; two bases in SW); the bot holds the
  north-east corner: two bases and no pills. The two dead pills
  (`p7_sw1`, `p7_sw2`) lie on the centre of the main road in, two squares
  apart (changed 2026-10-10; they were beside the road in the SW corner).
  **Why:** the gate (a teleport) and the moat are gone at the brief's
  request. No enemy pills: the lesson is taking bases with the player's
  pillboxes, and a pill in the bot's corner is one more thing the Easy bot
  defends.
- **What:** friendly bases sit on the road's centre column where the
  player shoots a lot: 2A, 3, 4C, 5 and 6.
  The Station 5 base (`b5`) is at (127,110), just north of the 5B RESET
  letters: their top row is 112, and row 111 is a clear road square
  between them (item 34, 2026-10-10; was 127,97 at the north end). The
  park spur (row 107) and the checkpoint pool are clear of it. The status line says "Low on
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
  the ring of red range markers. The RESET pens in Stations 5 and 7 have
  no marker (removed 2026-10-10 at Andrew's request); the word RESET in
  walls and the road arrow show them.
- **Status line:** a white line says the next step in one short sentence,
  with key tokens (`{FIRE}`, `{QUICK_PILL}`, ...) so it reads right on
  keyboard, controller and touch. The wording says "click", the desktop
  word. Live numbers: shots left on the 2C base, seconds hidden in 4B, bases
  owned in Station 7.
- **3 wall status line (2026-10-10, item 27):** "Build a wall: Click on the
  wall button\nor press {QUICK_WALL}, then click a grass square." On a
  keyboard the two rows are 38 and 37 characters. With a controller
  ("the next tool button") the second row is 57 and on touch ("the build
  buttons") 54; the longest controller row before was 52 (3 harvest). Kept
  as Andrew asked, since the keyboard is the case he balanced; the box
  sizes to the text, and a 57-character row is still well inside the game
  view.
- **3 shoot-the-wall marker is green (2026-10-10, item 28):** the target
  wall's marker is the same green square as 2A and the 5B blocker spot.
  It shows exactly while "Shoot down the wall" is the station's first
  unticked row, which is also when the status line names it: both come
  from `current_goal(3)`, so out of order play cannot split them. (The
  low-shells line, when it shows, takes the status line's place.)
- **Status line width (2026-10-10):** a status line that was wider than
  about half the game view is now two rows, split with `"\n"`. The client
  draws each row centred in one box (see SCENARIO_API.md, "Line breaks").
  The 5B, 6 and 7 lines are split this way.
- **The popup first, its white text after (item 37, 2026-10-10).**
  Andrew: "popup needs to come first" - the popup explains, the white text
  keeps the player on track. Before, a goal ticked, the status line moved
  to the next goal at once, and the next goal's popup came 3 seconds
  later (`POPUP_DELAY`); the "Station N done" line also went out before
  the popup queued with it. Now, while a popup waits in the script's queue
  (`S.queue`):
  - the status line is blank (`draw_status`);
  - every announcement waits with it (`say`, `S.says`), and goes out in
    the tick the queue empties, after the popup;
  - `mark` queues the next popup before it says "Station N done", so the
    line waits for that popup.
  All the script's announcements go through `say`: "Station N done",
  "Station N reset", the 5B rebuild line, "The enemy pillbox is back",
  "Moved to the green square", "Hide in the forest first". A popup shown
  at once (station entry, s2stay, s5b) goes out before the status line in
  the same tick. *Popup close:* the server cannot see a popup close, and
  no new call was needed: the client draws no white text while a popup is
  open and starts a line's time when it closes (item 26). The
  `tutorial_popup_first` arena checks the server's order through Station
  2 (entry, s2stay, s2b, s2c, s2d and "Station 2 done"): no non-blank
  status line or announcement goes out while a popup waits. It fails on
  the old script.
- **Never white text and a popup at once (item 26, 2026-10-10).** The
  client holds the announcements while its popup overlay is open (a
  `game.popup` or a tutorial message): no announcement and no status line
  is drawn, and the announcements' clocks stand still. A line sent in the
  same tick as a popup, or while it is open, is shown for its whole time
  after the popup closes; a line already up keeps what it had left; a line
  already out stays out. A round-end popup's line waits the same way: the
  round end closes the popup, and the line then shows. The script does
  send pairs in one tick: `mark` shows a waiting popup at once and then
  says "Station N done" (`tutorial_popup_announce` proves it). Engine:
  `clientSimScnAnnounceHold` (client_sim.c), called by
  `renderScenarioAnnounce` (sdl3imgui.cpp) every frame. The gate runs the
  server only, so the hold is proven in the unit suite
  (`scenario_announce_waits_for_popup`).
- **Long key names in popups (item 36, 2026-10-10).** Andrew: "Now press
  k until your range is extended" named the wrong key. His range keys are
  Keypad . and Keypad Enter. A key with no picture in the key pack is drawn
  as a cap with its name, and that cap drew only the first letter. Now:
  - the cap in a popup grows to fit its whole name
    (`drawProceduralKeycapWideAt`), and the line wraps on its real width;
  - a keypad key is written "Num ." / "Num Enter" / "Num +", in a popup
    cap and in a status or announcement line alike
    (`tutorialShortKeyName`); other names stay as SDL gives them;
  - keypad * and / use the pack's Asterisk and Slash pictures. The pack
    has no picture for the keypad's "." or ",". Keypad Enter already used
    the Enter picture.
  The Configure Keys dialog keeps its square cap: it writes the full name
  beside it. Unit test: `tutorial_tokens_long_key_names`.
- **Announcements stack (2026-10-10):** a second `game.announce` no longer
  replaces one still up. The client holds up to four, each with its own
  countdown, and draws the newest lowest. An empty line clears them all.
  *Why:* the tutorial puts a "done" line and a follow-up line close
  together; before, the second wiped the first.
- **Pillbox build wording** is the brief's text with `{QUICK_PILL}`: the 4A
  popup carries all of it; the 4A, 5B and 7 status lines carry "Press the
  pillbox build button or {QUICK_PILL}, then click ...".
- **Station 4B popup** ends exactly "Firing reveals your position."
- **Station 4C popup:** "This pillbox is already badly damaged: two more
  shots kill it." (2026-10-10, item 29: "and it shoots back" dropped.) Two
  shots (`P4_ARMOUR`), not one, so the player feels being shot at. The
  "Station 4 done" line adds "Shoot trees to clear the trees quickly." The
  repair item is gone. The 15-shot fact is in the 4A popup.
- **Station 6 popup** says a man cannot be run over (the engine has no
  run-over: a man dies only from shells, mines and blasts), how the
  crosshair works with `{GUN_DOWN}` and `{GUN_UP}`, and to aim at the
  craters. "A shell explodes at your crosshair" (item 35, 2026-10-10; was
  "bursts"). The status line: "Put your crosshair on the craters, then fire
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
- **2A refill is not enforced (changed 2026-10-10, item 21, Andrew: "I
  want to tell them to do it, but don't want it enforced"):** the telling
  stays: the `s2a` popup, the `s2stay` popup the moment the tank reaches
  the 2A base, the 2A status line ("Drive onto the green-marked base, then
  STOP on it and wait while it refills.") and the green marker on 2A.
  "Refill at your base" ticks on the first of: the tank is full on the 2A
  base; the tank drives off the 2A base after being on it (`S.on_2a`); any
  later Station 2 goal ticks (2B taken, 2C shot or taken). So nothing waits
  on a full tank: the player may stop half way, or skip the base and take
  2B first, and the station goes on. The 2B popup, the 2B marker and
  status line, the 2C goals and "Station 2 done" follow from the tick as
  before.
  **Why this shape and not a "suggested" row:** the status line and marker
  name the first unticked row. A row that never has to tick would keep the
  status line on "STOP and wait" while the player took 2B. Ticking on
  leaving the base keeps the telling while the tank is there and moves on
  the moment the player does.
  The panel still says "STOP on the base: wait" while the tank moves on a
  friendly base, then "Refilling: wait here", then "Full". The 5-shell
  start (see "First-entry fill") ends when 2A ticks, so a later respawn in
  Station 2 is full. The low-shells status line ("Low on shells") still
  waits until 2A is ticked, so it does not cover the 2A line.
- **2C shot count** comes from the rules: `base_shots(armour)` is the
  shells to bring the base down to `base_capture_armour`; 17 from full
  classic. The status line counts the shots left.
- **"Enemy base: N shots left" waits for 2B (2026-10-10, item 23):** the
  panel's yellow info line for the 2C base shows only once "Take the
  neutral base" is ticked (`S.done[2].b_take`), and not after 2C is shot
  empty. Before 2B the line is not drawn at all. *Why:* Andrew: it should
  "only appear once you've taken the neutral base"; before that the enemy
  base is not the goal.
- **Neutral, not grey (2026-10-10, item 22):** player text calls the 2B
  base neutral. The `s2b` popup is exactly "Neutral bases don't have red or
  green and can be claimed by driving over them."; the row is "Take the
  neutral base"; the 2B status line is "Drive onto the neutral base to make
  it yours." The marker colour on 2B is still the "grey" draw colour.
- **No "the panel" in player text (2026-10-10, item 24):** Andrew: the
  panel is self-evident and naming it is odd. Gone: "The panel says Full
  when your tank is full." (`s2stay`) and "The panel counts the shots
  left." (`s2c`). No other popup, status or announce line names the panel.
- **`s2d` wording (2026-10-10, item 25):** it opens "What your tank spawns
  with depends on game type." and ends "Tournament modes have a clear win
  condition: Once one team owns all the bases, they win, because the enemy
  team can't get any ammo." The Open / Tournament / Strict lines between
  are unchanged.

### Re-arming

- **What:** once a second the station the player is in puts back whatever
  a goal still needs while that goal is not ticked: the 2B base neutral and
  the 2C base the Station 6 bot's; the Station 3 target wall and grove; the
  4A dead pillbox home and dead (and picked up again if the tank died
  carrying it); the 4B/4C pillbox home at 2 armour (a hit taken in 4C stays); the 5B target home and
  the 5B blocker in the player's tank, or rebuilt after it died once
  built (2 seconds, see "5B blocker rebuilt"). The friendly road bases (2A, 3, 4C, 5, 6) are set back to the
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
  shells between it and the player both ways, and the two 5A pills' shells
  at the player. `can_build` keeps its man on `island5a`, and
  `can_capture` lets it pick up only the two 5A pills. After each take the
  script puts the target back, gives the bot its blocker pillbox again and
  sends it home; a watchdog restarts a take that has stalled for 150
  seconds. It plays at Hard, as a demonstration.
- **"Bot 2" is the 5A demo bot (item 17, 2026-10-10).** It is on the field
  only while the player's tank is in Station 5. The script fields it when
  the tank is in Station 5 and removes it (`game.remove_bot`, which also
  frees the lobby seat's tank) after about a second outside; the two 5A pills
  go home first. It has `BOT_PINGS_DEFAULT=false`, so it sends no ally
  pings. *Why:* Andrew found its pings annoying when he was elsewhere.
  The `tutorial_station5_entry` arena checks it on entry, on leaving and on
  re-entry.
- **The 5A demo bot's knobs (items 3 and 4, 2026-10-10).** Its init table
  sets `cfg=BLOCKER_ORTHOGONAL_ONLY=true`, `cfg=BOT_PINGS_DEFAULT=false`
  and, under a second key `cfg2` (an init value is at most 63 bytes),
  `cfg=STRATEGIC_PLACE_ENABLED=false`.
  - `BLOCKER_ORTHOGONAL_ONLY` is a new GoalHunter knob (default false, and
    false in `PRESETS.keel`). On, the bot builds its blocker only on a
    square orthogonally next to the target (Manhattan distance 1), never
    diagonal. The brief asked for this.
  - `STRATEGIC_PLACE_ENABLED=false` stops the bot dropping its carried
    pill on a square of its own choosing between takes. Without it, soak
    runs saw blockers off the target's side and one death.
  - Once a second the script tops the bot up to a full tank when its
    armour is under 30 or its shells under 10. GoalHunter will not start
    an attack carrying a pill with armour under 30 ("approach abort:
    carrying pill, armour 15 < 30"), so without the top-up it stalled.
  - **Measured:** `tutorial_take5a_soak` runs the demo for 9 minutes: 18
    takes, every blocker orthogonal, 0 bot deaths, 0 ticks off the island.
    `tutorial_take5a` and 7 runs with other start ticks all passed.
- **The blocker take (5A).** The bot builds its blocker itself, on the
  square orthogonally next to the target that it chooses
  (`BLOCKER_ORTHOGONAL_ONLY`), then parks behind it and kills the target
  while the blocker soaks up the target's fire. The script does not move
  the blocker. (The first build let the bot pick a diagonal square too;
  the brief asks for orthogonal, so the knob was added.)
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
- **The Station 7 bot shoots at the player's man and mostly misses
  (corrected 2026-10-10).** The first build made the man invulnerable to
  this bot: `can_die("builder")` refused any death of the player's man with
  the Station 7 bot as the killer. That was wrong. Andrew wants the bot to
  look like it is trying to kill the man but is not very good at it. The
  `can_die` refusal is gone, and the man can die to this bot.
  - **What replaced it:** two new per-bot GoalHunter knobs, set for this
    bot only through its init table: `cfg=LGM_MISS_WU=256`,
    `cfg=LGM_HIT_PCT=10`, `cfg=CAPTURE_LGM_HUNT=false`. They act only on
    the kill_lgm aim point (shots at a man). In 10 percent of aim blocks
    (12 thinks each) the bot aims true. In the rest the aim point moves
    256 world units (one square) off the man: behind him, within 45
    degrees, when he walks (a late shot), and any of 16 directions when he
    stands. The bot still turns to him and fires; the shell bursts about a
    square from him. A man dies within 128 wu of a burst. The draw is a
    hash of the tick block, the target and the bot (no `math.random`), so
    a seed replays exactly. The defaults, `LGM_MISS_WU = 0` and
    `LGM_HIT_PCT = 100`, are the old aim, and `PRESETS.keel` holds them.
    Easy's own handicaps still apply (`AIM_ERROR_BRADS = 4`,
    `FIRE_HOLD_TICKS = 16`, `REACTION_DELAY_TICKS = 20`).
  - **Why new knobs:** Easy's `AIM_ERROR_BRADS = 4` turns the aim by up to
    about 5.6 degrees, about 100 wu at four squares, which is inside the
    128 wu kill radius. With only that, Easy killed the man with 20 and 29
    percent of its shots at him (seeds 42 and 7).
  - **CAPTURE_LGM_HUNT stays off** (Easy already has it off). That hunt
    fires on its own line, which the miss knobs do not reach, so turning it
    on would give the bot accurate shots at a man near a pill.
  - **Measured** (`tutorial_bot7_man`, six minutes of the man walking out
    to build road two and three squares from his tank, the bot told not to
    fight tanks and its shells passing through the player's tank so only
    shots at the man count): hits / shots at the man, seed 42: 8/72
    (11.1%); seed 1: 10/71 (14.1%); 2: 5/100 (5.0%); 3: 7/68 (10.3%);
    4: 6/76 (7.9%); 5: 9/90 (10.0%); 6: 4/70 (5.7%); 7: 8/80 (10.0%);
    8: 3/59 (5.1%). All nine seeds: 60/686, **8.7 percent**. Other values
    tried: 192 wu any direction at 10% gave about 19%; 256 wu behind at 5%
    gave 6.3%; 224 wu behind at 10% gave 9.5% but 3.1 to 17.3% per seed.
    The arena passes when 5 to 15 percent of at least 40 shots at the man
    kill him and `can_die` refuses no death of the man.
  - **Arena set-up (2026-10-10, with item 18):** the arena now also puts
    the 5B blocker on its square, live and the player's, as a player who
    reaches Station 7 has built it. Left dead there, it drew the bot down
    the main road (`capture_pill` on 117,102 for most of the run; the script
    sends the bot back each time it leaves the island), and with the item 18
    map the seed-42 run had only 31 shots at the man. After the change:
    seed 42 7/74, seed 1 5/44, seed 2 6/60, seed 3 5/68.
  - **Off-island goals (closed 2026-10-10, item 19):** the bot used to
    pick goals off its island when they were cheap enough (the dead 4A
    pill, the neutral 2B base, the 5B pills, the two dead pills on the
    road in). The script sent it back each time, but it lost time trying.
    The new GoalHunter knob `STAY_AREA` (next entry) ends it.
  - **Arena changes with STAY_AREA:** the arena now runs ten minutes (was
    six; GATE `ticks=62000 limit=40`), and its pass floor is "at least one
    kill" in place of "at least 5 percent". Its own TANK_COMBAT_ENABLED
    value moved from init key `cfg2` to `cfg3`, because `cfg2` now holds
    the tutorial's STAY_AREA. Measured, seeds 42 and 1 to 7: 11/129
    (8.5%), 8/115 (7.0%), 2/57 (3.5%), 5/89 (5.6%), 11/128 (8.6%), 2/50
    (4.0%), 14/137 (10.2%), 14/130 (10.8%). **Why the floor moved:** at
    about 9 percent, 50 to 60 shots give only 1 or 2 kills often enough by
    chance; seeds 2 and 5 failed the 5 percent floor with 2 kills each.
    The ceiling (15 percent) and the 40-shot minimum stay. Without the
    knob the same arena (seed 42) gave 2/31 and the bot chased the dead
    5B blocker (117,102) for most of the run.
- **The Station 7 bot has `STAY_AREA` = the island7 region** (2026-10-10,
  item 19). `STAY_AREA` is a new per-bot GoalHunter knob
  (`brains/GoalHunter/constants.lua`): `"x1:y1:x2:y2"` in map squares,
  inclusive, several rectangles joined with `/`. (`,` and `;` cannot be
  used: the cfg token splitter eats them.) `""` is off, the old behaviour,
  and `PRESETS.keel` holds `""`. The tutorial sets it under init key
  `cfg2` (`"0;cfg=STAY_AREA=107:27:149:67"`), built from
  `LAYOUT.region.island7`, so it follows the map tool.
  - **What the knob does**, all only while it is set:
    - goal pools (`goals.lua`): every candidate outside the area is a
      reject row with the reason `outside STAY_AREA` (refuel, capture_base,
      capture_pill, repair_pill, attack_pill, attack_base, and defend_pill
      through the finalize pass), so a pool's best is always inside;
    - goal selection: every pool row whose goal square is outside (also
      attack_tank, kill_lgm, take_cover, flee, place, explore and the rest)
      is priced at the reject sentinel with the chip
      `REJECT outside STAY_AREA`;
    - `pick_goal` guard: a goal picked ahead of the pool (commands, sea
      harvest, special modes, explore fallback) that is outside becomes an
      explore goal to the nearest inside square;
    - explore frontier: squares outside are never explore targets;
    - the man (`builder.decide`): no build, road, farm, mine, pillbox or
      repair order outside, no tree outside;
    - attack plans: no standoff and no shield shot spot or blocker square
      outside (the shield scan then runs in Lua, as with
      BLOCKER_ORTHOGONAL_ONLY);
    - path planning: a one-square ring outside the area costs 32767 in the
      bot's own overlay (A* and the Dijkstra slates read it). Engine
      terrain never changes. While the tank is outside, the ring is off so
      it can get back in.
    Steering reflexes (swerve, deep-water and edge avoidance) are not
    touched.
  - **The leash stays, as a backstop.** The knob changes only the bot's
    own choices; a shove or a gap in the planner could still put the tank
    off the island, and the leash costs nothing while the bot stays on.
    `tutorial_bot7_stay` counts the leash: three minutes, seeds 42, 1, 2,
    3: leash 0, tank ticks off the island 0, builder ticks off 0. With the
    knob removed (seed 42) the leash fired 9 times; the bot chased the two
    dead pills on the road in, (127,68) and (127,70).
  - **Not used for bot5a or bot6.** bot5a's island is 6 squares wide, so
    a standoff ring of the take would fall mostly outside it and the knob
    would change the take the player is taught; its moat and home rule
    already hold it. bot6 is held by the script and never fights.
- **The Station 7 bot cannot pick up the two dead SW pills** (`can_capture`,
  2026-10-10). Before, it drove out of its corner to take them off the
  road before the player got there.
- **New man close by:** `on_lgm_died` calls `builder_parachute(p, nil,
  nil, 2)` for every seat (see the engine entry).

### Other

- **5A "watch" is told, not forced (item 30, 2026-10-10).** Andrew:
  "Don't force them to watch from the exact spot." "Watch the bot take one"
  ticks 10 seconds after the player's tank first comes within 5 squares of
  the green square `watch5a` (131,111), counted as the larger of the x and
  y distances (`WATCH_NEAR` = 5, `WATCH_NEAR_FRAMES` = 500 on_tick calls).
  The tank need not stay there: the timer runs on once started. The status
  line and the popup still name the green square. The `tutorial_watch5a`
  arena holds the tank 7 squares off for 15 seconds (no tick, no timer),
  then 4 squares off (ticks after 10.20 s). There is no call to move a
  player's view (`game.view`), so the watch spot is chosen so the island
  is on screen from it: T is 6 squares east and 6 north.
- **5A rings and moat put back on each demo reset (item 31, 2026-10-10).**
  Every time the 5A demo is reset (`reset_5a`, and when the demo bot
  spawns), `rebuild_5a_rings` puts back each square of both wall rings
  (`LAYOUT.walls5a`, 104 squares) and the moat (`LAYOUT.moat5a`, 52
  squares). A wall is rewritten when it is not a full wall (not building,
  or damaged); a moat square when it is not deep sea. The map tool works
  both lists out from the grid. *Why:* the demo bot and the player shoot
  the rings; a gap lets the bot off its island. `tutorial_watch5a` breaks 9
  squares (rubble, crater, grass, road) and checks all of them come back.
- **5B blocker in the tank at once (item 8, 2026-10-10).** The 5B blocker
  is put in the player's tank (`hand_5b`) the moment 5A is ticked, on
  every entry to Station 5 and on every respawn there, while "build" is
  not ticked. Before, it came with the next once-a-second re-arm. The
  `tutorial_park5b` and `tutorial_station5_entry` arenas check it.
- **5B parking square (item 32, 2026-10-10; was item 9).** Andrew: "4
  tiles right ... and one tile more north" of the old square (117,108),
  corrected to (121,107). The target stays at (117,101). The parking
  square (`t5b_park`, 121,107) is 4 east and 6 south of it, joined to the
  main road by a road spur along row 107. From the tank's centre the
  target's centre is 7.21 squares away and its near corner 6.52; a tank
  shell flies 7.125 squares, so a shot aimed at the target's centre ends
  inside its square. *Choice:* the blocker moves from (117,102) to
  (118,102), the square south-east of the target, because the line
  between the two centres now runs on the diagonal: it crosses (118,102)
  for 0.9 of a square, and does not cross (117,102). So the target's shots
  at the parked tank hit the blocker. The scripted shooter
  (`tests/brains/tutorial_shoot_north.lua`) aims at 232 (0 = north,
  clockwise). `can_hit` lets the player's own shells pass the player's
  blocker; in a real game they would hit it. *Note:* the firing client
  still draws the shell stopping at the blocker; the server lets it
  through.
- **5B blocker takes full damage (item 33, 2026-10-10).** Andrew: the
  blocker "repairs silently and is too strong". The sources found:
  - `pill_damage_scale` let only every fifth target shell at the blocker
    count: four of five hits did nothing. This was the cause. **Removed**,
    with its tooltip row (the script now has 20 callback rows).
  - `on_pill_placed` sets a blocker placed with armour 0 to full armour.
    This happens only when the player's man builds it, so it is the build,
    not a repair.
  - The once-a-second re-arm's 3-second fallback rebuild goes through
    `rebuild_5b`, which announces itself.
  - The engine has no pillbox self-heal; a repair is a builder action.
  - `can_hit` makes only the 5A pills miss the player; the 5B target hits.
  **Measured** (`tutorial_park5b`, scripted shooter on the parking
  square):
  - Watch, 60 s with no shells (the target is calm): the blocker loses 1
    armour every 2 seconds, 15 to 0 in 32 s, dies once, comes back once
    with the announcement, and starts down again. Silent rises: 0. The
    tank is not hit: the blocker takes every shell.
  - Take (from the end of the watch, blocker at 1 armour, the worst
    case): the target dies after 5.2 s. The angry target kills the
    blocker twice: at 0.06 s (it had 1 armour), and again 1.6 s after its
    rebuild (a full blocker against an angry target). The tank takes 8
    hits, 40 armour (all of a full tank's), and lives on 0.
  - *Decision for Andrew:* the take is possible but close. Once the
    target is angry a full blocker lasts about 1.6 s, and the take needs
    about 5 s of steady fire, so the tank is shot for the rest of it.
    A softer option, not built: a damage scale on the blocker only while
    the target is angry (one shell in two, say), with an announcement.
- **5B blocker rebuilt (item 10, 2026-10-10).** When the built 5B blocker
  dies before the target is picked up, the script rebuilds it on its
  square at full armour 2 seconds later and announces "To help you out,
  your friendly blocking pillbox has been automatically rebuilt." The
  once-a-second re-arm rebuilds it after 3 seconds as a fallback. The
  `tutorial_rebuild5b` arena measured 2.00 s.
- **5B target back (item 11, 2026-10-10):** 5 seconds after the player
  picks up the enemy pillbox it is put back, owned by the Station 6 bot's
  seat, at full armour, with "The enemy pillbox is back. Take it again, or
  go north." *Why 5, not 3:* picking it up usually finishes Station 5, and
  the "Station 5 done" line is up for 4 seconds. With 3 the two lines came
  together. Announcements now stack too, so they never replace each other.
- **4B hiding** mimics the engine's rule: the tank's square and the squares
  it overlaps all forest, and 3 or more squares from the pillbox on x or
  y; 5 seconds hidden in range ticks the item. Firing is not tracked: the
  popup says firing reveals the tank, and the pillbox then shoots it. The pillbox starts at
  2 armour, and `can_die` refuses its death until "Sit hidden in range" is
  ticked ("Hide in the forest first, then kill it.").
- **A pillbox taken out of its station** is sent home when the player
  enters another station carrying it, and one put down outside its station
  is sent home at once. *Why:* stations must not affect each other.
- **The Station 2 game-type popup** says a little more than the brief for
  Tournament: "the more bases are taken, the fewer shells a tank respawns
  with; once all bases are taken, tanks respawn without shells". *Why:*
  `gameTypeGetItems` gives 2 x 40 x (neutral bases / bases) shells, so the
  drop is gradual. *Undo:* cut the first clause in `POP.s2d`.
- **Border reminder (item 15, 2026-10-10):** the Station 7 popup and the
  "build" status line say to build the pillboxes near the border between
  the player's land and the bot's (north-east), to take control.
- **"Pick up both dead pills"** counts each Station 7 dead pillbox once it
  has been picked up, so building the first before picking up the second
  still ticks the item.
- **Gate arenas** hold the player's seat (a GoalHunter in the gate) still
  with `set_modifiers{speed = 1, turn = 1}` and teleport it about. Seventeen
  arenas: `tutorial_checkpoint`, `tutorial_reset`, `tutorial_take5a`,
  `tutorial_take5a_soak`, `tutorial_park5b`, `tutorial_rebuild5b`,
  `tutorial_station5_entry`, `tutorial_late_bots`, `tutorial_station6`,
  `tutorial_bot7_region`, `tutorial_bot7_stay`, `tutorial_bot7_man`,
  `tutorial_new_man`, `tutorial_station2`, `tutorial_watch5a`,
  `tutorial_popup_announce`, `tutorial_popup_first`. The
  5B arenas play the player with a scripted brain,
  `tests/brains/tutorial_shoot_north.lua`. The Station 2 steal is not
  covered: a held-still tank cannot drive onto a base.
