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

### Brain capacity

A brain may also give `brain.capacity(machine, measured)`: how many bots of
this brain the machine can run at full strength. The engine calls it once,
when the brain is first loaded, in a protected call, before `brain.open`.
The answer is kept by brain name (the brain's directory) and `brain.version`
(a string or number field on the table; "" when the brain has none), so
later loads of the same brain and version do not call it again.

```lua
function brain.capacity(machine, measured)
  -- machine.cpu_name        "AMD Ryzen 5 5600 6-Core Processor", or ""
  -- machine.logical_cores   12
  -- machine.physical_cores  6
  -- machine.speed_1t        CPU speed test, 1 thread (iterations/s); 0 = not measured
  -- machine.speed_all       CPU speed test, all cores at once; 0 = not measured
  -- machine.scaling         speed_all / speed_1t; 0 = not measured
  -- machine.worker_threads  the bot worker pool's threads; 0 when it is not running
  -- machine.on_battery      true on battery; false when not, or not known
  -- machine.version         the WinBolo version, "2.2.1"
  --
  -- measured is nil for now. Its shape, when the engine measures think
  -- times: a table keyed by bot count N, each row
  --   { think_ms_p50 = 1.2, think_ms_p95 = 3.4,
  --     below_top_pct = 0.5, overrun_pct = 0.1 }
  -- (think time median and 95th percentile in ms; the % of thinks that ran
  -- below the brain's top level, and the % that ran over their budget).
  return 5
end
```

- No `capacity` function, an error, or an answer that is not a number
  (nil included): the default, logical cores - 1, at least 1.
- A number is rounded down and clamped to 0..16.
- The answer is advice. It does not change how many bots start. The lobby
  turns Add Bot off once the bots that run on this machine (single player,
  or a game this client hosts) reach the answer for the brain it would add,
  and while the lobby's CPU speed test runs. In a game on a dedicated
  server, the server's own number (its host bot capacity, below) is the
  cap, and the tooltip names the server. A server that sends no number
  gives no cap.
- The CPU speed test (`WinBoloDS -cputest`, or the client's first game
  lobby for each new version) gives `speed_1t` and `speed_all`. It is
  stored per machine and version in `cpu_speed.json` with the other
  per-user files; the `WINBOLO_CPU_SPEED_FILE` environment variable puts it
  somewhere else. Until a result is stored, both are 0.
- `WinBoloDS -cputest` prints each brain's answer after the test.
  `WinBoloHeadless --cputest` runs and stores the same test, without the
  answers.
- In the client, the test runs in the background (low priority threads)
  when a game lobby first opens after each new version. If the game starts
  or the player leaves first, it stops and stores nothing, and the next
  lobby runs it again. It writes a start line and a result line to the log
  (the `client` category of `WINBOLO_LOG`).
- A brain written in C gives the same answer through
  `brainCapacityRegisterC(name, fn)` with
  `int fn(const BrainMachineInfo *, const BrainMeasured *)`
  (src/bolo/public/brain_capacity.h); a negative return means "use the
  default". It is asked only when the Lua table has no `capacity`.
- `BotPoolStats.brainCapacity` (`serverSimGetBotPoolStats`) is the smallest
  answer among the running bots' brains, or -1.

#### The dedicated server's host bot capacity

WinBoloDS works out how many bots it can run in its own process: its host
bot capacity.

- The first time its lobby opens for each new version, it runs the same
  2 s CPU speed test on low priority threads. The tick and the network do
  not wait for it. A finished test is stored (context `"server"`), and
  later starts read the stored result with no new test. If the game starts
  first, the test stops, stores nothing, and the next lobby tries again.
- It then asks its bot brain (`-brain`, or the default) for its
  `capacity` answer, on a worker thread. A brain that does not load gives
  the default answer.
- `-botcapacity N` (0..16; 0 = no bots in this process) sets the number
  and runs no test. A number above 16 gives 16, with a note. Anything that
  is not a whole number is an error. It also locks the lobby's Server bot
  slots at N: the row is read-only for every player (mouse-over "Set by
  -botcapacity on the server"), and the server refuses a change with
  `CMD_REJECT_LOCKED`.
- The lobby's Server bot slots (Server Settings, 0..16, `LST_SERVER_BOT_SLOTS`)
  sets how many bots the server runs in its own process: its slots in the
  bot placement and its part of the Add Bot limit. The default is the
  number above (the recommended value). A higher value is taken; the lobby
  shows it greyer with a warning. Players who may change the other
  settings may change it, in the lobby and while a game runs. During a
  game a lower value moves only bots that may move, and none with
  `-bothost-nomove`. `CTRL_SERVER_BOT_SLOTS` carries it to clients
  (`clientSimGetServerBotSlots`); an older peer skips it.
- One log line on stderr gives the result:
  `Bot capacity: 3 bots of GoalHunter (CPU speed test, stored result)`,
  `... measured now`, or `Bot capacity: override 3 (-botcapacity)`.
- A lobby Add Bot request past the number is refused with
  `CMD_REJECT_BOT_LIMIT`, as `-maxbots` does. While the test runs, every
  Add Bot request is refused. `-bots N` at start-up, scenario bots and
  sidecar bots are not capped. The console has no add-bot command.
- The number goes to clients as the last byte of the fixed part of the
  lobby settings (`lobbyHostBotCapacity`: 0..16, 0xFF = no cap,
  0xFE = the test runs). A body without the byte reads as no cap.
  `clientSimGetLobbyHostBotCapacity` gives it (-1 no cap, -2 the test
  runs).
- In C: `serverSimGetHostBotCapacity` / `serverSimSetHostBotCapacity`
  (-1 no cap, -2 the test runs, else 0..16), and
  `BotPoolStats.hostBotCapacity`. Only WinBoloDS sets it. Every other sim
  host (single player, a game a client hosts, Headless) leaves it at -1.
- `tests/ds_bot_capacity_test.py [build_dir]` runs WinBoloDS three times
  with a scratch `WINBOLO_CPU_SPEED_FILE`: with `-botcapacity 3`, with a
  fresh store (the test runs and stores), and with the stored result.

GoalHunter's answer (brains/GoalHunter/capacity.lua) is
`floor(physical_cores * (speed_1t / C.CAPACITY_REF_1T) * C.CAPACITY_K)`,
clamped to 1..16, with `CAPACITY_REF_1T = 25000` and `CAPACITY_K = 1.0` in
constants.lua. With no speed test result it returns nil (the default).

#### Where the dedicated server places its bots

By default a bot stays where it runs well (`botHostPlaceRebalanceKeep` in
bot_host_link.c). A layout that is lopsided but good stays as it is:

- A place is good while its window overrun rate is below P% (or not known
  yet) and its free CPU passes the free CPU check for the bots it has (or
  is not known yet). In the lobby every place counts as good.
- New bots start on the server and stay there while the server is good
  and has slots. Only the bots it cannot take go to Bot Hosts.
- A bot moves only when its place must give it:
  - over its slots (forced): a host with more bots than its slots gives
    one; the server itself does so only while a Bot Host has room;
  - bad: its overrun rate is P% or more, or its free CPU is too low. It
    gives one bot to a good place, then waits before the next, since its
    numbers need time to show the move. For a bad rate the wait is
    T / (rate / P) seconds, never below S (see the overrun move below);
    after the bot leaves, the rate is measured from the thinks since then.
    For low free CPU the wait is T seconds.
  - free CPU batch (`-bothost-free-batch 1`, the default): when the free
    CPU numbers show the place is short by n bots (each bot that leaves
    adds one bot's cost to its free CPU average, until the free CPU check
    passes for the bots left), it gives up to n bots in one step. Each goes
    by the rule below (the server first, then the best good host); a place
    takes at most one bot of a batch, and the batch stops when no good
    place is left. The place's limit then drops to its bots less the bots
    given, for this game. A batch needs free CPU numbers that show the
    last bot that left (no bot left within T seconds). A bad reading with
    no clean reading at all cannot count the bots: one bot. 0 gives one
    bot a step, as before.
- The place a bot goes to: the server first, while it is good and below
  its slots; else the Bot Host with the best score below.
- Forced fall-backs (a host gone, changed brain files) and the overrun
  move work as before.
- No quick way back: a bot that left a place by a placement decision (a
  place, a spread move or an overrun move) does not go back there for Y
  seconds (`-bothost-overrun-stay-secs`). Of the bots that may move, the
  first that did not just come from the target goes. A forced move takes
  that bot only when no other one can go. The overrun move keeps this rule
  too, except by its emergency rule.
- The DECISION records and the console give the reason: "over its slots",
  "overrun rate", "low free CPU", "low free CPU, batch" (a bot of a free
  CPU batch, and the cut that follows it), or "the spread" (the even
  spread). The console says the severity (rate / P) and the wait for a
  bad rate, and the shortage and the bots given for a batch.
- `-bothost-spread-even` brings back the old even spread: bots also move
  between good places so that each is about equally full, with the lag
  rules below. The stay rule still holds.
- Unit tests: `bot_host_place_keep_*` (`--test <name>`), among them
  `bot_host_place_keep_scaled_wait`, `bot_host_place_keep_step_min_old`,
  `bot_host_place_keep_free_batch`, `bot_host_place_keep_batch_no_target`
  and `bot_host_place_keep_batch_off`.

#### The even spread and lag (-bothost-spread-even)

With `-bothost-spread-even` WinBoloDS spreads its bots over its own process
and the Bot Hosts so that each is about equally full (bot_host_link.c). The
lag to each host also counts:

- A host's score is (bots + 1) / effective slots x (1 + lag / R). A new
  bot goes to the lowest score. The lag is the server's round trip to the
  host's player (a player's own host) or to the host, smoothed over about
  10 s. The server's own process has lag 0. A lag not known counts R.
- A host with more lag than L takes no new bots while a host with lag L
  or less has room.
- A bot moves to another place only when its score there is at least 33%
  lower than its score now, so small changes in lag move no bots.
- `-bothost-lag-ref-ms <ms>` sets R (default 100). 0 turns all the lag
  rules off: the placement is then the one without lag.
- `-bothost-lag-max-ms <ms>` sets L (default 250). 0: no ceiling.
- The unit test `bot_host_place_lag` prints the spread with and without
  lag for hosts of 8, 4, 2 and 1 slots.

#### Bot overruns on a host

The server also watches how often each host's bots overrun
(`botHostOverrunProbe` in bot_host_link.c). The server's own process counts
as a host too.

- Once a second each host is probed: its bots' new overruns and new thinks.
  The host's rate is the sum of its overruns over the sum of its thinks in
  the last W seconds. The rate is known only when the host had bots in each
  second of a full window. A host with no bots has no rate.
- When a host's rate is over P%, one bot may move to a better host. For a
  bot with m overrun moves in this game, a normal move needs all of these:
  the target's rate is below P% (or the target has no bots: a trial); the
  target's rate is at most the source rate x G^(m+1); the bot stayed at
  least Y x F^(m-1) seconds since its last overrun move (no stay rule
  while m = 0); and the target passes its free CPU check and has room.
- At a rate of E% or more the emergency rule applies: the target must be
  below P%, or at most half the source rate, or have no bots. The gain and
  stay rules do not apply. The free CPU check and room still do.
- The target is the one with the lowest rate, then the lowest lag, then the
  most slots. A target with no bots comes after every target with a rate.
  Lag only breaks ties: the lag ceiling does not stop an overrun move.
- A move adds 1 to m. A forced fall-back (a Bot Host left or failed) does
  not. m starts again at 0 in the lobby.
- After a move the source host's limit becomes its bot count minus 1 for
  this game. A bot with a pill or with its builder out does not move. The
  probes start again after each move.
- The wait between two moves off one host scales with how far its rate is
  over P: the severity is rate / P, and the wait is T / severity, never
  below S and never above T. With T = 30, S = 5 and P = 0.5: 0.5% waits
  30 s, 1% 15 s, 2.5% 6 s, 3% or more 5 s. After a move (or a raise) the
  rate is known from the probes since that change once there are S of
  them with at least 250 thinks (5 s of one bot), so a short wait judges
  on a short window. At a game start the first rate still needs a full
  window. S at or above T gives the old fixed step of T seconds and a full
  window after each move. A raise keeps its step of T seconds.
- When every other host is about as bad, no bot moves. The console says
  "no better host" at most once each T seconds per host.
- The server stays the last fall-back. A lowered server limit only moves
  bots off when another host has room.
- Each Bot Host reads the free CPU of its whole machine every 5 s and
  sends only the percent (CTRL_BOT_HOST_CPU_FREE). The server reads its
  own. The server keeps H seconds of readings per host: free %, bot count,
  and clean or bad (bad: the overrun rate of those seconds is over P%).
- A raise by 1 (at most each T seconds) needs all of these: the rate is
  not over P%; one bot's cost (the host's bot CPU / its bots) is known; a clean
  reading had free CPU at or below the predicted free CPU (the 30 s free
  average minus one bot's cost); and the predicted free CPU is above the
  free CPU of every bad reading. With no free CPU data there is no raise.
- Each new bot on a host also needs free CPU room: a host may take its
  first bot; after that a new reading must count the newest bot before the
  next one goes there. With no free CPU data only the slots count.
- A lowered limit is for one game: a new game, the next round or a rejoin
  clears it. The lobby's Add Bot limit does not change.
- `-bothost-overrun-pct <f>` sets P (default 0.5; 0 turns the check
  off).
- `-bothost-overrun-emergency-pct <f>` sets E (default 5; 0: no emergency
  rule).
- `-bothost-overrun-gain <f>` sets G (default 0.5).
- `-bothost-overrun-stay-secs <s>` sets Y (default 30).
- `-bothost-overrun-stay-factor <f>` sets F (default 2).
- `-bothost-overrun-window-secs <s>` sets W (default 30).
- `-bothost-overrun-step-secs <s>` sets T (default 30).
- `-bothost-overrun-step-min-secs <s>` sets S (default 5; 30, or T, gives
  the old fixed step).
- `-bothost-free-batch <0|1>` turns the free CPU batch on or off (default
  1; 0 gives one bot a step).
- The values before these changes: `-bothost-overrun-pct 1
  -bothost-overrun-step-min-secs 30 -bothost-free-batch 0`.
- `-bothost-free-sample-secs <s>` sets how often a reading is kept
  (default 5).
- `-bothost-free-history-secs <s>` sets H (default 300).
- `-record-bothost-data` writes each probe, each reading and each decision
  with its numbers into the round log (.wbv) as log_BotHostData records.
  Off by default. A game hosted from the client uses the "Record Bot Host
  Data" setting in the [SETTINGS] section of the preferences file. The Log
  Viewer shows them in its Bot Hosts window.
- The console shows each step with the host, the overrun rate, the new
  limit and the free CPU numbers.
- Unit tests: `bot_host_overrun_*` (`--test <name>`).

#### No bot moves during a game (-bothost-nomove)

For brain-debug games: each bot stays on one machine for the whole game.

- At start the DS prints "Bot moves during the game are off
  (-bothost-nomove)".
- In the lobby the placement works as without the flag: the spread, the
  free CPU check, listed Bot Hosts (-bothosts) and the players' hosts.
- While the game runs, a bot is placed once. A bot there at the game start,
  or one added during the game, may go from the server to a host only in
  its first 10 s (BOT_HOST_START_PLACE_TICKS), and only when it was not on
  a host before in this game. No bot moves after that: no spread move, no
  lag move and no overrun move.
- A fall-back still happens. A bot whose host leaves, times out, loses the
  bot's connection, changes its brain files or offers fewer slots (0 for
  the lobby's opt-out) goes back to the server and stays there.
- The probes and the free CPU readings still run, and -record-bothost-data
  still records them. When the overrun check would move a bot, the console
  says "would move (moves off)" with the numbers, and the log gets a
  decision 7 (would move) record. The Log Viewer shows it as a marker.
- The next lobby (or the next round) places normally again.
- Unit tests: `bot_host_nomove_*` (`--test <name>`).

#### Game bot limit and Bot Hosts on one computer

The lobby's Add Bot limit is the game bot limit: the server's bot slots
plus the slots of each Bot Host that runs bots. A host that opted out, a
host that still runs its CPU test, and a host on the same computer as the
server or as another counted host count 0. -bots, scenario, sidecar and
headless bots are not limited by it.

- The DS finds hosts on one computer in two ways:
  - The address: 127.0.0.1, or one of the DS's own interface addresses. A
    shared public address is never proof.
  - The machine token: each computer has a random 32-byte id file
    (Windows `C:\ProgramData\WinBolo\machine_id`, macOS
    `/Users/Shared/WinBolo`, Linux `/var/lib/winbolo` when writable, else
    the user data folder; `WINBOLO_MACHINE_ID_DIR` sets another folder).
    For each lobby, each game and each host join, the DS sends a new
    16-byte salt (`CTRL_BOT_HOST_MACHINE_SALT`). Each host answers
    SHA-256(id || salt) (`CTRL_BOT_HOST_MACHINE_ANSWER`). The id never
    leaves the computer. A host answers each salt once only (it keeps the
    last 64 in `machine_salts`). No answer in 5 s counts as another
    computer.
- A player's own host on the server's computer gets 0 slots. A listed host
  there keeps its slots. Of hosts on one other computer, the first-joined
  host that offers slots keeps them, and the others get 0. When it leaves,
  the next one takes over.
- A host that joins gets no bot until its group is known: at once when its
  address or token shows the server's computer, else when all hosts answer
  or the 5 s wait ends (`bot_host_machine_join_wait`).
- The lobby row says "<name>: same computer as the server (0 slots)" or
  "<name>: same computer as <other> (0 slots)" (`CTRL_BOT_HOST_ROW_SLOTS`).
- The DS console prints one line per group, with each member and how it was
  found, and a "game bot limit:" line when the limit changes.
- To test: run a DS and two headless clients (each has its own Bot Host) on
  one computer, with one `WINBOLO_MACHINE_ID_DIR` for all. The console
  shows both player hosts in the server group and the limit equal to the
  server's bot slots.

#### Bot Host slot override (-bothost-slots)

Each Bot Host row in the lobby (not the server row) has a "Slots" box: "All
(N)", or 0 up to the N slots the host offers now. It only lowers. If the
host later offers fewer slots, the lower number is used.

- The players who may change Server Settings may change it. Others see it
  read-only. It goes in the lobby setting `LST_BOT_HOST_SLOTS`
  ([row][value or 0xFF]).
- It lasts for this lobby: it goes back to "All" for the next lobby, and
  when the host leaves and joins again.
- `-bothost-slots <name>=N` on the DS (once per host; several flags are
  allowed) sets it for the host with that listed name or player name. It
  locks the row: read-only, with the mouse-over "Set by -bothost-slots on
  the server", and the DS refuses a change. Bad input prints
  "-bothost-slots: <reason>; ignored".
- The placement, the free CPU check, the overrun check and the game bot
  limit use the lower number. During a game a lower number moves only bots
  that may move, and none with -bothost-nomove.
- The Bot Host itself already has `--slots N` (standalone), and the
  headless client has `--bothost-slots N`.
- Unit tests: `bot_host_machine_*`, `machine_id_*`,
  `bot_host_game_bot_limit`, `bot_host_slots_*` (`--test <name>`).

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
