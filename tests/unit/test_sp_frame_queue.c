/*
 * Single-player frame queue and local map repair.
 *
 * Desktop single player ticks its ServerSim on a timer thread and polls the
 * passive local transport from the main thread. A snapshot carries only the
 * events of the frame it was built in, and serverSimTick clears them at the
 * top of every frame, so a poll that comes after two server frames never sees
 * the first frame's events. A tree cut down in that frame stayed standing on
 * the player's screen for the rest of the round: the map checksum noticed,
 * but only a UDP client answers a mismatch, with a resync.
 *
 * sp_frame_queue_keeps_skipped_frame — with the frame queue on, a change made
 *     in the first of two server frames reaches the client at its next poll.
 *     The same two frames with the queue off lose it, which is checked first
 *     so the case is known to reproduce the loss it guards against.
 * sp_local_map_repair — with the frame queue on, a client square that has
 *     drifted from the server's copy is put back at the next full sync; a
 *     mine the client knows of survives the repair and a mine it does not
 *     know of is not revealed by it.
 * sp_frame_queue_resets_on_lobby — an old tank snapshot cannot undo the
 *     lobby reset, and the next round still receives its frames.
 * sp_frame_queue_resets_on_map_change — old terrain events cannot alter
 *     a replacement map, even without a lobby-phase event.
 *
 * Single threaded: the test plays the timer thread's part itself, calling
 * serverInstanceTick and then clientSimNetCaptureLocalFrame under the threads
 * mutex, the way hostedServerTimerCb does, and polls with clientSimNetTick.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* scenarioTick, clientKnownMapObj */
#include "server_sim_lifecycle.h"
#include "server_lifecycle.h"
#include "game_sim.h"
#include "bolo_map.h"
#include "client_sim.h"
#include "client_sim_internal.h"   /* hasPredictedTank */
#include "client_net.h"
#include "threads.h"
#include "test_harness.h"

/* Server frames that are sure to cross a full sync (FULL_SYNC_INTERVAL is
 * 250 sim ticks, two to a frame) with room to spare. */
#define FQ_FULL_SYNC_FRAMES 200

/* The square the staged change cuts down, and whether the next frame should
 * cut it. */
static BYTE fqX, fqY;
static bool fqArmed;

/* Cuts the armed tree inside a running frame, through the scenario hook, so
 * the change is recorded as a map event the way any change in a frame is. */
static void fq_scenario_tick(void *ctx) {
    ServerSim *sim = (ServerSim *)ctx;
    GameSim *gs;
    if (!fqArmed) return;
    fqArmed = false;
    gs = serverSimGetGameSim(sim);
    mapSetPos(gs, &gs->mp, fqX, fqY, GRASS, FALSE, FALSE);
}

/* The skip-th square, counting from the map's top-left, that is forest on
 * both the server's map and the client's. */
static bool fq_find_forest(ServerSim *sim, ClientSim *cs, int skip,
                           BYTE *outX, BYTE *outY) {
    GameSim *gs = serverSimGetGameSim(sim);
    GameSim *cg = clientSimGetGameSim(cs);
    int x, y;
    for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
        for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
            if ((*gs->mp).mapItem[x][y] != FOREST ||
                (*cg->mp).mapItem[x][y] != FOREST) {
                continue;
            }
            if (skip-- > 0) continue;
            *outX = (BYTE)x;
            *outY = (BYTE)y;
            return true;
        }
    }
    return false;
}

/* One server frame as the desktop host timer runs it. */
static void fq_server_frame(ServerSim *sim, ClientSim *cs) {
    threadsWaitForMutex();
    serverInstanceTick(sim);
    clientSimNetCaptureLocalFrame(cs);
    threadsReleaseMutex();
}

static BYTE fq_client_tile(ClientSim *cs, BYTE x, BYTE y) {
    return (*clientSimGetGameSim(cs)->mp).mapItem[x][y];
}

/* A running single-player server with a passive local client in its slot,
 * the scenario hook armed and a few frames run. */
static bool fq_setup(ServerSim **outSim, ClientSim **outCs) {
    ServerInstanceConfig cfg;
    ServerSim *sim;
    ClientSim *cs;
    int i;

    if (!threadsCreate(TRUE)) return false;
    sim = ut_make_running_sim("Viewer");
    if (sim == NULL) return false;
    memset(&cfg, 0, sizeof(cfg));
    cfg.udpPort             = 0;
    cfg.bindAddr            = "";
    cfg.password            = "";
    cfg.maxPlayers          = MAX_TANKS;
    cfg.acceptRemoteClients = false;
    cfg.compTanks           = aiNone;
    cfg.trackerAddr         = "";
    if (!serverInstanceStartup(sim, &cfg)) return false;
    cs = clientSimAlloc();
    clientSimCreate(cs);
    if (!clientSimConnectLocalPassive(cs, sim, "Viewer", "", 0, 0)) return false;
    sim->scenarioTick = fq_scenario_tick;
    sim->scenarioTickCtx = sim;
    fqArmed = false;
    for (i = 0; i < 10; i++) {
        fq_server_frame(sim, cs);
        clientSimNetTick(cs);
    }
    *outSim = sim;
    *outCs = cs;
    return true;
}

static void fq_teardown(ServerSim *sim, ClientSim *cs) {
    clientSimDisconnect(cs);
    clientSimDestroy(cs);
    serverInstanceShutdown(sim);
    serverSimDestroy(sim);
    threadsDestroy();
}

static void fq_set_queue(ClientSim *cs, bool on) {
    threadsWaitForMutex();
    clientSimNetSetLocalFrameQueue(cs, on);
    threadsReleaseMutex();
}

int run_sp_frame_queue_keeps_skipped_frame(void) {
    ServerSim *sim = NULL;
    ClientSim *cs = NULL;
    BYTE lostX, lostY, keptX, keptY;

    UT_ASSERT(fq_setup(&sim, &cs));

    /* Queue off: the tree is cut in the first of two frames and the client
     * polls after the second. */
    UT_ASSERT(fq_find_forest(sim, cs, 0, &lostX, &lostY));
    fqX = lostX;
    fqY = lostY;
    fqArmed = true;
    fq_server_frame(sim, cs);
    fq_server_frame(sim, cs);
    clientSimNetTick(cs);
    UT_ASSERT((*serverSimGetGameSim(sim)->mp).mapItem[lostX][lostY] == GRASS);
    UT_ASSERT_MSG(fq_client_tile(cs, lostX, lostY) == FOREST,
                  "without the queue the skipped frame's change should be lost");

    /* Queue on: the same two frames, and the change arrives. */
    fq_set_queue(cs, true);
    UT_ASSERT(fq_find_forest(sim, cs, 40, &keptX, &keptY));
    fqX = keptX;
    fqY = keptY;
    fqArmed = true;
    fq_server_frame(sim, cs);
    fq_server_frame(sim, cs);
    clientSimNetTick(cs);
    UT_ASSERT_MSG(fq_client_tile(cs, keptX, keptY) == GRASS, "client holds %d",
                  (int)fq_client_tile(cs, keptX, keptY));

    /* A poll with nothing captured since the last one applies nothing and
     * leaves the client where it was. */
    clientSimNetTick(cs);
    UT_ASSERT_MSG(fq_client_tile(cs, keptX, keptY) == GRASS, "client holds %d",
                  (int)fq_client_tile(cs, keptX, keptY));

    fq_teardown(sim, cs);
    return 0;
}

int run_sp_local_map_repair(void) {
    ServerSim *sim = NULL;
    ClientSim *cs = NULL;
    GameSim *gs;
    GameSim *cg;
    BYTE slot;
    BYTE staleX, staleY, minedX, minedY, hiddenX, hiddenY;
    int i;

    UT_ASSERT(fq_setup(&sim, &cs));
    fq_set_queue(cs, true);
    gs = serverSimGetGameSim(sim);
    cg = clientSimGetGameSim(cs);
    slot = clientSimGetMyPlayerNum(cs);
    UT_ASSERT(slot < MAX_TANKS);

    UT_ASSERT(fq_find_forest(sim, cs, 0, &staleX, &staleY));
    UT_ASSERT(fq_find_forest(sim, cs, 40, &minedX, &minedY));
    UT_ASSERT(fq_find_forest(sim, cs, 80, &hiddenX, &hiddenY));

    threadsWaitForMutex();
    /* A tree the server cut down without the client hearing of it: the
     * server's map and this slot's copy say grass, the client a tree. */
    (*gs->mp).mapItem[staleX][staleY] = GRASS;
    sim->clientKnownMapObj[slot].mapItem[staleX][staleY] = GRASS;
    /* The same, under a mine the client knows of. */
    (*gs->mp).mapItem[minedX][minedY] = MINE_GRASS;
    sim->clientKnownMapObj[slot].mapItem[minedX][minedY] = MINE_GRASS;
    (*cg->mp).mapItem[minedX][minedY] = MINE_FOREST;
    /* A mine the client was never told of, on ground it has right. */
    (*gs->mp).mapItem[hiddenX][hiddenY] = MINE_FOREST;
    sim->clientKnownMapObj[slot].mapItem[hiddenX][hiddenY] = MINE_FOREST;
    threadsReleaseMutex();

    for (i = 0; i < FQ_FULL_SYNC_FRAMES; i++) {
        fq_server_frame(sim, cs);
        clientSimNetTick(cs);
        if (fq_client_tile(cs, staleX, staleY) == GRASS) break;
    }
    fprintf(stderr, "  repaired after %d frame(s)\n", i + 1);

    UT_ASSERT_MSG(fq_client_tile(cs, staleX, staleY) == GRASS,
                  "the stale tree was not repaired within %d frames",
                  FQ_FULL_SYNC_FRAMES);
    UT_ASSERT_MSG(fq_client_tile(cs, minedX, minedY) == MINE_GRASS, "client holds %d",
                  (int)fq_client_tile(cs, minedX, minedY));
    UT_ASSERT_MSG(fq_client_tile(cs, hiddenX, hiddenY) == FOREST, "client holds %d",
                  (int)fq_client_tile(cs, hiddenX, hiddenY));

    fq_teardown(sim, cs);
    return 0;
}

int run_sp_frame_queue_resets_on_lobby(void) {
    ServerSim *sim = NULL;
    ClientSim *cs = NULL;
    ControlEvent evt;

    UT_ASSERT(fq_setup(&sim, &cs));
    fq_set_queue(cs, true);
    UT_ASSERT(cs->clientState.hasPredictedTank);
    /* Leave a tank-bearing frame waiting when the synchronous lobby
     * control events reset the client. */
    fq_server_frame(sim, cs);
    threadsWaitForMutex();
    /* Isolate the phase event so a preceding map-change invalidation
     * cannot hide a missing invalidation on lobby entry itself. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_LOBBY;
    serverSimPublishControl(sim, &evt);
    threadsReleaseMutex();
    UT_ASSERT(clientSimIsInLobby(cs));
    UT_ASSERT(!cs->clientState.hasPredictedTank);
    clientSimNetTick(cs);
    UT_ASSERT_MSG(!cs->clientState.hasPredictedTank,
                  "a queued old-round tank undid the lobby reset");

    /* Exercise the complete server transition too, including a lobby
     * snapshot captured after the world was reset. */
    fq_server_frame(sim, cs);
    threadsWaitForMutex();
    serverSimSetLobbyEnabled(sim, true);
    serverSimReturnToLobby(sim);
    serverSimLocalOnReturnToLobby(sim);
    threadsReleaseMutex();
    UT_ASSERT(clientSimIsInLobby(cs));
    UT_ASSERT(!cs->clientState.hasPredictedTank);

    fq_server_frame(sim, cs);
    clientSimNetTick(cs);
    UT_ASSERT_MSG(!cs->clientState.hasPredictedTank,
                  "a queued old-round tank undid the lobby reset");

    /* Starting again must still deliver the first tank and subsequent
     * map events through the same queue. */
    threadsWaitForMutex();
    serverSimStartGame(sim);
    serverSimLocalOnGameStart(sim);
    threadsReleaseMutex();
    UT_ASSERT(!clientSimIsInLobby(cs));
    fq_server_frame(sim, cs);
    clientSimNetTick(cs);
    UT_ASSERT(cs->clientState.hasPredictedTank);
    UT_ASSERT(fq_find_forest(sim, cs, 0, &fqX, &fqY));
    fqArmed = true;
    fq_server_frame(sim, cs);
    fq_server_frame(sim, cs);
    clientSimNetTick(cs);
    UT_ASSERT(fq_client_tile(cs, fqX, fqY) == GRASS);

    fq_teardown(sim, cs);
    return 0;
}

int run_sp_frame_queue_resets_on_map_change(void) {
    ServerSim *sim = NULL;
    ClientSim *cs = NULL;
    ControlEvent evt;

    UT_ASSERT(fq_setup(&sim, &cs));
    fq_set_queue(cs, true);
    UT_ASSERT(fq_find_forest(sim, cs, 0, &fqX, &fqY));
    fqArmed = true;
    fq_server_frame(sim, cs);
    UT_ASSERT((*serverSimGetGameSim(sim)->mp).mapItem[fqX][fqY] == GRASS);

    /* Reinstall the pristine map without a lobby-phase event: map
     * replacement must invalidate the queue in its own right. */
    threadsWaitForMutex();
    serverSimResetGameWorld(sim);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_MAP_CHANGE;
    serverSimPublishControl(sim, &evt);
    threadsReleaseMutex();
    UT_ASSERT(fq_client_tile(cs, fqX, fqY) == FOREST);
    clientSimNetTick(cs);
    UT_ASSERT_MSG(fq_client_tile(cs, fqX, fqY) == FOREST,
                  "an old map event was applied to the replacement map");

    fq_teardown(sim, cs);
    return 0;
}
