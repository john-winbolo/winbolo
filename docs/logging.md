# Logging (`wb_log`)

WinBolo's structured logging layer. Wraps SDL3's logging facility with
project-specific categories, an optional persistent log file, and
compile-time level gating so release builds can drop verbose calls
entirely.

Source: `src/common/wb_log.h` and `src/common/wb_log.c`.

## TL;DR

```c
#include "common/wb_log.h"

WB_LOG_INFO(WB_LOG_CAT_NET, "client %u joined from %s", id, addr);
WB_LOG_DEBUG(WB_LOG_CAT_SERVER, "tick %u: %d players connected",
             tick, count);
WB_LOG_WARN(WB_LOG_CAT_NET, "ping spike: %ums", ping);
```

## Levels

Five levels, ordered most → least verbose. Pick the level that matches
the *intent* of the message, then filter at runtime or compile-time.

| Macro          | Numeric | Survives Release? | Use for |
|----------------|---------|-------------------|---------|
| `WB_LOG_TRACE` | 1 | No  | Per-packet, per-tick spam. Only useful when chasing a specific bug. Drop after diagnosis. |
| `WB_LOG_DEBUG` | 2 | No  | Handshake details, state transitions, packet decode results, allocation sizes. |
| `WB_LOG_INFO`  | 3 | Yes | Lifecycle: server created, client joined, map loaded, session start/end. |
| `WB_LOG_WARN`  | 4 | Yes | Recoverable problem: malformed packet, timeout-driven disconnect, retry exhausted, queue dropped a packet. |
| `WB_LOG_ERROR` | 5 | Yes | Failure: bind failed, allocation failed, network init failed, file read failed. |

**"Survives Release"** = the call is *not* compiled out at default
`WB_LOG_LEVEL`. Anything a release user might need to diagnose a
problem must be at `INFO` or higher. Reserve TRACE/DEBUG for messages
a developer would crank up via `WINBOLO_LOG=...=trace` while
debugging.

Examples from the codebase:

```c
/* Lifecycle — survives Release */
WB_LOG_INFO(WB_LOG_CAT_NET,
    "server created: port=%u bindAddr=%s maxPlayers=%u",
    port, bindAddr, maxPlayers);

/* Recoverable — survives Release */
WB_LOG_WARN(WB_LOG_CAT_NET,
    "timeout: slot=%d name='%s' diff=%u > timeout=%d -> disconnect",
    i, name, diff, CLIENT_TIMEOUT_TICKS);

/* Hard failure — survives Release */
WB_LOG_ERROR(WB_LOG_CAT_NET, "bind() failed on port %u", port);

/* Per-packet — only when WINBOLO_LOG=net=trace */
WB_LOG_TRACE(WB_LOG_CAT_NET, "recv %s (%u) len=%d from %s:%u",
             packetTypeName(t), t, len, addr, port);
```

There are two more SDL levels (`SDL_LOG_PRIORITY_VERBOSE` between TRACE
and DEBUG, `SDL_LOG_PRIORITY_CRITICAL` above ERROR). They're
recognized by `WINBOLO_LOG=...=verbose|critical` but `wb_log` doesn't
expose dedicated macros — the five above cover all practical needs.

## Categories

Defined in `wb_log.h`. Use the existing list — only add new ones if
you're touching a genuinely new subsystem. The string in the
"Filter name" column is what you use in `WINBOLO_LOG=...`.

| Category               | Filter name | Purpose | Typical files |
|------------------------|-------------|---------|---------------|
| `WB_LOG_CAT_NET`       | `net`       | Transport, sockets, packet ingress/egress, discovery, NAT punch. | `bolo/transport_udp_*.c`, `bolo/discovery.c`, `bolo/nat_portmap.c` |
| `WB_LOG_CAT_SERVER`    | `server`    | `ServerSim` lifecycle, player add/remove, lobby, balance, kicks. | `server/server_sim.c`, `server/server_lifecycle.c`, `server/servermain.c` |
| `WB_LOG_CAT_SIM`       | `sim`       | Game-tick simulation events: tanks, shells, bases, pillboxes, terrain. | `bolo/tank.c`, `bolo/shells.c`, `bolo/bases.c`, `bolo/pillbox.c`, `bolo/game_sim.*` |
| `WB_LOG_CAT_CLIENT`    | `client`    | Client-side game state, scrolling, snapshot apply, interpolation. | `bolo/client_sim.c`, `bolo/client_state.c`, `bolo/interpolation.c`, `bolo/screen.c` |
| `WB_LOG_CAT_GUI`       | `gui`       | ImGui dialogs, menus, settings, lobby, gamebrowser. | `gui/sdl3/sdl3imgui.cpp`, `gui/sdl3/dialogs/*.cpp` |
| `WB_LOG_CAT_AUDIO`     | `audio`     | Sound mixer, sample loading, volume control. | `gui/sdl3/sound.c` (when migrated) |
| `WB_LOG_CAT_ASSET`     | `asset`     | Tile/atlas/font/SVG loading, resource lookup. | `gui/sdl3/tileloader.c`, `gui/sdl3/sdl_bmp.c`, `gui/sdl3/imgui_fonts.h` |
| `WB_LOG_CAT_LUA`       | `lua`       | Lua brain handler, script load/run, AI brain VM. | `gui/sdl3/luabrainshandler.c`, `bolo/braincore.c`, `bolo/brain_*.c` |
| `WB_LOG_CAT_MAP`       | `map`       | Map editor, map gen (random/maze), map load/save, validation. | `mapeditor/*.c`, `bolo/bolo_map.c` |
| `WB_LOG_CAT_LOGVIEWER` | `logviewer` | Standalone log viewer, replay parsing. | `logviewer/*.c`, `logviewer/imgui/*.cpp` |
| `WB_LOG_CAT_PLATFORM`  | `platform`  | Steam, iOS, Android, WASM, sentry, OS-specific glue. | `steam/steam_wrapper.cpp`, `gui/ios/*`, `android/*`, `wasm/*` |

`*` and `all` are aliases that match every category, used in
`WINBOLO_LOG=*=info`.

Pre-existing `SDL_Log("...")` calls that haven't been migrated yet
log to `SDL_LOG_CATEGORY_APPLICATION` and show up under the `APP`
label. Their priority is set with `WINBOLO_LOG=...` only via `*` —
SDL's built-in category names (`audio`, `video`, etc. on the SDL
side) are not exposed by `wb_log`'s parser.

## Adding a new log entry

1. `#include "../common/wb_log.h"` (path is relative to the file).
2. Pick the smallest level that conveys the right urgency.
3. Pick the right category from the table above.
4. Format with `printf` syntax. Include enough state to reason about
   the event without digging into the code.

```c
WB_LOG_DEBUG(WB_LOG_CAT_NET,
    "join request from %s:%u name='%s' wbnToken=%s",
    inet_ntoa(addr.sin_addr), ntohs(addr.sin_port),
    name, hasToken ? "yes" : "no");
```

### Do — include identifying state

Slot/index, player name, addr/port, tick numbers, sizes, codes,
return values. The reader of an old log file can't ask follow-up
questions; the message has to stand alone.

### Don't — put side effects in args

The macro expands to `((void)0)` when stripped at compile time, so
arguments are **not evaluated**. This is the cost of cheap
compile-out — be aware of it.

```c
/* WRONG — counter never increments in Release */
WB_LOG_TRACE(WB_LOG_CAT_NET, "packet #%u", ++packetCount);

/* RIGHT */
packetCount++;
WB_LOG_TRACE(WB_LOG_CAT_NET, "packet #%u", packetCount);
```

### `inet_ntoa` aliasing

`inet_ntoa` returns a pointer to a static buffer. Calling it twice
in the same `printf` argument list shows the **second** address in
both `%s` slots. Use only one per call:

```c
/* WRONG — both addrs print as `to` */
WB_LOG_DEBUG(WB_LOG_CAT_NET, "from=%s to=%s",
             inet_ntoa(from->sin_addr), inet_ntoa(to->sin_addr));

/* RIGHT — copy first, then format */
char fromStr[INET_ADDRSTRLEN];
inet_ntop(AF_INET, &from->sin_addr, fromStr, sizeof fromStr);
WB_LOG_DEBUG(WB_LOG_CAT_NET, "from=%s to=%s",
             fromStr, inet_ntoa(to->sin_addr));
```

## Runtime control

Set per-category priorities via the `WINBOLO_LOG` environment variable
on startup.

**Format:** comma-separated `category=level` pairs. Level is one of
`trace`, `verbose`, `debug`, `info`, `warn`, `error`, `critical`,
`off`. Category is one of the names in the table above (lowercased)
or `*` / `all` to apply to every category. Later entries override
earlier ones, so `*=info,net=trace` sets everything to INFO except
NET, which gets TRACE.

Categories not mentioned in `WINBOLO_LOG` keep SDL's default priority
(typically `INFO` on `APPLICATION`, varies on built-in subsystems).

You can also call `SDL_SetLogPriority(WB_LOG_CAT_NET, SDL_LOG_PRIORITY_TRACE)`
at runtime — useful for an ImGui debug panel toggle.

### Linux / macOS (bash, zsh)

```sh
WINBOLO_LOG=net=trace,server=debug,*=info ./WinBolo
WINBOLO_LOG=*=warn ./WinBoloDS                    # silence everything below WARN
WINBOLO_LOG=net=trace ./WinBoloHeadless           # only crank up NET
```

### Windows — cmd.exe

`set` and the executable must run in the same shell, so chain them
with `&&` or use two separate lines:

```
set WINBOLO_LOG=net=trace,server=debug,*=info
WinBolo.exe
```

Or one line:

```
cmd /c "set WINBOLO_LOG=net=trace,*=info && WinBolo.exe"
```

To make it persistent across new shells (rarely needed):

```
setx WINBOLO_LOG "net=trace,*=info"
```

`setx` only affects shells started **after** it runs — the current
window keeps the old value.

### Windows — PowerShell

```powershell
$env:WINBOLO_LOG = "net=trace,server=debug,*=info"
.\WinBolo.exe
```

Or one line:

```powershell
$env:WINBOLO_LOG="net=trace,*=info"; .\WinBolo.exe
```

### Verifying it took effect

The session banner at the top of the log file shows the resolved log
file path:

```
[2026-04-29 14:33:21.456] [tid 0000000001] [APP     ] [INFO ] wb_log: session start, file=/home/john/.local/share/WinBolo/WinBolo/winbolo.log, compile-level=2
```

If `WINBOLO_LOG` was applied, you'll see TRACE/DEBUG lines from the
specified categories below the banner. If you see only INFO+, the
env var wasn't set in the same shell the executable was launched
from.

## Compile-time control

The `WINBOLO_LOG_LEVEL` CMake option strips calls below the threshold
to `((void)0)`. Use it to drop tracing overhead from shipping builds
or to crank verbosity up for a specific debug build:

```
cmake -DWINBOLO_LOG_LEVEL=TRACE   ...   # nothing stripped
cmake -DWINBOLO_LOG_LEVEL=INFO    ...   # TRACE+DEBUG stripped
cmake -DWINBOLO_LOG_LEVEL=WARN    ...   # TRACE+DEBUG+INFO stripped
cmake -DWINBOLO_LOG_LEVEL=OFF     ...   # all log calls stripped
```

When the option is empty (the default), the header chooses based on
build type: `DEBUG` for Debug builds, `INFO` for Release.

`WINBOLO_LOG_LEVEL` defines `WB_LOG_LEVEL` globally for all targets.
You can also set it per-file: `#define WB_LOG_LEVEL WB_LOG_LEVEL_TRACE`
before including `wb_log.h`.

## Log file

`wb_log_init` writes to a file under SDL's pref path:

| Target            | File name              |
|-------------------|------------------------|
| WinBolo (client)  | `winbolo.log`          |
| WinBoloDS         | `winbolods.log`        |
| WinBoloHeadless   | `winbolo-headless.log` |
| LogViewer         | `logviewer.log`        |

Pref-path location:
- Linux: `~/.local/share/WinBolo/<App>/`
- macOS: `~/Library/Application Support/WinBolo/<App>/`
- Windows: `%APPDATA%\WinBolo\<App>\`

The exact path is logged on startup (`wb_log: session start, file=...`).
Call `wb_log_path()` from code to retrieve it.

### Format

```
[2026-04-29 14:33:21.456] [tid 0000123456] [NET     ] [DEBUG] message text
```

Wall-clock timestamp + monotonic ms, thread ID, category name (8 chars
left-padded), priority, then the formatted message.

### Rotation

On each `wb_log_init`, existing files are rotated:

```
winbolo.log    -> winbolo.log.1
winbolo.log.1  -> winbolo.log.2
winbolo.log.2  -> winbolo.log.3   (oldest, dropped on next init)
```

Only the most recent four sessions are kept. Rotation is best-effort —
failures (e.g. file in use) are silent.

## Existing `SDL_Log` calls

A migration is in progress. Existing `SDL_Log("...")` calls keep
working — they go through `SDL_LOG_CATEGORY_APPLICATION` and end up in
the file alongside structured calls. Treat them as candidates for
migration when you're already touching the file. New code should use
`WB_LOG_*` directly with a category.

## Adding a new category

Avoid this unless you have a real new subsystem. To add one:

1. Append to the enum in `wb_log.h` (before `WB_LOG_CAT__COUNT`).
2. Add a name mapping in `wb_log_category_name()` in `wb_log.c`.
3. Add a string mapping in `wb_log_parse_category()` in `wb_log.c` so
   `WINBOLO_LOG=newcat=debug` works.
4. Add a row to the table at the top of this document.

## Lifecycle

- `wb_log_init(prefOrgName, prefAppName, baseName)` — call once early
  in `main`, after any `SDL_Init`. Idempotent.
- `wb_log_shutdown()` — registered via `atexit` in each entry point.
  Restores the previous SDL sink before closing the file so any late
  teardown logs go somewhere sensible.

The macros are safe to call from any thread; SDL serializes the sink
internally.

## Quick recipe: chasing a network bug

### Linux / macOS

```sh
WINBOLO_LOG=net=trace,server=debug,*=info ./WinBolo
# reproduce
tail -f ~/.local/share/WinBolo/WinBolo/winbolo.log         # Linux
tail -f ~/Library/Application\ Support/WinBolo/WinBolo/winbolo.log  # macOS
```

For server-side issues:

```sh
WINBOLO_LOG=net=trace,server=trace ./WinBoloDS
# reproduce
less ~/.local/share/WinBolo/WinBoloDS/winbolods.log
```

### Windows — cmd.exe

```
set WINBOLO_LOG=net=trace,server=debug,*=info
WinBolo.exe

REM in another window, follow the file:
powershell Get-Content -Path "%APPDATA%\WinBolo\WinBolo\winbolo.log" -Wait -Tail 50
```

### Windows — PowerShell

```powershell
$env:WINBOLO_LOG = "net=trace,server=debug,*=info"
.\WinBolo.exe

# in another window, follow the file:
Get-Content -Path "$env:APPDATA\WinBolo\WinBolo\winbolo.log" -Wait -Tail 50
```

For `WinBoloDS` on Windows replace `WinBolo\WinBolo` with
`WinBolo\WinBoloDS` and the `.exe` with `WinBoloDS.exe`.
