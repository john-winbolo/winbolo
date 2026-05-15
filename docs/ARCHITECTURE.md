# WinBolo Architecture — Header Tiers and Include Rules

This document describes how the codebase is partitioned into header
tiers, which subdirectories may include which tiers, and the patterns
clients of `src/bolo/` must follow.

The design and rationale are tracked in `plans/protectcore.md`; this
document is the stable reference for the rules themselves.

## The four tiers

| Tier | Purpose | Representative headers |
|---|---|---|
| **T1 — Sim runtime (public API)** | The official front door to the simulation. Opaque handles; no direct struct access. | `server_sim.h`, `client_sim.h`, `client_mapload.h`, `client_enums.h` |
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
| `brains/` | T1 + T2 + T3 + T4 | Builds `bot_brains_static` (bot brain implementations — NewAutopilot, ONNX backends). Compiles under the `sim_owner` profile because brain evaluation reads sim state directly. Not a frontend; every binary that ships bots links the same `bot_brains_static`, so the asymmetric-runtime bug class doesn't apply. |
| `src/server/` | T1 + T2 + T3 + T4 | Co-owner of the sim alongside `src/bolo/`. Most files compile via two libraries: `server_sim_static` (sim core: `server_sim.c`, `servermessages.c`) and `server_static` (dedicated-server runtime on top of it: `transport_udp_server.c`, `server_lifecycle.c`, `threads.c`, `geolookup.c`). Three more files are per-target sim runtime that ship inside WinBoloDS with T2 access via `bolo_grant_internal_source_access`: `servermain.c` (owns the dedicated-server `main()` and module globals), `server_frontend_stubs.c` (stubs the T2 callbacks bolo's sim TUs expect when there is no UI), and `server_dedicated_log.c` (real bodies for the dedicated-server's replay-log hooks; `server_dedicated_log_stubs.c` is the no-op pair every other binary picks). See "Per-file T2 grants" below for the mechanism. |
| `src/headless/` | T1 + T3 + T4 | Same as server. |
| `src/wasm/` | T1 + T3 + T4 | Web build of the desktop client. |
| `src/android/` | T1 + T3 + T4 | Mobile renderer; uses T3 like `src/gui/`. |
| `src/ios/` | T1 + T3 + T4 | Mobile renderer; uses T3 like `src/gui/`. |
| `src/logviewer/` | T1 + T3 + T4 | Replays recorded games; uses T3 for the playback render path. |
| `tests/`, `tools/` | T1 + T3 + T4 (by default) | Not currently wired through a profile. Tests that legitimately need T2 belong inside `src/bolo/tests/` and link against bolo's own target. |

**The enforced rule of thumb is two-tier**: outside `src/bolo/`, you get
everything in `public/` (T1 + T3 + T4) and nothing in `internal/` (T2).
The T3-vs-T4 distinction and the "don't use T3 in non-renderers" guidance
are policy enforced by review, not by the build. The three privileged
exceptions (`mapeditor`, `braintest`, `gym`) get full T2 access via
dedicated CMake profiles.

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
   privileged exceptions — `mapeditor`, `braintest`, `gym` — are
   bounded by the scopes listed below). If you find yourself
   reaching for `tank.h`, `players.h`, `game_sim.h`, or anything
   in the T2 list, the answer is to add a T1 accessor on the sim,
   not to add another include exception.

2. **Do not build packets from the GUI.** `bolo_packets.h`,
   `netpacks.h`, and `transport_udp.h` are T2. GUI code that today
   calls `transportUdpClientSendChat(...)` must migrate to
   `clientSimSendChat(cs, ...)`. The wire format is a private
   contract between `client_sim` and `server_sim`; callers go
   through the sim.

3. **Do not write to sim state directly.** Even if a field appears
   reachable through an internal header, mutating it from outside
   `src/bolo/` is the canonical asymmetric-runtime bug — the GUI
   client's copy of the world drifts from every other client's
   copy.

4. **Do not introduce new T2 exceptions casually.** The three
   exceptions that exist today (`mapeditor`, `braintest`, `gym`)
   each carry a documented scope and expiry condition — see the
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

**Setting up a client.**

```c
ClientSim *cs = clientSimAlloc();
clientSimCreate(cs, gameType, hiddenMines, startDelay, gameLen);
clientSimSetPlayerNum(cs, myPlayerNum);
clientSimSetupSelf(cs, myPlayerNum, "PlayerName", clientType, clientFlags);
```

`clientSimAlloc` heap-allocates and zero-initialises. `clientSimCreate`
applies game settings. `clientSimSetupSelf` populates the local
self-record (tank/LGM) once `setPlayerNum` has been called.

**Loading a map.** From disk:

```c
clientLoadMap(cs, "maps/everard.map", gameType, hiddenMines,
              startDelay, gameLen, "PlayerName", /*wantFree=*/false);
```

From an in-memory compressed blob (the UDP-join flow — server sends
the map during the join handshake):

```c
clientSimResetForMapLoad(cs);   /* drops map state, keeps transport */
clientLoadCompressedMap(cs, buff, buffLen, "mapname", gameType,
                        hiddenMines, startDelay, gameLen,
                        "PlayerName", playerNum, /*wantFree=*/false);
```

**Connecting.** Network (LAN, or internet via tracker — see `client_net.h`):

```c
clientSimConnectUdp(cs, serverAddr, serverPort, playerName,
                    password, wbnToken, wantRejoin,
                    trackerAddr, trackerPort);
```

In-process (single-player, or host-with-self):

```c
clientSimConnectLocal(cs, serverSim, playerNum);
```

The transport stays bound to the ClientSim across
`clientSimResetForMapLoad`, which is what makes UDP-join work:
connect → handshake → reset for map load → load compressed map.

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
Never build wire packets directly; that's a T2 violation.

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

**Player management.**

```c
serverSimAddPlayer(sim, playerNum, "Name", /*wantRejoin=*/false);
serverSimRemovePlayer(sim, playerNum);
serverSimAddBot(sim, playerNum, &botCfg);
```

The bot pool is process-global — call `serverSimBotPoolInit` once at
startup; multiple ServerSims share it.

**Lobby and start.**

```c
serverSimEnterLobby(sim);
serverSimSetTeam(sim, playerNum, teamNumber);
serverSimSetReady(sim, playerNum, true);
serverSimLobbyCheckAllReady(sim);   /* transitions to countdown */
serverSimStartGame(sim);            /* countdown done; world begins */
```

If players are added after `serverSimStartGame`, the server frontend
must call `serverSimReapplyTeamAlliances` to re-fix alliances. This is
tech debt — new frontends should add players and set teams before
calling `serverSimStartGame`.

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
| T1 send wrapper | `src/bolo/public/client_net.h` declares `clientSimNetSendChat`; implementation in `src/bolo/client_net.c` builds the wire packet |
| Wire (sender → server) | T2 packet type in `src/bolo/internal/bolo_packets.h` + send helper |
| Server reception | `src/bolo/` handler decodes the packet, validates |
| Server fanout — in-process | Publish a `ControlEvent` (e.g. `CTRL_PLAYER_CHAT`) via `serverSimPublishControl` — reaches local ClientSims, bots, host UI |
| Server fanout — wire | Broadcast the chat packet to every connected remote client via the UDP transport |
| Client reception — in-process | Local subscriber's `deliverCb` hands the message off to `frontEndMessages` |
| Client reception — remote | UDP-connected client decodes the packet, applies it locally, and publishes its own `CTRL_PLAYER_CHAT` so its own in-process subscribers see it; then `frontEndMessages` |
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

5. **Send wrapper (if client-originated).** If a *client* needs to
   trigger this event (e.g. a chat message), add a T1 send wrapper
   on `client_net.h` (`clientSimNetSend<Name>`) that builds the wire
   packet internally. Frontends call the wrapper; never build wire
   packets in `src/gui/` or any non-bolo directory.

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

Three categories of packet stay wire-only by design; the single-
publish recipe does not apply to them:

- **Periodic lobby refresh.** The 25-tick cosmetic refresh in
  `src/server/server_lifecycle.c` calls
  `transportUdpServerSendPeriodicLobbyRefresh`, which drives the
  same codec encoders the bus path uses but bypasses the bus. It
  carries ping and country data that bots, SP, and replay logs
  ignore — publishing it would wake every in-process subscriber
  400×/sec for nothing.
- **Per-tick world snapshots.** Tank positions, shells, and per-tick
  deltas live in the snapshot module, not the codec.
- **Per-client handshake and reliability.** `JOIN_ACCEPT`,
  `JOIN_REJECT`, `NAME_CHANGE_REJECT`, `MAP_DOWNLOAD` chunks,
  `PONG`, and `PLAYER_LIST` resync responses are point-to-point
  transport mechanics with no in-process audience.

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

### Adding a new exception

A new exception requires the same structure: a directory with its
own CMake profile, a documented scope (which T2 headers and why),
and a written expiry condition (the change of circumstance that
brings the asymmetric-runtime bug class back into scope). Without
that, the default answer is "add a T1 accessor".

## Per-file T2 grants

The four privileged profiles above (`sim_owner`, `mapeditor`,
`braintest`, `gym`) grant T2 access at the directory/target level.
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
- `src/bolo/client_mapload.c` — same flag.

**Per-target globals or `main()`.** A TU owns target-specific globals
or the binary's entry point, so it can't live in a shared archive:

- `src/server/servermain.c` — the dedicated-server `main()` and the
  module globals it owns.
- `src/server/server_frontend_stubs.c` — stubs the T2 callbacks
  bolo's sim TUs expect when there is no UI. Stubbing a T2 function
  requires its signature visible.
- `src/server/server_dedicated_log.c` — real bodies for the replay-
  log hooks; the no-op pair (`server_dedicated_log_stubs.c`) ships
  in every other binary, so the two can't both link.
- WinBolo's embedded map editor TUs (`src/mapeditor/mapeditor.c`,
  `mapeditor_export.c`, `mapeditor_validate.c`) — sim-co-owner files
  from the editor that need T2 access regardless of which binary
  they're compiled into.

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
target with `public/` on its path can see all three. The three
privileged profiles (`mapeditor`, `braintest`, `gym`) additionally
get `internal/` and the flat `src/bolo/` directory. `src/bolo/`'s
own target (`sim_owner` profile) has all three on its include path
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
