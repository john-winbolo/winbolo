# Scenario scripts for manual checks

Two scenarios that exercise the parts of the scenario surface no unit test
can see: what is drawn on a client, and what the lobby says a scenario
changes. **Nothing here runs under CTest.** The unit cases cover the byte
layouts, the codecs, the handlers and their refusals; these cover the pixels.

## Running one

Copy it into the hosting scenarios directory — they are not read from the
repository:

- Desktop client: Settings, Hosting, Scenario Directory. The default is
  `<pref path>/scenarios`, which does not exist until it is created.
- Dedicated server: `-scenariodir <dir>`, or `data/scenarios` otherwise.

Then host a game, press Choose on the lobby's scenario line, pick it, and
start the round. Both set `bound = false`, so they play over whatever map is
committed.

## panel_check.lua

Draws every panel primitive, puts up an announcement, places both marker
kinds, and clears each of them on a timer.

At the first running tick:

- The panel square at the top right of the game view: a dark filled
  rectangle with a white outline, a rule across it, "Panel Check" in white,
  "centred" in cyan on the midline, "right" in orange with its right edge at
  x=124, seat 0's name in yellow, a friendly base and pillbox sprite, a green
  bar about a third full, and a countdown starting near 2:00.
- The red rectangle at 112,112 is trimmed at the square's edge. Nothing is
  drawn outside it.
- A line across the upper third of the view for five seconds, which clears
  itself. Firing works while it is up.
- A cyan marker on square 12,12 and a magenta one riding seat 0's tank, both
  in the game view and in the full screen map.

Then: at ten seconds the bar moves to about 80% and a second line appears for
three; at twenty the panel and the square marker go; at thirty the follow
marker goes.

Also worth checking: drag the panel, quit, relaunch, host again — it returns
where it was left. At zoom 2 the square is twice the size it is at zoom 1.

Move `MARK_X, MARK_Y` if the map committed has nothing near 12,12.

**Repeat this one when the log viewer, wasm or mobile drawing changes** — the
same script covers all four frontends, and the drawer is shared.

## rules_demo.lua

Nothing but a rules table, so the lobby's Rules button is the only thing that
reports what it does. Five rules, one per phrasing:

| Rule | Classic | Set to | Reads as |
|---|---|---|---|
| `tank_reload_ticks` | 13 | 5 | 2.6x faster |
| `tank_full_shells` | 40 | 80 | twice as many |
| `tank_full_mines` | 40 | 20 | half as many |
| `tank_accel_rate` | 0.25 | 0.5 | 2x faster |
| `tank_full_armour` | 40 | 48 | +8 |

Pick it, then press Rules on the scenario line. Every rule is named, never
shown by index, and each phrase is one a person would say out loud. The last
row is under the 1.5x band, which is where a difference is reported instead of
a multiple.

A second client joining the lobby has the Rules button too and lists the same
numbers. Choosing None, or committing a plain map, removes the button.
