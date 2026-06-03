# WinBolo version constants shared between the root CMake project and
# the standalone wasm sub-project (src/wasm/CMakeLists.txt).
#
# Sets:
#   WINBOLO_VERSION         — semantic version string (e.g. "1.19"); cache
#                             string so a -D override sticks across configures
#   WINBOLO_GIT_HASH        — `git rev-parse --short HEAD`, or "unknown"
#   WINBOLO_BUILD_DATE      — yyyy-mm-dd of this configure
#   WINBOLO_BUILD_YEAR      — yyyy of this configure
#   BOLO_VERSION_MAJOR/MINOR/REVISION
#                           — per-byte network protocol version digits derived
#                             from WINBOLO_VERSION (e.g. "1.19" -> 1, 1, 9)

set(WINBOLO_VERSION "1.21" CACHE STRING "WinBolo version number")

# Anchor on this module's directory (always cmake/ under the repo root)
# so the wasm sub-project — whose CMAKE_SOURCE_DIR is src/wasm/ — still
# resolves the right git tree.
get_filename_component(_BOLO_VERSION_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY ${_BOLO_VERSION_REPO_ROOT}
                OUTPUT_VARIABLE WINBOLO_GIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT WINBOLO_GIT_HASH)
    set(WINBOLO_GIT_HASH "unknown")
endif()
string(TIMESTAMP WINBOLO_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP WINBOLO_BUILD_YEAR "%Y")

# Derive network protocol version bytes from WINBOLO_VERSION.
# Each digit of the version string (skipping dots) becomes one byte.
# E.g. "1.19" -> MAJOR=0x01, MINOR=0x01, REVISION=0x09.
string(REPLACE "." "" _VER_DIGITS "${WINBOLO_VERSION}")
string(SUBSTRING "${_VER_DIGITS}" 0 1 _VER_D0)
string(SUBSTRING "${_VER_DIGITS}" 1 1 _VER_D1)
string(SUBSTRING "${_VER_DIGITS}" 2 1 _VER_D2)
math(EXPR BOLO_VERSION_MAJOR "${_VER_D0}")
math(EXPR BOLO_VERSION_MINOR "${_VER_D1}")
math(EXPR BOLO_VERSION_REVISION "${_VER_D2}")
