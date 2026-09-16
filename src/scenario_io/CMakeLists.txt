# One library under src/scenario_io/:
#
#   scenario_io_static — a scenario's files, without the runtime that plays
#                     one. It reads and writes the WBSC container a scenario
#                     ships in — the framing, and the ZIP archive behind it
#                     holding the manifest, the script and any brain
#                     directories — from a byte buffer rather than a path, so
#                     the chunk appended to a map, a package file and a blob
#                     off the network are all read the same way. The manifest
#                     inside that container is read into and written out of
#                     the same struct the Lua reader fills, keeping the keys
#                     this build has no use for, and the two forms are held
#                     against each other so a package whose script restates
#                     its table cannot disagree with it. Writing that
#                     container on to a map file is here too, manifest and
#                     all, which is what the server's -pack flag reaches
#                     through scenario_static.
#
#                     No Lua, and no view of src/bolo/scenario_api/ or
#                     src/bolo/internal/: built under the runtime_only
#                     profile, so it sees src/bolo/public/ and nothing else.
#                     What a scenario means to a running sim is
#                     scenario_static's; what a scenario looks like on disk
#                     is this library's, and the shape they share —
#                     ScenarioManifest, the issue list — is declared here
#                     because this is the half that can be linked alone.
#
#                     scenario_static links it PUBLIC, so the six binaries
#                     that host a ServerSim get it and its directory without
#                     naming it. The map editor and the log viewer are
#                     expected to link it directly and nothing else from
#                     src/scenario/: they read and write a container and
#                     never start a round. Each tree that links it has to
#                     declare its dependencies as well: see the cJSON note at
#                     the foot of this file.

add_library(scenario_io_static STATIC
    scenario_chunk.c
    scenario_issues.c
    scenario_manifest_json.c
    scenario_package.c
)

bolo_apply_include_rules(scenario_io_static runtime_only)

set_target_properties(scenario_io_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The manifest's shape and the issue list are what a consumer reads, so this
# directory reaches the consumers publicly. scenario_static publishes its own
# directory the same way, and links this one PUBLIC, so a target that links
# either resolves both sets of headers.
target_include_directories(scenario_io_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# server_sim.h, which scenario_manifest.h hands on to its includers, opens
# with <SDL3/SDL.h>. PUBLIC so the include directory reaches anything that
# includes that header, as it does for the other bolo libraries.
target_link_libraries(scenario_io_static PUBLIC SDL3::SDL3)

# minizip carries zip.h, unzip.h and ioapi.h, and the zlib behind them, for
# the WBSC container's archive.
#
# PRIVATE: scenario_package.h names no minizip type, so an includer has no
# business seeing those headers, and a static library still passes the link
# itself along, so the binaries resolve the symbols either way.
target_link_libraries(scenario_io_static PRIVATE minizip)

# cJSON, for manifest.json. It carries no include directory of its own —
# nothing in the tree links it without naming ${cjson_SOURCE_DIR} too — so
# the directory is added here beside the link.
#
# PRIVATE for the same reason as minizip: scenario_manifest_json.h names no
# cJSON type, so the tree stays inside the library and no includer sees
# cJSON.h.
#
# What PRIVATE does not do is create the target. A build that links this
# library has to declare cjson for itself, the way it declares SDL3, zlib and
# minizip — so every tree that links it, or links scenario_static and gets it
# that way, fetches it: the root CMakeLists.txt for WinBolo, WinBoloIOS,
# WinBoloDS, WinBoloHeadless and WinBoloUnitTests, and
# android/app/CMakeLists.txt for the Android main. A tree that links this
# library on its own — the map editor, the log viewer — adds its own
# declaration.
target_link_libraries(scenario_io_static PRIVATE cjson)
target_include_directories(scenario_io_static PRIVATE ${cjson_SOURCE_DIR})

if(NOT MSVC)
    target_compile_options(scenario_io_static PRIVATE -fcommon)
endif()
