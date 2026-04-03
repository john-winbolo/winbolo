cmake_minimum_required(VERSION 3.15)

# Allow vendored dependencies with older cmake_minimum_required calls
set(CMAKE_POLICY_VERSION_MINIMUM 3.10 CACHE INTERNAL "")

project(winbolo_android C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# Android 15+ requires 16 KB page size aligned ELF segments
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")

# Project root is two levels up from android/app/
set(PROJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# ---------------------------------------------------------------------------
# Dependencies (FetchContent) — same versions as main CMakeLists.txt
# ---------------------------------------------------------------------------

include(FetchContent)

FetchContent_Declare(
    sdl3
    URL      https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.2.tar.gz
    SYSTEM
)
FetchContent_Declare(
    sdl3_ttf
    URL      https://github.com/libsdl-org/SDL_ttf/archive/refs/tags/release-3.2.0.tar.gz
    SYSTEM
)
FetchContent_Declare(
    imgui
    URL      https://github.com/ocornut/imgui/archive/refs/heads/docking.tar.gz
    SYSTEM
)
FetchContent_Declare(
    lua
    URL      https://github.com/walterschell/Lua/archive/refs/tags/v5.4.7.tar.gz
    SYSTEM
)
FetchContent_Declare(
    zlib
    URL      https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
    SYSTEM
)
FetchContent_Declare(
    libmaxminddb
    URL      https://github.com/maxmind/libmaxminddb/archive/refs/tags/1.12.2.tar.gz
    SYSTEM
)

# libmaxminddb: static, no tests/binaries
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(MAXMINDDB_BUILD_BINARIES OFF CACHE BOOL "" FORCE)

# SDL3 on Android MUST be built as a shared library for JNI loading
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)

# SDL3_ttf: disable vendored and harfbuzz, we provide freetype via FetchContent
set(SDLTTF_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)

# Fetch freetype and make it available before SDL3_ttf configures
FetchContent_Declare(
    freetype
    URL      https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz
    SYSTEM
)
FetchContent_MakeAvailable(freetype)

# Set Freetype variables so SDL3_ttf's find_package(Freetype) succeeds
set(FREETYPE_FOUND TRUE)
set(FREETYPE_LIBRARY freetype)
set(FREETYPE_LIBRARIES freetype)
set(FREETYPE_INCLUDE_DIRS "${freetype_SOURCE_DIR}/include")
set(Freetype_FOUND TRUE)

# Also create a FindFreetype.cmake override so find_package picks it up
file(WRITE "${CMAKE_BINARY_DIR}/cmake-modules/FindFreetype.cmake" "
set(FREETYPE_FOUND TRUE)
set(FREETYPE_LIBRARY freetype)
set(FREETYPE_LIBRARIES freetype)
set(FREETYPE_INCLUDE_DIRS \"${freetype_SOURCE_DIR}/include\")
set(Freetype_FOUND TRUE)
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()
")
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_BINARY_DIR}/cmake-modules")

FetchContent_MakeAvailable(sdl3 sdl3_ttf imgui lua zlib libmaxminddb)

# ---------------------------------------------------------------------------
# minizip (from zlib contrib)
# ---------------------------------------------------------------------------

add_library(minizip STATIC
    ${zlib_SOURCE_DIR}/contrib/minizip/zip.c
    ${zlib_SOURCE_DIR}/contrib/minizip/unzip.c
    ${zlib_SOURCE_DIR}/contrib/minizip/ioapi.c
)
target_include_directories(minizip PUBLIC ${zlib_SOURCE_DIR}/contrib/minizip)
target_link_libraries(minizip PUBLIC zlibstatic)

# ---------------------------------------------------------------------------
# nanosvg — header-only SVG parser/rasterizer (vendored)
# ---------------------------------------------------------------------------

add_library(nanosvg STATIC ${PROJ_ROOT}/src/third_party/nanosvg/nanosvg_impl.c)
target_include_directories(nanosvg PUBLIC ${PROJ_ROOT}/src/third_party/nanosvg)
target_compile_options(nanosvg PRIVATE -Wno-unused-function)

# ---------------------------------------------------------------------------
# stb_image — header-only image loader (vendored)
# ---------------------------------------------------------------------------

add_library(stb_image STATIC ${PROJ_ROOT}/src/third_party/stb/stb_image_impl.c)
target_include_directories(stb_image PUBLIC ${PROJ_ROOT}/src/third_party/stb)
target_compile_options(stb_image PRIVATE -Wno-unused-function)

# ---------------------------------------------------------------------------
# ImGui backends (SDL3 + SDL Renderer)
# ---------------------------------------------------------------------------

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

# ---------------------------------------------------------------------------
# Main shared library (SDL3 on Android expects libmain.so)
# ---------------------------------------------------------------------------

set(ANDROID_SOURCES
    # Android entry point and frontend
    ${PROJ_ROOT}/src/android/main_android.c
    ${PROJ_ROOT}/src/android/android_frontend.c
    ${PROJ_ROOT}/src/android/touch_input.c
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_welcome.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_gamesetup.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_mapchooser.cpp
    ${PROJ_ROOT}/src/android/players_panel.cpp
    ${PROJ_ROOT}/src/android/winbolonet_stub.c
    ${PROJ_ROOT}/src/android/android_dialog_stubs.c

    # Bolo engine (same as CLIENT_SOURCES in main CMakeLists.txt)
    ${PROJ_ROOT}/src/bolo/allience.c
    ${PROJ_ROOT}/src/bolo/bases.c
    ${PROJ_ROOT}/src/bolo/bolo_map.c
    ${PROJ_ROOT}/src/bolo/building.c
    ${PROJ_ROOT}/src/bolo/crc.c
    ${PROJ_ROOT}/src/bolo/debug_file_output.c
    ${PROJ_ROOT}/src/bolo/net_packet_debug.c
    ${PROJ_ROOT}/src/bolo/explosions.c
    ${PROJ_ROOT}/src/bolo/floodfill.c
    ${PROJ_ROOT}/src/bolo/gametype.c
    ${PROJ_ROOT}/src/bolo/global.c
    ${PROJ_ROOT}/src/bolo/grass.c
    ${PROJ_ROOT}/src/bolo/labels.c
    ${PROJ_ROOT}/src/bolo/lgm.c
    ${PROJ_ROOT}/src/bolo/log.c
    ${PROJ_ROOT}/src/bolo/mathWinbolo.c
    ${PROJ_ROOT}/src/bolo/messages.c
    ${PROJ_ROOT}/src/bolo/mines.c
    ${PROJ_ROOT}/src/bolo/minesexp.c
    ${PROJ_ROOT}/src/bolo/gui_message.c
    ${PROJ_ROOT}/src/bolo/pillbox.c
    ${PROJ_ROOT}/src/bolo/players.c
    ${PROJ_ROOT}/src/bolo/playersrejoin.c
    ${PROJ_ROOT}/src/bolo/rubble.c
    ${PROJ_ROOT}/src/bolo/screen.c
    ${PROJ_ROOT}/src/bolo/screenbrainmap.c
    ${PROJ_ROOT}/src/bolo/screenbullet.c
    ${PROJ_ROOT}/src/bolo/screencalc.c
    ${PROJ_ROOT}/src/bolo/screenlgm.c
    ${PROJ_ROOT}/src/bolo/screentank.c
    ${PROJ_ROOT}/src/bolo/scroll.c
    ${PROJ_ROOT}/src/bolo/shells.c
    ${PROJ_ROOT}/src/bolo/sounddist.c
    ${PROJ_ROOT}/src/bolo/starts.c
    ${PROJ_ROOT}/src/bolo/swamp.c
    ${PROJ_ROOT}/src/bolo/tank.c
    ${PROJ_ROOT}/src/bolo/tankexp.c
    ${PROJ_ROOT}/src/bolo/treegrow.c
    ${PROJ_ROOT}/src/bolo/udppackets.c
    ${PROJ_ROOT}/src/bolo/util.c
    ${PROJ_ROOT}/src/bolo/braincore.c
    ${PROJ_ROOT}/src/bolo/brain_pathfinder.c
    ${PROJ_ROOT}/src/bolo/brain_worldsim.c
    ${PROJ_ROOT}/src/bolo/transport_local.c
    ${PROJ_ROOT}/src/bolo/transport_udp.c
    ${PROJ_ROOT}/src/bolo/client_state.c
    ${PROJ_ROOT}/src/bolo/interpolation.c
    ${PROJ_ROOT}/src/bolo/client_sim.c
    ${PROJ_ROOT}/src/bolo/bot_manager.c
    ${PROJ_ROOT}/src/bolo/discovery.c

    # SDL3 GUI (rendering, sound, input, etc.)
    ${PROJ_ROOT}/src/gui/ui_mode.c
    ${PROJ_ROOT}/src/gui/sdl3/clientmutex.c
    ${PROJ_ROOT}/src/gui/sdl3/sdl3imgui.cpp
    ${PROJ_ROOT}/src/gui/sdl3/sdl3imgui_tablet.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialog_backend.c
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_udpsetup.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_gamebrowser.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_setname.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_skins.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_messagebox.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_lobby.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_settings.cpp
    ${PROJ_ROOT}/src/gui/sdl3/luabrainshandler.c
    ${PROJ_ROOT}/src/gui/sdl3/cursor.c
    ${PROJ_ROOT}/src/gui/sdl3/flags.c
    ${PROJ_ROOT}/src/gui/sdl3/dns_lookups.c
    ${PROJ_ROOT}/src/gui/sdl3/gamefront.c
    ${PROJ_ROOT}/src/gui/sdl3/bg_game.c
    ${PROJ_ROOT}/src/gui/sdl3/mapview.c
    ${PROJ_ROOT}/src/gui/sdl3/sdl3draw.c
    ${PROJ_ROOT}/src/gui/sdl3/tileloader.c
    ${PROJ_ROOT}/src/gui/skins.c
    ${PROJ_ROOT}/src/gui/sdl3/input.c
    ${PROJ_ROOT}/src/gui/sdl3/input_touch.c
    ${PROJ_ROOT}/src/gui/sdl3/currentgames.c
    ${PROJ_ROOT}/src/gui/sdl3/lang.c
    ${PROJ_ROOT}/src/gui/sdl3/sound.c

    # LZW compression
    ${PROJ_ROOT}/src/lzw/ecodlzw.c
    ${PROJ_ROOT}/src/lzw/dcodlzw.c

    # Server (for single-player)
    ${PROJ_ROOT}/src/server/geolookup.c
    ${PROJ_ROOT}/src/server/servermessages.c
    ${PROJ_ROOT}/src/server/threads.c
    ${PROJ_ROOT}/src/server/server_sim.c
    ${PROJ_ROOT}/src/server/server_stubs.c
    ${PROJ_ROOT}/src/server/posix_stubs.c
)

add_library(main SHARED ${ANDROID_SOURCES})

target_include_directories(main PRIVATE
    ${PROJ_ROOT}/src/bolo
    ${PROJ_ROOT}/src/gui/sdl3
    ${PROJ_ROOT}/src/gui/sdl3/dialogs
    ${PROJ_ROOT}/src/gui
    ${PROJ_ROOT}/src/server
)

target_link_libraries(main PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    imgui_backends
    minizip
    nanosvg
    stb_image
    lua_static
    zlibstatic
    maxminddb
    m
    log
    android
)

target_compile_definitions(main PRIVATE
    HAVE_SCREEN_C=1
    HAVE_MAXMINDDB=1
    WINBOLO_VERSION="1.18"
    BOLO_VERSION_MAJOR=1
    BOLO_VERSION_MINOR=1
    BOLO_VERSION_REVISION=8
)

target_compile_options(main PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-implicit-function-declaration
    -Wno-nonportable-include-path
    -fcommon
)
