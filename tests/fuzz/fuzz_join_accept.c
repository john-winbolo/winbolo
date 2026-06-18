/* JOIN_ACCEPT fuzz target (hardening plan §1.2).
 *
 * The join handshake was a coverage blind spot — the snapshot target sets
 * joinState=CONNECTED and never enters it — yet the audit found a real bug
 * there: the server-assigned slot (buf[8]) was stored into myPlayerNum
 * unchecked, then used as an array index throughout the client.
 *
 * The fuzz input is the accept body ([playerNum][serverTick][mapSize]
 * [optional connId]); the WB_FUZZ seam in transport_udp_client.c frames the
 * header on an exact-size, ASan-guarded buffer and forces the JOINING state.
 */
#include <stddef.h>
#include <stdint.h>

#include "platform_net.h"  /* bolo_net_init */
#include "client_sim.h"    /* ClientSim, clientSimAlloc, clientSimCreate */

/* Seam exported by transport_udp_client.c only under -DWB_FUZZ. */
void transportUdpClientFuzzInit(ClientSim *sim);
void transportUdpClientFuzzProcessJoinAccept(const uint8_t *body, size_t size);

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
    transportUdpClientFuzzProcessJoinAccept(data, size);
    return 0;
}
