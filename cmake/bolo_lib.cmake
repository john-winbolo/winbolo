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
#   gui          - the desktop game GUI and any platform-specific GUI binary.
#                  Today sees public/, internal/, and flat src/bolo/ for
#                  transitional reasons; a future change will tighten this
#                  to public/ only.
#   runtime_only - headless / server-only / logviewer runtime binaries
#                  that ship parts of the sim but no desktop GUI. Same
#                  transitional state as gui; same future tightening.
#
# This file is the single point of policy for the include-rule lockdown.
# gui/runtime_only targets see only public/; the four privileged profiles
# (sim_owner / mapeditor / braintest / gym) see the full tree.
#
# After this change, the gui and runtime_only profiles resolve to public/
# only. The sim_owner, mapeditor, braintest, and gym profiles continue to
# see the full public + internal + flat tree. The latter four either own
# the sim (sim_owner), have a privileged scoped exception documented above
# (mapeditor, braintest, gym), or aren't subject to the asymmetric-runtime
# bug class this rule protects against.

set(BOLO_PUBLIC_DIR   "${CMAKE_CURRENT_LIST_DIR}/../src/bolo/public")
set(BOLO_INTERNAL_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/bolo/internal")
set(BOLO_FLAT_DIR     "${CMAKE_CURRENT_LIST_DIR}/../src/bolo")

function(bolo_apply_include_rules target profile)
    if(profile STREQUAL "sim_owner"
       OR profile STREQUAL "mapeditor"
       OR profile STREQUAL "braintest"
       OR profile STREQUAL "gym")
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
