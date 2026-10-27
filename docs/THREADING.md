# Threading in WinBolo

This page says which thread runs which code, which data each thread
touches, and which lock guards that data. It also lists every frontend
callback in `src/bolo/public/frontend.h` and the thread it runs on.

It was written for issue #311. That issue names two problems:

1. On the desktop, a hold on the window title bar (to move or resize the
   window) stops the game loop. A long hold drops the player.
2. On the web, a tab without focus stops the game loop, and the player
   is disconnected.

The fix for problem 1 uses no new thread. The section "Ticks during a
window move or resize" tells how. Problem 2 was fixed earlier by PR #474.
The section "The web client" tells how, and what a fuller design needs.

## Threads

### Desktop client (WinBolo.exe)

| Thread | Created by | What it runs |
|---|---|---|
| Main | the OS | SDL events, ImGui, all rendering, all window calls, the client game tick (`windowRunGameTick`, `clientFrontRunTickStep`), the UDP client transport, sound playback calls, all dialogs. |
| SDL timer | SDL, on the first `SDL_AddTimer` | `windowGameTimer` (every 10 ms, `GAME_TICK_LENGTH`), `windowFrameRateTimer`, and `hostedServerTimerCb` (every 20 ms, `SERVER_TICK_LENGTH`) when the client hosts a game (single player, the tutorial, or a listen server). |
| Bot workers | `bot_worker_pool.c` | Bot brains (Lua), started and joined by the server tick. |
| Audio | SDL | The SDL audio mixer. It reads the audio streams that the main thread fills. |
| WBN | `winbolonetthread.c`, `imgui_winbolonet.cpp`, `gamefront.c` | WinBolo.net login, update and prefs sync. Each one hands its result back through its own lock or atomic. |
| DNS | `dns_lookups.c` | Name lookups. |
| Short jobs | `gamefront.c` (`mobileJoinJobStart`), `lobby_clip_gif_gallery.cpp` | Join jobs and GIF decoding. Each job writes only to its own job struct and marks it done with an atomic. |
| Save dialog | the OS file dialog | The save-map dialog callback. The main thread polls the result (`windowSaveMapPoll`). |

`windowGameTimer` does no game work. It sets the atomic `needsGameTick`,
and the main loop runs the tick. The game tick, the client transport and
all drawing are on the main thread.

### Dedicated server (WinBoloDS.exe)

| Thread | What it runs |
|---|---|
| Main | The console, start and shut down. |
| SDL timer, or `wb-asap-tick` | `serverGameTimer`: the server tick, every 20 ms. It takes the threads mutex. |
| `SrvRecv` | `udp_server_recv.c`: reads the UDP socket into a single-producer, single-consumer ring. The tick thread empties the ring each tick. |
| Bot workers | Bot brains. |
| `WbnBalance`, `WbnUpdate` | WinBolo.net work. |

### Web client

The web build is one thread for the game. `main_wasm.c` runs the game
loop with ASYNCIFY and paces it with `wasmFrameWait`. The build uses
`-pthread`, so worker threads such as the bot pool can run.

## Locks

There are two game locks. Both are recursive SDL mutexes.

- **Threads mutex** (`src/server/threads.c`, `threadsWaitForMutex`).
  It guards the server: `ServerSim`, the hosted-server state in
  `gamefront.c` (`spServerSim`, `spFrameQueueSim`) and the server stats.
  The server tick holds it for the whole tick.
- **Client mutex** (`src/gui/sdl3/clientmutex.c`, `clientMutexWaitFor`).
  It guards the on-screen `ClientSim` during the keys half, the game half
  and the display half of a tick, and during the frame render.
  `clientMutexWaitFor` takes the threads mutex first and then the client
  mutex. `clientMutexRelease` lets them go in the opposite order.

Other data that more than one thread uses:

- `needsGameTick` is an SDL atomic. The timer thread sets it. The main
  thread reads it and clears it.
- The server receive ring is single-producer, single-consumer, with
  atomic head and tail.
- Each worker thread (WBN, DNS, jobs) writes only to its own result and
  marks it done with an atomic or its own lock.

### Lock order

Take locks only in this order:

1. The SDL event watch list lock. SDL holds it while it calls an event
   watch. Only the main thread ever runs code under it here (see below).
2. The threads mutex.
3. The client mutex.

`clientMutexWaitFor` keeps steps 2 and 3 in order by its own design. No
code takes the client mutex alone and then the threads mutex.

### Why there is no deadlock

A deadlock needs two threads that each hold one lock and wait for the
other. Here is why that cannot happen.

- The client mutex is always taken together with the threads mutex, and
  the threads mutex is always first. So these two locks cannot cross.
- The new event watch (`windowLiveTickWatch`) runs the game tick while
  SDL holds the watch list lock. So the main thread can hold the watch
  list lock and then wait for the threads mutex. A deadlock would need
  another thread to hold the threads mutex and then wait for the watch
  list lock. A thread waits for that lock only when it pushes an SDL
  event, or adds or removes a watch. No game code does this:
  - There is no `SDL_PushEvent` and no window call in `src/server`,
    `src/bolo`, `src/scenario` or `src/client_frontend`.
  - The server tick, the bot workers and `SrvRecv` push no SDL events.
  - SDL itself pushes audio device events from `SDL_UpdateAudio`. That
    runs inside the event pump, on the main thread, with no game lock
    held.
- The watch is added and removed only by the main thread, with no game
  lock held.

**Rule:** no thread may push an SDL event, or add or remove an event
watch, while it holds the threads mutex or the client mutex. Keep this
rule if you add code to the server tick or to a frontend callback.

## Ticks during a window move or resize

### The problem

On Windows, a press on the title bar or the window border starts a modal
loop inside the OS. `SDL_PollEvent` does not return until the user lets
go. The main loop stops, so the client sends no input and no pings.

The server drops a client after `CLIENT_TIMEOUT_TICKS` (1000 server
ticks, 20 s) with no packet from it. So a hold of 20 s or more drops the
player.

In a hosted game the server tick still runs on the timer thread, but the
client tick stops. The tank does not move, and the client falls behind.

### The fix

SDL 3 already helps here. During the modal loop, SDL sets a Windows
timer. On each timer message it sends `SDL_EVENT_WINDOW_EXPOSED` with
`data1 = 1` (a "live resize" expose) to the event watches. macOS does the
same at 60 Hz. The SDL docs say an event watch may redraw on this event.

`winbolo.c` adds one event watch, `windowLiveTickWatch`. On a live
resize expose, it runs the normal game tick: the same
`windowRunGameTick` and the same wall-clock catch-up as the main loop.
The tick runs on the main thread, so no new thread and no new lock is
needed. Tick length and tick order do not change.

When the OS loop starts (`WM_ENTERSIZEMOVE`), SDL calls
`SDL_ResetKeyboard()`. So all held keys count as released, and the tank
does not obey held keys during the move.

The watch runs the tick only when all of these are true:

- The event is `SDL_EVENT_WINDOW_EXPOSED` with `data1 == 1`.
- `SDL_IsMainThread()` is true.
- The main game loop is inside its own `SDL_PollEvent` call
  (`sdl3ImguiInEventPoll`). It is false while a dialog loop runs, and
  false while an event handler runs. So the tick never runs inside a
  dialog and never runs inside itself.
- `liveTickCs` is set, so a game is running.
- No game tick is in progress (`inGameTick`).
- The game loop is not done (`finishedLoop`), and no game over is held.
- `needsGameTick` is set. The watch clears it, like the main loop does.

Things the tick must not do inside the OS loop:

- **Open a dialog.** A time-limit game over opens a message box from the
  tick (`frontEndGameOver`). The watch turns on a hold
  (`clientFrontHoldGameOver`). The game over is then only noted. The main
  loop shows it after `sdl3ImguiProcessEvents` returns
  (`clientFrontRunHeldGameOver`).
- **Show the lost-connection box.** `windowRunGameTick` gets
  `inOsLoop = true` and returns early on `SERVER_SHUTDOWN`. The main loop
  sees the same state on its next tick and shows the box there.

### Limits

- The window does not redraw during the drag. Only the game tick runs.
  A redraw could be added to the same watch later.
- Voice chat ticks and Steam callbacks pause during the drag, as before.
- The lobby has its own loop (`imgui_lobby.cpp`). It ticks the transport
  once per frame and is not covered. A drag of 20 s or more in the lobby
  can still drop the player. The same watch idea can cover it later.
- Other modal loops that do not send live resize exposes (for example a
  native file dialog) are not covered.
- In-game dialogs that run their own event loop (Settings, the message
  boxes) are not covered. While one is open, a window move still stops
  the client, and a hold of 20 s or more can still drop the player. This
  is the same as before this change.

## The web client

When a tab is hidden, the browser stops `requestAnimationFrame` and slows
page timers to one call per second or less.

PR #474 fixed the network game. `wasmFrameWait` waits for a frame or a
250 ms timeout. When the page is hidden and `Module.wbFastHiddenFrames`
is set, a small blob Worker posts a message every 20 ms, and each message
ends the wait. Worker timers are not slowed like page timers. So the game
loop keeps running and the client keeps sending to the server.
`wasmNetworkGameFrames(true)` turns this on for the game loop of a
network game. The web lobby draws inside the same loop, so it is covered
too. Single player pauses while hidden, by design.

No web code changed for #311. emsdk was not installed on the machine
used for this work, so the web build was not compiled or tested here.

What a fuller design would need, if the Worker tick is not enough:

- Run the transport, or the whole game tick, on a Worker or a pthread.
  This needs `SharedArrayBuffer`, so the page must be served with
  `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp`. The build already uses
  `-pthread`, so the hosting already needs these headers
  (docs/web-hosting.md).
- The socket is an Emscripten sockfs WebSocket that belongs to the main
  browser thread. A Worker cannot use it directly. The Worker would need
  its own WebSocket, or a proxy back to the main thread.
- All `ClientSim` access would need the client mutex, and every frontend
  callback would need a queue to the main thread (see below).

This is a large change, so it was not made.

## Frontend callbacks

These are the callbacks in `src/bolo/public/frontend.h`. The game code
calls them.

"Main" means the main thread. "Server timer" means the SDL timer thread
that runs `hostedServerTimerCb`, in a hosted game on the desktop.

In a hosted desktop game, the local transport applies some server
control events at once, on the server timer thread, to the human's
`ClientSim` (`transport_local.c`). So a few callbacks can run on the
server timer thread. This was true before #311 and did not change.

| Callback | Thread |
|---|---|
| `frontEndUpdateTankStatusBars` | Main |
| `frontEndUpdateBaseStatusBars` | Main |
| `frontEndPlaySound` | Main |
| `frontEndPlaySoundPan` | Main |
| `frontEndDrawMainScreen` | Main |
| `frontEndStatusPillbox` | Main |
| `frontEndStatusTank` | Main |
| `frontEndStatusBase` | Main |
| `frontEndMessages` | Main |
| `frontEndKillsDeaths` | Main |
| `frontEndManStatus` | Main |
| `frontEndManClear` | Main |
| `frontEndGameOver` | Main. Held during a live tick (see above). |
| `frontEndClearPlayer` | Main. Server timer in a hosted desktop game. |
| `frontEndSetPlayer` | Main. Server timer in a hosted desktop game. |
| `frontEndUpdatePlayerPing` | Main |
| `frontEndUpdatePlayerFlags` | Main |
| `frontEndDrawDownload` | Main |
| `frontEndDrawReturningToLobby` | Main |
| `frontEndAudioReturningToLobby` | Main. Server timer in a hosted desktop game. |
| `frontEndSetPlayerCheckState` | Main. Server timer in a hosted desktop game. |
| `frontEndApplyLocalTankPrefs` | Main |
| `frontEndSetActiveClientSim` | Main |
| `frontEndEnableRequestAllyMenu` | Main |
| `frontEndEnableLeaveAllyMenu` | Main |
| `frontEndShowGunsight` | Main |
| `frontEndRedrawAll` | Main. Server timer in a hosted desktop game. |
| `frontEndTutorial` | Main |
| `frontEndTutorialReset` | Main |
| `frontEndShowAllianceRequest` | Main |

The callbacks that draw check `sdl3DrawOnRenderThread()` or
`clientFrontIsActiveUiSim` and return at once when they must not draw.
The `sdl3Imgui*` setters that the server timer thread can reach are plain
stores into static arrays. `sdl3ImguiClearPlayer` also calls
`voiceForgetPlayer`.

The live tick added for #311 runs on the main thread. So it does not move
any callback to another thread.

## Known issues and follow-ups

These existed before #311. They are listed here so that later work can
fix them.

- **Callbacks on the server timer thread.** The callbacks marked above
  write UI arrays that the main thread reads with no lock. The server
  timer thread holds the threads mutex, but the main thread reads these
  arrays outside it. A fix: put these callbacks into a queue, and let the
  main thread run the queue once per frame.
- **Timer reads of `humanSim`.** `windowGameTimer` and
  `windowFrameRateTimer` call `clientSimHasTransport(humanSim)` on the
  timer thread with no lock. The read is of one pointer and is harmless
  in practice. A fix: keep an atomic flag that the main thread sets.
- **Lobby drag.** See "Limits" above.

## Shutdown order

The watch is added once, before the outer game loop in `winbolo.c`. It
stays in place between games. Between games `liveTickCs` is NULL, so the
watch does nothing in the menu or the lobby.

At the end of each game:

1. The main loop ends. `liveTickCs` is NULL outside the main loop's
   `sdl3ImguiProcessEvents` call, so no live tick can run after it.
2. `finishedLoop` is set. The game tick timers are removed.
3. The game is torn down (`gameFrontEnd`), and the next game starts.

When the player quits:

1. `SDL_RemoveEventWatch(windowLiveTickWatch, NULL)` removes the watch.
   It runs on the main thread with no game lock held.
2. `gameFrontShutdownServer` stops the hosted-server timer and frees the
   server.
3. The client mutex is destroyed, then SDL quits.

SDL removes a watch under the watch list lock. So after step 1 returns,
the watch cannot be running and cannot start again.
