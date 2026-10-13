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

- One table. The column headers are the map's own pictures of a pillbox and
  a base no team holds: the game's red pillbox and the game's neutral base
  (black with white dots), made smaller (Oct 13; `lv_item_picture`, the
  helper the events lane uses). The two share bars start with the same
  pictures. They keep the size the game's status icons had there (12
  points at the usual font), so the table, the bars and the least width of
  the panel are as they were. The tooltip of a header picture is its word
  ("Pillboxes", "Bases"). The table has no picture for each team, so none
  is in a team's colour.
- Not changed, because the map's picture does not go in with no change to
  the layout: the HUD panel (its grids are the game's own 12 by 12 status
  slots) and the Tank panel's line of carried pillboxes (the game's status
  picture, in the game's status layout). The Events panel had no column of
  icons then; it has one now (see "Each line of the Events panel starts with
  its marker").
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
- Builder (Oct 13; before that the line kept its word): the map's own
  picture of a builder in place of the word. The line is
  `<builder>: in tank`, `: out`, `: dead` or `: dead, back in 0:07`, as the
  line in words has it. It is the first builder frame of the tiles, 3 by 4
  pixels, in the game's own colours (`lv_drawBuilderPicture` in `draw.c`,
  the cell `lv_drawLGMs` draws; `lv_builder_picture` in
  `imgui_lv_icons.cpp` is the shared helper). It is drawn at a whole number
  of times its size, so it stays sharp: 9 by 12 on a 13 point line. While
  the builder is dead the picture is the parachute tile of the map (16 by
  16, a square as high as the line, blended). The picture is in the middle of the column
  the other pictures have, and its colon is under their colons. The tooltip
  of the picture is the word "Builder", taken from the line before its
  colon. With no tiles loaded a drawn figure stands in. The form in words
  still reads `Builder: in tank`; the gauges-only form is unchanged.

Notes for the owner:

- The first builder frame was taken as the standing one. It was not seen on
  a screen. If it is a walking frame, the cell is one number in
  `lv_drawBuilderPicture`.
- The builder line was the widest line of the picture form (about 112
  pixels in the default font). With the picture it is about 72 at its
  widest state ("dead"), and the choice of form counts it at that width. So
  the picture form now shows in narrower panels: in the try-out a panel 110
  wide and 300 or more high now has the pictures (gauges only before), and
  a panel 180 wide has them beside the gauges (under them before).
- The pillbox line was left as it is: it already has the game's own
  pillbox picture, not a word and not a drawn shape.
- A language whose builder lines have no colon keeps the line in words.
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

**A double click on a tank shows that player's view, or turns it off (Oct 13).**
A double click on a tank on the map selects it and turns on the view of its player:
the state the eye and the name in the HUD panel stand for. The X of the HUD
panel goes back to all teams, as before.

- Before this a double click had no meaning of its own. It was two single
  clicks: the tank was selected twice (Information mode) or picked twice
  (Select Team mode), with no other effect. The map had no way to turn a
  player's view on; only a click on a tank in the HUD panel did that. (In
  Select Team mode a view that was already on moved with the picked tank,
  and still does.)
- A double click on the tank whose view is on turns the view off: back to
  all teams, as the X of the HUD panel does. The tank stays selected. (The
  first build of this left the view on; the owner then asked: "let's make
  double clicking when it's on also turn it off if it's the same tank".)
- A double click on another tank while a view is on moves the view to that
  tank. A double click on a tank with no view on turns its view on.
- The rule is `lvMapClickViewsTank` in `lv_tank_pick.h`. It answers "on",
  "off" or "no change" for a click, from which click of the row it is, the
  tank, the tank of the click before, and the view **as it was before the
  first click of the pair** (kept by the map when that first click comes
  in). The map then puts the view in that state: the HUD panel's own calls
  (`lv_screenTogglePerspective` to turn it on, `lv_screenClearPerspective`
  to turn it off), each made only when the view is not in that state yet.
- Each click of the double click still does all a single click does. The
  first selects the tank and does not change the view in Information mode.
  Only the second click of a pair changes the view, so it changes once for
  a double click. A third fast click is the first of a new pair and changes
  nothing; a fourth is a second double click.
- Both clicks must be on the same tank. ImGui counts the clicks (its double
  click time and distance: 0.3 s, 6 points).
- A double click on the map, a pillbox or a base does what it did before:
  two single clicks.
- Select Team mode: the first click picks the tank's team as before ("self"
  is that player), and, as before, moves a view that is on to the picked
  tank. That is why the rule uses the view from before the first click:
  - view on tank A, double click on tank B: the first click moves the view
    to B, the second finds "the view was on A, not B" and keeps it on B;
  - view on tank A, double click on A: the first click changes nothing, the
    second turns the view off. "Self" stays A;
  - no view on: the first click picks the tank, the second turns its view
    on.
  So both modes end in the same state for the same double click. In Select
  Team mode with a view on another tank the view changes on the first click
  and not on the second.
- The viewer has one map and one place where a click on it is read
  (`imgui_game_viewport.cpp`), so this is the same in every view of the map,
  on a file log and on a live feed. The embedded viewer in the game has no
  click on a tank and has none now.
- The hint of the Tank panel now ends "Double-click a tank on the map to
  see the game as its player sees it".
- Unit test: `lv_map_double_click`. The click itself was not tried: the
  headless try-out has no map.


### The name line: team stripe in the HUD panel, and an X to deselect

The Tank panel's name has a stripe in the colour of the tank's team at its
left. The line of the HUD panel that names the player of the one-player view
now has the same stripe. Both panels draw the line with one helper
(`lv_name_header` in `imgui_lv_icons.cpp`): stripe, an icon when there is one
(the eye in the HUD panel), the name, and a small X at the right end.

- **Tank panel X** ("Deselect tank"): no tank is selected. The panel shows
  its hint and the map has no mark. It does not leave a one-player view,
  also when that view is of the same player: the X of each panel does one
  thing.
- **HUD panel X** ("Back to all teams"): leaves the one-player view and
  clears the selection.
- The text button "All teams" that stood before the name in the HUD panel is
  gone; the X does its work and the line is shorter. With no one-player view
  the line still reads "All teams" in grey, as before.
- "No tank selected" is a real state (`lv_playersGetSelected` is -1).
  Nothing puts a tank back but the user: a click on a tank on the map or in
  the HUD panel. Only the load of a log selects the first player. Escape
  does nothing new.
- **Narrow panel**: the X stays at the right end and the name is cut to the
  room left, with "..." (`lvNameHeaderLayout`, unit test
  `lv_name_header_sums`). The whole name is the tooltip of a cut name. In
  the headless try-out, with a 41-letter name, the line was never wider than
  the panel from 400 down to 50 wide, and a click on each X did its work.
- The X is drawn by the shared icon helper (`LV_ICON_CLOSE`); it has no
  frame until the mouse is over it.


### "Make unused sidebar portions transparent": an option of a sidebar

**What was there before, and why it did nothing.** The option was one a
panel ("Make unused panel portions transparent", a gear on every panel). In
a sidebar it showed no change: a docked panel made clear showed the
sidebar's own strip, which is a window in the same colour (`ImGui::Begin` of
the strip in `draw_side`, `imgui_dock.cpp`), and the map stopped at the
sidebar's edge (`lvDrawBlitTargetToWindow`, `draw.c`), so nothing was behind a
docked panel to see. The owner then said what was wanted: the option belongs
to a sidebar, and "unused" means the part of the sidebar under its panels,
not the background of a panel.

**The per-panel option is removed.** The gear on every panel, the helper
(`imgui_lv_panel.cpp`, `lv_panel_opts.h`), the backing boxes and the
preference `Window.<Panel>.Transparent` are gone. The gear had no other
item. An old `Window.<Panel>.Transparent` key in a preferences file is not
read and is not moved to the new option. A floating panel has no such option
now. String 2840 keeps its number with a new name and new words:
`STR_LV_DOCK_TRANSPARENT` "Make unused sidebar portions transparent".

**The option.** One a sidebar (left, right, top, bottom), off by default.
It is in one place: the Windows menu, under the four sidebar items, a
submenu "Make unused sidebar portions transparent" with the four sidebars as
check items.

**No gear (Oct 13).** For a short time each sidebar also had a gear on a
corner of the map, with this one item in its menu. The owner had it taken
out: the menu bar is enough. The gear, its menu, its place on the map, the
drawn gear icon (`LV_ICON_GEAR`, which nothing else used) and its tooltip
string are gone. Number 2839 (`STR_LV_TIP_DOCK_OPTIONS`, "Sidebar options")
is not used and is not to be given to a new string; `lang.h` says so. The
map has no button on it now.

**Option off.** The sidebar is as it was: a solid strip, the panels share
all of its length, the map stops at its edge.

**Option on.** (`lv_dock_clear.h` has the sums, `draw_side` uses them.)

- The panels are packed from the start of the sidebar with nothing between
  them but a grip: from the top down in the left and right sidebars, from
  the left across in the top and bottom ones. The same function does both;
  only the axis differs.
- Each panel has its own length there (a height in the left and right
  sidebars, a width in the top and bottom ones). It does not stretch.
- A grip 4 points thick is on the far edge of each panel (its bottom edge,
  or its right edge in the top and bottom sidebars). The cursor changes on
  it. A drag changes the length of that panel and of no other; the panels
  after it move. A double click gives the panel the length of what it shows.
  ImGui's own splitters along the sidebar are off while the option is on
  (a drag of one takes from one panel what it gives the next).
- A panel is fully solid inside its rectangle. Nothing in a panel is
  see-through.
- After the last grip the sidebar is clear to its end. There is no window
  there at all: the map shows, and **a click, a drag or the wheel there goes
  to the map**.
- The first length of a panel, the first time it is in a sidebar with the
  option on: the Controls, HUD, Overview and Tank panels show a fixed amount
  and get the length of their content (shorter or longer than they were). A
  list (Game Information, the scenario panel, Item Information, Events,
  Comments) gets the smaller of the length it had and the length of its
  content, so a long list does not take the whole sidebar. Never less than
  the least length: 48 points down, 110 points across.
- **When the panels do not fit** (a short window): the last panel gives up
  length first, down to its least length, then the one before it, and so
  on. The first panels keep their size. When even the least lengths do not
  fit, every panel gets its share of the room. Nothing goes past the end of
  the sidebar, and a panel too short for what it shows scrolls inside, as
  any panel does. The saved lengths are not changed by this: in a taller
  window each panel has its own length again.
- Panels side by side across the sidebar (a split the other way) count as
  one part with one length and one grip. Panels as tabs of one node are one
  part too. The length saved for such a part is under its first panel.
- A sidebar with no panel in it and the option on is the whole strip with
  no backdrop and the hint text on a small plate. That strip still takes the
  mouse, so a panel can be dropped in it.
- The grip on the sidebar's inner edge (the one that makes the sidebar
  bigger) works along the whole length. Beside the clear part it is drawn
  only while the mouse is on it or holds it.
- To add a panel to a sidebar with the option on, drop it on a panel that is
  there (the drop targets of that panel). The clear part is map, not a drop
  target.
- Option turned off again: the panels fill the strip again, in the
  proportions the lengths left them.

**The map under the sidebar.** With the option on the map is drawn under
that sidebar (`lv_imgui_dock_map_margins`: the margins with 0 for a clear
sidebar). The left and right sidebars run the full height, so when one of
them is clear the map also reaches the top and the bottom of the window, and
goes under the top and bottom sidebars, solid or not; a solid one hides that
part.

**The visible map area does not change.** What counts as the visible map,
for the middle of the view, is still the part of the window no sidebar is
on, option on or off. The screen is told how much of the view is under a
clear sidebar at each edge (`lv_screenSetViewInset`), and "Tank Centred",
centre-on-click, the jump to an item and the jump to an event all take it
off first. When the option is turned on or off the map's corner moves on the
screen; the scroll position is moved by as much the other way, so the map
stays where it was (at the edge of the world it can only go as far as the
map allows). The ring's arrow for a place off the view goes at the edge of
the visible area, not under a sidebar. The "sidebars take at most two thirds
of the window" rule is unchanged.

**Preferences** (section LOGVIEWER), saved with the rest of the layout:

- `Dock.Clear`: one line, `v1 R=0 L=1 T=0 B=0`.
- `Dock.Len.Right`, `.Left`, `.Top`, `.Bottom`: the lengths of that
  sidebar's panels, `v1 Controls=142 Overview=394 Events=267`. A panel with
  no length yet is left out.

**Tests.**

- Unit: `lv_dock_clear_layout` (packing, overflow, drag limits, first and
  snap lengths), `lv_dock_clear_rects` (panel, grip, used and clear
  rectangles, both axes), `lv_dock_clear_map` (map margins, which points are
  map, the middle of the visible map, the scroll shift), `lv_dock_clear_prefs`
  (both preference lines out and back, damaged text).
- Headless try-out with the real `imgui_dock.cpp` and the real Controls,
  HUD, Overview and Tank panels (the other five are stand-ins with rows of
  text), a hidden SDL window, a made-up mouse. For each of the four sides:
  empty with the option on; three parts (Controls, Overview, Events with
  Comments as a tab) option off, then on; packing and the 4 point grips; the
  clear part has no window under the mouse and is map; a drag of the last
  grip and of the first grip; a double click; a window too short and far too
  short, and back; the inner edge dragged beside the clear part; no gear
  window and no popup; option off again; Tank beside Controls across
  the sidebar; the saved lines. No check failed and no ImGui assert.
- NOT tested: the map drawn under the sidebar (the try-out has no renderer),
  the look of the grips, and the wiring in `screen.c`,
  `draw.c` and `logviewer.c` beyond the sums above. Those need a look on a
  screen.


### Controls panel: rows in the middle, wider playback buttons (Oct 13)

The panel has three parts, top to bottom:

- **The top row:** Rewind, Play, Pause, Fast Forward ("Jump to live" as a
  fifth on a live feed), the speed (its label and its slider) and the
  "Overlay events" list.
- The lane of event markers and the seek bar. They keep the full width.
- **The bottom row:** the time ("Time: now / total"), the time left and the
  tick. (Tablet and Steam Deck modes do not draw this row.)

The top row and the bottom row are in the middle of the panel's width. The
places are worked out every frame from the width of the panel, before
anything is drawn (`lv_controls_layout.h`: `lvControlsTopLayout`,
`lvControlsFlow`). A row that does not go on one line takes more lines, and
each line is in the middle by itself. A piece of text wider than the panel
starts at the left edge and goes on to more lines.

The playback buttons are twice as wide as they are high. The icon keeps its
size and stays in the middle; the whole button takes the click. In less room
all the buttons get narrower by the same amount, down to the old square. That
is what a sidebar at its least width (110) gets: four buttons 20 wide. In
less room than four squares they stay square and go on more lines, as before.
The buttons take their double width before the speed gets room on their
line: at a panel 300 wide the speed is now on a second line. No width gives a
scroll bar from left to right.

- Unit tests: `lv_controls_top_row` (every width from 20 to 900, four and
  five buttons), `lv_controls_bottom_row`.
- Headless try-out: the Controls panel at twelve sizes in five states,
  floating and docked: no assert, and no content wider than the panel that
  was not so before (only the 32 by 32 panel, as before).

### Controls panel: "Overlay events", markers over the seek bar

The Controls panel has a list, "Overlay events". The user ticks kinds of
event in it. Each event of a ticked kind then has a small marker in a lane
just above the seek bar, at the place the handle of the bar has when
playback is at that event. The mouse over a marker says what happened. A
click goes there. The code is `imgui_event_overlay.cpp`; the sums are in
`lv_event_overlay.h`.

**One list of events.** The markers show the lines of the Events panel. They
keep no list of their own and the log is not read a second time. The kinds
and their names are the ones of the filter of the Events panel
(`lv_event_kind.h`): pillbox captured, pillbox stolen, base captured, base
stolen, builder killed, tank killed, player, chat, game, highlight.

**There is no "pillbox picked up" or "pillbox placed" kind.** The request
named them. The viewer does not post a line for either today, so there is
nothing to mark. A new kind of line is a change to the Events panel too, so
it was not added here. The saved list ignores a name it does not know, so
such a kind can be added later with no change to the preferences.

**The list.** It is a combo on the line of the buttons, after the speed. It
stays open while kinds are ticked. "All" and "None" are at its top. Each row
has the marker of its kind, the name of the kind and how many events of that
kind the log has (with the lobby hidden: without those of the lobby). With
little room the combo is only its arrow, and its name is in its tooltip; the
speed slider gives up width (down to its 50 points) to keep that arrow on
the line. With no room on the line it goes on a line of its own.

**Default.** A new install shows what changes the score: pillbox captured,
pillbox stolen, base captured, base stolen, tank killed, builder killed.
Player, chat, game and highlight are off.

**Saved.** One key of the LOGVIEWER preferences, `Controls.OverlayEvents`:
the saved names of the ticked kinds with commas between them. "none" is an
empty choice (an empty text would read as "never saved"). A name from a
newer build is ignored. It is written at each tick.

**The same list is the filter of the Events panel (Oct 13).** The Events
panel had a filter of one kind or all. It now has the list above: the same
function draws both (`lv_event_kinds_combo` in `imgui_event_overlay.cpp`),
so the ten kinds, their order, their markers, the counts and the "All" and
"None" buttons are the same. Each has its own choice.

- The panel's choice is saved under `Events.Kinds`, in the same form as
  `Controls.OverlayEvents`. The default is every kind. With no `Events.Kinds`
  saved, an old `Events.Filter` of one kind is taken over as that kind alone
  and written under the new key (`lvEventsMaskFromPrefs`). The old key is
  left in the file and not read again once the new one is there.
- The list shows only the lines of the ticked kinds. One test decides it for
  the panel's lines and for the lane's markers: `lvEventKindTicked`.
- **A change from before:** the old filter fell back to "all" when the log
  had no line of the chosen kind, and did not offer such a kind. The list
  now offers all ten kinds (a kind with no line reads "(0)") and shows what
  is ticked. With lines in the log and none of a ticked kind, the panel
  reads "No events of the selected kinds".
- Closed, the filter reads "All events (412)" with every kind ticked,
  "Tank killed (37)" with one kind, "37 of 412 events" with any other
  choice. In a narrow panel it reads only "(37)", and in a narrower one it
  is the arrow alone (`lvEventsLabelPick`); what is left out is then in the
  tooltip.
- The selected line stays selected when the filter changes and its kind is
  still ticked, and the list is moved so that it is in the middle. A
  selected line the filter now hides is not selected any more.
- The panel has no text filter and no player filter, and no "next event" or
  "previous event" keys; none was there before and none was added. A click
  on a line goes to that event, as before, and can only be on a shown line.
- Unit tests: `lv_event_kinds_filter`, `lv_event_kinds_prefs`. A headless
  try-out ran the real Events panel (400 made-up lines; nothing saved, one
  kind, three kinds, none, a kind with no line) at ten widths from 400 down
  to 40: no ImGui assert and no scroll bar across. A click opened the list
  at 300, 110 and 60 wide; "None", one row and "All" each saved the right
  text and left the list open. Not checked there: what text the closed
  filter shows (only the sum that picks it is tested), and the place of the
  selected line after a change.
**Each line of the Events panel starts with its marker (Oct 13).** The
owner: "now that we have an icon for each event let's put those on the
events actual list".

- Every line of the list has, at its left, the marker its event has in the
  lane over the seek bar: the map's picture of a pillbox or a base in the
  colour of the team the line names (the new owner; no team: the game's red
  pillbox, the game's neutral base), or the kind's icon on the dark square
  with a line in the team's colour round it.
- One function says what the marker of an event is (`lvEventMarker` in
  `lv_event_overlay.h`: picture or icon, and the team) and one function
  draws it (`lv_event_marker_draw` in `imgui_event_overlay.cpp`). The lane,
  the list of kinds and the lines of the Events panel all call it, so the
  three cannot differ. Because of that the list of kinds changed a little:
  an icon there now has the dark square behind it, and "Tank killed" has
  the game's "dead" picture, as in the lane.
- The marker is a square as high as the text (13 points at the usual font),
  in the middle of the line. The line is not higher than before. The
  picture is made smaller with the rule of the lane (blended at 13 pixels,
  sharp at 16, 32, 48).
- The marker of a line still to come is dim, as its text is.
- The whole line is one item, so a click on the marker does what a click on
  the text does. The mouse at rest on the marker shows the name of the kind.
- The text of a line is not changed. "Copy" still copies the text only.
- A line names two teams at most; the marker has the colour of the first.
  The lane shows the second team as a tick under the marker; a line of the
  list has no room for it under the marker, so it is not shown there.
- Narrow panel: the list scrolled sideways when a line was longer than the
  list, before this change, and it still does. The marker does not scroll:
  it stays at the left of the list and the text goes under its column and
  is cut off there. The text is not shortened with "...".
- **"Builder lost" is a builder with an X over him (Oct 13).** The owner:
  "'Builder lost' should be an LGM with X over it, I think we have that
  icon somewhere?" The game has no such picture. What it has: the builder
  of the map (`data/tile.bmp`, three frames of 3 by 4 pixels at x 428, 431
  and 434, y 90: `LGM0_X`... in `src/gui/tiles.h`), the parachute tile of
  his replacement, the "dead" picture of the status panels (a white X on
  black, 12 by 12 at 440, 60: `STATUS_ITEM_DEAD_X`), and, for a dead
  builder in the game's own HUD, the builder circle filled red
  (`sdl3DrawSetManStatus` in `src/gui/sdl3/sdl3draw.c`). `data/items.bmp`
  and `data/background.bmp` have no builder. So the marker is put together:
  the map's builder (the standing frame, through `lv_builder_picture`), in
  the game's own green with the red head, the most whole times his size
  that go in the marker (2 times at 13 points: 6 by 8), and two thin red
  lines from corner to corner over him, each on a darker line. It is on the
  dark square with the line in the team's colour round it, as the other
  icons are. With no tile sheet the drawn head and shoulders takes the
  builder's place under the X. `lvEventMarker` says "builder lost" is drawn
  so, for the lane, the list of kinds and the lines of the Events panel.
- Only the lines in view are drawn (the list clipper is kept), and no
  texture is made: the pictures come from the game's sheets, already loaded.
- Unit test: `lv_event_marker`. The headless try-out of the real Events
  panel, with and without a stand-in for the game's sheets, at ten widths
  from 400 down to 40: no ImGui assert, no scroll bar across the panel;
  about 13 lines drawn of 400; a click on a marker and a click on a text
  both seek; the tooltip shows on the marker and not on the text. Not run
  there: the list scrolled sideways.

- Note: under about 116 points wide the "Auto Scroll" check box of the panel
  is wider than the panel. That was so before; the filter itself fits down
  to its arrow.

**The lane.** It is as wide as the seek bar and one line of text high, plus
3 points, with 1 point between it and the bar. The x of a marker is the sum
ImGui does for the middle of the handle (`lvOverlaySliderX`: 2 points of
padding and half the handle at each end). The headless try-out found the
lane and the slider at the same x extent, and a click on the slider at the
x of a marker gave the value of that marker's time within one pixel. With
no kind ticked there is no lane and the panel is that much lower. The
default height of the panel, floating and in a sidebar, is 17 points more
while the lane shows.

**The markers.**

- A pillbox or a base changed hands: **the map's own picture of a pillbox or
  a base, made smaller, in the colour of the team that now owns it** (Oct
  13; before that a filled square and a filled disc). It is the same sheet
  and the same cell the map draws (`lv_drawItemPicture` in `draw.c`, by
  `lvPanelItemSprite` in `lv_team_sprite.h`): a team's cell of
  `data/items.bmp`, a red team in orange, the pillbox at full armour. With
  no team it is the game's own tile: the red pillbox, the neutral base
  (black with white dots). The picture carries the team's colour, so these
  markers have no dark box, no fill and no line in the team's colour. The
  white line round the marker under the mouse stays.
- The picture is 16 pixels in the sheet and the marker is as high as a line
  of text (13 points). At a whole number of times 16 pixels on the display
  it is drawn sharp, with no blending; at any other size it is blended, so
  no row or column of pixels is dropped (`lvItemPictureSharp`). The 12 by 12
  status icons in the other panels are only ever drawn at whole multiples,
  sharp.
- A marker that stands for several events, all of them pillbox events
  (captured or stolen, in any mix) or all of them base events: the picture,
  in the colour of the team most of them name, with the number on a small
  dark plate in its lower right corner. Any other mix is the box with the
  number, as before.
- In the "Overlay events" list each kind has the marker of an event of
  that kind that names no team: the four pillbox and base kinds have the
  picture of one no team holds, the other kinds their icon on the dark
  square with no line of a team round it.
- One helper draws the picture for every panel (`lv_item_picture` in
  `imgui_lv_icons.cpp`). When the game's sheets are not there the old plain
  shape is drawn. Unit test: `lv_event_overlay_picture`.
- Any other kind: a plain icon in the colour of the text on a dark square,
  with a **1 point line in the team's colour round it**. A tank kill has
  the game's own "dead" picture of the status panels and the colour of the
  killer's team (the team of the tank that died, when it died with no
  killer). A builder lost is the map's builder with an X over him (see
  below), with the line in the colour of the team that lost it (the log
  does not say who shot it). Chat is a speech bubble
  in the colour of the sender's team. Player is a ring, game a diamond and
  highlight a star; these have no team.
- A second team (the old owner of a stolen pillbox or base, the tank that
  was killed) is a short tick under the marker in that team's colour.
- The team is the one the player was in when the event happened. The
  decoder now says which players a line is about (`lv_messageTagPlayers`),
  and the Events panel keeps the two teams with the line.
- The markers playback has passed are dimmer (45%).
- All of them are drawn with the draw list (`imgui_lv_icons.cpp` has the
  new shapes), except the "dead" picture, which is from the tile sheet. With
  no tile sheet it is a drawn X.

**Markers too close to each other.** Markers nearer than one marker width
become one: a box with the number of events in it ("99+" over 99), in the
colour of the team most of its events name (grey when none names a team).
A marker takes in every event less than one width after its first event, so
two markers never overlap. It is at the place of its first event.

**The mouse over a marker** (at once, no delay): for one event its time,
its tick and its kind, then its line of the Events panel. For several:
their number, the first 5 lines with their times, and "+N more".

**A click** goes to the event as a click on its line of the Events panel
does (`lv_windowSeekToEvent`: the playhead goes to the event's time and the
map is centred on its place). On a marker of several it goes to the first.
A highlight goes to 5 seconds before it, as its line does. On a live feed a
click does nothing, as in the Events panel. A press in the lane where no
marker is does nothing. The lane is an item of its own above the slider, so
it takes no press that is meant for the slider and a press in it does not
drag the panel.

**Cost.** What the lane draws is worked out from the list once and kept. It
is worked out again only when the list changes (its version counter), the
ticked kinds change, the shown stretch of the log changes (lobby shown or
hidden; on a live feed it grows all the time) or the width of the bar
changes. The headless try-out with 20,000 events: 0.4 ms for the frame that
works it out, then 0.05 ms a frame for the whole Controls panel with no
read of the list.

**Other notes.**

- The embedded viewer and the browser build have the same Controls panel,
  so they have the list and the lane. In the browser build the Events list
  holds only what playback has passed, so the markers are only behind the
  playhead there.
- Unit tests: `lv_event_overlay_slider`, `lv_event_overlay_clusters`,
  `lv_event_overlay_choice`.
- Headless try-out: a log with no events, 5 events, a highlight, the lobby
  hidden, a live feed, 20,000 events, the list (All, None, one check box,
  a click outside), the panel floating at 9 sizes down to 32 x 32 and as a
  tab at 4 sizes: no assert.


### Times to the hundredth of a second; a jump to an event flashes a ring on the map

**How a time is written.** Every playback time of the viewer is now written
by one function, `lvTimeFormat` (`lv_time_fmt.h`): `19:48.22`, and
`1:02:07.22` from one hour on. Before, each place had its own "mm:ss".

- The request wrote the example as "19:48:22". The mark before the
  hundredths is a **dot**, not a colon: with a colon, "19:48:22" reads as
  hours, minutes and seconds, and a log of one hour or more could not be told
  from it. The mark is one constant, `LV_TIME_FRACTION_MARK`; it is one line
  to change.
- The hundredths are cut, not rounded, and the time is never rounded to the
  second first. Playback moves in steps of 20 ms, so each step has a text of
  its own and the last digit is always even.
- With the hundredths: each line of the Events panel, the "now" line of that
  panel, the tooltips of the markers over the seek bar, and the time playback
  is at in the Controls panel (in the seek bar and in the line under it).
- To the second, with the same function: the length of the log and the time
  left in the Controls panel (they do not change by the hundredth in a way a
  reader can use), and the time in the name of a saved map.
- From one hour on the hours are written ("1:15:03"; before it was "75:03").
- No box of the viewer takes a typed time today, so there was none to
  change. `lvTimeParse` reads the new form back ("ss", "mm:ss", "h:mm:ss",
  each with or without hundredths) for the day one does.

**Where a click on an event goes.** The time kept with an event is the clock
of the tick that posted it, so it is already exact: a whole 20 ms step. A
click on a line of the Events panel, or on a marker over the seek bar, seeks
to that time and playback stops **on that tick, with the event done** (the
pillbox has its new owner, the tank is dead). No lead time is added. The
Controls panel then reads the same time the line has. One tick earlier the
event has not happened; the `,` key steps back to it.

- A highlight still goes to 5 seconds before its moment, as before: it is a
  clip to watch, not a point.
- The seek skips a target that is less than one tick behind the playhead
  (made for the release of the seek bar). An event's time is a whole tick, so
  that never takes a jump to an event: it is either the tick playback is on
  (nothing to do) or a tick away.
- Unit test `lv_event_seek_exact`: from the end, from the start and from one
  tick before, the seek stops on the event's tick with the event done and
  posts its line once.

**The ring.** After such a click a ring flashes on the map round the place of
the event (`draw.c`, sums in `lv_event_flash.h`).

- It closes in on the place. It starts 3 squares out from the middle of the
  event's square and shrinks to 1 square, twice over, one second each time:
  2 seconds of real time. Each pulse is full strength for its first half
  second; the first then fades to 45% and the second to nothing. It runs the
  same with playback stopped. (The first ring was 1.5 to 2 squares in 1.2
  seconds, once: too small and too short.)
- With a team: a white band 2 points wide with a band in the team's colour,
  2 points wide, inside it. With no team: white alone, 3 points. A dark line
  1 point wide, 85% solid, each side keeps it readable on pale ground. The team is the one the
  marker over the seek bar has: the new owner of a pillbox or base, the
  killer of a tank (the tank's own team when it died with no killer), the
  team that lost the builder. The colour is the HUD panel's.
- **The place goes to the middle of the visible map area, every time.** The
  visible map area is the window less the menu bar and the sidebars: where
  the map shows. The middle of the event's square goes to the middle of that
  area, to the pixel (`lvFlashCentreScroll`; the view scrolls in pixels).
  Near an edge of the map the place goes as near the middle as the map
  allows, and the ring still marks it. A panel that floats over the map is
  not counted: only sidebars are.
- **The view that cannot move: an arrow at its edge.** With "Tank Centred"
  on, and in the game view (one player's own screen), the view belongs to
  the followed tank and the click does not move it. The ring is drawn when
  the place is in view. When it is not, an arrow at the edge of the view
  points to it and pulses as the ring does: white on a dark arrow, with the
  team's colour inside it. The game view draws the ring too now.
- **The fault the owner found (Oct 13): no ring, and poor centring.** After
  every jump the window called `lv_imgui_game_view_init_camera`, which puts
  the view on the camera slot's tank. That is right in the game view and
  wrong in the overview: the map left the event's place on each click, so
  the place was not in the middle and the ring was drawn where the view no
  longer was. The call was there before this branch (it also undid the
  centring of a highlight line). It is now made only in the game view
  (`lvFlashCameraFollowsTank`). Two smaller causes of poor centring went
  with it: the view was placed in whole squares, up to a square to one side,
  and the first ring's rule moved the map only for a place outside the view
  or within 2 squares of its edge. That rule is gone.
- A second click on the same line starts the ring again.
- One ring at a time. A new click starts it again at the new place. It ends
  at once when playback is moved by anything else: the seek bar, Rewind,
  Fast Forward, a step key. Playing on does not end it.
- It is drawn after the map and before the panels, so a panel over the map
  hides it. It is only in the viewer's own window: not in the viewer inside
  the game's lobby, not in the browser build, not in a saved picture.
- The viewer draws a frame every time round its loop, so the ring needs no
  redraw request of its own and costs nothing when none is running.

**Events with no place: no ring, and the map is not moved.** Player lines
(joined, left, changed team), chat messages, game lines (the start, the end
of the log) and a pillbox that changed hands while a tank carried it. With a
place: pillbox and base captured and stolen, builder killed, tank killed or
died, a ping, a highlight.

**Other notes.**

- A click on a live feed still does nothing, so there is no ring there.
- Unit tests: `lv_time_fmt_sums`, `lv_time_parse_sums`,
  `lv_event_flash_sums`, `lv_event_flash_arrow`, `lv_event_centre_scroll`,
  `lv_event_jump_keeps_place_in_view`, `lv_event_seek_exact`.
- The ring and the arrow are SDL drawing and were not run in the headless
  try-out. The faulty call was in the window's own file, which the unit
  tests do not link: `lv_event_jump_keeps_place_in_view` runs the window's
  steps by hand, the old way and the new.


### "Tank Builder Status": a marker over each tank for its builder

**What it is.** Options has a second check item under "Tank Armour and
Shells Bars": "Tank Builder Status". It is off until set and is saved
(`LOGVIEWER`, key `Tank Builder Status`). With it on, each live tank on the
map has a small marker for its builder (`draw.c`, sums in
`lv_builder_status.h`):

- Builder in the tank: nothing.
- Builder out of the tank: an arrow that points from the tank toward the
  builder, one of 16 ways. When the builder is on the tank's own square: a
  dot.
- Builder dead: a small parachute, then the time until the new builder is in
  the tank, as `M:SS`.

**Where it is.** Over the tank, middle over middle. From the tank up: the
tank's picture, a gap of 1 point, the two bars when they are on, a gap of 1
point, the marker. With the bars off (or a recording that holds no tank
stocks, where no bars are drawn) the marker is 1 point above the picture. It
follows the rules of the bars: none for a dead tank, none for a tank the view
does not draw, none when a map square is under 12 points, and only in the
viewer's own window (not the game view, not the viewer in the game's lobby,
not the browser build, not a saved picture).

**Sizes and colours.** The marker is made of square cells: the arrow and the
parachute are 7 cells high and wide, a digit is 3 cells wide and 5 high. A
cell is 1 point, and 2 points from 32 points to a map square. So the marker
is 7 points high at the usual zooms and 14 at the large ones. Every part is
white, for every team and for no team: the arrow, the dot, the parachute and
the time (`lvBuilderMarkColour`). Every part has a black edge of 1 point and
is drawn fully solid. (Until Oct 13 the arrow, the dot and the parachute were
in the team's colour and all of it was 75% solid, like the bars. The owner
found that too hard to see. The marker stands over the tank, not on it, so a
solid marker hides no tank; it can hide a little of the map above the tank.)
The digits are drawn by the viewer as cells; no font is loaded for them.

**Where the time comes from.** The game has no fixed time for a new builder.
When a builder dies, the new one starts at a random start square, flies in a
straight line to where the tank was at the death (3 world units a tick with
the usual rules, `lgm_helicopter_speed`; a map square is 256), lands when he
is within 16 units of that place (`lgm_arrive_tolerance`), and then walks to
the tank. So the time depends on the map and on where the tank goes. The
exact answer is only in the log, later than the death.

- **A log from a file: exact.** The viewer already decodes the whole log
  once at load, for the Events panel (`lv_screenEventScanRun`). That walk now
  also fills a table: for each player, each stretch from a builder's death
  (`log_LostMan`) to the first tick the log names no place for the builder
  (he is in the tank). The countdown is the end of that stretch less the
  playback time. It is by time, so it is right after any seek, and it runs
  backward when playback is stepped backward.
- **The countdown runs to "in the tank", not to the landing.** After the new
  builder lands he is an ordinary builder on foot: the marker changes from
  the parachute to the arrow, and the time stays beside the arrow until he
  is in the tank. A builder who is out and was not killed has an arrow and
  no time. A builder killed again on his walk back stays in the same
  stretch.
- **A live feed, or a log that ends before he is back: an estimate, with
  "~".** There is no later log to read. While the new builder flies, the
  time is his distance to the tank less the 16 units, over the speed (both
  out of the recorded rules), one 20 ms tick a step: "~0:17" for 10 squares.
  The log does not say where the tank was at the death, so the place of the
  tank now stands in. The estimate ends at the landing; after it the arrow
  has no time.
- Seconds are rounded up, so "0:00" is shown only when no time is left. A
  time is never under "0:00". Minutes stop at 99.

**The Tank panel.** The builder's line reads "Builder: dead, back in 0:07"
(the same time, with "~" for an estimate) when there is room for the longer
line. The longer line is not part of the width the panel's layout is chosen
by, so it never costs the panel its words; with less room the line reads
"Builder: dead" as before. In the try-out the time showed at 250 and 700
points wide, and not at 300 (text beside the gauges) or 180.

**One change to playback.** The log names a builder's place in every tick he
is out. The viewer cleared the places only in a tick that had events, so
through a run of ticks with no events the last place stayed: a builder back
in his tank was still "out" until the next event. Now the first tick of such
a run clears them too (`lv_screenQuietTick`). In a real round nearly every
tick has events, so this showed only in quiet moments.

**Other notes.**

- The state (in, out, dead; the places; the time) is read at the playback
  time under the client mutex, as the bars' values are.
- The Mac menu bar has the item by the pattern of the bars' item. It was not
  built.
- Unit tests: `lv_builder_status_sums`, `lv_builder_countdown`,
  `lv_builder_trips`, and `lv_builder_trips_scan` (a hand-built log: the
  estimate with no scan, the exact time after the scan, nothing for a
  builder who was not killed, and the table dropped by a new load).
- The marker is SDL drawing and was not run in the headless try-out. The
  Tank panel's line was: three states, nine sizes, no assert, no size
  clipped that was not clipped before. It was run again with the builder's
  picture (20 sizes, four passes, and the three dead states at nine sizes):
  the same sizes clip as before (60 by 60 and 20 by 20), no new one.


### The zoom of the map as a percentage (Oct 13)

The owner: "Can you put the zoom % printed somewhere visible".

**What zoom is in the code.** The overview has nine steps
(`g_zoomSteps` in `draw.c`): 0.5, 0.6, 0.7, 0.8, 0.9, 1, 2, 3 and 4. A step
is how many points of the window one pixel of the game's art takes. The
wheel, a pinch, the + and - keys, a controller and the Zoom menu all move
one step or pick one. Nothing fits the overview to the window, so its zoom
is always one of the nine. The game view has its own zoom, a whole number
of times from 1 to 4, cut to what the window has room for when the window
is full screen or maximized.

**100% is the game's own size:** one map square (16 pixels of art) on 16
points of the window. A point is one pixel of an ordinary display and more
of a dense one, so 100% is 16 pixels there and 32 on a display with two
pixels a point: the same size to the eye. The number comes from the size a
square is really drawn at, in pixels of the display, over 16 times the
display's pixels a point (`lvZoomPercent` in `lv_zoom_text.h`, fed by
`lv_drawZoomPercent` in `draw.c`). For the overview that is the step times
100 on every display. For the game view it is the pixels a pixel of art
takes over the true density, which can be a number such as 267% on a
display of 1.5 (the game view draws at a whole number of pixels).

**The text** is a whole number ("150%"). A value under 100 that is not
whole has one decimal ("62.5%"). The nine steps are all whole.

**Where it shows.**

1. The Controls panel, as the last piece of the bottom row, after the time,
   the time left and the tick: "Zoom 150%". The row stays in the middle.
   The zoom gives way first when the panel gets narrow: it never adds a
   line to the row. It is whole while it goes on the last line, then only
   the number ("150%"), then it is left out (`lvControlsZoomForm` in
   `lv_controls_layout.h`). The tooltip says what the number is and what
   changes it. A click does nothing: the viewer has no "back to 100%" of
   its own (the Zoom menu has the 1x step). In the tablet and Steam Deck
   layout the panel has no bottom row, so nothing shows there.
2. On the map: "Zoom 150%" in the bottom left corner of the map's part of
   the window (inside the sidebars, so it moves with them), for 1.5 s after
   the zoom changes, the last 0.5 s fading out. White with a dark edge, as
   the scenario's status line is. It is behind the panels, so a floating
   panel in that corner is over it. The other corners: the scenario's
   status line is top centre and the spectator badge top right. Nothing
   shows for the zoom the viewer starts with. It is drawn in the overview,
   on a live feed and in the game view.
3. The Zoom menu (Options): the percentage is its first line, greyed, a
   line to read.

- New string 2851 (the tooltip). The word "Zoom" is string 1229, the name
  of the menu.
- The embedded viewer inside the game has the same Controls panel, so it
  has the piece on the bottom row. It does not call the label on the map.
  The browser build does not call it either.
- Unit test: `lv_zoom_percent` (the percentage on three densities, the
  text, the fade, and the form on the bottom row at every width from 600
  down to 10). The headless try-out of the Controls panel at all its sizes:
  the size of the content is the same as before at every size, and the
  count of lines that were too wide before (20, none of them this row) did
  not change.

### Alt+Enter is full screen (Oct 13)

The owner: "can we also support alt+enter when in the log viewer?"

**How the game does it.** The key is read off the SDL event before ImGui is
given it: in a game in `sdl3imgui.cpp`, on the welcome screen and the lobby
through `dialogIsFullScreenToggleEvent` (`imgui_dialog_utils.h`). Both Alt
keys, Enter and the Enter of the number pad, no repeats. Outside a game the
command is `windowFullScreenChoose` (`winbolo.c`): `SDL_SetWindowFullscreen`
with no display mode chosen, which is a borderless window over the whole of
the display the window is on, then `SDL_SyncWindow`, then the setting is
saved (`[SETTINGS] FullScreen`) and comes back at the next start. SDL keeps
the size and place the window had. On a Mac the Window menu has "Enter Full
Screen" with Control-Command-F and a checkmark. Not on a Steam Deck or a
tablet, where the window is full screen from the start.

**The viewer had no full screen** of its own: no menu item, no key, no
setting. (The old Windows resource file has a greyed "Fullscreen" item that
nothing reads.)

**What the viewer does now.**

- Alt+Enter turns full screen on and off. The key is taken before anything
  else sees the event, in all three loops (a log, a live game, and the wait
  for a live game to start), so it works with the keyboard in any panel, on
  the map, playing or paused, with a log open or none, and the Enter never
  reaches a text box or a button. Held down it is one command: the repeats
  do nothing and are kept from the viewer too (the game lets them through;
  here a repeat of Enter in the comment box would be a new line). The rule
  is `lvFullScreenKey` in `lv_fullscreen_key.h`.
- The Windows menu has "Full Screen" as its last item, with a checkmark and
  "Alt+Enter" beside it (string 2013, which the game's settings already
  had). On a Mac it is "Enter Full Screen" with Control-Command-F and a
  checkmark, as in the game's Window menu (string 1249). No new string.
- The viewer's own window: the same SDL call as the game, so the same kind
  of full screen, and SDL puts the size and place back. The state is saved
  as `Window.FullScreen` beside the window's place and comes back at the
  next start, as the game's does. While the window is full screen its
  saved size, place and "maximized" stay as they were, so they are what it
  goes back to.
- WinBolo's window (Watch a Log, or a live game watched from the game): the
  game is not reading the keyboard while the viewer runs, so nothing would
  happen, and nothing is done twice. The viewer asks the game: it
  calls `lvGameFullScreenChoose` (`lv_fullscreen_game.c`, built into WinBolo
  only), which calls the game's `windowFullScreenChoose`. The game's front
  end has no code for this. So the game's own setting changes with
  the window and is saved by the game.
- The sidebars and panels stay as they are: to them it is a change of the
  window's size, the same as maximizing. The zoom of the overview does not
  change, so its percentage does not. The game view (the ` key) has a size
  of its own; when full screen is turned off while it shows, it is set up
  again so its window has that size.
- Not on a Steam Deck or a tablet, as in the game, and not in a browser.

- Unit test: `lv_fullscreen_key`.

### Pings show their kind and the game's marker (Oct 13)

The owner: "For chat and pings, can we put the type of ping it is and its
animation just like how the game does it?"

**What the log holds.** One `log_Ping` record for each ping (`log.h`,
written in `server_command_dispatch.c`): the sender's slot, the kind, and
the place in world units (256 for each map square). The time is the tick of
the record. The log does not hold who the ping was sent to, the object the
ping was on (a pill, a base, a tank), or which players had pings muted. It
does hold the lobby teams (`log_TeamSet`) and the alliances, so the viewer
works out who was sent a ping with the game's own rule
(`serverSimPingReachesClient`): the sender, the sender's allies, and the
players in the sender's lobby team.

**The kind.** A ping is still an event of the Chat kind, with the game's own
line as its text ("{player}: Attack!"). A kind of its own would drop pings
from the list of every user who has a saved choice of kinds. The marker at
the start of the Events row, and the marker on the timeline's event lane, is
now the game's icon for that kind of ping, in the game's colour for the
kind, with a line in the sender's team colour round it. The tooltip of the
marker is the game's name for the kind. A chat line keeps the speech bubble.
The icons and the colours are the game's (`ping_kinds.h`, `ping_icons.c`),
so a new kind of ping in the game needs no work here; a compile-time check
in `screen.c` fails if the count of kinds changes.

**The animation.** The game draws a ping with `pingMarkerDraw`
(`ping_marker.c`), which works out the blink and the fade from the age of
the ping and nothing else. The viewer calls the same function with the age
in game time: the time of the log now, minus the time of the ping. So the
marker is the game's at each speed, stands still while the log is paused,
and is right after a seek. A click on a ping's row goes to the tick of the
ping, where its age is 0 and the animation starts.

- The viewer kept the last 16 pings that the decoder passed. After a seek
  back, those were pings from later in the log. It now keeps the pings of
  the whole log (`lv_ping_view.h`, up to 4096, sorted by time; the scan of
  the log fills it) and shows those whose time is in the last
  `PING_DISPLAY_MS` before now. On a live feed it has the pings decoded so
  far.
- The game view drew pings already. The Overview map now draws them too
  (`lvDrawPingsOverBlit` in `draw.c`), with the sender's name as the game's
  full-screen map does.
- With the view of one player (the eye), only the pings that player was
  sent are drawn. With all teams, all pings are drawn. The Events list
  shows all pings in both views, as it shows all chat.

**Not done, and why.**

- No object for the ping: the log has none. The marker is at the place the
  sender clicked.
- A player who had pings muted still has the ping drawn in that player's
  view: the log does not hold mutes.
- On the Overview map the marker is drawn over the finished picture, so it
  is over the tanks. The game draws it under them.
- The Overview map of the viewer inside WinBolo (Watch a Log) is drawn by
  another path and has no ping markers yet. Its game view has them.
- No sound: the viewer plays no ping sound, and none was added.
- No setting to turn the markers off.
- More than 16 pings on screen at one time: the newest 16 are drawn.

To show more, the log writer must also write, at the time the ping is sent:
the players the ping went to, the object it was on, and the mutes.

### Save Log File: the name offered is the date, the map and the players (Oct 13)

The name offered was the log's own file name, which is often a key of
letters and numbers, or the map name for a downloaded log. It is now made
from the log, for both sources:

`2026-10-13 14.05 Oil Rig - Canuck, Eryan vs rooklift, Bot1.wbv`

The code is `lvSaveNameBuild` in `src/logviewer/lv_save_name.h` (no SDL, no
decoder). `lvSaveLogDefaultName` in `logviewer.c` collects the inputs under
the decoder mutex. The copy and the "already there" check did not change.

Calls made:

- **Shape.** Date, time, map, then ` - ` and the players. A part the log
  does not have is left out, with its separator.
- **Time of day** is `HH.MM` (a colon cannot be in a file name).
- **Date source**, in this order:
  1. The stamp at the start of the source file's name, when the server named
     the file (`<YYYYMMDD>t<HHMMSS>_<map>`, `serverDedicatedLogMakeAutoName`).
     It is the time that round's log was opened.
  2. The create time in the log header (`g_lv->gmeCreateTime`). The server
     writes `serverSimGetTimeCreated`, which is set one time, when the
     server is created (`src/server/server_sim.c:287`). So it is the time
     the server was started, not the round. It is right for a game that is
     hosted and played at once. It is early for a server that stays up.
     This is the reason the file stamp is first.
  3. For a file source, the time the file was last written.
  4. A downloaded log with no create time has no date in its name.
  The times are shown in the local time of the computer.
- **Players.** Every player the log has had, not only those in the game at
  the end. The event scan that runs at load fills the list (`LvRoster`), so
  there is no second pass over the log. A player who left and came back is
  one row. A player who took a slot that another left is a new row.
- **Teams.** From `log_TeamSet` only. The last team a player was on is kept.
  The names are grouped with ` vs ` only when every listed player has a team
  and there are two teams or more. Otherwise the names are one comma list.
  Alliances are not used, so an old log with no team records gets one list.
- **Limits.** Six names at most, then ` +N`. By team, the six are taken one
  from each team in turn. The name without `.wbv` is 120 bytes at most. A cut
  is never inside a UTF-8 character, and a cut removes a separator, a ` vs`
  or a count left at the end.
- **Characters.** `\ / : * ? " < > |` and control characters become `.`, as
  before. Dots and spaces at the end are removed. An empty name is
  `log.wbv`.
- Bots are named as other players. There is no bot mark in the file name.

Tests: `lv_save_name_parts`, `lv_save_name_limits`, `lv_save_name_roster`,
`lv_save_name_scan`.

### The map name of a log that opens in a lobby (Oct 13)

Report: the viewer shows the wrong map for some logs.

Cause, on the writer side. The server opens the next log when the lobby
opens (`handleLobbyEnter`, `src/server/server_dedicated_log.c:367`).
`logStart` writes the map name into the header at that time
(`src/bolo/log.c:1511`). The map then is the map of the round that just
ended. When the host chooses another map, the header is not written again.
The server knows this: it renames the file after the round
(`serverDedicatedLogRenameForMap`), but the name inside the file stays.

The viewer read the header only (`lv_logLoadCommon` in `screen.c`).

The writer is not changed on this branch. The log does hold the right name
in two later records, and the viewer now uses them:

- `log_MessageServer` with the text `Map changed to <name>`
  (`handleLobbyMapChange`, `server_dedicated_log.c:429`);
- `log_MapSkipApplied`, which carries the name of the map a skip vote went
  to (`src/server/sim/server_sim_vote.c:89`).

The event scan keeps the last such name between `log_LobbyEnter` and
`log_LobbyExit`. A name after `log_LobbyExit` is ignored: it can be the next
round's map. After the scan, that name replaces the header's name
(`lv_screenEventScanRun`), and the Game Information panel gets it. A log
with no lobby is not changed.

Limits:

- The text is the server's fixed English text. If the server changes that
  text, the viewer falls back to the header name.
- A live feed has no scan, so it keeps the header name.
- The text dump (`lv_dump.c`) still prints the header name.
- A smaller fix on the writer side is possible and is better: write a record
  with the map name at the round start (for example in `handleGameStart`,
  next to the `log_GameSettings` record). That needs a new record or a new
  field, so it belongs on its own branch.

Test: `lv_save_name_scan` (a hand-built log with two lobby map changes and
one after the round start).

### Tank panel: a dead builder is a parachute and a countdown (Oct 13)

The builder's line said "dead", with the time added only when there was
room. No line says "dead" now.

- **Pictures state:** the parachute in the picture column (as before), then
  `:` and the time: `<parachute>: 0:07`.
- **Words state:** `Builder:`, the parachute at the height of the line, and
  the time: `Builder: <parachute> 0:07`. `Builder:` is the word of the
  "Builder: in tank" line. A language with no colon in that line gets the
  parachute and the time only.
- **Gauges-only state:** the panel shows no builder line, as before.
- **Time not known:** the parachute alone. In the pictures state the colon
  is not drawn either.
- An estimate has `~` in front, as on the map.

One source. The map marker (`lvDrawBuilderStatusOverBlit` in `draw.c`) and the
panel (`lv_screenTankStatusSnapshot` in `screen.c`) both take the time from
`lv_screenBuilderReturn`. That was already so; no second copy existed. Both
now make the text with `lvBuilderDeadText` (`lv_builder_status.h`), which
gives the countdown or nothing. The time follows log time, so it is right
when paused, at each speed and after a seek.

Width. The line is measured with `~0:00` (`LV_BUILDER_COUNTDOWN_WIDEST`), so
the choice between words and pictures does not move as the time runs. The
words line is a little wider than "Builder: in tank" was at most fonts, so
the panel can go to pictures a few pixels sooner than before. A time of ten
minutes or more would be one digit wider than measured; a builder is never
that long on its way.

The strings `STR_LV_TANK_BUILDER_DEAD` and `STR_LV_TANK_BUILDER_BACK` are not
used now. Their ids stay in the four lang files.

Test: `lv_builder_dead_text`.

### A bot has the game's chip in front of its name (Oct 13)

The panels did not mark a bot. They now draw the chip the game draws for a
computer player in its players panel and on its tank labels: the two
pictures `data/ui/bot-cpu-red.svg` and `data/ui/bot-cpu-green.svg`, in their
own colours, as high as a line of text. No new picture was made.

Where a player's name is shown, and what changed:

| Place | Before | Now |
| --- | --- | --- |
| Tank panel, the name line | no mark | chip before the name |
| HUD panel, the name line of the one-player view | no mark | chip after the eye, before the name |
| HUD panel, tooltip of a tank slot | no mark | chip at the start of the line |
| Overview panel, a team's row and its tooltip | no mark | chip before each bot's name |
| Game Information, the scores table | no mark | chip before the name |
| HUD panel, tooltip of a pillbox or a base (the owner) | no mark | not changed |
| Item Information, the owner line | no mark | not changed |
| Events panel | names are inside the text of a line; no player column | not changed |
| Chat and newswire lines on the map | names are inside the text | not changed |
| Tank labels on the map | the text `[AI] ` before the name | not changed |
| Statistics text (`lv_stats.c`) | the text ` (bot)` | not changed |
| Save Log File name | no mark | no mark, on purpose |

Calls made:

- **Which chip.** The game draws the green chip for a bot on the player's
  side and the red chip for any other bot. The viewer has a player only
  while the view of one player is on. Then that player (when it is a bot)
  and its allies have the green chip, and each other bot has the red one.
  With no one-player view, each bot has the red chip. That is the chip the
  game draws when it does not know the sides. The rule is `lvBotChipFor` in
  `src/logviewer/lv_bot_chip.h`; `lv_screenBotChip` in `screen.c` gives the
  answer for a slot.
- **No room for a person.** A person has no chip and no gap. The one
  exception is the scores table: when one row has a chip, each player's row
  keeps the room, so the names are in one column.
- **Picture not there.** When the two files cannot be read, the width of the
  chip is 0 and each place draws what it drew before. There is no gap and
  no other mark.
- **Loading.** The viewer reads the two files itself (`bot_chip_texture` in
  `imgui_lv_icons.cpp`), with the game's own SVG loader
  (`imguiLoadSvgIcon`), at the game's size of 64 pixels. The game's copy of
  the textures is in `sdl3imgui.cpp`, which the standalone viewer does not
  link.
- **Tank panel sums.** The chip is part of what comes before the name
  (`lead` of `lvNameHeaderLayout`), so the name is cut sooner and the X
  button keeps its place. The room under the name line did not change.
- **Overview rows.** The check "do all the names go in the row" counts the
  chips. A row with a bot can go to "A +2" a little sooner than before.
- The chip has no tooltip of its own. The game's chip says "AI" on hover.
- The tank labels on the map keep `[AI] `. The label drawer the viewer
  shares with the game can draw the chip, but the viewer answers its
  questions with "not a bot" (`bolo_shim.c`). To change that is a change to
  the map and not to a list, so it was left.

Tests: `lv_overview_bots` (the rule, and where each name of a team's row
starts).

### A double click on an event puts its place in the middle of the map (Oct 13)

One click on a line of the Events panel, or on a marker over the seek bar,
goes to the event's time and tries to show its place. It does not always
end with the place in the middle of the map that shows:

- **"Tank Centred" is on.** The view is not moved at all. The view follows
  the tank, and an arrow at the edge of the view points to the place.
- **The place is near the edge of the world.** The view stops at the map's
  edge, so the place is off the middle. A view as big as the world (a large
  window, zoomed out) does not move.
- **A sidebar has "Make unused sidebar portions transparent" on.** The
  middle is the middle of the part no sidebar is on, though the map also
  shows under the clear part of that sidebar. That was a decision of the
  option: it does not move the map.
- **The event has no place.** Only the time changes.
- **A highlight line.** The time is 5 seconds before the moment; the place
  is put in the middle.
- **A panel that floats over the map** is not counted.
- **The browser build and the reel in the game** put the square in the
  middle in whole squares, not to the pixel, and have no ring.

The zoom is never changed by a click.

A double click now does more. The first click does what one click does, at
once: there is no wait to see if a second click comes. The second click
goes to the same time, shows the ring, and puts the event's square in the
exact middle of the map that shows. Only the edge of the world can hold it
off the middle.

Calls made:

- **What a double click turns off.** "Tank Centred" is the one thing that
  holds the overview's view. A double click turns it off, as the menu item
  does, and it stays off: the view does not go back to the tank. The tick
  in the menu and the saved preference change with it. The view of one
  player (the eye in the HUD panel) does not move the map, so it stays on.
  One click still leaves "Tank Centred" on and shows the arrow.
- **The game view** (one player's own screen) has no Events panel and no
  seek bar, so a double click cannot be made there. If it could, the camera
  would stay on the followed tank.
- **The map that shows.** It is the window less the menu bar and the solid
  sidebars. A sidebar whose unused part is clear counts as map. When the
  middle of that is under a panel of a clear sidebar, the middle of the
  part no sidebar is on is used. So with a clear sidebar a double click and
  one click put the place at two different points of the window. One click
  was left as it was.
- **A marker that stands for more than one event** goes to the first of
  them, as one click does.
- **An event with no place** only seeks, and "Tank Centred" is left as it
  is.
- **The second click is for the line the first one chose.** The first
  click's jump can move the rows of the list (the "now" line, auto scroll).
  The row under the mouse at the second click is not asked.
- Ctrl and a double click is two Ctrl clicks, as before.
- No text in the viewer tells the user what a click on an event does, so
  no text was changed.

The sum is `lvViewCentreScrollPx` in `src/logviewer/lv_event_flash.h`
(`lvViewCentreScroll` for the middle of a square). It takes the visible part
of the view as a start and a length, in points from the view's own start,
and the zoom. `lv_screenCentrePxIn` in `screen.c` moves the view with it.
`lvHostVisibleMap` in `logviewer.c` gives the visible part, and
`lvDockClearOpenMargins` in `lv_dock_clear.h` what the solid sidebars take.
`lv_windowCentreOnEvent` is the double click.

Tests: `lv_view_centre_scroll` (each square of the map at seven zooms and
five visible parts: the place is in the middle to a map pixel, or held by
the map's edge on the right side of the middle), `lv_dock_clear_map` (what
the solid sidebars take).

## Not tested on screen

Nothing on this branch was seen on a screen by the author. The rules of the
work did not allow the viewer or the game to be started. What was checked:
unit tests, and a headless run of the Overview and Tank panels.

Please look at, at least:

- The eye icon, the Overview panel and the Tank panel, floating and docked.
- Tank bars at each zoom step, and on a display with a pixel density of 2.
- The builder markers over the tanks, with the bars on and off: the arrow,
  the parachute and its time, on a file log and on a live feed.
- Sidebars on all four sides, and the layout after a restart.
- "Make unused sidebar portions transparent" on each of the four sidebars:
  the map under the clear part, a click and a drag there, the panel grips,
  a jump to an event with the option on (the place
  goes to the middle of the map that shows), and the option turned on and
  off (the map does not move).
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
