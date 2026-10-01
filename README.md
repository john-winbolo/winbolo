<p align="center">
  <img src="data/smalllogo-transparent.png" alt="WinBolo logo" width="200">
</p>

# WinBolo

WinBolo is a networked multiplayer tank game based on Bolo, the Macintosh game
by Stuart Cheshire. Players drive tanks around an island, capture refuelling
bases and pillboxes, build roads, walls and boats with their man, and form
alliances with other players. A game can have up to 16 players over the
internet or a LAN, with bots filling empty seats.

This repository holds WinBolo 2: the game client, the dedicated server, the map
editor, the replay viewer, the bots, and the tools and tests around them.

- Website: <https://www.winbolo.com/>
- Player accounts, game logs and forums: <https://www.winbolo.net/>

## Platforms

| Platform | Status | Minimum |
|---|---|---|
| Windows | Supported | Windows 10 or 11, 64-bit, x86-64 (ARM64 runs under emulation) |
| macOS | Supported | macOS 11 Big Sur; one universal build runs natively on Intel and Apple Silicon |
| Linux and SteamOS | Supported | Ubuntu 22.04, Debian 12, Fedora 38, Arch or SteamOS 3; x86-64; X11 or Wayland; PulseAudio, PipeWire or ALSA; system libcurl |
| Web browser | Beta | |
| iOS | In development | |
| Android | In development | |

The Windows, macOS and Linux versions are available on Steam. They need 1 GB
of memory, 200 MB of disk space and a broadband internet connection; on Linux
the graphics driver must support OpenGL 3.0. These are the requirements for
the builds we publish. Building from source may work on other systems, such as
Linux on ARM64, but those are not tested.

Every platform runs the same game and uses the same network protocol, so
players on any of them can join the same game. The web version reaches game
servers through a WebSocket proxy, and cannot host a game itself.

## What's in it

| Program | What it does |
|---|---|
| `WinBolo` | The game client (Windows, macOS, Linux; also iOS, Android and a WebAssembly build) |
| `WinBoloDS` | The dedicated server |
| `MapEditor` | Creates and edits `.map` files and the scenario scripts packed with them |
| `LogViewer` | Plays back recorded games (`.wbv` files) |
| `WinBoloHeadless` | A client with no window or sound that runs a Lua bot brain; used by the test suite |
| `BrainTest` | A development viewer for watching what a bot brain is doing |
| `winbolo_gym` | A shared library for training machine-learning bots |

Features added in WinBolo 2 include:

- Bots written in Lua (the GoalHunter brains under `brains/`), and optional
  ONNX models for machine-learned bots
- Built-in voice chat between allies, with push to talk or open mic
- Skins that replace the game's artwork and sounds, including WinBolo 1.x
  `.wsf` skins
- Scenarios: Lua scripts packed into a map that change the rules of a round
- Steam integration: Workshop sharing for skins and scenarios, Steam
  Input controller support, rich presence and achievements
- Translations of the user interface (`data/lang/`)

## Building

WinBolo builds with CMake. Most dependencies (SDL3, Dear ImGui, Lua, zlib,
cJSON and others) are downloaded and built automatically, so on most systems
this is enough:

```bash
cmake -B build -S .
cmake --build build
```

The build needs CMake 3.28 or newer and a C99 / C++17 compiler. Linux needs a
few system packages first, and iOS, Android and WebAssembly each have their own
steps. See [docs/BUILDING.md](docs/BUILDING.md) for every platform, the list of
build targets, the optional features (ONNX Runtime, Steam, Sentry), and
signing and packaging.

The Steamworks SDK is not included in this repository. Without it the build
still succeeds and the Steam features report themselves as unavailable.

## Running a server

```bash
./build/WinBoloDS -inbuilt -port 27500 -gametype open
```

`-inbuilt` uses the built-in map, Everard Island; `-map <file>` loads a map
file instead. Running `WinBoloDS` without the required arguments prints the
full list of options. A client can also host a game itself
from the in-game menu, without a separate server.

## Tests

The unit tests and the baseline tests (scripted games played by headless bots,
compared against recorded output) run through CTest:

```bash
cmake --build build --target WinBoloUnitTests WinBoloHeadless WinBoloDS
ctest --test-dir build
```

The unit tests need `WinBoloHeadless` and `WinBoloDS` built as well, which is
why they are in the build command above. See [tests/README.md](tests/README.md)
for the headless client and the test harness, and
[docs/BUILDING.md](docs/BUILDING.md#build-and-run-baseline-tests) for adding a
baseline scenario.

## Documentation

| Document | Contents |
|---|---|
| [docs/BUILDING.md](docs/BUILDING.md) | Building, packaging and releasing on every platform |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the source is split into layers, and which directories may include which headers |
| [docs/TOOLS.md](docs/TOOLS.md) | Developer tools that are not part of a normal build |
| [docs/SCENARIO_API.md](docs/SCENARIO_API.md) | Writing scenario scripts |
| [docs/SKINS.md](docs/SKINS.md) | Making a skin |
| [docs/replay-format.md](docs/replay-format.md) | The `.wbv` replay file format |
| [docs/logging.md](docs/logging.md) | Logging |
| [tests/README.md](tests/README.md) | The headless client and test infrastructure |

## Repository layout

| Path | Contents |
|---|---|
| `src/bolo/` | The game simulation and network protocol, shared by every program |
| `src/server/` | The server: hosting a game, and the dedicated server |
| `src/gui/` | The desktop client (SDL3 and Dear ImGui) |
| `src/client_frontend/` | Client code shared by the desktop, mobile and web builds |
| `src/gui/ios/`, `src/android/`, `src/wasm/` | Platform-specific clients (`android/` holds the Gradle project) |
| `src/mapeditor/`, `src/logviewer/`, `src/headless/`, `src/braintest/`, `src/gym/` | The other programs listed above |
| `src/scenario/`, `src/scenario_io/` | Running scenario scripts, and reading and writing scenario files |
| `src/winbolonet/` | The client for the winbolo.net tracker and player accounts |
| `src/steam/` | Steam integration and the stub used when the SDK is absent |
| `brains/` | Bot brains |
| `data/` | Artwork, sounds, fonts, maps, translations and controller layouts |
| `tests/` | Unit tests, baseline scenarios, fuzz targets and fixtures |
| `tools/`, `scripts/` | Development scripts |
| `cmake/` | CMake helper modules |

## Contributing

Fixes, features, maps, translations and documentation changes are welcome.
See [CONTRIBUTING.md](CONTRIBUTING.md) for how to send a pull request.

## Licence

WinBolo is released under the GNU General Public License, version 3 or (at
your option) any later version. See
[LICENSE](LICENSE).

The game's graphics and sounds are not covered by the GPL. They are Stuart
Cheshire's designs from Bolo, copyright 1987-1995 Stuart Cheshire, and are used
with his permission. The files are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md#bolo-graphics-and-sounds).
The Everard Island and Inbuilt Tutorial maps are also Stuart Cheshire's, on the
same terms, and are listed in the same section. The other maps in `data/maps/` are Bolo community maps, each its
author's own work, and are not covered by the GPL either; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md#bolo-community-maps).

Steam builds also link the Steamworks SDK, which is owned by Valve, is not
open source, and is not included in this repository. The copyright holders
give additional permissions in [LICENSE-EXCEPTION.md](LICENSE-EXCEPTION.md)
for linking with the Steamworks SDK and other platform SDKs, and for releasing
WinBolo through app stores and on locked devices such as phones and consoles,
on the condition that WinBolo's source code stays available under the GPL.
WinBolo builds and runs without any of these SDKs.

The third-party libraries it uses, and their licences, are
listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The people who have
worked on WinBolo are listed in [AUTHORS.md](AUTHORS.md).
