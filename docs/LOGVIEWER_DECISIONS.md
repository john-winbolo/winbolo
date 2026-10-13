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
- The viewer does write three keys outside `[LOGVIEWER]`:
  `[WINBOLO.NET] Host`, `[WINBOLO.NET] CountryCode` and `[DEVICE] SyncDirty`.
  For this reason the merge works by key, not by section. This differs from
  the instruction ("graft the LOGVIEWER section"); a section graft would lose
  those three keys or need a special list.
- If the file is missing or cannot be read, the viewer writes its whole copy,
  as before.
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
