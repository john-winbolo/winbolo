# Log Viewer improvements: decisions

Branch `logviewer-improvements`. This file lists what the branch adds, the
calls that were made while the owner was away, and what is still open. Each
call can be changed; the file says where.

## What the branch adds

- The seek slider moves the world while it is dragged.
- Sidebars on the right, left, top and bottom of the window. Panels can be
  dragged into them. The whole layout is saved.
- Slow playback speeds (0.5x, 0.25x, 0.1x) and one-tick stepping.
- The Events panel shows the whole log, with a filter and click-to-jump.
- A HUD panel with a status grid for each team, and a view of the game from
  one player's side.
- A text dump of a recording around one tick (`-dump`).
- File > "Open in single player" (the map at this time) and "Save Log".
- The server tick in the Controls panel.
- Neutral pillboxes and bases drawn with the game's own tiles. A red team is
  drawn orange.
- An eye icon in the HUD panel in place of the words "View of".
- An Overview panel: pillboxes and bases for each team.
- A Tank panel: the four gauges of the selected tank.
- An option for armour and shells bars over each tank on the map.

## Calls made without the owner

### Eye icon

- The eye is drawn with lines and circles in code. No picture file and no
  icon font was added. To change it: `imgui_hud.cpp`.

### Overview panel

- One table. The column headers are the game's neutral status icons.
- Each team row has a stripe in the team's colour. Below the teams there is a
  Neutral row and a Total row.
- Two share bars (pillboxes, bases), each 6 pixels high.
- A pillbox that a tank carries counts for its owner.
- A pillbox or base whose owner left the game counts as neutral.
- Shortcut: Ctrl+7.

### Tank panel

- The tank it shows is the viewer's existing "self" player. A click on a tank
  in the HUD panel selects it, and so does a map click in Select Teams mode.
  A map click in Information mode does not select. After a load the panel
  shows player slot 0.
- Hint text: "Click a tank in the HUD panel to select it".
- An old recording does not hold what a tank carries. The panel then says
  "This recording does not hold tank resources".
- The recording also holds tank modifiers. The panel does not show them.
- The gauge art is cut from `data/background.bmp` (x 438 to 493, y 151 to 258).
- Shortcut: Ctrl+8.

### Tank bars on the map

- Two bars above each living tank: armour on top, shells below.
- Each bar is exactly one map square wide.
- Height: one eighth of a square, never under 2 points, never over 5 points.
  The gaps are 1 point.
- Colours: track (24,24,24); shells (110,200,255); armour green above 50%,
  yellow above 25%, red at 25% and below. Opacity 0.75. No outline.
- No bars when a map square is under 12 points on the screen.
- No bars in the one-player game view, in the embedded viewer, or in exports.
- Menu: Options > Tank Bars. Off by default.

### Layout

- `LV_DOCK_TREE_WINS` was raised to 12 so the saved layout has room for the
  two new panels.
- Sidebar default sizes: 420 points wide (left, right), 220 points high (top,
  bottom). Minimum sizes: 220 wide, 100 high. Sidebars together take at most
  two thirds of the window each way, so the map always keeps one third.
- The layout is saved about one second after the window and the sidebars stop
  changing, and when the viewer closes.

### Colours (earlier commits)

- A team whose colour is the game's red is drawn orange. Only that team
  changes.
- Neutral pillboxes and bases use the game's red (hostile) tiles.
- The "Neutral Colour" setting no longer changes the map. It is still in the
  Team Colours dialog. The owner can remove it or give it a new use.

## Oct 13 review round 1

Numbers are the reviewer's finding numbers.

### 1. Preferences save overwrote the game's changes (blocker) - fixed

- The viewer now re-reads `WinBolo.json` before each save, copies in only the
  keys the viewer changed since its last save, and writes the file with a
  temporary file and a rename. It then keeps the result as its own copy, so
  it sees the game's other changes.
- One file write for each autosave. All viewer saves go through `prefsWrite`.
- The viewer does write two keys outside `[LOGVIEWER]`:
  `[WINBOLO.NET] Host` and `[WINBOLO.NET] CountryCode` (the shared network
  code sets them). For this reason the merge works by key, not by section.
  This differs from the instruction ("graft the LOGVIEWER section"); a
  section graft would lose those two keys or need a special list.
- The viewer does not write `[DEVICE] SyncDirty`. That flag is set only for a
  change in a section that goes to the cloud. `[LOGVIEWER]` and
  `[WINBOLO.NET]` are both kept on the device only (`kDeviceLocalSections` in
  `src/common/prefs.c`), so no viewer save sets it. An earlier version of
  this file said the viewer wrote it. That was wrong.
- If the file cannot be read, the viewer writes its whole copy, as before.
  A missing file was handled the same way; round 2, item 4 changed that.
- Left for the owner: the game client was not changed. The game still writes
  its whole in-memory document (about 15 seconds after a change), so the game
  can still overwrite a layout that the viewer saved while the game ran. One
  line fixes it (`prefsSetMergeOnSave(true)` after `prefsInit` in the game's
  start-up). It was not added because the game cannot be run here, and the
  change replaces the game's in-memory document on each save.
- A small gap stays: if the game writes between the viewer's read and the
  viewer's rename, that one game write is lost.

### 2. Web Log Viewer did not link (blocker) - fixed by reading, NOT built

- Added the four new panel sources to the web source list, defined
  `lv_windowCollectAllEvents` in `main_wasm.c`, and raised the saved-speed
  limit to `LV_SPEED_MAX`.
- `sdl3StatusItemPos` has an empty stand-in on the web. The real file was not
  added because it pulls in more of the game client.
- The web page does not draw the HUD, Overview and Tank panels or the
  sidebars. Their menu items and Ctrl+6/7/8 do nothing there. Left for the
  owner: show them on the web, or hide the menu items.
- Other gaps on the web were there before this branch and were left: code
  that the linker drops because nothing calls it.
- The game client's web stubs and the Android and iOS source lists need no
  change.

### 3. `lv_pillsGetPill` did not copy `inTank` - fixed

- Carried pillboxes are now left out of the single-player map and marked in
  the dump. Tests added.

### 4. Kill and death counts went up twice after a backward seek - fixed

- Each snapshot keeps the counts. A seek puts them back.

### 5. A dead tank stayed "alive" after a backward seek - fixed

- Checked the recorder: the map x it writes is 0 only for a destroyed tank,
  never for a tank in column 0. So "x is 0" is the right test.
- Note: a tank that has not spawned yet reads as dead at the start of a round,
  until its first position arrives.

### 6. Tank bars on recordings with no tank stocks - fixed

- No bars are drawn when the recording holds no stocks.

### 7. Slider release decoded the round again - fixed

- The release seeks only when its position differs from the last one applied.
  A seek to a time inside the current 20 ms step does nothing.

### 8. "Open in single player" on macOS, and the order of the checks - fixed, NOT built on Mac

- The viewer looks for the game first and writes the map after.
- On macOS it looks for `WinBolo.app` beside `Log Viewer.app`, which is how
  the package script ships them. When the game is not found the menu item is
  disabled on macOS.
- Left for the owner: macOS can run a downloaded app from a random folder
  ("translocation"). The game is then not found and the item stays disabled.

### 9. macOS menu bar lacked the new items - done, NOT built

- Added Overview (Cmd+7), Tank (Cmd+8), the left, top and bottom sidebars and
  Tank Bars. Each copies the pattern of the HUD item and the right sidebar
  item. It was read twice but never compiled.

### 10. Save Log - fixed

- The offered name of a downloaded log is cleaned of characters a file name
  cannot hold.
- The name offered and the path returned are two buffers.
- The bytes of a downloaded log are copied before the dialog opens.
- A source file that is gone gives its own message.
- Note: Save Map (older code) still passes one buffer. The dialog now accepts
  that.

### 11. `logviewer-maps` folder - fixed

- A folder that cannot be made is reported.
- A map is never written over an older one: the second gets " (2)".
- Left: nothing deletes old maps. The folder grows by one small file for each
  use.

### 12. Clock ran past the end of the log - fixed

- The clock moves only while the log plays. Play at the end does nothing.
- The embedded replay in the game was left as it was.

### 13. Tick readout: the unit differs by recording type - left

- The Controls panel shows the server's tick where the recording holds it, and
  a count of 20 ms steps where it does not. The two are not the same unit.
- Left for the owner: label the second kind differently, or hide it.

### 14. Scrub state stuck when the Controls panel was hidden mid-drag - fixed

### 15. Bar sizes were pixels, not points - fixed

- Thickness, limits, gap and the 12-point cutoff are points and are scaled by
  the display's pixel density. The width stays one square.

### 16. Bars read tank state without the lock - fixed

- The values are copied under the client mutex and drawn from the copy. The
  mutex can be taken twice by one thread, so there is no deadlock.

### 17. Dump argument and path checks - fixed

- A number over 32 bits is refused. A path too long for its buffer is refused.
  A tank line cut short still ends with "]".

### 18. Game started with no wait - fixed

- Checked the SDL3 source in the build tree. On macOS and Linux a child that
  is not waited for does stay as a zombie until the viewer ends. On Windows it
  does not.
- The game is now started as an SDL background process, which leaves nothing
  behind. On Windows the only change is that the game's standard input and
  output go nowhere.

### 19. Tests with their own copies of constants - fixed

- The Tank and HUD tests read the sizes from `gui/positions.h`. The label
  height and gap in the HUD test stay local: the panel takes them from the
  font at run time, so no header holds them.
- `lvDockMapRect` is now used by the two zoom functions.
- The information-only `printf` is gone.

### 20. Overview dock height one frame late after a load - fixed

### 21. One-player view: the panels still show every team - left, accepted

- The viewer is a spectator tool. The Overview and HUD panels show all teams
  in the one-player view too.

### Also left

- Some unit tests write their files into the working directory. This is the
  pattern the older tests use.

## Oct 13 review round 2

Five small items. No blockers.

### 1. A key set again to its old value did not reach the file - fixed

- Case: the game changes a viewer key in the file. The viewer then sets that
  key to the value the viewer still holds. The set was dropped as "no
  change", because the compare was with the viewer's copy, not with the file.
- In merge mode every set is now applied and its key is noted. With merge
  mode off the code path is the same as before.
- Still one file write for each save. A save that finds every noted key
  already in the file with the same value writes nothing; it only takes the
  file's content as the viewer's copy.
- Checked: the viewer sets keys only on a user action or inside its save
  routine, not each frame. So an idle viewer does not read or write the file.
- A set of the value already held does not set `[DEVICE] SyncDirty`.
- Tests: `prefs_merge_reset_wins`, `prefs_merge_idle_no_write`.

### 2. This file said the viewer writes `[DEVICE] SyncDirty` - corrected

- See round 1, item 1. The viewer writes `[LOGVIEWER]` keys and two
  `[WINBOLO.NET]` keys. It does not write `SyncDirty`.

### 3. Two programs shared one temporary file - fixed in the shared code

- The temporary file was `WinBolo.json.tmp` for every program. It is now
  `WinBolo.json.<process id>.tmp`, so two programs that save at the same
  moment do not write into one file.
- This is shared code: the game, the server tools and the viewer all use it.
  The change is one name. The process id comes from `_getpid` on Windows and
  `getpid` elsewhere, as other files in the tree do it.
- Left-over files: a failed write removes its temporary file, as before. A
  program that is killed in the middle of a write leaves one small file. A
  later run with the same process id writes over it. A left-over with another
  process id stays until someone deletes it by hand. No program removes it,
  because a program cannot tell it from a file that another running program
  is writing at that moment.
- An old `WinBolo.json.tmp` from before this change, if one exists, also
  stays. Nothing reads it.
- The Windows build was tested. The other platforms' path was changed the
  same way and was not built here.

### 4. A file missing for a moment made the viewer write its whole copy - fixed

- The other program removes the old file just before it renames its new file
  into place. A viewer save in that moment found no file and wrote its whole
  copy, with old values for the game's keys.
- In merge mode the first save that finds no file now writes nothing and
  reports "not saved". The viewer tries again at the end of the next frame.
  If the file is back, the save is a normal merge. If the file is still
  missing, the viewer writes its whole copy.
- At exit there is no next frame. The viewer tries twice, one directly after
  the other. If the file is still missing on the second try, the viewer
  writes its whole copy. Decision: at exit it is better to keep the viewer's
  settings than to lose them, and a file that is missing two times is most
  probably really gone.
- Note: the two tries at exit are only microseconds apart. If the game is
  inside its remove-and-rename at that exact time, the viewer's whole copy
  can still land first; the game's rename then replaces it, and the viewer's
  last change is lost. This is much less probable than before.
- Test: `prefs_merge_missing_retry`. `prefs_merge_fallback` was updated.

### 5. Death static played from the wrong time after a backward seek - fixed

- A snapshot says only that a tank is destroyed, not when it died. A seek
  back to such a snapshot, from a time when the tank was alive, stamped the
  snapshot's time as the time of death.
- Each stored snapshot now keeps the time each tank last died, as playback
  had it when it first reached the snapshot. This is stored beside the kill
  and death counts. A seek puts the time back for the tanks that are
  destroyed in the snapshot.
- Test: `lv_death_time_seek_back` (fails without the fix: 260 ms, not 40 ms).

### Standing note for the owner: the game still writes its whole document

- The GAME client was not changed. It saves its whole in-memory document
  about 15 seconds after a change (the debounce). While the game and the
  viewer both run, the game can put a stale Log Viewer layout back over one
  that the viewer saved.
- The reviewer's judgement: to turn merge mode on in the game should work. It
  is one line, `prefsSetMergeOnSave(true)`, after `prefsInit` in
  `src/gui/sdl3/gamefront.c` (near line 782).
- It is NOT done here. It is not tested together with the debounce. It
  belongs in a separate pull request, after a unit test of debounce and merge
  together.

## Oct 13 panel changes

### Tank panel: pictures for the words, and three states

The owner asked for icons in place of the words at the smallest size, and
for the text to go away when it cannot all be shown.

The panel now shows one of three things. `lvTankLayoutPick` in
`src/logviewer/lv_tank_status.h` picks it each frame. It reads only the room
under the name and the sizes of the text. It does not read what the last
frame drew, so the panel does not flicker at an edge.

1. Words: `Shells: 12 / 40`. Shown when all the text fits with the gauges at
   scale 2 or more. Scale 2 is the size the panel opens at.
2. Pictures: `<picture>: 12 / 40`. Shown when the words do not fit as above,
   and the pictures fit with the gauges at scale 1 or more.
3. No text. Shown when no line of text can be shown whole. Only the name
   and the gauges stay. The text comes back when the panel is large enough.

Rules that go with this:

- Each form is tried beside the gauges and under them. The place that leaves
  the larger gauges wins. Beside wins a tie.
- A block that fits exactly is shown. One pixel less and it is not.
- If there are no pictures (the background picture did not load), the words
  stay down to scale 1.
- The panel never scrolls now. It has no scroll bar and the mouse wheel does
  nothing in it. The small scroll in a 700 by 150 bottom slot is gone.
- A room smaller than the gauges at scale 1 shrinks the gauges to fit, down
  to a quarter of their size (`LV_TANK_WELL_MIN_SCALE`). Under scale 1 the
  picture is drawn smoothed. Smaller than a quarter, the gauges are cut off.

The art:

- Shells, mines, armour, trees: the four small pictures under the gauges in
  `data/background.bmp`. Each is 7 by 8 pixels. The panel cuts a 9 by 10
  cell with one black pixel round it and draws it as tall as a line of text.
  They read well at that size. The tooltip on each picture is the word.
- Pillboxes carried: the game's 12 by 12 neutral pillbox status picture from
  the tiles, the same one the Overview panel uses. The line becomes
  `<picture>: 2`. The tooltip is the full line in words.
- Builder: no picture. The game's builder is 3 by 4 pixels and does not read
  as an icon. The line stays `Builder: in tank` in both forms.

Notes for the owner:

- The builder line is the widest line of the picture form (about 112 pixels
  in the default font). A panel narrower than that shows gauges only. A
  short word or a drawn icon for the builder would let the picture form go
  narrower. That needs a new string or new art, so it was not done.
- No new strings. The numbers are taken from the existing lines: the part
  after the colon. A translation with no colon keeps its line in words.

### The narrowest sidebar is half as wide

The owner asked for the panel's minimum width to be half of what it was.

Limits found, and what was done with each:

- Left and right sidebar, `LV_DOCK_WIDTH_MIN` in `src/logviewer/lv_dock_fit.h`:
  220 before, 110 now. This is the limit that stopped a docked panel.
- Tank panel: it has no minimum of its own. A floating panel is held only by
  ImGui's own smallest window (32 by 32). Nothing was changed.
- No other panel shares a minimum width with the others.
- Scenario panel, `LV_SCN_PANEL_SIDE_MIN`: 128. This is the smallest side of
  its square map when it floats. It is that panel's own limit, so it was
  left at 128. Tell me if this one must be 64.
- Top and bottom sidebar height (100): left as it is.

The map still keeps at least one third of the window. A width saved with the
old limit is inside the new one, so a saved layout loads as before.

At 110 wide the Tank panel shows its gauges only.

### Controls panel: Pause in place of Stop, drawn icon buttons, jump tooltips

**Pause replaces Stop.** The Stop button of the panel did not hold the
playhead: it paused, waited half a second and closed the log
(`lv_windowStop`). The panel now has a Pause button in that slot. Pause holds
playback where it is; nothing moves the playhead and the log stays open.

- The panel has two buttons, Play and Pause, not one button that changes.
  Play is on when playback is paused and the log is not at its end. Pause is
  on while playback runs. The old text button that toggled Play / Pause is
  gone; with it the row would have had two Pause buttons.
- Space still toggles play and pause. Ctrl+P and Ctrl+U are unchanged.
- **Action > Stop (Ctrl+S) is kept** in the main menu and in the Mac menu.
  That menu already has a Pause item, and Stop there means "close the log".
  `lv_windowStop` is also what the open-file and browser paths call. If the
  owner wants the menu item gone or renamed, it is one line in
  `imgui_main_menu.cpp` and one in `mac_menubar.mm`.
- The wasm page has no Stop button of its own; it draws this same panel.

**Icon buttons.** Each button is a square as high as a slider, with an icon
drawn by the draw list in the colour of the text. The frame, the hovered,
held and disabled looks and the keyboard are those of an ImGui button. All
drawn icons of the viewer are now in one file, `imgui_lv_icons.cpp`:

| Icon | Shape | Used by |
|---|---|---|
| `LV_ICON_REWIND` | two triangles to the left | Controls |
| `LV_ICON_PLAY` | a triangle to the right | Controls |
| `LV_ICON_PAUSE` | two upright bars | Controls |
| `LV_ICON_FAST_FORWARD` | two triangles to the right | Controls |
| `LV_ICON_TO_END` | a triangle against a bar | Controls, "Jump to live" (live feed only) |
| `LV_ICON_EYE` | an eye | HUD panel (moved here from `imgui_hud.cpp`) |
| `LV_ICON_GEAR` | a toothed wheel | panel options |

No picture file and no icon font were added. A button whose action has
nothing to act on is dimmed: Rewind at the start, Play at the end or while
playing, Pause while paused, Fast Forward at the end. A dimmed button still
shows its tooltip. The speed stays a slider. There are no tick-step buttons;
the one-tick step is on the `,` and `.` keys only, as before.

**What Rewind and Fast Forward do.** They are not fixed skips. Rewind goes
back to the last snapshot of the log before the playhead; when that snapshot
is less than one second back it goes to the one before it, and it never goes
before the start of the game while the lobby is hidden. Fast Forward plays on
to the next snapshot, or to the end of the log when there is none. The
tooltip states the real distance at this moment: "Back 3.4 seconds (Ctrl+R)",
"Forward 12 seconds (Ctrl+F)". One decimal under ten seconds, whole seconds
from there. The number comes from `lv_screenRewindTargetMs` and
`lv_screenFastForwardTargetMs`, and the rewind one shares its pick with the
action (`lv_rewindPick`). The unit test `lv_jump_targets` asks for each
target and then presses the button: the two agree. With no target (and on a
live feed, which has no snapshot ahead) the tooltip is the plain name and key.

**Narrow panel.** A button that has no room on the line starts the next
line. The speed slider gives up width first (100 down to 50); under that the
label and slider take a line of their own; in a panel too narrow for both the
label goes and becomes the tooltip of the slider. The time texts wrap. The
headless try-out ran 6 states (no log, paused at start, playing, at end, live,
paused) at 12 sizes from 700 down to 60 wide: nothing is wider than the
panel, no assert. At ImGui's smallest window (32 wide) one button is 3 pixels
wider than the room.


### HUD panel: the name tooltip of a cell shows at once

The tooltip of a tank cell of the HUD panel (the name and team) showed after
ImGui's tooltip delay, and only when the mouse stood still. It now shows in
the first frame the mouse is over the cell and goes from cell to cell with
the mouse (`ImGuiHoveredFlags_DelayNone` on that one hover test). The pillbox
and base cells use the same code, so they do the same. The tooltip delay of
the rest of the viewer is not changed.


### A click on a tank on the map selects it

A left click on a tank's picture on the map selects that tank. The Tank
panel shows it, the map marks it, and the HUD panel puts a thin white edge
round its cell.

- **The selection is a state of its own.** Before, "the selected tank" was
  the viewer's "self" player, the same number the one-player view uses. Now
  there is a separate slot (`lv_playersGetSelected`, -1 for none). A map
  click sets only that slot. It does not turn the one-player view on, and in
  a one-player view it does not move the view to the other player.
- A click on a tank in the HUD panel sets both, as before: that player's
  view, and that tank selected.
- **Information mode** (the default): a click on a tank selects it, and the
  square under the click still gets its information as before. The two do
  not fight.
- **Select Team mode**: a click there has always meant "pick this player's
  team", and a one-player view that is on goes with it. That is kept, and
  the tank is selected too. This is the one place where a map click still
  moves a one-player view. The owner can say if Select Team mode must also
  only select.
- **The hit box** is the tank's picture where the last blit drew it
  (`lv_drawTankAtPoint`, the same place sums as the bars: sidebars, scroll,
  zoom, pixel density). It is never under 12 points a side, so a tank can be
  hit when the map is zoomed out. With two tanks under the mouse the nearer
  middle wins (`lv_tank_pick.h`, unit test `lv_tank_pick_sums`).
- A dead tank cannot be clicked. A tank the view does not draw cannot be
  clicked. A click that was a drag scrolls the map and selects nothing. A
  click on a panel does not reach the map.
- A click on empty map keeps the selection. A second click on the selected
  tank keeps it. Escape does nothing.
- **The mark** is four white corners just outside the picture, 1 point
  thick, 75% solid. It follows the rules of the bars: no mark for a dead
  tank, none under 12 points to a square, only in the viewer's own window
  (not in the game view, the embedded viewer or an export).
- The selection stays through a seek. When its player is not in the game at
  the new time, the Tank panel shows its hint until the player is there
  again. The selection is not saved in the preferences. A log opens with the
  first player selected, as before.
- "Tank Centred" still follows "self", not the selection.
- The hint of the Tank panel now reads "Click a tank on the map or in the
  HUD panel to select it".


## Not tested on screen

Nothing on this branch was seen on a screen by the author. The rules of the
work did not allow the viewer or the game to be started. What was checked:
unit tests, and a headless run of the Overview and Tank panels.

Please look at, at least:

- The eye icon, the Overview panel and the Tank panel, floating and docked.
- Tank bars at each zoom step, and on a display with a pixel density of 2.
- Sidebars on all four sides, and the layout after a restart.
- Seek slider: drag, release, and hiding the Controls panel during a drag.
- Step back and forward across a kill: counts and the dead tank.
- Play at the end of a log: the clock stays.
- Save Log for a file log and for a downloaded log.
- Open in single player, twice at the same time of the same log.
- The game and the viewer open together: change a setting in each, close
  both, and check both changes are in `WinBolo.json`.

## Not built

- The Web Log Viewer (wasm). The source list and the functions were checked
  by reading only.
- macOS: `mac_menubar.mm`, the lookup of `WinBolo.app`, and the background
  start of the game.
- Linux was not built either.
