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

# WASM replacement sources
set(WASM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gamefront_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/clientmutex_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dns_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/server_stubs_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gui_stubs_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sound_wasm.c
    ${SRC_ROOT}/steam/steam_wrapper_stub.c
    ${SRC_ROOT}/common/sentry_integration.c
    ${SRC_ROOT}/common/wb_log.c
    ${SRC_ROOT}/common/md5.c
    ${SRC_ROOT}/common/mp_diag_log.c
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
    ${WASM_SOURCES}
)

target_include_directories(winbolo PRIVATE
    ${SRC_ROOT}/gui/sdl3
    ${SRC_ROOT}/gui
    ${SRC_ROOT}/server
    ${SRC_ROOT}/mapeditor
    ${cjson_SOURCE_DIR}
    ${SRC_ROOT}/third_party/imgui_markdown
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

# Preload data files into the virtual filesystem
target_link_options(winbolo PRIVATE
    "SHELL:--preload-file ${DATA_DIR}@/data"
    "SHELL:--preload-file ${PRELOAD_DIR}/EverardIsland.map@/data/EverardIsland.map"
    "SHELL:--preload-file ${PRELOAD_DIR}/InbuiltTutorial.map@/data/InbuiltTutorial.map"
    "SHELL:--preload-file ${REPO_ROOT}/brains/GoalHunter_1.5@/Brains/GoalHunter_1.5"
    "SHELL:-sEXPORTED_FUNCTIONS=['_main']"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
    -sALLOW_MEMORY_GROWTH=1
    "-sINITIAL_MEMORY=67108864"
    "-sSTACK_SIZE=1048576"
    "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html"
    -sFORCE_FILESYSTEM=1
    -sASYNCIFY=1
    -lwebsocket.js
)

set_target_properties(winbolo PROPERTIES
    SUFFIX ".html"
)
