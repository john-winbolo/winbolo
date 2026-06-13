/*
 * Connection-id NAT-rebind migration.
 *
 * The server hands each client a random 64-bit connId in JOIN_ACCEPT; the
 * client echoes it as a framing prefix on every INPUT packet
 * ([header 8][connId 8][count 1][29-byte inputs…]). When an INPUT arrives whose
 * connId matches a connected slot but whose source address has changed (a NAT
 * mapping rebind), the server re-homes the slot to the new address instead of
 * timing the player out. An absent (0) or unknown connId falls back to the
 * IP:port lookup.
 *
 * Two cases:
 *  - rehome: drives transportUdpServerFindByConnId (the pure match-and-rehome
 *    decision factored out of serverHandleInput) over a synthetic client table
 *    — no sockets. Asserts match/no-match and the re-home flag.
 *  - e2e: a real UDP client joins a real in-process server over the loopback
 *    harness, the server stores a non-zero connId for the slot, and inputs
 *    driven through the new framing reach the sim (write/read offsets agree).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "platform_net.h"
#include "transport_udp.h"          /* UdpServerClient + connId helpers */
#include "server_sim.h"
#include "server_sim_internal.h"    /* lastProcessedInput[] */
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "input_packet.h"
#include "test_harness.h"
#include "loopback_harness.h"

static struct sockaddr_in cm_addr(const char *ip, unsigned short port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = inet_addr(ip);
    a.sin_port        = htons(port);
    return a;
}

/* The connId match-and-rehome decision over a synthetic client table. */
int run_conn_migration_rehome(void) {
    UdpServerClient clients[MAX_TANKS];
    memset(clients, 0, sizeof(clients));

    const uint64_t ID = 0x0123456789abcdefULL;
    struct sockaddr_in home   = cm_addr("127.0.0.1", 5000);
    struct sockaddr_in newPort = cm_addr("127.0.0.1", 6000);  /* port rebind */
    struct sockaddr_in newIp   = cm_addr("10.0.0.9",  5000);  /* ip rebind */

    clients[3].connected = true;
    clients[3].connId    = ID;
    clients[3].addr      = home;

    bool rehome = true;
    int idx;

    /* Same address → match, no re-home. */
    idx = transportUdpServerFindByConnId(clients, ID, &home, &rehome);
    UT_ASSERT_MSG(idx == 3, "connId match returned slot %d, expected 3", idx);
    UT_ASSERT_MSG(!rehome, "same-address match must not request re-home");

    /* Changed port → match + re-home. */
    rehome = false;
    idx = transportUdpServerFindByConnId(clients, ID, &newPort, &rehome);
    UT_ASSERT_MSG(idx == 3 && rehome,
                  "port rebind must match slot 3 and re-home (idx=%d rehome=%d)",
                  idx, (int)rehome);

    /* Changed ip → match + re-home. */
    rehome = false;
    idx = transportUdpServerFindByConnId(clients, ID, &newIp, &rehome);
    UT_ASSERT_MSG(idx == 3 && rehome,
                  "ip rebind must match slot 3 and re-home (idx=%d rehome=%d)",
                  idx, (int)rehome);

    /* connId 0 → no match; caller falls back to IP:port. */
    rehome = true;
    idx = transportUdpServerFindByConnId(clients, 0, &home, &rehome);
    UT_ASSERT_MSG(idx < 0 && !rehome, "connId 0 must not match (idx=%d)", idx);

    /* Unknown connId → no match. */
    rehome = true;
    idx = transportUdpServerFindByConnId(clients, ID ^ 0xffULL, &home, &rehome);
    UT_ASSERT_MSG(idx < 0 && !rehome, "unknown connId must not match (idx=%d)", idx);

    /* A disconnected slot carrying the right connId must not match. */
    clients[3].connected = false;
    rehome = true;
    idx = transportUdpServerFindByConnId(clients, ID, &home, &rehome);
    UT_ASSERT_MSG(idx < 0 && !rehome,
                  "disconnected slot must not match on connId (idx=%d)", idx);

    return 0;
}

static bool cm_pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* Generous bounds: the clean-path join lands well inside these (see
 * test_loopback_join); a hang is clearly distinguishable from a slow pass. */
#define CM_CONNECT_MAX 600
#define CM_INPUT_MAX   600

/* End-to-end: join over the loopback harness, confirm the server stored a
 * non-zero connId, and drive inputs through the new framing to the sim. */
int run_conn_migration_e2e(void) {
    LoopbackHarness h;
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Migrator", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 7u),
                  "harness start failed");

    int connectedAt = loopbackHarnessPumpUntil(&h, CM_CONNECT_MAX,
                                               cm_pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CM_CONNECT_MAX);
    }

    BYTE slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS) {
        loopbackHarnessStop(&h);
        UT_FAIL("client was assigned no slot (got %u)", (unsigned)slot);
    }

    /* The server generated and stored a non-zero connId for the slot. */
    uint64_t srvConnId = transportUdpServerGetClientConnId(slot);
    if (srvConnId == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("server slot %d has connId 0 after join", (int)slot);
    }

    /* Drive inputs and confirm they reach the sim: lastProcessedInput
     * advancing proves the client write offset and server read offset agree
     * (a misaligned connId prefix would misread the count and inputs). Also
     * confirm a snapshot returns and the session stays connected. */
    int snapshotAt = -1;
    int i;
    for (i = 1; i <= CM_INPUT_MAX; i++) {
        InputPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick      = (uint32_t)i;
        pkt.playerNum = slot;
        clientSimNetSendInput(h.cs, &pkt);
        loopbackHarnessPump(&h);

        int ppsRecv = 0, ppsSent = 0, bpsRecv = 0, bpsSent = 0, numErrors = 0;
        int snapsRecv = 0, snapsLost = 0, snapsLostTotal = 0;
        clientSimGetUdpNetStats(h.cs, &ppsRecv, &ppsSent, &bpsRecv, &bpsSent,
                                &numErrors, &snapsRecv, &snapsLost,
                                &snapsLostTotal);
        if (snapsRecv > 0) {
            snapshotAt = i;
            break;
        }
    }

    if (snapshotAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("no snapshot within %d input pumps", CM_INPUT_MAX);
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped out of CONNECTED while exchanging inputs");
    }
    if (h.sim->lastProcessedInput[slot] == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("server processed no inputs from the new framing "
                "(lastProcessedInput[%d] == 0)", (int)slot);
    }

    loopbackHarnessStop(&h);
    return 0;
}
