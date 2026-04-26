cmake_minimum_required(VERSION 3.28)

# Allow vendored dependencies with older cmake_minimum_required calls
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")

# CMP0168 (CMake 3.30+): Use direct population for FetchContent instead of a
# sub-build.  The sub-build approach fails with the Xcode generator when
# cross-compiling for iOS because the populate target inherits iOS toolchain
# settings and cannot run the download scripts.
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()

# Use vcpkg if installed at C:/vcpkg and no toolchain file was specified
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

project(winbolo C)

# Enable Objective-C for macOS pinch-to-zoom support (not iOS)
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJC)
endif()

# On macOS, ensure CMAKE_OSX_SYSROOT is set so that dependencies (e.g.
# crashpad) can find system .defs files under the SDK path instead of the
# non-existent /usr/include.
if(APPLE AND NOT CMAKE_OSX_SYSROOT)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE CMAKE_OSX_SYSROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

set(WINBOLO_VERSION "1.18" CACHE STRING "WinBolo version number")

# Derive network protocol version bytes from WINBOLO_VERSION.
# Each digit of the version string (skipping dots) becomes one byte.
# E.g. "1.18" -> MAJOR=0x01, MINOR=0x01, REVISION=0x08.
string(REPLACE "." "" _VER_DIGITS "${WINBOLO_VERSION}")
string(SUBSTRING "${_VER_DIGITS}" 0 1 _VER_D0)
string(SUBSTRING "${_VER_DIGITS}" 1 1 _VER_D1)
string(SUBSTRING "${_VER_DIGITS}" 2 1 _VER_D2)
math(EXPR BOLO_VERSION_MAJOR "${_VER_D0}")
math(EXPR BOLO_VERSION_MINOR "${_VER_D1}")
math(EXPR BOLO_VERSION_REVISION "${_VER_D2}")

set(CMAKE_C_STANDARD 99)

# On Linux, embed $ORIGIN in RPATH so installed/dist binaries find .so files in
# the same directory, while keeping the default build-tree RPATH for development.
if(NOT WIN32 AND NOT APPLE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
endif()
# On macOS (not iOS), use @executable_path so binaries find .dylib files next to them.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_INSTALL_RPATH "@executable_path")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
endif()

# On Linux, libm is a separate library that must be linked explicitly.
# On macOS/iOS it is part of libSystem (always linked), so adding -lm is
# redundant and causes duplicate-library linker warnings.
if(UNIX AND NOT APPLE)
    set(MATH_LIB m)
else()
    set(MATH_LIB)
endif()

# ---------------------------------------------------------------------------
# Dependencies (FetchContent)
# ---------------------------------------------------------------------------

include(FetchContent)
FetchContent_Declare(
    sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.2
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    sdl3_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG        release-3.2.0
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/walterschell/Lua.git
    GIT_TAG        v5.4.7
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    libmaxminddb
    GIT_REPOSITORY https://github.com/maxmind/libmaxminddb.git
    GIT_TAG        1.12.2
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
# curl is only built from source on Windows (uses Schannel for SSL).
# On Linux/macOS, the system libcurl is used via find_package().
if(WIN32)
    FetchContent_Declare(
        curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG        curl-8_12_1
        GIT_SHALLOW    TRUE
        SYSTEM
        EXCLUDE_FROM_ALL
    )
endif()

# Skip SDL test/example targets — we only need the library
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)

# iOS: only build static SDL libraries (shared libs require code signing
# and are not useful in an app bundle)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_LIBC ON CACHE BOOL "" FORCE)

    # CMake's try_compile/check_symbol_exists probes each launch a full
    # xcodebuild invocation on the Xcode generator, making configure
    # extremely slow.  Temporarily switch probes to STATIC_LIBRARY mode
    # which uses a simple compile check instead, then restore after all
    # dependency fetches are done.
    set(_saved_try_compile_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
endif()

# Fetch SDL first (needed by everything)
FetchContent_MakeAvailable(sdl3)

# Lua — only build the static library, skip interpreter/compiler/tests
set(LUA_BUILD_BINARY OFF CACHE BOOL "" FORCE)
set(LUA_BUILD_COMPILER OFF CACHE BOOL "" FORCE)
set(LUA_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lua)
# lua_static must be built with -fPIC on Linux so it can link into shared libs (winbolo_gym)
set_target_properties(lua_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_compile_definitions(lua_internal INTERFACE LUA_USE_IOS)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # iOS: vendored freetype, no harfbuzz (avoids CoreText linking issues)
    # Static only — shared libs require code signing in Xcode
    # Point SDL3_ttf's find_package(SDL3) at the in-tree build so it can
    # locate the static-only SDL3 we just built.
    set(SDL3_DIR "${sdl3_BINARY_DIR}" CACHE PATH "" FORCE)
    set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
    set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
    set(SDLTTF_SHARED OFF CACHE BOOL "" FORCE)
    set(SDLTTF_STATIC ON CACHE BOOL "" FORCE)
    # SDL3_ttf reads BUILD_SHARED_LIBS (not SDLTTF_SHARED) to decide whether
    # to look for SDL3::SDL3-shared.  Force it OFF so it uses the static SDL3.
    set(_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl3_ttf imgui)
    set(BUILD_SHARED_LIBS "${_saved_build_shared_libs}" CACHE BOOL "" FORCE)
    # Restore try_compile mode now that all probing dependencies are fetched
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_saved_try_compile_type}")
else()
    # MinGW (MSYS2): vendor freetype so we don't depend on vcpkg or system packages
    if(MINGW)
        set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
        set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
    endif()
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl3_ttf imgui zlib)
endif()

# MinGW debug builds: HarfBuzz's single-file amalgamation overflows COFF debug
# section limits. Use -Wa,-mbig-obj so the assembler emits PE-COFF bigobj format.
if(MINGW AND TARGET harfbuzz)
    target_compile_options(harfbuzz PRIVATE -Wa,-mbig-obj)
endif()

# On Unix (non-iOS), shared libraries have a versioned filename (e.g. libSDL3_ttf.0.2.0.dylib)
# and a soname symlink (libSDL3_ttf.0.dylib).  Setting VERSION = SOVERSION makes the real
# filename match the soname so copies just work.
if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    foreach(_lib SDL3_ttf-shared SDL3-shared)
        if(TARGET ${_lib})
            get_target_property(_sover ${_lib} SOVERSION)
            if(NOT _sover STREQUAL "_sover-NOTFOUND")
                set_target_properties(${_lib} PROPERTIES VERSION "${_sover}")
            endif()
        endif()
    endforeach()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # Build libmaxminddb as static to avoid needing a separate DLL
    set(BUILD_SHARED_LIBS_SAVE "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(MAXMINDDB_BUILD_BINARIES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libmaxminddb)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVE}" CACHE BOOL "" FORCE)

    # cJSON — force static
    set(BUILD_SHARED_LIBS_SAVE2 "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_INSTALL OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_UNINSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cjson)
    # cJSON enables -Werror which breaks on MinGW (isnan/isinf float-conversion warnings)
    if(TARGET cjson AND MINGW)
        target_compile_options(cjson PRIVATE -Wno-error)
    endif()
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVE2}" CACHE BOOL "" FORCE)

    # minizip library from zlib's contrib/minizip
    add_library(minizip STATIC
        ${zlib_SOURCE_DIR}/contrib/minizip/zip.c
        ${zlib_SOURCE_DIR}/contrib/minizip/unzip.c
        ${zlib_SOURCE_DIR}/contrib/minizip/ioapi.c
    )
    target_include_directories(minizip PUBLIC ${zlib_SOURCE_DIR}/contrib/minizip)
    target_link_libraries(minizip PUBLIC zlibstatic)
endif()

# nanosvg — header-only SVG parser/rasterizer (vendored)
add_library(nanosvg STATIC src/third_party/nanosvg/nanosvg_impl.c)
target_include_directories(nanosvg SYSTEM PUBLIC src/third_party/nanosvg)
if(MSVC)
    target_compile_options(nanosvg PRIVATE /wd4201 /wd4244 /wd4267)
else()
    target_compile_options(nanosvg PRIVATE -Wno-unused-function)
endif()

# stb_image — header-only image loader (vendored)
add_library(stb_image STATIC src/third_party/stb/stb_image_impl.c)
target_include_directories(stb_image SYSTEM PUBLIC src/third_party/stb)
if(NOT MSVC)
    target_compile_options(stb_image PRIVATE -Wno-unused-function)
endif()

# ImGui backends library for SDL3 + SDL Renderer
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

# Enable C++ for ImGui
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)

# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------

if(MSVC)
    # COMPILE_LANGUAGE guard prevents these C/C++ flags from being passed
    # to MASM (ml64.exe) for crashpad's .asm files.
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:/WX> $<$<COMPILE_LANGUAGE:C,CXX>:/sdl> $<$<COMPILE_LANGUAGE:C,CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:C,CXX>:/W4> $<$<COMPILE_LANGUAGE:C,CXX>:/external:W3>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4005> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4013>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4018> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4057>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4100> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4101>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4127> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4131>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4132> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4146>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4152> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4189>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4244> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4245>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4295> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4310>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4456> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4457>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4459> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4505>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4554> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4701>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4702> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4996>
        $<$<COMPILE_LANGUAGE:C,CXX>:/guard:cf> $<$<COMPILE_LANGUAGE:C,CXX>:/GL>
        $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Debug>>:/RTC1>
    )
    add_link_options(/GUARD:CF /INCREMENTAL:NO /LTCG)
else()
    add_compile_options(
        -Wall -Wextra
        -Wno-unused-parameter
        -Wno-sign-compare
        -Wno-implicit-fallthrough
    )
endif()

# ---------------------------------------------------------------------------
# Flat output directories (no /Debug, /Release subdirs under MSVC)
# ---------------------------------------------------------------------------

function(set_flat_output TARGET OUTDIR)
    set_target_properties(${TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${OUTDIR}"
        LIBRARY_OUTPUT_DIRECTORY "${OUTDIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${OUTDIR}"
    )
    if(CMAKE_CONFIGURATION_TYPES)
        foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
            string(TOUPPER "${cfg}" CFGU)
            set_target_properties(${TARGET} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY_${CFGU} "${OUTDIR}"
                LIBRARY_OUTPUT_DIRECTORY_${CFGU} "${OUTDIR}"
                ARCHIVE_OUTPUT_DIRECTORY_${CFGU} "${OUTDIR}"
            )
        endforeach()
    endif()
endfunction()

# ===========================================================================
# Platform targets: iOS vs Desktop
# ===========================================================================

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")

# ---------------------------------------------------------------------------
# WinBolo iOS Client
# ---------------------------------------------------------------------------

enable_language(OBJC)

set(IOS_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/gui/ios/Info.plist")

# App icon asset catalog
set(IOS_ASSET_CATALOG "${CMAKE_SOURCE_DIR}/src/gui/ios/Assets.xcassets")

# Data files to bundle
# Data files that go into the data/ subdirectory of the bundle
set(IOS_DATA_BMP
    ${CMAKE_SOURCE_DIR}/data/background.bmp
    ${CMAKE_SOURCE_DIR}/data/skin.bmp
    ${CMAKE_SOURCE_DIR}/data/tile.bmp
    ${CMAKE_SOURCE_DIR}/data/tanks.bmp
    ${CMAKE_SOURCE_DIR}/data/boats.bmp
    ${CMAKE_SOURCE_DIR}/data/items.bmp
    ${CMAKE_SOURCE_DIR}/data/splash.bmp
    ${CMAKE_SOURCE_DIR}/data/logo.bmp
    ${CMAKE_SOURCE_DIR}/data/base.bmp
    ${CMAKE_SOURCE_DIR}/data/pillbox.bmp
)
# Data files that go into the root of the bundle (next to the executable)
set(IOS_DATA_ROOT
    ${CMAKE_SOURCE_DIR}/data/smalllogo.bmp
    ${CMAKE_SOURCE_DIR}/data/smalllogo-transparent.png
    ${CMAKE_SOURCE_DIR}/data/button_practice.bmp
    ${CMAKE_SOURCE_DIR}/data/button_tcp.bmp
    ${CMAKE_SOURCE_DIR}/data/button_lan.bmp
    ${CMAKE_SOURCE_DIR}/data/button_internet.bmp
)
# Maps that go into the root (Everard Island for single-player)
set(IOS_DATA_ROOT_MAPS
    "${CMAKE_SOURCE_DIR}/data/maps/Everard Island.map"
    "${CMAKE_SOURCE_DIR}/data/maps/Inbuilt Tutorial.map"
)
set(IOS_DATA_FONT
    ${CMAKE_SOURCE_DIR}/data/fonts/CourierPrime-Regular.ttf
)
file(GLOB IOS_DATA_MAPS "${CMAKE_SOURCE_DIR}/data/maps/*.map")
file(GLOB IOS_DATA_SOUNDS "${CMAKE_SOURCE_DIR}/data/sounds/*.wav")
file(GLOB IOS_DATA_SVG "${CMAKE_SOURCE_DIR}/data/svg/*.png" "${CMAKE_SOURCE_DIR}/data/svg/*.svg")
file(GLOB IOS_DATA_UI "${CMAKE_SOURCE_DIR}/data/ui/*.svg")
file(GLOB IOS_LANG_FILES "${CMAKE_SOURCE_DIR}/data/lang/*.txt")

# Bolo engine sources (matching desktop CLIENT_SOURCES)
set(IOS_BOLO_SOURCES
    src/bolo/allience.c src/bolo/bases.c src/bolo/bolo_map.c
    src/bolo/braincore.c src/bolo/building.c src/bolo/client_sim.c src/bolo/client_state.c
    src/bolo/crc.c src/bolo/debug_file_output.c src/bolo/net_packet_debug.c
    src/bolo/explosions.c src/bolo/floodfill.c src/bolo/gametype.c src/bolo/global.c
    src/bolo/grass.c src/bolo/interpolation.c src/bolo/position_history.c src/bolo/labels.c src/bolo/lgm.c
    src/bolo/mathWinbolo.c src/bolo/messages.c src/bolo/mines.c src/bolo/minesexp.c
    src/bolo/gui_message.c
    src/bolo/pillbox.c src/bolo/players.c src/bolo/playersrejoin.c src/bolo/rubble.c
    src/bolo/screen.c src/bolo/screenbrainmap.c src/bolo/screenbullet.c
    src/bolo/screencalc.c src/bolo/screenlgm.c src/bolo/screentank.c src/bolo/scroll.c src/bolo/scroll_item_list.c
    src/bolo/shells.c src/bolo/sounddist.c src/bolo/starts.c src/bolo/swamp.c
    src/bolo/tank.c src/bolo/tankexp.c src/bolo/tutorial.c src/bolo/transport_local.c src/bolo/transport_udp_common.c src/bolo/transport_udp_client.c src/bolo/transport_udp_server.c
    src/bolo/treegrow.c src/bolo/udppackets.c src/bolo/util.c
    src/bolo/bot_manager.c
    src/bolo/brain_pathfinder.c src/bolo/brain_worldsim.c
    src/bolo/discovery.c
    src/bolo/obs_builder.c
)

# Server sources needed for single-player mode
set(IOS_SERVER_SOURCES
    src/server/server_sim.c src/server/servermessages.c
    src/server/server_stubs.c
    src/server/threads.c
    src/mapeditor/mapeditor_generate.c src/mapeditor/mapeditor_maze.c
    src/mapeditor/mapgen_imgui.cpp
)



# GUI/support sources (reused from sdl3 — matches desktop CLIENT_SOURCES minus winbolo.c)
set(IOS_GUI_SOURCES
    src/gui/sdl3/clientmutex.c
    src/gui/sdl3/sdl3imgui.cpp
    src/gui/sdl3/sdl3imgui_tablet.cpp
    src/gui/sdl3/sdl3draw.c
    src/gui/sdl3/mapview.c
    src/gui/sdl3/tileloader.c
    src/gui/sdl3/sound.c
    src/gui/sdl3/lang.c
    src/gui/sdl3/currentgames.c
    src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/bg_game.c
    src/gui/sdl3/minimap_render.c
    src/gui/sdl3/flags.c
    src/gui/sdl3/dns_lookups.c
    src/gui/sdl3/input.c
    src/gui/sdl3/input_touch.c
    src/gui/sdl3/tutorial_text.c
    src/gui/ui_mode.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/gui/sdl3/dialogs/imgui_welcome.cpp
    src/gui/sdl3/dialogs/imgui_gamesetup.cpp
    src/gui/sdl3/dialogs/imgui_mapchooser.cpp
    src/gui/sdl3/dialogs/imgui_udpsetup.cpp
    src/gui/sdl3/dialogs/imgui_gamebrowser.cpp
    src/gui/sdl3/dialogs/imgui_setname.cpp
    src/gui/sdl3/dialogs/imgui_skins.cpp
    src/gui/sdl3/dialogs/imgui_trackersetup.cpp
    src/gui/sdl3/dialogs/imgui_winbolonet.cpp
    src/gui/sdl3/dialogs/imgui_messagebox.cpp
    src/gui/sdl3/dialogs/imgui_lobby.cpp
    src/gui/sdl3/dialogs/imgui_settings.cpp
    src/gui/sdl3/dialogs/imgui_keysetup.cpp
    src/gui/sdl3/dialogs/imgui_wbn_browser.cpp
    src/gui/sdl3/map_preview_popup.cpp
)

# Shared mobile UI (touch input, legacy players panel for non-tablet mode)
set(IOS_MOBILE_SOURCES
    src/gui/mobile/touch_input.c
    src/gui/mobile/players_panel.cpp
)

# Lua brain scripts to bundle
file(GLOB IOS_BRAIN_SCRIPTS "${CMAKE_SOURCE_DIR}/brains/NewAutopilot/*.lua")


add_executable(WinBoloIOS MACOSX_BUNDLE
    src/gui/ios/winbolo_ios.m
    src/gui/ios/ios_stubs.c
    src/gui/ios/imgui_ios.cpp
    src/gui/sdl3/gamefront.c
    src/gui/sdl3/dialog_backend.c
    ${IOS_BOLO_SOURCES}
    ${IOS_SERVER_SOURCES}
    ${IOS_GUI_SOURCES}
    ${IOS_MOBILE_SOURCES}
    ${IOS_INFO_PLIST}
    ${IOS_DATA_BMP}
    ${IOS_DATA_ROOT}
    ${IOS_DATA_ROOT_MAPS}
    ${IOS_DATA_MAPS}
    ${IOS_DATA_FONT}
    ${IOS_BRAIN_SCRIPTS}
    ${IOS_DATA_SOUNDS}
    ${IOS_DATA_SVG}
    ${IOS_DATA_UI}
    ${IOS_LANG_FILES}
    ${IOS_ASSET_CATALOG}
)

# Info.plist is used by CMake for bundle metadata, not compiled — mark as header
set_source_files_properties(${IOS_INFO_PLIST} PROPERTIES
    HEADER_FILE_ONLY TRUE
)

# Place data files in the app bundle
set_source_files_properties(${IOS_DATA_BMP} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data"
)
set_source_files_properties(${IOS_DATA_FONT} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data"
)
set_source_files_properties(${IOS_DATA_MAPS} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/maps"
)
set_source_files_properties(${IOS_BRAIN_SCRIPTS} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Brains/NewAutopilot"
)
set_source_files_properties(${IOS_DATA_SOUNDS} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/sounds"
)
set_source_files_properties(${IOS_DATA_SVG} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/svg"
)
set_source_files_properties(${IOS_DATA_UI} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/ui"
)
set_source_files_properties(${IOS_LANG_FILES} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/lang"
)
set_source_files_properties(${IOS_DATA_ROOT} PROPERTIES
    MACOSX_PACKAGE_LOCATION ""
)
set_source_files_properties(${IOS_DATA_ROOT_MAPS} PROPERTIES
    MACOSX_PACKAGE_LOCATION ""
)
set_source_files_properties(${IOS_ASSET_CATALOG} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

# Organize in Xcode sidebar
source_group("Resources" FILES ${IOS_INFO_PLIST} ${IOS_DATA_BMP} ${IOS_DATA_FONT} ${IOS_DATA_MAPS} ${IOS_DATA_SOUNDS} ${IOS_DATA_SVG} ${IOS_DATA_UI})
source_group("Brains" FILES ${IOS_BRAIN_SCRIPTS})
source_group("Bolo Engine" FILES ${IOS_BOLO_SOURCES})
source_group("Server" FILES ${IOS_SERVER_SOURCES})
source_group("GUI" FILES ${IOS_GUI_SOURCES})
source_group("Mobile" FILES ${IOS_MOBILE_SOURCES})
source_group("iOS" FILES
    src/gui/ios/winbolo_ios.m
    src/gui/ios/ios_stubs.c
    src/gui/ios/imgui_ios.cpp
    src/gui/ios/gamefront_ios.c
)

set_target_properties(WinBoloIOS PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${IOS_INFO_PLIST}"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.winbolo.ios"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_INCLUDE_ALL_APPICON_ASSETS "YES"
)

target_include_directories(WinBoloIOS PRIVATE
    src/gui/sdl3
    src/gui
    src/gui/ios
    src/gui/mobile
    src/bolo
    src/server
    src/mapeditor
)

target_link_libraries(WinBoloIOS PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui_backends lua_static
    nanosvg stb_image
    "-framework CoreText" "-framework CoreFoundation" "-framework CoreGraphics"
    "-framework AVFoundation"
    "-framework GameController"
)

target_compile_options(WinBoloIOS PRIVATE -fcommon)
target_compile_definitions(WinBoloIOS PRIVATE
    HAVE_SCREEN_C=1
    __IPHONEOS__=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

if(HAVE_SENTRY_IOS)
    target_compile_definitions(WinBoloIOS PRIVATE HAVE_SENTRY=1)
    if(SENTRY_DSN)
        target_compile_definitions(WinBoloIOS PRIVATE SENTRY_DSN="${SENTRY_DSN}")
    endif()
    target_link_libraries(WinBoloIOS PRIVATE "${SENTRY_COCOA_XCFW}")
endif()

target_sources(WinBoloIOS PRIVATE src/steam/steam_wrapper_stub.c)
set_flat_output(WinBoloIOS "${CMAKE_BINARY_DIR}")

# Hide dependency targets from the Xcode project — only WinBoloIOS should
# appear as a buildable scheme.
foreach(_t
    SDL3-static SDL3_Headers SDL3_test
    SDL3_ttf-static
    freetype
    lua_static lua_internal
    imgui_backends
    nanosvg stb_image
)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()

else() # NOT iOS — Desktop targets (Windows, Linux, macOS)

# libcurl for WinBolo.net HTTP/HTTPS
if(WIN32)
    # Windows: build curl from source using Schannel (no OpenSSL needed)
    set(BUILD_SHARED_LIBS_SAVE_CURL "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(curl)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVE_CURL}" CACHE BOOL "" FORCE)
    # Provide the CURL::libcurl alias that the rest of the build expects
    if(NOT TARGET CURL::libcurl)
        add_library(CURL::libcurl ALIAS libcurl_shared)
    endif()
else()
    # Linux/macOS: use system-installed libcurl
    find_package(CURL REQUIRED)
endif()

find_package(OpenMP)

# Classic "pillmassage" bug — pillboxes miss tanks at very close range due to
# the original Bolo forward-prediction formula breaking down at short distances.
# Enable for authentic behaviour matching the original game.
option(ENABLE_PILLMASSAGE_BUG "Enable classic Bolo pillmassage close-range miss bug" ON)
if(ENABLE_PILLMASSAGE_BUG)
    add_compile_definitions(ENABLE_PILLMASSAGE_BUG)
endif()

# ONNX Runtime — optional dependency for ML brain inference
# Downloads prebuilt binaries via FetchContent when not already installed.
# Set -DENABLE_ONNXRUNTIME=OFF to skip entirely.
option(ENABLE_ONNXRUNTIME "Enable ONNX Runtime ML brain support" ON)
set(HAVE_ONNXRUNTIME FALSE)

if(ENABLE_ONNXRUNTIME AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # Try system/vcpkg install first
    find_package(onnxruntime QUIET)
    if(onnxruntime_FOUND)
        set(HAVE_ONNXRUNTIME TRUE)
    else()
        find_library(ONNXRUNTIME_LIB onnxruntime)
        find_path(ONNXRUNTIME_INCLUDE onnxruntime_c_api.h PATH_SUFFIXES onnxruntime)
        if(ONNXRUNTIME_LIB AND ONNXRUNTIME_INCLUDE)
            set(HAVE_ONNXRUNTIME TRUE)
            add_library(onnxruntime::onnxruntime UNKNOWN IMPORTED)
            set_target_properties(onnxruntime::onnxruntime PROPERTIES
                IMPORTED_LOCATION "${ONNXRUNTIME_LIB}"
                INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE}"
            )
        endif()
    endif()

    # Fall back to downloading prebuilt release
    if(NOT HAVE_ONNXRUNTIME)
        set(_ORT_VERSION "1.22.0")
        if(WIN32)
            set(_ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${_ORT_VERSION}/onnxruntime-win-x64-${_ORT_VERSION}.zip")
        elseif(APPLE)
            set(_ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${_ORT_VERSION}/onnxruntime-osx-universal2-${_ORT_VERSION}.tgz")
        else()
            set(_ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${_ORT_VERSION}/onnxruntime-linux-x64-${_ORT_VERSION}.tgz")
        endif()

        FetchContent_Declare(
            onnxruntime_prebuilt
            URL "${_ORT_URL}"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(onnxruntime_prebuilt)

        # Find the shared library in the extracted directory
        if(WIN32)
            file(GLOB _ORT_DLL "${onnxruntime_prebuilt_SOURCE_DIR}/lib/onnxruntime.dll")
            file(GLOB _ORT_IMPLIB "${onnxruntime_prebuilt_SOURCE_DIR}/lib/onnxruntime.lib")
            if(_ORT_IMPLIB AND _ORT_DLL)
                add_library(onnxruntime::onnxruntime SHARED IMPORTED)
                set_target_properties(onnxruntime::onnxruntime PROPERTIES
                    IMPORTED_IMPLIB "${_ORT_IMPLIB}"
                    IMPORTED_LOCATION "${_ORT_DLL}"
                    INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_prebuilt_SOURCE_DIR}/include"
                )
                set(HAVE_ONNXRUNTIME TRUE)
                set(ONNXRUNTIME_DLL "${_ORT_DLL}" CACHE INTERNAL "")
            endif()
        else()
            file(GLOB _ORT_SO "${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime${CMAKE_SHARED_LIBRARY_SUFFIX}*")
            list(GET _ORT_SO 0 _ORT_SO_FIRST)
            if(_ORT_SO_FIRST)
                add_library(onnxruntime::onnxruntime SHARED IMPORTED)
                set_target_properties(onnxruntime::onnxruntime PROPERTIES
                    IMPORTED_LOCATION "${_ORT_SO_FIRST}"
                    IMPORTED_NO_SONAME TRUE
                    INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_prebuilt_SOURCE_DIR}/include"
                )
                set(HAVE_ONNXRUNTIME TRUE)
                set(ONNXRUNTIME_LIBDIR "${onnxruntime_prebuilt_SOURCE_DIR}/lib" CACHE INTERNAL "")
            endif()
        endif()
    endif()
endif()

if(HAVE_ONNXRUNTIME)
    message(STATUS "ONNX Runtime found — ML brain support enabled")
else()
    message(STATUS "ONNX Runtime not found — ML brain support disabled")
endif()

# Sentry crash reporting — optional.
# Set -DENABLE_SENTRY=OFF to skip entirely.
# Pass -DSENTRY_DSN="https://key@o0.ingest.sentry.io/0" to enable reporting.
option(ENABLE_SENTRY "Enable Sentry crash reporting" ON)
set(SENTRY_DSN "" CACHE STRING "Sentry DSN for crash reporting")
set(HAVE_SENTRY FALSE)
set(HAVE_SENTRY_IOS FALSE)

if(ENABLE_SENTRY AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT EMSCRIPTEN)
    FetchContent_Declare(
        sentry
        GIT_REPOSITORY https://github.com/getsentry/sentry-native.git
        GIT_TAG        0.7.17
        GIT_SHALLOW    TRUE
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # Crashpad has poor MinGW/GCC 15 support; use inproc backend on MinGW.
    if(MINGW)
        set(SENTRY_BACKEND "inproc" CACHE STRING "" FORCE)
    else()
        set(SENTRY_BACKEND "crashpad" CACHE STRING "" FORCE)
    endif()
    FetchContent_MakeAvailable(sentry)

    set(HAVE_SENTRY TRUE)
endif()

# Sentry crash reporting — iOS (sentry-cocoa static xcframework).
# Requires CMake 3.28+ for xcframework support with the Xcode generator.
if(ENABLE_SENTRY AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
    FetchContent_Declare(
        sentry_cocoa
        URL https://github.com/getsentry/sentry-cocoa/releases/download/9.8.0/Sentry.xcframework.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sentry_cocoa)
    set(SENTRY_COCOA_XCFW "${sentry_cocoa_SOURCE_DIR}/Sentry.xcframework")
    set(HAVE_SENTRY_IOS TRUE)
endif()

if(HAVE_SENTRY)
    message(STATUS "Sentry crash reporting enabled")
else()
    message(STATUS "Sentry crash reporting disabled")
endif()

# ---------------------------------------------------------------------------
# WinBolo Client (cross-platform: Windows, Linux, macOS)
# ---------------------------------------------------------------------------

set(CLIENT_SOURCES
    src/bolo/allience.c src/bolo/bases.c src/bolo/bolo_map.c
    src/bolo/building.c src/bolo/crc.c src/bolo/debug_file_output.c src/bolo/net_packet_debug.c
    src/bolo/explosions.c src/bolo/floodfill.c src/bolo/gametype.c src/bolo/global.c
    src/bolo/grass.c src/bolo/labels.c src/bolo/lgm.c src/bolo/log.c
    src/bolo/mathWinbolo.c src/bolo/messages.c src/bolo/mines.c src/bolo/minesexp.c
    src/bolo/gui_message.c
    src/bolo/pillbox.c src/bolo/players.c src/bolo/playersrejoin.c src/bolo/rubble.c
    src/bolo/screen.c src/bolo/screenbrainmap.c src/bolo/screenbullet.c
    src/bolo/screencalc.c src/bolo/screenlgm.c src/bolo/screentank.c src/bolo/scroll.c src/bolo/scroll_item_list.c
    src/bolo/shells.c src/bolo/sounddist.c src/bolo/starts.c src/bolo/swamp.c
    src/bolo/tank.c src/bolo/tankexp.c src/bolo/tutorial.c src/bolo/treegrow.c src/bolo/udppackets.c
    src/bolo/util.c src/bolo/braincore.c src/bolo/brain_pathfinder.c src/bolo/brain_worldsim.c
    src/gui/ui_mode.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/sdl3imgui.cpp src/gui/sdl3/sdl3imgui_tablet.cpp src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/cursor.c src/gui/sdl3/flags.c src/gui/sdl3/dns_lookups.c
    src/gui/sdl3/winbolo.c src/gui/sdl3/gamefront.c
    src/gui/sdl3/dialog_backend.c src/gui/sdl3/dialogs/imgui_welcome.cpp src/gui/sdl3/dialogs/imgui_gamesetup.cpp src/gui/sdl3/dialogs/imgui_mapchooser.cpp src/gui/sdl3/dialogs/imgui_udpsetup.cpp src/gui/sdl3/dialogs/imgui_gamebrowser.cpp src/gui/sdl3/dialogs/imgui_setname.cpp src/gui/sdl3/dialogs/imgui_skins.cpp src/gui/sdl3/dialogs/imgui_trackersetup.cpp src/gui/sdl3/dialogs/imgui_winbolonet.cpp src/gui/sdl3/dialogs/imgui_messagebox.cpp src/gui/sdl3/dialogs/imgui_lobby.cpp src/gui/sdl3/dialogs/imgui_settings.cpp src/gui/sdl3/dialogs/imgui_keysetup.cpp src/gui/sdl3/dialogs/imgui_wbn_browser.cpp
    src/gui/sdl3/map_preview_popup.cpp
    src/gui/sdl3/minimap_render.c
    src/gui/sdl3/bg_game.c src/gui/sdl3/mapview.c src/gui/sdl3/tileloader.c src/gui/sdl3/sdl3draw.c src/gui/sdl3/input.c src/gui/sdl3/input_touch.c
    src/gui/sdl3/tutorial_text.c
    src/gui/sdl3/currentgames.c src/gui/sdl3/lang.c src/gui/sdl3/sound.c
    src/mapeditor/mapeditor.c src/mapeditor/mapeditor_imgui.cpp src/mapeditor/mapgen_imgui.cpp src/mapeditor/mapeditor_undo.c src/mapeditor/mapeditor_validate.c src/mapeditor/mapeditor_symmetry.c src/mapeditor/mapeditor_generate.c src/mapeditor/mapeditor_maze.c src/mapeditor/mapeditor_text.c src/mapeditor/mapeditor_fonts.c src/mapeditor/mapeditor_stats.c src/mapeditor/mapeditor_image.c src/mapeditor/mapeditor_stamp.c src/mapeditor/mapeditor_export.c
    src/third_party/stb/stb_image_write_impl.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/bolo/transport_local.c src/bolo/transport_udp_common.c src/bolo/transport_udp_client.c src/bolo/transport_udp_server.c src/bolo/client_state.c src/bolo/interpolation.c src/bolo/position_history.c src/bolo/client_sim.c
    src/bolo/discovery.c
    src/bolo/bot_manager.c
    src/bolo/obs_builder.c
    src/server/geolookup.c
    src/server/servermessages.c src/server/threads.c
    src/server/server_sim.c src/server/server_stubs.c
    src/winbolonet/winbolonet.c src/winbolonet/http.c
    src/winbolonet/winbolonetthread.c src/winbolonet/winbolonetevents.c src/third_party/tweetnacl/tweetnacl.c
)


# BSD headers (compiled as .c but really just declarations)
file(GLOB BSD_SOURCES src/bsd/*.c)
list(APPEND CLIENT_SOURCES ${BSD_SOURCES})

# common/ utilities
file(GLOB COMMON_SOURCES src/common/*.c)
list(APPEND CLIENT_SOURCES ${COMMON_SOURCES})
if(NOT WIN32)
    list(FILTER CLIENT_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()

# Logviewer library sources (embedded in WinBolo, excludes standalone main.c)
set(LOGVIEWER_LIB_SOURCES
    src/logviewer/logviewer.c
    src/logviewer/screen.c
    src/logviewer/draw.c
    src/logviewer/sound.c
    src/logviewer/players.c
    src/logviewer/blocks.c
    src/logviewer/shells.c
    src/logviewer/snapshot.c
    src/logviewer/allience.c
    src/logviewer/pillbox.c
    src/logviewer/bases.c
    src/logviewer/bolo_map.c
    src/logviewer/starts.c
    src/logviewer/screencalc.c
    src/logviewer/screentank.c
    src/logviewer/screenlgm.c
    src/logviewer/screenbullet.c
    src/logviewer/sounddist.c
    src/logviewer/clientmutex.c
    src/logviewer/dns.c
    src/logviewer/util.c
    src/logviewer/global.c
    src/logviewer/labels.c
    src/logviewer/messages.c
    src/logviewer/draw_setup_arrays.c
    src/logviewer/imgui/imgui_context.cpp
    src/logviewer/imgui/imgui_main_menu.cpp
    src/logviewer/imgui/imgui_controls.cpp
    src/logviewer/imgui/imgui_game_info.cpp
    src/logviewer/imgui/imgui_events.cpp
    src/logviewer/imgui/imgui_item_info.cpp
    src/logviewer/imgui/imgui_dialogs.cpp
    src/logviewer/imgui/imgui_game_viewport.cpp
    src/logviewer/platform/platform_config.c
    src/logviewer/platform/platform_dialogs.c
)
list(APPEND CLIENT_SOURCES ${LOGVIEWER_LIB_SOURCES})

add_executable(WinBolo ${CLIENT_SOURCES})
target_compile_definitions(WinBolo PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

# macOS trackpad pinch-to-zoom support
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(WinBolo PRIVATE src/gui/sdl3/macos_pinch.m)
    target_link_libraries(WinBolo PRIVATE "-framework AppKit")
endif()

# macOS app bundle with icon
if(APPLE)
    set(WINBOLO_ICON "${CMAKE_SOURCE_DIR}/src/gui/sdl3/platform/AppIcon.icns")
    if(EXISTS "${WINBOLO_ICON}")
        target_sources(WinBolo PRIVATE "${WINBOLO_ICON}")
        set_source_files_properties("${WINBOLO_ICON}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        set_target_properties(WinBolo PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/gui/sdl3/platform/Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.winbolo.macos"
            MACOSX_BUNDLE_BUNDLE_NAME "WinBolo")
    endif()
endif()

# Address Sanitizer — enable to diagnose heap corruption
if(MSVC AND ENABLE_ASAN)
    target_compile_options(WinBolo PRIVATE /fsanitize=address)
    target_link_options(WinBolo PRIVATE /fsanitize=address)
endif()

target_include_directories(WinBolo PRIVATE
    src/gui/sdl3
    src/gui
    src/bolo
    src/server
    src/mapeditor
    src
    src/logviewer
    src/logviewer/imgui
    src/logviewer/platform
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)

if(WIN32)
    target_sources(WinBolo PRIVATE
        src/gui/skins.c
        src/res/winbolo.rc
    )
    set_target_properties(WinBolo PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(WinBolo PRIVATE ws2_32 winmm user32 comctl32)
    target_compile_options(WinBolo PRIVATE -fcommon)
    if(MSVC)
        target_link_options(WinBolo PRIVATE "/SUBSYSTEM:WINDOWS,10.0" "/MANIFEST:NO")
        # Suppress warnings in third-party sources that MSVC treats as errors
        set_source_files_properties(src/third_party/tweetnacl/tweetnacl.c
            PROPERTIES COMPILE_FLAGS "/wd4132 /wd4146 /wd4018 /wd4295")
        # Suppress C4505 (unreferenced static function) in mapgen_imgui.cpp
        set_source_files_properties(src/mapeditor/mapgen_imgui.cpp
            PROPERTIES COMPILE_FLAGS "/wd4505")
    endif()
else()
    target_sources(WinBolo PRIVATE
        src/gui/skins.c
        src/server/posix_stubs.c
    )
    target_link_libraries(WinBolo PRIVATE ${MATH_LIB} pthread)
    target_compile_options(WinBolo PRIVATE -fcommon)
endif()


# IP-to-country geolocation (optional)
if(TARGET maxminddb)
    target_link_libraries(WinBolo PRIVATE maxminddb)
    target_compile_definitions(WinBolo PRIVATE HAVE_MAXMINDDB=1)
endif()

target_link_libraries(WinBolo PRIVATE
    SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui_backends minizip lua_static CURL::libcurl
    nanosvg stb_image cjson)

# ONNX Runtime ML brain support (optional)
if(HAVE_ONNXRUNTIME)
    target_sources(WinBolo PRIVATE src/bolo/ml_brain.c)
    target_compile_definitions(WinBolo PRIVATE HAVE_ONNXRUNTIME=1)
    target_link_libraries(WinBolo PRIVATE onnxruntime::onnxruntime)
endif()

# Sentry crash reporting (always compile — stubs when HAVE_SENTRY is off)
target_sources(WinBolo PRIVATE src/common/sentry_integration.c)
if(HAVE_SENTRY)
    target_compile_definitions(WinBolo PRIVATE HAVE_SENTRY=1)
    if(SENTRY_DSN)
        target_compile_definitions(WinBolo PRIVATE SENTRY_DSN="${SENTRY_DSN}")
    endif()
    target_link_libraries(WinBolo PRIVATE sentry::sentry)
endif()

# Steamworks SDK integration (optional)
set(STEAMWORKS_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/steamworks")
if(EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h")
    add_library(steam_api SHARED IMPORTED)
    set_target_properties(steam_api PROPERTIES
        IMPORTED_LOCATION "${STEAMWORKS_SDK_DIR}/redistributable_bin/linux64/libsteam_api.so"
        IMPORTED_IMPLIB "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${STEAMWORKS_SDK_DIR}/public"
    )
    target_sources(WinBolo PRIVATE src/steam/steam_wrapper.cpp)
    target_compile_definitions(WinBolo PRIVATE HAVE_STEAM=1)
    target_link_libraries(WinBolo PRIVATE steam_api)
    message(STATUS "Steamworks SDK found — Steam integration enabled")
else()
    target_sources(WinBolo PRIVATE src/steam/steam_wrapper_stub.c)
    message(STATUS "Steamworks SDK not found — Steam integration disabled (stubs)")
endif()

# Copy data assets and brain scripts to the correct location.
# On macOS bundles, SDL_GetBasePath() returns Contents/Resources/, so assets go there.
# On Windows/Linux, they go next to the executable.
if(APPLE)
    set(WINBOLO_DATA_DIR "$<TARGET_FILE_DIR:WinBolo>/../Resources")
else()
    set(WINBOLO_DATA_DIR "$<TARGET_FILE_DIR:WinBolo>")
endif()

add_custom_command(TARGET WinBolo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data" "${WINBOLO_DATA_DIR}/data"
    COMMENT "Copying WinBolo data files to output directory"
)

add_custom_command(TARGET WinBolo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WINBOLO_DATA_DIR}/Brains"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/brains/NewAutopilot"
        "${WINBOLO_DATA_DIR}/Brains/NewAutopilot"
    COMMENT "Copying brain scripts to output Brains/ directory"
)

# Copy shared libraries next to WinBolo executable (all platforms).
# FetchContent-built libs (SDL3, SDL3_ttf) are not picked up by TARGET_RUNTIME_DLLS.
add_custom_command(TARGET WinBolo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>"         "$<TARGET_FILE_DIR:WinBolo>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3_ttf::SDL3_ttf>" "$<TARGET_FILE_DIR:WinBolo>"
    COMMENT "Copying SDL3 shared libraries to WinBolo output directory"
)
# Copy ONNX Runtime shared library if using FetchContent prebuilt
if(HAVE_ONNXRUNTIME)
    if(WIN32 AND DEFINED ONNXRUNTIME_DLL)
        add_custom_command(TARGET WinBolo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:WinBolo>"
            COMMENT "Copying ONNX Runtime DLL to WinBolo output directory"
        )
    elseif(DEFINED ONNXRUNTIME_LIBDIR)
        file(GLOB _ORT_ALL_SO "${ONNXRUNTIME_LIBDIR}/libonnxruntime*")
        foreach(_f ${_ORT_ALL_SO})
            add_custom_command(TARGET WinBolo POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" "$<TARGET_FILE_DIR:WinBolo>"
            )
        endforeach()
    endif()
endif()
# Copy crashpad_handler next to WinBolo executable
if(HAVE_SENTRY AND TARGET crashpad_handler)
    add_custom_command(TARGET WinBolo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:crashpad_handler>" "$<TARGET_FILE_DIR:WinBolo>"
        COMMENT "Copying crashpad_handler to WinBolo output directory"
    )
endif()
# Copy Steam runtime library and steam_appid.txt next to executable
if(EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h")
    if(WIN32)
        add_custom_command(TARGET WinBolo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.dll"
                "$<TARGET_FILE_DIR:WinBolo>"
            COMMENT "Copying steam_api64.dll to WinBolo output directory"
        )
    endif()
    add_custom_command(TARGET WinBolo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/steam_appid.txt"
            "$<TARGET_FILE_DIR:WinBolo>"
        COMMENT "Copying steam_appid.txt to WinBolo output directory"
    )
endif()
# On Windows, also copy vcpkg / imported DLLs (curl, zlib, etc.)
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    add_custom_command(TARGET WinBolo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:WinBolo> "$<TARGET_FILE_DIR:WinBolo>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying runtime DLLs to WinBolo output directory"
    )
endif()
# MinGW: copy runtime DLLs (libwinpthread, libgcc, libstdc++) next to executables
if(MINGW)
    get_filename_component(_MINGW_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    foreach(_dll libwinpthread-1.dll libgcc_s_seh-1.dll libstdc++-6.dll)
        if(EXISTS "${_MINGW_BIN_DIR}/${_dll}")
            add_custom_command(TARGET WinBolo POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_MINGW_BIN_DIR}/${_dll}" "$<TARGET_FILE_DIR:WinBolo>"
                COMMENT "Copying ${_dll} to WinBolo output directory"
            )
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# WinBoloDS Server (cross-platform: Windows, Linux, macOS)
# ---------------------------------------------------------------------------

set(SERVER_SOURCES
    src/bolo/allience.c
    src/bolo/bases.c src/bolo/bolo_map.c
    src/bolo/building.c src/bolo/crc.c src/bolo/debug_file_output.c src/bolo/net_packet_debug.c
    src/bolo/explosions.c src/bolo/floodfill.c src/bolo/gametype.c src/bolo/global.c
    src/bolo/grass.c src/bolo/labels.c src/bolo/lgm.c src/bolo/log.c
    src/bolo/mathWinbolo.c src/bolo/mines.c src/bolo/minesexp.c
    src/bolo/pillbox.c src/bolo/players.c src/bolo/playersrejoin.c src/bolo/rubble.c
    src/bolo/screenbrainmap.c src/bolo/screenbullet.c src/bolo/screencalc.c
    src/bolo/shells.c src/bolo/starts.c
    src/bolo/swamp.c src/bolo/tank.c src/bolo/tankexp.c src/bolo/tutorial.c src/bolo/treegrow.c
    src/bolo/udppackets.c src/bolo/util.c src/bolo/braincore.c src/bolo/brain_pathfinder.c src/bolo/brain_worldsim.c
    src/server/geolookup.c
    src/server/servermain.c src/server/servermessages.c
    src/server/threads.c src/server/server_sim.c src/server/server_stubs.c
    src/server/server_frontend_stubs.c
    src/bolo/transport_local.c src/bolo/transport_udp_common.c src/bolo/transport_udp_client.c src/bolo/transport_udp_server.c src/bolo/client_state.c src/bolo/interpolation.c src/bolo/position_history.c src/bolo/client_sim.c
    src/bolo/bot_manager.c
    src/bolo/obs_builder.c
    src/gui/sdl3/luabrainshandler.c
    src/winbolonet/winbolonet.c src/winbolonet/http.c
    src/winbolonet/winbolonetthread.c src/winbolonet/winbolonetevents.c src/third_party/tweetnacl/tweetnacl.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/mapeditor/mapeditor_generate.c src/mapeditor/mapeditor_maze.c
)


add_executable(WinBoloDS ${SERVER_SOURCES})
target_sources(WinBoloDS PRIVATE src/steam/steam_wrapper_stub.c)
target_compile_definitions(WinBoloDS PRIVATE
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(WinBoloDS PRIVATE
    src/gui/sdl3
    src/gui
    src/bolo
    src/server
    src/mapeditor
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)

if(WIN32)
    target_sources(WinBoloDS PRIVATE
        src/gui/sdl3/lang.c
        src/res/winbolods.rc
    )
    target_link_libraries(WinBoloDS PRIVATE ws2_32 winmm)
    target_compile_options(WinBoloDS PRIVATE -fcommon)
    if(MSVC)
        target_link_options(WinBoloDS PRIVATE "/SUBSYSTEM:CONSOLE,10.0")
    endif()
else()
    target_sources(WinBoloDS PRIVATE
        src/server/posix_stubs.c
        src/gui/sdl3/lang.c
    )
    target_link_libraries(WinBoloDS PRIVATE ${MATH_LIB} pthread)
    target_compile_options(WinBoloDS PRIVATE -fcommon)
endif()

if(TARGET maxminddb)
    target_link_libraries(WinBoloDS PRIVATE maxminddb)
    target_compile_definitions(WinBoloDS PRIVATE HAVE_MAXMINDDB=1)
endif()

target_link_libraries(WinBoloDS PRIVATE minizip SDL3::SDL3 CURL::libcurl lua_static cjson)

# ONNX Runtime ML brain support for server bots (optional)
if(HAVE_ONNXRUNTIME)
    target_sources(WinBoloDS PRIVATE src/bolo/ml_brain.c)
    target_compile_definitions(WinBoloDS PRIVATE HAVE_ONNXRUNTIME=1)
    target_link_libraries(WinBoloDS PRIVATE onnxruntime::onnxruntime)
endif()

# Sentry crash reporting (always compile — stubs when HAVE_SENTRY is off)
target_sources(WinBoloDS PRIVATE src/common/sentry_integration.c)
if(HAVE_SENTRY)
    target_compile_definitions(WinBoloDS PRIVATE HAVE_SENTRY=1)
    if(SENTRY_DSN)
        target_compile_definitions(WinBoloDS PRIVATE SENTRY_DSN="${SENTRY_DSN}")
    endif()
    target_link_libraries(WinBoloDS PRIVATE sentry::sentry)
endif()

# Copy geo database to server output directory
if(EXISTS "${CMAKE_SOURCE_DIR}/data/dbip-country-lite.mmdb")
    add_custom_command(TARGET WinBoloDS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:WinBoloDS>/data"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/data/dbip-country-lite.mmdb"
            "$<TARGET_FILE_DIR:WinBoloDS>/data/dbip-country-lite.mmdb"
        COMMENT "Copying geo database to server output directory"
    )
endif()

# Copy shared libraries next to WinBoloDS (all platforms)
add_custom_command(TARGET WinBoloDS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:WinBoloDS>"
    COMMENT "Copying SDL3 shared library to WinBoloDS output directory"
)

add_custom_command(TARGET WinBoloDS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data/lang" "$<TARGET_FILE_DIR:WinBoloDS>/data/lang"
    COMMENT "Copying WinBoloDS lang files to output directory"
)
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    add_custom_command(TARGET WinBoloDS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:WinBoloDS> "$<TARGET_FILE_DIR:WinBoloDS>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying runtime DLLs to WinBoloDS output directory"
    )
endif()

# Copy crashpad_handler next to WinBoloDS executable
if(HAVE_SENTRY AND TARGET crashpad_handler)
    add_custom_command(TARGET WinBoloDS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:crashpad_handler>" "$<TARGET_FILE_DIR:WinBoloDS>"
        COMMENT "Copying crashpad_handler to WinBoloDS output directory"
    )
endif()
# Copy Lua brain scripts to output Brains/ directory (server)
add_custom_command(TARGET WinBoloDS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:WinBoloDS>/Brains"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/brains/NewAutopilot"
        "$<TARGET_FILE_DIR:WinBoloDS>/Brains/NewAutopilot"
    COMMENT "Copying brain scripts to WinBoloDS output Brains/ directory"
)

# ---------------------------------------------------------------------------
# LogViewer (cross-platform: Windows, Linux, macOS)
# ---------------------------------------------------------------------------

file(GLOB LOGVIEWER_SOURCES src/logviewer/*.c src/common/*.c)
if(NOT WIN32)
    list(FILTER LOGVIEWER_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()

set(LOGVIEWER_IMGUI_SOURCES
    src/logviewer/imgui/imgui_context.cpp
    src/logviewer/imgui/imgui_main_menu.cpp
    src/logviewer/imgui/imgui_controls.cpp
    src/logviewer/imgui/imgui_game_info.cpp
    src/logviewer/imgui/imgui_events.cpp
    src/logviewer/imgui/imgui_item_info.cpp
    src/logviewer/imgui/imgui_dialogs.cpp
    src/logviewer/imgui/imgui_game_viewport.cpp
)

set(LOGVIEWER_PLATFORM_SOURCES
    src/logviewer/platform/platform_config.c
    src/logviewer/platform/platform_dialogs.c
)

add_executable(LogViewer
    ${LOGVIEWER_SOURCES}
    ${LOGVIEWER_IMGUI_SOURCES}
    ${LOGVIEWER_PLATFORM_SOURCES}
    src/gui/sdl3/lang.c
)
target_compile_definitions(LogViewer PRIVATE
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

if(WIN32)
    target_sources(LogViewer PRIVATE src/logviewer/script1.rc)
endif()

set_target_properties(LogViewer PROPERTIES OUTPUT_NAME "Log Viewer")

if(WIN32)
    set_target_properties(LogViewer PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(LogViewer PRIVATE "/SUBSYSTEM:WINDOWS,10.0" "/MANIFEST:NO")
    endif()
endif()

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    set_target_properties(LogViewer PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/logviewer/platform/Info.plist"
    )
    set(APP_ICON_ICNS "${CMAKE_SOURCE_DIR}/src/logviewer/platform/AppIcon.icns")
    if(EXISTS "${APP_ICON_ICNS}")
        target_sources(LogViewer PRIVATE "${APP_ICON_ICNS}")
        set_source_files_properties("${APP_ICON_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        set_target_properties(LogViewer PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "AppIcon.icns")
    endif()
endif()

target_include_directories(LogViewer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/logviewer
    ${CMAKE_SOURCE_DIR}/src/logviewer/imgui
    ${CMAKE_SOURCE_DIR}/src/logviewer/platform
)

target_link_libraries(LogViewer PRIVATE
    minizip SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui_backends
)

if(WIN32)
    target_link_libraries(LogViewer PRIVATE ws2_32)
endif()
if(MATH_LIB)
    target_link_libraries(LogViewer PRIVATE ${MATH_LIB})
endif()

# Copy shared libraries next to LogViewer (all platforms)
add_custom_command(TARGET LogViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:LogViewer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3_ttf::SDL3_ttf>" "$<TARGET_FILE_DIR:LogViewer>"
    COMMENT "Copying SDL3 shared libraries to LogViewer output directory"
)

if(APPLE)
    add_custom_command(TARGET LogViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data"
            "$<TARGET_BUNDLE_CONTENT_DIR:LogViewer>/Resources/data"
        COMMENT "Copying data files into app bundle Resources"
    )
else()
    add_custom_command(TARGET LogViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data" "$<TARGET_FILE_DIR:LogViewer>/data"
        COMMENT "Copying LogViewer data files to output directory"
    )
endif()

# ---------------------------------------------------------------------------
# WinBoloHeadless (headless client for automated testing)
# ---------------------------------------------------------------------------

set(HEADLESS_SOURCES
    src/bolo/allience.c src/bolo/bases.c src/bolo/bolo_map.c
    src/bolo/building.c src/bolo/crc.c src/bolo/debug_file_output.c src/bolo/net_packet_debug.c
    src/bolo/explosions.c src/bolo/floodfill.c src/bolo/gametype.c src/bolo/global.c
    src/bolo/grass.c src/bolo/labels.c src/bolo/lgm.c src/bolo/log.c
    src/bolo/mathWinbolo.c src/bolo/messages.c src/bolo/mines.c src/bolo/minesexp.c
    src/bolo/gui_message.c
    src/bolo/pillbox.c src/bolo/players.c src/bolo/playersrejoin.c src/bolo/rubble.c
    src/bolo/screen.c src/bolo/screenbrainmap.c src/bolo/screenbullet.c
    src/bolo/screencalc.c src/bolo/screenlgm.c src/bolo/screentank.c src/bolo/scroll.c src/bolo/scroll_item_list.c
    src/bolo/shells.c src/bolo/sounddist.c src/bolo/starts.c src/bolo/swamp.c
    src/bolo/tank.c src/bolo/tankexp.c src/bolo/tutorial.c src/bolo/treegrow.c src/bolo/udppackets.c
    src/bolo/util.c src/bolo/braincore.c src/bolo/brain_pathfinder.c src/bolo/brain_worldsim.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/currentgames.c src/gui/sdl3/lang.c
    src/gui/sdl3/dns_lookups.c
    src/headless/headless_main.c src/headless/headless_frontend.c
    src/bolo/transport_local.c src/bolo/transport_udp_common.c src/bolo/transport_udp_client.c src/bolo/transport_udp_server.c src/bolo/client_state.c src/bolo/interpolation.c src/bolo/position_history.c src/bolo/client_sim.c
    src/bolo/bot_manager.c
    src/bolo/obs_builder.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/server/geolookup.c
    src/server/servermessages.c src/server/threads.c
    src/server/server_sim.c src/server/server_stubs.c
    src/mapeditor/mapeditor_generate.c src/mapeditor/mapeditor_maze.c
    src/winbolonet/winbolonet.c src/winbolonet/http.c
    src/winbolonet/winbolonetthread.c src/winbolonet/winbolonetevents.c src/third_party/tweetnacl/tweetnacl.c
)


# BSD headers
file(GLOB HEADLESS_BSD_SOURCES src/bsd/*.c)
list(APPEND HEADLESS_SOURCES ${HEADLESS_BSD_SOURCES})

# common/ utilities
file(GLOB HEADLESS_COMMON_SOURCES src/common/*.c)
list(APPEND HEADLESS_SOURCES ${HEADLESS_COMMON_SOURCES})
if(NOT WIN32)
    list(FILTER HEADLESS_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()

add_executable(WinBoloHeadless ${HEADLESS_SOURCES})
target_sources(WinBoloHeadless PRIVATE src/steam/steam_wrapper_stub.c)
target_compile_definitions(WinBoloHeadless PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(WinBoloHeadless PRIVATE
    src/gui/sdl3
    src/gui
    src/bolo
    src/server
    src/headless
    src/mapeditor
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)

if(WIN32)
    target_link_libraries(WinBoloHeadless PRIVATE ws2_32 winmm user32)
    target_compile_options(WinBoloHeadless PRIVATE -fcommon)
else()
    target_sources(WinBoloHeadless PRIVATE
        src/server/posix_stubs.c
    )
    target_link_libraries(WinBoloHeadless PRIVATE ${MATH_LIB} pthread)
    target_compile_options(WinBoloHeadless PRIVATE -fcommon)
endif()

if(TARGET maxminddb)
    target_link_libraries(WinBoloHeadless PRIVATE maxminddb)
    target_compile_definitions(WinBoloHeadless PRIVATE HAVE_MAXMINDDB=1)
endif()

target_link_libraries(WinBoloHeadless PRIVATE
    SDL3::SDL3 minizip lua_static CURL::libcurl cjson)

# ONNX Runtime ML brain support for headless (optional)
if(HAVE_ONNXRUNTIME)
    target_sources(WinBoloHeadless PRIVATE src/bolo/ml_brain.c)
    target_compile_definitions(WinBoloHeadless PRIVATE HAVE_ONNXRUNTIME=1)
    target_link_libraries(WinBoloHeadless PRIVATE onnxruntime::onnxruntime)
endif()

# Sentry crash reporting (always compile — stubs when HAVE_SENTRY is off)
target_sources(WinBoloHeadless PRIVATE src/common/sentry_integration.c)
if(HAVE_SENTRY)
    target_compile_definitions(WinBoloHeadless PRIVATE HAVE_SENTRY=1)
    if(SENTRY_DSN)
        target_compile_definitions(WinBoloHeadless PRIVATE SENTRY_DSN="${SENTRY_DSN}")
    endif()
    target_link_libraries(WinBoloHeadless PRIVATE sentry::sentry)
endif()

# Copy shared libraries and brain scripts next to headless executable
add_custom_command(TARGET WinBoloHeadless POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:WinBoloHeadless>"
    COMMENT "Copying SDL3 shared library to WinBoloHeadless output directory"
)
add_custom_command(TARGET WinBoloHeadless POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:WinBoloHeadless>/Brains"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/brains/NewAutopilot"
        "$<TARGET_FILE_DIR:WinBoloHeadless>/Brains/NewAutopilot"
    COMMENT "Copying brain scripts to WinBoloHeadless output directory"
)
add_custom_command(TARGET WinBoloHeadless POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data/lang" "$<TARGET_FILE_DIR:WinBoloHeadless>/data/lang"
    COMMENT "Copying WinBoloHeadless lang files to output directory"
)
# Copy crashpad_handler next to WinBoloHeadless executable
if(HAVE_SENTRY AND TARGET crashpad_handler)
    add_custom_command(TARGET WinBoloHeadless POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:crashpad_handler>" "$<TARGET_FILE_DIR:WinBoloHeadless>"
        COMMENT "Copying crashpad_handler to WinBoloHeadless output directory"
    )
endif()

# ---------------------------------------------------------------------------
# WinBoloGym shared library (C API for ML training)
# ---------------------------------------------------------------------------

set(GYM_SOURCES
    src/bolo/allience.c src/bolo/bases.c src/bolo/bolo_map.c
    src/bolo/building.c src/bolo/crc.c src/bolo/debug_file_output.c src/bolo/net_packet_debug.c
    src/bolo/explosions.c src/bolo/floodfill.c src/bolo/gametype.c src/bolo/global.c
    src/bolo/grass.c src/bolo/labels.c src/bolo/lgm.c src/bolo/log.c
    src/bolo/mathWinbolo.c src/bolo/messages.c src/bolo/mines.c src/bolo/minesexp.c
    src/bolo/gui_message.c
    src/bolo/pillbox.c src/bolo/players.c src/bolo/playersrejoin.c src/bolo/rubble.c
    src/bolo/screen.c src/bolo/screenbrainmap.c src/bolo/screenbullet.c
    src/bolo/screencalc.c src/bolo/screenlgm.c src/bolo/screentank.c src/bolo/scroll.c src/bolo/scroll_item_list.c
    src/bolo/shells.c src/bolo/sounddist.c src/bolo/starts.c src/bolo/swamp.c
    src/bolo/tank.c src/bolo/tankexp.c src/bolo/tutorial.c src/bolo/treegrow.c src/bolo/udppackets.c
    src/bolo/util.c src/bolo/braincore.c src/bolo/brain_pathfinder.c src/bolo/brain_worldsim.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/currentgames.c src/gui/sdl3/lang.c
    src/gui/sdl3/dns_lookups.c
    src/headless/headless_frontend.c
    src/bolo/transport_local.c src/bolo/transport_udp_common.c src/bolo/transport_udp_client.c src/bolo/transport_udp_server.c src/bolo/client_state.c src/bolo/interpolation.c src/bolo/position_history.c src/bolo/client_sim.c
    src/bolo/bot_manager.c
    src/bolo/obs_builder.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/server/geolookup.c
    src/server/servermessages.c src/server/threads.c
    src/server/server_sim.c src/server/server_stubs.c
    src/mapeditor/mapeditor_generate.c src/mapeditor/mapeditor_maze.c
    src/winbolonet/winbolonet.c src/winbolonet/http.c
    src/winbolonet/winbolonetthread.c src/winbolonet/winbolonetevents.c src/third_party/tweetnacl/tweetnacl.c
    src/gym/winbolo_gym.c
)


# BSD headers
file(GLOB GYM_BSD_SOURCES src/bsd/*.c)
list(APPEND GYM_SOURCES ${GYM_BSD_SOURCES})

# common/ utilities
file(GLOB GYM_COMMON_SOURCES src/common/*.c)
list(APPEND GYM_SOURCES ${GYM_COMMON_SOURCES})
if(NOT WIN32)
    list(FILTER GYM_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()

add_library(winbolo_gym SHARED ${GYM_SOURCES})
target_sources(winbolo_gym PRIVATE src/steam/steam_wrapper_stub.c)
target_compile_definitions(winbolo_gym PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_GYM_EXPORTS=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(winbolo_gym PRIVATE
    src/gui/sdl3
    src/gui
    src/bolo
    src/server
    src/headless
    src/mapeditor
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)

if(WIN32)
    target_link_libraries(winbolo_gym PRIVATE ws2_32 winmm user32)
    target_compile_options(winbolo_gym PRIVATE -fcommon)
else()
    target_sources(winbolo_gym PRIVATE
        src/server/posix_stubs.c
    )
    target_link_libraries(winbolo_gym PRIVATE ${MATH_LIB} pthread)
    target_compile_options(winbolo_gym PRIVATE -fcommon)
endif()

if(TARGET maxminddb)
    target_link_libraries(winbolo_gym PRIVATE maxminddb)
    target_compile_definitions(winbolo_gym PRIVATE HAVE_MAXMINDDB=1)
endif()

target_link_libraries(winbolo_gym PRIVATE
    SDL3::SDL3 minizip lua_static CURL::libcurl cjson)

if(OpenMP_C_FOUND)
    target_link_libraries(winbolo_gym PRIVATE OpenMP::OpenMP_C)
endif()

set_target_properties(winbolo_gym PROPERTIES
    PUBLIC_HEADER src/gym/winbolo_gym.h
)

# Copy SDL3 next to the gym library
add_custom_command(TARGET winbolo_gym POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:winbolo_gym>"
    COMMENT "Copying SDL3 shared library to winbolo_gym output directory"
)
add_custom_command(TARGET winbolo_gym POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data/lang" "$<TARGET_FILE_DIR:winbolo_gym>/data/lang"
    COMMENT "Copying winbolo_gym lang files to output directory"
)

# ---------------------------------------------------------------------------
# gym_batch_test (Phase 0 determinism baseline)
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# BrainTest (standalone brain debug viewer with overlays)
# ---------------------------------------------------------------------------

set(BRAINTEST_SOURCES
    src/bolo/allience.c src/bolo/bases.c src/bolo/bolo_map.c
    src/bolo/building.c src/bolo/crc.c src/bolo/debug_file_output.c src/bolo/net_packet_debug.c
    src/bolo/explosions.c src/bolo/floodfill.c src/bolo/gametype.c src/bolo/global.c
    src/bolo/grass.c src/bolo/labels.c src/bolo/lgm.c src/bolo/log.c
    src/bolo/mathWinbolo.c src/bolo/messages.c src/bolo/mines.c src/bolo/minesexp.c
    src/bolo/gui_message.c
    src/bolo/pillbox.c src/bolo/players.c src/bolo/playersrejoin.c src/bolo/rubble.c
    src/bolo/screen.c src/bolo/screenbrainmap.c src/bolo/screenbullet.c
    src/bolo/screencalc.c src/bolo/screenlgm.c src/bolo/screentank.c src/bolo/scroll.c src/bolo/scroll_item_list.c
    src/bolo/shells.c src/bolo/sounddist.c src/bolo/starts.c src/bolo/swamp.c
    src/bolo/tank.c src/bolo/tankexp.c src/bolo/tutorial.c src/bolo/treegrow.c src/bolo/udppackets.c
    src/bolo/util.c src/bolo/braincore.c src/bolo/brain_pathfinder.c src/bolo/brain_worldsim.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/lang.c
    src/gui/sdl3/mapview.c src/gui/sdl3/tileloader.c
    src/bolo/transport_local.c src/bolo/transport_udp_common.c src/bolo/transport_udp_client.c src/bolo/transport_udp_server.c src/bolo/client_state.c src/bolo/interpolation.c src/bolo/position_history.c src/bolo/client_sim.c
    src/bolo/bot_manager.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/server/servermessages.c src/server/server_sim.c src/server/server_stubs.c
    src/mapeditor/mapeditor_generate.c src/mapeditor/mapeditor_maze.c
    src/braintest/braintest_main.c src/braintest/braintest_frontend.c
)


# BSD headers
file(GLOB BRAINTEST_BSD_SOURCES src/bsd/*.c)
list(APPEND BRAINTEST_SOURCES ${BRAINTEST_BSD_SOURCES})

# common/ utilities
file(GLOB BRAINTEST_COMMON_SOURCES src/common/*.c)
list(APPEND BRAINTEST_SOURCES ${BRAINTEST_COMMON_SOURCES})
if(NOT WIN32)
    list(FILTER BRAINTEST_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()

add_executable(BrainTest ${BRAINTEST_SOURCES})
target_sources(BrainTest PRIVATE src/steam/steam_wrapper_stub.c)
target_compile_definitions(BrainTest PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(BrainTest PRIVATE
    src/gui/sdl3
    src/gui
    src/bolo
    src/server
    src/braintest
    src/mapeditor
)

if(WIN32)
    target_link_libraries(BrainTest PRIVATE ws2_32 winmm user32)
    target_compile_options(BrainTest PRIVATE -fcommon)
else()
    target_sources(BrainTest PRIVATE
        src/server/posix_stubs.c
    )
    target_link_libraries(BrainTest PRIVATE ${MATH_LIB} pthread)
    target_compile_options(BrainTest PRIVATE -fcommon)
endif()

target_link_libraries(BrainTest PRIVATE
    SDL3::SDL3 minizip lua_static
    nanosvg stb_image
)

# Copy shared libraries and assets next to BrainTest executable
add_custom_command(TARGET BrainTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:BrainTest>"
    COMMENT "Copying SDL3 shared library to BrainTest output directory"
)
add_custom_command(TARGET BrainTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:BrainTest>/Brains"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/brains/NewAutopilot"
        "$<TARGET_FILE_DIR:BrainTest>/Brains/NewAutopilot"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data" "$<TARGET_FILE_DIR:BrainTest>/data"
    COMMENT "Copying BrainTest data files to output directory"
)

# ---------------------------------------------------------------------------
# MapEditor (standalone map editor)
# ---------------------------------------------------------------------------

set(MAPEDITOR_SOURCES
    src/bolo/allience.c src/bolo/bases.c src/bolo/bolo_map.c
    src/bolo/building.c src/bolo/crc.c src/bolo/debug_file_output.c src/bolo/net_packet_debug.c
    src/bolo/explosions.c src/bolo/floodfill.c src/bolo/gametype.c src/bolo/global.c
    src/bolo/grass.c src/bolo/labels.c src/bolo/lgm.c src/bolo/log.c
    src/bolo/mathWinbolo.c src/bolo/messages.c src/bolo/mines.c src/bolo/minesexp.c
    src/bolo/gui_message.c
    src/bolo/pillbox.c src/bolo/players.c src/bolo/playersrejoin.c src/bolo/rubble.c
    src/bolo/screen.c src/bolo/screenbrainmap.c src/bolo/screenbullet.c
    src/bolo/screencalc.c src/bolo/screenlgm.c src/bolo/screentank.c src/bolo/scroll.c src/bolo/scroll_item_list.c
    src/bolo/shells.c src/bolo/sounddist.c src/bolo/starts.c src/bolo/swamp.c
    src/bolo/tank.c src/bolo/tankexp.c src/bolo/tutorial.c src/bolo/treegrow.c src/bolo/udppackets.c
    src/bolo/util.c src/bolo/braincore.c src/bolo/brain_pathfinder.c src/bolo/brain_worldsim.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/minimap_render.c
    src/gui/sdl3/lang.c
    src/gui/sdl3/mapview.c src/gui/sdl3/tileloader.c
    src/bolo/transport_local.c src/bolo/transport_udp_common.c src/bolo/transport_udp_client.c src/bolo/transport_udp_server.c src/bolo/client_state.c src/bolo/interpolation.c src/bolo/position_history.c src/bolo/client_sim.c
    src/bolo/bot_manager.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/server/servermessages.c src/server/server_sim.c src/server/server_stubs.c
    src/mapeditor/mapeditor.c src/mapeditor/mapeditor_imgui.cpp src/mapeditor/mapgen_imgui.cpp src/mapeditor/mapeditor_undo.c src/mapeditor/mapeditor_validate.c src/mapeditor/mapeditor_symmetry.c src/mapeditor/mapeditor_generate.c src/mapeditor/mapeditor_maze.c src/mapeditor/mapeditor_text.c src/mapeditor/mapeditor_fonts.c src/mapeditor/mapeditor_stats.c src/mapeditor/mapeditor_image.c src/mapeditor/mapeditor_stamp.c src/mapeditor/mapeditor_export.c
    src/third_party/stb/stb_image_write_impl.c
    src/mapeditor/mapeditor_main.c
    src/braintest/braintest_frontend.c
)

file(GLOB MAPEDITOR_BSD_SOURCES src/bsd/*.c)
list(APPEND MAPEDITOR_SOURCES ${MAPEDITOR_BSD_SOURCES})

file(GLOB MAPEDITOR_COMMON_SOURCES src/common/*.c)
list(APPEND MAPEDITOR_SOURCES ${MAPEDITOR_COMMON_SOURCES})
if(NOT WIN32)
    list(FILTER MAPEDITOR_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()

add_executable(MapEditor ${MAPEDITOR_SOURCES})
target_sources(MapEditor PRIVATE src/steam/steam_wrapper_stub.c)
target_compile_definitions(MapEditor PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(MapEditor PRIVATE
    src/gui/sdl3
    src/gui
    src/bolo
    src/server
    src/mapeditor
)

if(WIN32)
    target_link_libraries(MapEditor PRIVATE ws2_32 winmm user32)
    target_compile_options(MapEditor PRIVATE -fcommon)
else()
    target_sources(MapEditor PRIVATE
        src/server/posix_stubs.c
    )
    target_link_libraries(MapEditor PRIVATE ${MATH_LIB} pthread)
    target_compile_options(MapEditor PRIVATE -fcommon)
endif()

target_link_libraries(MapEditor PRIVATE
    SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui_backends minizip lua_static
    nanosvg stb_image
)

# macOS trackpad pinch-to-zoom support
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(MapEditor PRIVATE src/gui/sdl3/macos_pinch.m)
    target_link_libraries(MapEditor PRIVATE "-framework AppKit")
endif()

# Copy shared libraries and assets next to MapEditor executable
add_custom_command(TARGET MapEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:MapEditor>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3_ttf::SDL3_ttf>" "$<TARGET_FILE_DIR:MapEditor>"
    COMMENT "Copying SDL3 and SDL3_ttf shared libraries to MapEditor output directory"
)
add_custom_command(TARGET MapEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data" "$<TARGET_FILE_DIR:MapEditor>/data"
    COMMENT "Copying MapEditor data files to output directory"
)

# MinGW: statically link the GCC/C++ runtime so the executables don't need
# libgcc_s_seh-1.dll, libstdc++-6.dll, or libwinpthread-1.dll at runtime.
if(MINGW)
    foreach(_tgt WinBolo WinBoloDS LogViewer WinBoloHeadless BrainTest MapEditor)
        if(TARGET ${_tgt})
            target_link_options(${_tgt} PRIVATE -static)
        endif()
    endforeach()
endif()

set_flat_output(LogViewer        "${CMAKE_BINARY_DIR}")
set_flat_output(WinBolo          "${CMAKE_BINARY_DIR}")
set_flat_output(WinBoloDS        "${CMAKE_BINARY_DIR}")
set_flat_output(WinBoloHeadless  "${CMAKE_BINARY_DIR}")
set_flat_output(BrainTest        "${CMAKE_BINARY_DIR}")
set_flat_output(MapEditor        "${CMAKE_BINARY_DIR}")
set_flat_output(winbolo_gym      "${CMAKE_BINARY_DIR}")

# ---------------------------------------------------------------------------
# Distribution zip  (cmake --build <dir> --target dist)
# ---------------------------------------------------------------------------

set(DIST_DIR "${CMAKE_BINARY_DIR}/dist/winbolo")
set(DIST_ZIP "${CMAKE_BINARY_DIR}/winbolo-dist.zip")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/dist")

# The WinBolo POST_BUILD steps already copy ALL needed shared libraries
# (SDL3, SDL3_ttf, curl, zlib, etc.) into the flat build output
# directory (CMAKE_BINARY_DIR).  The dist target depends on WinBolo, so by
# the time it runs, every DLL / .so / .dylib is already sitting there.
# We use a helper cmake -P script to glob and copy them all into dist/.

# Crashpad handler must be inside the dist target's command sequence (before the
# zip step) so it ends up in the archive.  We build the COMMAND as a variable
# because if() cannot appear inside add_custom_target().
set(DIST_CRASHPAD_HANDLER_CMD "")
if(HAVE_SENTRY AND TARGET crashpad_handler)
    set(DIST_CRASHPAD_HANDLER_CMD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:crashpad_handler>" "${DIST_DIR}/")
endif()

add_custom_target(dist
    DEPENDS WinBolo WinBoloDS LogViewer
    COMMENT "Creating distribution zip ${DIST_ZIP}"

    COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/Brains"

    # Binaries
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinBolo>"   "${DIST_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinBoloDS>" "${DIST_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:LogViewer>" "${DIST_DIR}/"
    ${DIST_CRASHPAD_HANDLER_CMD}

    # Shared libraries — glob all DLLs / .so / .dylib from the build output
    # directory.  This picks up FetchContent libs AND vcpkg libs in one step.
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_BINARY_DIR}
        -DDEST_DIR=${DIST_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_shared_libs.cmake

    # Data assets (includes data/lang)
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "${DIST_DIR}/data"

    # Brain scripts
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/brains/NewAutopilot" "${DIST_DIR}/Brains/NewAutopilot"

    # Create zip
    COMMAND ${CMAKE_COMMAND} -E rm -f "${DIST_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E tar "cf" "${DIST_ZIP}" --format=zip "winbolo"

    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
    BYPRODUCTS "${DIST_ZIP}"
    VERBATIM
)

endif() # iOS vs Desktop
