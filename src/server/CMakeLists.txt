# server_static — shared server-peer files factored out of the
# per-target *_SOURCES lists of the binaries that compile the real
# server lifecycle today.
#
# Linked by 4 targets:
#   WinBolo, WinBoloDS, WinBoloHeadless, winbolo_gym
#
# NOT linked by:
#   WinBoloIOS, BrainTest, MapEditor, android main, wasm winbolo
#     (these use server_stubs.c in place of the real server_lifecycle.c.
#      server_stubs.c only stubs the lifecycle API and not threads /
#      servermessages / geolookup, so those five targets keep their
#      existing partial server source set inline.)
#   LogViewer, logviewer-wasm — link no bolo or server source.
#
# server_sim.c now lives in this library — the previous HAVE_SCREEN_C
# divergence was lifted into server_dedicated_log.{c,h} so the same
# server_sim.c body compiles for every consumer. Targets that don't
# link server_static (iOS / BrainTest / MapEditor / android / wasm)
# still compile their own copy from their per-target *_SOURCES.
#
# Other src/server/*.c files stay per-target:
#   servermain.c               — WinBoloDS only
#   server_frontend_stubs.c    — WinBoloDS only
#   server_stubs.c             — non-lifecycle linkers
#   server_dedicated_log.c     — WinBoloDS only (real log bodies that
#                                touch servermain.c-owned globals)
#   server_dedicated_log_stubs.c — every binary other than WinBoloDS
#                                  (no-op bodies for the same hooks)
#   posix_stubs.c              — every non-Windows target (incl. LogViewer
#                                standalone for its WBN http INI shims)

add_library(server_static STATIC
    geolookup.c
    servermessages.c
    threads.c
    server_lifecycle.c
    server_sim.c
    transport_udp_server.c
)

bolo_apply_include_rules(server_static sim_owner)

set_target_properties(server_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(server_static PRIVATE
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

# geolookup.c is fully #ifdef HAVE_MAXMINDDB-wrapped. Every server_static
# linker today defines HAVE_MAXMINDDB and links libmaxminddb, so the
# shared compile picks the real implementation. Gated on the target
# existing so the lib still builds in projects that omit maxminddb.
if(TARGET maxminddb)
    target_compile_definitions(server_static PRIVATE HAVE_MAXMINDDB=1)
    target_link_libraries(server_static PUBLIC maxminddb)
endif()

# server TUs include bolo internal headers and reference bolo symbols
# (tankCreate, clientSimGetGameSim, etc.). PUBLIC so consumers don't
# need to add bolo_static separately when they link server_static.
target_link_libraries(server_static PUBLIC bolo_static)

if(NOT MSVC)
    target_compile_options(server_static PRIVATE -fcommon)
endif()
