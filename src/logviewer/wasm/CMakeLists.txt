###
# LogViewer WebAssembly Build
#
# Standalone CMake project — does not modify the main CMakeLists.txt.
# Usage:
#   cd winbolo
#   emcmake cmake -B build-wasm -S src/logviewer/wasm
#   cmake --build build-wasm
#
# Output: build-wasm/logviewer.html, logviewer.js, logviewer.wasm, logviewer.data
# Packaged for deployment: cmake --build build-wasm --target wasm-dist
#
# Serve locally with: python3 -m http.server -d build-wasm
#

cmake_minimum_required(VERSION 3.15)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")

project(LogViewerWasm C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# Root of the repo (3 levels up from this CMakeLists.txt: wasm/ -> logviewer/ -> src/ -> repo)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(SRC_ROOT "${REPO_ROOT}/src")
set(LV_ROOT  "${SRC_ROOT}/logviewer")
set(DATA_DIR "${REPO_ROOT}/data")

include(${REPO_ROOT}/cmake/bolo_lib.cmake)
include(${REPO_ROOT}/cmake/bolo_version.cmake)

###
# Fetch dependencies (same versions as main build)
#

# Tell SDL3_ttf to build freetype from source (avoids system dependency,
# required for Emscripten cross-compilation)
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.14
)
FetchContent_Declare(sdl3_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG        release-3.2.0
)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
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

# cJSON as a static library, with its tests and install rules off.
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UNINSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(sdl3 sdl3_ttf imgui zlib cjson)

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
# nanosvg + stb_image — vendored header-only libraries used by the
# tile atlas builder (gui/sdl3/tileloader.c). Each provides a single
# *_impl.c file that defines the implementation; everything else is
# header-only.
#
add_library(nanosvg STATIC ${SRC_ROOT}/third_party/nanosvg/nanosvg_impl.c)
target_include_directories(nanosvg SYSTEM PUBLIC ${SRC_ROOT}/third_party/nanosvg)

add_library(stb_image STATIC ${SRC_ROOT}/third_party/stb/stb_image_impl.c)
target_include_directories(stb_image SYSTEM PUBLIC ${SRC_ROOT}/third_party/stb)

###
# Gather LogViewer source files — same as main build, but swap out
# main.c, clientmutex.c, dns.c, and platform/*.c for WASM versions.
#

file(GLOB LV_SOURCES
    ${LV_ROOT}/*.c
    ${SRC_ROOT}/common/*.c
)

# Exclude files we're replacing with WASM versions
list(FILTER LV_SOURCES EXCLUDE REGEX ".*main\\.c$")
list(FILTER LV_SOURCES EXCLUDE REGEX ".*logviewer\\.c$")
# lv_embed.c stays in: main_wasm.c replaces logviewer.c, and lv_embed.c holds
# the half of the host every build shares -- g_lv, lvHostSetupCore and the
# replay driver.
list(FILTER LV_SOURCES EXCLUDE REGEX ".*clientmutex\\.c$")
# The game view is the desktop viewer's; main_wasm.c has no key to enter it.
# game_view_stubs.c answers draw.c's calls, as it does in the wasm client.
list(FILTER LV_SOURCES EXCLUDE REGEX ".*logviewer/game_view\\.c$")
list(FILTER LV_SOURCES EXCLUDE REGEX ".*dns\\.c$")

# Exclude files not needed in LogViewer
list(FILTER LV_SOURCES EXCLUDE REGEX ".*memoryblock\\.c$")
list(FILTER LV_SOURCES EXCLUDE REGEX ".*dialogfullscreensetup\\.c$")
list(FILTER LV_SOURCES EXCLUDE REGEX ".*font\\.c$")
list(FILTER LV_SOURCES EXCLUDE REGEX ".*dialogWindowSize\\.c$")
list(FILTER LV_SOURCES EXCLUDE REGEX ".*dialogteamcolours\\.c$")
# common/util.c is Win32-only
list(FILTER LV_SOURCES EXCLUDE REGEX ".*common/util\\.c$")

# ImGui integration (unchanged)
set(LV_IMGUI_SOURCES
    ${LV_ROOT}/imgui/imgui_context.cpp
    ${LV_ROOT}/imgui/imgui_main_menu.cpp
    ${LV_ROOT}/imgui/imgui_controls.cpp
    ${LV_ROOT}/imgui/imgui_game_info.cpp
    ${LV_ROOT}/imgui/imgui_events.cpp
    ${LV_ROOT}/imgui/imgui_item_info.cpp
    ${LV_ROOT}/imgui/imgui_dialogs.cpp
    ${LV_ROOT}/imgui/imgui_game_viewport.cpp
    ${LV_ROOT}/imgui/imgui_scenario_panel.cpp
)

# WASM replacement files
set(LV_WASM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/clientmutex_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dns_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform_config_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform_dialogs_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/wbn_stubs_wasm.c
)

###
# Build the WASM executable
#
add_executable(logviewer
    ${LV_SOURCES}
    ${LV_IMGUI_SOURCES}
    ${LV_WASM_SOURCES}
    ${SRC_ROOT}/gui/sdl3/sdl_bmp.c
    # The replay's smart-ping markers load their glyphs through this, the
    # same loader the game's overlay uses.
    ${SRC_ROOT}/gui/sdl3/ping_icons.c
    ${SRC_ROOT}/gui/sdl3/sprite_positions.c
    ${SRC_ROOT}/gui/sdl3/tileloader.c
    ${SRC_ROOT}/gui/sdl3/skin_source.c
    ${SRC_ROOT}/gui/sdl3/gfx_settings.c
    # The zoomed-out ground colours, which draw.c fills a square with below
    # 1x, and the shapes it draws on top of them. Plain SDL, the tile numbers
    # and ring_band.c, which is here for the disc a pillbox marker is built
    # from and which nothing else in this target pulls in.
    ${SRC_ROOT}/gui/sdl3/map_colours.c
    ${SRC_ROOT}/gui/sdl3/map_markers.c
    ${SRC_ROOT}/gui/sdl3/ring_band.c
    # The tablet / Steam Deck mode the ImGui panels lay out by, the game-front
    # calls it and the panels make, and the keyboard-nav outline
    # imgui_context.cpp draws. The standalone viewer builds the same three.
    ${SRC_ROOT}/gui/ui_mode.c
    ${SRC_ROOT}/gui/sdl3/gamefront_stubs.c
    ${SRC_ROOT}/gui/sdl3/dialogs/imgui_nav_outline.cpp
    # langGetText / langGetTextFmt, which imgui_game_info.cpp and
    # sim_rules_phrase.c call. Plain SDL and libc.
    ${SRC_ROOT}/gui/sdl3/lang.c
    ${SRC_ROOT}/gui/sdl3/sim_rules_phrase.c
    # The viewer names a rule, phrases a change and parses a scenario panel.
    # This target links no bolo_static, so the rule tables and the panel
    # parser compile in directly. Both call nothing but libc.
    ${SRC_ROOT}/bolo/sim_rules.c
    ${SRC_ROOT}/bolo/scenario_panel.c
    # The panel square's drawer, the game's own. It needs ImGui, the public
    # scenario_panel.h and sprite_positions.c above, and nothing else.
    ${SRC_ROOT}/gui/sdl3/scenario_panel_draw.cpp
)

target_include_directories(logviewer PRIVATE
    ${SRC_ROOT}
    ${LV_ROOT}
    ${LV_ROOT}/imgui
    ${LV_ROOT}/platform
    ${cjson_SOURCE_DIR}
)
bolo_apply_include_rules(logviewer runtime_only)
# sim_rules.c spells the classic values in the sim's own constants, which
# live in internal/ headers.
bolo_grant_internal_source_access(logviewer
    ${SRC_ROOT}/bolo/sim_rules.c)
# sim_rules.c reaches brain.h through control_event.h, and brain.h uses
# u_char / u_short, which Emscripten's sys/types.h declares only under
# _GNU_SOURCE. The wasm game client sets it for its whole target; here only
# this file needs it.
set_source_files_properties(${SRC_ROOT}/bolo/sim_rules.c
    TARGET_DIRECTORY logviewer
    PROPERTIES COMPILE_DEFINITIONS _GNU_SOURCE)

# Version constants, from the same cmake/bolo_version.cmake the root project
# and the wasm game client use, so all three report one version. screen.c's
# spectator seed-load path stamps BOLO_VERSION_* into the published game info.
# PREFERENCE_FILE is not passed here — lv_global.h already defines it.
target_compile_definitions(logviewer PRIVATE
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_link_libraries(logviewer PRIVATE
    minizip
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    imgui_backends
    nanosvg
    stb_image
    cjson
)

###
# Emscripten-specific settings
#

# ---------------------------------------------------------------------------
# CJK font subsetting — same as the game client (src/wasm/CMakeLists.txt). The
# bundled Noto Sans CJK + Sarasa Mono Slab fonts are ~163 MB and dominate the
# preload; subset them to the glyph ranges parsed from imgui_draw.cpp (~19 MB).
# The logviewer renders replays' player names and chat, so it inherits the same
# Sarasa coverage trade-off. Needs Python 3 + fonttools; falls back to the full
# fonts with a warning otherwise.
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

# Preload data files so they're available in the virtual filesystem
set(EMSCRIPTEN_LINK_FLAGS "")
string(APPEND EMSCRIPTEN_LINK_FLAGS " --preload-file ${DATA_DIR}@/data")
# The licence and its additional permissions, which every copy of the program
# has to carry.
string(APPEND EMSCRIPTEN_LINK_FLAGS " --preload-file ${REPO_ROOT}/LICENSE@/data/LICENSE")
string(APPEND EMSCRIPTEN_LINK_FLAGS " --preload-file ${REPO_ROOT}/LICENSE-EXCEPTION.md@/data/LICENSE-EXCEPTION.md")

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
    add_dependencies(logviewer subset_fonts)

    # Exclude the full CJK fonts (exact source paths, no wildcards) and layer
    # the subset versions over /data/fonts.
    foreach(_f IN LISTS _cjk_fonts)
        string(APPEND EMSCRIPTEN_LINK_FLAGS " --exclude-file ${DATA_DIR}/fonts/${_f}")
    endforeach()
    string(APPEND EMSCRIPTEN_LINK_FLAGS " --preload-file ${SUBSET_FONTS_DIR}@/data/fonts")
else()
    message(WARNING
        "CJK font subsetting disabled (need Python 3 + fonttools: "
        "`pip install fonttools brotli`) — preloading the full ~163 MB fonts")
endif()

# Export the wasm_open_log_file function so JS can call it
string(APPEND EMSCRIPTEN_LINK_FLAGS " -sEXPORTED_FUNCTIONS=['_main','_wasm_open_log_file','_malloc','_free']")
string(APPEND EMSCRIPTEN_LINK_FLAGS " -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','stringToNewUTF8','stringToUTF8','UTF8ToString','FS']")

# Memory settings
string(APPEND EMSCRIPTEN_LINK_FLAGS " -sALLOW_MEMORY_GROWTH=1")
string(APPEND EMSCRIPTEN_LINK_FLAGS " -sINITIAL_MEMORY=67108864")  # 64MB

# We build SDL3 from source via FetchContent — do NOT use Emscripten's
# built-in SDL port (-sUSE_SDL) as it would conflict.

# Generate HTML shell
string(APPEND EMSCRIPTEN_LINK_FLAGS " --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html")

# Filesystem support (needed for writing uploaded files to /tmp)
string(APPEND EMSCRIPTEN_LINK_FLAGS " -sFORCE_FILESYSTEM=1")

set_target_properties(logviewer PROPERTIES
    SUFFIX ".html"
    LINK_FLAGS "${EMSCRIPTEN_LINK_FLAGS}"
)

# Force a relink (regenerating the preloaded .data) when the subset fonts
# change but no object file did — e.g. a source font or ImGui bump.
if(_have_fonttools)
    set_property(TARGET logviewer APPEND PROPERTY LINK_DEPENDS "${SUBSET_STAMP}")
endif()

# wasm-dist: precompressed web assets and licence files in wasm-dist.zip, the
# same target the game client has (see cmake/wasm_dist.cmake).
include(${REPO_ROOT}/cmake/wasm_dist.cmake)
bolo_add_wasm_dist(logviewer
    COMPRESS logviewer.html logviewer.js logviewer.wasm logviewer.data)
