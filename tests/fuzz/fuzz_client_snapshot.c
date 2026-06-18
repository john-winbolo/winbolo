/* Client snapshot fuzz target (hardening plan §1.2).
 *
 * The fuzz input is a STATE_SNAPSHOT *body*; the WB_FUZZ seam in
 * transport_udp_client.c frames it with a valid header and feeds it to
 * udpClientProcessPacket on an exact-size, ASan-guarded buffer.
 *
 * This target exists to reach the count-driven reliable-event loop that
 * codegen left hand-written — the suspected over-read at
 * transport_udp_client.c:~1550, where the loop guards only the 1-byte event
 * type but unpackGameEvent then memcpy's up to GAME_EVENT_MAX_DATA bytes. The
 * wire-codec and server-dispatch targets cannot reach this client-side path.
 *
 * The committed corpus holds only well-formed (non-crashing) snapshots so the
 * replay ctest stays green; discovery is expected to find the truncated-event
 * input that trips the over-read.
 */
#include <stddef.h>
#include <stdint.h>

#include "platform_net.h"  /* bolo_net_init */
#include "client_sim.h"    /* ClientSim, clientSimAlloc, clientSimCreate */

/* Seam exported by transport_udp_client.c only under -DWB_FUZZ. */
void transportUdpClientFuzzInit(ClientSim *sim);
void transportUdpClientFuzzProcessSnapshot(const uint8_t *body, size_t size);

static ClientSim *g_cs = NULL;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    bolo_net_init();
    g_cs = clientSimAlloc();
    if (g_cs == NULL || !clientSimCreate(g_cs)) {
        return -1;
    }
    transportUdpClientFuzzInit(g_cs);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (g_cs == NULL) return 0;
    transportUdpClientFuzzProcessSnapshot(data, size);
    return 0;
}
