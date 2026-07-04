# GoalHunter 1.5 — bot-specific rules

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
