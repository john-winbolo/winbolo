# Three libraries under src/server/:
#
#   threads_static     — the SDL-mutex thread manager. threads.c on every
#                        platform except Emscripten, where threads_wasm.c
#                        substitutes single-threaded no-ops with the same
#                        symbol surface. Linked by every binary that ticks
#                        a sim (not just the dedicated server) — bolo_static's
#                        transport_local.c calls threadsWaitForMutex /
#                        threadsReleaseMutex. server_static PUBLIC-links
#                        threads_static so the four dedicated-server
#                        binaries pick it up transitively; the in-process
#                        single-player builds (WinBoloIOS, android main,
#                        wasm winbolo, WinBoloUnitTests) link it directly.
#
#   server_sim_static  — the server-side sim core: server_sim.c,
#                        server_command_dispatch.c and servermessages.c in
#                        this directory, plus the per-concern translation
#                        units under sim/ that server_sim.c has been split
#                        into.
#                        The shared round-stats derivation (computeAwards,
#                        roundStatsApplyRecord) lives in bolo_static's
#                        round_stats_derive.c so the standalone log viewer
#                        can rebuild stats without the sim.
#                        Linked by every binary that hosts a sim, including
#                        the in-process single-player builds (WinBoloIOS,
#                        BrainTest, MapEditor, android main, wasm
#                        winbolo). Replaces the per-target inline
#                        compilation of these files those binaries
#                        used to do, which also drops the matching
#                        per-file bolo_grant_internal_source_access
#                        entries.
#
#   server_static      — the full dedicated-server runtime on top of
#                        the sim: transport_udp_server.c + server_lifecycle.c
#                        + geolookup.c, plus threads_static PUBLIC-linked.
#                        PUBLIC-links server_sim_static so consumers don't
#                        have to list both. Linked by the four binaries
#                        that run the real dedicated-server lifecycle:
#                        WinBolo, WinBoloDS, WinBoloHeadless, winbolo_gym.
#                        EXCLUDE_FROM_ALL so platforms that only need
#                        server_sim_static (iOS, BrainTest, MapEditor,
#                        android, wasm) don't pay to build the dedicated-
#                        server bits.
#
# Files that stay per-target:
#   servermain.c               — WinBoloDS only
#   server_frontend_stubs.c    — WinBoloDS only
#   server_stubs.c             — non-server_static linkers (iOS,
#                                BrainTest, MapEditor, android, wasm)
#                                — stubs the transportUdpServer*,
#                                serverLifecycle*, and lobbyClientMayEdit
#                                symbols those binaries don't pull in
#                                via server_static

if(EMSCRIPTEN)
    add_library(threads_static STATIC threads_wasm.c)
else()
    add_library(threads_static STATIC threads.c)
endif()

bolo_apply_include_rules(threads_static runtime_only)

set_target_properties(threads_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# threads.c links against SDL3's mutex primitives. PUBLIC so consumers
# of threads_static don't have to repeat the SDL3 link entry — every
# consumer today already links SDL3 directly, but PUBLIC keeps the
# transitive link line correct.
target_link_libraries(threads_static PUBLIC SDL3::SDL3)

if(EMSCRIPTEN)
    target_compile_options(threads_static PRIVATE -fno-common -include sys/types.h)
elseif(NOT MSVC)
    target_compile_options(threads_static PRIVATE -fcommon)
endif()


add_library(server_sim_static STATIC
    server_sim.c
    server_command_dispatch.c
    servermessages.c
    sim/server_sim_accessors.c
    sim/server_sim_bots.c
    sim/server_sim_callbacks.c
)

bolo_apply_include_rules(server_sim_static sim_owner)

set_target_properties(server_sim_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# server_sim.c references winbolonet.h / http.h transitively through
# bolo_static's link surface; PUBLIC so consumers don't need to add
# bolo_static separately when they link server_sim_static.
target_link_libraries(server_sim_static PUBLIC bolo_static)

if(EMSCRIPTEN)
    target_compile_options(server_sim_static PRIVATE -fno-common -include sys/types.h)
elseif(NOT MSVC)
    target_compile_options(server_sim_static PRIVATE -fcommon)
endif()


add_library(server_static STATIC EXCLUDE_FROM_ALL
    geolookup.c
    server_lifecycle.c
    transport_udp_server.c
    mdns_advertise.c
    server_dedicated_log.c
    server_dedicated_log_path.c
)

# mdns_advertise.c is the only TU that pulls in the all-static-inline
# mdns.h; suppress the unused-function noise for that one file rather
# than the whole library.
if(MSVC)
    set_source_files_properties(mdns_advertise.c PROPERTIES COMPILE_OPTIONS "/wd4505")
else()
    set_source_files_properties(mdns_advertise.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")
endif()

bolo_apply_include_rules(server_static sim_owner)

set_target_properties(server_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(server_static PRIVATE
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
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

# server_sim.c calls transportUdpServerEnforcePing /
# serverLifecycleGetTickStats / etc.; transport_udp_server.c and
# server_lifecycle.c call serverSimXyz. The two archives form a cycle
# that single-pass ld can't resolve. Consumers that link server_static
# must wrap it with server_sim_static in a $<LINK_GROUP:RESCAN,...>
# group — see the four desktop binaries (WinBolo, WinBoloDS,
# WinBoloHeadless, winbolo_gym). PRIVATE so the dep doesn't auto-
# propagate to consumers in the wrong order: explicit grouping at
# the consume site is what makes the cycle resolvable.
target_link_libraries(server_static PRIVATE server_sim_static)

# mdns.h (header-only) include directory for mdns_advertise.c. Not linked
# under Emscripten — mdns_advertise.c compiles to no-op stubs there (no
# multicast sockets / getifaddrs in the WASM runtime).
if(NOT EMSCRIPTEN)
    target_link_libraries(server_static PRIVATE mdns)
endif()

# threads_static owns threads.c (and its threads_wasm.c sibling under
# Emscripten). PUBLIC so consumers of server_static — the four desktop
# server binaries — get the mutex symbols transitively without naming
# threads_static themselves.
target_link_libraries(server_static PUBLIC threads_static)

# bcrypt: BCryptGenRandom seeds the address-proof cookie secret in
# transport_udp_server.c on Windows. PUBLIC so each server_static consumer's
# link line picks it up after the static lib (mirrors how ws2_32 reaches
# consumers for the socket calls).
if(WIN32)
    target_link_libraries(server_static PUBLIC bcrypt)
endif()

if(EMSCRIPTEN)
    target_compile_options(server_static PRIVATE -fno-common -include sys/types.h)
elseif(NOT MSVC)
    target_compile_options(server_static PRIVATE -fcommon)
endif()
