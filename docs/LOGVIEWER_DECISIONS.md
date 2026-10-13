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
  picture, in the game's status layout). The Events panel has no column of
  icons.
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


### A gear on every panel, and "Make unused panel portions transparent"

Every panel (Controls, HUD, Overview, Tank, Game Information, the scenario
panel, Item Information, Events, Comments) has a small gear. A click opens
the panel's options: one check item, "Make unused panel portions
transparent". One helper does it for all nine: a panel opens its window with
`lv_panel_begin` and closes it with `lv_panel_end` (`imgui_lv_panel.cpp`).

**Where the gear is.**

- A floating panel: in the title bar, left of the close button, the same
  size as the close button. It takes no room from what the panel shows.
- A panel that is a tab in a sidebar (or tabbed with another panel) has no
  title bar of its own. Its gear is in the same place in the bar of tabs. It
  belongs to the tab that shows, as the close button of that bar does.
- With no bar at all (the bar of tabs of a dock node can be hidden): the
  bottom right corner of the panel, drawn over what the panel shows. Not the
  top right corner: that is where the Tank and HUD panels have the X of
  their name line.
- The gear is drawn with the draw list (the shared icon helper), as tall as
  the text, in the colour of the text, dim until the mouse is over it. Its
  tooltip is "Panel options". A patch in the bar's colour is drawn under it,
  so a long title or a tab does not show through it.
- A click on the gear does not start a drag of the window, and does not pull
  a tab out of its sidebar.

**What the option does.** The option is one a panel, off by default, kept in
the LOGVIEWER preferences as `Window.<Panel>.Transparent` (Yes/No) with the
same panel names the `Window.<Panel>.Visible` keys have, and saved by the
same "once it has been still for a second" save as the rest of the layout.

With it on:

- The background of the panel is not drawn: not the window's, not that of a
  list inside it, not the striped rows of a table, not the lines of a
  table.
- What the panel shows stays: text, pictures, gauges, buttons, sliders,
  scroll bars. The title bar (or the tab) stays as it is.
- **To keep text readable over the map, each block of what the panel shows
  has a backing**: a rounded box in the colour of the panel's background at
  75% of its opacity, 3 points larger than the block on every side. This
  was chosen over a dark outline round the letters: a backing keeps small
  text readable over the brightest squares of the map, and pictures and
  gauges get it too. The blocks are found from what the panel drew in the
  frame: rows that have something in them and are no more than 6 points
  apart are one block, as wide as its widest row (`lvBackingBlocks` in
  `lv_panel_opts.h`, unit test `lv_panel_backing_sums`). So lines of text
  under each other are one box, and the empty part of the panel under and
  beside them is see-through. A list with three rows in a tall panel has a
  box round the three rows only.
- The border shows only while the mouse is over the panel or the panel is
  being moved or resized.
- The option does not change the size of anything in the panel (measured in
  the headless try-out).

**Open question for the owner: clicks.** A click on the see-through part of
a panel still belongs to the panel. It does not go through to the map. That
is the simple and safe choice (the panel can still be moved and resized by
its empty part), but it can surprise: the map shows there and does not
answer. Making that part click-through is possible later.

**In a sidebar.** The map is not drawn under a sidebar, and that is not
changed. The option still applies to a docked panel: its own background is
not drawn, and the backdrop of the sidebar shows in the unused part. The
difference to see is small there. The option is for floating panels.

**Other notes.**

- The embedded viewer and the browser build draw the same panels with the
  same helper, so they have the gear too.
- The Mac menu needs nothing: the option is in the panel, not in a menu.
- Unit tests: `lv_panel_opts_sums` (key names, the place of the gear),
  `lv_panel_backing_sums` (the blocks).
- Headless try-out (Overview, Tank, Controls and HUD panels): option off and
  on, floating at 6 sizes down to 32 x 32, docked as tabs in a dock space at
  5 sizes down to 40 x 40: no assert. A click on the gear of a floating
  panel and on the gear in a bar of tabs opened the options; a click on the
  item changed that panel only; the floating window did not move and the
  tab stayed docked. The other five panels use the same helper but were not
  run there.


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
the saved names of the ticked kinds with commas between them, as the
`Events.Filter` key names one kind. "none" is an empty choice (an empty text
would read as "never saved"). A name from a newer build is ignored. It is
written at each tick.

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
- In the "Overlay events" list the four pillbox and base kinds have the
  picture of one no team holds.
- One helper draws the picture for every panel (`lv_item_picture` in
  `imgui_lv_icons.cpp`). When the game's sheets are not there the old plain
  shape is drawn. Unit test: `lv_event_overlay_picture`.
- Any other kind: a plain icon in the colour of the text on a dark square,
  with a **1 point line in the team's colour round it**. A tank kill has
  the game's own "dead" picture of the status panels and the colour of the
  killer's team (the team of the tank that died, when it died with no
  killer). A builder is a head and shoulders, in the colour of the team
  that lost it (the log does not say who shot it). Chat is a speech bubble
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
  clipped that was not clipped before.


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
