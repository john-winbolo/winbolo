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

###
# Fetch dependencies (same versions as main build)
#

# Tell SDL3_ttf to build freetype from source (avoids system dependency,
# required for Emscripten cross-compilation)
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
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
)
FetchContent_MakeAvailable(sdl3 sdl3_ttf imgui zlib)

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
list(FILTER LV_SOURCES EXCLUDE REGEX ".*clientmutex\\.c$")
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
)

# WASM replacement files
set(LV_WASM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/clientmutex_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dns_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform_config_wasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform_dialogs_wasm.c
)

###
# Build the WASM executable
#
add_executable(logviewer
    ${LV_SOURCES}
    ${LV_IMGUI_SOURCES}
    ${LV_WASM_SOURCES}
    ${SRC_ROOT}/gui/sdl3/sdl_bmp.c
)

target_include_directories(logviewer PRIVATE
    ${SRC_ROOT}
    ${LV_ROOT}
    ${LV_ROOT}/imgui
    ${LV_ROOT}/platform
)

target_link_libraries(logviewer PRIVATE
    minizip
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    imgui_backends
)

###
# Emscripten-specific settings
#

# Preload data files so they're available in the virtual filesystem
set(EMSCRIPTEN_LINK_FLAGS "")
string(APPEND EMSCRIPTEN_LINK_FLAGS " --preload-file ${DATA_DIR}@/data")

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
