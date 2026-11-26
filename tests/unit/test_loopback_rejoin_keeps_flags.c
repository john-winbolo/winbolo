/*
 * A lobby map change keeps the client's own player flags.
 *
 * A lobby map change sends every connected client through the join again:
 * the server sends PACKET_LOBBY_MAP_CHANGE, the client sends a new JOIN, and
 * the server's already-connected branch sends the JOIN_ACCEPT again. That
 * branch sends no roster replay, so no CTRL_PLAYER_JOIN follows the accept.
 * The accept handler used to give the client's own slot type 0 and flags 0
 * and wait for the replay to put the server's values back. After a map change
 * nothing put them back.
 *
 * The snapshot gives back only the account and voice bits, so the bits it
 * does not carry stayed 0 for the rest of the connection. One of them is
 * PLAYER_FLAG_BOT. A bot on a Bot Host plays through a UDP connection of its
 * own, and its brain reads its own bit in info.player_bots. A bot seat on a
 * Bot Host stays connected from one round to the next, and every return to the
 * lobby is a map change. So in the next round the bot lost its own bit, and it
 * took its own ATTACK markers as orders from a human.
 *
 * The test seats a client, marks its slot a bot on the server the way a bot
 * seat is marked, and checks that the client sees its own bit. Then it makes
 * a lobby map change and requires the bit to be there when the client is back.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "server_sim.h"
#include "server_sim_join.h"      /* setClientTypeFlagsInternal,
                                   * fillAndPublishPlayerJoin */
#include "players.h"              /* playersGetClientFlags/Type */
#include "player_flags.h"         /* PLAYER_FLAG_BOT */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define KEEPFLAGS_CONNECT_MAX 4000   /* join + first map download */
#define KEEPFLAGS_STAMP_MAX   1000   /* the PLAYER_JOIN to reach the client */
#define KEEPFLAGS_CHANGE_MAX  4000   /* the SET_MAP to land and the client to settle */
#define KEEPFLAGS_AFTER       100    /* more pumps once the client is back */

/* A server map that is not the harness's starting map. */
#define KEEPFLAGS_MAP      "Baringi.map"
#define KEEPFLAGS_MAP_NAME "Baringi"

static uint8_t own_flags(LoopbackHarness *h) {
    ClientSim *cs = h->cs;
    return playersGetClientFlags(&clientSimGetGameSim(cs)->plyrs,
                                 clientSimGetMyPlayerNum(cs));
}

static bool pred_in_lobby(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           clientSimIsInLobby(h->cs) &&
           clientSimIsMapDownloadComplete(h->cs);
}

static bool pred_own_bot_bit(LoopbackHarness *h, void *user) {
    (void)user;
    return (own_flags(h) & PLAYER_FLAG_BOT) != 0;
}

/* The client has left CONNECTED for the re-join the map change sends it on. */
static bool pred_rejoining(LoopbackHarness *h, void *user) {
    (void)user;
    ClientConnectState st = clientSimGetConnectState(h->cs);
    return st == CLIENT_CONNECT_JOINING || st == CLIENT_CONNECT_DOWNLOADING_MAP;
}

/* The server is on the new map and the client has its download back. */
static bool pred_settled_on(LoopbackHarness *h, void *user) {
    const char *name = (const char *)user;
    const char *now = serverSimGetMapName(h->sim);
    return now != NULL && strcmp(now, name) == 0 &&
           clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           clientSimIsMapDownloadComplete(h->cs);
}

int run_loopback_map_change_keeps_own_flags(void) {
    LoopbackHarness h;
    int at, i;
    BYTE slot;
    uint8_t serverFlags, before, after;

    if (!loopbackHarnessStart(&h, "KeepFlags", /*lobbyMode*/ true,
                              /*impairSpec*/ NULL, /*seed*/ 0x5E7B1u)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (map change keeps own flags) failed");
    }
    if (loopbackHarnessPumpUntil(&h, KEEPFLAGS_CONNECT_MAX,
                                 pred_in_lobby, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached the lobby within %d pumps",
                KEEPFLAGS_CONNECT_MAX);
    }
    slot = clientSimGetMyPlayerNum(h.cs);

    /* Mark the slot a bot on the server and announce it, as the server does
     * for a bot seat (serverSimAddBot stamps the bit, then publishes the
     * join). The server maps are pointed at the source tree so the SET_MAP
     * below resolves wherever the test runs. */
    threadsWaitForMutex();
    serverFlags = (uint8_t)(playersGetClientFlags(
                                &serverSimGetGameSim(h.sim)->plyrs, slot) |
                            PLAYER_FLAG_BOT);
    setClientTypeFlagsInternal(
        h.sim, slot,
        playersGetClientType(&serverSimGetGameSim(h.sim)->plyrs, slot),
        serverFlags);
    fillAndPublishPlayerJoin(h.sim, slot);
    serverSimInstallMapDirList(h.sim, NULL, 0, WB_DATA_MAPS_DIR);
    threadsReleaseMutex();

    if (loopbackHarnessPumpUntil(&h, KEEPFLAGS_STAMP_MAX,
                                 pred_own_bot_bit, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never saw its own PLAYER_FLAG_BOT after the "
                "server announced it");
    }
    before = own_flags(&h);

    /* The map change: the client re-joins and is sent the accept again. */
    clientSimNetSendLobbySetMap(h.cs, KEEPFLAGS_MAP);
    if (loopbackHarnessPumpUntil(&h, KEEPFLAGS_CHANGE_MAX,
                                 pred_rejoining, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the map change never sent the client to re-join");
    }
    at = loopbackHarnessPumpUntil(&h, KEEPFLAGS_CHANGE_MAX, pred_settled_on,
                                  (void *)KEEPFLAGS_MAP_NAME);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the map change never landed and settled");
    }
    for (i = 0; i < KEEPFLAGS_AFTER; i++) {
        loopbackHarnessPump(&h);
    }

    after = own_flags(&h);
    fprintf(stderr, "  map change keeps own flags: slot %u server 0x%02x "
                    "client before 0x%02x after 0x%02x (settled@%d)\n",
            (unsigned)slot, (unsigned)serverFlags, (unsigned)before,
            (unsigned)after, at);
    if ((after & PLAYER_FLAG_BOT) == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("after the lobby map change the client's own slot lost "
                "PLAYER_FLAG_BOT (flags 0x%02x, were 0x%02x): the re-sent "
                "JOIN_ACCEPT set them to 0 and no PLAYER_JOIN followed",
                (unsigned)after, (unsigned)before);
    }

    loopbackHarnessStop(&h);
    return 0;
}
