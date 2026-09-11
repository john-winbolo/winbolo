/*
 * A pillbox taken off the map and put back, over the real loopback transport.
 *
 * The entity ops are the only thing that changes how long a client's item
 * lists are mid-round: the periodic full sync writes a record per index but
 * never the count or the live flags, and the compressed blob is only handed
 * out at a join. So the CTRL_ENTITY_CHANGE the arm publishes is the whole of
 * what a connected client has to go on, and this drives it end to end —
 * server-side op, real sockets, the client's own pill list — rather than
 * asserting on the event the server sent.
 *
 * What is compared is the count and the per-index live flags, plus the owner
 * and armour of the pillbox that came back. The square is deliberately left
 * out: a pillbox outside the recipient's viewport has its square withheld and
 * reported as the one that client was last given, so x and y legitimately
 * differ between the two sides for anything it cannot see.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_connect_state.h"
#include "game_sim.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_scenario.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "mines.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX 2000   /* join + map download                        */
#define SETTLE_MAX   200   /* a few running ticks after the download      */
#define CHANGE_MAX   600   /* the op's event crossing to the client       */

/* The pillbox the case takes away and puts back. */
#define EC_PILL_INDEX 5

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* True once the client agrees the pillbox is off the map. */
static bool pred_pill_removed(LoopbackHarness *h, void *user) {
    GameSim *gs = clientSimGetGameSim(h->cs);
    (void)user;
    return gs != NULL && gs->pb != NULL &&
           pillsIsActive(&gs->pb, EC_PILL_INDEX + 1) == FALSE;
}

/* And once it agrees it is back. */
static bool pred_pill_back(LoopbackHarness *h, void *user) {
    GameSim *gs = clientSimGetGameSim(h->cs);
    (void)user;
    return gs != NULL && gs->pb != NULL &&
           pillsIsActive(&gs->pb, EC_PILL_INDEX + 1) == TRUE;
}

/* An empty land square the add can use, read off the server's own world. */
static bool ecFindLand(ServerSim *sim, BYTE *ox, BYTE *oy) {
    GameSim *gs = &sim->sim;
    int x, y;
    for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
            BYTE t = mapGetPos(&gs->mp, (BYTE)x, (BYTE)y);
            if (t != GRASS && t != ROAD && t != SWAMP && t != RUBBLE &&
                t != CRATER && t != FOREST) {
                continue;
            }
            if (pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y)) continue;
            if (basesExistPos(&gs->bs, (BYTE)x, (BYTE)y)) continue;
            if (minesExistPos(&gs->mns, &gs->mp, (BYTE)x, (BYTE)y)) continue;
            *ox = (BYTE)x;
            *oy = (BYTE)y;
            return true;
        }
    }
    return false;
}

/* Both lists name the same pillboxes, live and removed. */
static int ecListsAgree(ServerSim *sim, ClientSim *cs, const char *when) {
    GameSim *sv = &sim->sim;
    GameSim *cl = clientSimGetGameSim(cs);
    BYTE n;
    BYTE i;

    UT_ASSERT(cl != NULL && cl->pb != NULL);
    n = pillsGetNumPills(&sv->pb);
    UT_ASSERT_MSG(pillsGetNumPills(&cl->pb) == n,
                  "%s: the client holds %u pillboxes and the server %u",
                  when, (unsigned)pillsGetNumPills(&cl->pb), (unsigned)n);
    for (i = 1; i <= n; i++) {
        UT_ASSERT_MSG(pillsIsActive(&cl->pb, i) == pillsIsActive(&sv->pb, i),
                      "%s: pillbox %u is %s on the server and %s on the client",
                      when, (unsigned)i,
                      pillsIsActive(&sv->pb, i) ? "on the map" : "removed",
                      pillsIsActive(&cl->pb, i) ? "on the map" : "removed");
    }
    return 0;
}

int run_loopback_entity_change(void) {
    LoopbackHarness h;
    ScenarioOp op;
    ScnOpOut out;
    GameSim *cl;
    BYTE lx = 0, ly = 0;
    BYTE serverCount;
    int at;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "EntityWatcher", false, NULL, 90210),
                  "loopback start failed");

    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    UT_ASSERT_MSG(at > 0, "the client never connected");
    loopbackHarnessPumpUntil(&h, SETTLE_MAX, NULL, NULL);

    serverCount = pillsGetNumPills(&h.sim->sim.pb);
    UT_ASSERT_MSG(serverCount > EC_PILL_INDEX,
                  "the map carries %u pillboxes", (unsigned)serverCount);
    if (ecListsAgree(h.sim, h.cs, "after the join") != 0) {
        loopbackHarnessStop(&h);
        return 1;
    }

    /* Take it off the map. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_REMOVE_PILL;
    op.u.entityRemovePill.pill = EC_PILL_INDEX;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(h.sim, &op, NULL) == SCN_OP_OK,
                  "the removal was refused on the server");

    at = loopbackHarnessPumpUntil(&h, CHANGE_MAX, pred_pill_removed, NULL);
    UT_ASSERT_MSG(at > 0,
                  "the client still has pillbox %u on the map after %d pumps",
                  (unsigned)(EC_PILL_INDEX + 1), CHANGE_MAX);
    if (ecListsAgree(h.sim, h.cs, "after the removal") != 0) {
        loopbackHarnessStop(&h);
        return 1;
    }

    /* And put one back. The list has exactly one removed slot, so the add
       lands in it rather than extending the list. */
    UT_ASSERT(ecFindLand(h.sim, &lx, &ly));
    memset(&op, 0, sizeof(op));
    memset(&out, 0, sizeof(out));
    out.index = 0xFF;
    op.type = SCN_OP_ENTITY_ADD_PILL;
    op.u.entityAddPill.x = lx;
    op.u.entityAddPill.y = ly;
    op.u.entityAddPill.owner = 0;
    op.u.entityAddPill.armour = 9;
    op.u.entityAddPill.speed = PILLBOX_ATTACK_NORMAL;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(h.sim, &op, &out) == SCN_OP_OK,
                  "the add was refused on the server");
    UT_ASSERT_MSG(out.index == EC_PILL_INDEX,
                  "the add took index %u, expected the freed slot %u",
                  (unsigned)out.index, (unsigned)EC_PILL_INDEX);

    at = loopbackHarnessPumpUntil(&h, CHANGE_MAX, pred_pill_back, NULL);
    UT_ASSERT_MSG(at > 0,
                  "the client never put pillbox %u back after %d pumps",
                  (unsigned)(EC_PILL_INDEX + 1), CHANGE_MAX);
    if (ecListsAgree(h.sim, h.cs, "after the add") != 0) {
        loopbackHarnessStop(&h);
        return 1;
    }

    /* The record the event carried reached the client's own list. Owner and
       armour are public on every path, so they are the two the client must
       agree on whether or not it can see the square. */
    cl = clientSimGetGameSim(h.cs);
    {
        pillbox got;
        memset(&got, 0, sizeof(got));
        pillsGetPill(&cl->pb, &got, EC_PILL_INDEX + 1);
        UT_ASSERT_MSG(got.owner == 0,
                      "the client has the re-added pillbox owned by %u",
                      (unsigned)got.owner);
        UT_ASSERT_MSG(got.armour == 9,
                      "the client has the re-added pillbox at armour %u",
                      (unsigned)got.armour);
    }

    loopbackHarnessStop(&h);
    return 0;
}
