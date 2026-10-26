# Classic Tutorial scenario: decisions

The old built-in tutorial was C code: a step table in `src/bolo/tutorial.c`,
a sequencer in the client, a stop in the tank code, a respawn rule in the
start code, and a launch path in the menu. This change plays the same
tutorial as a scenario,
`data/maps/tutorials/Classic Tutorial.scenario.lua`, on the same map, and
removes the C code that nothing uses any more.

One entry per decision. Each entry says what, why, and how to undo it.

## The name

- **What:** the scenario is called "Classic Tutorial". Its files are
  `data/maps/tutorials/Classic Tutorial.map` (the old
  `Inbuilt Tutorial.map`, byte for byte) and
  `data/maps/tutorials/Classic Tutorial.scenario.lua`.
  **Why:** the new Tutorial (PR #521) is "Tutorial", and the main menu starts
  that one. A player who picks a map must be able to tell the two apart.
  "Classic" says that this is the one WinBolo always had.
  **Undo:** rename the two files and the `name` in the scenario table.
- **What:** the Classic Tutorial is not on the main menu. A player picks it
  from the map list like any other scenario.
  **Why:** the main menu's tutorial entry starts the new Tutorial.

## What the player sees

The script copies the old tutorial step for step:

| Old C code | Scenario |
|---|---|
| `tutorialSteps[]`: a row and up to four messages a step | `STEPS`: the same rows and the same message names, in the same order |
| The intro step waits for 3 presented frames | The intro is sent on the first tick that the player's tank is on the map |
| A step fires when the tank's row is the step's row | The same, checked every tick in `on_tick` |
| The C overlay shows the step's messages, one after the other | One `game.popup` a message. The client queues them and shows them in order, in the same overlay |
| The overlay pauses a solo game | A popup pauses a solo game in the same way |
| The server sets the tank's speed to 0 when it drives north onto a step row further north than any row it reached (`tutorialMinRow`) | The same test in `on_tick`, then `game.stop_tank` |
| After the row-66 step, respawns use start 2, not start 1 | `on_choose_start` answers start 2 after the row-66 step |
| The first respawn at start 2 shows `STR_TUTORIAL_RESPAWN1`, once | `on_tank_spawned` shows it once |
| Launch: strict tournament, no bots, one player | `game = "strict"`, `max_players = 1`, and `on_start` takes off the field any bot the host added |

- **What:** popups only; no status line, announcements or panel.
  **Why:** the old tutorial showed its messages in the overlay and nothing
  else. A status line or a panel would add text that the old tutorial did
  not have, and every new string must then be translated into 16 languages.
  The popup is the overlay the old tutorial used, so it looks the same.
- **What:** a step fires when the step's popups are sent, not when the
  player closes the last one.
  **Why:** the server cannot see a popup close. In a solo game the popup
  pauses the game, so nothing can happen between the send and the close.
  The old code moved to start 2 when the row-66 messages were closed; the
  scenario moves when they are sent. In a solo game the two are the same.
- **What:** the stop is one tick later than it was.
  **Why:** the old code set the speed to 0 inside the tank's move, in the
  tick that the tank entered the row. The scenario sees the row in
  `on_tick`, after the move, and the stop applies before the next move. A
  tank moves much less than a square in one tick, so it stops on the same
  row.

## Wording

- **What:** the last paragraph of `STR_TUTORIAL25` ("If you'd like to replay
  the tutorial later, you can launch it again from the Settings menu.") is
  removed, in English and in the 16 other languages.
  **Why:** the Classic Tutorial is no longer launched from the Settings
  menu. The sentence would send the player to the new Tutorial. This is the
  smallest change: the other paragraphs stay word for word, and no new
  string is needed.
  **Undo:** put the paragraph back in `lang.c` and the 17 lang files.
- **What:** no other message changes.

## Languages and input types

- **What:** each popup is sent as `{LANG:STR_NAME}`, the name of the old
  message in the language files. The client shows that string in the
  player's own language, and picks the `_TOUCH` or `_CTRL` wording for the
  player's input with the same rule the old overlay used (`pickStringId`).
  Then it fills in the key tokens (`{ACCEL}` and the others) as for any
  popup.
  **Why:** this keeps all 17 translations and all three input wordings with
  no copy of the text in the script. The other choice was per-language
  tables in the script, read with `game.lang(p)`, as the new Tutorial does.
  That choice cannot pick the touch or controller wording: the server does
  not know the client's input, and the input can change during the game. It
  also needs about 30 messages times 17 languages times up to 3 wordings in
  the script, which is about 250 KB, against about 8 KB now.
  **Undo:** remove `langRefText` from `tutorial_text.c`; the script must
  then carry its own tables.
- **What:** a reference is a whole text only. `"Hi {LANG:STR_X}"` is shown
  as written.
  **Why:** one old message is one popup, so nothing needs more. A whole-text
  match cannot change a popup that a script wrote for itself.
- **What:** a name that the language file does not have is shown as
  written. A client from before this change also shows the reference as
  written.
  **Why:** the server cannot know which client reads the popup. The unit
  test `lang_name_table_classic_tutorial` fails if a string the script uses
  is removed or renamed.

## Engine: `game.stop_tank(p)`

- **What:** a new op `SCN_OP_TANK_STOP` and a Lua row `game.stop_tank(p)`.
  It sets the tank's speed to 0 and nothing else.
  **Why:** the old tutorial stopped the tank on each step's row. No existing
  call does that: `game.teleport` moves the tank to the centre of the
  square and refuses deep sea (the first steps are at sea), and
  `game.set_modifiers` with speed 0 means the classic 100. This is a new
  scenario call, not a game rule change: a round with no script never
  reaches it.
  **Undo:** remove the op, its Lua row and its server arm.
- **What:** the op is placed after `SCN_OP_TANK_SET_MODIFIERS` in the enum.
  **Why:** it is a tank op. Op types are never written to the wire or to a
  file, so the numbers of the later ops can move.

## What the scenario cannot copy

These were set by the old launch path. A scenario cannot set them, so the
host's lobby settings apply:

- hidden mines (the old tutorial turned them off);
- the time limit (the old tutorial had none);
- the start delay (the old tutorial had none).

The old tutorial also hid the main menu's tutorial button once it was done.
The main menu now starts the new Tutorial, so the Classic Tutorial does not
change that button.

## Tests

- Unit: `scenario_tank_stop` (the op), `tutorial_lang_ref` (the parser),
  `lang_name_table_classic_tutorial` (every name the script uses is in the
  language table), and the op lists in `scenario_lua_every_row_answers` and
  the funnel tests.
- Arenas (`tests/scenario`, `GATE script=`):
  - `classic_tutorial_steps`: every step's popups, in order, on the right
    row, and a stop on each step's row;
  - `classic_tutorial_stop_once`: a passed row does not stop the tank again
    after a respawn, and a tank that drives south is not stopped;
  - `classic_tutorial_respawn`: start 1 before the boat step, start 2 after
    it, and the far-bank message once;
  - `classic_tutorial_real_row`: the first step and the stop on a real tank.
