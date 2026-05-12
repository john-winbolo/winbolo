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

cmake_minimum_required(VERSION 3.15)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")

project(WinBoloWasm C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# Repo root (2 levels up from this CMakeLists.txt: wasm/ -> src/ -> repo)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SRC_ROOT "${REPO_ROOT}/src")
set(DATA_DIR "${REPO_ROOT}/data")

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

# Disable all test/example targets from dependencies
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(sdl3 sdl3_ttf imgui lua zlib)

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
# Bolo engine sources
#
set(BOLO_SOURCES
    ${SRC_ROOT}/bolo/allience.c
    ${SRC_ROOT}/bolo/bases.c
    ${SRC_ROOT}/bolo/bolo_map.c
    ${SRC_ROOT}/bolo/building.c
    ${SRC_ROOT}/bolo/crc.c
    ${SRC_ROOT}/bolo/debug_file_output.c
    ${SRC_ROOT}/bolo/net_packet_debug.c
    ${SRC_ROOT}/bolo/explosions.c
    ${SRC_ROOT}/bolo/floodfill.c
    ${SRC_ROOT}/bolo/gametype.c
    ${SRC_ROOT}/bolo/global.c
    ${SRC_ROOT}/bolo/grass.c
    ${SRC_ROOT}/bolo/labels.c
    ${SRC_ROOT}/bolo/lgm.c
    ${SRC_ROOT}/bolo/log.c
    ${SRC_ROOT}/bolo/messages.c
    ${SRC_ROOT}/bolo/mines.c
    ${SRC_ROOT}/bolo/minesexp.c
    ${SRC_ROOT}/bolo/gui_message.c
    ${SRC_ROOT}/bolo/pillbox.c
    ${SRC_ROOT}/bolo/players.c
    ${SRC_ROOT}/bolo/playername_validate.c
    ${SRC_ROOT}/bolo/playersrejoin.c
    ${SRC_ROOT}/bolo/rubble.c
    ${SRC_ROOT}/bolo/screen.c
    ${SRC_ROOT}/bolo/screenbrainmap.c
    ${SRC_ROOT}/bolo/screenbullet.c
    ${SRC_ROOT}/bolo/screencalc.c
    ${SRC_ROOT}/bolo/screenlgm.c
    ${SRC_ROOT}/bolo/screentank.c
    ${SRC_ROOT}/bolo/scroll.c
    ${SRC_ROOT}/bolo/scroll_item_list.c
    ${SRC_ROOT}/bolo/shells.c
    ${SRC_ROOT}/bolo/sounddist.c
    ${SRC_ROOT}/bolo/starts.c
    ${SRC_ROOT}/bolo/swamp.c
    ${SRC_ROOT}/bolo/tank.c
    ${SRC_ROOT}/bolo/tankexp.c
    ${SRC_ROOT}/bolo/treegrow.c
    ${SRC_ROOT}/bolo/udppackets.c
    ${SRC_ROOT}/bolo/util.c
    ${SRC_ROOT}/bolo/braincore.c
    ${SRC_ROOT}/bolo/brain_data.c
    ${SRC_ROOT}/bolo/brain_overlay.c
    ${SRC_ROOT}/bolo/brain_pathfinder.c
    ${SRC_ROOT}/bolo/brain_pathfinder_serial.c
    ${SRC_ROOT}/bolo/brain_worldsim.c
    ${SRC_ROOT}/bolo/transport_local.c
    ${SRC_ROOT}/bolo/transport_udp_common.c
    ${SRC_ROOT}/bolo/transport_udp_client.c
    ${SRC_ROOT}/bolo/transport_udp_server.c
    ${SRC_ROOT}/bolo/client_state.c
    ${SRC_ROOT}/bolo/client_snapshot.c
    ${SRC_ROOT}/bolo/interpolation.c
    ${SRC_ROOT}/bolo/position_history.c
    ${SRC_ROOT}/bolo/client_sim.c
    ${SRC_ROOT}/bolo/client_sim_control.c
    ${SRC_ROOT}/bolo/bot_manager.c
    ${SRC_ROOT}/bolo/bot_worker_pool.c
    ${SRC_ROOT}/bolo/tutorial.c
    ${SRC_ROOT}/bolo/discovery.c
)

# Server modules (used by client too)
# threads.c is replaced by threads_wasm.c in WASM_SOURCES
set(SERVER_SOURCES
    ${SRC_ROOT}/server/geolookup.c
    ${SRC_ROOT}/server/servermessages.c
    ${SRC_ROOT}/server/server_sim.c
    ${SRC_ROOT}/server/server_stubs.c
    ${SRC_ROOT}/server/posix_stubs.c
)

# LZW compression
file(GLOB LZW_SOURCES ${SRC_ROOT}/lzw/*.c)

# Map editor sources reachable from shared dialogs (map chooser pulls in
# the procedural generator + its imgui controls + maze generator).
set(MAPEDITOR_SOURCES
    ${SRC_ROOT}/mapeditor/mapeditor_generate.c
    ${SRC_ROOT}/mapeditor/mapeditor_maze.c
    ${SRC_ROOT}/mapeditor/mapgen_imgui.cpp
)

# WinBoloNet — replaced by a no-op stub (libcurl not available in Emscripten)
set(WBN_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/winbolonet_wasm.c)

# GUI sources (SDL3) — excluding files we replace (winbolo.c, gamefront.c, sound.c, netclient.c, clientmutex.c, dns_lookups.c)
set(GUI_SOURCES
    ${SRC_ROOT}/gui/ui_mode.c
    ${SRC_ROOT}/gui/skins.c
    ${SRC_ROOT}/gui/sdl3/sdl3imgui.cpp
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
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_lobby.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_settings.cpp
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_keysetup.cpp
    ${SRC_ROOT}/gui/sdl3/map_preview_popup.cpp
    ${SRC_ROOT}/gui/sdl3/bg_game.c
    ${SRC_ROOT}/gui/sdl3/mapview.c
    ${SRC_ROOT}/gui/sdl3/sprite_positions.c
    ${SRC_ROOT}/gui/sdl3/tileloader.c
    ${SRC_ROOT}/gui/sdl3/sdl3draw.c
    ${SRC_ROOT}/gui/sdl3/sdl3draw_status.c
    ${SRC_ROOT}/gui/sdl3/sdl_bmp.c
    ${SRC_ROOT}/gui/sdl3/input.c
    ${SRC_ROOT}/gui/sdl3/input_touch.c
    ${SRC_ROOT}/gui/sdl3/tutorial_text.c
    ${SRC_ROOT}/gui/sdl3/currentgames.c
    ${SRC_ROOT}/gui/sdl3/lang.c
)

# WASM replacement sources
set(WASM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gamefront_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/clientmutex_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dns_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/threads_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/server_stubs_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sound_wasm.c
    ${SRC_ROOT}/steam/steam_wrapper_stub.c
    ${SRC_ROOT}/common/sentry_integration.c
    ${SRC_ROOT}/common/wb_log.c
)

# Per-bot C bindings under brains/<bot>/c/*.c (e.g. NewAutopilot's
# pillcontrib overlay). luabrainshandler.c #includes these headers and
# calls into their per-tick registrations, so they must be compiled in
# and their directories added to the include path.
file(GLOB BOT_C_SOURCES CONFIGURE_DEPENDS "${REPO_ROOT}/brains/*/c/*.c")
set(BOT_C_INCLUDE_DIRS "")
foreach(_bot_c_src ${BOT_C_SOURCES})
    get_filename_component(_bot_c_dir "${_bot_c_src}" DIRECTORY)
    list(APPEND BOT_C_INCLUDE_DIRS "${_bot_c_dir}")
endforeach()
list(REMOVE_DUPLICATES BOT_C_INCLUDE_DIRS)

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
    ${BOT_C_SOURCES}
)

target_include_directories(winbolo PRIVATE
    ${SRC_ROOT}/gui/sdl3
    ${SRC_ROOT}/gui
    ${SRC_ROOT}/bolo
    ${SRC_ROOT}/server
    ${SRC_ROOT}/mapeditor
    ${BOT_C_INCLUDE_DIRS}
)

target_compile_definitions(winbolo PRIVATE
    _GNU_SOURCE
    USE_64WITH32
    WINBOLO_VERSION="1.18"
    BOLO_VERSION_MAJOR=1
    BOLO_VERSION_MINOR=1
    BOLO_VERSION_REVISION=8
)
target_compile_options(winbolo PRIVATE -fno-common -include sys/types.h)
target_link_options(winbolo PRIVATE "LINKER:--allow-multiple-definition")

target_link_libraries(winbolo PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    imgui_backends
    minizip
    lua_static
    nanosvg
    stb_image
    utf8proc
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
    "SHELL:--preload-file ${REPO_ROOT}/brains/NewAutopilot@/Brains/NewAutopilot"
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
