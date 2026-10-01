# One library under src/scenario/:
#
#   scenario_static — the scenario runtime. Finds the Lua script beside a
#                     map, boots a VM, reads the scenario table it declares
#                     and applies that table's rules through the op funnel
#                     from the sim's round-start callback, holds the bus
#                     events a round raises until the tick that drains them,
#                     and puts the game table a script reads the sim through
#                     on every state it boots. It also checks a script
#                     without running one: the same parse against a stub game
#                     table, reported as a list of keys, lines and messages.
#
#                     It also reads the server's scenarios directory — the
#                     .scenario packages and loose .lua scripts a server
#                     offers independently of any map — into the list the
#                     lobby sends a client. The sim reaches that read through
#                     a callback registered on it, not by calling in here:
#                     this library calls the funnel, so it is linked ahead of
#                     the server group and a call the other way would not
#                     resolve.
#
#                     A scenario's triggers are run by a router written in
#                     Lua, scenario_triggers.lua, which loads as a second
#                     chunk after the author's script and chains on to the
#                     hooks the triggers name. It ships inside the binary:
#                     tools/embed_lua.py turns it into scenario_triggers.inc,
#                     which scenario_host.c includes. The generator is run by
#                     hand and its output committed, the way the lang table's
#                     is, so the build needs no Python; the unit case
#                     scenario_hooks_router_matches_source is what holds the
#                     two together.
#
#                     The files a scenario is made of — the WBSC container,
#                     manifest.json, the chunk written on to a map — are
#                     scenario_io_static's under src/scenario_io/, which this
#                     library links PUBLIC. What is left here is the half
#                     that needs a VM or a sim: the server's -pack flag lands
#                     on scenario_pack.c, which validates the script and then
#                     hands the manifest and the source over to be written.
#
#                     Built under the scenario_host include profile: it sees
#                     src/bolo/public/ and src/bolo/scenario_api/, and
#                     nothing under src/bolo/internal/. It is the first
#                     target built under that profile, so compiling it is
#                     what proves scenario_defs.h and server_sim_scenario.h
#                     stand up against public/ alone.
#
#                     Linked by the seven binaries that host a ServerSim:
#                     WinBoloDS and WinBoloHeadless, the four client builds
#                     that host in process — WinBolo, WinBoloIOS, the
#                     Android main and the web client (src/wasm), whose
#                     practice game runs its server in the page — and
#                     WinBoloUnitTests, which drives it directly. Each of
#                     those seven is built from a CMake tree that has to
#                     declare this library's own dependencies as well: see
#                     the cJSON note at the foot of this file. Not linked by
#                     gym or braintest, neither of which hosts.

add_library(scenario_static STATIC
    scenario_host.c
    scenario_dir.c
    scenario_events.c
    scenario_lua.c
    scenario_pack.c
    scenario_pattern.c
    scenario_sandbox.c
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

# The file half of a scenario. PUBLIC because the headers that come with it
# are part of what this library hands its includers — scenario_validate.h
# names ScnValidateResult and scenario_lua.h names ScenarioManifest, both
# declared over there — so the seven binaries that link scenario_static
# resolve those headers and those symbols without naming the library.
target_link_libraries(scenario_static PUBLIC scenario_io_static)

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

# minizip and cJSON are scenario_io_static's, which links both PRIVATE: no
# file left here includes zip.h, unzip.h, ioapi.h or cJSON.h. A static
# library passes the link itself along, so a tree that links scenario_static
# still resolves the symbols — and still has to declare both targets for
# itself, the way it declares SDL3 and Lua. See the cJSON note in
# src/scenario_io/CMakeLists.txt.

if(NOT MSVC)
    target_compile_options(scenario_static PRIVATE -fcommon)
endif()
