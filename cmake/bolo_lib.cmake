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
#                  bolo's internal map-data layouts; never runs the sim. The
#                  exception expires the moment anyone adds in-editor
#                  playtest, live preview against a running sim, or any
#                  other path that ticks the world from the editor — at
#                  that point mapeditor joins the T1+T3+T4 group and the
#                  map-data access moves behind T1 accessors.
#   braintest    - the BrainTest debug visualiser binary. Privileged
#                  access to bot_manager.h, brain_pathfinder.h,
#                  brain_overlay.h, braincore.h, and control_event.h
#                  for live introspection of brain state. The
#                  asymmetric-runtime bug class doesn't apply: BrainTest
#                  is a dev tool, not shipped to players, and is the
#                  only consumer of these introspection getters. The
#                  exception expires the moment a second consumer needs
#                  the same access — at which point the right answer is
#                  to deep-copy the introspected state into a POD on a
#                  public header.
#   gym          - the winbolo_gym ML training harness. Privileged
#                  access to GameSim layout (game_sim.h) and the
#                  per-substruct headers (players.h, tank.h,
#                  shells.h, lgm.h, etc.) for observation and
#                  reward extraction during reinforcement-learning
#                  rollouts. The asymmetric-runtime bug class
#                  doesn't apply: gym is an offline training tool,
#                  not shipped to players in this form. The
#                  exception expires the moment gym ships in any
#                  player-facing distribution — at which point
#                  the observation builder migrates onto the
#                  snapshot APIs that the GUI clients already use,
#                  and gym drops back to runtime_only.
#   unittests    - the WinBoloUnitTests binary. Privileged access
#                  to bolo T2 for invariant checks. The scope is
#                  broad, and deliberately so: a test asserts on
#                  the state the code actually keeps. In practice
#                  that is the sim state structs, both sim
#                  internals, the wire and transport layer, the
#                  client's view and render internals, and the
#                  bot and brain headers; ARCHITECTURE.md's
#                  "tests/unit/" section carries the authoritative
#                  list. The asymmetric-runtime bug class doesn't
#                  apply: the tests are not shipped to players,
#                  have a single consumer (CTest), and aren't a
#                  runtime peer of the GUI / server / mobile /
#                  wasm clients. The exception expires if the
#                  binary ever ships in a player-facing
#                  distribution or gains a consumer beyond CTest —
#                  at that point it is a runtime peer like any
#                  other. Short of that the scope narrows rather
#                  than ends: every T2 include a new T1 accessor
#                  makes unnecessary should go.
#   gui          - the desktop game GUI and any platform-specific GUI binary.
#                  Sees public/ only. Reaching into bolo internals from a
#                  GUI translation unit is the asymmetric-runtime bug class
#                  this rule protects against.
#   runtime_only - headless / server-only / logviewer runtime binaries that
#                  ship parts of the sim but no desktop GUI. Sees public/
#                  only. Same rationale as gui.
#
# This file is the single point of policy for the include-rule lockdown.
# gui and runtime_only targets see only public/; the five privileged
# profiles (sim_owner / mapeditor / braintest / gym / unittests) see the
# full tree because they either own the sim or have a documented scoped
# exception.

set(BOLO_PUBLIC_DIR   "${CMAKE_CURRENT_LIST_DIR}/../src/bolo/public")
set(BOLO_INTERNAL_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/bolo/internal")
set(BOLO_FLAT_DIR     "${CMAKE_CURRENT_LIST_DIR}/../src/bolo")

function(bolo_apply_include_rules target profile)
    if(profile STREQUAL "sim_owner"
       OR profile STREQUAL "mapeditor"
       OR profile STREQUAL "braintest"
       OR profile STREQUAL "gym"
       OR profile STREQUAL "unittests")
        target_include_directories(${target} PRIVATE
            ${BOLO_PUBLIC_DIR}
            ${BOLO_INTERNAL_DIR}
            ${BOLO_FLAT_DIR})
    elseif(profile STREQUAL "gui" OR profile STREQUAL "runtime_only")
        target_include_directories(${target} PRIVATE
            ${BOLO_PUBLIC_DIR})
    else()
        message(FATAL_ERROR
            "bolo_apply_include_rules: unknown profile '${profile}' for target '${target}'")
    endif()
endfunction()

# Grant per-source-file internal/ access on top of a target's
# profile. Used for source files that are bolo's own internal
# implementation but happen to be compiled per-target rather
# than via bolo_static / server_static — typically because of
# target-divergent compile defs (HAVE_STEAM, BOLO_PORTMAP,
# HAVE_SCREEN_C) that prevent the file from being precompiled
# once into a shared static library.
#
# The scope is per-source-file, not per-target: other TUs in
# the same target remain bound by the target's profile, so
# the lockdown still bites GUI-side code in the same binary.
#
# Pass a target name followed by one or more source-file
# paths (absolute or relative to CMAKE_SOURCE_DIR).
function(bolo_grant_internal_source_access target)
    foreach(source IN LISTS ARGN)
        set_source_files_properties(${source}
            DIRECTORY ${CMAKE_SOURCE_DIR}
            TARGET_DIRECTORY ${target}
            PROPERTIES INCLUDE_DIRECTORIES
            "${BOLO_PUBLIC_DIR};${BOLO_INTERNAL_DIR};${BOLO_FLAT_DIR}")
    endforeach()
endfunction()
