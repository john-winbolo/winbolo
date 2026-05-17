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

include(${CMAKE_SOURCE_DIR}/cmake/bolo_lib.cmake)

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

# Build universal2 on macOS so the same .app runs natively on Apple Silicon
# and Intel Macs. Override on the command line with -DCMAKE_OSX_ARCHITECTURES=arm64
# for a single-arch build. iOS sets its own architectures via the toolchain.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CMAKE_OSX_ARCHITECTURES)
    # FORCE because CLion / some IDE configures write an empty cache entry
    # for CMAKE_OSX_ARCHITECTURES, which would otherwise block this default.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS build architectures" FORCE)
endif()

set(WINBOLO_VERSION "1.18" CACHE STRING "WinBolo version number")

# Global macOS bundle metadata. Set once here so every MACOSX_BUNDLE target
# (WinBolo, Log Viewer, Map Editor) picks up matching version/copyright keys
# in its Info.plist, and so the deployment target applies to all of them.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    string(TIMESTAMP _WB_CURRENT_YEAR "%Y")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${WINBOLO_VERSION}")
    set(MACOSX_BUNDLE_BUNDLE_VERSION       "${WINBOLO_VERSION}")
    set(MACOSX_BUNDLE_LONG_VERSION_STRING  "${WINBOLO_VERSION}")
    set(MACOSX_BUNDLE_COPYRIGHT            "Copyright 1998-${_WB_CURRENT_YEAR} John Morrison")
endif()

execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE WINBOLO_GIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT WINBOLO_GIT_HASH)
    set(WINBOLO_GIT_HASH "unknown")
endif()
string(TIMESTAMP WINBOLO_BUILD_DATE "%Y-%m-%d")

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
    GIT_TAG        release-3.4.8
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
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl3_ttf imgui zlib)
    set(BUILD_SHARED_LIBS "${_saved_build_shared_libs}" CACHE BOOL "" FORCE)
    # Restore try_compile mode now that all probing dependencies are fetched
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_saved_try_compile_type}")
else()
    # MinGW (MSYS2): vendor freetype so we don't depend on vcpkg or system packages.
    # macOS: vendor freetype so a universal2 (arm64;x86_64) build doesn't link
    # against an arm64-only Homebrew libfreetype.dylib.
    if(MINGW OR APPLE)
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

# utf8proc — Unicode normalization + property tables (vendored, MIT)
# Compiled as a static lib to avoid duplicating its ~2MB Unicode table
# across every target that links the bolo sources.
add_library(utf8proc STATIC src/third_party/utf8proc/utf8proc.c)
target_include_directories(utf8proc SYSTEM PUBLIC src/third_party/utf8proc)
target_compile_definitions(utf8proc PUBLIC UTF8PROC_STATIC=1)
set_target_properties(utf8proc PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(utf8proc PRIVATE /wd4244 /wd4267)
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
        # /RTC1 (Debug runtime checks) is incompatible with /fsanitize=address;
        # gate it off when ENABLE_ASAN is on so the two don't fight.
        $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Debug>,$<NOT:$<BOOL:${ENABLE_ASAN}>>>:/RTC1>
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
# AddressSanitizer (opt-in via -DENABLE_ASAN=ON)
#
# Instruments allocations to catch heap overflows, use-after-free, and
# stack/global overflows at the moment of the bad access. Slows the
# binary ~2-3x. On MinGW the runtime DLL (libasan*.dll, libssp*.dll)
# must be on PATH or next to the .exe. With MSVC, /RTC1 is incompatible;
# this block strips it from Debug when ASan is enabled.
# ---------------------------------------------------------------------------

option(ENABLE_ASAN "Build with AddressSanitizer instrumentation" OFF)
if(ENABLE_ASAN)
    if(MSVC)
        # /RTC1 is gated off above when ENABLE_ASAN is on so the two
        # don't conflict; just turn ASan on here.
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address>)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    message(STATUS "AddressSanitizer ENABLED")
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
file(GLOB IOS_DATA_FONT
    "${CMAKE_SOURCE_DIR}/data/fonts/*.ttf"
    "${CMAKE_SOURCE_DIR}/data/fonts/*.otf"
)
file(GLOB IOS_DATA_MAPS "${CMAKE_SOURCE_DIR}/data/maps/*.map")
file(GLOB IOS_DATA_SOUNDS "${CMAKE_SOURCE_DIR}/data/sounds/*.wav")
file(GLOB IOS_DATA_SVG "${CMAKE_SOURCE_DIR}/data/svg/*.png" "${CMAKE_SOURCE_DIR}/data/svg/*.svg")
file(GLOB IOS_DATA_UI "${CMAKE_SOURCE_DIR}/data/ui/*.svg")
file(GLOB IOS_LANG_FILES "${CMAKE_SOURCE_DIR}/data/lang/*.txt")

# Bolo engine sources that stay per-target (HAVE_STEAM-divergent).
# Everything else now lives in bolo_static (added via add_subdirectory
# below). iOS doesn't define HAVE_STEAM, so PLAYER_FLAG_STEAM_BUILD is
# never OR'd into selfFlags / clientHints by these two TUs.
set(IOS_BOLO_SOURCES
    src/bolo/client_mapload.c
    src/bolo/transport_udp_client.c
)

# Build bolo_static for the iOS branch. iOS doesn't set HAVE_ONNXRUNTIME
# or ENABLE_PILLMASSAGE_BUG, so bolo_static here has the matching
# minimal feature set.
add_subdirectory(src/bolo)
add_subdirectory(src/server)
add_subdirectory(brains)

# Server sources needed for single-player mode
# server_sim.c + servermessages.c now come from server_sim_static;
# threads.c comes from threads_static.
set(IOS_SERVER_SOURCES
    src/server/server_stubs.c
    src/server/server_dedicated_log_stubs.c
    src/mapeditor/mapgen_imgui.cpp
)



# GUI/support sources (reused from sdl3 — matches desktop CLIENT_SOURCES minus winbolo.c)
set(IOS_GUI_SOURCES
    src/gui/sdl3/clientmutex.c
    src/gui/sdl3/sdl3imgui.cpp
    src/gui/sdl3/sdl3imgui_tablet.cpp
    src/gui/sdl3/sdl3draw.c
    src/gui/sdl3/sdl3draw_status.c
    src/gui/sdl3/mapview.c
    src/gui/sdl3/sprite_positions.c
    src/gui/sdl3/tileloader.c
    src/gui/sdl3/sdl_bmp.c
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
    src/gui/sdl3/map_preview_popup.cpp
    src/gui/sdl3/map_preview_view.cpp
    src/gui/sdl3/wb_theme.cpp
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
    src/server
    src/mapeditor
)
bolo_apply_include_rules(WinBoloIOS gui)
bolo_grant_internal_source_access(WinBoloIOS
    src/bolo/client_mapload.c
    src/bolo/transport_udp_client.c)

target_link_libraries(WinBoloIOS PRIVATE bolo_static server_sim_static bot_brains_static threads_static)
target_link_libraries(WinBoloIOS PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui_backends lua_static
    nanosvg stb_image utf8proc zlibstatic
    "-framework CoreText" "-framework CoreFoundation" "-framework CoreGraphics"
    "-framework AVFoundation"
    "-framework GameController"
)

target_compile_options(WinBoloIOS PRIVATE -fcommon)
target_compile_definitions(WinBoloIOS PRIVATE
    HAVE_SCREEN_C=1
    __IPHONEOS__=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
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
target_sources(WinBoloIOS PRIVATE src/common/wb_log.c)
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

# libplum — automatic UPnP / NAT-PMP / PCP port mapping for hosted servers.
# When OFF, src/bolo/nat_portmap.c compiles to no-op stubs.
option(BOLO_PORTMAP "Enable UPnP/NAT-PMP automatic port mapping" ON)

# wb_log compile-time level. Calls below this level expand to ((void)0)
# and are stripped by the compiler. Empty = wb_log.h decides (DEBUG in
# Debug builds, INFO in Release).
set(WINBOLO_LOG_LEVEL "" CACHE STRING
    "Compile-time log level for wb_log macros: TRACE/DEBUG/INFO/WARN/ERROR/OFF (empty = auto)")
set_property(CACHE WINBOLO_LOG_LEVEL PROPERTY STRINGS
    "" "TRACE" "DEBUG" "INFO" "WARN" "ERROR" "OFF")
if(WINBOLO_LOG_LEVEL)
    if(WINBOLO_LOG_LEVEL STREQUAL "TRACE")
        set(_WB_LOG_LEVEL_NUM 1)
    elseif(WINBOLO_LOG_LEVEL STREQUAL "DEBUG")
        set(_WB_LOG_LEVEL_NUM 2)
    elseif(WINBOLO_LOG_LEVEL STREQUAL "INFO")
        set(_WB_LOG_LEVEL_NUM 3)
    elseif(WINBOLO_LOG_LEVEL STREQUAL "WARN")
        set(_WB_LOG_LEVEL_NUM 4)
    elseif(WINBOLO_LOG_LEVEL STREQUAL "ERROR")
        set(_WB_LOG_LEVEL_NUM 5)
    elseif(WINBOLO_LOG_LEVEL STREQUAL "OFF")
        set(_WB_LOG_LEVEL_NUM 6)
    else()
        message(FATAL_ERROR
            "WINBOLO_LOG_LEVEL must be one of TRACE/DEBUG/INFO/WARN/ERROR/OFF (got '${WINBOLO_LOG_LEVEL}')")
    endif()
    add_compile_definitions(WB_LOG_LEVEL=${_WB_LOG_LEVEL_NUM})
    message(STATUS "wb_log: compile-time level = ${WINBOLO_LOG_LEVEL} (${_WB_LOG_LEVEL_NUM})")
else()
    message(STATUS "wb_log: compile-time level = auto (DEBUG in Debug builds, INFO in Release)")
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

# libplum — UPnP / NAT-PMP / PCP port mapping for hosted servers.
# Skipped on iOS (mobile) and Emscripten (wasm); the wrapper compiles to
# no-op stubs there.  Build static so we don't ship an extra shared library.
set(HAVE_LIBPLUM FALSE)
if(BOLO_PORTMAP AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT EMSCRIPTEN)
    FetchContent_Declare(
        libplum
        GIT_REPOSITORY https://github.com/paullouisageneau/libplum.git
        GIT_TAG        v0.5.3
        GIT_SHALLOW    TRUE
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    set(PLUM_NO_EXAMPLE         ON  CACHE INTERNAL "")
    set(PLUM_WARNINGS_AS_ERRORS OFF CACHE INTERNAL "")
    set(BUILD_SHARED_LIBS_SAVE_PLUM "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libplum)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVE_PLUM}" CACHE BOOL "" FORCE)
    # PIC so the static lib can also link into shared targets (winbolo_gym).
    if(TARGET plum)
        set_target_properties(plum PROPERTIES POSITION_INDEPENDENT_CODE ON)
        # MSVC: libplum's pcp.h has a zero-sized array (C4200) which our
        # /WX-as-error policy rejects. Disable just that warning for plum.
        if(MSVC)
            target_compile_options(plum PRIVATE /wd4200)
        endif()
    endif()
    set(HAVE_LIBPLUM TRUE)
    message(STATUS "libplum enabled — automatic NAT port mapping")
else()
    message(STATUS "libplum disabled — nat_portmap wrapper compiles to no-ops")
endif()

# ---------------------------------------------------------------------------
# bolo_static / server_sim_static / server_static — the bulk of the sim
# and the shared server-peer files. Added here so every desktop target
# below can link them. server_sim_static holds server_sim.c + servermessages.c;
# server_static (the dedicated-server runtime) PUBLIC-links it.
# Per-target conditional sources (client_mapload, transport_udp_client,
# nat_portmap, servermain, server_frontend_stubs, server_stubs, posix_stubs)
# stay in their respective *_SOURCES lists.
# ---------------------------------------------------------------------------

add_subdirectory(src/bolo)
add_subdirectory(src/server)
add_subdirectory(brains)

# server_static and server_sim_static have a circular dependency that
# GNU ld resolves with --start-group/--end-group ($<LINK_GROUP:RESCAN>).
# MSVC's link.exe does multi-pass static-lib resolution natively, and
# Apple's ld doesn't support library groups at all -- both also reject
# $<LINK_GROUP> for the CXX link language, so emit plain library names
# there. Use this variable in target_link_libraries wherever both libs
# are linked together.
if(MSVC OR APPLE)
    set(SERVER_LINK_GROUP server_static server_sim_static)
else()
    set(SERVER_LINK_GROUP "$<LINK_GROUP:RESCAN,server_static,server_sim_static>")
endif()

# ---------------------------------------------------------------------------
# WinBolo Client (cross-platform: Windows, Linux, macOS)
# ---------------------------------------------------------------------------

set(CLIENT_SOURCES
    src/bolo/client_mapload.c
    src/bolo/transport_udp_client.c
    src/bolo/nat_portmap.c
    src/gui/ui_mode.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/sdl3imgui.cpp src/gui/sdl3/sdl3imgui_tablet.cpp src/gui/sdl3/luabrainshandler.c src/gui/sdl3/wb_theme.cpp
    src/gui/sdl3/cursor.c src/gui/sdl3/flags.c src/gui/sdl3/dns_lookups.c
    src/gui/sdl3/winbolo.c src/gui/sdl3/gamefront.c
    src/gui/sdl3/dialog_backend.c src/gui/sdl3/dialogs/imgui_welcome.cpp src/gui/sdl3/dialogs/imgui_mapchooser.cpp src/gui/sdl3/dialogs/imgui_udpsetup.cpp src/gui/sdl3/dialogs/imgui_gamebrowser.cpp src/gui/sdl3/dialogs/imgui_setname.cpp src/gui/sdl3/dialogs/imgui_skins.cpp src/gui/sdl3/dialogs/imgui_trackersetup.cpp src/gui/sdl3/dialogs/imgui_winbolonet.cpp src/gui/sdl3/dialogs/imgui_messagebox.cpp src/gui/sdl3/dialogs/imgui_lobby.cpp src/gui/sdl3/dialogs/imgui_settings.cpp src/gui/sdl3/dialogs/imgui_keysetup.cpp src/gui/sdl3/dialogs/imgui_wbn_browser.cpp
    src/gui/sdl3/map_preview_popup.cpp
    src/gui/sdl3/map_preview_view.cpp
    src/gui/sdl3/minimap_render.c
    src/gui/sdl3/bg_game.c src/gui/sdl3/mapview.c src/gui/sdl3/sprite_positions.c src/gui/sdl3/tileloader.c src/gui/sdl3/sdl3draw.c src/gui/sdl3/sdl3draw_status.c src/gui/sdl3/sdl_bmp.c src/gui/sdl3/input.c src/gui/sdl3/input_touch.c
    src/gui/sdl3/tutorial_text.c
    src/gui/sdl3/currentgames.c src/gui/sdl3/lang.c src/gui/sdl3/sound.c
    src/mapeditor/mapeditor.c src/mapeditor/mapeditor_imgui.cpp src/mapeditor/mapgen_imgui.cpp src/mapeditor/mapeditor_undo.c src/mapeditor/mapeditor_validate.c src/mapeditor/mapeditor_symmetry.c src/mapeditor/mapeditor_text.c src/mapeditor/mapeditor_fonts.c src/mapeditor/mapeditor_stats.c src/mapeditor/mapeditor_image.c src/mapeditor/mapeditor_stamp.c src/mapeditor/mapeditor_export.c
    src/third_party/stb/stb_image_write_impl.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/server/server_dedicated_log_stubs.c
    src/winbolonet/winbolonet.c src/winbolonet/http.c src/winbolonet/wbn_comments.cpp
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
    src/logviewer/imgui/imgui_context.cpp
    src/logviewer/imgui/imgui_main_menu.cpp
    src/logviewer/imgui/imgui_controls.cpp
    src/logviewer/imgui/imgui_game_info.cpp
    src/logviewer/imgui/imgui_events.cpp
    src/logviewer/imgui/imgui_item_info.cpp
    src/logviewer/imgui/imgui_dialogs.cpp
    src/logviewer/imgui/imgui_game_viewport.cpp
    src/logviewer/imgui/imgui_game_view.cpp
    src/logviewer/imgui/imgui_comments.cpp
    src/logviewer/platform/platform_config.c
    src/logviewer/platform/platform_dialogs.c
    # Phase D of plans/ctrailer.md — embedded build links empty stubs;
    # the real game-view impls in game_view.c are standalone-only because
    # they include bolo headers that would conflict with the host
    # client's screen-struct layouts.
    src/logviewer/game_view_stubs.c
)
list(APPEND CLIENT_SOURCES ${LOGVIEWER_LIB_SOURCES})

add_executable(WinBolo ${CLIENT_SOURCES})
target_compile_definitions(WinBolo PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

# macOS trackpad pinch-to-zoom support
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(WinBolo PRIVATE src/gui/sdl3/macos_pinch.m)
    target_sources(WinBolo PRIVATE src/gui/sdl3/platform/mac_menubar.mm)
    # Embedded Log Viewer reuses WinBolo's window — the LogViewer NSMenu
    # shim is linked in so gamefront.c can swap menu bars on entry/exit.
    target_sources(WinBolo PRIVATE src/logviewer/platform/mac_menubar.mm)
    # Same story for the embedded Map Editor entry from the welcome
    # screen — the editor swaps WinBolo's menu out and restores it on
    # exit via its own NSMenu shim.
    target_sources(WinBolo PRIVATE src/mapeditor/platform/mac_menubar.mm)
    target_link_libraries(WinBolo PRIVATE "-framework AppKit")
endif()

# macOS app bundle with icon
if(APPLE)
    set(WINBOLO_ICON "${CMAKE_SOURCE_DIR}/data/icons/winbolo.icns")
    if(EXISTS "${WINBOLO_ICON}")
        target_sources(WinBolo PRIVATE "${WINBOLO_ICON}")
        set_source_files_properties("${WINBOLO_ICON}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        set_target_properties(WinBolo PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_ICON_FILE "winbolo.icns"
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
    src/server
    src/mapeditor
    src
    src/logviewer
    src/logviewer/imgui
    src/logviewer/platform
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)
bolo_apply_include_rules(WinBolo gui)
bolo_grant_internal_source_access(WinBolo
    src/bolo/client_mapload.c
    src/bolo/transport_udp_client.c
    src/mapeditor/mapeditor.c
    src/mapeditor/mapeditor_export.c
    src/mapeditor/mapeditor_validate.c)

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
    nanosvg stb_image utf8proc cjson)

# libplum NAT port mapping (optional)
if(HAVE_LIBPLUM)
    target_compile_definitions(WinBolo PRIVATE BOLO_PORTMAP)
    target_link_libraries(WinBolo PRIVATE LibPlum::LibPlum)
endif()

# bolo_static contains ml_brain.c unconditionally; when HAVE_ONNXRUNTIME
# is set the lib was compiled with the real path and PUBLIC-links
# onnxruntime, so consumers inherit the link without repeating it.
target_link_libraries(WinBolo PRIVATE
    bolo_static
    ${SERVER_LINK_GROUP}
    bot_brains_static)

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
            # Skip .dSYM bundles — they are directories, not files, so
            # copy_if_different fails on them. They are debug symbols that
            # don't need to ship next to the binary.
            if(_f MATCHES "\\.dSYM$")
                continue()
            endif()
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
    src/bolo/transport_udp_client.c
    src/bolo/nat_portmap.c
    src/server/servermain.c
    src/server/server_dedicated_log.c
    src/server/server_frontend_stubs.c
    src/gui/sdl3/luabrainshandler.c
    src/headless/cmd_stdin.c
    src/winbolonet/winbolonet.c src/winbolonet/http.c src/winbolonet/wbn_comments.cpp
    src/winbolonet/winbolonetthread.c src/winbolonet/winbolonetevents.c src/third_party/tweetnacl/tweetnacl.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
)


add_executable(WinBoloDS ${SERVER_SOURCES})
target_sources(WinBoloDS PRIVATE src/steam/steam_wrapper_stub.c)
target_compile_definitions(WinBoloDS PRIVATE
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(WinBoloDS PRIVATE
    src/gui/sdl3
    src/gui
    src/server
    src/mapeditor
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)
bolo_apply_include_rules(WinBoloDS runtime_only)
bolo_grant_internal_source_access(WinBoloDS
    src/bolo/transport_udp_client.c
    src/server/servermain.c
    src/server/server_frontend_stubs.c
    src/server/server_dedicated_log.c)

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

target_link_libraries(WinBoloDS PRIVATE
    bolo_static
    ${SERVER_LINK_GROUP}
    bot_brains_static)
target_link_libraries(WinBoloDS PRIVATE minizip SDL3::SDL3 CURL::libcurl lua_static cjson utf8proc)

# libplum NAT port mapping (optional). nat_portmap.c stays per-target
# so its #ifdef BOLO_PORTMAP block compiles against this binary's view.
if(HAVE_LIBPLUM)
    target_compile_definitions(WinBoloDS PRIVATE BOLO_PORTMAP)
    target_link_libraries(WinBoloDS PRIVATE LibPlum::LibPlum)
endif()

# Sentry crash reporting (always compile — stubs when HAVE_SENTRY is off)
target_sources(WinBoloDS PRIVATE src/common/sentry_integration.c)
target_sources(WinBoloDS PRIVATE src/common/wb_log.c)
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
# bolo_shim.c and game_view.c are added explicitly via LOGVIEWER_IMGUI_SOURCES
# (Phases B and D). They include bolo headers directly and would conflict
# with logviewer/backend.h if pulled into a TU that also sees logviewer types.
list(FILTER LOGVIEWER_SOURCES EXCLUDE REGEX ".*logviewer/bolo_shim\\.c$")
list(FILTER LOGVIEWER_SOURCES EXCLUDE REGEX ".*logviewer/game_view\\.c$")
# game_view_stubs.c is the embedded-only stub set; standalone uses the
# real game_view.c via LOGVIEWER_IMGUI_SOURCES — drop the stubs from the
# glob so the standalone link doesn't see duplicate symbols.
list(FILTER LOGVIEWER_SOURCES EXCLUDE REGEX ".*logviewer/game_view_stubs\\.c$")

set(LOGVIEWER_IMGUI_SOURCES
    src/logviewer/imgui/imgui_context.cpp
    src/logviewer/imgui/imgui_main_menu.cpp
    src/logviewer/imgui/imgui_controls.cpp
    src/logviewer/imgui/imgui_game_info.cpp
    src/logviewer/imgui/imgui_events.cpp
    src/logviewer/imgui/imgui_item_info.cpp
    src/logviewer/imgui/imgui_dialogs.cpp
    src/logviewer/imgui/imgui_game_viewport.cpp
    src/logviewer/imgui/imgui_game_view.cpp
    src/logviewer/imgui/imgui_comments.cpp
    src/gui/sdl3/mapview.c
    src/gui/sdl3/sdl3draw_status.c
    src/logviewer/bolo_shim.c
    src/logviewer/game_view.c
)

# Reuse the SDL3 main-game WBN browser dialog. LogViewer provides stubs for
# the few main-game-only symbols it pulls in (bg_game, gamefront token,
# imgui_winbolonet sign-in section). The dialog is *not* added to
# LOGVIEWER_LIB_SOURCES because the WinBolo client already links its own copy.
set(LOGVIEWER_WBN_BROWSER_SOURCES
    src/gui/sdl3/dialogs/imgui_wbn_browser.cpp
    src/logviewer/imgui/imgui_lv_browser_stubs.cpp
)

set(LOGVIEWER_PLATFORM_SOURCES
    src/logviewer/platform/platform_config.c
    src/logviewer/platform/platform_dialogs.c
)

# WinBolo.net HTTP client + Ed25519 signing — shared with the SDL3 client and
# servers. LogViewer uses it to fetch/post comments on logs that have a WBN key.
set(LOGVIEWER_WBN_SOURCES
    src/winbolonet/http.c
    src/winbolonet/wbn_comments.cpp
    src/third_party/tweetnacl/tweetnacl.c
)

add_executable(LogViewer
    ${LOGVIEWER_SOURCES}
    ${LOGVIEWER_IMGUI_SOURCES}
    ${LOGVIEWER_PLATFORM_SOURCES}
    ${LOGVIEWER_WBN_SOURCES}
    ${LOGVIEWER_WBN_BROWSER_SOURCES}
    src/gui/sdl3/lang.c
    src/gui/sdl3/gamefront_stubs.c
    src/gui/sdl3/sdl_bmp.c
    src/gui/sdl3/sprite_positions.c
    src/gui/sdl3/tileloader.c
    src/gui/ui_mode.c
)
if(NOT WIN32)
    # posix_stubs provides preferencesGetPreferenceFile + GetPrivateProfileString /
    # WritePrivateProfileString shared with the main game and map editor.
    target_sources(LogViewer PRIVATE src/server/posix_stubs.c)
endif()
target_compile_definitions(LogViewer PRIVATE
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
    PREFERENCE_FILE="WinBolo.ini"
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
    set_target_properties(LogViewer PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/logviewer/platform/Info.plist"
    )
    set(APP_ICON_ICNS "${CMAKE_SOURCE_DIR}/data/icons/logviewer.icns")
    if(EXISTS "${APP_ICON_ICNS}")
        target_sources(LogViewer PRIVATE "${APP_ICON_ICNS}")
        set_source_files_properties("${APP_ICON_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        set_target_properties(LogViewer PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "logviewer.icns")
    endif()
endif()

target_include_directories(LogViewer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/logviewer
    ${CMAKE_SOURCE_DIR}/src/logviewer/imgui
    ${CMAKE_SOURCE_DIR}/src/logviewer/platform
    ${CMAKE_SOURCE_DIR}/src/winbolonet
    ${CMAKE_SOURCE_DIR}/src/third_party/tweetnacl
    ${cjson_SOURCE_DIR}
)
bolo_apply_include_rules(LogViewer runtime_only)

target_link_libraries(LogViewer PRIVATE
    minizip SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui_backends
    nanosvg stb_image CURL::libcurl cjson
)

if(WIN32)
    target_link_libraries(LogViewer PRIVATE ws2_32)
endif()
if(MATH_LIB)
    target_link_libraries(LogViewer PRIVATE ${MATH_LIB})
endif()

# macOS trackpad pinch-to-zoom support, plus native NSMenu shim for the
# standalone Log Viewer.app menu bar.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(LogViewer PRIVATE src/gui/sdl3/macos_pinch.m)
    target_sources(LogViewer PRIVATE src/logviewer/platform/mac_menubar.mm)
    target_link_libraries(LogViewer PRIVATE "-framework AppKit")
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
    src/bolo/client_mapload.c
    src/bolo/transport_udp_client.c
    src/bolo/nat_portmap.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/currentgames.c src/gui/sdl3/lang.c
    src/gui/sdl3/dns_lookups.c
    src/headless/headless_main.c src/headless/headless_frontend.c
    src/headless/cmd_stdin.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/server/server_dedicated_log_stubs.c
    src/winbolonet/winbolonet.c src/winbolonet/http.c src/winbolonet/wbn_comments.cpp
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
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(WinBoloHeadless PRIVATE
    src/gui/sdl3
    src/gui
    src/server
    src/headless
    src/mapeditor
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)
bolo_apply_include_rules(WinBoloHeadless runtime_only)
bolo_grant_internal_source_access(WinBoloHeadless
    src/bolo/client_mapload.c
    src/bolo/transport_udp_client.c)

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
    bolo_static
    ${SERVER_LINK_GROUP}
    bot_brains_static)
target_link_libraries(WinBoloHeadless PRIVATE
    SDL3::SDL3 minizip lua_static CURL::libcurl cjson utf8proc)

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
    src/bolo/client_mapload.c
    src/bolo/transport_udp_client.c
    src/bolo/nat_portmap.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/currentgames.c src/gui/sdl3/lang.c
    src/gui/sdl3/dns_lookups.c
    src/headless/headless_frontend.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/server/server_dedicated_log_stubs.c
    src/winbolonet/winbolonet.c src/winbolonet/http.c src/winbolonet/wbn_comments.cpp
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
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(winbolo_gym PRIVATE
    src/gui/sdl3
    src/gui
    src/server
    src/headless
    src/mapeditor
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)
bolo_apply_include_rules(winbolo_gym gym)

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
    bolo_static
    ${SERVER_LINK_GROUP}
    bot_brains_static)
target_link_libraries(winbolo_gym PRIVATE
    SDL3::SDL3 minizip lua_static CURL::libcurl cjson utf8proc)

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
# lua_strip — generic tool: strips named call-statement patterns from Lua
# source files and syntax-checks the result via luaL_loadfile.
# Brains use this to produce a debug-free opt/ copy; see each brain's
# strip.bat / strip.sh for the per-bot invocation.
# ---------------------------------------------------------------------------
add_executable(lua_strip src/tools/lua_strip.c)
target_link_libraries(lua_strip PRIVATE lua_static)
if(WIN32)
    target_link_libraries(lua_strip PRIVATE winmm)
endif()

# ---------------------------------------------------------------------------
# BrainTest (standalone brain debug viewer with overlays)
# ---------------------------------------------------------------------------

set(BRAINTEST_SOURCES
    src/bolo/client_mapload.c
    src/bolo/transport_udp_client.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/lang.c
    src/gui/sdl3/mapview.c src/gui/sdl3/sprite_positions.c src/gui/sdl3/tileloader.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/server/server_stubs.c
    src/server/server_dedicated_log_stubs.c
    src/braintest/braintest_main.c src/braintest/braintest_frontend.c
    src/braintest/braintest_viz_registry.c
    src/braintest/braintest_vizwindow.cpp
    src/braintest/braintest_panel_registry.c
    src/braintest/braintest_panel_types.c
    src/braintest/braintest_panelwindow.cpp
    src/braintest/braintest_botwindow.cpp
    src/braintest/braintest_mainimgui.cpp
    src/braintest/braintest_shotsimpanel.cpp
    src/braintest/braintest_shotsim_poi_registry.c
    src/braintest/braintest_vizdetail_registry.c
    src/braintest/braintest_vizdetailwindow.cpp
    src/braintest/braintest_pillcontrib_registry.c
)

# Per-bot BrainTest panel modules. Each bot may ship its own set of
# C++ panel renderers under brains/<bot>/braintest_panels/*.cpp; they
# self-register via static initializers using panelTypeRegister().
# Glob is build-time — adding a new bot's panels needs a CMake re-run.
file(GLOB BRAINTEST_BOT_PANEL_SOURCES
    "${CMAKE_SOURCE_DIR}/brains/*/braintest_panels/*.cpp")
list(APPEND BRAINTEST_SOURCES ${BRAINTEST_BOT_PANEL_SOURCES})


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
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(BrainTest PRIVATE
    src/gui/sdl3
    src/gui
    src/server
    src/braintest
    src/mapeditor
    ${cjson_SOURCE_DIR}
)
bolo_apply_include_rules(BrainTest braintest)

# braintest_vizwindow.cpp uses Dear ImGui (SDL3 + SDL_Renderer
# backends). imgui_backends carries the headers + the impl libs.
target_link_libraries(BrainTest PRIVATE imgui_backends)

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

target_link_libraries(BrainTest PRIVATE bolo_static server_sim_static bot_brains_static threads_static)
target_link_libraries(BrainTest PRIVATE
    SDL3::SDL3 minizip lua_static
    nanosvg stb_image utf8proc cjson
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
    src/bolo/client_mapload.c
    src/bolo/transport_udp_client.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/minimap_render.c
    src/gui/sdl3/lang.c
    src/gui/sdl3/gamefront_stubs.c
    src/gui/sdl3/mapview.c src/gui/sdl3/sprite_positions.c src/gui/sdl3/tileloader.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/server/server_stubs.c
    src/server/server_dedicated_log_stubs.c
    src/mapeditor/mapeditor.c src/mapeditor/mapeditor_imgui.cpp src/mapeditor/mapgen_imgui.cpp src/mapeditor/mapeditor_undo.c src/mapeditor/mapeditor_validate.c src/mapeditor/mapeditor_symmetry.c src/mapeditor/mapeditor_text.c src/mapeditor/mapeditor_fonts.c src/mapeditor/mapeditor_stats.c src/mapeditor/mapeditor_image.c src/mapeditor/mapeditor_stamp.c src/mapeditor/mapeditor_export.c
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
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(MapEditor PRIVATE
    src/gui/sdl3
    src/gui
    src/server
    src/mapeditor
)
bolo_apply_include_rules(MapEditor mapeditor)

if(WIN32)
    target_sources(MapEditor PRIVATE src/res/mapeditor.rc)
    target_link_libraries(MapEditor PRIVATE ws2_32 winmm user32)
    target_compile_options(MapEditor PRIVATE -fcommon)
else()
    target_sources(MapEditor PRIVATE
        src/server/posix_stubs.c
    )
    target_link_libraries(MapEditor PRIVATE ${MATH_LIB} pthread)
    target_compile_options(MapEditor PRIVATE -fcommon)
endif()

target_link_libraries(MapEditor PRIVATE bolo_static server_sim_static bot_brains_static threads_static)
target_link_libraries(MapEditor PRIVATE
    SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui_backends minizip lua_static
    nanosvg stb_image utf8proc
)

# macOS trackpad pinch-to-zoom support, plus native NSMenu shim for the
# Map Editor's menu bar (used by both the standalone MapEditor.app and
# the embedded entry from WinBolo's welcome screen).
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(MapEditor PRIVATE src/gui/sdl3/macos_pinch.m)
    target_sources(MapEditor PRIVATE src/mapeditor/platform/mac_menubar.mm)
    target_link_libraries(MapEditor PRIVATE "-framework AppKit")
endif()

# macOS app bundle with icon
if(APPLE)
    set(MAPEDITOR_ICON "${CMAKE_SOURCE_DIR}/data/icons/mapeditor.icns")
    if(EXISTS "${MAPEDITOR_ICON}")
        target_sources(MapEditor PRIVATE "${MAPEDITOR_ICON}")
        set_source_files_properties("${MAPEDITOR_ICON}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        set_target_properties(MapEditor PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_ICON_FILE "mapeditor.icns"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/mapeditor/platform/Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.winbolo.mapeditor"
            MACOSX_BUNDLE_BUNDLE_NAME "WinBolo Map Editor")
    endif()
endif()

# Copy shared libraries and assets next to MapEditor executable
add_custom_command(TARGET MapEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:MapEditor>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3_ttf::SDL3_ttf>" "$<TARGET_FILE_DIR:MapEditor>"
    COMMENT "Copying SDL3 and SDL3_ttf shared libraries to MapEditor output directory"
)

# bot_brains_static pulls in onnxruntime, so MapEditor inherits the
# dylib dependency. Mirror the WinBolo POST_BUILD copy step so the
# .app bundle ships with the dylib next to the executable — without
# this, launching the standalone MapEditor.app fails at dyld load
# time with "Library not loaded: @rpath/libonnxruntime.*.dylib".
if(HAVE_ONNXRUNTIME)
    if(WIN32 AND DEFINED ONNXRUNTIME_DLL)
        add_custom_command(TARGET MapEditor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:MapEditor>"
            COMMENT "Copying ONNX Runtime DLL to MapEditor output directory"
        )
    elseif(DEFINED ONNXRUNTIME_LIBDIR)
        file(GLOB _ORT_ALL_SO "${ONNXRUNTIME_LIBDIR}/libonnxruntime*")
        foreach(_f ${_ORT_ALL_SO})
            # Skip .dSYM bundles — they are directories, not files, so
            # copy_if_different fails on them. They are debug symbols that
            # don't need to ship next to the binary.
            if(_f MATCHES "\\.dSYM$")
                continue()
            endif()
            add_custom_command(TARGET MapEditor POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" "$<TARGET_FILE_DIR:MapEditor>"
            )
        endforeach()
    endif()
endif()
if(APPLE)
    add_custom_command(TARGET MapEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data"
            "$<TARGET_BUNDLE_CONTENT_DIR:MapEditor>/Resources/data"
        COMMENT "Copying data files into MapEditor app bundle Resources"
    )
else()
    add_custom_command(TARGET MapEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data" "$<TARGET_FILE_DIR:MapEditor>/data"
        COMMENT "Copying MapEditor data files to output directory"
    )
endif()

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

# ---------------------------------------------------------------------------
# macOS code-signing + notarization + stapling
# (cmake --build <dir> --target sign_macos)
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_custom_target(sign_macos
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/sign_macos.sh" "${CMAKE_BINARY_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Sign + notarize + staple all .app bundles (requires Developer ID cert)"
        VERBATIM
        USES_TERMINAL
    )

    add_custom_target(package_macos
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/package_macos.sh" "${CMAKE_BINARY_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS sign_macos
        COMMENT "Bundle signed apps + WinBoloDS into a notarized DMG (requires create-dmg)"
        VERBATIM
        USES_TERMINAL
    )
endif()

enable_testing()

# Per-scenario CTest entries. Scenario names must stay in sync with the
# dispatch_scenario case in tests/baseline/run.sh. --fast scenarios have
# no shared state and parallelize freely. UDP scenarios share a hardcoded
# port per helper, so each helper's scenarios get a matching RESOURCE_LOCK
# — different helpers (different ports) still run concurrently.
set(_baseline_fast
    everard_island_1bot_idle
    everard_island_1bot_sit
    everard_island_1bot_drive
    everard_island_1bot_shoot
    everard_island_1bot_watch
    forest_rig_1bot_idle
    forest_rig_1bot_sit
    forest_rig_1bot_drive
    forest_rig_1bot_shoot
    forest_rig_1bot_watch
    slugfest_iv_1bot_idle
    slugfest_iv_1bot_sit
    slugfest_iv_1bot_drive
    slugfest_iv_1bot_shoot
    slugfest_iv_1bot_watch
    centralize_events_fast
    centralize_events_teams_fast
    centralize_events_alliance_fast
    centralize_events_name_change_fast
    centralize_events_map_skip_fast
)
set(_baseline_udp_50001 ds_4bot_melee ds_2v2_team)
set(_baseline_udp_50002 centralize_events_udp)
set(_baseline_udp_50003
    centralize_events_name_change_udp
    centralize_events_map_skip_udp
    centralize_events_alliance_leave_udp
)
set(_baseline_udp_50004 centralize_events_shutdown_udp)
set(_baseline_udp_50005 centralize_events_alliance_2client_udp)
set(_baseline_udp_50006 centralize_events_game_over_udp)
set(_baseline_udp_50007
    centralize_events_lobby_smoke_udp
    centralize_events_lobby_name_change_udp
)
set(_baseline_udp_50008
    centralize_events_chat_unicast_3client_udp
    centralize_events_chat_alliance_3client_udp
)

foreach(_name IN LISTS _baseline_fast)
    add_test(NAME baseline.${_name}
             COMMAND stdbuf -oL bash ${CMAKE_SOURCE_DIR}/tests/baseline/run.sh
                          --scenario ${_name}
                          $<TARGET_FILE:WinBoloHeadless>
                          $<TARGET_FILE:WinBoloDS>)
    set_tests_properties(baseline.${_name} PROPERTIES TIMEOUT 60)
endforeach()

foreach(_port 50001 50002 50003 50004 50005 50006 50007 50008)
    foreach(_name IN LISTS _baseline_udp_${_port})
        add_test(NAME baseline.${_name}
                 COMMAND stdbuf -oL bash ${CMAKE_SOURCE_DIR}/tests/baseline/run.sh
                              --scenario ${_name}
                              $<TARGET_FILE:WinBoloHeadless>
                              $<TARGET_FILE:WinBoloDS>)
        set_tests_properties(baseline.${_name} PROPERTIES
                             TIMEOUT 60
                             RESOURCE_LOCK "udp_${_port}")
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# WinBoloUnitTests — in-process tests of bolo_static / server_sim_static
# / server_static without the SDL3 GUI. Each test is a registered name
# dispatched from tests/unit/test_main.c; CTest entries below run them
# one at a time.
#
# Uses the `unittests` privileged include profile defined in
# cmake/bolo_lib.cmake; scope and expiry documented in
# docs/ARCHITECTURE.md.
# ---------------------------------------------------------------------------
set(WINBOLO_UNITTESTS_SOURCES
    tests/unit/test_main.c
    tests/unit/test_stubs.c
    tests/unit/test_transport_local_passive_threads.c
    tests/unit/test_sp_subscriber_delivery.c
    tests/unit/test_active_local_input_to_shot.c
    tests/unit/test_sp_shoot_through_timer.c
    tests/unit/test_ini_reader_writer.c
    tests/unit/test_lobby_control_variants.c
    # transport_udp_client.c stays per-target because of HAVE_STEAM
    # gating (it's excluded from bolo_static for that reason). Linked
    # here without HAVE_STEAM so client_net.c's transportUdpClient*
    # references resolve — the tests never run the UDP code path.
    src/bolo/transport_udp_client.c
    # server_stubs.c (transportUdpServer* / serverLifecycle* stubs)
    # is intentionally NOT linked here: server_static (linked below)
    # provides the real transport_udp_server.c and server_lifecycle.c
    # bodies, and stubbing them too would collide at link time. The
    # client-side hooks server_lifecycle.c still references but
    # server_static doesn't own (winbolonet* in src/winbolonet/,
    # natPortMap* in src/bolo/nat_portmap.c) stay stubbed in
    # test_stubs.c — acceptRemoteClients=false keeps them off the
    # tick-time call paths.
    src/server/server_dedicated_log_stubs.c
    src/lzw/dcodlzw.c
    src/lzw/ecodlzw.c
)

add_executable(WinBoloUnitTests ${WINBOLO_UNITTESTS_SOURCES})
target_sources(WinBoloUnitTests PRIVATE src/steam/steam_wrapper_stub.c)
target_compile_definitions(WinBoloUnitTests PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(WinBoloUnitTests PRIVATE
    src/gui/sdl3
    src/server
    src
    ${CMAKE_SOURCE_DIR}/tests/unit
    ${cjson_SOURCE_DIR}
)
bolo_apply_include_rules(WinBoloUnitTests unittests)

target_link_libraries(WinBoloUnitTests PRIVATE
    bolo_static ${SERVER_LINK_GROUP} bot_brains_static
    SDL3::SDL3 minizip lua_static cjson utf8proc)

if(WIN32)
    target_link_libraries(WinBoloUnitTests PRIVATE ws2_32 winmm user32)
    target_compile_options(WinBoloUnitTests PRIVATE -fcommon)
else()
    target_sources(WinBoloUnitTests PRIVATE src/server/posix_stubs.c)
    target_link_libraries(WinBoloUnitTests PRIVATE ${MATH_LIB} pthread)
    target_compile_options(WinBoloUnitTests PRIVATE -fcommon)
endif()

set_flat_output(WinBoloUnitTests "${CMAKE_BINARY_DIR}")

set(_unit_test_names
    transport_local_passive_threads
    sp_subscriber_delivery
    active_local_input_to_shot
    sp_shoot_through_timer
    ini_writer_persistence
    ini_writer_insertion_point
    ini_writer_security
    lobby_settings_codec_and_apply
    lobby_team_meta_codec_and_apply
    lobby_bot_config_codec_and_apply
    lobby_bot_brain_codec_and_apply
    lobby_brain_list_codec_and_apply
)
foreach(_name IN LISTS _unit_test_names)
    add_test(NAME unit.${_name}
             COMMAND $<TARGET_FILE:WinBoloUnitTests> --test ${_name})
    set_tests_properties(unit.${_name} PROPERTIES TIMEOUT 60)
endforeach()

endif() # iOS vs Desktop
