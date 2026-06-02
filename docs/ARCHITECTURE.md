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
| **T4 — Shared leaves** | Plain types and constants with no dependencies. | `types.h`, `global.h`, `tilenum.h`, `gametype.h`, `platform_types.h` |

## Who may include what

| Directory | Allowed tiers | Notes |
|---|---|---|
| `src/bolo/` | T1 + T2 + T3 + T4 | Owns T2; contributes to all tiers. |
| `src/gui/` | T1 + T3 + T4 | The desktop renderer. Cannot reach into sim internals. |
| `src/mapeditor/` | T1 + T2 + T3 + T4 | Privileged exception (see below) — full T2 access for map-data editing. |
| `src/braintest/` | T1 + T2 + T3 + T4 | Privileged exception (see below) — dev visualisation tool, not shipped to players. |
| `src/gym/` | T1 + T2 + T3 + T4 | Privileged exception (see below) — ML training harness, not shipped in player builds. |
| `brains/` | T1 + T2 + T3 + T4 | Builds `bot_brains_static` (bot brain implementations — GoalHunter, ONNX backends). Compiles under the `sim_owner` profile because brain evaluation reads sim state directly. Not a frontend; every binary that ships bots links the same `bot_brains_static`, so the asymmetric-runtime bug class doesn't apply. |
| `src/server/` | T1 + T2 + T3 + T4 | Co-owner of the sim alongside `src/bolo/`. Most files compile via three libraries: `server_sim_static` (sim core: `server_sim.c`, `servermessages.c`); `server_static` (dedicated-server runtime on top of it: `transport_udp_server.c`, `server_lifecycle.c`, `geolookup.c`, plus `threads_static` PUBLIC-linked); and `threads_static` (the SDL-mutex thread manager — `threads.c` on every platform except Emscripten, where `threads_wasm.c` substitutes single-threaded no-ops with the same symbol surface). `threads_static` is consumed by every binary that ticks a sim, not only the dedicated server: in-process single-player builds (WinBoloIOS, android main, wasm winbolo, WinBoloUnitTests) link it directly; the four dedicated-server binaries get it transitively through `server_static`. Three more files are per-target sim runtime that ship inside WinBoloDS with T2 access via `bolo_grant_internal_source_access`: `servermain.c` (owns the dedicated-server `main()` and module globals), `server_frontend_stubs.c` (stubs the T2 callbacks bolo's sim TUs expect when there is no UI), and `server_dedicated_log.c` (the dedicated-server's replay-log subscriber, registered against the ServerSim bus from `servermain.c`). See "Per-file T2 grants" below for the mechanism. |
| `src/headless/` | T1 + T3 + T4 | Same as server. |
| `src/wasm/` | T1 + T3 + T4 | Web build of the desktop client. |
| `src/android/` | T1 + T3 + T4 | Mobile renderer; uses T3 like `src/gui/`. |
| `src/ios/` | T1 + T3 + T4 | Mobile renderer; uses T3 like `src/gui/`. |
| `src/logviewer/` | T1 + T3 + T4 | Replays recorded games; uses T3 for the playback render path. |
| `src/winbolonet/winbolonet_core/` | T1 + T4 | Shared HTTP, async event queue, WBN key storage. Includes `server_sim.h` (T1) only. Linked by every WBN-aware binary. |
| `src/winbolonet/winbolonet_server/` | T1 + T4 | Server tracker calls (`server/register`, `server/update`, lobby/map/teams/balance). Linked by binaries that run a server: WinBoloDS, WinBoloHeadless, SDL3 client (SP host). |
| `src/winbolonet/winbolonet_client/` | T4 | User auth, comments. Linked by binaries with a UI: SDL3 client, LogViewer. |
| `tests/unit/` | T1 + T2 + T3 + T4 | Privileged exception (see below) — in-process tests of bolo internals. Not shipped to players. |
| `tests/`, `tools/` | T1 + T3 + T4 (by default) | Not currently wired through a profile. Tests that legitimately need T2 belong inside `src/bolo/tests/` and link against bolo's own target. |

**The enforced rule of thumb is two-tier**: outside `src/bolo/`, you get
everything in `public/` (T1 + T3 + T4) and nothing in `internal/` (T2).
The T3-vs-T4 distinction and the "don't use T3 in non-renderers" guidance
are policy enforced by review, not by the build. The four privileged
exceptions (`mapeditor`, `braintest`, `gym`, `tests/unit/`) get full
T2 access via dedicated CMake profiles.

## What clients must do

1. **Reach the sim through T1 only.** Construct, drive, and query the
   simulation via `client_sim.h` and `server_sim.h`. Treat `ClientSim`
   and `ServerSim` as opaque handles. Do not assume struct layout.

2. **Use T1 accessors for anything sim-owned.** Tank state, player
   state, map data, bot state, network status — all of it is reached
   by calling a function on the sim handle, not by reading a struct
   field.

3. **Use T4 freely.** `types.h`, `global.h`, etc. have no dependencies
   and exist precisely so every subdirectory can share basic
   primitives without coupling.

4. **In the desktop renderer (`src/gui/`), treat T3 as read-only.**
   `viewport`, `screentank`, `screenbullet`, `screenlgm`, and friends
   are per-frame views the sim publishes for drawing. Read them.
   Do not mutate them. (Where the receive-side API can be tightened
   to `const`, it should be.)

## What clients must not do

The following patterns are the bug class this architecture exists to
prevent. They all manifest as **asymmetric runtime behaviour** —
features that work on one client (e.g. the desktop GUI) and silently
break on another (server, headless, gym, brain test, Android, WASM)
because the broken client did not run the same code path.

1. **Do not include T2 headers from outside `src/bolo/`** (the
   privileged exceptions — `mapeditor`, `braintest`, `gym`,
   `tests/unit/` — are bounded by the scopes listed below). If you find yourself
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
   `tests/unit/`) each carry a documented scope and expiry condition — see the
   "Privileged exceptions" section below. `src/server/`,
   `src/headless/`, `src/wasm/`, `src/android/`, and `src/ios/`
   all run the sim and all participate in this bug class. A new
   exception requires the same justification structure: bounded
   scope, written expiry condition, and a reason the asymmetric-
   runtime bug class doesn't apply. The default answer is still
   "add a T1 accessor".

5. **Do not include T3 from non-renderer binaries.** `src/gui/`,
   the mobile renderers (`src/android/`, `src/ios/`, `src/wasm/`),
   and `src/logviewer/` (for replay rendering) are the legitimate
   consumers of T3 headers. Server, headless, and gym should not
   reach for `screentank.h`, `viewport_types.h`, etc. — they don't
   draw. This rule is enforced by review, not the build: T3 lives
   in `public/` alongside T1 and T4, so the include path doesn't
   physically block a stray T3 include from a server TU. Catch it
   in code review.

## Writing a new frontend

A frontend is any binary outside `src/bolo/` that drives a `ClientSim`
or `ServerSim` through the public T1 API. Existing frontends are
`src/gui/sdl3/` (desktop), `src/android/` and `src/ios/` (mobile),
`src/wasm/` (web), `src/headless/` (no UI), `src/braintest/` (dev
tool), `src/logviewer/` (replay viewer), and the dedicated-server
binary built from `src/server/`. Note: the dedicated-server binary
is not a pure T1-only frontend — `servermain.c`,
`server_frontend_stubs.c`, and `server_dedicated_log.c` are
per-target sim runtime with T2 access via per-file grant. See the
`src/server/` table row and "Per-file T2 grants" below.

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
`src/server/server_sim.c`). The implementation walks the registered
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

This is foot-gun removal, not compile-time enforcement. The old
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
| Per-tick world snapshot (positions, shells, deltas) | existing snapshot module |
| Per-client handshake / reliability (JOIN_ACCEPT, JOIN_REJECT, NAME_CHANGE_REJECT, MAP_DOWNLOAD chunks, PONG, PLAYER_LIST resync) | `src/bolo/transport_udp_server.c` / `src/bolo/transport_udp_client.c` |

### Recipe — adding a new event type

1. **Define the event.** Add a variant to `ControlEventType` in
   `src/bolo/public/control_event.h` and the corresponding union
   member to `ControlEvent.u`. If a typed populate helper makes
   call sites cleaner, add a `serverSimFill<Name>Event` function
   on `server_sim.h`.

2. **Define the wire form (if any).** Add an encoder and a decoder
   in `src/bolo/transport_control_codec.c`. Wire the encoder into
   `s_encoders[]` (keyed by `ControlEventType`) and the decoder
   into `transportControlCodecDecoder` (keyed by wire packet type).
   The encoder receives a per-recipient `UdpServerClient *recipient`
   it can ignore for fan-to-all variants or use for filtering
   single-target events — though the established precedent is to
   keep the codec recipient-agnostic and put the slot check in
   `udpClientDeliverControl` (`CTRL_ALLIANCE_REQUEST`'s target check
   lives there).

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

5. **Send wrapper (if client-originated).** A client-originated event
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
`serverSimDeliverToClientSim` (in `src/server/server_sim.c`) and the
bot manager's deliver callback — both end in
`clientSimApplyControl`. SP, bots, and network converge on one
funnel.

### Load-bearing wire-only exceptions

Two categories of packet stay wire-only by design; the single-
publish recipe does not apply to them:

- **Per-tick world snapshots.** Tank positions, shells, and per-tick
  deltas live in the snapshot module, not the codec. Snapshots also
  carry the per-client reliable control-event tail (see
  `PACKET_CONTROL_TICK` below for the lobby-phase equivalent), but
  that tail is fed by the bus — the snapshot module is the carrier,
  not the publisher.
- **Per-client handshake and reliability.** `JOIN_ACCEPT`,
  `JOIN_REJECT`, `NAME_CHANGE_REJECT`, `MAP_DOWNLOAD` chunks, and
  `PONG` are point-to-point transport mechanics. The reliable bus
  has two parallel carriers for events that DO ride a queue:
  - **Down-leg.** `PACKET_CONTROL_TICK` (server → client) and
    `PACKET_CONTROL_ACK` (client → server) drain the per-client
    `ControlEvent` queue when snapshots aren't flowing
    (lobby / countdown / gameover).
  - **Up-leg.** `PACKET_COMMAND_TICK` (client → server) and
    `PACKET_COMMAND_ACK` (server → client, unicast) drain the
    per-connection `ClientCommand` queue. Each entry carries a
    client-assigned `cmdSeq` for dedupe and reject correlation.
    See "Adding a new client→server command" below for the full
    carrier semantics.
- **`PLAYER_LIST` resync** is a load-bearing wire-only exception
  for a failure mode the reliable control queue does not reach:
  `transportUdpServerOnGameStart` wipes every per-client
  control-event queue at countdown end
  (`nextSeq = 1; ackedSeq = 1; memset(buffer, 0)`) before
  publishing `CTRL_GAME_PHASE_RUNNING`. A late-countdown joiner's
  `CTRL_PLAYER_JOIN` may still be in-flight (un-ACKed) for one or
  more existing clients at that instant; the memset destroys it
  and the next event published is `CTRL_GAME_PHASE_RUNNING` at
  seq=1, with no retransmit path back to the dropped JOIN. The
  server flips `needsPlayerList = true` for every connected client
  inside the same reset, and the per-tick send loop fires an
  unsolicited `PACKET_PLAYER_LIST` after the reset completes,
  restoring the missing roster entries. (The JOIN-time use of the
  same flag — `serverHandleJoinRequest` setting it for the new
  client — is redundant with `serverSimSyncSubscriber`'s replay of
  `CTRL_PLAYER_JOIN` per in-use player, and is kept as a
  belt-and-braces overlap.)

### Compatibility rules

- Adding a `ControlEventType` variant is additive. Subscribers that
  don't know about it fall through their `switch` default and ignore
  it. Safe.
- Adding a wire packet ID is additive. Old clients that don't
  understand the new ID drop the packet on the floor. Safe.
- **Never reorder, renumber, or repurpose existing wire packet IDs.**
  That breaks every deployed client that hasn't been rebuilt against
  the new code.
- **Never change the on-wire layout of an existing packet** without
  versioning. Add a new packet ID instead.

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

### Recipe — adding a new command

1. **Define the variant.** Add `CMD_<NAME>` to `ClientCommandType` in
   `src/bolo/public/client_command.h` plus a payload struct in the
   union (`u.<name>`). Variable-length payloads use fixed-size
   in-union buffers sized to `PACKET_MAX_*`.
2. **Add the codec pair.** In `src/bolo/transport_command_codec.c`,
   add `commandEncode<Name>` and `commandDecode<Name>` next to the
   existing variants, then register in the `commandCodecEncode` /
   `commandCodecDecode` dispatch tables. Reuse an existing
   `PACKET_*` packet number as the inner-entry tag.
3. **Add the dispatcher arm.** New `case CMD_<NAME>:` in
   `applyCommandInner` (`src/server/server_command_dispatch.c`).
   Owns state guards, authority checks (`lobbyClientMayEdit`, slot
   bounds), mutation, downstream publishes (`serverSimPublish*`),
   logging. Returns `CMD_OK` or a `CMD_REJECT_*` code.
4. **Add the send wrapper.** New `clientSimNetSend<Name>` in
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
- `clientSimNetSendLobbyMapListRequest` / `…SearchRequest` —
  request / paginated-response.
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
exceptions (JOIN_ACCEPT, MAP_DOWNLOAD, PONG, PLAYER_LIST resync):
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

Three non-bolo directories are permitted to include T2 headers
today. Each has its own CMake profile in `cmake/bolo_lib.cmake`,
a scoped justification, and a written expiry condition.

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

**Expires** the moment anyone adds in-editor playtest, live
preview against a running sim, or any other path that ticks the
world from the editor. At that point mapeditor joins the T1+T3+T4
group and the map-data access moves behind T1 accessors.

### `src/braintest/`

BrainTest is a dev visualisation tool for inspecting bot brain
state. It is not shipped to players, only used internally for
bot development. The asymmetric-runtime bug class does not apply
because it is the only consumer of these introspection getters
and no production code path depends on them.

Scope: brain introspection (`bot_manager.h`, `brain_pathfinder.h`,
`brain_overlay.h`, `braincore.h`, `control_event.h`).

**Expires** the moment a second consumer needs the same access —
at which point the right answer is to deep-copy the introspected
state into POD types on a public header.

### `src/gym/`

The winbolo_gym ML training harness extracts observation and
reward signals from a running sim for reinforcement-learning
rollouts. It is an offline training tool, not shipped to players
in this form.

Scope: `GameSim` layout (`game_sim.h`) and the per-substruct
headers (`players.h`, `tank.h`, `shells.h`, `lgm.h`, etc.) used
for observation and reward extraction.

**Expires** the moment gym ships in any player-facing
distribution. At that point the observation builder migrates
onto the snapshot APIs the GUI clients already use, and gym
drops back to the standard public-only access.

### `tests/unit/`

The `WinBoloUnitTests` binary exercises in-process invariants
that aren't reachable through T1 today — passive transport
queue mechanics under cross-thread access, subscriber-side
ClientSim state after a control-event publish. It is not
shipped to players, has a single consumer (CTest), and is not
a runtime peer of the GUI / server / mobile / wasm clients, so
the asymmetric-runtime bug class does not apply.

Scope: `transport.h` (the passive `transport_local` queue
indices the concurrency test asserts on), `game_sim.h` plus
`players.h` (the subscriber-dispatch test reads the client's
player table back through `&cs->sim.plyrs` after
`CTRL_PLAYER_NAME` delivery).

**Expires** the moment T1 accessors expose the passive
transport's queue state and the subscriber-side player view
the tests currently reach T2 to observe. At that point the
tests migrate to T1+T3+T4 (the default `tests/` row in "Who
may include what" above) and this profile is removed.

### Adding a new exception

A new exception requires the same structure: a directory with its
own CMake profile, a documented scope (which T2 headers and why),
and a written expiry condition (the change of circumstance that
brings the asymmetric-runtime bug class back into scope). Without
that, the default answer is "add a T1 accessor".

## Per-file T2 grants

The five privileged profiles above (`sim_owner`, `mapeditor`,
`braintest`, `gym`, `unittests`) grant T2 access at the directory/target level.
A finer-grained mechanism — `bolo_grant_internal_source_access` in
`cmake/bolo_lib.cmake` — grants T2 access at the individual source-
file level inside a target that is otherwise locked to `public/`.

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
- `src/server/server_dedicated_log.c` — registers a bus subscriber
  that maintains the WinBoloDS replay log in response to
  `CTRL_GAME_PHASE_*` events. T2 access is needed because the
  subscriber reads `sim->wantLogging`, `sim->mapName`,
  `sim->userLogFileName`, `sim->hasPassword`, `sim->playerConnected[]`,
  and routes through `playersGetAccountFlags`. Ships only in
  WinBoloDS; no other binary references the symbol surface, so no
  companion stub file exists.
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
src/bolo/public/    — T1 + T3 + T4 headers
src/bolo/internal/  — T2 headers
src/bolo/           — sim .c files only (no headers)
```

External targets get `src/bolo/public/` on their include path —
that single directory contains T1, T3, and T4 headers, so any
target with `public/` on its path can see all three. The four
privileged profiles (`mapeditor`, `braintest`, `gym`, `unittests`)
additionally get `internal/` and the flat `src/bolo/` directory.
`src/bolo/`'s own target (`sim_owner` profile) has all three on
its include path
so internal-to-bolo includes can stay short (`#include "tank.h"`,
not `#include "internal/tank.h"`).

The single point of policy for include paths is
`cmake/bolo_lib.cmake`'s `bolo_apply_include_rules(target, profile)`
helper. Every target in the tree — desktop, iOS, Android, wasm,
server, headless, logviewer, gym, braintest, mapeditor — is wired
through it.

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
