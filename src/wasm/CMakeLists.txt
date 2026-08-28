###
# WinBolo Game Client WebAssembly Build
#
# Standalone CMake project — does not modify the main CMakeLists.txt.
# Usage:
#   cd winbolo
#   emcmake cmake -B build-wasm-game -S src/wasm
#   cmake --build build-wasm-game
#
# Output: build-wasm-game/winbolo.html, winbolo.js, winbolo.wasm, winbolo.data
#
# Serve locally with: python3 -m http.server -d build-wasm-game

cmake_minimum_required(VERSION 3.18)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")

project(WinBoloWasm C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# Repo root (2 levels up from this CMakeLists.txt: wasm/ -> src/ -> repo)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SRC_ROOT "${REPO_ROOT}/src")
set(DATA_DIR "${REPO_ROOT}/data")

include(${REPO_ROOT}/cmake/bolo_lib.cmake)
include(${REPO_ROOT}/cmake/bolo_version.cmake)

###
# Fetch dependencies (same versions as main build)
#

# Tell SDL3_ttf to build freetype from source
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.2
)
FetchContent_Declare(sdl3_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG        release-3.2.0
)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/walterschell/Lua.git
    GIT_TAG        v5.4.7
)
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
)
FetchContent_Declare(cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

# Disable all test/example targets from dependencies
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UNINSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(sdl3 sdl3_ttf imgui lua zlib cjson)

###
# nanosvg — header-only SVG parser/rasterizer (vendored)
#
add_library(nanosvg STATIC ${SRC_ROOT}/third_party/nanosvg/nanosvg_impl.c)
target_include_directories(nanosvg SYSTEM PUBLIC ${SRC_ROOT}/third_party/nanosvg)
target_compile_options(nanosvg PRIVATE -Wno-unused-function)

# stb_image — header-only image loader (vendored)
add_library(stb_image STATIC ${SRC_ROOT}/third_party/stb/stb_image_impl.c)
target_include_directories(stb_image SYSTEM PUBLIC ${SRC_ROOT}/third_party/stb)
target_compile_options(stb_image PRIVATE -Wno-unused-function)

# utf8proc — Unicode normalization + property tables (vendored, MIT)
add_library(utf8proc STATIC ${SRC_ROOT}/third_party/utf8proc/utf8proc.c)
target_include_directories(utf8proc SYSTEM PUBLIC ${SRC_ROOT}/third_party/utf8proc)
target_compile_definitions(utf8proc PUBLIC UTF8PROC_STATIC=1)

###
# minizip (from zlib contrib)
#
add_library(minizip STATIC
    ${zlib_SOURCE_DIR}/contrib/minizip/zip.c
    ${zlib_SOURCE_DIR}/contrib/minizip/unzip.c
    ${zlib_SOURCE_DIR}/contrib/minizip/ioapi.c
)
target_include_directories(minizip PUBLIC ${zlib_SOURCE_DIR}/contrib/minizip)
target_link_libraries(minizip PUBLIC zlibstatic)

###
# ImGui backends (SDL3 + SDL Renderer)
#
add_library(imgui_backends STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui_backends PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_backends PUBLIC SDL3::SDL3)

###
# bolo_static — the bulk of the bolo source set as a single static lib.
# transport_udp_client.c stays per-target (HAVE_STEAM-divergent);
# nat_portmap.c isn't referenced by the wasm build.
#
add_subdirectory(${REPO_ROOT}/src/bolo ${CMAKE_BINARY_DIR}/bolo_static)
add_subdirectory(${REPO_ROOT}/src/server ${CMAKE_BINARY_DIR}/server_static)
add_subdirectory(${REPO_ROOT}/brains ${CMAKE_BINARY_DIR}/bot_brains_static)

set(BOLO_SOURCES
    ${SRC_ROOT}/bolo/transport_udp_client.c
)

# Server modules (used by client too).
# server_sim.c + servermessages.c come from server_sim_static.
# threads.c (or threads_wasm.c here) comes from threads_static.
# server_static (the dedicated-server runtime: transport_udp_server.c,
# NAT/tracker/WBN bring-up) is NOT linked — the wasm client can't host.
# server_stubs.c + server_stubs_wasm.c cover the server-side symbols the
# shared sim/dialogs reference; serverInstanceStartup is a local-only stub
# in server_stubs_wasm.c, matching the iOS/Android/BrainTest clients.
set(SERVER_SOURCES
    ${SRC_ROOT}/server/geolookup.c
    ${SRC_ROOT}/server/server_stubs.c
)

# LZW compression
file(GLOB LZW_SOURCES ${SRC_ROOT}/lzw/*.c)

# Map editor sources reachable from shared dialogs (map chooser pulls in
# the procedural generator's imgui controls; mapgen.c/mapgen_maze.c come
# in via bolo_static).
set(MAPEDITOR_SOURCES
    ${SRC_ROOT}/mapeditor/mapgen_imgui.cpp
)

# WinBoloNet — replaced by a no-op stub (libcurl not available in Emscripten)
set(WBN_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/winbolonet_wasm.c)

# GUI sources (SDL3) — excluding files we replace (winbolo.c, gamefront.c, sound.c, netclient.c, clientmutex.c, dns_lookups.c)
set(GUI_SOURCES
    ${SRC_ROOT}/gui/ui_mode.c
    ${SRC_ROOT}/gui/skins.c
    ${SRC_ROOT}/gui/sdl3/sdl3imgui.cpp
    ${SRC_ROOT}/gui/sdl3/imgui_steam_nav.cpp
    ${SRC_ROOT}/gui/sdl3/sdl3imgui_tablet.cpp
    ${SRC_ROOT}/gui/sdl3/luabrainshandler.c
    ${SRC_ROOT}/gui/sdl3/minimap_render.c
    ${SRC_ROOT}/gui/sdl3/cursor.c
    ${SRC_ROOT}/gui/sdl3/flags.c
    ${SRC_ROOT}/gui/sdl3/dialog_backend.c
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_welcome.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_mapchooser.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_udpsetup.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_gamebrowser.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_setname.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_skins.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_trackersetup.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_winbolonet.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_messagebox.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_tutorial_overlay.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_keycap.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_nav_outline.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_lobby.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_settings.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_keysetup.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_deck_pause.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_keyboard.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_quickchat.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_about.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/dialog_footer.cpp
    ${SRC_ROOT}/gui/sdl3/map_preview_popup.cpp
    ${SRC_ROOT}/gui/sdl3/lobby_start_list.cpp
    ${SRC_ROOT}/gui/sdl3/map_preview_view.cpp
    ${SRC_ROOT}/gui/sdl3/wb_theme.cpp
    ${SRC_ROOT}/gui/sdl3/bg_game.c
    ${SRC_ROOT}/gui/sdl3/mapview.c
    ${SRC_ROOT}/gui/sdl3/sprite_positions.c
    ${SRC_ROOT}/gui/sdl3/tileloader.c
    ${SRC_ROOT}/gui/sdl3/sdl3draw.c
    ${SRC_ROOT}/gui/sdl3/sdl3draw_status.c
    ${SRC_ROOT}/gui/sdl3/sdl_bmp.c
    ${SRC_ROOT}/gui/sdl3/input.c
    ${SRC_ROOT}/gui/sdl3/input_touch.c
    ${SRC_ROOT}/gui/sdl3/input_joystick.c
    ${SRC_ROOT}/gui/sdl3/input_gamepad.c
    ${SRC_ROOT}/gui/sdl3/glyphs.c
    ${SRC_ROOT}/gui/sdl3/build_cursor.c
    ${SRC_ROOT}/gui/sdl3/tutorial_text.c
    ${SRC_ROOT}/gui/sdl3/lang.c
)

# Log viewer sources — the .wbv decoder, its block-grid renderer and the
# embedded reel host (lv_embed.c). The modal viewer, the spectator host and
# every ImGui panel of the viewer's own are left out; lv_stubs_wasm.c (in
# WASM_SOURCES below) resolves what the decoder still calls back into.
#
# Never add here:
#   logviewer.c, main.c   - the modal viewer / spectator host and the
#                           standalone entry point. lv_embed.c is the half of
#                           the host that every build needs; logviewer.c drags
#                           in the viewer's menus, dialogs and comments panel.
#   clientmutex.c, dns.c,
#   platform/platform_config.c,
#   platform/platform_dialogs.c
#                         - desktop drivers, replaced by the wasm variants
#                           under src/logviewer/wasm listed below.
#   bolo_shim.c           - maps bolo names onto lv_* bodies for the
#                           standalone viewer's copy of mapview.c; it would
#                           collide with src/bolo's own definitions here.
#   imgui_main_menu.cpp, imgui_dialogs.cpp, imgui_comments.cpp,
#   imgui_logviewer_menu.cpp
#                         - reach WinBolo.net through http.c, which carries a
#                           hard #error under Emscripten because it embeds the
#                           WBN signing key. Not ported, not stubbed into the
#                           UI: the web build has no comments panel.
#
# src/logviewer/wasm/clientmutex_wasm.c and dns_wasm.c do not collide with
# src/wasm/clientmutex_wasm.c and dns_wasm.c already in this target: the
# logviewer pair defines lv_clientMutex* / lv_dns*, the client pair defines
# clientMutex* / dnsLookups*. Nothing is shared between them.
set(LOGVIEWER_SOURCES
    # Host bring-up, replay driver and the embedded reel API
    ${SRC_ROOT}/logviewer/lv_embed.c
    # Log container: zip/zlib member access and the snapshot index
    ${SRC_ROOT}/logviewer/blocks.c
    ${SRC_ROOT}/logviewer/lv_attribution.c
    ${SRC_ROOT}/logviewer/snapshot.c
    # Decode + screen assembly
    ${SRC_ROOT}/logviewer/screen.c
    ${SRC_ROOT}/logviewer/screencalc.c
    ${SRC_ROOT}/logviewer/screentank.c
    ${SRC_ROOT}/logviewer/screenlgm.c
    ${SRC_ROOT}/logviewer/screenbullet.c
    ${SRC_ROOT}/logviewer/spec_seed_load.c
    # Replayed world model
    ${SRC_ROOT}/logviewer/players.c
    ${SRC_ROOT}/logviewer/shells.c
    ${SRC_ROOT}/logviewer/allience.c
    ${SRC_ROOT}/logviewer/pillbox.c
    ${SRC_ROOT}/logviewer/bases.c
    ${SRC_ROOT}/logviewer/bolo_map.c
    ${SRC_ROOT}/logviewer/starts.c
    ${SRC_ROOT}/logviewer/labels.c
    ${SRC_ROOT}/logviewer/messages.c
    ${SRC_ROOT}/logviewer/sounddist.c
    ${SRC_ROOT}/logviewer/global.c
    ${SRC_ROOT}/logviewer/util.c
    # Tile renderer. game_view_stubs.c covers the standalone-only game view
    # that draw.c calls when LogViewerState::gameView is set — never in an
    # embed, which leaves it FALSE.
    ${SRC_ROOT}/logviewer/draw.c
    ${SRC_ROOT}/logviewer/game_view_stubs.c
    # Platform drivers, wasm variants
    ${SRC_ROOT}/logviewer/wasm/clientmutex_wasm.c
    ${SRC_ROOT}/logviewer/wasm/dns_wasm.c
    ${SRC_ROOT}/logviewer/wasm/platform_config_wasm.c
    ${SRC_ROOT}/logviewer/wasm/platform_dialogs_wasm.c
)

# WASM replacement sources
set(WASM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gamefront_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/clientmutex_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dns_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/server_stubs_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gui_stubs_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lv_stubs_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sound_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/prefs_bridge_wasm.c
    # The recap reel's WinBolo.net round-log source. http.c (libcurl + signing
    # key) stays excluded here too; this drives the page's fetch() instead, and
    # without suspending the C stack, since the reel polls it mid-frame.
    ${CMAKE_CURRENT_SOURCE_DIR}/round_log_fetch_wasm.c
    ${SRC_ROOT}/steam/steam_wrapper_stub.c
    ${SRC_ROOT}/common/sentry_integration.c
    ${SRC_ROOT}/common/wb_log.c
    ${SRC_ROOT}/common/md5.c
    ${SRC_ROOT}/common/mp_diag_log.c
    # Cloud-prefs sync: the document store and the pure decision/PUT-body
    # logic. http.c (libcurl + signing key) stays excluded — prefs_bridge_wasm.c
    # supplies the fetch()-backed transport instead.
    ${SRC_ROOT}/common/prefs.c
    ${SRC_ROOT}/common/prefs_doc.c
    ${SRC_ROOT}/winbolonet/wbn_prefs_sync.c
    # Shared client-frontend cores called by both desktop and web (the
    # connect/landing contract and the per-tick step live here so they cannot
    # drift per platform).
    ${SRC_ROOT}/client_frontend/client_frontend_connect.c
    ${SRC_ROOT}/client_frontend/client_frontend_tick.c
    ${SRC_ROOT}/client_frontend/client_frontend_common.c
)

###
# Build the WASM executable
#
add_executable(winbolo
    ${BOLO_SOURCES}
    ${SERVER_SOURCES}
    ${LZW_SOURCES}
    ${WBN_SOURCES}
    ${GUI_SOURCES}
    ${MAPEDITOR_SOURCES}
    ${LOGVIEWER_SOURCES}
    ${WASM_SOURCES}
)

target_include_directories(winbolo PRIVATE
    ${SRC_ROOT}/gui/sdl3
    ${SRC_ROOT}/gui
    ${SRC_ROOT}/server
    ${SRC_ROOT}/mapeditor
    ${SRC_ROOT}/winbolonet
    ${SRC_ROOT}/client_frontend
    ${cjson_SOURCE_DIR}
    ${SRC_ROOT}/third_party/imgui_markdown
    # Listed after src/gui so a GUI translation unit's "tiles.h" / "positions.h"
    # still resolves to its own; the log viewer's copies of those names reach
    # its TUs through the including file's own directory.
    ${SRC_ROOT}/logviewer
    ${SRC_ROOT}/logviewer/imgui
    ${SRC_ROOT}/logviewer/platform
)
bolo_apply_include_rules(winbolo gui)
# bg_game.c reaches into a privatised setTeam setter for the dev-mode
# lobby-background animation; mirror the main build's per-file grant.
# server_stubs_wasm.c's local-only serverInstanceStartup calls
# serverSimApplyInstanceConfig, declared in internal/server_sim_lifecycle.h.
bolo_grant_internal_source_access(winbolo
    ${SRC_ROOT}/bolo/transport_udp_client.c
    ${SRC_ROOT}/gui/sdl3/bg_game.c
    ${CMAKE_CURRENT_SOURCE_DIR}/server_stubs_wasm.c)

target_compile_definitions(winbolo PRIVATE
    _GNU_SOURCE
    USE_64WITH32
    HAVE_SCREEN_C=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)
target_compile_options(winbolo PRIVATE -fno-common -include sys/types.h)
target_link_options(winbolo PRIVATE "LINKER:--allow-multiple-definition")

target_link_libraries(winbolo PRIVATE bolo_static server_sim_static bot_brains_static threads_static)
target_link_libraries(winbolo PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    imgui_backends
    minizip
    lua_static
    nanosvg
    stb_image
    utf8proc
    cjson
)

###
# Emscripten-specific settings
#

# Copy map files with spaces to build dir with safe names
set(PRELOAD_DIR "${CMAKE_CURRENT_BINARY_DIR}/preload")
file(MAKE_DIRECTORY "${PRELOAD_DIR}")
file(COPY "${REPO_ROOT}/data/maps/Everard Island.map" DESTINATION "${PRELOAD_DIR}")
file(RENAME "${PRELOAD_DIR}/Everard Island.map" "${PRELOAD_DIR}/EverardIsland.map")
file(COPY "${REPO_ROOT}/data/maps/Inbuilt Tutorial.map" DESTINATION "${PRELOAD_DIR}")
file(RENAME "${PRELOAD_DIR}/Inbuilt Tutorial.map" "${PRELOAD_DIR}/InbuiltTutorial.map")

# ---------------------------------------------------------------------------
# CJK font subsetting. The bundled Noto Sans CJK + Sarasa Mono Slab fonts are
# ~163 MB uncompressed and dominate the preload. Subset them to the glyph
# ranges WinBolo can actually display — parsed straight out of imgui_draw.cpp
# so they track the FetchContent'd ImGui version — cutting that to ~19 MB.
#   * Noto (ImGui dialog font) is range-pinned: the atlas only bakes the
#     requested ranges, so subsetting it is lossless.
#   * Sarasa (in-game text, FreeType) renders arbitrary codepoints; we subset
#     it to the union of the three ranges, which only narrows in-game CJK
#     coverage to match what the lobby dialogs already cover.
# Needs Python 3 + fonttools. If either is missing we warn and fall back to
# preloading the full fonts, so the build still works.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
set(_have_fonttools FALSE)
if(Python3_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import fontTools"
        RESULT_VARIABLE _ft_rv OUTPUT_QUIET ERROR_QUIET)
    if(_ft_rv EQUAL 0)
        set(_have_fonttools TRUE)
    endif()
endif()

set(_cjk_fonts
    NotoSansCJKjp-Regular.otf   NotoSansCJKkr-Regular.otf
    NotoSansCJKsc-Regular.otf   NotoSansCJKtc-Regular.otf
    SarasaMonoSlabJ-Regular.ttf SarasaMonoSlabK-Regular.ttf
    SarasaMonoSlabSC-Regular.ttf SarasaMonoSlabTC-Regular.ttf)

if(_have_fonttools)
    set(SUBSET_FONTS_DIR "${CMAKE_CURRENT_BINARY_DIR}/fonts_subset")
    set(SUBSET_STAMP     "${CMAKE_CURRENT_BINARY_DIR}/fonts_subset.stamp")
    set(SUBSET_SCRIPT    "${REPO_ROOT}/cmake/subset_cjk_fonts.py")

    set(_cjk_font_srcs)
    foreach(_f IN LISTS _cjk_fonts)
        list(APPEND _cjk_font_srcs "${DATA_DIR}/fonts/${_f}")
    endforeach()

    add_custom_command(
        OUTPUT ${SUBSET_STAMP}
        COMMAND ${Python3_EXECUTABLE} ${SUBSET_SCRIPT}
                --imgui-draw ${imgui_SOURCE_DIR}/imgui_draw.cpp
                --src-fonts ${DATA_DIR}/fonts
                --out ${SUBSET_FONTS_DIR}
        COMMAND ${CMAKE_COMMAND} -E touch ${SUBSET_STAMP}
        DEPENDS ${SUBSET_SCRIPT} ${imgui_SOURCE_DIR}/imgui_draw.cpp ${_cjk_font_srcs}
        COMMENT "Subsetting CJK fonts to ImGui glyph ranges (~163MB -> ~19MB)"
        VERBATIM)
    add_custom_target(subset_fonts DEPENDS ${SUBSET_STAMP})
    add_dependencies(winbolo subset_fonts)
    # Force a relink (which regenerates the preloaded .data) when the subset
    # fonts change but no object file did — e.g. a source font or ImGui bump.
    set_target_properties(winbolo PROPERTIES LINK_DEPENDS "${SUBSET_STAMP}")

    # Preload all of data/ EXCEPT the full CJK fonts, then layer the subset
    # versions over /data/fonts. --exclude-file matches the file's source path,
    # so listing the originals' exact paths strips only them; the subset dir
    # (a different path) is preloaded intact. Exact paths — no wildcards — so
    # the link command's shell can't glob them.
    set(WASM_DATA_PRELOAD "SHELL:--preload-file ${DATA_DIR}@/data")
    foreach(_f IN LISTS _cjk_fonts)
        list(APPEND WASM_DATA_PRELOAD
             "SHELL:--exclude-file ${DATA_DIR}/fonts/${_f}")
    endforeach()
    list(APPEND WASM_DATA_PRELOAD
         "SHELL:--preload-file ${SUBSET_FONTS_DIR}@/data/fonts")
else()
    message(WARNING
        "CJK font subsetting disabled (need Python 3 + fonttools: "
        "`pip install fonttools brotli`) — preloading the full ~163 MB fonts")
    set(WASM_DATA_PRELOAD "SHELL:--preload-file ${DATA_DIR}@/data")
endif()

# Preload data files into the virtual filesystem
target_link_options(winbolo PRIVATE
    ${WASM_DATA_PRELOAD}
    "SHELL:--preload-file ${PRELOAD_DIR}/EverardIsland.map@/data/EverardIsland.map"
    "SHELL:--preload-file ${PRELOAD_DIR}/InbuiltTutorial.map@/data/InbuiltTutorial.map"
    "SHELL:--preload-file ${REPO_ROOT}/brains/GoalHunter_1.6@/Brains/GoalHunter_1.6"
    # _malloc/_free + the string/value helpers are used by prefs_bridge_wasm.c's
    # EM_ASYNC_JS fetch transport to hand response bodies back to C.
    "SHELL:-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_wbWasmWheel','_wbWasmResetHeldKeys']"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','lengthBytesUTF8','stringToUTF8','UTF8ToString','setValue']"
    -sALLOW_MEMORY_GROWTH=1
    "-sINITIAL_MEMORY=67108864"
    "-sSTACK_SIZE=1048576"
    "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html"
    -sFORCE_FILESYSTEM=1
    -sASYNCIFY=1
    -lwebsocket.js
)

# --shell-file above is just a flag string to CMake — it does not parse the
# path out and register it as an input, so editing shell.html changes no
# object file and no tracked dependency, and the link is skipped. The build
# then succeeds while still emitting the previous winbolo.html, and the only
# symptom is a page whose behaviour does not match the source. Same reason
# the CJK subset stamp is a link dependency above; APPEND so it composes
# with that one instead of replacing it.
set_property(TARGET winbolo APPEND PROPERTY
             LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")

set_target_properties(winbolo PROPERTIES
    SUFFIX ".html"
)

# ---------------------------------------------------------------------------
# wasm-dist: precompress the web assets (for Caddy
# `file_server { precompressed br gzip }`) and pack everything a deploy needs
# into wasm-dist.zip. Run explicitly — it is NOT part of the normal build, so
# day-to-day builds don't re-brotli the data file:
#     cmake --build <build-dir> --target wasm-dist
# Requires `brotli` (apt-get install brotli); gzip is already standard.
# ---------------------------------------------------------------------------
find_program(BROTLI_EXE brotli)
find_program(GZIP_EXE   gzip)
if(BROTLI_EXE AND GZIP_EXE)
    # Files served by Caddy. -k keeps the original (Caddy needs it as the
    # fallback for clients that accept neither encoding); -f overwrites stale
    # variants. CJK font subsetting brings the data file down to ~68 MB, small
    # enough to brotli every asset at -q 11.
    set(_wasm_dist_assets winbolo.html winbolo.js winbolo.wasm winbolo.data)
    set(_wasm_dist_cmds)
    set(_wasm_dist_zip_files)
    foreach(_a IN LISTS _wasm_dist_assets)
        list(APPEND _wasm_dist_cmds COMMAND ${BROTLI_EXE} -f -k -q 11 "${_a}")
        list(APPEND _wasm_dist_cmds COMMAND ${GZIP_EXE} -f -k -9 "${_a}")
        list(APPEND _wasm_dist_zip_files "${_a}" "${_a}.br" "${_a}.gz")
    endforeach()

    add_custom_target(wasm-dist
        ${_wasm_dist_cmds}
        COMMAND ${CMAKE_COMMAND} -E tar "cf" wasm-dist.zip --format=zip
                ${_wasm_dist_zip_files}
        WORKING_DIRECTORY $<TARGET_FILE_DIR:winbolo>
        COMMENT "Precompressing web assets (.br + .gz) and packing wasm-dist.zip"
        VERBATIM
    )
    add_dependencies(wasm-dist winbolo)
else()
    message(WARNING
        "wasm-dist: brotli and/or gzip not found "
        "(apt-get install brotli) — the wasm-dist target is unavailable")
endif()
