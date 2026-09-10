/*
 * Smart ping across the wire (test_ping_network.c).
 *
 * The other ping tests stop at the seams: test_ping.c pins the geometry and
 * the EVENT_PING wire shape, and test_ping_dispatch.c pins the CMD_PING arm
 * and the team-only reach predicate. Both call into the pieces directly. What
 * neither exercises is the whole path end to end over a real connection:
 *
 *   client -> clientSimNetSendPing -> CMD_PING on the wire
 *          -> server recv -> serverSimApplyCommand (CMD_PING arm) -> EVENT_PING
 *          -> serverInstanceTick -> transportUdpServerDrainEvents (CHANNEL_GAME)
 *          -> client decode -> clientSimApplyGameEvents (EVENT_PING arm)
 *          -> clientSimAddPing -> clientSimGetPings
 *
 * and that is exactly the path a networked ping fails on. The loopback harness
 * stands up the real UDP server and client over localhost and pumps them one
 * tick at a time through the production serverInstanceTick / clientSimNetTick,
 * so nothing here is stubbed — the CMD_PING codec, the dispatch arm, the event
 * drain and the client ingest are all the shipping code.
 *
 * Crucially the pump goes through serverInstanceTick, NOT a direct
 * serverSimAddEvent + transportUdpServerDrainEvents (the shortcut
 * test_loopback_channel.c's overflow case takes). That matters: a ping is
 * emitted from the command-dispatch path, which runs during packet receive —
 * BEFORE serverSimTick clears the per-frame event buffer. A test that drains
 * events by hand skips that clear and would pass even with the bug present.
 * Driving the real per-frame ordering is what makes this a faithful repro.
 *
 *   1. Self-echo. A single player in a running game pings. The server sends a
 *      player its own ping back like everyone else's (the client draws nothing
 *      until the echo arrives), so the sender must see it come back over the
 *      wire. This is the tightest repro of the drain-ordering bug: no alliance
 *      needed, since serverSimPingReachesClient(sender, sender) is always true.
 *
 *   2. Teammate delivery, and the opponent who must not see it. Three players:
 *      two on one team, one on another. The first pings; the teammate must
 *      receive it over the wire — the scenario the feature exists for — and
 *      the opponent must still hold no ping at all once the exchange has run
 *      on. Pins both directions of the team-only reach filter on the real
 *      drain path, not just in the predicate's own unit test: a filter that
 *      was dropped or inverted there fails one half or the other.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"                 /* M_W_SHIFT_SIZE */
#include "client_sim.h"             /* ClientPing, clientSimGetPings, MAX_CLIENT_PINGS */
#include "client_net.h"             /* clientSimNetSendPing, clientSimNetTick */
#include "client_connect_state.h"   /* CLIENT_CONNECT_CONNECTED */
#include "input_packet.h"           /* PING_KIND_*, EVENT_PING */
#include "server_sim.h"             /* serverSimReapplyTeamAlliances */
#include "server_sim_lifecycle.h"   /* serverSimSetTeamBatch */
#include "server_lifecycle.h"       /* serverInstanceTick */
#include "transport_udp.h"          /* transportUdpServerTestDownloadComplete */
#include "threads.h"                /* threadsWaitForMutex / threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

/* A world position comfortably inside Everard Island — the middle of square
 * (80, 90). World units are 256 to a square (M_W_SHIFT_SIZE == 8). Matches the
 * point test_ping_dispatch.c uses so the two tests point at the same place. */
#define PN_WORLD_X ((uint16_t)((80 << M_W_SHIFT_SIZE) + 128))
#define PN_WORLD_Y ((uint16_t)((90 << M_W_SHIFT_SIZE) + 128))

#define CONNECT_MAX   2000   /* join + map download */
#define PING_MAX       600   /* command up-leg + event down-leg over the wire */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* The server flips a slot's downloadComplete a CHANNEL_BULK ack after the
 * client reports CONNECTED; the event producer skips a still-downloading slot,
 * so a wire ping delivered before this would never be drained. Wait for it. */
static bool pred_server_download_complete(LoopbackHarness *h, void *user) {
    (void)user;
    return transportUdpServerTestDownloadComplete(
        (int)clientSimGetMyPlayerNum(h->cs));
}

/* Is a ping from `wantSender` of `wantKind` at (PN_WORLD_X, PN_WORLD_Y) in this
 * client's ping ring right now? Reads through the shipping getter with a fresh
 * clock so the age filter is the real one. */
static bool client_has_ping(ClientSim *cs, uint8_t wantSender, uint8_t wantKind) {
    ClientPing pings[MAX_CLIENT_PINGS];
    int n = clientSimGetPings(cs, (uint32_t)SDL_GetTicks(), pings, MAX_CLIENT_PINGS);
    int i;
    for (i = 0; i < n; i++) {
        if (pings[i].sender == wantSender && pings[i].kind == wantKind &&
            pings[i].worldX == PN_WORLD_X && pings[i].worldY == PN_WORLD_Y) {
            return true;
        }
    }
    return false;
}

/* ── Case 1: a lone player's ping echoes back to itself over the wire. ────── */
static int run_ping_self_echo(void) {
    LoopbackHarness h;
    uint8_t slot;
    bool got = false;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Pinger", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0x9114Eu),
                  "harness start (self echo) failed");

    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX,
                                 pred_server_download_complete, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("server never completed the join download within %d pumps",
                CONNECT_MAX);
    }

    slot = clientSimGetMyPlayerNum(h.cs);

    /* Fire one ping and let it round-trip: the command rides the next input
     * datagram to the server, the accepted CMD_PING becomes an EVENT_PING, and
     * the event must come back down CHANNEL_GAME to this same client. */
    clientSimNetSendPing(h.cs, PING_KIND_ATTACK, PN_WORLD_X, PN_WORLD_Y);

    for (i = 1; i <= PING_MAX; i++) {
        loopbackHarnessPump(&h);
        if (client_has_ping(h.cs, slot, PING_KIND_ATTACK)) {
            got = true;
            break;
        }
    }

    fprintf(stderr, "  ping self-echo: sender slot=%u received=%d after %d pump(s)\n",
            (unsigned)slot, (int)got, i);

    if (!got) {
        loopbackHarnessStop(&h);
        UT_FAIL("a player's own ping never came back over the wire within %d "
                "pumps — the EVENT_PING is emitted during command receive but "
                "serverSimTick clears the event buffer before "
                "transportUdpServerDrainEvents can send it", PING_MAX);
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped during the self-echo exchange");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* How many pings are in this client's ring at all, of any sender or kind. A
 * client on another team must finish the exchange with none: the team filter
 * lives on the server's drain path, so an opponent should never be handed the
 * event to ingest in the first place. */
static int client_ping_count(ClientSim *cs) {
    ClientPing pings[MAX_CLIENT_PINGS];
    return clientSimGetPings(cs, (uint32_t)SDL_GetTicks(), pings,
                             MAX_CLIENT_PINGS);
}

/* ── Case 2: one teammate pings; the other receives it, an opponent does not. */

/* An extra real UDP client sharing the harness's server + port. The harness
 * owns only one; these are stood up and torn down here. */
typedef struct {
    ClientSim *cs;
    bool       up;
} SecondClient;

static bool second_connect(SecondClient *b, LoopbackHarness *h,
                           const char *name) {
    memset(b, 0, sizeof(*b));
    b->cs = clientSimAlloc();
    if (b->cs == NULL) return false;
    clientSimCreate(b->cs);
    if (!clientSimConnectUdp(b->cs, "127.0.0.1", h->port, name,
                             /*fallbackCountry*/ "", /*password*/ "",
                             /*wbnApiToken*/ NULL, /*wbnServerKey*/ NULL,
                             /*wantRejoin*/ false, /*trackerAddr*/ "",
                             /*trackerPort*/ 0, /*spectator*/ false)) {
        return false;
    }
    b->up = true;
    return true;
}

static void second_stop(SecondClient *b) {
    if (b->cs != NULL) {
        if (b->up) clientSimDisconnect(b->cs);
        clientSimDestroy(b->cs);
        b->cs = NULL;
    }
    b->up = false;
}

/* B, the teammate, and C, the opponent. */
#define PN_EXTRAS 2

/* One tick for every client and the server. Mirrors loopbackHarnessPump but
 * ticks the extra clients too, so all endpoints drain the same server frame.
 * The 1ms yield lets the server's background recv thread deliver this pump's
 * datagrams before serverInstanceTick drains the queue (same reason as the
 * harness pump). */
static void pump_all(LoopbackHarness *h, SecondClient *extra, int n) {
    int i;
    if (h->clientUp) clientSimNetTick(h->cs);
    for (i = 0; i < n; i++) {
        if (extra[i].up) clientSimNetTick(extra[i].cs);
    }
    SDL_Delay(1);
    if (h->serverUp) serverInstanceTick(h->sim);
}

/* Once the harness and the extra clients are up, no failure may return
 * straight out of the test: that would leak two ClientSims and their UDP
 * sockets, and leave a server bound to the harness's port for whatever runs
 * next. So a failure is recorded here and the test jumps to the single
 * teardown at `done`, which prints it after everything is stopped. */
static char pn_fail[512];

#define PN_FAIL(fmt, ...)                                                   \
    do {                                                                    \
        snprintf(pn_fail, sizeof(pn_fail), "%s:%d: " fmt,                   \
                 __FILE__, __LINE__, ##__VA_ARGS__);                        \
        goto done;                                                          \
    } while (0)

/* How long to keep pumping after B has the ping, before asking whether C got
 * one too. A ping that is wrongly drained to C rides the very next server
 * frame, so this only has to outlast the wire, not a timeout. */
#define PN_SETTLE 120

static int run_ping_teammate_delivery(void) {
    LoopbackHarness h;
    SecondClient extra[PN_EXTRAS];
    SecondClient *b = &extra[0];   /* PingerB — A's teammate */
    SecondClient *c = &extra[1];   /* PingerC — the other team */
    uint8_t slotA = 0, slotB = 0, slotC = 0;
    bool got = false;
    int cPings = 0;
    int i;

    pn_fail[0] = '\0';
    memset(extra, 0, sizeof(extra));

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "PingerA", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0x7EA33u),
                  "harness start (teammate) failed");
    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL) < 0) {
        PN_FAIL("client A never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    if (!second_connect(b, &h, "PingerB")) PN_FAIL("client B connect failed");
    if (!second_connect(c, &h, "PingerC")) PN_FAIL("client C connect failed");

    /* Pump all three until B and C are connected and every slot has finished
     * downloading — the producer will only drain to a slot the server has
     * marked complete. */
    for (i = 1; i <= CONNECT_MAX; i++) {
        pump_all(&h, extra, PN_EXTRAS);
        if (clientSimGetConnectState(b->cs) == CLIENT_CONNECT_CONNECTED &&
            clientSimGetConnectState(c->cs) == CLIENT_CONNECT_CONNECTED) {
            break;
        }
    }
    if (clientSimGetConnectState(b->cs) != CLIENT_CONNECT_CONNECTED) {
        PN_FAIL("client B never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    if (clientSimGetConnectState(c->cs) != CLIENT_CONNECT_CONNECTED) {
        PN_FAIL("client C never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    slotA = clientSimGetMyPlayerNum(h.cs);
    slotB = clientSimGetMyPlayerNum(b->cs);
    slotC = clientSimGetMyPlayerNum(c->cs);
    if (!(slotA < MAX_TANKS && slotB < MAX_TANKS && slotC < MAX_TANKS &&
          slotA != slotB && slotA != slotC && slotB != slotC)) {
        PN_FAIL("unexpected slots A=%u B=%u C=%u",
                (unsigned)slotA, (unsigned)slotB, (unsigned)slotC);
    }

    for (i = 1; i <= CONNECT_MAX; i++) {
        pump_all(&h, extra, PN_EXTRAS);
        if (transportUdpServerTestDownloadComplete(slotA) &&
            transportUdpServerTestDownloadComplete(slotB) &&
            transportUdpServerTestDownloadComplete(slotC)) {
            break;
        }
    }
    if (!transportUdpServerTestDownloadComplete(slotA) ||
        !transportUdpServerTestDownloadComplete(slotB) ||
        !transportUdpServerTestDownloadComplete(slotC)) {
        PN_FAIL("the three slots never finished the join download within %d "
                "pumps", CONNECT_MAX);
    }

    /* A and B share a team, C gets its own, and alliances are reconciled once.
     * The batch form + a single reapply is what the lobby's multi-slot paths
     * use (see test_ping_dispatch.c's run_ping_reaches_team_only for why
     * one-at-a-time would mis-ally the slots on the way through). */
    threadsWaitForMutex();
    serverSimSetTeamBatch(h.sim, slotA, 1);
    serverSimSetTeamBatch(h.sim, slotB, 1);
    serverSimSetTeamBatch(h.sim, slotC, 2);
    serverSimReapplyTeamAlliances(h.sim);
    threadsReleaseMutex();

    /* A pings; B must receive it, carrying A's slot as the sender. */
    clientSimNetSendPing(h.cs, PING_KIND_ASSIST, PN_WORLD_X, PN_WORLD_Y);

    for (i = 1; i <= PING_MAX; i++) {
        pump_all(&h, extra, PN_EXTRAS);
        if (client_has_ping(b->cs, slotA, PING_KIND_ASSIST)) {
            got = true;
            break;
        }
    }

    /* Keep the exchange running a while longer. C's copy, if the server were
     * to send one, arrives on the same frames B's did — so this is where a
     * missing team filter on the drain path would show up. */
    for (i = 1; i <= PN_SETTLE; i++) pump_all(&h, extra, PN_EXTRAS);
    cPings = client_ping_count(c->cs);

    fprintf(stderr, "  ping teammate delivery: A slot=%u -> B slot=%u received=%d, "
                    "off-team C slot=%u holds %d ping(s)\n",
            (unsigned)slotA, (unsigned)slotB, (int)got,
            (unsigned)slotC, cPings);

    if (!got) {
        PN_FAIL("a teammate's ping never reached the other client over the wire "
                "within %d pumps — the EVENT_PING is cleared by serverSimTick "
                "before transportUdpServerDrainEvents runs", PING_MAX);
    }
    if (cPings != 0) {
        PN_FAIL("a client on another team received %d ping(s) over the wire; "
                "the team filter on the drain path is not holding", cPings);
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED ||
        clientSimGetConnectState(b->cs) != CLIENT_CONNECT_CONNECTED ||
        clientSimGetConnectState(c->cs) != CLIENT_CONNECT_CONNECTED) {
        PN_FAIL("a client dropped during the teammate-delivery exchange");
    }

done:
    for (i = 0; i < PN_EXTRAS; i++) second_stop(&extra[i]);
    loopbackHarnessStop(&h);
    if (pn_fail[0] != '\0') {
        fprintf(stderr, "FAIL %s\n", pn_fail);
        return 1;
    }
    return 0;
}

int run_ping_network(void) {
    int rc = run_ping_self_echo();
    if (rc != 0) return rc;
    return run_ping_teammate_delivery();
}
