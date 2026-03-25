# WinBolo Headless Client & Test Infrastructure

## Overview

The headless client (`WinBoloHeadless`) is a non-graphical WinBolo client that connects to a `WinBoloDS` server, runs a Lua brain script, and optionally logs game state to a JSON file. It has no window, no renderer, no ImGui, and no sound -- it runs the full game engine and network stack but skips all display and audio.

This is the foundation for automated testing: start a server, connect one or more headless clients with scripted brains, run for N ticks, then check the results.

## Building

```bash
# From the project root
cmake -B build-linux
cmake --build build-linux --target WinBoloDS WinBoloHeadless
```

Both binaries land in `build-linux/`.

## WinBoloHeadless Usage

```
WinBoloHeadless --server HOST --port PORT [options]
```

### Required Arguments

| Argument | Description |
|----------|-------------|
| `--server HOST` | Server IP or hostname to connect to |
| `--port PORT` | Server UDP port |

### Optional Arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `--name NAME` | `HeadlessBot` | Player name shown in-game |
| `--brain PATH` | *(none)* | Path to a Lua brain script |
| `--ticks N` | `0` (unlimited) | Exit after N game ticks |
| `--log-state FILE` | *(none)* | Write per-tick JSON state to FILE (`-` for stdout) |
| `--password PASS` | *(none)* | Server password if required |
| `--quiet` | off | Suppress informational output on stderr |
| `--help` | | Show usage and exit |

### Examples

Connect to a local server, run an idle brain for 500 ticks:
```bash
./WinBoloHeadless --server 127.0.0.1 --port 27500 \
  --brain tests/brains/idle.lua --ticks 500 --name TestBot
```

Log per-tick state to a file:
```bash
./WinBoloHeadless --server 127.0.0.1 --port 27500 \
  --brain tests/brains/drive_forward.lua --ticks 200 \
  --log-state output.jsonl --quiet
```

### State Log Format

When `--log-state` is used, one JSON object is written per game tick (JSONL format):

```json
{"tick":1,"x":31488,"y":38400,"net_status":2}
{"tick":2,"x":31488,"y":38400,"net_status":2}
...
```

| Field | Description |
|-------|-------------|
| `tick` | Game tick number (starts at 1) |
| `x` | Tank world X coordinate |
| `y` | Tank world Y coordinate |
| `net_status` | Network status enum (2 = running) |

### Exit Behaviour

The client exits cleanly (return code 0) when:
- The tick limit is reached (`--ticks N`)
- SIGINT or SIGTERM is received (Ctrl+C)

It exits with a non-zero code on:
- Connection failure
- Network disconnection during play

### Lua Brain Scripts

Brain scripts follow the same API as the full GUI client's brain system. A brain is a Lua module returning a table with three functions:

```lua
local brain = {}

function brain.open(info)
  -- Called once when the brain starts.
  -- info.player_number, info.tankx, info.tanky, info.num_players, etc.
end

function brain.think(info)
  -- Called every game tick.
  -- Return a table of key commands:
  return {
    holdkeys = KEY_FASTER,  -- keys to hold this tick (bitfield)
    tapkeys = 0,            -- keys to tap this tick (bitfield)
  }
end

function brain.close(info)
  -- Called once when the brain shuts down.
end

return brain
```

Available key constants: `KEY_FASTER`, `KEY_SLOWER`, `KEY_TURNLEFT`, `KEY_TURNRIGHT`, `KEY_SHOOT`, `KEY_LAYMINE`.

Available object type constants (for `info.objects`): `OBJECT_TANK`, `OBJECT_PILLBOX`, `OBJECT_BASE`, etc.

## Test Runner

The test runner (`tests/run_test.py`) automates the full cycle: start a server, connect headless clients with brain scripts, wait for completion, and validate results.

### Usage

```bash
python3 tests/run_test.py [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--build-dir DIR` | `build-linux` | Directory containing WinBoloDS and WinBoloHeadless |
| `--port PORT` | `27600` | Server port (use a non-default port to avoid conflicts) |
| `--ticks N` | `200` | Game ticks per test |
| `--test NAME` | `all` | Run a specific test (see below) |
| `--verbose` / `-v` | off | Show server and client output |
| `--map FILE` | built-in | Map file for the server |

### Running Tests

Run all tests:
```bash
python3 tests/run_test.py
```

Run a single test:
```bash
python3 tests/run_test.py --test connect
python3 tests/run_test.py --test 0.2
```

Verbose mode (shows all server/client output, useful for debugging):
```bash
python3 tests/run_test.py --test movement -v
```

### Test Scenarios

#### Test 0.1 -- connect

A single headless client connects with the `idle.lua` brain (which does nothing) and runs for the specified number of ticks.

**Pass condition:** Client exits with return code 0.

**What it validates:** The headless client can connect to a server, enter the game, run the game loop, and shut down cleanly.

#### Test 0.2 -- movement

A single client connects with `drive_forward.lua`, which holds the accelerate key (`KEY_FASTER`) for the first 100 ticks. State is logged to a JSONL file.

**Pass condition:** Client exits cleanly and a state log with position entries is generated.

**What it validates:** Brain key input works, the tank moves in response to input, and state logging captures position data.

#### Test 0.3 -- two_players

Two clients connect: one with `drive_forward.lua` (Driver) and one with `watch_objects.lua` (Watcher). The watcher checks `info.objects` for `OBJECT_TANK` entries.

**Pass condition:** Both clients exit with return code 0.

**What it validates:** Multiple headless clients can connect to the same server simultaneously and the game runs correctly with multiple players.

#### Test 0.4 -- terrain_agreement

Two idle clients connect with `log_state.lua` and sample the map terrain around their tanks each tick. After the run, their terrain samples at overlapping coordinates are compared tile-by-tile.

**Pass condition:** All terrain tiles that both clients sampled are identical.

**What it validates:** Both clients have the same map state. Catches bugs where clients receive different map data from the server or apply map updates inconsistently.

#### Test 0.5 -- tank_visibility

Client A drives forward (`drive_and_log.lua`), client B sits idle (`log_state.lua`). Both log their full state including visible objects. The test checks that B's `info.objects` contains an `OBJECT_TANK` entry, and cross-checks the observed position against A's self-reported position (within 2 map squares tolerance for network lag).

**Pass condition:** B sees A's tank, and the observed position agrees with A's actual position at least 50% of the time.

**What it validates:** The object visibility system works correctly across clients. Tanks within view range appear in the objects list with accurate positions.

#### Test 0.6 -- position_agreement

Both clients drive forward (`drive_and_log.lua`) and log state. The test checks that each client sees the other's tank, and cross-checks observed positions against self-reported positions in both directions.

**Pass condition:** Both clients see each other, with at least 50% position agreement.

**What it validates:** Mutual consistency of the game state. Each client's view of the other matches what the other reports about itself. This is the core test for server-authoritative correctness.

#### Test 1.1 -- normal_movement

A single client drives forward with `drive_and_check_corrections.lua` and monitors for position jumps larger than 1 map square between consecutive ticks. Such jumps would indicate the server sent a position correction to override the client's predicted position.

**Pass condition:** Zero corrections detected, and the tank actually moved from its start position.

**What it validates:** Normal, legal movement is accepted by the server without interference. The server-side validation (speed check, terrain check, distance check) does not produce false positives for a normally-behaving client.

#### Test 1.2 -- terrain_speed

A single client drives forward while turning with `drive_terrain_speed.lua`, logging the terrain type under the tank and the current speed every tick. The test runner checks that the reported speed never exceeds the maximum speed for the current terrain type (e.g. 16 on road, 12 on grass, 6 in forest, 3 in swamp/crater/rubble, 0 on buildings).

**Pass condition:** Zero ticks where speed exceeds the terrain speed limit (with 1 unit tolerance for float rounding).

**What it validates:** The server enforces terrain-based speed limits. A client cannot report a speed higher than the terrain allows.

#### Test 1.6 -- consistent_validated

Both clients drive forward (`drive_and_log.lua`) and log state. Like test 0.6, but with tighter position tolerance (384 world units / ~1.5 map squares instead of 512) since the server now validates all positions before broadcasting them.

**Pass condition:** Both clients see each other, with at least 60% position agreement within the tighter tolerance.

**What it validates:** Server-validated positions are consistent across clients. Since the server rejects invalid movements, both clients should see positions that have been approved by the server, leading to tighter agreement than Phase 0.

#### Tests not yet automated (require modified client)

Tests 1.3 (wall collision blocked), 1.4 (teleport detected), 1.5 (speed hack detected), and 1.7 (high latency tolerance) from the Phase 1 plan require a modified client that sends fabricated position packets. These cannot be tested with Lua brains alone since brains control key inputs, not raw network data. They would need either a custom test client or a network packet injection tool.

### Output

```
WinBolo Phase 0 Tests
  Build dir: /path/to/build-linux
  Port: 27600
  Ticks: 200

Test 0.1: Connect with idle brain...
  PASS: Client connected and exited cleanly

Test 0.2: Drive forward with position logging...
  Logged 200 ticks
  First: tick=1 pos=(31488, 38400)
  Last:  tick=200 pos=(31488, 37200)
  PASS: Position log generated

Test 0.3: Two players - drive and watch...
  PASS: Both clients connected and ran

Results: 3 passed, 0 failed
```

## Test Brain Scripts

Located in `tests/brains/`:

| Script | Purpose |
|--------|---------|
| `idle.lua` | Does nothing. Validates basic connect/run/disconnect. |
| `drive_forward.lua` | Holds accelerate for 100 ticks, logs position every 25 ticks. Checks on close that the tank actually moved. |
| `watch_objects.lua` | Scans `info.objects` each tick for other tanks. Reports on close whether another tank was ever seen. |
| `log_state.lua` | Logs full game state (position, objects, terrain) as JSON to stdout each tick. Used by validation tests. |
| `drive_and_log.lua` | Combines `drive_forward` behaviour with `log_state` output. Drives forward while logging everything. |
| `drive_and_check_corrections.lua` | Drives forward and detects position jumps (server corrections). Used by test 1.1. |
| `drive_terrain_speed.lua` | Drives forward while turning, logs terrain type and speed every tick. Used by test 1.2. |

## Architecture

```
  WinBoloDS (server)          WinBoloHeadless (client)
  +-----------------+         +---------------------+
  | servercore.c    |  UDP    | headless_main.c     |  game loop, CLI, state log
  | servernet.c     |<------->| network.c           |  full bolo network stack
  | servermain.c    |         | screen.c            |  full game engine
  +-----------------+         | luabrainshandler.c   |  Lua brain execution
                              | headless_frontend.c  |  stub display/sound/UI
                              +---------------------+

  tests/run_test.py
  +----------------------------------+
  | Starts WinBoloDS as subprocess   |
  | Starts 1+ WinBoloHeadless procs  |
  | Waits for clients to finish      |
  | Checks return codes & log files  |
  +----------------------------------+
```

The headless client runs the same game engine (`screen.c`, `tank.c`, `network.c`, etc.) as the full GUI client. The difference is that all display, sound, and UI callbacks are stubbed out in `headless_frontend.c` -- they accept calls from the engine and do nothing. The game loop in `headless_main.c` replicates the tick timing from the GUI client (`winbolo.c`).
