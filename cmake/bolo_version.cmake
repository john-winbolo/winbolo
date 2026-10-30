# WinBolo version constants shared between the root CMake project and
# the standalone wasm sub-project (src/wasm/CMakeLists.txt).
#
# Sets:
#   WINBOLO_VERSION         — numeric version string in three fields,
#                             "major.minor.revision" (e.g. "2.1.0"); cache
#                             string so a -D override sticks across configures.
#                             This is the value the network protocol bytes are
#                             derived from, so DO NOT add non-numeric characters
#                             here — use WINBOLO_VERSION_SUFFIX instead.
#   WINBOLO_VERSION_SUFFIX  — optional display-only suffix (e.g. "a") for a
#                             build with UI-only changes that stay wire- and
#                             gameplay-compatible with the same numeric version.
#   WINBOLO_DISPLAY_VERSION — human-facing version string shown in the UI
#                             (about box, welcome screen, server banner):
#                             WINBOLO_VERSION with WINBOLO_VERSION_SUFFIX appended.
#   WINBOLO_GIT_HASH        — `git rev-parse --short HEAD`, or "unknown"
#   WINBOLO_BUILD_DATE      — yyyy-mm-dd of this configure
#   WINBOLO_BUILD_YEAR      — yyyy of this configure
#   BOLO_VERSION_MAJOR/MINOR/REVISION
#                           — the network protocol version bytes, one per
#                             field of WINBOLO_VERSION (e.g. "2.1.0" -> 2, 1, 0;
#                             each field 0-255). The suffix does NOT affect
#                             these, so a "2.1.0a" build connects to "2.1.0"
#                             peers. Builds from before 2.1.0 wrote the same
#                             bytes as "2.1" (one digit per byte), so 2.1.0
#                             builds connect to them.

# The set() of the default below must stay the first line in this file that
# sets WINBOLO_VERSION to a quoted value: android/app/build.gradle.kts reads
# the version from the first such line with a regex.
#
# A cache entry is never replaced by a new default, so a build dir configured
# before a version change would keep building the old version. To stop that,
# the default this configure saw is kept in the internal cache entry
# WINBOLO_VERSION_DEFAULT_SEEN. On each configure the old cache value is saved
# and the entry removed, so the set() below reads back the current default.
# The old value is then dropped in favour of the current default when:
#   - it equals the default the last configure saw, so nobody chose it; or
#   - it is in the two-field form every default before 2.1.0 used ("1.19",
#     "2.03", "2.1"). Those can no longer be built (the check further down
#     wants three fields), and build dirs made before
#     WINBOLO_VERSION_DEFAULT_SEEN existed hold one of them.
# Any other value was set by hand (-D or the cache editor) and is kept. A -D
# value that happens to equal the default is treated as the default and
# follows later changes to it.
if(DEFINED CACHE{WINBOLO_VERSION})
    set(_WINBOLO_VERSION_CACHED "$CACHE{WINBOLO_VERSION}")
    set(_WINBOLO_VERSION_PREV_DEFAULT "$CACHE{WINBOLO_VERSION_DEFAULT_SEEN}")
    unset(WINBOLO_VERSION CACHE)
endif()
set(WINBOLO_VERSION "2.2.0" CACHE STRING "WinBolo numeric version number, major.minor.revision (drives network protocol bytes)")
set(WINBOLO_VERSION_DEFAULT_SEEN "${WINBOLO_VERSION}" CACHE INTERNAL
    "The WINBOLO_VERSION default the last configure saw")
if(DEFINED _WINBOLO_VERSION_CACHED)
    if(_WINBOLO_VERSION_CACHED MATCHES "^[0-9]+\\.[0-9]+$")
        message(STATUS "WINBOLO_VERSION: replaced the old two-field cache "
                       "value \"${_WINBOLO_VERSION_CACHED}\" with \"${WINBOLO_VERSION}\"")
    elseif(NOT _WINBOLO_VERSION_CACHED STREQUAL _WINBOLO_VERSION_PREV_DEFAULT)
        set(WINBOLO_VERSION "${_WINBOLO_VERSION_CACHED}" CACHE STRING
            "WinBolo numeric version number, major.minor.revision (drives network protocol bytes)" FORCE)
    endif()
endif()
unset(_WINBOLO_VERSION_CACHED)
unset(_WINBOLO_VERSION_PREV_DEFAULT)
set(WINBOLO_VERSION_SUFFIX "a" CACHE STRING "Display-only version suffix (UI text only, ignored by the network protocol)")

# Human-facing version string. Only affects displayed text; the network
# protocol bytes below are derived from WINBOLO_VERSION alone.
set(WINBOLO_DISPLAY_VERSION "${WINBOLO_VERSION}${WINBOLO_VERSION_SUFFIX}")

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

# Derive network protocol version bytes from WINBOLO_VERSION: each of the
# three fields becomes one byte, so "2.1.0" -> MAJOR=2, MINOR=1, REVISION=0.
if(NOT WINBOLO_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR
        "WINBOLO_VERSION \"${WINBOLO_VERSION}\" is not major.minor.revision "
        "(three numbers separated by dots, e.g. \"2.1.0\"). Put any letter "
        "suffix in WINBOLO_VERSION_SUFFIX.")
endif()
math(EXPR BOLO_VERSION_MAJOR "${CMAKE_MATCH_1}")
math(EXPR BOLO_VERSION_MINOR "${CMAKE_MATCH_2}")
math(EXPR BOLO_VERSION_REVISION "${CMAKE_MATCH_3}")
foreach(_ver_byte IN ITEMS ${BOLO_VERSION_MAJOR} ${BOLO_VERSION_MINOR} ${BOLO_VERSION_REVISION})
    if(_ver_byte GREATER 255)
        message(FATAL_ERROR
            "WINBOLO_VERSION \"${WINBOLO_VERSION}\" has a field over 255; "
            "each field is sent as one byte.")
    endif()
endforeach()
