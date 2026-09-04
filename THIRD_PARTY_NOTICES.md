# Third-Party Notices

WinBolo uses the following third-party libraries and code.

## FetchContent Dependencies (downloaded at build time)

### SDL3 (Simple DirectMedia Layer)
- Version: 3.4.2
- License: Zlib
- https://github.com/libsdl-org/SDL

### SDL3_ttf
- Version: 3.2.0
- License: Zlib
- https://github.com/libsdl-org/SDL_ttf
- Pulls in FreeType (FreeType License) and HarfBuzz (MIT)

### Dear ImGui
- Branch: docking
- License: MIT
- https://github.com/ocornut/imgui

### Lua
- Version: 5.4.7
- License: MIT
- https://github.com/walterschell/Lua

### zlib (includes minizip)
- Version: 1.3.1
- License: Zlib
- https://github.com/madler/zlib

### cJSON
- Version: 1.7.18
- License: MIT
- https://github.com/DaveGamble/cJSON

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

### MD5 (RFC 1321 reference)
- Location: src/bolo/md5.c, src/bolo/public/md5.h
- License: Public domain
- https://www.rfc-editor.org/rfc/rfc1321
- Used for the lobby map-upload MD5 handshake (integrity check only — not cryptographically safe)

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

### LZW/RLE Compression
- Location: src/lzw/
- Original author: David Bourgin (1994-1995)
- Modified for WinBolo; distributed under GPL v2+

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
