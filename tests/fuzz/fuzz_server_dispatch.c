/* Tier-2 fuzz target — the server packet dispatcher.
 *
 * Feeds attacker-controlled datagrams through serverProcessPacket (via the
 * WB_FUZZ-gated seam in transport_udp_server.c), exercising the whole packet
 * switch plus the hand-written count-loops and chunk reassembly that Cluster A
 * codegen did NOT touch — COMMAND_TICK, MAP_ACK, the reliable game/map event
 * loops, and the lobby/map handlers. That hand-rolled residue is the highest
 * remaining over-read surface; this target is where §1.2 earns its keep.
 *
 * The ServerSim is built once over a known-good map in LLVMFuzzerInitialize;
 * the seam runs socket-free and thread-free (sock = INVALID_SOCKET), so no
 * datagram leaves the process and there is no recv-thread race.
 */
#include <stddef.h>
#include <stdint.h>

#include "global.h"                /* BYTE, gameType, gameOpen */
#include "platform_net.h"          /* bolo_net_init */
#include "server_sim.h"            /* ServerSim, serverSimCreateCompressed */
#include "server_sim_lifecycle.h"  /* serverSimSetAllowNewPlayers */
#include "everard_map.h"           /* E_MAP — same map the loopback harness uses */

#define FUZZ_EMAP_LEN E_MAP_LEN

/* Seam exported by src/server/udp/udp_server_test_hooks.c only under
 * -DWB_FUZZ. */
void transportUdpServerFuzzInit(ServerSim *sim);
void transportUdpServerFuzzProcessPacket(ServerSim *sim,
                                         const uint8_t *data, size_t size);

static ServerSim *g_sim = NULL;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    static BYTE emap[6000] = E_MAP;
    (void)argc;
    (void)argv;

    bolo_net_init();

    g_sim = serverSimCreateCompressed(emap, FUZZ_EMAP_LEN, "Everard Island",
                                      gameOpen, false, 0, -1);
    if (g_sim == NULL) {
        return -1; /* abort startup — nothing to fuzz against */
    }
    serverSimSetAllowNewPlayers(g_sim, true);
    transportUdpServerFuzzInit(g_sim);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (g_sim == NULL) return 0;
    transportUdpServerFuzzProcessPacket(g_sim, data, size);
    return 0;
}
