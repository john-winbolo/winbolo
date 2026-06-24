# bolo_static — the bulk of the sim runtime in a single static library.
#
# Two src/bolo/*.c files stay per-target in each binary's *_SOURCES
# because they have target-divergent compile defs whose gated code
# paths can't be archive-dropped (the .o files are always live, so
# a single shared compile can't satisfy every consumer's view):
#
#   transport_udp_client.c  — HAVE_STEAM gates a PLAYER_FLAG_STEAM_BUILD bit (JOIN_REQUEST path)
#   nat_portmap.c           — BOLO_PORTMAP swaps real libplum for no-op stubs
#
# Built with the sim_owner include-rule profile, so TUs see public/,
# internal/, and the flat src/bolo/ tree. Compile defs are inherited
# from the parent project's variables: WINBOLO_VERSION and
# BOLO_VERSION_MAJOR/MINOR/REVISION are mandatory; ENABLE_PILLMASSAGE_BUG
# and HAVE_ONNXRUNTIME flip on optional code paths.

add_library(bolo_static STATIC
    allience.c
    bases.c
    bolo_map.c
    bolo_rand.c
    bot_manager.c
    bot_worker_pool.c
    brain_data.c
    brain_list.c
    brain_overlay.c
    brain_pathfinder.c
    brain_pathfinder_serial.c
    brain_worldsim.c
    braincore.c
    building.c
    bulk_transfer.c
    channel_mux.c
    client_mappreview.c
    client_net.c
    client_render.c
    client_sim.c
    client_sim_control.c
    client_snapshot.c
    client_state.c
    client_timing.c
    client_ui_events.c
    crc.c
    debug_file_output.c
    discovery.c
    discovery_mdns.c
    explosions.c
    floodfill.c
    gametype.c
    global.c
    grass.c
    gui_message.c
    interpolation.c
    labels.c
    lgm.c
    lobby_bot_pools.c
    log.c
    mapgen.c
    mapgen_maze.c
    messages.c
    mines.c
    minesexp.c
    ml_brain.c
    net_impair.c
    net_packet_debug.c
    obs_builder.c
    pillbox.c
    playername_validate.c
    players.c
    playersrejoin.c
    position_history.c
    rubble.c
    screenbrainmap.c
    screenbullet.c
    screencalc.c
    screenlgm.c
    screentank.c
    scroll.c
    scroll_item_list.c
    shells.c
    sounddist.c
    spectator_ring.c
    spectator_replay.c
    starts.c
    swamp.c
    tank.c
    tankexp.c
    transport_command_codec.c
    transport_control_codec.c
    transport_local.c
    transport_udp_common.c
    treegrow.c
    tutorial.c
    udppackets.c
    util.c
    viewport.c
    wbn_key_codec.c
)

bolo_apply_include_rules(bolo_static sim_owner)

# discovery_mdns.c is the only bolo TU that pulls in the all-static-inline
# mdns.h; suppress the unused-function noise for that one file. Under
# Emscripten the file compiles to no-op stubs (no mdns.h), so the mdns
# include dir is not linked there.
if(NOT EMSCRIPTEN)
    target_link_libraries(bolo_static PRIVATE mdns)
    if(MSVC)
        set_source_files_properties(discovery_mdns.c PROPERTIES COMPILE_OPTIONS "/wd4505")
    else()
        set_source_files_properties(discovery_mdns.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")
    endif()
endif()

set_target_properties(bolo_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(bolo_static PRIVATE
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

if(ENABLE_PILLMASSAGE_BUG)
    target_compile_definitions(bolo_static PRIVATE ENABLE_PILLMASSAGE_BUG)
endif()

if(HAVE_ONNXRUNTIME)
    target_compile_definitions(bolo_static PRIVATE HAVE_ONNXRUNTIME=1)
    target_link_libraries(bolo_static PUBLIC onnxruntime::onnxruntime)
endif()

# Third-party libs whose headers bolo .c files include. PUBLIC so
# consumers transitively get the link order without repeating them —
# every bolo-linking binary links these directly today anyway.
target_link_libraries(bolo_static PUBLIC
    SDL3::SDL3
    lua_static
    zlibstatic
    minizip
    utf8proc
)

# Windows system lib that bolo TUs reference: ws2_32 for the sockets
# called from transport_udp_server.c, transport_udp_common.c, discovery.c.
# PUBLIC so the consumer link line sees bolo_static.a *before* ws2_32,
# which MinGW's ld requires for one-pass symbol resolution.
if(WIN32)
    target_link_libraries(bolo_static PUBLIC ws2_32)
endif()

# -fcommon matches the per-target setting every bolo-linking binary
# uses today. Keeps tentative definitions like debug_file_output.c's
# `char fileName[2048]` mergeable with the strong definition of the
# same name in servermain.c (WinBoloDS).
#
# Emscripten/wasm-ld can't merge COMMON symbols, and there's no
# servermain.c in the wasm link anyway. -include sys/types.h brings
# in the BSD typedefs (u_char, u_short) that brain.h relies on under
# non-_WIN32 platforms.
if(EMSCRIPTEN)
    target_compile_options(bolo_static PRIVATE -fno-common -include sys/types.h)
elseif(NOT MSVC)
    target_compile_options(bolo_static PRIVATE -fcommon)
endif()
