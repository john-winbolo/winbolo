cmake_minimum_required(VERSION 3.18)

# Allow vendored dependencies with older cmake_minimum_required calls
set(CMAKE_POLICY_VERSION_MINIMUM 3.10 CACHE INTERNAL "")

project(winbolo_android C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# Android 15+ requires 16 KB page size aligned ELF segments
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")

# Project root is two levels up from android/app/
set(PROJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

include(${PROJ_ROOT}/cmake/bolo_lib.cmake)

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
# utf8proc — Unicode normalization + property tables (vendored, MIT)
# ---------------------------------------------------------------------------

add_library(utf8proc STATIC ${PROJ_ROOT}/src/third_party/utf8proc/utf8proc.c)
target_include_directories(utf8proc PUBLIC ${PROJ_ROOT}/src/third_party/utf8proc)
target_compile_definitions(utf8proc PUBLIC UTF8PROC_STATIC=1)
set_target_properties(utf8proc PROPERTIES POSITION_INDEPENDENT_CODE ON)

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

# bolo_static covers the bulk of the bolo source set; only the
# HAVE_STEAM-divergent transport_udp_client.c stays per-target here.
# client_mapload.c and nat_portmap.c are not referenced in this build —
# android calls into the sim through its own android_gamesetup path
# and has no NAT-PMP / UPnP wrapper.
set(WINBOLO_VERSION "1.18")
set(BOLO_VERSION_MAJOR 1)
set(BOLO_VERSION_MINOR 1)
set(BOLO_VERSION_REVISION 8)
add_subdirectory(${PROJ_ROOT}/src/bolo ${CMAKE_BINARY_DIR}/bolo_static)
add_subdirectory(${PROJ_ROOT}/src/server ${CMAKE_BINARY_DIR}/server_static)
add_subdirectory(${PROJ_ROOT}/brains ${CMAKE_BINARY_DIR}/bot_brains_static)

set(ANDROID_SOURCES
    # Android entry point and frontend
    ${PROJ_ROOT}/src/android/main_android.c
    ${PROJ_ROOT}/src/android/android_frontend.c
    ${PROJ_ROOT}/src/android/touch_input.c
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_welcome.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_mapchooser.cpp
    ${PROJ_ROOT}/src/android/players_panel.cpp
    ${PROJ_ROOT}/src/android/winbolonet_stub.c
    ${PROJ_ROOT}/src/android/android_dialog_stubs.c
    ${PROJ_ROOT}/src/steam/steam_wrapper_stub.c

    # Per-target bolo source (HAVE_STEAM-divergent).
    ${PROJ_ROOT}/src/bolo/transport_udp_client.c

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
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_keysetup.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_winbolonet.cpp
    ${PROJ_ROOT}/src/gui/sdl3/map_preview_popup.cpp
    ${PROJ_ROOT}/src/gui/sdl3/map_preview_view.cpp
    ${PROJ_ROOT}/src/gui/sdl3/minimap_render.c
    ${PROJ_ROOT}/src/gui/sdl3/luabrainshandler.c
    ${PROJ_ROOT}/src/gui/sdl3/cursor.c
    ${PROJ_ROOT}/src/gui/sdl3/flags.c
    ${PROJ_ROOT}/src/gui/sdl3/dns_lookups.c
    ${PROJ_ROOT}/src/gui/sdl3/gamefront.c
    ${PROJ_ROOT}/src/gui/sdl3/bg_game.c
    ${PROJ_ROOT}/src/gui/sdl3/mapview.c
    ${PROJ_ROOT}/src/gui/sdl3/sprite_positions.c
    ${PROJ_ROOT}/src/gui/sdl3/sdl3draw.c
    ${PROJ_ROOT}/src/gui/sdl3/sdl3draw_status.c
    ${PROJ_ROOT}/src/gui/sdl3/tileloader.c
    ${PROJ_ROOT}/src/gui/sdl3/sdl_bmp.c
    ${PROJ_ROOT}/src/gui/skins.c
    ${PROJ_ROOT}/src/gui/sdl3/input.c
    ${PROJ_ROOT}/src/gui/sdl3/input_touch.c
    ${PROJ_ROOT}/src/gui/sdl3/tutorial_text.c
    ${PROJ_ROOT}/src/gui/sdl3/currentgames.c
    ${PROJ_ROOT}/src/gui/sdl3/lang.c
    ${PROJ_ROOT}/src/gui/sdl3/sound.c

    # Shared map-gen controls used by the map chooser dialog (not the
    # full map editor — this file matches what the iOS build pulls in).
    ${PROJ_ROOT}/src/mapeditor/mapgen_imgui.cpp

    # LZW compression
    ${PROJ_ROOT}/src/lzw/ecodlzw.c
    ${PROJ_ROOT}/src/lzw/dcodlzw.c

    # Server (for single-player). server_sim.c + servermessages.c
    # come from server_sim_static; threads.c comes from threads_static.
    ${PROJ_ROOT}/src/server/geolookup.c
    ${PROJ_ROOT}/src/server/server_stubs.c
    ${PROJ_ROOT}/src/server/server_dedicated_log_stubs.c
    ${PROJ_ROOT}/src/server/posix_stubs.c
)

add_library(main SHARED ${ANDROID_SOURCES})

target_include_directories(main PRIVATE
    ${PROJ_ROOT}/src/gui/sdl3
    ${PROJ_ROOT}/src/gui/sdl3/dialogs
    ${PROJ_ROOT}/src/gui
    ${PROJ_ROOT}/src/server
)
bolo_apply_include_rules(main gui)
bolo_grant_internal_source_access(main
    ${PROJ_ROOT}/src/bolo/transport_udp_client.c)

target_link_libraries(main PRIVATE bolo_static server_sim_static bot_brains_static threads_static)
target_link_libraries(main PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    imgui_backends
    minizip
    nanosvg
    stb_image
    utf8proc
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
