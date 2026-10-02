# Third-Party Notices

WinBolo uses the following third-party libraries, code, artwork and sounds.

WinBolo itself is licensed under the GNU General Public License, version 3 or
later (see [LICENSE](LICENSE)). Every code library below is under a licence
that can be combined with it, except the Steamworks SDK, which is covered by
[LICENSE-EXCEPTION.md](LICENSE-EXCEPTION.md).

## Bolo graphics and sounds

- Designs copyright 1987-1995 Stuart Cheshire. **Not licensed under the GPL.**
- WinBolo's versions of them, including the high-resolution redraws, were made
  by John Morrison from the original Bolo graphics and sounds. Because they
  reproduce Stuart Cheshire's designs, they cannot be licensed without him,
  and the GPL does not apply to them.
- Used with the permission of Stuart Cheshire, for distribution as part of
  WinBolo. This permission does not extend to any other use. If you
  distribute WinBolo or a modified version, these files remain Stuart
  Cheshire's and are not covered by the GPL.
- WinBolo's graphics and sounds have been published with its source code since
  12 December 2008, first on Google Code and winbolo.com, and later on GitHub.
- Files:
  - `data/tile.bmp`, `data/tanks.bmp`, `data/pillbox.bmp`, `data/base.bmp`,
    `data/boats.bmp`, `data/items.bmp` and `data/skin.bmp`
  - every file in `data/svg/` (the high-resolution tiles and sprites)
  - the game sounds in `data/sounds/`: `big_explosion_*`, `bubbles`,
    `farming_tree_*`, `hit_tank_*`, `man_building_*`, `man_dying_*`,
    `man_lay_mine_near`, `mine_explosion_*`, `shooting_*`, `shot_building_*`,
    `shot_tree_*` and `tank_sinking_*`. The lobby and ping sounds are not
    included: the lobby sounds are listed under [Audio](#audio), and the ping
    sounds (`ping_default.wav`, `ping_caution.wav`) were added by Andrew Roth
    as part of WinBolo and are covered by the GPL.
  - Everard Island, Bolo's original built-in map: `data/maps/Everard Island.map`
    and the copy compiled into the program from
    `src/bolo/public/everard_map.h`
  - `data/maps/Inbuilt Tutorial.map`, the map the tutorial is played on
- Apart from the built-in copy of Everard Island, they are separate data files
  that WinBolo loads at run time, not part of the program's code, so they can
  be distributed alongside the GPL program under their own terms.
- A skin can replace any of the graphics and sounds; see
  [docs/SKINS.md](docs/SKINS.md).

## Bolo community maps

- Location: `data/maps/*.map`, except Everard Island and Inbuilt Tutorial
  (above).
- Made by members of the Bolo community and shared publicly on Bolo FTP map
  archives and Usenet newsgroups. Each map remains its author's work.
  **Not licensed under the GPL.**
- Included in WinBolo as freely shared community maps, the way Bolo players
  have always passed them around. `data/maps/readme.txt` describes how the
  original selection was chosen.
- If you made one of these maps and want it credited or removed, open an
  issue.
- They are separate data files that WinBolo loads at run time, distributed
  alongside the GPL program under their own terms.
- The scenario scripts beside some maps (`*.scenario.lua`) are WinBolo's own
  code and are covered by the GPL.

## FetchContent Dependencies (downloaded at build time)

### SDL3 (Simple DirectMedia Layer)
- Version: 3.4.14
- License: Zlib
- https://github.com/libsdl-org/SDL

### SDL3_ttf
- Version: 3.2.0
- License: Zlib
- https://github.com/libsdl-org/SDL_ttf
- Pulls in FreeType (FreeType License) and HarfBuzz (MIT)

### Dear ImGui
- Version: v1.92.9b-docking (the wasm client and log viewer track the docking branch)
- License: MIT
- https://github.com/ocornut/imgui

### Lua
- Version: 5.4.7
- License: MIT
- https://github.com/walterschell/Lua

### LuaJIT (default on desktop; -DWINBOLO_LUAJIT=OFF builds PUC-Lua 5.4 instead)
- Version: v2.1 branch, commit faaf663340347a78b22ed94c63c24fe090bd9784
- License: MIT
- https://github.com/LuaJIT/LuaJIT
- Author: Mike Pall
- Runs the bot brains. Not available on wasm or iOS.

### zlib (includes minizip)
- Version: 1.3.1
- License: Zlib
- https://github.com/madler/zlib

### cJSON
- Version: 1.7.18
- License: MIT
- https://github.com/DaveGamble/cJSON

### libopus
- Version: 1.5.2
- License: BSD 3-Clause, with the royalty-free patent licenses listed in its COPYING
- https://github.com/xiph/opus
- The voice chat codec, in the desktop and wasm clients. The dedicated server forwards encoded frames without decoding them and does not link it.

### SpeexDSP
- Version: 1.2.1, commit ba75b509fb1c5940ea07fa2ba5552e44f3a3576b (pinned to a commit because the newest release tag ships autotools only; the CMake build lives on main)
- License: BSD 3-Clause
- https://github.com/xiph/speexdsp
- Echo cancellation, noise suppression and automatic gain control on captured voice. Desktop client only — the browser gets these from getUserMedia.

### curl (Windows only; Linux/macOS use system libcurl)
- Version: 8.12.1
- License: MIT
- https://github.com/curl/curl

### libmaxminddb (optional, for GeoIP)
- Version: 1.12.2
- License: Apache 2.0
- https://github.com/maxmind/libmaxminddb

### ONNX Runtime (optional, for ML brain inference)
- Version: 1.22.0
- License: MIT
- https://github.com/microsoft/onnxruntime
- The prebuilt library bundles over sixty components under Apache 2.0, BSD
  and MIT licences, among them protobuf, Eigen, Boost, abseil, flatbuffers and
  cpuinfo. Their notices are in ONNX Runtime's
  [ThirdPartyNotices.txt](https://github.com/microsoft/onnxruntime/blob/v1.22.0/ThirdPartyNotices.txt),
  which ships with every WinBolo build that includes the library as
  `THIRD_PARTY_NOTICES-onnxruntime.txt`, beside this file.

### libplum (optional, BOLO_PORTMAP; not built on iOS or wasm)
- Version: 0.5.3
- License: Mozilla Public License 2.0
- https://github.com/paullouisageneau/libplum
- UPnP / NAT-PMP / PCP port mapping for hosted servers

### sentry-native (optional, ENABLE_SENTRY; desktop)
- Version: 0.14.2
- License: MIT
- https://github.com/getsentry/sentry-native
- On Windows (MSVC) and Linux, sentry-native is built with its crashpad
  backend, which brings in the two libraries below. MinGW and macOS use its
  in-process backend and include neither.

#### crashpad (sentry-native's fork, bundled with sentry-native)
- License: Apache 2.0
- https://github.com/getsentry/crashpad

#### mini_chromium (bundled with crashpad)
- License: BSD 3-Clause
- https://github.com/getsentry/mini_chromium

### sentry-cocoa (optional, ENABLE_SENTRY; iOS)
- Version: 9.8.0
- License: MIT
- https://github.com/getsentry/sentry-cocoa

## Not included: Steamworks SDK

### Steamworks SDK (optional; Steam builds only)
- Owner: Valve Corporation
- License: proprietary, under the
  [Steamworks SDK Access Agreement](https://partner.steamgames.com/documentation/sdk_access_agreement).
  Not licensed under the GPL.
- Not included in this repository or in any WinBolo source release. A Steam
  build needs a copy placed in `third_party/steamworks/`, obtained from Valve;
  without it the build uses `src/steam/steam_wrapper_stub.c` instead.
- A Steam build links the SDK's `steam_api` runtime library and ships it beside
  the game. The copyright holders of WinBolo permit this linking and
  distribution in Permission 1 of [LICENSE-EXCEPTION.md](LICENSE-EXCEPTION.md).
- Provides the Steam overlay, rich presence, achievements, Steam Input and the
  Workshop.

## Vendored Source

### TweetNaCl
- Location: src/third_party/tweetnacl/
- License: Public domain
- https://tweetnacl.cr.yp.to
- Authors: Daniel J. Bernstein, Tanja Lange, Peter Schwabe
- Used for Ed25519 request signing

### nanosvg
- Location: src/third_party/nanosvg/
- License: Zlib
- https://github.com/memononen/nanosvg
- Author: Mikko Mononen
- Modified for WinBolo: nanosvg.h wraps the NSVGpaint anonymous union in
  `#pragma warning(push/disable: 4201/pop)` under `_MSC_VER`, so including
  the header does not disable C4201 in caller code. Re-apply on upgrade.

### stb_image
- Location: src/third_party/stb/
- Version: 2.30
- License: Public domain / MIT (dual-licensed)
- https://github.com/nothings/stb
- Author: Sean Barrett

### msf_gif
- Location: src/third_party/msf_gif/
- Version: 2.4
- License: MIT / Public domain (dual-licensed)
- https://github.com/notnullnotvoid/msf_gif
- Author: Miles Fogle
- Used to encode the post-game recap's highlight clips as animated GIFs

### utf8proc
- Location: src/third_party/utf8proc/
- Version: 2.10.0
- License: MIT
- https://github.com/JuliaStrings/utf8proc
- Authors: Steven G. Johnson, Jiahao Chen, Peter Colberg, Tony Kelman, Scott P. Jones, and other contributors; Public Software Group e. V.
- Used for UTF-8 NFC normalization and codepoint property lookup in player-name validation

### imgui_markdown
- Location: src/third_party/imgui_markdown/
- Commit: 7f88a689f783b5f628a2c446ccc2e7198e732dfe (2026-05-18)
- License: Zlib
- https://github.com/juliettef/imgui_markdown
- Authors: Juliette Foucaut, Doug Binks
- Used to render the in-game news popup (headings, emphasis, links, images)

### mdns
- Location: src/third_party/mdns/
- Version: 1.4.3
- License: Public domain
- https://github.com/mjansson/mdns
- Author: Mattias Jansson

### ImGuiColorTextEdit
- Location: src/third_party/ImGuiColorTextEdit/
- Commit: 264bee49ddc3c789b05d928d09c628649458da47 (2025-10-11)
- License: MIT
- https://github.com/santaclose/ImGuiColorTextEdit
- Authors: BalazsJako (original), santaclose
- The map editor's scenario script pane - a text editor with Lua syntax
  highlighting. Only TextEditor.h, TextEditor.cpp, LanguageDefinitions.cpp
  and LICENSE are taken; the demo panel and tests are not.
- Modified for WinBolo: GetLineCount casts mLines.size() to int, and
  SetPalette gives palletteBase a starting value ahead of its switch, so
  MSVC's /W4 /WX /sdl build accepts the header and the sources.
  Re-apply on upgrade.

### MD5 (RFC 1321 reference)
- Location: src/common/md5.c, src/common/md5.h
- License: Public domain
- https://www.rfc-editor.org/rfc/rfc1321
- Used for the lobby map-upload MD5 handshake (integrity check only — not cryptographically safe)

### Lua 5.4 pattern matcher
- Location: src/scenario/scenario_pattern.c
- Version: 5.4.7 (the PATTERN MATCHING section of lstrlib.c)
- License: MIT
- https://www.lua.org
- Authors: R. Ierusalimschy, L. H. de Figueiredo, W. Celes (Lua.org, PUC-Rio)
- Serves string.find, string.match, string.gmatch and string.gsub to
  scenario scripts, with 5.4's behaviour on LuaJIT hosts as well
- Modified for WinBolo: every matcher step is counted and charged to the
  scenario instruction budgets, gmatch's iterator resets its recursion depth
  on each call, and the few 5.4 API calls LuaJIT lacks are shimmed. Lua's
  notice is kept at the top of the file. Re-apply on upgrade.

### ldump
- Location: brains/GoalHunter/ldump.lua and brains/GoalHunter/opt/ldump.lua
- Version: 1.4.0 (the library's init.lua)
- License: MIT No Attribution (MIT-0)
- https://github.com/girvel/ldump
- Author: Nikita Dobrynin (girvel)
- A Lua serialisation library. The GoalHunter brain's debugger uses it to
  snapshot and restore the brain's state. The two copies are identical; the
  root one is the recorder build and `opt/` the production build.
- Unmodified apart from the origin line at the top of each copy. The licence
  asks for no credit; the entry is here so the file is not taken for WinBolo's
  own code.

## Controller Glyphs

### Xelu's Free Controller & Key Prompts
- Location: data/controller/
- License: CC0 1.0 (public domain)
- https://thoseawesomeguys.com/prompts/
- Author: Nicolae (Xelu) Berbece
- Xbox, PlayStation 5, Nintendo Switch, and keyboard/mouse glyph atlases used as the Path B (non-Steam-Input) glyph fallback for on-screen button hints

## Fonts

### Inter
- Location: data/fonts/InterVariable.ttf
- License: SIL Open Font License 1.1
- https://github.com/rsms/inter
- Author: Rasmus Andersson

### Noto Sans CJK
- Location: data/fonts/NotoSansCJK{jp,kr,sc,tc}-Regular.otf
- License: SIL Open Font License 1.1 (data/fonts/LICENSE)
- https://github.com/notofonts/noto-cjk
- Author: Google LLC, Adobe Inc.

### Sarasa Mono Slab
- Location: data/fonts/SarasaMonoSlab{J,K,SC,TC}-Regular.ttf
- License: SIL Open Font License 1.1 (data/fonts/LICENSE)
- https://github.com/be5invis/Sarasa-Gothic
- Author: belleve invis (Renzhi Li)

## Audio

### Universal UI/Menu Soundpack
- Location: data/sounds/lobby_chat.wav, data/sounds/lobby_ready.wav, data/sounds/lobby_unready.wav, data/sounds/lobby_player_join.wav, data/sounds/lobby_player_leave.wav, data/sounds/lobby_countdown.wav
- License: Creative Commons Attribution 4.0 International (CC BY 4.0)
- https://cyrex-studios.itch.io/universal-ui-soundpack
- Author: Nathan Gibson
- Lobby UI and event sound effects (chat, ready/unready, player join/leave, countdown tick).

### Race start beeps
- Location: data/sounds/lobby_game_start.wav
- License: Pixabay Content License
- https://pixabay.com/sound-effects/film-special-effects-race-start-beeps-125125/
- Source: Pixabay
- Lobby game-start sound.

## Data Files

### DB-IP IP-to-Country Lite
- Location: data/dbip-country-lite.mmdb
- License: Creative Commons Attribution 4.0 International (CC BY 4.0)
- https://db-ip.com/db/download/ip-to-country-lite
- Author: DB-IP (https://db-ip.com)
- Used by the server (and the in-client game browser) for IP-to-country lookups via libmaxminddb. Rebuilt monthly; download the latest .mmdb and replace the file in place.

### flag-icons
- Location: data/flags/
- License: MIT
- https://github.com/lipis/flag-icons
- Author: Panayiotis Lipiridis and contributors
- Version: v7.5.0 (commit 7aa5b2bdddd570ece62c812c0cb588ccdc099e2e)
- The flag SVG art in data/flags/ comes from this project. data/flags/countries.csv is derived from its country.json: the English country names keyed by the 2-char flag basenames.
