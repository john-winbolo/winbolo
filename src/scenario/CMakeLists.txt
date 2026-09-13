# One library under src/scenario/:
#
#   scenario_static — the scenario runtime. Finds the Lua sidecar beside a
#                     map, boots a VM, reads the scenario table it declares
#                     and applies that table's rules through the op funnel
#                     from the sim's round-start callback, holds the bus
#                     events a round raises until the tick that drains them,
#                     and puts the game table a script reads the sim through
#                     on every state it boots. It also checks a sidecar
#                     without running one: the same parse against a stub game
#                     table, reported as a list of keys, lines and messages.
#
#                     Built under the scenario_host include profile: it sees
#                     src/bolo/public/ and src/bolo/scenario_api/, and
#                     nothing under src/bolo/internal/. It is the first
#                     target built under that profile, so compiling it is
#                     what proves scenario_defs.h and server_sim_scenario.h
#                     stand up against public/ alone.
#
#                     Linked by the binaries that host a ServerSim from a
#                     map file on disk — WinBoloDS and WinBoloHeadless — and
#                     by WinBoloUnitTests, which drives it directly. Not by
#                     wasm, gym or braintest, none of which host.

add_library(scenario_static STATIC
    scenario_host.c
    scenario_events.c
    scenario_lua.c
    scenario_validate.c
)

bolo_apply_include_rules(scenario_static scenario_host)

set_target_properties(scenario_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# scenario_host.h is what a frontend includes, so this directory reaches the
# consumers publicly. That publishes the whole directory, scenario_manifest.h,
# scenario_events.h and scenario_lua.h included, which is what lets the unit
# tests read the parsed manifest and the event queue and drive the binding
# table; the bolo include rules above stay PRIVATE, so linking scenario_static
# still hands nobody the scenario surface itself.
target_include_directories(scenario_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# server_sim.h, which scenario_host.h hands on to its includers, opens with
# <SDL3/SDL.h>. PUBLIC so the include directory reaches anything that
# includes the host header, as it does for the other bolo libraries.
target_link_libraries(scenario_static PUBLIC SDL3::SDL3)

# lua_static is the stable name for whichever Lua the tree picked — LuaJIT
# where it builds, PUC-Lua 5.4 otherwise. It carries the headers, the
# WINBOLO_LUAJIT define and the 5.4-to-5.1 shim's force-include with it, so
# the host compiles against either without naming one.
#
# PRIVATE: nothing on scenario_host.h names a Lua type, so a frontend has no
# business seeing Lua's headers. A static library still passes the link
# itself along under PRIVATE, so the binaries resolve the symbols either
# way. scenario_lua.h does name them, and what includes that — the unit
# tests — links lua_static for itself.
target_link_libraries(scenario_static PRIVATE lua_static)

if(NOT MSVC)
    target_compile_options(scenario_static PRIVATE -fcommon)
endif()
