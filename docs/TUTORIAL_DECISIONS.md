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
  "the build buttons" (new string `STR_TUTORIAL_QUICK_BUILD_TOUCH`, 2763).
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
