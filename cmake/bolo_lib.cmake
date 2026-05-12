# cmake/bolo_lib.cmake
#
# Shared include-rule policy for the bolo sim. Each platform's CMake root
# (top-level CMakeLists.txt, android/app/CMakeLists.txt, src/wasm/CMakeLists.txt,
# src/logviewer/wasm/CMakeLists.txt) includes this file and calls
#   bolo_apply_include_rules(<target> <profile>)
# in place of the bolo-related entries in target_include_directories.
#
# Profiles:
#   sim_owner    - the bolo sim implementation itself. Sees public/, internal/,
#                  and (transitionally) the flat src/bolo/ directory.
#   mapeditor    - the standalone map editor binary. Privileged T2 access to
#                  bolo's internal map-data layouts; never runs the sim.
#   gui          - the desktop game GUI and any platform-specific GUI binary.
#                  Today sees public/, internal/, and flat src/bolo/ for
#                  transitional reasons; a future change will tighten this
#                  to public/ only.
#   runtime_only - headless / gym / braintest / server-only / wasm /
#                  logviewer runtime binaries that ship parts of the sim but
#                  no desktop GUI. Same transitional state as gui; same
#                  future tightening.
#
# This file is the single point of edit for the future lockdown step. Until
# then, every profile maps to the same set of include directories, so
# wiring a target through this function does not change its behavior.

set(BOLO_PUBLIC_DIR   "${CMAKE_CURRENT_LIST_DIR}/../src/bolo/public")
set(BOLO_INTERNAL_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/bolo/internal")
set(BOLO_FLAT_DIR     "${CMAKE_CURRENT_LIST_DIR}/../src/bolo")

function(bolo_apply_include_rules target profile)
    if(profile STREQUAL "sim_owner"
       OR profile STREQUAL "mapeditor"
       OR profile STREQUAL "gui"
       OR profile STREQUAL "runtime_only")
        target_include_directories(${target} PRIVATE
            ${BOLO_PUBLIC_DIR}
            ${BOLO_INTERNAL_DIR}
            ${BOLO_FLAT_DIR})
    else()
        message(FATAL_ERROR
            "bolo_apply_include_rules: unknown profile '${profile}' for target '${target}'")
    endif()
endfunction()
