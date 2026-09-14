/*
 * A scenario's changeover seen from a connected client, over the real
 * loopback transport.
 *
 * A wave fields a held seat and takes it off again, one seat per tick, while
 * a human sits in the round holding a pillbox. Neither half of that is the
 * human's business: the seat keeps its place in the roster, its team and its
 * alliance throughout, so nothing the human owns has any reason to change
 * hands. What this drives is that nothing does — the client's own pillbox
 * keeps its owner across the fielding and the unfielding, tick by tick.
 *
 * Tick by tick, and with the slot's full-sync clock asserted alongside, is
 * the point of doing it here rather than on the server alone. The periodic
 * base-and-pill sync rewrites every owner the client holds, so a case that
 * only compared the two ends could be passing on a sync having landed in the
 * middle and papered over a migration. The window is opened right after one
 * sync so the next is 250 ticks away, and the clock is checked on every pump
 * to prove none arrived.
 *
 * run_loopback_scenario_seat_keeps_pills
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"    /* clientSimGetConnectState */
#include "client_connect_state.h"
#include "game_sim.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lobbyPlayers, lastFullSyncTick, tick */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType / BrainPath */
#include "server_sim_scenario.h"
#include "pillbox.h"
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX 2000   /* join + map download                          */
#define SETTLE_MAX   200   /* a few running ticks after the download        */
#define OWNER_MAX    600   /* the client agreeing who owns the pillbox      */
#define SYNC_MAX     700   /* reaching the far side of a periodic full sync */
#define WATCH        40    /* pumps watched either side of the changeover   */

#define LS_SEAT  4         /* the seat the wave holds                       */
#define LS_TEAM  2         /* on a team of its own, not the human's         */
#define LS_PILL  1         /* the pillbox the human holds                   */
#define LS_NAME  "Raider1"

static char lsBrainPath[128];

static bool lsMakeBrainFile(void) {
    FILE *f;

    SDL_snprintf(lsBrainPath, sizeof(lsBrainPath),
                 "test_loopback_scenario_seat_brain.lua");
    f = fopen(lsBrainPath, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
}

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* True once the client agrees the pillbox belongs to the slot it is in. */
static bool pred_client_owns_pill(LoopbackHarness *h, void *user) {
    GameSim *cl = clientSimGetGameSim(h->cs);
    BYTE want = *(BYTE *)user;
    return cl != NULL && cl->pb != NULL &&
           pillsGetPillOwner(&cl->pb, LS_PILL) == want;
}

/* True just after a periodic full sync went out, which is where the watched
 * window opens: the next one is then a whole interval (250 ticks) away, and
 * the window is 80. */
static bool pred_just_synced(LoopbackHarness *h, void *user) {
    BYTE slot = *(BYTE *)user;
    return h->sim->lastFullSyncTick[slot] != 0 &&
           h->sim->tick - h->sim->lastFullSyncTick[slot] <= 3;
}

/* The client still holds the pillbox, and no full sync told it so. */
static int lsOwnerHolds(LoopbackHarness *h, BYTE slot, uint32_t syncAt,
                        const char *when) {
    GameSim *cl = clientSimGetGameSim(h->cs);

    UT_ASSERT(cl != NULL && cl->pb != NULL);
    UT_ASSERT_MSG(pillsGetPillOwner(&cl->pb, LS_PILL) == slot,
                  "%s: the client has pillbox %d owned by %u, expected its "
                  "own slot %u", when, LS_PILL,
                  (unsigned)pillsGetPillOwner(&cl->pb, LS_PILL),
                  (unsigned)slot);
    UT_ASSERT_MSG(h->sim->lastFullSyncTick[slot] == syncAt,
                  "%s: a full sync landed mid-changeover — the clock reads "
                  "%u and the window opened at %u", when,
                  (unsigned)h->sim->lastFullSyncTick[slot], (unsigned)syncAt);
    return 0;
}

int run_loopback_scenario_seat_keeps_pills(void) {
    LoopbackHarness h;
    ScenarioOp op;
    BYTE slot;
    uint32_t syncAt;
    const ClientLobbySlot *mirror;
    int at;
    int i;

    UT_ASSERT(lsMakeBrainFile());
    ut_brain_stub_arm(true);

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "PillHolder", false, NULL, 40711),
                  "loopback start failed");

    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    UT_ASSERT_MSG(at > 0, "the client never connected");
    loopbackHarnessPumpUntil(&h, SETTLE_MAX, NULL, NULL);
    slot = clientSimGetMyPlayerNum(h.cs);
    UT_ASSERT_MSG(slot < MAX_TANKS, "the client holds no slot");

    /* Give the human a pillbox, and hold a seat for the wave on a team of
       its own. Under the server's mutex, as the harness's own start does:
       the server's tick runs on this thread, but its recv thread does not. */
    threadsWaitForMutex();
    serverSimSetBotAiType(h.sim, aiFull);
    serverSimSetBotBrainPath(h.sim, lsBrainPath);
    pillsSetPillOwner(&h.sim->sim, &h.sim->sim.pb, LS_PILL, slot, TRUE);
    if (!serverSimAddUnfieldedSeat(h.sim, LS_SEAT, LS_NAME, LS_TEAM)) {
        threadsReleaseMutex();
        loopbackHarnessStop(&h);
        UT_FAIL("the seat could not be held");
    }
    SDL_strlcpy(h.sim->seatBrain[LS_SEAT], lsBrainPath,
                sizeof(h.sim->seatBrain[LS_SEAT]));
    threadsReleaseMutex();

    at = loopbackHarnessPumpUntil(&h, OWNER_MAX, pred_client_owns_pill, &slot);
    if (at <= 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never agreed it owns pillbox %d", LS_PILL);
    }

    /* Open the window on the far side of a full sync. */
    at = loopbackHarnessPumpUntil(&h, SYNC_MAX, pred_just_synced, &slot);
    if (at <= 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("no periodic full sync went out in %d pumps", SYNC_MAX);
    }
    syncAt = h.sim->lastFullSyncTick[slot];

    /* The wave fields the seat. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ROSTER_SPAWN_BOT;
    op.u.rosterSpawnBot.slot  = LS_SEAT;
    op.u.rosterSpawnBot.start = SCN_NONE;
    if (serverSimApplyScenarioOp(h.sim, &op, NULL) != SCN_OP_QUEUED) {
        loopbackHarnessStop(&h);
        UT_FAIL("the spawn was refused on the server");
    }
    for (i = 0; i < WATCH; i++) {
        loopbackHarnessPump(&h);
        if (lsOwnerHolds(&h, slot, syncAt, "while the seat is fielded") != 0) {
            loopbackHarnessStop(&h);
            return 1;
        }
    }
    if (!h.sim->lobbyPlayers[LS_SEAT].fielded) {
        loopbackHarnessStop(&h);
        UT_FAIL("the spawn never fielded the seat");
    }

    /* And takes it off again, which is the half that used to be a departure
       and an arrival. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ROSTER_REMOVE_BOT;
    op.u.rosterRemoveBot.slot = LS_SEAT;
    if (serverSimApplyScenarioOp(h.sim, &op, NULL) != SCN_OP_QUEUED) {
        loopbackHarnessStop(&h);
        UT_FAIL("the removal was refused on the server");
    }
    for (i = 0; i < WATCH; i++) {
        loopbackHarnessPump(&h);
        if (lsOwnerHolds(&h, slot, syncAt,
                         "while the seat is taken off") != 0) {
            loopbackHarnessStop(&h);
            return 1;
        }
    }

    if (!h.sim->playerConnected[LS_SEAT] ||
        h.sim->lobbyPlayers[LS_SEAT].fielded) {
        loopbackHarnessStop(&h);
        UT_FAIL("the seat is connected=%d fielded=%d, expected a held seat",
                (int)h.sim->playerConnected[LS_SEAT],
                (int)h.sim->lobbyPlayers[LS_SEAT].fielded);
    }

    /* The one event that did go out reached the client: its roster still has
       the seat, and has it off the field. */
    mirror = clientSimGetLobbySlot(h.cs, LS_SEAT);
    if (mirror == NULL || !mirror->connected || mirror->fielded) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client's roster has the seat connected=%d fielded=%d",
                mirror ? (int)mirror->connected : -1,
                mirror ? (int)mirror->fielded : -1);
    }

    loopbackHarnessStop(&h);
    remove(lsBrainPath);
    return 0;
}
