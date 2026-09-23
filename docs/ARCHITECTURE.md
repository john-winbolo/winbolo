# WinBolo Architecture — Header Tiers and Include Rules

This document describes how the codebase is partitioned into header
tiers, which subdirectories may include which tiers, and the patterns
clients of `src/bolo/` must follow.

The design and rationale are tracked in `plans/protectcore.md`; this
document is the stable reference for the rules themselves.

## The four tiers

| Tier | Purpose | Representative headers |
|---|---|---|
| **T1 — Sim runtime (public API)** | The official front door to the simulation. Opaque handles; no direct struct access. | `server_sim.h`, `client_sim.h`, `client_net.h`, `client_enums.h`, `bolo_map_validate.h` |
| **T2 — Sim internals** | Implementation details of the sim: state structs, wire protocol, sub-systems. | `tank.h`, `players.h`, `game_sim.h`, `allience.h`, `client_sim_internal.h`, `server_sim_internal.h`, `client_sim_control.h`, `bot_manager.h`, `shells.h`, `mines.h`, `lgm.h`, `viewport.h`, `bolo_packets.h`, `netpacks.h`, `transport_udp.h`, `bolo_map.h`, `starts.h`, `pillbox.h`, `bases.h` |
| **T3 — Presentation data** | Read-only per-frame views the sim publishes for the renderer. | `viewport_types.h`, `client_render.h`, `client_ui_events.h`, `screentank.h`, `screenbullet.h`, `screenlgm.h`, `screencalc.h`, `frontend.h` |
| **T4 — Shared leaves** | Plain types and constants, and the handful of resolvers over an opaque `GameSim` that sit beside them. | `types.h`, `global.h`, `tilenum.h`, `gametype.h`, `platform_types.h` |

## Who may include what

| Directory | Allowed tiers | Notes |
|---|---|---|
| `src/bolo/` | T1 + T2 + T3 + T4 | Owns T2; contributes to all tiers. |
| `src/bolo/scenario_api/` | T1 + T4 | A third header directory beside `public/` and `internal/`. Holds the scenario write funnel (`serverSimApplyScenarioOp`), the policy vtable and tick registrations, and the POD types those calls take. Read by the `scenario_host` profile (the scenario runtime), by `sim_owner` to implement the funnel, and by `unittests` to drive it; nothing else sees it. Not in `public/` because these are server-authoritative entry points on the same footing as the lifecycle start functions — a frontend that wants to change the world sends a command, and a scenario is the one caller whose intent is applied to the sim directly. See "Privileged exceptions". |
| `src/scenario/` | T1 + T4 + `scenario_api/` | The scenario runtime, `scenario_static`, the one target built under the `scenario_host` profile. Finds the Lua file beside a map, boots the VM, parses the `scenario` table and the triggers in it — a trigger being data rather than code: a hook to run on, a list of tests and a list of actions — marshals `game.*` calls onto the funnel and T1 reads, queues bus events and drains them into hooks, runs those triggers through a router written in Lua, and checks a script for `-validate`. The router is `src/scenario/scenario_triggers.lua`, turned into `src/scenario/scenario_triggers.inc` by `tools/embed_lua.py`, which is run by hand and its output committed so the build needs no Python; the unit case `scenario_hooks_router_matches_source` is what holds the two together. It loads as a second chunk into the state the author's script has already run in rather than being concatenated on to it, so the author's file keeps its own line numbers in an error. The library also holds the function catalogue the editor is written from — one row per hook and per policy, with the parameters each takes. Sees `public/` plus `scenario_api/` and nothing in `internal/` or `src/server/`; a binding that needs sim state it cannot read gets a T1 accessor, never an include. Links `lua_static` PRIVATE. Frontends see only `scenario_host.h` (attach, detach, follow the map, is-active, name, description, script path, reload, last error, the scripts switch `scenarioHostSetEnabled` and the narrower `scenarioHostSetUploadScriptsEnabled` beside it, the map-has-script question `scenarioHostMapHasScript`, and `scenarioHostRegisterMapScripted`, which hands that question to a sim so its map lister can ask it), which includes `server_sim.h` alone and names no `scenario_api/` type, so a `gui`-profile file can include it. Both of those are the rule a call added here has to satisfy, not just a description of the calls there now: `server_sim.h` and the C standard headers are the whole of what this header may include, and every parameter and return type has to be a plain type or `ServerSim` / `ScenarioHost`. A call that would need a `scenario_api/` type in its signature belongs behind the funnel instead. Links `scenario_io_static` PUBLIC and holds no file format of its own: the container, `manifest.json` and the chunk written on to a map are that library's, and `scenario_pack.c` here is the half that has to validate a script first. Linked into every binary that hosts a `ServerSim` from a map file: WinBoloDS, WinBoloHeadless, WinBolo, WinBoloIOS, Android `main`, WinBoloUnitTests. Not wasm (never hosts), gym, braintest or the log viewer. MapEditor links it too, and is the one binary that links it without hosting anything: the scenario panel calls ten things here — `scenarioValidateSource` to check the script in its pane, `scenarioLuaRows` for the `game.*` completion list and for the ops a trigger's actions are written against, `scenarioLuaOpIsScalar` and `scenarioLuaOpIsAction` to ask what one of those ops takes and whether it changes anything, `scenarioLuaFunctions` and `scenarioLuaFnFields` for the function catalogue and the payload fields each of its rows reaches, `scenarioLuaOpDecidesRound` and `scenarioLuaFnDecidesRound` to ask whether one op, or one function, is the kind that ends a round, `scenarioLuaRoundDeciderAt` to walk that same list of ops for the sentence the panel writes when a file says mod, and `scnScriptPath` for where a script sits beside a map — and nothing else in this library. It also expands `SCN_HOOK_LIST` and `SCN_POLICY_LIST` at compile time, which is a reach of a different kind and not a call: the editor's own description table is pasted out of those two lists, so a hook or a policy added to either without a line of its own does not compile. It creates no `ServerSim`, attaches no host and ticks nothing — the check loads a script's top level once in a Lua state of its own against a stub `game` table, and is handed a NULL sim, which leaves out the one check that reads a map. The rules are checked either way: with no sim they go to `scenarioCheckRulesFromClassic`, and a rule's bounds belong to the field it is declared in rather than to a round, so the answer an editor gets is the answer the server gives. The tags are the check that genuinely reads a map, because it asks how many pills, bases and starts this one carries. On MapEditor's link line it sits ahead of the server group for the same reason it does everywhere else. |
| `src/scenario_io/` | T1 only | A scenario's files, `scenario_io_static`, built under the `runtime_only` profile. Reads and writes the WBSC container a scenario ships in and the `manifest.json` inside it, from a byte buffer rather than a path, and writes that container on to a map file. Holds the shape the two halves share: `ScenarioManifest`, the two enums a trigger's values and operators are stored as — `ScnTrigValueKind`, which says whether a value is a number, a string, a bool or the name of a payload field read when the trigger fires, and `ScnTrigCompare`, the seven tests plus the unknown that a word matching none of them reads as — the one table of operator names both readers resolve a file's spelling through, `scnManifestTrigOpName` out of it and `scnManifestTrigOpFrom` into it, so the JSON decoder and the script reader cannot drift apart over which word means which test, and the issue list a check fills. `ScnManifestRegion` carries two fields beside its rectangle that are runtime only — `owner` and `bit` — which `manifest.json` neither reads nor writes: the round's composer and `game.define_region` fill them while a round runs, so a package is the same bytes on disk and on the wire whether or not the server that wrote it knew about them, and a manifest that was never composed carries a bit of zero throughout. They stay in the struct rather than moving to a runtime-side type. No Lua, no `scenario_api/`, no `internal/` — it sees `public/` and nothing else, and it reads no sim state, so nothing here can depend on a round being in progress. `scenario_static` links it PUBLIC, so the six binaries that host a `ServerSim` get it without naming it. The log viewer is expected to link it directly and nothing else from `src/scenario/`: it reads a scenario's files and never starts a round. The map editor reads and writes those files on the same footing, and also links `scenario_static` for the ten calls named in the row above — still without starting a round. |
| `src/gui/` | T1 + T3 + T4 | The desktop renderer. Cannot reach into sim internals. |
| `src/mapeditor/` | T1 + T2 + T3 + T4 | Privileged exception (see below) — full T2 access for map-data editing. |
| `src/braintest/` | T1 + T2 + T3 + T4 | Privileged exception (see below) — dev visualisation tool, not shipped to players. |
| `src/gym/` | T1 + T2 + T3 + T4 | Privileged exception (see below) — ML training harness, not shipped in player builds. |
| `brains/` | T1 + T2 + T3 + T4 | Builds `bot_brains_static` (bot brain implementations — GoalHunter, ONNX backends). Compiles under the `sim_owner` profile because brain evaluation reads sim state directly. Not a frontend; every binary that ships bots links the same `bot_brains_static`, so the asymmetric-runtime bug class doesn't apply. |
| `src/server/` | T1 + T2 + T3 + T4 | Co-owner of the sim alongside `src/bolo/`. Most files compile via three libraries: `server_sim_static` (sim core: `server_sim.c`, `server_command_dispatch.c` and `servermessages.c` in this directory, plus the per-concern translation units under `src/server/sim/` that `server_sim.c` has been split into); `server_static` (dedicated-server runtime on top of it: `transport_udp_server.c`, `server_lifecycle.c`, `geolookup.c`, `server_dedicated_log.c`, `server_dedicated_log_path.c`, plus the per-concern translation units under `src/server/udp/` that `transport_udp_server.c` has been split into, plus `threads_static` PUBLIC-linked); and `threads_static` (the SDL-mutex thread manager — `threads.c` on every platform except Emscripten, where `threads_wasm.c` substitutes single-threaded no-ops with the same symbol surface). `threads_static` is consumed by every binary that ticks a sim, not only the dedicated server: in-process single-player builds (WinBoloIOS, android main, wasm winbolo, WinBoloUnitTests) link it directly; the four dedicated-server binaries get it transitively through `server_static`. Two more files are per-target sim runtime that ship inside WinBoloDS with T2 access via `bolo_grant_internal_source_access`: `servermain.c` (owns the dedicated-server `main()` and module globals) and `server_frontend_stubs.c` (stubs the T2 callbacks bolo's sim TUs expect when there is no UI). The replay-log subscriber `server_dedicated_log.c` (with its `server_dedicated_log_path.c` name helper) is a member of `server_static` above — built under the `sim_owner` profile, so it keeps its T2 access at the target level rather than through a per-file grant. It carries private state instead of reaching servermain globals, and is installed against the ServerSim bus by each host: `servermain.c` for WinBoloDS and `gameFrontSetupServer` for client-hosted games (the SDL3 client links `server_static` to host). See "Per-file T2 grants" below for the mechanism. |
| `src/headless/` | T1 + T3 + T4 | The headless runner. Not the same access as `src/server/` above: WinBoloHeadless is built under the `runtime_only` profile (`cmake/bolo_lib.cmake`), which puts `src/bolo/public/` on the include path and nothing else, so this directory has the desktop client's reach rather than the server's T2. Two things sit outside that. The target's one per-file T2 grant is for `src/bolo/transport_udp_client.c`, which is bolo's own translation unit compiled per-target rather than anything in this directory. And `headless_main.c` includes `../bolo/internal/server_sim_lifecycle.h` by relative path, which include directories cannot stop; a new reach into `internal/` from here wants a T1 accessor instead. |
| `src/wasm/` | T1 + T3 + T4 | Web build of the desktop client — shares the `src/gui/sdl3/` ImGui UI and the shared sim-driving cores, forking only the single-threaded driver (emscripten main loop in place of the SDL timer thread). See "Platform variants: share the logic, fork only the driver". |
| `src/android/` | T1 + T3 + T4 | Mobile renderer; uses T3 like `src/gui/`. |
| `src/ios/` | T1 + T3 + T4 | Mobile renderer; uses T3 like `src/gui/`. |
| `src/client_frontend/` | T1 + T3 + T4 | The shared sim-driving cores every client platform calls instead of keeping its own copy: `client_frontend_tick.c` (the alternating keys/game tick step), `client_frontend_connect.c` (the post-connect join/landing wait), `client_frontend_common.c` (the sim-state-guarded `frontEnd*` bodies). Not a library — the sources compile directly inside each frontend target (WinBolo, WinBoloIOS, android `main`, wasm `winbolo`) under that target's `gui` profile, so they see only `public/`. See "Platform variants: share the logic, fork only the driver". |
| `src/logviewer/` | T1 + T3 + T4 | Replays recorded games; uses T3 for the playback render path. |
| `src/winbolonet/winbolonet_core/` | T1 + T4 | Shared HTTP, async event queue, WBN key storage. Includes `server_sim.h` (T1) only. Linked by every WBN-aware binary. |
| `src/winbolonet/winbolonet_server/` | T1 + T4 | Server tracker calls (`server/register`, `server/update`, lobby/map/teams/balance). Linked by binaries that run a server: WinBoloDS, WinBoloHeadless, SDL3 client (SP host). |
| `src/winbolonet/winbolonet_client/` | T4 | User auth, comments. Linked by binaries with a UI: SDL3 client, LogViewer. |
| `tests/unit/` | T1 + T2 + T3 + T4 | Privileged exception (see below) — in-process tests of bolo internals. Not shipped to players. Also links four leaf `src/gui/sdl3` geometry files, which keep public-only access rather than borrowing this row's — see "Linked GUI sources". |
| `tests/`, `tools/` | T1 + T3 + T4 (by default) | Not currently wired through a profile. Tests that legitimately need T2 belong inside `src/bolo/tests/` and link against bolo's own target. |

**The enforced rule of thumb is two-tier**: outside `src/bolo/`, you get
everything in `public/` (T1 + T3 + T4) and nothing in `internal/` (T2).
The T3-vs-T4 distinction and the "don't use T3 in non-renderers" guidance
are policy enforced by review, not by the build. The four privileged
exceptions (`mapeditor`, `braintest`, `gym`, `tests/unit/`) get full
T2 access via dedicated CMake profiles. One further profile,
`scenario_host`, is not among them and grants no T2 at all: it sees
`public/` plus `src/bolo/scenario_api/`, a directory of entry points
and POD types rather than sim internals. It narrows what its consumer
may reach rather than widening it, which is why it is not an
exception to the rule above.

## What clients must do

1. **Reach the sim through T1 only.** Construct, drive, and query the
   simulation via `client_sim.h` and `server_sim.h`. Treat `ClientSim`
   and `ServerSim` as opaque handles. Do not assume struct layout.

2. **Use T1 accessors for anything sim-owned.** Tank state, player
   state, map data, bot state, network status — all of it is reached
   by calling a function on the sim handle, not by reading a struct
   field.

3. **Use T4 freely.** `types.h`, `global.h` and their neighbours exist
   precisely so every subdirectory can share basic primitives without
   coupling. Most of them have no dependencies at all. `gametype.h` is
   the one that is more than that: beside the loadout constants and the
   `gameType` enum it declares `gameTypeGetItems` and `gameTypeResolve`,
   which both take a `struct GameSim *` through a forward declaration —
   so the header itself still includes nothing but `global.h`, and the
   type stays opaque to anyone reading it. A caller that already holds a
   `GameSim` from `server_sim.h` or `client_sim.h` may call the two;
   nobody gains a way to reach sim state they did not already have.

4. **In the desktop renderer (`src/gui/`), treat T3 as read-only.**
   `viewport`, `screentank`, `screenbullet`, `screenlgm`, and friends
   are per-frame views the sim publishes for drawing. Read them.
   Do not mutate them. (Where the receive-side API can be tightened
   to `const`, it should be.)

5. **For remembered terrain, read `OverviewMap` — never `brainMap`.**
   Two map-shaped arrays hang off a `ClientSim` and only one of them
   answers "what has this player seen?".

   `clientSimGetOverviewMap` returns the fog memory: for each square,
   the tile it carried the last time the local player could see it,
   plus the regions they can see this instant. It stores the *drawn*
   tile — alliance-correct pillboxes and bases, mines only where the
   client knows one — and freezes a square when it leaves view. The
   memory is seeded from the map when it lands (the terrain is in the
   map file every client holds — for a client that joined a running
   game, the terrain as the round started; see "Per-client terrain"),
   so every square reads dimmed from the first frame; entities stay
   gated on the live regions, and a seeded square freezes like any
   other until the player can see it.

   Which regions are live follows the server's view policies rather
   than the client's own idea of what it may watch: the tank's own
   block, plus a block on each viewable pillbox, allied base and
   allied tank whose category the server allows. A category set to
   `off` contributes none. Under `key`, watching an item takes the
   tank's own block away for as long as it lasts — one view at a
   time, the way the classic screen leaves the tank behind while the
   player is in an item view. That is the map choosing what to draw,
   not what it holds: the server sends the recipient's own tank screen
   whatever it is watching, because the client predicts its tank
   against the ground round it. Under `decay` a
   region also carries a brightness — full while its proximity clock
   is inside the window, ramping down over the last
   `VIEW_DECAY_FADE_SECS` seconds of it, and gone once the clock runs
   out, at which point the squares under it freeze the way any region
   leaving view does. So the fog never shows as live a square the
   server is not feeding.

   `clientSimGetBrainMap` returns the terrain array brains reason
   over, and it is not a record of anything the player saw. It holds
   raw map bytes rather than tiles; `viewport.c` writes every square
   the viewport passes over into it; `screenBrainMapFillFromMap`
   fills it from the entire map for bots; and `mapSetPos` refreshes
   a square on the server sim whenever the terrain there changes,
   whoever can or cannot see it. Drawing from it would show ground
   the player cannot currently see: for the local host and for bots
   it carries terrain changes made anywhere on the map, and a bot's
   copy is filled from the whole map outright. A client on the wire
   is the narrower case — the server withholds the changes outside
   its viewports, so what never arrived cannot be in `brainMap`
   either — but the rule is the same one either way, and
   `OverviewMap` is the array that answers the question being asked.

   The two are deliberately not unified. Pointing brains at the fog
   memory would change how bots play, which is a gameplay decision
   and not a refactor.

## What clients must not do

The following patterns are the bug class this architecture exists to
prevent. They all manifest as **asymmetric runtime behaviour** —
features that work on one client (e.g. the desktop GUI) and silently
break on another (server, headless, gym, brain test, Android, WASM)
because the broken client did not run the same code path.

1. **Do not include T2 headers from outside `src/bolo/`** (the
   privileged exceptions — `mapeditor`, `braintest`, `gym`,
   `tests/unit/` — are bounded by the scopes listed below;
   `scenario_host` is not one of them, because
   `src/bolo/scenario_api/` is not T2). If you find yourself
   reaching for `tank.h`, `players.h`, `game_sim.h`, or anything
   in the T2 list, the answer is to add a T1 accessor on the sim,
   not to add another include exception.

2. **Do not build packets from the GUI.** `bolo_packets.h`,
   `netpacks.h`, and `transport_udp.h` are T2. GUI code that wants
   to send a chat message calls `clientSimNetSendChat(cs, ...)` on
   `client_net.h`; the wrapper builds a `ClientCommand` and routes
   it through the bus. The wire format is a private contract
   between `client_sim` and `server_sim`; callers go through the
   sim.

3. **Do not write to sim state directly.** Even if a field appears
   reachable through an internal header, mutating it from outside
   `src/bolo/` is the canonical asymmetric-runtime bug — the GUI
   client's copy of the world drifts from every other client's
   copy.

4. **Do not introduce new T2 exceptions casually.** The four
   exceptions that exist today (`mapeditor`, `braintest`, `gym`,
   `tests/unit/`) each carry a documented scope and a written note
   of what they rest on — see the "Privileged exceptions" section
   below. `scenario_host` is not a fifth: the scenario surface got
   its own header directory and a profile that sees `public/` plus
   that directory and nothing else, which is the shape a new door
   should take when the default answer will not do. `src/server/`,
   `src/headless/`, `src/wasm/`, `src/android/`, and `src/ios/`
   all run the sim and all participate in this bug class. A new
   exception requires the same justification structure: bounded
   scope, a written note of what it rests on, and a reason the
   asymmetric-runtime bug class doesn't apply. The default answer
   is still "add a T1 accessor".

5. **Do not include T3 from non-renderer binaries.** `src/gui/`,
   the mobile renderers (`src/android/`, `src/ios/`, `src/wasm/`),
   and `src/logviewer/` (for replay rendering) are the legitimate
   consumers of T3 headers. Server, headless, and gym should not
   reach for `screentank.h`, `viewport_types.h`, etc. — they don't
   draw. This rule is enforced by review, not the build: T3 lives
   in `public/` alongside T1 and T4, so the include path doesn't
   physically block a stray T3 include from a server TU. Catch it
   in code review.

## Renderer / window lifetime

The SDL3 desktop client creates its window and renderer exactly once, at
startup (`sdl3DrawSetup`), and keeps both for the process lifetime. The
Steam overlay installs its Metal-layer hook against that renderer when it
is created, so the renderer must not be destroyed and recreated at
runtime — doing so invalidates the overlay's hook and crashes on some
macOS versions.

Zoom changes and skin changes therefore reconfigure the existing renderer
in place rather than recreating it:

- **Zoom** — `sdl3DrawReconfigureZoom` rebuilds every zoom-dependent
  resource (tile atlas, fonts, status caches, man-status render target,
  and game render target) against the live renderer.
- **Skin** — `sdl3DrawReloadTiles` rebuilds only the tile atlas from the
  on-disk skin assets.

Neither path may call `sdl3DrawCleanup` + `sdl3DrawSetup`, which destroy
and recreate the window/renderer. Those two remain the building blocks for
process start/shutdown and for any future OS-driven surface or device-loss
recovery — which would key off a device-lost event, never a user-initiated
zoom or skin change. As a guard, `sdl3DrawCleanup` asserts (via
`sdl3DrawSetReconfigureGuard`) that it is never entered while an in-place
zoom/skin reconfigure is in progress.

## Renderer thread ownership (SDL3 desktop)

The SDL renderer — and the Metal/GL command queue behind it — is **not**
safe to touch from two threads at once. All GPU work must run on the thread
that created the renderer (the main/render thread). This matters because the
hosted server tick does **not** run on the main thread: `serverInstanceTick`
fires from SDL's timer thread (`hostedServerTimerCb`), and the sim, while
applying that tick, calls back into `frontEnd*` callbacks. If one of those
callbacks draws, it races the main thread's `SDL_RenderPresent` and segfaults
inside the GPU driver — a real, historically-shipped crash.

**Rule: a `frontEnd*` callback reachable from the sim must never issue
renderer calls directly.** It caches the state it was handed; the main
thread's per-frame render pass repaints from that cache (or straight from sim
state). Concretely:

- **Icon panels** (bases/pills/tanks) — the callback does nothing; the
  per-frame `sdl3DrawMainScreen` repaints every icon from `clientSimGet*Alliance`.
- **Resource bars, man-status, kills/deaths, newswire text** — the callback
  is *cache-only* (`sdl3DrawStatus*Bars`, `sdl3DrawSetManStatus`,
  `sdl3DrawKillsDeaths`, `sdl3DrawMessages` all just store values). The
  render-thread pass rebuilds the textures / draws the text from cache each
  frame (`sdl3RenderTankBarsTex`/`BaseBarsTex`, `sdl3RenderManStatusTex` inside
  `sdl3RenderStatusPanels`; `sdl3RenderCachedText`).
- **Full redraw** (`frontEndRedrawAll`) — reachable from the tick via
  `playersSetPlayer`; it early-returns off the render thread since the main
  loop already repaints every frame. Legacy WM_PAINT relic, not needed under
  the SDL3 per-frame loop.

The lock order in `clientmutex.c` (threads mutex outer, client mutex inner)
serializes the tick against `clientRenderFrame`, but **`SDL_RenderPresent`
runs outside the lock** (it must, or every tick would stall on vsync), so
serialization alone does not make off-thread draws safe — they must not
happen at all.

**Guard / regression net:** `sdl3DrawOnRenderThread()` records the renderer's
creating thread and is `SDL_assert`ed at every GPU choke point
(`sdl3RenderStatusPanels`, `sdl3RenderCachedText`, the texture rebuilds). Any
future callback that draws off-thread trips the assert immediately in dev
builds. When adding a new `frontEnd*` callback that needs to show something,
cache + let the per-frame pass draw it; do not call `sdl3Draw*`/`SDL_Render*`
from the callback. (ImGui-panel data written from callbacks — e.g. the player
list — is read on the main thread and must likewise treat its shared state as
cross-thread.)

## Writing a new frontend

A frontend is any binary outside `src/bolo/` that drives a `ClientSim`
or `ServerSim` through the public T1 API. Existing frontends are
`src/gui/sdl3/` (desktop), `src/android/` and `src/ios/` (mobile),
`src/wasm/` (web), `src/headless/` (no UI), `src/braintest/` (dev
tool), `src/logviewer/` (replay viewer), and the dedicated-server
binary built from `src/server/`. Note: the dedicated-server binary
is not a pure T1-only frontend — `servermain.c` and
`server_frontend_stubs.c` are per-target sim runtime with T2 access
via per-file grant. (`server_dedicated_log.c`, formerly in that set,
now takes T2 through `server_static`'s `sim_owner` profile instead.)
See the `src/server/` table row and "Per-file T2 grants" below.

### Client

A client frontend implements a set of `frontEnd*` callbacks declared
in `frontend.h` and drives a `ClientSim` through its per-tick API.

**Callbacks you must implement.** The sim calls these to push state
out — they are free functions linked statically against the sim, one
implementation per frontend binary. A minimal set:

- `frontEndDrawMainScreen` — main viewport render, called once per game tick
- `frontEndUpdateTankStatusBars` / `frontEndUpdateBaseStatusBars` — HUD updates
- `frontEndPlaySound` — sound effects from in-game events
- `frontEndStatusTank` / `frontEndStatusBase` / `frontEndStatusPillbox` — alliance and capture changes
- `frontEndSetPlayer` / `frontEndClearPlayer` — lobby roster updates
- `frontEndMessages` / `frontEndKillsDeaths` / `frontEndGameOver` — message and state updates
- `frontEndManStatus` / `frontEndManClear` — LGM (builder) state
- `frontEndRedrawAll` / `frontEndSetActiveClientSim` — UI bookkeeping

A headless frontend stubs them as empty functions; see
`src/headless/headless_frontend.c`. A real rendering frontend
implements `frontEndDrawMainScreen` against the per-frame view
buffers (`screen *`, `screenTanks *`, `screenBullets *`, `screenLgm *`)
the sim passes in.

**Setting up a client and connecting.** The complete frontend setup
for any game — SP, LAN host, LAN join, internet host, internet join —
is alloc-create-set-active-connect:

```c
ClientSim *cs = clientSimAlloc();
clientSimCreate(cs);
frontEndSetActiveClientSim(cs);
```

`clientSimAlloc` heap-allocates and zero-initialises. `clientSimCreate`
prepares the ClientSim for a connect call; game settings (gameType,
hiddenMines, startDelay, gameLength) install later via setters driven
by the connect handshake — the frontend never threads them through
construction. `frontEndSetActiveClientSim` must run **before** the
connect call: sync-replay fires `frontEndSetPlayer` /
`frontEndUpdateTankStatusBars` from inside the connect body and those
hooks look at the active-sim pointer to dispatch.

Then connect, either to an in-process ServerSim (single-player or
host-with-self):

```c
clientSimConnectLocalPassive(cs, serverSim,
                             playerName,
                             winbolonetGetCountryCode(),
                             clientType, clientFlags);
/* clientSimConnectLocal (active) for headless / gym — same shape,
 * but the local transport's tick also drives serverSimTick. Passive
 * leaves that to the host's timer thread. */
```

…or to a remote server via UDP (LAN, or internet via tracker — see
`client_net.h`):

```c
clientSimConnectUdp(cs, serverAddr, serverPort,
                   playerName,
                   winbolonetGetCountryCode(),
                   password, wbnApiToken, wbnServerKey,
                   wantRejoin,
                   trackerAddr, trackerPort);
```

The connect call does everything the frontend used to drive by hand:
picks the slot, installs the map blob the server sends, registers the
auto-subscriber so control events fan into the ClientSim, creates the
local tank, and applies the first snapshot. The frontend never sees
the map bytes, never picks a slot, never stamps a country code — it
supplies a `fallbackCountry` (cached from WBN) so the server has
something to fall back to when GeoIP can't resolve the joiner's IP,
and that's the only piece of identity the frontend hands over.

Post-conditions of `clientSimConnectLocal{,Passive}` are **synchronous**:
on successful return the map is installed, the local tank exists in
the assigned slot, the initial snapshot has been applied,
`clientSimGetConnectState(cs) == CLIENT_CONNECT_CONNECTED`. Post-
conditions of `clientSimConnectUdp` are **asynchronous**: transport is
bound and `JOIN_REQUEST` is queued; `clientSimGetConnectState(cs) ==
CLIENT_CONNECT_JOINING`. The remaining state arrives over subsequent
`clientSimNetTick` calls — frontends observe progress via
`clientSimGetConnectState` / `clientSimGetMapDownloadPercent` /
`clientSimIsInLobby` / `clientSimIsMapDownloadComplete`. No additional
install call is required; the transport drives the map install
internally on `CTRL_GAME_PHASE` transition or on no-lobby
`MAP_DOWNLOAD` completion.

On failure, the connect call writes a localised rejection reason into
the ClientSim that the frontend reads via
`clientSimGetConnectErrorReason(cs)`. One error-handling path covers
SP, LAN host, LAN join, internet host, and internet join.

**Ticking.** Alternating cadence — keys on odd ticks, full game on
even ticks. From `src/headless/headless_main.c`:

```c
InputPacket pkt;
clientBuildInputPacket(cs, &pkt, button, isShoot, isMine, isBrain,
                       isGameTick, playerNum, tick);

if (isGameTick) {
    clientSimGameTick(cs, &pkt, isBrain);
} else {
    clientSimKeysTick(cs, &pkt);
}

clientSimNetSendInput(cs, &pkt);   /* push to server or local sim */
clientSimNetTick(cs);              /* pull snapshot back */

if (isGameTick) {
    clientSimDisplayTick(cs, isBrain);
    /* eventually calls back into frontEndDrawMainScreen */
}
```

Lobby and chat actions go through dedicated send wrappers in
`client_net.h` — `clientSimNetSendChat`, `clientSimNetSendReady`,
`clientSimNetSendTeamSet`, `clientSimNetSendAllianceRequest`, etc.
Each wrapper builds a `ClientCommand` and submits it through
`clientSimSubmitCommand`, which routes to the reliable UDP carrier
or to `serverSimApplyCommand` under the mutex for in-process
clients. Never build wire packets directly; that's a T2 violation.
See "Adding a new client→server command" below for the recipe.

**Tearing down.**

```c
clientSimDisconnect(cs);
clientSimDestroy(cs);   /* frees the ClientSim allocation itself */
```

`clientSimDestroy` is safe on NULL.

### Platform variants: share the logic, fork only the driver

The same client ships on several platforms — desktop (`src/gui/sdl3/`),
web (`src/wasm/`), and mobile (`src/android/`, `src/ios/`). They differ
only in the *driver*: the mechanism that decides **when** to tick and
how to block or pump. Desktop ticks from an SDL timer thread; web ticks
from the emscripten `requestAnimationFrame` loop with a wall-clock
accumulator (no threads, no `SDL_AddTimer`); mobile ticks from the
platform's frame callback. Everything else — the sim-driving *logic* —
is identical across platforms and must be **shared and called, never
copied per platform.**

**The rule.** A platform variant supplies its driver and its
genuinely platform-specific rendering. It does **not** re-implement the
tick cadence, the connect/landing sequence, or the sim-state-guarded
`frontEnd*` bodies. Those are the contract, and the contract has one
implementation that every platform calls. This is the same discipline
as `threads_static`: `threads.c` on every platform, `threads_wasm.c`
substituted under Emscripten, one symbol surface, callers unchanged
(`src/server/CMakeLists.txt`).

**The hazard this prevents.** When a new platform is brought up by
copying a peer frontend's tick loop or connect body wholesale and then
trimming the platform-specific parts, the copy is correct *the day it
is made* — and silently drifts every time the original gains a step.
The dropped step is not greppable (the function still exists, still
compiles, still mostly works), so category audits — "is every callback
registered?", "is any body an empty stub?" — miss it. Only a
line-by-line diff against the source of truth finds a body that dropped
a step mid-logic. Sharing the body removes the diff target entirely.

**The shared cores — call them, do not re-derive them.** They live in
`src/client_frontend/` (see its row in the directory table) and compile
into each frontend target rather than as a separate library:

- **The tick cadence** — `client_frontend_tick.c`
  (`clientFrontRunTickStep`). The alternating keys/game step, plus the
  lobby/countdown branch (tick the transport only — do not run a game
  tick in lobby), the gunsight-adjust consume into `pkt.flags`, and the
  keys-half transport pump gated on `clientSimTransportTicksServer` (an
  active local transport advances the server inside `tick()`, so a
  second pump would double-step the sim; passive-local and UDP clients
  keep the pump for snapshot-latency and event-sampling reasons).
  Canonical shape: the tick block under "Ticking" above
  (`src/headless/headless_main.c`). A frontend that owns a human at the
  wheel adds the lobby branch and gunsight consume the headless example
  omits; those additions belong in the shared core, not in each
  platform's copy.
- **The connect/landing sequence** — `client_frontend_connect.c`
  (`clientFrontAwaitJoin`). After `clientSimConnectUdp` the
  state is asynchronous: poll `clientSimNetTick` while
  `clientSimGetConnectState` is `JOINING`/`DOWNLOADING_MAP`, **break as
  soon as `clientSimIsInLobby`** (lobby-enabled servers deliver
  `CTRL_LOBBY_SETTINGS` before the map, so `inLobby` latches true while
  the connect state is still `DOWNLOADING_MAP`), and accept the join
  when `state == CLIENT_CONNECT_CONNECTED` **or** `clientSimIsInLobby`.
  A landing check that only accepts `CONNECTED` hangs every lobby join
  until timeout. See the connect post-conditions above.
- **The sim-state-guarded `frontEnd*` bodies** —
  `client_frontend_common.c`. Several callbacks carry
  guard logic that is part of the contract, not cosmetics:
  `frontEndRedrawAll` skips the game-frame blit when `!clientSimIsRunning`
  or `clientSimIsInLobby` (so a mid-lobby roster update does not stomp
  the lobby UI); the status/draw callbacks drop stale-sim events via the
  active-`ClientSim` guard; the settings toggles persist through
  `gameFrontSaveCurrentPrefs`. These guards belong in shared code so a
  platform cannot silently omit one.

When adding or maintaining a platform variant, the review question is
not "does it compile and mostly run?" but "does it call the shared core,
or does it hold its own copy that can drift?" A copy is a latent bug.

### Server

A server frontend creates a `ServerSim`, manages player slots, drives
the per-tick sim, and (for non-local servers) speaks the wire
protocol to remote clients.

**Creating a server.**

```c
ServerSim *sim = serverSimCreate("maps/everard.map", gameType,
                                 hiddenMines, startDelay, gameLen);
/* Variants:
 *   serverSimCreateCompressed   — in-memory map blob
 *   serverSimCreateRandomMap    — procedural map generation
 */
```

**Startup config.** `serverInstanceStartup` is the single entry point
that takes a `ServerInstanceConfig` and applies every cfg field
(lobbyEnabled / skipLobby / emptyResetEnabled / hasPassword /
botBrainPath / botAiType / autoLockOnGameStart / ranked / openHost /
serverLocks / viewPlayer / uploadPolicy / maxPlayers / …) onto the
sim, plus drives the UDP transport / WBN / tracker / NAT-portmap
bring-up when `cfg.acceptRemoteClients` is true.

```c
ServerInstanceConfig cfg;
memset(&cfg, 0, sizeof(cfg));
cfg.acceptRemoteClients = true;            /* false for SP, gym, braintest, bg_game */
cfg.lobbyEnabled        = true;            /* mutually exclusive with skipLobby */
cfg.emptyResetEnabled   = true;
cfg.hasPassword         = (password[0] != '\0');
cfg.botBrainPath        = "brains/GoalHunter/init.lua";
cfg.botAiType           = aiFull;
cfg.udpPort             = port;
/* …other transport / WBN / tracker fields… */

serverInstanceStartup(sim, &cfg);
```

For headless callers that don't run the UDP / WBN stack (gym,
braintest, bg_game, headless `--fast`), pass `cfg.acceptRemoteClients
= false` and `cfg.skipLobby = true` to enter running state directly.

**Player management.** Players join through the connect path on the
**client** side — `clientSimConnectLocal{,Passive}` calls
`serverSimLocalJoin` internally for in-process joins;
`clientSimConnectUdp` queues `PACKET_JOIN_REQUEST`, which the UDP
server handler turns into a join. Frontends never call into the
server's join machinery directly. Bots flow through a separate
publish path:

```c
serverSimRemovePlayer(sim, playerNum);
serverSimCreateBot(sim, playerNum, brainPath, "Name", aiFull,
                   gameType, hiddenMines);
```

The bot pool is process-global — call `serverSimBotPoolInit` once at
startup; multiple ServerSims share it.

**Lobby → running transition.** Driven by the all-ready detector
inside `serverSimLobbyCheckAllReady`, fired from the `CMD_READY` arm
of `serverSimApplyCommand` (`src/server/server_command_dispatch.c`)
after a ready-toggle. Every client — UDP, SP-host, bot — reaches the
same arm via `clientSimSubmitCommand`. The detector branches on
`sim->worldPreLoaded`:

- Fresh sim from `serverSimCreate*` → `serverSimStartGameInPlace`
  (synchronous; no countdown). Used by SP host on first round.
- Subsequent rounds (after a `serverSimResetGameWorld`) → countdown
  state, then `serverSimStartGame` on countdown expiry. Used by MP
  and by SP's second-and-later rounds.

The detector entry points are private to `src/server/` — frontends
never call `serverSimStartGame` directly; they fire
`clientSimNetSendReady(cs, true)` and the server side decides what to
do with that signal.

**Ticking.**

```c
serverSimApplyInput(sim, &inputPacket);   /* buffer one client's input */
serverSimTick(sim);                       /* advance world */
serverSimBotTick(sim, aiYes);             /* run brains */
/* Build and broadcast snapshots through the transport */
```

**Subscribers — control event delivery.** Servers fan out
`ControlEvent`s (alliance changes, player joins, phase transitions,
etc.) to registered subscribers:

```c
SubscriberHandle h = serverSimRegisterSubscriber(sim, deliverCb, ctx);
/* later */
serverSimUnregisterSubscriber(sim, h);
```

For in-process single-player, register a `ClientSim` directly with
the convenience helper:

```c
serverSimRegisterClientSubscriber(sim, cs);
```

**Tearing down.**

```c
serverSimDestroy(sim);   /* frees the ServerSim allocation itself */
```

`serverSimDestroy` is safe on NULL. Unlike pre-protectcore code, the
caller must NOT call `free(sim)` afterward — `serverSimDestroy` owns
the deallocation.

## Adding a new capability that needs sim internals

When a client outside `src/bolo/` legitimately needs something that
today only lives in T2, the fix is **always** to add a T1 accessor:

1. Add the function signature to `client_sim.h` or `server_sim.h`
   (whichever side owns the state).
2. Implement it in `src/bolo/` against the internal structs.
3. Call it from the client. Do not add the T2 header to the
   client's include line.

If the new accessor would expose a T2 struct by value, forward-declare
it as opaque in the T1 header and add getter/setter functions for
the fields the client actually needs. Opacity is what makes T1 a
real boundary instead of a documentation convention.

## Adding a feature end-to-end

A feature that touches input, sim state, and rendering crosses several
layers. The layering rule is the same as for accessors — frontends do
not reach into sim internals — so a multi-layer feature is usually a
small piece of code in each layer rather than one big change in one
file.

The general shape:

1. **Sim logic** lives in `src/bolo/`. State changes, world updates,
   server-side authority, network reception — all internal. The
   `.c` and T2 `.h` for this code live alongside the existing sim
   modules.
2. **Public surface** lives in `src/bolo/public/`. Anything a
   frontend needs to call (to send a request) or read (to render
   state) gets a T1 declaration. If the feature is network-visible,
   the send wrapper lives in `client_net.h`; if it's a pure read,
   it lives in `client_sim.h` or `server_sim.h`.
3. **Frontend** lives in `src/gui/sdl3/` or the platform binary.
   It calls the T1 send wrapper for outgoing actions and consumes
   either a `frontEnd*` callback or a T1 accessor for incoming
   state. It never includes T2 headers, builds wire packets, or
   mutates sim state directly.

**Worked example — adding a chat message.**

| Layer | Where the code goes |
|---|---|
| GUI input | `src/gui/sdl3/` — chat dialog calls `clientSimNetSendChat(cs, dst, msg)` |
| T1 send wrapper | `client_net.h` declares `clientSimNetSendChat`; `client_net.c` builds a `ClientCommand` and calls `clientSimSubmitCommand` |
| Submit | UDP: enqueue in the per-connection command queue; eager-send a `PACKET_COMMAND_TICK`. Local: hand `&cmd` to `serverSimApplyCommand` under the mutex |
| Server reception (UDP) | `serverProcessPacket`'s `case PACKET_COMMAND_TICK:` loop decodes each entry via `commandCodecDecode`, dedupes by `cmdSeq`, calls `serverSimApplyCommand`, and unicasts back a `PACKET_COMMAND_ACK` |
| Dispatcher | `applyCommandInner`'s `case CMD_CHAT:` arm in `src/server/server_command_dispatch.c` validates, mutates state, publishes `CTRL_CHAT` via `serverSimPublishControl` |
| Server fanout — in-process | The publish reaches local ClientSims, bots, host UI, replay log |
| Server fanout — wire | The per-client UDP subscriber encodes `CTRL_CHAT` via the control codec and unicasts `PACKET_CHAT_BROADCAST` to each remote client |
| Client reception — in-process | Local subscriber's `deliverCb` calls `clientSimApplyControl`; its `CTRL_CHAT` case hands the message off to `frontEndMessages` |
| Client reception — remote | UDP client decodes `PACKET_CHAT_BROADCAST`, builds a `ControlEvent`, calls `clientSimApplyControl` — same final path |
| Render | Each frontend's `frontEndMessages` implementation paints the chat line |

The two "Server fanout" rows are both required. Skipping the in-process
publish breaks SP-with-bots (the bots never hear the chat). Skipping
the wire broadcast breaks networked play (remote clients never hear
it). Either failure is the asymmetric-runtime bug class. See
"Adding a new server event" below for the recipe.

The same shape applies to any feature with a server-side authority
step: input → T1 send → wire → server validation → control event →
client subscribers → frontend callback. Variants:

- **Local-only UI changes** (e.g. camera zoom): skip the network leg —
  the frontend mutates its own state and calls a T1 setter if any sim
  state is involved (`clientSimSetXOffset`, etc.).
- **Pure read** (e.g. a HUD that shows tank velocity): no input/send
  step. Add a T1 getter (`clientSimGetTankVelocity`) and call it from
  the renderer.
- **Server-driven state with no client input** (e.g. round timer): no
  send leg. The server publishes a `ControlEvent`; clients receive it
  through their subscriber callback and update.

**What never changes.** A feature that adds a render path also adds
implementations to every frontend binary that ships it — `src/gui/sdl3/`,
`src/android/`, `src/ios/`, `src/wasm/`, `src/headless/` (stub),
`src/braintest/` (stub), `src/server/` (stub). If you add a
`frontEnd*` callback to `frontend.h`, every backend that links the
sim must provide an implementation (even an empty one) or the link
fails. Use that as the cue: if linkage breaks on the headless or
server stub, you've extended the frontend surface and need to add
the stub.

## The channel-mux reliability layer

Reliable, ordered delivery over UDP is provided by one primitive — a **channel
mux** (`src/bolo/internal/channel_mux.h`, `src/bolo/channel_mux.c`) — in place of
the several hand-rolled reliable queues, a standalone control-event carrier, and
three bespoke bulk chunkers that preceded it. The mux is built and tested
**entirely off-socket**: it turns logical sends into byte frames and consumes byte
frames back into messages; the transport only shuttles those frames. It owns no
socket.

**Five channels, three flavors.** Each channel is an independent substream with
its own sequence space, so loss on one never stalls another (head-of-line
blocking is per-channel):

| id | channel | flavor | carries |
| --- | --- | --- | --- |
| 0 | `CHANNEL_GAME` | message | reliable must-arrive game events: kills, mine reveals, server / assistant / LGM-lost text |
| 1 | `CHANNEL_MAP` | message | terrain-change events (`EVENT_MAP_CHANGE`), per recipient — a client on the wire is sent only the changes inside its own viewports; in-process clients take every change (see "Per-client terrain") |
| 2 | `CHANNEL_CONTROL` | message | lobby / chat / alliance / phase control events |
| 3 | `CHANNEL_BULK` | stream | map preview / upload / download / resync blobs |
| 4 | `CHANNEL_GAME_EFFECT` | best-effort | ephemeral game events: sounds, explosions, captures, and pill/base state deltas |

A **message** channel delivers each `channelSend` as one whole logical message, in
order, exactly once. A **stream** channel (`channelStreamSend`) appends to a byte
stream the core splits into segments and the receiver concatenates in order — this
is the bulk-transfer fragmentation layer, so no separate chunker exists.

A **best-effort** channel (`channelSendBestEffort` / `channelReceiveBestEffort`) is
fire-and-forget: a segment is delivered on arrival with no ack, no retransmit, and
no in-order wait. It drops a stale or duplicate seq (anything at or below the
highest already delivered) and, when its ring overflows, drops the *oldest* pending
segment to make room rather than disconnecting the slot — a high-rate effect stream
must never stall the connection. This is a separate send/receive core, not an
`if (reliable)` branch on the reliable readers. The backstop for a dropped state
delta (a pill/base update) is the next periodic full snapshot re-sync, which
restores the authoritative state regardless of which deltas were lost.

**Per-channel sizing.** Each channel sizes its send/receive rings to its own
traffic rather than one uniform pair (`channel_mux.h`): the game channel is deep
with tiny segments (window 512 × 16 B), map likewise (128 × 16 B), control
is shallow with large segments (64 × 1024 B — one control event, e.g. the full
brain list, maps to one segment and one datagram), and bulk is a
bandwidth-delay-product
window of stream segments (96 × 256 B). The best-effort game-effect channel is
64 × 16 B — sized to one tick's burst of effect events plus margin, not to
retransmit depth, since nothing is ever held for resend. The whole mux is a plain
value type — the rings are embedded, no heap allocation.

**One reliability core** serves both reliable flavors (message and stream):
cumulative ack ("received everything below `ackedSeq`"), full-tail-resend on a
per-channel retransmit timeout (RTO derived from the ping RTT), window-bounded so a
resend re-sends at most one window, and a receive-side reorder buffer so a
jitter-reordered early segment is held and delivered in order rather than dropped.
Each ack record also carries `highestSeen`, an exclusive upper bound on what the
receiver has buffered on that channel; when `highestSeen > ackedSeq` a segment past
the in-order point is missing, so the sender rewinds and retransmits its unacked
tail immediately (~1 RTT) — a NAK fast-retransmit — instead of waiting out the RTO,
which stays as the backstop. A caught-up channel sends nothing — no steady-state
storm. Window overflow without acks is a stuck or malicious peer and disconnects
the slot; it is never a silent drop. This core drives only the reliable channels;
the best-effort channel is never acked and has no retransmit or NAK.

**How frames ride datagrams.** One shared channel-frame codec — an ack list (each
record `channelId` + `ackedSeq` + `highestSeen`) then a segment list — is carried
three ways, never as a phase fork: appended as a
**trailer** after a per-tick snapshot (server → client) or after an `InputPacket`
(client → server), or as a standalone `PACKET_CHANNEL` when no primary packet is
due. The receiver recovers the frame as the bytes past the primary packet's parsed
end. `channelRecvFrame` is the single bounds-checked parse site (the fuzz target).
Because the carrier is per-datagram packing, not a logic branch, control events
flow identically whether or not snapshots are running — the lobby-vs-running
duality the old `PACKET_CONTROL_TICK`/`PACKET_CONTROL_ACK` carrier needed is gone.

**Game-boundary and mid-transfer resets (`CTRL_CHANNEL_RESET`).** Channel sequence
spaces are monotonic — they never restart at 1. At game start the server drops the
previous game's undelivered game/map tails by advancing the *send* baseline on
channels 0 and 1 (`channelResetSend`: `ackedSeq = nextSeq`, low seqs are never
reused) and ships a per-client `CTRL_CHANNEL_RESET` on the persistent control
channel carrying those baselines; the client adopts them (`channelResetExpected`)
so a previous-game straggler dedups away. The same event re-bases `CHANNEL_BULK`
when a lobby map change restarts an in-flight download. Map resync additionally
carries a generation (`resyncGen`) so a terrain change tagged older than the
installed map is dropped once a fresh blob lands.

**Bulk transfers (`src/bolo/bulk_transfer.c`).** A sized blob (a map) rides
`CHANNEL_BULK` behind a small app-level stream header (`kind`, `gen`, `totalSize`,
`path`). `BulkSender` feeds the header+blob into the channel's staging buffer as
the window drains, serialized so two transfers never interleave on one peer's
stream; `BulkReceiver` reassembles header-then-body from the in-order fragments and
hands the parsed header to a **recipient-agnostic sink** that decides where the
blob lands and what to do on completion — so preview, upload, download, and resync
(`BULK_KIND_*`) all ride the one machinery.

The *contents* of a map download or resync blob are not shared between clients:
each is built for the one slot it is going to, from that slot's own copy of the
terrain (`serverSimGetCompressedMapFor`), so a client is never streamed ground it
was culled out of. See "Per-client terrain". A lobby map preview and a lobby map
change are lobby-wide and do serialise the live map.

A join download is pull-started: the server arms it at `JOIN_ACCEPT` but streams
only after the client's `PACKET_MAP_DL_READY` confirms its receive buffers exist,
so the stream head can never race the accept that sizes them. The client re-sends
the READY if the stream never starts or stalls outright, and the server answers a
re-ask with a full restart behind a `CHANNEL_BULK` re-base — the recovery for a
transfer whose bytes the channel has already acked but the receiver could not
keep (e.g. its framing was reset mid-body). The restart recompresses that slot's
own copy and re-sends `JOIN_ACCEPT` first, because the client drops a stream whose
header size disagrees with the size its accept carried.

**Off-socket testability.** Because the reliability burden lives behind a pure
byte-buffer seam, the whole loss / reorder / dup matrix is a unit test with no
sockets or threads: `tests/unit/test_channel_mux.c` drives two `ChannelMux`
instances through a seeded loss/reorder/dup shuttle, and
`tests/unit/test_bulk_transfer.c` does the same for stream reassembly. The live UDP
transport is a thin adapter over this seam (it still binds a real socket; the
loopback harness exercises that end-to-end in-process). New reliable wire code
therefore lands on this layer — a new control event in `transport_control_codec.c`
on `CHANNEL_CONTROL`, a new transfer in `bulk_transfer.c` on `CHANNEL_BULK` — not
in a hand-rolled socket send. (The reliability logic was extracted into these small
off-socket modules rather than shrinking `transport_udp_server.c`, which stayed
large as per-client mux/bulk wiring moved in.)

## Per-client terrain

The server does not send every terrain change to every client. It keeps, per
slot, a copy of the terrain that client is supposed to have —
`clientKnownMap[MAX_TANKS]` in `ServerSim` — and every question of the form
"what map does this client hold?" is answered from that copy rather than from
the live map.

**What writes the copy.** A slot the UDP transport has marked culled
(`shadowCulledSlots`, set when the transport takes the slot and cleared when it
goes) has each map event tested against that client's viewport set — the same
rects `serverSimBuildViewports` produces for entity culling: the tank's own
screen plus a screen for each pillbox, base and allied tank its view policies
allow, plus, while a `key` category is granting the item the client reports
watching, that item's screen beside them. An event inside the rects is queued on
`CHANNEL_MAP` as before *and* its new terrain byte written into that slot's
copy. An event outside them is skipped and the copy keeps the old byte — that
staleness is the record of what the client is owed. The write is tied to the
enqueue, not to the test, so a queue-full drop also leaves the copy stale and
heals the same way. In-process clients — the local host player and bots — are
never marked culled: they take every change, and their copy tracks the live
map. The one gap is a tick that produces more than `MAX_MAP_EVENTS` terrain
changes: the overflow never reaches any copy, and since each client's checksum
is taken over its own copy, the two ends still agree and nothing asks for a
resync. It takes a pathological tick to reach, and no normal round comes near
it.

**How ground fills in as a client drives into it.** There is no "this client
entered an area" event; the disagreement itself is the trigger.
`serverSimShadowSweep` compares a slot's copy against the live map *inside that
slot's current rects* and turns each differing square into an
`EVENT_MAP_CHANGE` on that client's queue, advancing the copy as it emits. It
runs on a slot-staggered `MAP_SWEEP_STRIDE` cadence (each slot comes up every
five frames) and emits at most `MAP_SWEEP_MAX_EVENTS` a sweep, and never more
than the queue has room for, so driving into long-changed ground cannot crowd
out live changes; whatever is left over is still a difference and goes out on a
following sweep. The sweep is held off while a resync is in flight, since that
transfer carries the whole copy anyway.

**The checksum is taken over the copy.** `hdr.mapChecksum` is stamped on each
client's own full-sync tick from `mapCalcChecksum` over
`clientKnownMap[slot]`, not over the live map. This is what keeps a culled
client out of a resync loop: it legitimately does not hold the live map, so
hashing the live map would give it a mismatch it could never clear — three
mismatches ask for a resync, the resync would deliver a map that still does not
hash to the live one, and the client would keep asking until it hit
`MAP_RESYNC_MAX_ATTEMPTS` and disconnected. Hashing what the client was
actually sent makes the two ends agree by construction once the sweep has
caught up. The recording paths (`noCull`) have no client copy behind them and
hash the live map.

**Resync and join read the copy too.** A resync blob is built by
`serverSimGetCompressedMapFor(sim, slot, …)`, which serialises that slot's copy
alongside the current (public) pill/base/start structs — streaming the live map
there would hand a client exactly the ground it was culled out of. A wire client
joining a game already running is re-seeded from the round-start copy of the
terrain (`serverSimShadowSeedRoundStart`) and its download blob comes from that
same copy, so arriving — or leaving and rejoining — tells it nothing about what
has happened since the round began; it is paid the differences by the sweep as
its viewports cover the ground. A lobby or countdown joiner keeps the current
map, which nothing has changed yet.

## Adding a new server event

A server event is anything authoritative the server decides happens
in the world — a player joined, an alliance changed, the game phase
advanced, the map was swapped. Every such event must reach **every**
audience that participates in the sim:

- In-process consumers: local `ClientSim`s in single-player, bots in
  the same process, host-side UI surfaces, replay log writers.
- Remote network clients: every UDP-connected `ClientSim` that has
  joined the server.

**Both audiences always have to see the same events.** A bot in a
single-player game and a remote player in a UDP-connected game are
the same kind of consumer from the sim's point of view — they each
have a `ClientSim` driving rendering, decisions, and state. If one
audience sees an event the other doesn't, that's the asymmetric-
runtime bug class this architecture exists to prevent: features that
work over the network but break in SP, or work in SP but break under
network play, because the broken side never got told.

### One publish, fanned out by subscribers

Server-authoritative state changes flow through one call:
`serverSimPublishControl(sim, &evt)` (defined in
`src/server/sim/server_sim_control.c`). The implementation walks the registered
subscriber array and invokes each `deliverCb`. Two kinds of
subscribers are attached:

- **In-process subscribers** for every local `ClientSim` (SP humans,
  bots in the same process), host-side UI surfaces, and replay log
  writers. They consume the event directly.
- **A per-client subscriber** registered by the UDP transport in its
  join handler — `udpClientDeliverControl` in
  `src/server/transport_udp_server.c`, one per connected remote
  client. Its deliver callback runs the event through a per-variant
  codec encoder (`src/bolo/transport_control_codec.c`) that produces
  the wire bytes, then unicasts them to that one client.

A single publish therefore reaches every audience by construction: a
publish that reaches one audience reaches the other by definition,
and the asymmetric-runtime bug class is closed.

This removes the easy mistake, not compile-time enforcement. The old
`transportUdpServerBroadcast*` helpers are gone, so the easy copy-
paste pattern that produced asymmetric runtimes no longer exists.
But `udpSendTo`, `packHeader`, and the `PACKET_*` constants are
still reachable inside `transport_udp_server.c`; a contributor could
still hand-roll a wire-only broadcast. The documented recipe just
makes it obvious why they shouldn't. This is structurally weaker
than the include-tier compile-error enforcement that `src/bolo/`'s
public/internal split provides.

### Where new wire-format code lives

| Packet kind | Lives in |
| --- | --- |
| Backed by a `ControlEventType` variant (state changes — joins, leaves, alliances, chat, lobby, phases, balance, shutdown) | `src/bolo/transport_control_codec.c` (encoder + decoder) |
| Fixed-layout binary message (per-tick snapshots) | field list in `src/bolo/internal/wire_messages.h` + a `DEFINE_WIRE_CODEC[_MASKED]` line in `src/bolo/transport_udp_common.c` — see "Fixed-layout wire messages" below |
| Bulk byte transfer (map preview / download / resync / scenario details) | streamed on `CHANNEL_BULK` behind a bulk-transfer stream header — `src/bolo/bulk_transfer.c`. Download and resync blobs are built per recipient; the scenario details reply (`BULK_KIND_SCENARIO_DETAILS`) carries one script file's rules and callbacks blob |
| Per-client handshake / reliability (JOIN_ACCEPT, JOIN_REJECT, NAME_CHANGE_REJECT, PONG) | `src/bolo/transport_udp_server.c` / `src/bolo/transport_udp_client.c` |

These rows say where each payload is *defined*; **how** it is reliably
carried is the channel-mux reliability layer (above): control events on
`CHANNEL_CONTROL`, terrain-change events on `CHANNEL_MAP`, per-tick game
events on `CHANNEL_GAME`, and bulk blobs on `CHANNEL_BULK`.

### Fixed-layout wire messages — the field-list codec

Fixed binary structs on the wire — the per-tick snapshots (`TankSnapshot`,
`ShellSnapshot`, `TkExplosionSnapshot`, `BaseSnapshot`, `PillSnapshot`) — are
**declared once as a field list and their codec is generated**, not
hand-numbered. (Control-event and client-command payloads are a different layer:
they keep their own codecs in `transport_control_codec.c` /
`transport_command_codec.c`, described above. Bulk byte transfers — map preview,
download, resync — are a third layer: they stream on `CHANNEL_BULK` behind the
`bulk_transfer.c` stream header, not a fixed-layout field list.)

The machinery lives in `src/bolo/internal/wire_codec.h`; the field lists in
`src/bolo/internal/wire_messages.h`; the instantiations sit next to each other in
`src/bolo/transport_udp_common.c`. A field list is a one-line-per-field X-macro,
in wire order:

```c
#define SHELL_SNAPSHOT_FIELDS(F) \
    F(U16, worldX) F(U16, worldY) F(U8, angle) F(U8, owner) F(U8, length)
```

and a single line emits `packShellSnapshot` / `unpackShellSnapshot`:

```c
DEFINE_WIRE_CODEC(ShellSnapshot, "shell_snapshot", SHELL_SNAPSHOT_FIELDS)
```

From that one list the machinery generates pack, unpack, and the wire size
(`WIRE_SIZE_OF`), so **encoder and decoder cannot drift**, offsets are computed
rather than counted, and every generated `unpack` carries a per-field `avail`
bounds check (it returns 0 on a short/hostile buffer). Field types are `U8`,
`U16`, `U32`, big-endian via the existing `packU16`/`packU32` helpers.

Two forms:

- **Flat** — `DEFINE_WIRE_CODEC(Name, "label", FIELDS)` for a fixed field list.
- **Presence-bitmask** — `DEFINE_WIRE_CODEC_MASKED(Name, "label", stubField,
  stubFlag, FIELDS)` for omit-zero messages. The list uses `F(type, name)` for
  always-present core fields, `FMASK()` to mark the mask byte's wire slot, and
  `FGROUP(bit, type, name)` for fields sent only when their group bit is set
  (the bit is set iff any field carrying it is non-zero; absent groups decode
  back to 0). `stubField`/`stubFlag` give the 1-byte stub short-circuit.
  `TankSnapshot` is the worked example.

**What stays hand-written (the honest boundary):** variable-length records
(length-prefixed strings, count-driven loops) and stream reassembly. For a
count-driven message you generate the *fixed leaf record* and leave the
surrounding loop and length validation hand-rolled; a sized byte blob (a map)
rides `CHANNEL_BULK` and is reassembled by `bulk_transfer.c`, not a field list.
Don't try to express a length-prefixed string or a repeat count in a field list.

**Verification — the byte-identity net.** `tests/unit/test_wire_corpus.c` is a
differential test: generated pack/unpack must round-trip and be **byte-identical
to committed golden fixtures** (`tests/fixtures/wire/*.hex`) captured from a
scripted loopback session (the `wire_corpus_capture` test regenerates them).
Presence-bitmask messages additionally diff against a reference oracle across
every group combination. An unintended wire change fails this test — it is the
only source of truth for "byte-identical".

#### Recipe — adding or changing a fixed-layout message

1. **Edit the field list** in `wire_messages.h` (add/reorder a field, in wire
   order). For a brand-new message, add the struct + `pack`/`unpack` prototypes
   to the internal header and one `DEFINE_WIRE_CODEC[_MASKED]` line in
   `transport_udp_common.c`; call sites use `packName`/`unpackName` directly.
2. **Re-run `test_wire_corpus`.** For an existing message a layout change is
   expected to fail the golden until you intend it; for a new message, run
   `wire_corpus_capture` once to write the golden fixture, commit it, and add a
   `check_*` (round-trip + boundary, and the oracle path if masked).
3. Never hand-count byte offsets for these messages, and never let pack and
   unpack become two separately-maintained functions again.

### Recipe — adding a new event type

1. **Define the event.** Add a variant to `ControlEventType` in
   `src/bolo/public/control_event.h`, **before the
   `CTRL_EVENT_TYPE_COUNT` sentinel, which must stay last**, and the
   corresponding union member to `ControlEvent.u`. If a typed populate
   helper makes call sites cleaner, add a `serverSimFill<Name>Event`
   function on `server_sim.h`.

2. **Define the wire form.** Add `encode<Name>Body` and
   `decode<Name>Body` in `src/bolo/transport_control_codec.c` and
   register them in `s_bodyEncoders[]` and `s_bodyDecoders[]`, both
   keyed by `ControlEventType`. That is the whole of it for a new
   event: the reliable carrier calls these through
   `transportControlCodecBodyEncoder` / `…BodyDecoder`, which is how
   every control event on `CHANNEL_CONTROL` is carried.

   **Do not add to `s_encoders[]` or `transportControlCodecDecoder`.**
   Those are the pre-channel-mux full-packet path, keyed by wire packet
   type, and they exist only for the events that still have a legacy
   `PACKET_*` type. `transportControlCodecEncoder` — the accessor for
   `s_encoders[]` — has no callers outside the codec file itself.
   Every event added since the carrier landed is body-only:
   `CTRL_VIEW_TARGET`, `CTRL_SPECTATOR_CHAT`, `CTRL_ROUND_RATING_POSTED`,
   `CTRL_STATS_SEED`, `CTRL_VOICE_TALKING`.

   The encoder receives a per-recipient `UdpServerClient *recipient`.
   **Ignore it** — mark the function `/* recipient: safe — ignored. */`
   and `(void)recipient;`, as its neighbours do. The codec is
   recipient-agnostic by design: the bytes are identical for every
   recipient and per-recipient filtering belongs in
   `udpClientDeliverControl` (`CTRL_ALLIANCE_REQUEST`'s target check
   lives there). A payload computed per recipient breaks the caching
   and retransmission the carrier does on those bytes.

3. **Publish from the server-side handler:**
   ```c
   ControlEvent evt;
   memset(&evt, 0, sizeof(evt));
   evt.type = CTRL_<NAME>;
   /* fill evt.u.<name> ... */
   serverSimPublishControl(sim, &evt);
   ```
   That is the entire server-side change. The per-client UDP
   subscribers fan out to remote clients via the codec encoder; the
   in-process subscribers receive it via the bus walk.

4. **Receive on the client.** Wire decoders in
   `src/bolo/transport_udp_client.c` build a `ControlEvent` and call
   `clientSimApplyControl(cs, &evt)`. Add a case for the new variant
   in `clientSimApplyControl` (`src/bolo/client_sim_control.c`) —
   the dispatcher covers SP, bots, and network in one place.

5. **Register it everywhere else.** Steps 1–4 make the event work;
   these make it debuggable, and each is easy to miss:

   - `mpDiagCtrlName` in **both** `src/server/transport_udp_server.c`
     and `src/bolo/transport_udp_client.c` — the name the MP diag log
     prints. Without it the event logs as `<unknown>` on that side.
   - `src/headless/headless_main.c` — the type name and, if the payload
     is worth seeing, a case in the event logger.

   The reliable way to find the rest is to sweep an existing event of a
   similar shape: `rg -n "CTRL_ALLIANCE_RESET" src` names every site one
   bitmap-carrying event touches. Some sites are deliberate non-entries
   — `serverSpectatorDeliverControl`'s allowlist is drop-by-default, so
   adding an event there is a decision about what spectators may see,
   not a registration.

6. **Add a body-codec round-trip test.** One file per event, following
   `tests/unit/test_view_target_codec.c`: resolve the functions through
   `transportControlCodecBodyEncoder` / `…BodyDecoder` the way the live
   path does, encode, decode, and assert the payload survives. Cover the
   boundaries the field can lose — the top bit of a bitmap, an empty
   value, a short body being rejected.

   **A new test file registers in four places**, all required:
   `tests/unit/test_harness.h` (the `run_<name>` declaration), the
   `s_tests[]` table in `tests/unit/test_main.c`, `WINBOLO_UNITTESTS_SOURCES`
   in `CMakeLists.txt`, and `_unit_test_names` in `CMakeLists.txt`. Miss
   the fourth and the build is green while the case never runs.

7. **Send wrapper (if client-originated).** A client-originated event
   means there's a corresponding `CMD_*` command. See "Adding a new
   client→server command" below for the recipe. Briefly: add a
   `clientSimNetSend<Name>` wrapper on `client_net.h` that builds a
   `ClientCommand` and calls `clientSimSubmitCommand(cs, &cmd)`. The
   dispatcher arm in `server_command_dispatch.c` runs the authority
   check, mutates state, and publishes the `ControlEvent`. Frontends
   call the wrapper; never build wire packets or `ClientCommand`
   values in `src/gui/` or any non-bolo directory.

### Client-side dispatcher rule

Wire decoders in `src/bolo/transport_udp_client.c` never mutate
`ClientSim` state directly and never call `frontEnd*` callbacks
directly — they build a `ControlEvent` and route through
`clientSimApplyControl`. Subscribers, not decoders, are where an
event is routed onward to UI, brains, or other sinks. The chat-
rendering migration is the precedent: `CTRL_CHAT` flows through the
codec, and every subscriber that wants to render or log chat sees
the same event whether it arrives from the network, an SP send, or
a bot.

Display side effects that are inherently transport-aware — the
`PACKET_CHAT_BROADCAST` localized-langid render, the
`PACKET_PLAYER_LEFT` lobby-chat line, the `PACKET_ALLIANCE_UPDATE`
request-dialog popup — stay in the wire-client branch around the
codec call, because they depend on state (peer player records,
current dialog, render targets) that only exists in the network-
client runtime. The bus publish reaches subscribers; the wire branch
handles the transport-aware display.

For single-player and bots the same funnel holds via
`serverSimDeliverToClientSim` (in `src/server/sim/server_sim_control.c`) and the
bot manager's deliver callback — both end in
`clientSimApplyControl`. SP, bots, and network converge on one
funnel.

### Load-bearing wire-only exceptions

Two categories of packet stay wire-only by design; the single-
publish recipe does not apply to them:

- **Per-tick world snapshots.** Tank positions, shells, and per-tick
  deltas live in the snapshot module, not the codec. A snapshot now
  carries only world state plus a trailing **channel frame** for the
  channel-mux reliability layer — the reliable game / map / control
  event tails it once carried are all gone. Those events ride
  `CHANNEL_GAME` / `CHANNEL_MAP` / `CHANNEL_CONTROL`, including the
  server lock/unlock notice (the last off-per-tick game event), which
  moved off the snapshot tail onto `CHANNEL_GAME`. See "The channel-mux
  reliability layer" above.
- **Per-client handshake and reliability.** `JOIN_ACCEPT`,
  `JOIN_REJECT`, `NAME_CHANGE_REJECT`, and `PONG` are point-to-point
  transport mechanics. The compressed map is not one of these: join
  download and resync stream on `CHANNEL_BULK` via `bulk_transfer.c`
  (see the reliability layer above), not a bespoke
  `MAP_DOWNLOAD`/`MAP_ACK` pair. The one reliable carrier that still
  has its own packet pair is the client→server command **up-leg**:
  `PACKET_COMMAND_TICK` (client → server) and `PACKET_COMMAND_ACK`
  (server → client, unicast) drain the per-connection `ClientCommand`
  queue; each entry carries a client-assigned `cmdSeq` for dedupe and
  reject correlation (see "Adding a new client→server command" below).
  The server→client control **down-leg** that used to pair with it
  (`PACKET_CONTROL_TICK`/`PACKET_CONTROL_ACK`) is retired — control
  events ride `CHANNEL_CONTROL` on the channel mux instead.

The player roster is **not** a wire-only exception: `CTRL_PLAYER_JOIN` /
`CTRL_PLAYER_LEFT` ride `CHANNEL_CONTROL` like every other control event
(carrying the full alliance bitmap, covering humans and bots, surviving
the game-start boundary), and the join sync replay re-announces every
in-use slot to a new subscriber. There is no separate roster packet.

### Compatibility rules

**The wire is not frozen during development.** There are no deployed
peers to keep compatible — the JOIN handshake gates on an exact
version match, so every connected peer is running the same build by
construction. In-place wire-layout changes are therefore fine now and
are how the protocol evolves at this stage (the snapshot header and
`InputPacket` were both shrunk in place when their dormant fields were
retired). The two "never" rules below are the **post-freeze** policy —
they become binding once the wire is locked for a public release, not
before.

- Adding a `ControlEventType` variant is additive. Subscribers that
  don't know about it fall through their `switch` default and ignore
  it. Safe at any time.
- Adding a wire packet ID is additive. A peer that doesn't understand
  the new ID drops the packet on the floor. Safe at any time.
- **Once frozen: never reorder, renumber, or repurpose an existing
  wire packet ID**, and **never change an existing packet's on-wire
  layout** — add a new packet ID instead. (The retired ids `187`/`188`
  — the old `PACKET_CONTROL_TICK`/`PACKET_CONTROL_ACK` carrier — are
  freed but kept reserved by preference so old captures and logs stay
  unambiguous; reuse them only deliberately.)

## Adding a new client→server command

The mirror image of "Adding a new server event." Up-leg commands
flow through one funnel — `clientSimSubmitCommand(cs, &cmd)` —
regardless of transport. UDP enqueues into a per-connection reliable
carrier; local hands the command directly to the dispatcher under
the mutex. The same dispatcher arm runs in both cases, so SP-host
and UDP cannot disagree on apply.

### The flow

```
clientSimNetSend<Name>(cs, ...)             ← T1 wrapper, called by GUI
  └─ build ClientCommand cmd { .type = CMD_<NAME>, .u.<name> = {...} }
     clientSimSubmitCommand(cs, &cmd)
        ├─ UDP   → transportUdpClientSubmitCommand → ClientCommandQueue
        │         ↓ eager-send / 80ms retransmit
        │         PACKET_COMMAND_TICK ────────────→ server
        │         PACKET_COMMAND_ACK  ←────────────
        │
        └─ local → threadsWaitForMutex
                   serverSimApplyCommand(sim, mySlot, &cmd)
                   threadsReleaseMutex
```

Both arms end at `serverSimApplyCommand` in
`src/server/server_command_dispatch.c`. The dispatcher is the single
authority site — no per-transport mirror to drift.

### Dispatcher contract

`CmdResult serverSimApplyCommand(ServerSim *sim, int senderSlot, const ClientCommand *cmd)`:

- **Mutex.** Caller holds `threadsMutex` (asserted in debug). UDP
  server callers are already on the server-tick thread which holds
  it. Local callers (SP-host, bots on a passive transport) wrap the
  call in `threadsWaitForMutex()` / `threadsReleaseMutex()` —
  `clientSimSubmitCommand` already does this for them.
- **Sender attribution.** `senderSlot` is authoritative — for UDP
  it comes from `serverFindClient(fromAddr)`; for local it comes
  from `clientSimGetMyPlayerNum(cs)`. Wire payloads never carry an
  attributing playerNum.
- **State guards live in the arm**, not the dispatcher prelude. Most
  lobby commands assert `serverSimIsLobbyEnabled(sim) && serverSimGetState(sim) == serverStateLobby`;
  game-time commands assert their own state; `CMD_WBN_REAUTH` asserts
  `serverSimGetRanked(sim)`. `CMD_BALANCE_*` is host-only
  (`senderSlot == 0`) + lobby-state but does not require ranked — a
  non-ranked host can still ask WBN to skill-balance the teams.
- **Tail effects stay with the arm**, not the dispatcher. Each arm
  runs its own `serverSimPublish*`, `logAddEvent`, etc. in the same
  order the original handler did.
- **Reject publishing.** On any non-`CMD_OK` return the dispatcher
  wrapper publishes
  `CTRL_COMMAND_REJECTED { origCmdSeq, origCmdType, reasonCode, origSlot }`
  via `serverSimPublishControl` — the unified reject channel for
  client UI feedback.

### Wire carrier

- `PACKET_COMMAND_TICK` (client → server): `[header 8][count 1][for each: entryLen u16, codecPacket entryLen bytes]`.
  Each `codecPacket` is a `commandCodecEncode` output with the
  inner-entry header carrying the per-command packet number (e.g.
  `PACKET_LOBBY_TEAM_SET`) as the type tag and a 4-byte `cmdSeq`
  in the slot immediately after the header.
- `PACKET_COMMAND_ACK` (server → client, unicast): `[header 8][highestProcessedCmdSeq u32]`.
  Emitted at the end of every received `PACKET_COMMAND_TICK` —
  carries the highest contiguously processed `cmdSeq`. The client
  uses it to advance the queue head past acked entries.
- **Send timing: eager-then-coalesce.** Each `clientSimSubmitCommand`
  triggers an immediate `PACKET_COMMAND_TICK` if the queue was
  previously empty. Subsequent submits within the same tick window
  coalesce into the next outgoing frame.
- **Retransmit.** The transport tick re-drains the unacked window
  if the head entry's last send was more than ~80ms ago.
- **Dedupe.** Server's `inboundCmdSeq` is the highest contiguously
  applied `cmdSeq` per client. Entries at or below it are skipped;
  out-of-order arrivals wait for the missing-gap retransmit.

### Reject UX

`CTRL_COMMAND_REJECTED` is broadcast through the same control bus
the down-leg uses, but with two per-recipient filters:

- The UDP per-client subscriber `udpClientDeliverControl` skips
  delivery when `evt->u.commandRejected.origSlot != client->playerNum`,
  matching the `CTRL_ALLIANCE_REQUEST` precedent. The codec stays
  recipient-agnostic; the filter lives at the bus layer.
- `clientSimApplyControl`'s `CTRL_COMMAND_REJECTED` arm filters by
  `origSlot == clientSimGetMyPlayerNum(cs)` before mutating the
  reject toast state. In-process subscribers receive every published
  event, but only the originator's slot reacts.

The reject is informational. The UI correlates by `origCmdSeq` and
dismisses when the rejected command's effect was undone by a later
successful submit — "most recent reject wins" is wrong because
rejects can land after newer state mutations.

Each `CMD_REJECT_*` code renders its own toast line, so the code a
dispatcher arm picks is what the sender reads. `CMD_REJECT_SCENARIO`
says the setting is fixed by the map's scenario, and is what the
lobby-setting path answers for the three a scenario holds: any game-type
change, ranked on, and the no-bots AI policy. `CMD_REJECT_COOLDOWN` says
to try again in a moment, and is what a per-sim rate limit answers —
the lobby scenario reload allows one a second.

A command that has already told the sender why returns `CMD_OK` instead
of a reject, so the sender is not told twice. The reload arm in
`src/server/server_command_dispatch.c` is the worked example: it sends a
`CTRL_SERVER_TEXT` addressed to the asking slot either way — saying what
a reload changed when it worked, and carrying the error with the file and
line when it did not — and then returns `CMD_OK`, because a reject code
on top would add a second toast saying only that something was wrong. Its
rate limit is the one place it does return a reject, and that one has
nothing else to say.

### Recipe — adding a new command

1. **Define the variant.** Add `CMD_<NAME>` to `ClientCommandType` in
   `src/bolo/public/client_command.h` plus a payload struct in the
   union (`u.<name>`). Variable-length payloads use fixed-size
   in-union buffers sized to `PACKET_MAX_*`.
2. **Add the codec pair.** In `src/bolo/transport_command_codec.c`,
   add `commandEncode<Name>` and `commandDecode<Name>` next to the
   existing variants, then register in the `commandCodecEncode` /
   `commandCodecDecode` dispatch tables. The encoder stamps a
   `PACKET_*` number as the inner-entry tag, and every variant carries
   its own: no number is shared between two commands, so a new command
   normally means a new number rather than reusing one.
3. **Register the packet number.** The new `PACKET_*` goes in
   `src/bolo/internal/netpacks.h` beside the others in its block, then
   into `PACKET_NAME_TABLE` in `src/bolo/transport_udp_common.c` so the
   diagnostic logs name it instead of printing `UNKNOWN`, then into the
   independent copy of that table at the top of
   `tests/unit/test_packet_type_names.c`. The test holds the two copies
   against each other, so a number added to the production table and
   left out of the test's copy fails `packet_type_names`.
4. **Add the dispatcher arm.** New `case CMD_<NAME>:` in
   `applyCommandInner` (`src/server/server_command_dispatch.c`).
   Owns state guards, authority checks (`lobbyClientMayEdit`, slot
   bounds), mutation, downstream publishes (`serverSimPublish*`),
   logging. Returns `CMD_OK` or a `CMD_REJECT_*` code.
5. **Add the send wrapper.** New `clientSimNetSend<Name>` in
   `src/bolo/client_net.c` — five lines: NULL/transport guard, build
   `ClientCommand`, fill `u.<name>`, call `clientSimSubmitCommand`.
   Public declaration in `client_net.h`. The wrapper has no
   transport branch; `clientSimSubmitCommand` owns the fanout.

That is the whole change. Frontends call `clientSimNetSend<Name>`;
nothing else.

### Bespoke channels (when to skip the bus)

A handful of helpers stay on their direct UDP path because they
don't fit fire-and-apply:

- `clientSimNetSendInput` — every-frame, snapshot-ACK paired.
- `clientSimNetSendLobbyMapListRequest` / `…SearchRequest` /
  `…MapPreviewRequest` — request / paginated-or-streamed-response. The
  preview variant streams a map's raw bytes back
  (`PACKET_LOBBY_MAP_PREVIEW_BEGIN/_CHUNK/_ERR`) for the chooser to
  rasterise client-side; it applies nothing to sim state, so it can't
  ride the command bus.
- `PACKET_LOBBY_SCENARIO_DETAILS_REQ` — sent by the transport tick
  from the ClientSim's WANTED slots, not by a `clientSimNetSend*`
  wrapper. It is a read-only request for one script file's details
  blob; the reply streams on `CHANNEL_BULK`
  (`BULK_KIND_SCENARIO_DETAILS`) and applies nothing to sim state,
  the same shape as the map preview.
- `clientSimNetSendLobbyMapUploadBytes` / `…MapUseLocal` — large
  payload or chunked upload that doesn't fit fire-and-apply.
- `clientSimNetSendWbnReauth` — synchronous WBN tracker round-trip
  to mint a fresh playerKey before sending; the helper's work
  happens between the join-state guard and the codec encode.

These keep their per-command `transportUdpClientSend*` helper in
`transport_udp_client.c` and a matching direct-receive
`case PACKET_*:` arm in `serverProcessPacket`. They are not under
the asymmetric-runtime invariant the rest of the up-leg enforces,
and adding more of them re-opens the bug class — only add to this
list when fire-and-apply genuinely doesn't fit.

## Localization

User-facing strings ship through a runtime string-ID table backed by
per-language override files. Frontends never embed English literals
directly; they look up an integer ID and the lang module returns the
loaded translation, falling back to English if the loaded file
doesn't override that ID.

### What gets localized

In scope: every binary with a UI — desktop (`src/gui/sdl3/`), mobile
(`src/android/`, `src/ios/`), wasm (`src/wasm/`), log viewer
(`src/logviewer/`), map editor (`src/mapeditor/`), and the bolo sim's
user-visible messages from `bases.c`, `lgm.c`, `pillbox.c`,
`players.c`, `screen.c`, `tank.c`, `tankexp.c`.

Out of scope: dedicated server (`src/server/`) operator CLI, brain
test (`src/braintest/`), and gym (`src/gym/`) — all developer/operator
tools, English-only by design.

### Three-file contract

| File | Role |
|---|---|
| `src/gui/lang.h` | `#define STR_FOO <int>` — canonical ID header. The integer is wire-stable. |
| `src/gui/sdl3/lang.c` | `{<int>, "English text"}` — built-in English baseline. |
| `data/lang/en.txt` | **GENERATED** — never hand-edited. Regenerated from lang.h + lang.c. |
| `src/gui/sdl3/lang_names.inc` | **GENERATED** — STR_* name → langid lookup used by the file parser. |
| `data/lang/<code>.txt` | Hand-edited per-language overrides. Missing entries fall back to English at runtime. |

Run `python3 tools/dump_lang_en.py` to regenerate the two derived
files after editing `lang.h` or `lang.c`. The `--check` mode exits
non-zero if regeneration would change the output (suitable for CI).

### APIs

```c
char *langGetText(langid id);
const char *langGetTextFmt(langid id, const MessageArgs *args);
```

`langGetText` returns a stable pointer into the lang table for the
lifetime of the loaded language file — safe to store in long-lived
locations.

`langGetTextFmt` expands named placeholders from `args` (`MessageArgs`
in `src/bolo/public/lang_message.h`). The result points into a
thread-local ring of four buffers — safe for immediate use, but
`SDL_snprintf` it into a local buffer if the pointer must outlive
the next three `langGetTextFmt` calls.

### Named placeholders, not printf

Translations use named placeholders so translators can reorder them
to match target-language word order. POSIX `%1$s` numbered args
don't work on Windows.

| Placeholder | Source field |
|---|---|
| `{player}` | `args.playerName` |
| `{other}` | `args.otherName` |
| `{number}`, `{number2..4}` | `args.number`, `args.number2..4` (rendered as `%d`) |
| `{string1}`, `{string2}` | `args.string1` / `string2` (64-byte buffers for pre-formatted floats, duration labels, etc.) |

Lang-file entry example: `STR_DLGLOBBY_VOTES={number}/{number2} votes to skip`

### Common patterns

**PLAIN** — static text:

```c
ImGui::Button(langGetText(STR_DLGLOBBY_ADD_TEAM));
```

**FMT** — has args:

```c
MessageArgs args = {};
args.number = teamId;
ImGui::Text("%s", langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &args));
```

**WIDGET_ID** — ImGui label with `##suffix`:

```c
char buf[64];
SDL_snprintf(buf, sizeof(buf), "%s##j%d",
             langGetText(STR_DLGLOBBY_JOIN_TEAM), teamId);
if (ImGui::SmallButton(buf)) { ... }
```

**TOOLTIP_FMT** — `ImGui::SetTooltip` is printf-style, so wrap the
localized text in `"%s"` to defuse any literal `%` in the
translation:

```c
ImGui::SetTooltip("%s", langGetTextFmt(STR_FOO, &args));
```

**MULTILINE** — keep the whole entry on one line with embedded `\n`
escapes. `dump_lang_en.py`'s entry regex matches a single `"..."`
literal per `{<id>, ...}` block, so adjacent C string literals (the
compiler-collapses-them pattern) are **not** picked up — the entry
ends up missing from `en.txt`:

```c
/* lang.c */
{1306, "Remove every bot from the lobby before flagging\nthe game as Ranked. Ranked matches are humans-only."},
```

**PLURAL_PAIR** — plurals are two separate IDs branched at the call
site, not smuggled through `{string1}`. Different languages have
different plural rules (Polish 3 forms, Russian 3 forms, Japanese
none); the singular/plural split is the simplest mechanism that
works everywhere:

```c
if (count == 1) {
    ImGui::Text("%s", langGetText(STR_DLGLOBBY_TEAM_MEMBERS_1));
} else {
    MessageArgs args = {};
    args.number = count;
    ImGui::Text("%s", langGetTextFmt(STR_DLGLOBBY_TEAM_MEMBERS_N, &args));
}
```

### Adding a localized string

1. **Allocate an ID.** Append after the highest existing `STR_*` in
   `lang.h`; gaps in the numbering are historical and fine.
2. **Add to `src/gui/lang.h`** under a section comment that names
   the feature area. `dump_lang_en.py` uses these comments to group
   entries in `en.txt`.
3. **Add to `src/gui/sdl3/lang.c`** as `{<int>, "<English>"}`,
   inserted in sorted order by ID.
4. **Replace the call site** with `langGetText(STR_FOO)` or
   `langGetTextFmt(STR_FOO, &args)` per the patterns above.
5. **Regenerate** with `python3 tools/dump_lang_en.py`.

**Reuse vs. new ID.** Short generic UI words (Yes, Cancel, OK) reuse
the existing generic IDs (`STR_YES`, `STR_CANCEL`, `STR_OK`). Longer
semantically-distinct strings get their own ID even when the English
happens to match an existing entry — a translator may want to phrase
a window title differently from a button that uses the same English
words.

**Server messages stay English.** The dedicated server's stdout and
operator CLI use plain `printf` / `fprintf(stderr, ...)`,
intentionally. Don't reach into the lang module from `src/server/`.

### Translation files

Per-language overrides live at `data/lang/<code>.txt`. The codes are
SDL's `SDL_GetPreferredLocales()` codes (lowercase ISO 639 + optional
region: `en`, `de`, `pt-BR`, `zh-CN`). Currently shipped: cs, de, en,
es, fr, it, ja, ko, nl, pl, pt-BR, ru, sv, tr, uk, zh-CN, zh-TW.

File format:

```
# Header (shown in the language picker)
name=Deutsch
author=Translator Name
notes=Free-form notes.

# Free-form section comment (ignored by parser, only for humans)
STR_FOO=Translated text
STR_BAR=Another translation
```

Parser behavior (`langLoadFile` in `src/gui/sdl3/lang.c`):

- `name=`, `author=`, `notes=` populate `LangFileMeta` for the
  picker UI.
- `STR_*=` lines override the English baseline for that ID. The
  symbolic name resolves via `lang_names.inc`.
- Blank lines and `#`-prefixed comments are ignored.
- Unknown IDs are silently skipped. Missing IDs fall back to
  English at runtime — partial translation is fine.

### Updating translations after adding strings

After adding new IDs and running `dump_lang_en.py`:

1. The new entries appear in `data/lang/en.txt` under the section
   comment you added to `lang.h`.
2. For each `data/lang/<code>.txt`, append the new entries with
   their translations. Section comments are optional but help the
   next translator find their place.
3. Preserve named placeholders verbatim. They may be reordered to
   fit target-language word order, but not renamed or removed.
4. Preserve `\n` escape sequences.
5. Plural pairs (`STR_FOO_1` / `STR_FOO_N`) translate independently
   — different languages need different forms.
6. Partial coverage is fine. Untranslated IDs fall back to English.

**Validation tools.** `tools/validate_lang.py` checks placeholder
consistency between `en.txt` and each translation (catches `{numer}`
typos). `tools/test_lang_roundtrip.py` confirms `en.txt` round-trips
through `dump_lang_en.py`.

## macOS native menu bar

The SDL3 desktop client renders **two** menu bars on macOS:

- The cross-platform ImGui menu bar inside the game window — built
  once per frame by `renderMenuBar()` in `src/gui/sdl3/sdl3imgui.cpp`.
  This is what every other platform (Windows, Linux, wasm) sees.
- The macOS-native `NSMenu` attached to `NSApp.mainMenu` — built once
  at startup by `mac_menubar_install()` in
  `src/gui/sdl3/platform/mac_menubar.mm` and refreshed per-frame by
  `mac_menubar_refresh()`. macOS users expect a real menu bar at the
  top of the screen; the ImGui in-window bar remains available too.

The two bars are independent UI surfaces over the same underlying
sim. Both call into the same T1 client API
(`clientSimNetSend*`, `clientSimSet*`, `windowMenu*_toggle`,
sim-side actions). Neither bar reaches into sim internals.

### Contract — both bars in lockstep

**A user-facing menu item must exist in both bars, or a deliberate
exception must be documented in the code.** A macOS user with the
native menu bar focused and a Linux user with the ImGui bar must be
able to reach the same actions. The two surfaces today have a small
set of intentional differences:

- App > Preferences (⌘,) replaces the in-window WinBolo > Settings
  entry on macOS — the platform convention.
- Send Message uses ⇧⌘M on the native bar (⌘M is reserved for
  Window > Minimize on macOS) and ⌘M in the in-window bar.
- The dynamic per-player roster row in the in-window Players menu is
  not replicated natively. The Players Panel window owns that
  surface; the native menu has only the static action items.
- Window > Enter Full Screen (⌃⌘F) has no in-window twin. It sits with
  Minimize / Zoom / Bring All To Front, which are AppKit window
  commands, by macOS convention. It is not a divergence in behaviour:
  it calls `sdl3ImguiToggleFullScreen`, the same command Alt+Enter
  makes, which in a game is the full screen map that File > Overview
  in Window drives in both bars, and outside one is the app full
  screen flag.

Add new exceptions sparingly and only with a justification — every
diverged item is a future asymmetric-UI bug.

### State flow

The native menu's per-frame state is a **snapshot push** from the
ImGui frontend, not a pull from the sim:

```
sdl3imgui.cpp (per frame, inside renderMenuBar context)
  populateMacMenuState(&mms, cs)           // gather flags from cs + UI globals
    ↓
  mac_menubar_refresh(&mms)                // apply to cached NSMenuItem pointers
```

`MacMenuState` (in `src/gui/sdl3/platform/mac_menubar.h`) is a POD
mirror of every checkmark, enable flag, dynamic title, and per-slot
roster field the native bar needs. Producer (`populateMacMenuState`
in `sdl3imgui.cpp`) and consumer (`mac_menubar_refresh` in
`mac_menubar.mm`) communicate only through this struct — neither
pulls anything live from `ClientSim` outside that producer call.

Pre-computing the gating flags GUI-side keeps the native consumer
free of sim state and ensures the two bars enable/disable items
under identical predicates: any `if (...)` that decides whether an
ImGui `MenuItem` is enabled has a corresponding field on
`MacMenuState` that drives `[NSMenuItem setEnabled:]`.

### Recipe — adding a new menu item

1. **Localize the label and tooltip.** Add `STR_*` ids per the
   Localization section above; the native bar reads the same
   `langGetText(STR_*)` via the `LANG_STR(id)` macro in
   `mac_menubar.mm`.
2. **Add it to the ImGui bar** in `renderMenuBar()`
   (`src/gui/sdl3/sdl3imgui.cpp`). Call a T1 send wrapper for the
   action — never build wire packets or call sim internals.
3. **Add it to the native bar** in `mac_menubar_install()`
   (`src/gui/sdl3/platform/mac_menubar.mm`):
   - Cache the `NSMenuItem *` in a `static` so the refresh can
     reach it.
   - Add a selector declaration and matching `- (void)onFoo:`
     method on `WBMenuBridge`. The method forwards to the same T1
     entry the ImGui handler called.
   - If the parent menu uses `setAutoenablesItems:NO` (the WinBolo
     and Players menus do), the refresh must explicitly drive
     `setEnabled:` — otherwise the item stays whatever it was at
     construction.
4. **Mirror enable / checkmark gating.** Extend `MacMenuState` with
   the predicate the ImGui code already computes. Set it in
   `populateMacMenuState` and consume it in `mac_menubar_refresh`.
   Don't recompute predicates inside the refresh — that's how the
   two bars drift.
5. **For items that need a `ClientSim`,** route through `g_clientSim`
   (the cached pointer set by `mac_menubar_set_clientsim`). NULL is
   acceptable during startup; handlers must early-return on NULL.

### Other platform native shells

`src/logviewer/platform/mac_menubar.{h,mm}` and
`src/mapeditor/platform/mac_menubar.{h,mm}` are the same pattern for
the LogViewer and MapEditor binaries — each has its own bridge,
its own state struct, and its own refresh. They share the
`LANG_STR()` convention with the SDL3 client. The mobile clients
(`src/ios/`, `src/android/`) and the wasm build don't have a system
menu bar; they ship only the ImGui in-window bar.

## Standalone ImGui dialogs — controller navigation and quit

Every blocking dialog under `src/gui/sdl3/dialogs/` (welcome, lobby,
settings, keysetup, onboarding, …) creates its **own** ImGui context and
runs its **own** SDL event loop. Two things therefore have to be wired
up per dialog rather than inherited: the controller, because the context
is per-dialog, and the quit, because the event loop is. Missing any of
them silently breaks that dialog with no compile error.

Controller navigation needs two separate paths enabled, and missing
either one breaks the pad on that dialog:

- **Path B — native SDL gamepad** (non-Steam launches). The ImGui SDL3
  backend turns raw gamepad events into nav, but only when
  `ImGuiConfigFlags_NavEnableGamepad` is set on the context.
- **Path A — Steam Input** (Steam / Steam Deck launches). Steam
  intercepts the pad so the backend never sees it; the
  `imguiSteamNav*` bridge (`src/gui/sdl3/imgui_steam_nav.h`) injects
  keyboard-nav events instead, and only works if its per-frame helpers
  are called.

### Contract — every standalone dialog must

1. Set **both** nav flags on its context, right after creating it:
   ```c
   io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
   io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
   ```
2. `#include "../imgui_steam_nav.h"`.
3. Call the bridge **once per frame, immediately after
   `ImGui::NewFrame()`**:
   ```c
   imguiSteamNavActivateMenuSet();
   imguiSteamNavFeedCurrentContext();
   ```
   `imguiSteamNavFeedCurrentContext()` is a no-op when Steam Input has
   no active controller, so it is safe on every platform.

A dialog that re-creates its context mid-loop (e.g. `imgui_settings.cpp`
after launching the key-setup dialog) must re-apply the flags on the new
context, but the per-frame helpers already cover it since the loop calls
them every frame.

`src/gui/sdl3/dialogs/imgui_keysetup.cpp` is the canonical reference —
copy its context setup and per-frame preamble when adding a dialog.

### Contract — every standalone dialog must also end the application

A quit reaches whichever loop is running and stops there. Read as
"close me" — which is what every dialog did before — Cmd+Q in the game
browser closed the browser and left the player on the menu. So the
poll loop has to recognise one and end the loop:

```c
if (dialogHandleQuitEvent(window, &ev)) { /* dialog's own close path */ }
```

`dialogHandleQuitEvent` (`imgui_dialog_utils.h`) wraps
`dialogQuitClassify` in `dialogs/dialog_quit.cpp`, which is where the
decision actually lives. Three points it is easy to get wrong:

- **A quit is not a cancel.** Cmd+Q, Alt+F4 and the window's close box
  end the application. The gamepad's B button reaches the loop as a
  close request `dialogHandleGamepadCancelEvent` forged, carrying
  `DIALOG_CLOSE_IS_GAMEPAD_CANCEL` in `window.data1`, and only closes
  the dialog. Call the gamepad helper **before** the quit check so the
  marker is on the event by the time it is classified.
- **The dialogs do not call `windowSetQuitting` themselves.** They are
  linked into the standalone Log Viewer and Map Editor too, which have
  no application loop, and the direct call breaks the `LogViewer` link.
  The host registers what a quit means — `winbolo.c` calls
  `dialogSetQuitHandler(windowSetQuitting)` at startup — and with
  nothing registered a quit just closes the dialog.
- **Closing a dialog is not the application ending.** A dialog closes
  on a quit the same way it closes on Cancel, so the front end asks
  `windowIsQuitting()` rather than reading the dialog's result.
  `gameFrontDialogs()` asks once per turn of its state machine and
  stops unwinding. `windowIsQuitting()` reads `quitRequested`, not
  `winboloQuit`: the game loop sets `winboloQuit` TRUE on the way in as
  its default answer, so it cannot say whether the player asked for
  anything.

A screen that owns the window for a while rather than being one of
these dialogs — the embedded map editor, log viewer and spectator —
runs its own loop and cannot see the host's flag. Each separates
leaving from quitting and reports which happened
(`mapEditorAppQuitRequested()`, `logViewerAppQuitRequested()`), and the
`gameFrontDialogs()` case that ran it hands the answer to
`windowSetQuitting()`. A run that can raise such a flag must clear it
on entry, or the next caller to ask inherits someone else's quit.

## WinBolo.net subsystem

The WinBolo.net (WBN) integration is split across three sibling
static libraries under `src/winbolonet/`. They share a directory on
disk but each library covers a different surface and a different
link set, so binaries pick the subset they need.

### What each library is for

**`winbolonet_core`** owns the shared HTTP transport (libcurl
wrapper in `http.c`), the async event-delivery queue, WBN key
storage, the in-memory server bearer-token state (`wbn_bearer.c`
holds the `server_token` returned by `POST server/register` and
the `Authorization: Bearer` setter/clearer/accessor that `http.c`
reads), and the preferences-file path stored via
`winbolonetCoreSetPreferencesPath` (called once at startup by each
binary's `main()` before any WBN call). It is the hard dependency
of the other two libraries and is linked into every binary that
talks to WBN.

**`winbolonet_server`** is the tracker-reporting surface a server
runtime calls: `server/register` at boot, per-tick `server/update`,
lobby and map status updates, teams and balance, client verify,
and leave on shutdown. It depends on `winbolonet_core`. Linked into
binaries that run a server.

**`winbolonet_client`** is the surface UI frontends call to sign in
(`winbolonetAuthLogin`, `winbolonetAuthSteam`,
`winbolonetAuthValidate`), to exchange the API token for a
short-lived server-scoped `player_key`
(`winbolonetClientJoinSession`, called from the transport on
JOIN / rejoin / re-auth — see "Cross-tier dependency" below),
and to fetch user-facing data (`wbn_comments`). It depends on
`winbolonet_core`. Linked into binaries with a UI, plus
WinBoloDS and WinBoloHeadless — those two pull it solely to
satisfy `winbolonetClientJoinSession`, which the per-target
`transport_udp_client.c` references unconditionally (see
"Cross-tier dependency" below). The stub-set binaries
(mobile / wasm / braintest / mapeditor / unit-tests / gym)
resolve the same symbol with a no-op stub.

### Per-binary link sets

| Binary | core | server | client | Notes |
|---|---|---|---|---|
| `WinBolo` (SDL3) | yes | yes | yes | Hosts SP server in-process |
| `WinBoloDS` | yes | yes | yes¹ | |
| `WinBoloHeadless` | yes | yes | yes¹ | SP via `cmd_stdin` |
| `LogViewer` | yes | — | yes | Replay UI uses `http.c` + comments |

¹ DS and Headless link `winbolonet_client` only to satisfy
`winbolonetClientJoinSession`, which `transport_udp_client.c`
references unconditionally. That TU is per-target (see
"Per-file T2 grants") so the symbol must resolve in every binary
that compiles it. Neither binary calls the auth functions —
there is no UI to invoke them — so `winbolonetAuthLogin`,
`winbolonetAuthSteam`, and `winbolonetAuthValidate` are dead
code in these builds. See "Cross-tier dependency" below.
| `WinBoloIOS`, `WinBoloAndroid`, `BrainTest`, `MapEditor`, `WinBoloUnitTests`, wasm, `Gym` | — | — | — | Stubbed. Each platform-class group has its own stub file: `android/winbolonet_stub.c`, `gui/ios/ios_stubs.c`, `wasm/winbolonet_wasm.c`, and `gym/winbolonet_stub.c`. Gym's lives in its own file because gym links `server_static`, which references the lifecycle-driven WBN surface (`winbolonetEndSession`, `winbolonetBeginSession`, `winbolonetSendLobbyStatus`) the other stub sets don't carry. `bolo/log.c`'s `winboloNetGetServerKey` call is satisfied by every stub, returning an empty string. |

**Gym caveat.** Gym is an offline ML training harness with no
business phoning home. It inherits the `server_static` runtime,
which contains calls into `winbolonet_server` from `server_sim.c`
and `server_lifecycle.c`, so the WBN surface has to resolve. Every
such call is gated by `winbolonetIsRunning()`, which gym never
sets TRUE, so they were always no-ops at runtime — the same
situation as a dedicated server started with `-nowinbolonet`. Gym
historically linked the real `winbolonet_core` + `winbolonet_server`
libraries to satisfy the symbol surface; it now resolves the
surface through `src/gym/winbolonet_stub.c` instead, dropping
the libcurl, cjson, and tweetnacl dependencies it never exercised.

### The worker and its jobs

Nothing on the server tick posts to WinBolo.net. `winbolonetthread.c`
runs one worker for the life of the process (created after the first
`server/register`, drained rather than destroyed at a round boundary,
so its pooled connection carries across rounds), and every post is a
job on its queue: an endpoint, a body, whether the bearer is attached,
and a **kind** — `WBN_JOB_NONE` for a post nobody reads the reply to,
`WBN_JOB_REGISTER`, `WBN_JOB_VERIFY`, `WBN_JOB_UPLOAD`. A kinded job is
queued with `winbolonetThreadAddJob`, which returns a **job id** unique
for the process, and its reply is held until the tick collects it:
`serverInstanceTick` calls `winbolonetThreadDrainResults`, which hands
each result to `serverLifecycleWbnResult` on the tick thread. That
handler owns the register; it passes a verify to the server transport,
which matches it to the slot and connection the re-auth captured, by
job id. A `WBN_JOB_NONE` job answers TRUE or FALSE at the queue
instead: FALSE means the worker is not running, or that its waiting
queue already holds `WBN_WAITING_NONE_MAX` fire-and-forget posts, and
the caller sends the post itself or drops it.

The cap counts and refuses `WBN_JOB_NONE` posts alone. A register, a
verify and an upload carry a kind and are never refused, and neither
are the two posts queued through `winbolonetThreadAddSessionRequest`:
the round transition's `server/quit` and the forced `server/update`
flush the tick sends just before it. Both of those have a caller that
posts on the tick thread when the queue says no, so counting them
against the cap would put an HTTP post in the game loop rather than
keep one out of it.

A body that names the session leaves `server_key` out and is queued
through `winbolonetThreadAddServerKeyedRequest`; the worker stamps the
key that is current when the job **fires**. A round transition swaps
the key on the tick between a post being queued and it going out, and
a post that named the old session was refused.

The round transition is three jobs in one order —
`server/quit`, the round-log upload, `server/register` — queued by
`serverLifecycleQueueRotation` at each of the three sites that end a
round. Everything that needs the new key (the lobby update held dirty,
the rekey broadcast to every participating client, the lock re-send)
waits for the register's completion handler, not for the tick that
queued it. Only one register may be outstanding: a round that ends
while one is out marks the rotation deferred, and the handler runs it
against the key it has just installed.

One verify per slot: the re-auth handler refuses a second
`client/verify` for a slot while its first is outstanding. The packet
that carries a re-auth bypasses the command-sequence dedup, and the
worker posts one job at a time, so without that rule one client could
put a verify per datagram ahead of the register every other slot is
waiting on.

### Recipe — adding a new WBN endpoint

Two shapes, depending on which side of the tracker the new endpoint
lives on.

**Client-side fetcher** — a UI frontend wants user-facing data from
the tracker. Modelled on `wbn_comments`:

1. Pick `winbolonet_client` as home. Add `wbn_<name>.{cpp,h}`
   alongside `wbn_comments.{cpp,h}`.
2. Use the established async surface: an `std::atomic<bool> done`,
   an `std::mutex` guarding the result struct, and four entry
   points — `wbn_<name>_start` (kicks off a worker thread),
   `wbn_<name>_done` (poll), `wbn_<name>_result` (collect once
   done), `wbn_<name>_free` (release). The worker issues the
   request via `wbn_api_get` from `winbolonet_core`.
3. Call from a UI frontend (e.g. an ImGui dialog under
   `src/gui/sdl3/dialogs/` or `src/logviewer/imgui/`). The frontend
   includes `winbolonet_client.h`.

**Server-tracker endpoint** — the sim runtime wants to report
something authoritative to the tracker. Modelled on
`winbolonetServerUpdate`:

1. Add the function to `winbolonet_server.c`, following the
   existing `winbolonetServer*` naming and shape: build a `cJSON`
   body, call `wbn_api_call_server` from `winbolonet_core`
   (which attaches the `Authorization: Bearer <server_token>`
   header), parse the response. Use the `_server` variant
   whenever a bearer has been minted — everything except
   bootstrap calls that run before or instead of one.
   `server/register` (and the round-boundary re-register inside
   `winbolonetBeginSession`) use plain `wbn_api_call` because
   that call is what mints the bearer in the first place;
   `client/verify` uses plain `wbn_api_call` because the WBN API
   defines it as unauthenticated. The endpoint prefix is not the
   discriminator (e.g. `client/leave` uses the `_server` variant —
   the server is the caller).
2. Declare it on `winbolonet_server.h`.
3. Call it from the appropriate server-side site (`server_sim.c`,
   `server_lifecycle.c`, `transport_udp_server.c`, `servermain.c`),
   gated by `winbolonetIsRunning()`.

### Auth invariants

Three rules that, if violated, regress to bugs this subsystem was
restructured to fix. They belong in this doc for the same reason
"do not include T2 from outside `src/bolo/`" does — invariants
that bound the design.

**Two-token rule.** The WBN API token never appears on the wire
between a client and a game server. The API token is the
long-lived credential, stored in prefs by `winbolonetAuthLogin` /
`winbolonetAuthSteam`. Frontends pass it only to
`winbolonet_client` (the auth functions). The transport calls
`winbolonetClientJoinSession(apiToken, serverKey, ...)` to mint a
short-lived `player_key` scoped to one server's session; only the
`player_key` crosses the wire, in the JOIN packet's `wbnJoinKey`
field or in `PACKET_WBN_REAUTH`. Servers validate the
`player_key` via `winboloNetVerifyClientKey` against WBN; servers
never call any function that takes an API token. A hostile or
compromised game server therefore cannot replay a credential
elsewhere or impersonate the user against WBN — the wire only
ever carries a capability scoped to that server.

**Server bearer-token rule.** The `server_token` returned by
`POST server/register` lives only in process memory inside
`winbolonet_core` (`wbn_bearer.c`). Never logged, never persisted
(not even to the INI prefs), never reaches a UI surface,
discarded on every session rollover —
`winbolonetEndSession` clears it immediately after the
`server/quit` POST, and `winbolonetDestroy(TRUE)` clears it on
shutdown. The next `register` call mints a fresh pair.
Authenticated calls go through `wbn_api_call_server` /
`wbn_api_post_server` (or `httpSendLogFile` for the multipart
log upload), which are the only sanctioned paths that attach the
header — the bootstrap calls that run before a bearer exists
(`server/register` itself, and `client/verify`, which WBN
defines as unauthenticated) use plain `wbn_api_call`. The narrow
accessor `winboloNetGetServerToken` is declared in
`wbn_bearer.h`, not on the general `winbolonet_core.h` surface —
its only legitimate callers are `http.c`'s server-scoped call
variants and the log upload.

**Lobby re-keying contract.** WBN issues a fresh `server_key`
for every pre-game / game cycle. When a server's
round-end `winbolonetEndSession` / `winbolonetBeginSession`
pair completes successfully, the lifecycle code broadcasts
`PACKET_WBN_REKEY` to every WBN-participating remote client, carrying the new `server_key` zero-padded into the
65-byte `wbnJoinKey`-shaped wire envelope (same encode/decode
helpers in `bolo/wbn_key_codec.c`, by design — the uniform shape
is intentional). On receipt, each client stores the new key and
the existing lobby-snapshot re-auth path mints a fresh
`player_key` via `winbolonetClientJoinSession` and ships it in
`PACKET_WBN_REAUTH`. A single-target variant of the same packet
fires once per new join, immediately after `JOIN_ACCEPT`, so a
freshly-connected client learns the current `server_key` without
it ever riding the JOIN wire — JOIN starts anonymous (empty
`wbnJoinKey`) and the slot becomes WBN-verified on the next
snapshot tick. Both call sites are gated on
`winbolonetIsRunning()` so non-WBN servers pay nothing.

`PACKET_WBN_REKEY` sits alongside the existing wire-only
exceptions (JOIN_ACCEPT, PONG):
per-client reliability with no in-process audience. Routing through
`ControlEvent` would put a WBN-specific concept on the sim's T1
surface where nothing else in the sim references it.

### Cross-tier dependency: `transport_udp_client.c` → `winbolonet_client`

Today `src/winbolonet/` depends on `src/bolo/` via T1
(`server_sim.h`), and nothing in `src/bolo/` reaches back. The
client-side two-token flow introduces one explicit exception:
`src/bolo/transport_udp_client.c` includes
`../winbolonet/winbolonet_client.h` to call
`winbolonetClientJoinSession` during JOIN, rejoin, and
lobby-snapshot re-auth. It is the single chokepoint that owns
the client-side join handshake on behalf of every UI frontend —
without the edge, every frontend would have to duplicate
call-WBN-then-pass-key-to-netSetup. So `transport_udp_client.c`
gets the allowance; nothing else in `src/bolo/` does.

The transitive consequence: every binary linking `bolo_static`
must resolve `winbolonetClientJoinSession`. WBN-aware binaries
(WinBolo, WinBoloDS, WinBoloHeadless) get the real
implementation from `winbolonet_client`; stub-set binaries
(mobile, wasm, braintest, mapeditor, unit-tests, gym) get a
no-op stub returning failure. Same pattern as
`winboloNetGetServerKey` already follows.

### What not to do

- **Do not `#include "winbolonet_server.h"` from a UI translation
  unit** (`src/gui/sdl3/dialogs/*`, `gamefront.c`, etc.).
  `winbolonet_server` is a sim-runtime surface. If a UI file
  thinks it needs it, the answer is either a T1 sim accessor or
  surfacing the data through `winbolonet_client` (which is what
  `wbn_comments` already does).
- **Do not `#include "winbolonet_client.h"` from server-side TUs**
  (`src/server/*`). Server code has no API token and no business
  signing in as a user. The one allowed crossover inside
  `src/bolo/` is `transport_udp_client.c`, which owns the
  client-side join handshake on behalf of every UI frontend (see
  "Cross-tier dependency" above). Other sim TUs — everything else
  in `src/bolo/`, everything in `src/server/` — do not include
  `_client.h`.
- **Do not pass the API token to any `winbolonet_server` function.**
  The API token's only legitimate destinations are the
  `winbolonet_client` auth/join functions
  (`winbolonetAuthLogin`, `winbolonetAuthSteam`,
  `winbolonetAuthValidate`, `winbolonetClientJoinSession`). From
  anywhere else it should already have been exchanged for a
  `player_key`. A wire field carrying a credential into a game
  server is the bug the two-token rule above exists to prevent.

## Privileged exceptions

Four non-bolo directories are permitted to include T2 headers
today. Each has its own CMake profile in `cmake/bolo_lib.cmake`,
a scoped justification, and a written note of what it rests on.

Nothing here is time-limited, and nothing removes itself. Each
exception was granted because some fact about that directory is
true — it never ticks a sim, it is not shipped, it has one
consumer — and the **Rests on** line names that fact. If the fact
stops being true the reason for the exception has gone with it,
and the directory goes back under the ordinary rule. Whoever
makes that change is the one who has to notice: the build will
not complain, so a stale entry here is a lie about the codebase
rather than a broken compile.

### `src/mapeditor/`

The map editor edits `.map` files and does not run the simulation.
The bug class this architecture prevents is *asymmetric runtime*
behaviour; mapeditor has no runtime to be asymmetric with. It *may*
render a preview that disagrees with how the game later renders
the same map — that is a preview-accuracy concern, not a runtime-
asymmetry concern.

Scope: map-data structures (`bolo_map.h`, `pillbox.h`, `bases.h`,
`starts.h`, and friends) plus the map-render math it needs for
the editor preview.

The same scope covers the non-shipping map generators under
`src/mapeditor/tools/` — currently `make_tile_test_map.c`, the
`MakeTileTestMap` target (`EXCLUDE_FROM_ALL`, see
[TOOLS.md](TOOLS.md#maketiletestmap)). They write `.map` files
through the same map-data headers and never tick a sim, so they
sit inside the editor's exception rather than needing one of
their own. They belong here and not in `tools/`, which is a
public-only directory — see the `tests/`, `tools/` row in "Who
may include what".

**Rests on** the editor never ticking the world. Remove this the
moment anyone adds in-editor playtest, live preview against a
running sim, or any other path that runs the sim from the editor:
at that point mapeditor joins the T1+T3+T4 group and the map-data
access moves behind T1 accessors.

The editor links `scenario_static` to check the script in its
scenario panel, to list the `game.*` calls for completion, and to
read the function catalogue and the op signatures behind its
trigger and function panes — ten calls, named in the
`src/scenario/` row above — and that does not disturb any of the
above. The check makes no `ServerSim`: it is handed NULL where one
would go, which is why the one check that reads a map — the tag
check, which asks how many pills, bases and starts the map
carries — does not run in the editor and the panel says so. A
check that wanted it would have to build a sim, and that is the
in-editor playtest this exception is written against — so it is
not a thing to add quietly here.

### `src/braintest/`

BrainTest is a dev visualisation tool for inspecting bot brain
state. It is not shipped to players, only used internally for
bot development. The asymmetric-runtime bug class does not apply
because it is the only consumer of these introspection getters
and no production code path depends on them.

Scope: brain introspection (`bot_manager.h`, `brain_pathfinder.h`,
`brain_overlay.h`, `braincore.h`, `control_event.h`).

**Rests on** BrainTest being the only consumer of these getters.
Remove this the moment a second one needs the same access — at
which point the right answer is to deep-copy the introspected
state into POD types on a public header.

### `src/gym/`

The winbolo_gym ML training harness extracts observation and
reward signals from a running sim for reinforcement-learning
rollouts. It is an offline training tool, not shipped to players
in this form.

Scope: `GameSim` layout (`game_sim.h`) and the per-substruct
headers (`players.h`, `tank.h`, `shells.h`, `lgm.h`, etc.) used
for observation and reward extraction.

**Rests on** gym not being shipped to players. Remove this the
moment it ships in any player-facing distribution: at that point
the observation builder migrates onto the snapshot APIs the GUI
clients already use, and gym drops back to the standard
public-only access.

### `tests/unit/`

The `WinBoloUnitTests` binary asserts on in-process invariants
that have no T1 expression: wire-codec and channel-mux byte
layouts, per-client snapshot and terrain-copy state, client-side
view and overview bookkeeping, passive transport queue mechanics
under cross-thread access, subscriber-side ClientSim state after a
control-event publish. It is not shipped to players, has a single
consumer (CTest), and is not a runtime peer of the GUI / server /
mobile / wasm clients, so the asymmetric-runtime bug class does
not apply.

Scope: broad, and deliberately so — a test asserts on the state
the code actually keeps. In practice it reaches the sim state
structs (`game_sim.h`, `players.h`, `tank.h`, `pillbox.h`,
`bases.h`, `shells.h`, `mines.h`, `lgm.h`, `starts.h`,
`allience.h`, `bolo_map.h`), both sim internals
(`client_sim_internal.h`, `server_sim_internal.h` and the
`server_sim_*` helper headers), the wire and transport layer
(`transport.h`, `transport_udp.h`, `transport_udp_internal.h`,
`channel_mux.h`, `bulk_transfer.h`, `netpacks.h`, `wire_codec.h`,
`wire_messages.h`, the control and command codecs;
`transport_udp_server_internal.h` for the per-slot upload reservation
and its expiry, which the loopback upload timeout tests drive with a
synthetic clock and which no T1 call reports), the client's
view and render internals (`viewport.h`, `overview_map.h`,
`interpolation.h`, `scroll.h`, `messages.h`), and the bot and
brain headers (`bot_manager.h`, `braincore.h`,
`brain_pathfinder.h`).

**Rests on** the binary not being a runtime peer: not shipped in a
player-facing distribution, and no consumer beyond CTest. Remove
this if either stops being true — at that point it is a peer like
any other and the bug class applies to it.

**Kept honest by** review of the scope list, not by that condition,
which is not expected to fire. The list above is the part that can
rot: it reached its present size by being appended to, a header at
a time, while the sentence describing it stayed still. So it is
read at each release — every T2 include a new T1 accessor has made
unnecessary comes off, and nothing goes on without a line saying
what it observes that T1 cannot. A list that grows across two
releases with nothing coming off means the review has stopped, and
the grant needs re-arguing rather than extending.

**Linked GUI sources.** A second, narrower exception rides on the
same target, and it is not a T2 grant. Eleven `src/gui/sdl3` files
are compiled *into* `WinBoloUnitTests`, the only files from a
renderer directory that are: `skin_source.c`, `tileloader.c`,
`sdl_bmp.c`, `sound_variants.c`, `overview_camera.cpp`,
`overview_fog.cpp`, `overview_hud_layout.cpp`, `sprite_positions.c`,
`ring_band.c`, `dialogs/dialog_quit.cpp` and `gfx_settings.c`.
Between them they hold skin lookup, the tile sheet
builder, the BMP sheet reader, the sound variant naming, the map
overview's camera maths, its fog mask, its in-window HUD geometry
and the sprite placement arithmetic behind `mapview.c`'s drawers.
`ring_band.c` is here for `ringAnimAt`, which is where a closing ring
is and how solid after a given elapsed time — pure arithmetic, no
renderer, and the one piece of the smart ping's arrival effect and
the overview's respawn ring that a test can observe at all.
`dialogs/dialog_quit.cpp` is the odd one by directory and not by
rule: it is the only file under `dialogs/` that holds no ImGui, and
it is there so that `dialogQuitClassify` — what a quit means to
whichever dialog is up — can be driven from a test with hand-built
events instead of a window (see "Standalone ImGui dialogs" below).
The first ten are each called directly by a test beside them;
`gfx_settings.c` is here because `tileloader.c` calls it, and is the
one file on the list no test drives on its own.

They do not borrow the target's T2 access. They keep the `gui`
profile's public-only rule. That is the rule which qualifies a file
for this list: **a leaf a test can call with no display attached —
no ImGui, and no window, renderer or audio device of its own.**
Nothing here creates or holds one. `sdl_bmp.c` marks where the
boundary runs: its upload calls take an `SDL_Renderer *` they are
handed, and the surface-level half the tests use needs none, so the
file goes in while a file that opened a renderer would not. The
drawing half of the overview (`overview_view.cpp`) does not qualify
and stays out.

The alternative, for the geometry files, was moving the maths into
`src/bolo/`, which would put pixel, zoom and panel-layout concerns
onto the sim purely to buy testability. Keeping them in the renderer
and linking the leaf files is the smaller distortion of the two.

**Rests on** each of the eleven still meeting that rule, so it is
checked per file rather than for the group. One that gains an ImGui
include, or that opens a renderer or a device of its own, has left
the category, and the answer is to split the leaf back out — the
link break is the signal, not a build problem to route around by
widening the test binary. A twelfth file joins only on the same
test: callable with no display attached, or it does not go in.

Six `src/mapeditor` files ride the same rule from a different
directory and are not on that list: `mapeditor_scenario.c`,
`mapeditor_scenario_form.c`, `mapeditor_scenario_check.c`,
`mapeditor_scenario_pack.c`, `mapeditor_scenario_fndesc.c` and
`mapeditor_scenario_fnscan.c`. Between them they hold the scenario
panel's non-drawing half: the script file beside a map, the manifest
behind the metadata, lobby, rules, tags and trigger forms, the check
over the pane's text, the container the editor writes, the description
and stub text shown beside each function, and the scan for which
functions a script has already written. Each is plain C that draws
nothing and opens no window, renderer or device, so each meets the
same test the eleven do. `mapeditor_scenario_check.c` is the one of
the six that reaches the scenario library's Lua-facing side:
`scenarioValidateSource` loads the pane's text in a Lua state of its
own, which none of the other five do. `mapeditor_scenario_fndesc.c`
reads the same function catalogue through `scenarioLuaFunctions`, but
reads it out of a table and opens no state.

### `src/bolo/scenario_api/`

Not a T2 grant, and it widens nothing — it is recorded here because
it is the other place `cmake/bolo_lib.cmake` decides who sees what,
and because it carries the same kind of condition its neighbours do.
The directory is a third peer beside `public/` and `internal/`, and
the `scenario_host` profile that reads it sees `public/` plus that
directory and no sim internals at all.

It holds the entry points a scenario changes the world through — the
op funnel, the policy vtable, the tick and state registrations — and
the POD types they take. Reads are ordinary T1 accessors on
`server_sim.h` and events are the control bus, so this is the whole
of the non-public scenario surface.

Scope: `scenario_defs.h` and `server_sim_scenario.h`. `sim_owner`
sees them to implement the funnel and `unittests` to drive it, and
because `server_sim_internal.h` includes `scenario_defs.h`, the other
three privileged profiles (`mapeditor`, `braintest`, `gym`) see the
directory too; they already see the whole tree. `gui` and
`runtime_only` do not, so a frontend translation unit that includes
the funnel header fails to compile. That is checked rather than
assumed: the CTest entry `include_rules.scenario_api_hidden_from_gui`
builds exactly such a translation unit under the `gui` profile and
expects the build to fail. One POD the funnel's payloads use,
`ScnTable`, lives in `public/scenario_table.h` rather than here,
because it is also the parameter of the public `serverSimCreateBot`;
a type a public call takes cannot live behind this door.

**Rests on** these being server-authoritative entry points, the same
footing the lifecycle start functions already have: a frontend that
wants to change the world sends a command, and a scenario is the one
caller whose intent is applied directly. Revisit it the moment a
second kind of caller needs the funnel, or a call in here becomes
something a frontend legitimately makes — at that point the calls
that qualify move to `public/` under the ordinary T1 rule and the
directory keeps only what is left.

**The one call that qualifies and stays.**
`scenarioCheckRulesFromClassic` takes no `ServerSim` — it is the
rules check the map editor makes holding a script and no round — so
the rule above would send it to `public/`. Its return type is what
keeps it here: it answers `ScnOpResult`, which is declared in
`scenario_defs.h`, and moving the declaration out would put a
`scenario_api/` type in the tier every target sees. It is the
`ScnTable` condition read the other way round — a type a public call
takes cannot live behind this door, and a call that answers a type
behind this door cannot move out in front of it.

**The consumer: `src/scenario/`.** `scenario_static` is the one
target under the `scenario_host` profile and the proof that the two
headers build against `public/` alone. Its shape follows from the
profile: every read a binding makes is a T1 accessor on
`server_sim.h`; every write goes through `serverSimApplyScenarioOp`;
events arrive through the ordinary subscriber bus; and the sim
reaches back into the host only through registered pointers
(`serverSimSetScenarioTick`, `serverSimSetScenarioRoundBoot`,
`serverSimSetScenarioRoundStart`, `serverSimSetScenarioMapChanged`,
`serverSimSetScenarioReload`, `serverSimSetScenarioMapScripted`, and
the policy vtable), because the sim library cannot link the scenario
library. Two things go the other way as data instead, so the sim can
act on them with the host gone. The host hands over a
`ScnLobbyTemplate` by value and the sim seats and reconciles it
without calling out; it hands over the scenario's identity — the
source, the name, the file name, the description and the extra-teams
flag — through `serverSimSetScenarioIdentity`. The two are kept apart
because they have different lives: the template is re-read every time
a lobby is seated or reconciled, and the identity is only copied onto
the lobby-settings event. The frontend-facing header is
`scenario_host.h`, which includes `server_sim.h` and nothing from this
directory, so the dedicated server, the desktop host and the headless
runner attach a scenario without seeing the funnel.

**How a round start reaches the scenario.** A start makes exactly two
calls into it, both through `serverSimScenarioStartCall` in
`src/server/sim/server_sim_round.c`: the boot, before the start batch
picks a square or a tank is built, and the round-start call once the
world, the tanks and the roster are built and the state already reads
running. `src/scenario/scenario_host.c` answers them with
`scnRoundBootLocked` — a fresh Lua state, the chunk run again in it,
the table read again, then the rules — and `scnRoundSetupLocked`,
which is `on_setup`. Four things hold across both calls, and a fifth is
settled before either of them runs:

- The setup window is open. The funnel refuses every op while a start
  is in progress, and the window is what lets the ops through; the six
  roster ops are the exception and keep their refusal either way,
  because what that guard exists for is a roster edit re-entering the
  all-ready detector.
- The frame's game-event buffer is put back to what it held on entry.
  What a scenario arranges is the world the round begins in rather than
  something that happened in it, so it rides the opening snapshot's own
  tank, base and pill lists: sixteen bases dealt at setup arrive as
  sixteen owners with no captures in front of them.
- The map-change callback is installed. Only a running tick installs it
  otherwise, and a start is not one, so without this every terrain
  square a scenario writes would reach the server's own map and no
  client's.
- The stats funnel in `serverSimAddEvent` is skipped. The state reads
  running by the time the round-start call is made, so without this the
  same sixteen bases would be sixteen captures on a seat's record and
  sixteen lines on the round's timeline at tick 0.
- The round has already been put on the classic rules table. Both
  starts write it at the top, before the boot call, so a scenario's own
  rules go over a known table rather than over the last script's, and a
  round with none of its own to write — the round after a scripted
  round, and a round whose scenario failed to boot — plays classic.

**What a scenario looks like from the lobby side.** None of it comes
through `server_sim.h`. The identity and the base game type ride the
scenario tail of `CTRL_LOBBY_SETTINGS`, which the codec writes only
when a scenario is attached, so a lobby with none puts exactly the
bytes on the wire it always did; a client reads them back through five
accessors on `client_sim.h` — `clientSimGetLobbyScenarioSource`,
`Name`, `FileName`, `Description` and `ExtraTeams` — while the base
game goes onto the client's own `GameSim`, where `gameTypeResolve`
reads it.

Two T1 calls on `server_sim.h` are the other half:
`serverSimScenarioSeatLobby` and `serverSimScenarioApplyLobbyRules`.
The sim reaches them itself at three points — a map being committed, a
lobby resetting once the last player leaves, and a startup that skips
the lobby. A boot that opens a lobby is none of the three, so a process
booting onto a scripted map makes the calls for itself, and all three
hosts follow the same rule.

The rules are applied before a startup that will start the round: the
round is built inside `serverInstanceStartup`, and a game type set after
it would never be asked for. A startup that skips the lobby then seats
the template itself, inside `serverSimApplyInstanceConfig`, between
writing the bot brain path and AI level the seating reads and starting
the round that builds a tank for each fielded seat. So the hosts seat
only on their lobby paths, and apply the rules on both: seating a second
time runs `serverSimScenarioClearSeats` first, which would empty those
seats and rebuild them inside a round already running, leaving them with
no tanks.

The scripts switch is process-wide and is set at every site that can
attach — `-noscenarios` on the dedicated server, `--noscenarios` on the
headless runner — and again whenever the desktop preference "Run map
scripts when hosting" changes, rather than once at startup, so the
preference is true of the process from the moment it moves rather than
from the next hosted game.

`scenarioHostSetUploadScriptsEnabled` is the narrower one beside it,
set the same way at the same sites (`-nouploadscripts`,
`--nouploadscripts`, and the desktop preference "Run scripts in
uploaded maps"), and it decides whether a map a client uploaded may
bring a script. What tells an uploaded map from an operator's own is
its path and nothing else: the upload path writes the bytes it was
sent under their final name and stamps nothing on the file, so the
library asks `serverSimGetUploadsDir` — a T1 accessor answering what
the virtual `Uploads` folder resolves to — and compares that prefix.
The comparison levels separators and drops case, which can only call a
map an upload that is not one; the mistake the other way would run a
script the operator switched off.

Two things the profile does not enforce, so review does. There is no
`include_rules` CTest entry proving `scenario_host` cannot see
`internal/` — only the `gui` direction is proved — so a stray internal
include in `src/scenario/` fails the build today but nothing would go
red if the profile were widened to admit it. And `scenario_static`
publishes its own directory to whatever links it, so a frontend target
that also links `lua_static` could include `scenario_lua.h`,
`scenario_events.h` or `scenario_sandbox.h`; those headers are the
library's and the unit tests' by intent — each names Lua types, which
is the line `scenario_host.h` stays the other side of — and a frontend
that reaches for one is reaching past `scenario_host.h` for a reason
that wants a T1 accessor instead. The map editor is the one frontend
that reads one of the three on purpose: `mapeditor_scenario_check.c`
includes `scenario_lua.h` for the `game.*` rows and
`mapeditor_scenario_fndesc.c` for the two function lists, which is the
linkage the editor's own exception a few sections up already settles.
It touches neither of the other two, and for every frontend but that
one the warning stands as written. `scenario_manifest.h` is the other
kind: it belongs to `scenario_io_static`, names no Lua type, and a map
editor or a log viewer is meant to include it.

### Adding a new exception

A new exception requires the same structure: a directory with its
own CMake profile, a documented scope (which T2 headers and why),
and a written note of what it rests on — the fact about that
directory which makes the asymmetric-runtime bug class not apply,
stated so that when the fact changes the exception goes with it.
Without that, the default answer is "add a T1 accessor".

## Per-file T2 grants

The five privileged profiles above (`sim_owner`, `mapeditor`,
`braintest`, `gym`, `unittests`) grant T2 access at the directory/target level;
`scenario_host` grants none and so plays no part in this mechanism.
A finer-grained mechanism — `bolo_grant_internal_source_access` in
`cmake/bolo_lib.cmake` — grants T2 access at the individual source-
file level inside a target that is otherwise locked to `public/`. It
hands the file the same include path `sim_owner` has, scenario
surface included, so a granted file and a `server_sim_static` file
resolve the same headers.

It exists because some sim source files have to compile per-target
rather than join `bolo_static` or `server_static`. The reasons fall
into two categories.

**Compile-def divergence.** A TU has `#ifdef HAVE_STEAM` (or another
target-specific switch) and its gated code paths can't be archive-
dropped — every binary's view of the symbol surface must match its
own defines. Files in this category:

- `src/bolo/transport_udp_client.c` — `HAVE_STEAM` flips the
  `PLAYER_FLAG_STEAM_BUILD` bit in the JOIN_REQUEST path.

**Per-target globals, `main()`, or target-specific stubs.** A TU
owns target-specific globals or the binary's entry point, or stubs a
sim-internal function whose production body lives in a static archive
the target deliberately doesn't link:

- `src/server/servermain.c` — the dedicated-server `main()` and the
  module globals it owns.
- `src/server/server_frontend_stubs.c` — stubs the T2 callbacks
  bolo's sim TUs expect when there is no UI. Stubbing a T2 function
  requires its signature visible.
- `src/braintest/braintest_lifecycle_stub.c` — BrainTest deliberately
  omits `server_static`'s transport/WBN/maxmind chain, so the real
  `serverInstanceStartup` body in `server_lifecycle.c` isn't on its
  link line. The stub is a thin wrapper that forwards to
  `serverSimApplyInstanceConfig` (which lives in `server_sim_static`,
  reachable by both call paths) and stubs `serverInstanceTick` /
  `serverInstanceShutdown` as no-ops.
- WinBolo's embedded map editor TUs (`src/mapeditor/mapeditor.c`,
  `mapeditor_export.c`, `mapeditor_validate.c`) — sim-co-owner files
  from the editor that need T2 access regardless of which binary
  they're compiled into.

**Frontend sim-driver without per-actor ClientSim.** A frontend that
drives an in-process ServerSim with bots but doesn't allocate a
ClientSim per bot, so the wire-wrapper migration that routes SP-host
lobby mutations through the local transport doesn't apply.

- `src/gui/sdl3/bg_game.c` — the lobby-background animation. Mutates
  `lobbyPlayers[].teamNumber` via `serverSimSetTeamBatch` (paired with
  `serverSimReapplyTeamAlliances` after the batch) because there's no
  ClientSim per bot to call `clientSimNetSendTeamSet` on. The grant is
  bounded to that team-setter pair; the rendering helpers read
  `viewPlayer` via a parameter threaded from the render entry point,
  not via the sim's `viewPlayer` field, so they don't need privileged
  access.

This is not a third tier of privileged exception. The grants are a
CMake-level workaround for archive packaging, not an architectural
relaxation: the files are sim co-owner code that happens to be
per-target. The asymmetric-runtime bug class doesn't apply because
each file participates in one binary at a time, and its T2 use is
internal to the sim it co-owns.

**When NOT to use it.** If a *frontend* TU needs T2 — that's the bug
class this architecture exists to prevent. Add a T1 accessor instead.
Recent example: `src/headless/headless_main.c`'s `--cmd-stdin`
`--fast` dispatch used to be granted access; the fix was to add
`serverSimAcceptAlliance` / `serverSimLeaveAlliance` /
`serverSimSetPlayerName` to `server_sim.h`. If a TU could just join
`bolo_static` or `server_static`, do that instead.

## Layout on disk

The compile-time boundary is **two-tier**: `public/` (open to
external targets) vs `internal/` (sim-only, plus privileged
exceptions). The T3/T4 distinction inside `public/` is
documentation, not a separate directory.

```
src/bolo/public/       — T1 + T3 + T4 headers
src/bolo/internal/     — T2 headers
src/bolo/scenario_api/ — the scenario write and policy surface
src/bolo/              — sim .c files only (no headers)
src/scenario/          — the scenario runtime; public/ + scenario_api/
                         on its path, nothing in internal/
src/scenario_io/       — a scenario's files (container, manifest.json,
                         the chunk on a map); public/ alone on its path
```

External targets get `src/bolo/public/` on their include path —
that single directory contains T1, T3, and T4 headers, so any
target with `public/` on its path can see all three. The four
privileged profiles (`mapeditor`, `braintest`, `gym`, `unittests`)
additionally get `internal/` and the flat `src/bolo/` directory.
`scenario_api/` is not part of either group: `scenario_host` gets
`public/` plus that directory, and the privileged profiles get it as
well so the funnel can be implemented and tested.
`src/bolo/`'s own target (`sim_owner` profile) has all three on
its include path
so internal-to-bolo includes can stay short (`#include "tank.h"`,
not `#include "internal/tank.h"`).

The single point of policy for include paths is
`cmake/bolo_lib.cmake`'s `bolo_apply_include_rules(target, profile)`
helper. Every target in the tree — desktop, iOS, Android, wasm,
server, headless, logviewer, gym, braintest, mapeditor, the scenario
runtime — is wired through it.

## Enforcement

The include rules are enforced by CMake `target_include_directories`:
external targets simply do not have `src/bolo/internal/` on their
include path, so a stray `#include "tank.h"` from `src/gui/` fails
to compile. This is structural enforcement at the T1/T2 boundary —
the build catches T2 violations the moment they appear.

The T3-vs-T4 split, and the rule that non-renderer binaries should
not include T3 headers, are **not** compile-enforced. T3 lives
inside `public/` alongside T1 and T4, so any target that gets
`public/` can resolve a T3 include. These rules are upheld by
review.

Opacity of T1 handles (`ClientSim`, `ServerSim` as forward-declared
structs) reinforces the include rule — even where T1 is permitted,
the caller can only reach the published API surface.

## Why this exists

Historically the codebase had no header boundary between `src/bolo/`
and its clients. Every subdirectory could reach into any sim
internal, and several did. The result was a recurring class of bug
where a feature implemented against the desktop GUI's direct access
to sim state would silently fail on the server, headless runner,
gym, brain test, or mobile clients — because those binaries did not
share the GUI's code path into sim internals.

The tiered model exists to make that bug class impossible to write
in the first place. T1 is the only path into the sim that every
client shares, so any feature that ships through T1 ships
uniformly across every runtime.
