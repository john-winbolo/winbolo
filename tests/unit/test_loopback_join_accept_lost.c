/*
 * A joiner whose first JOIN_ACCEPT is lost still readies its own lobby slot.
 *
 * The server sends JOIN_ACCEPT once as a raw datagram and the lobby replay on
 * the reliable channel. The client applies channel packets in any join state,
 * so when the accept is lost the lobby can open (clientFrontAwaitJoin returns)
 * while the transport's player number is still 0. The client keeps sending
 * JOIN, the server resends the accept, and the transport and ClientSim then
 * learn the real slot.
 *
 * The lobby used to read its own slot from a copy gameFrontGetPlayerNum took
 * once, when clientFrontAwaitJoin returned. That copy stayed 0, so the Ready
 * button showed slot 0 (the ready host) as "Unready", and a click sent
 * ready=false for the joiner's real slot, which was already not ready. The
 * player could never ready up. gameFrontGetPlayerNum now reads the slot live
 * through clientSimGetServerPlayerNum; gamefront.c is not linked here, so this
 * case checks that getter.
 *
 * What it pins:
 *   - The joiner's first PACKET_JOIN_ACCEPT really was dropped.
 *   - After the resent accept, clientSimGetServerPlayerNum on the joiner is
 *     the slot the server gave it (found by name on the server), and agrees
 *     with clientSimGetMyPlayerNum.
 *   - With the host ready and the joiner not, CMD_READY true sent through
 *     clientSimNetSendReady (the path the Ready button takes) sets the
 *     joiner's slot ready on the server and in the joiner's own lobby view.
 *
 * What it only prints: the slot clientSimGetServerPlayerNum returned at the
 * first pump the joiner was in the lobby — the moment clientFrontAwaitJoin
 * would have returned and the old copy was taken. It is 0 when the lobby
 * replay beat the resent accept, but that order depends on timing, and the
 * fix does not change it, so it is not asserted.
 *
 * Clean path apart from the one chosen drop. Server state is read off the
 * ServerSim struct (the unittests profile permits internal access).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lobbyPlayers[].ready */
#include "client_sim.h"
#include "client_net.h"            /* clientSimNetSendReady, connect state */
#include "client_connect_state.h"
#include "game_sim.h"
#include "players.h"               /* playersIsInUse, playersGetPlayerName */
#include "transport_udp.h"         /* PACKET_JOIN_ACCEPT */
#include "test_harness.h"
#include "loopback_harness.h"

#define ACCEPT_LOST_SEED 0xACCE97u

/* Join plus the map download on a clean path. The other clean-path loopback
 * cases budget 2000 pumps for the same work; the lost accept costs one JOIN
 * retry interval on top. */
#define ACCEPT_LOST_CONNECT_MAX 2000

/* A CMD_READY round trip on a clean path is a few pumps; this is a
 * convergence bound, not an estimate. */
#define ACCEPT_LOST_READY_MAX 600

static const char *const kHostName   = "Host";
static const char *const kJoinerName = "Joiner";

/* The slot the server holds for `name`, or 0xFF when no slot has it. */
static BYTE acceptLostServerSlotFor(LoopbackHarness *h, const char *name) {
    GameSim *gs = serverSimGetGameSim(h->sim);
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        char buf[PLAYER_NAME_LEN];
        if (playersIsInUse(&gs->plyrs, i) != TRUE) continue;
        playersGetPlayerName(&gs->plyrs, i, buf, sizeof(buf), TRUE);
        if (strcmp(buf, name) == 0) return i;
    }
    return 0xFF;
}

static bool acceptLostHostJoined(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           acceptLostServerSlotFor(h, kHostName) != 0xFF;
}

static bool acceptLostSlotReady(LoopbackHarness *h, void *user) {
    BYTE slot = *(const BYTE *)user;
    return h->sim->lobbyPlayers[slot].ready;
}

typedef struct {
    ClientSim *cs;
    BYTE       slot;
} AcceptLostReadyView;

static bool acceptLostBothSeeReady(LoopbackHarness *h, void *user) {
    const AcceptLostReadyView *v = (const AcceptLostReadyView *)user;
    const ClientLobbySlot *ls = clientSimGetLobbySlot(v->cs, v->slot);
    return h->sim->lobbyPlayers[v->slot].ready &&
           ls != NULL && ls->ready;
}

int run_loopback_join_accept_lost_ready(void) {
    LoopbackHarness h;
    BYTE hostSlot, joinerSlot, liveSlot;
    BYTE slotAtLanding = 0xFF;
    bool landed = false;
    int i, connectedAt = -1;
    AcceptLostReadyView view;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, kHostName, /*lobbyMode*/ true,
                                       NULL, ACCEPT_LOST_SEED),
                  "loopback harness start failed");

    /* Host joins first and takes the low slot. */
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, ACCEPT_LOST_CONNECT_MAX,
                                           acceptLostHostJoined, NULL) > 0,
                  "host never joined the lobby");
    hostSlot = acceptLostServerSlotFor(&h, kHostName);
    fprintf(stderr, "  host slot=%u\n", (unsigned)hostSlot);

    /* Joiner connects with its first JOIN_ACCEPT dropped. The connect only
     * queues the JOIN; the server answers on a later pump, so arming the drop
     * here is in time. */
    UT_ASSERT_MSG(loopbackHarnessAddClient(&h, kJoinerName),
                  "second client connect failed");
    loopbackHarnessDropNextToClient(&h, h.cs2, PACKET_JOIN_ACCEPT, 1);

    for (i = 1; i <= ACCEPT_LOST_CONNECT_MAX; i++) {
        loopbackHarnessPump(&h);
        if (!landed && clientSimIsInLobby(h.cs2)) {
            /* The moment clientFrontAwaitJoin returns. */
            landed = true;
            slotAtLanding = clientSimGetServerPlayerNum(h.cs2);
            fprintf(stderr, "  joiner in lobby at pump %d: "
                    "clientSimGetServerPlayerNum=%u (old copy taken here), "
                    "accept drops left=%d\n",
                    i, (unsigned)slotAtLanding,
                    loopbackHarnessDropNextLeft(&h, h.cs2));
        }
        if (clientSimGetConnectState(h.cs2) == CLIENT_CONNECT_CONNECTED &&
            acceptLostServerSlotFor(&h, kJoinerName) != 0xFF) {
            connectedAt = i;
            break;
        }
    }
    UT_ASSERT_MSG(connectedAt > 0, "joiner never connected after lost accept");
    UT_ASSERT_MSG(landed, "joiner connected without entering the lobby");
    UT_ASSERT_MSG(loopbackHarnessDropNextLeft(&h, h.cs2) == 0,
                  "the first JOIN_ACCEPT was never dropped");

    /* The live getter gameFrontGetPlayerNum reads now names the joiner's own
     * slot, not the host's. */
    joinerSlot = acceptLostServerSlotFor(&h, kJoinerName);
    liveSlot   = clientSimGetServerPlayerNum(h.cs2);
    fprintf(stderr, "  joiner connected at pump %d: server slot=%u live=%u "
            "(at landing=%u)\n", connectedAt, (unsigned)joinerSlot,
            (unsigned)liveSlot, (unsigned)slotAtLanding);
    UT_ASSERT_MSG(joinerSlot != hostSlot, "joiner shares the host's slot");
    UT_ASSERT_MSG(liveSlot == joinerSlot,
                  "live player number does not match the server's slot");
    UT_ASSERT_MSG(clientSimGetMyPlayerNum(h.cs2) == joinerSlot,
                  "ClientSim slot does not match the server's slot");

    /* Host readies; the joiner stays not ready, so no countdown starts. */
    clientSimNetSendReady(h.cs, true);
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, ACCEPT_LOST_READY_MAX,
                                           acceptLostSlotReady,
                                           &hostSlot) > 0,
                  "host ready never reached the server");
    UT_ASSERT_MSG(!h.sim->lobbyPlayers[joinerSlot].ready,
                  "joiner is ready before it asked to be");

    /* The Ready button path: CMD_READY true through the client. */
    clientSimNetSendReady(h.cs2, true);
    view.cs   = h.cs2;
    view.slot = liveSlot;
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, ACCEPT_LOST_READY_MAX,
                                           acceptLostBothSeeReady,
                                           &view) > 0,
                  "joiner's ready never reached its slot");

    loopbackHarnessStop(&h);
    return 0;
}
