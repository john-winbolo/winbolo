# GoalHunter 1.5 — bot-specific rules

## Two-copy base/opt system (READ FIRST)
Every Lua file exists TWICE:
- `brains/GoalHunter_1.5/*.lua` — **base** (dev) version with debug code:
  `BRAIN_DEBUG_MODE` branches, `print2`, `viz.*`, `overlay_*` calls, etc.
- `brains/GoalHunter_1.5/opt/*.lua` — **stripped** production version with the
  debug constructs removed. Used by `--opt` runs AND by the WinBolo
  splash-screen background game (`bg_game.c`).

Rules:
- **Only edit the BASE files** (`brains/GoalHunter_1.5/*.lua`). NEVER hand-edit
  anything under `opt/` — it is GENERATED. A hand-added `print2`/`viz.`/
  `overlay_` line in `opt/` is a base/opt divergence a regen will silently
  undo (opt/ should contain zero `print2(`/`viz.`/`overlay_` call statements;
  the `local print2 = require("print2")` line survives because it begins with
  `local`, not `print2`).
- After editing any base file, regenerate `opt/` with `strip.bat` (see below),
  then copy BOTH trees into the build so the running BrainTest/WinBolo picks
  them up (`--opt`/splash game read from `opt/`):
  ```powershell
  Copy-Item "brains/GoalHunter_1.5/*.lua"     "build/Brains/GoalHunter_1.5/"     -Force
  Copy-Item "brains/GoalHunter_1.5/opt/*.lua" "build/Brains/GoalHunter_1.5/opt/" -Force
  ```
  Lua under `build/Brains/` is loaded fresh each BrainTest run (no rebuild
  needed for Lua-only edits); C changes still need a rebuild.

### Regenerating opt/ (REQUIRED whenever a base edit touches a stripped construct)
`lua_strip` removes any line beginning with `print2`, `viz.`, or `overlay_`,
plus single-line `if BRAIN_DEBUG_MODE ... end` blocks. Run `strip.bat` from the
brain dir (it invokes `../../build/Release/lua_strip.exe`, excludes
`los_stamp_cache.lua`/`shield_stamp_cache.lua`, and writes into `opt/`):
```
brains/GoalHunter_1.5/strip.bat
```
If `lua_strip.exe` is missing, build it once:
`MSBuild.exe build/lua_strip.vcxproj /p:Configuration=Release /p:Platform=x64`.

**Multi-line `if BRAIN_DEBUG_MODE ...` blocks break the stripper** — the first
line is stripped but the continuation (`and ...`/`then` on later lines) remains,
producing a Lua syntax error like `unexpected symbol near 'and'`/`'then'`.
ALWAYS write `BRAIN_DEBUG_MODE` conditionals on ONE line. If strip reports
`syntax error in stripped 'opt\<file>.lua'` it's almost always a multi-line
`if BRAIN_DEBUG_MODE ...` — fix the base file and re-run.

## Debugging output
- NEVER use console `print()` for debugging or diagnostics in this bot's Lua.
  With 4+ bots ticking at 50 Hz, console output is interleaved, unattributed,
  and unsearchable after the fact.
- Use `print2(...)` instead (per-module `local print2 = require("print2")`):
  it writes to `debug_sessions/<TS>/print2_bot<N>.log` — per-bot, timestamped,
  greppable, and stripped from opt/ builds automatically.
- For anything spatial or stateful, prefer a **visualizer** over log lines:
  register an id in `viz.lua` `M.IDS` and draw via `viz.rect/line/circle/text/
  hud_text` (also stripped from opt/). A toggleable overlay that shows exactly
  what the algorithm sees beats scrolling logs — and per project rules the
  overlay must match the actual algorithm shape/bounds.
- Rare acceptable `print()` uses: one-shot startup banners that a human must
  see in the console at launch (e.g. a TEST-AID roll announcement). Everything
  recurring goes to print2/viz.

## Logging locations
- `print2` messages → `debug_sessions/<TS>/print2_bot<N>.log` (one per bot).
- `astar.log` — one per session, shared across bots, for C-side A* per-step
  traces.
