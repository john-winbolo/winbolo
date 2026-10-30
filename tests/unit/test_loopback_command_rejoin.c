/*
 * A command queued behind a lobby map change still reaches the server.
 *
 * Client commands ride one reliable queue: the head is sent at once, and a
 * command submitted while an earlier one is unacked waits behind it, unsent,
 * for the next drain. A lobby map change is itself a command (CMD_LOBBY_SET_MAP),
 * and the server answers it in a fixed order: applying it sends the raw
 * PACKET_LOBBY_MAP_CHANGE and JOIN_ACCEPT, and only then does the
 * PACKET_COMMAND_ACK go out. So the ACK reaches a client that has already
 * left CONNECTED to re-join. The ACK handler advances the head onto the
 * waiting command and drains, but the drain sends nothing outside CONNECTED,
 * and the periodic retransmit only resends a head that has been sent before.
 * The waiting command was never sent, so nothing ever sent it, and every
 * command after it queued behind it for the rest of the connection: map
 * picks, Use This Map, Ready, chat.
 *
 * In the wild the waiting command was most likely the voice state the client
 * reports every few seconds, landing inside the map change's round trip; a
 * PLAYER_MUTE stands in for it here because the server answers it without
 * publishing anything. The test then makes a second map change the way a
 * player would, after the client is back, and requires it to land.
 *
 * The window the old code lost is the time between the map change's send and
 * its ACK arriving: under 80 ms the retransmit has not yet stamped the waiting
 * command, which a clean localhost round trip always is.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "server_sim.h"
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define REJOIN_CONNECT_MAX 4000   /* join + first map download */
#define REJOIN_CHANGE_MAX  4000   /* one SET_MAP to land and the client to settle */

/* Two server maps that are neither the harness's starting map nor each other,
 * so each change is visible in the server's map name. */
#define REJOIN_FIRST_MAP   "Baringi.map"
#define REJOIN_FIRST_NAME  "Baringi"
#define REJOIN_SECOND_MAP  "Bad Lands.map"
#define REJOIN_SECOND_NAME "Bad Lands"

static bool pred_in_lobby(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           clientSimIsInLobby(h->cs) &&
           clientSimIsMapDownloadComplete(h->cs);
}

/* The server is on the named map and the client has its download back. */
static bool pred_settled_on(LoopbackHarness *h, void *user) {
    const char *name = (const char *)user;
    const char *now = serverSimGetMapName(h->sim);
    return now != NULL && strcmp(now, name) == 0 &&
           clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           clientSimIsMapDownloadComplete(h->cs);
}

int run_loopback_command_queued_behind_map_change(void) {
    LoopbackHarness h;
    int at;
    int slot;
    char mapName[MAP_STR_SIZE];

    if (!loopbackHarnessStart(&h, "CmdRejoin", /*lobbyMode*/ true,
                              /*impairSpec*/ NULL, /*seed*/ 0x5E7A4u)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (command behind map change) failed");
    }
    if (loopbackHarnessPumpUntil(&h, REJOIN_CONNECT_MAX,
                                 pred_in_lobby, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached the lobby within %d pumps",
                REJOIN_CONNECT_MAX);
    }
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    /* The server resolves a SET_MAP name against its map root, which defaults
     * to "data/maps" relative to the working directory. Point it at the shipped
     * maps in the source tree so the test does not depend on where it runs. */
    threadsWaitForMutex();
    serverSimInstallMapDirList(h.sim, NULL, 0, WB_DATA_MAPS_DIR);
    threadsReleaseMutex();

    /* The map change goes out at once; the mute queues behind it unsent. */
    clientSimNetSendLobbySetMap(h.cs, REJOIN_FIRST_MAP);
    clientSimNetSendPlayerMute(h.cs, (BYTE)((slot + 1) % MAX_TANKS), true);

    at = loopbackHarnessPumpUntil(&h, REJOIN_CHANGE_MAX, pred_settled_on,
                                  (void *)REJOIN_FIRST_NAME);
    /* Each name is copied out before any Stop below, which frees the sim. */
    if (at < 0) {
        SDL_strlcpy(mapName, serverSimGetMapName(h.sim), sizeof(mapName));
        loopbackHarnessStop(&h);
        UT_FAIL("first map change never landed (server map '%s')", mapName);
    }

    /* A later pick, made the way a player makes it once the lobby is back. */
    clientSimNetSendLobbySetMap(h.cs, REJOIN_SECOND_MAP);
    at = loopbackHarnessPumpUntil(&h, REJOIN_CHANGE_MAX, pred_settled_on,
                                  (void *)REJOIN_SECOND_NAME);
    SDL_strlcpy(mapName, serverSimGetMapName(h.sim), sizeof(mapName));
    fprintf(stderr, "  command behind map change: second change landed@%d "
                    "server map '%s'\n", at, mapName);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("a map change made after the client re-joined never reached "
                "the server (still on '%s'): the command queued behind the "
                "first change was never sent, and everything after it waits "
                "behind it", mapName);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* The client has left CONNECTED for the re-join a map change sends it on. */
static bool pred_rejoining(LoopbackHarness *h, void *user) {
    (void)user;
    ClientConnectState st = clientSimGetConnectState(h->cs);
    return st == CLIENT_CONNECT_JOINING || st == CLIENT_CONNECT_DOWNLOADING_MAP;
}

/*
 * A command submitted while the client is re-joining after a map change
 * still reaches the server.
 *
 * The case above is a command already on the queue when the re-join starts.
 * This is one submitted during it: a host who clicks a second map in the
 * chooser, or presses Use This Map, before the first pick's re-join and map
 * download are done. The submit used to throw away anything outside
 * CONNECTED, so the pick was lost without a word while the chooser's own
 * preview, fetched separately, showed the map as picked. On a remote server
 * the re-join is several round trips and a map download long, which is
 * plenty of time to click.
 */
int run_loopback_command_submitted_during_rejoin(void) {
    LoopbackHarness h;
    int at;
    char mapName[MAP_STR_SIZE];

    if (!loopbackHarnessStart(&h, "CmdDuringRejoin", /*lobbyMode*/ true,
                              /*impairSpec*/ NULL, /*seed*/ 0x5E7A5u)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (command during re-join) failed");
    }
    if (loopbackHarnessPumpUntil(&h, REJOIN_CONNECT_MAX,
                                 pred_in_lobby, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached the lobby within %d pumps",
                REJOIN_CONNECT_MAX);
    }

    threadsWaitForMutex();
    serverSimInstallMapDirList(h.sim, NULL, 0, WB_DATA_MAPS_DIR);
    threadsReleaseMutex();

    clientSimNetSendLobbySetMap(h.cs, REJOIN_FIRST_MAP);
    if (loopbackHarnessPumpUntil(&h, REJOIN_CHANGE_MAX,
                                 pred_rejoining, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the first map change never sent the client to re-join");
    }

    /* The second pick, made while the first one's re-join is under way. */
    clientSimNetSendLobbySetMap(h.cs, REJOIN_SECOND_MAP);
    at = loopbackHarnessPumpUntil(&h, REJOIN_CHANGE_MAX, pred_settled_on,
                                  (void *)REJOIN_SECOND_NAME);
    SDL_strlcpy(mapName, serverSimGetMapName(h.sim), sizeof(mapName));
    fprintf(stderr, "  command during re-join: second change landed@%d "
                    "server map '%s'\n", at, mapName);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("a map change submitted while the client was re-joining "
                "never reached the server (still on '%s'): the submit threw "
                "it away outside CONNECTED", mapName);
    }

    loopbackHarnessStop(&h);
    return 0;
}
