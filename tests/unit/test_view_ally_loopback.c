/*
 * Reaching an ally the client has never been sent, over the real loopback
 * transport.
 *
 * Under viewPolicyKey the server sends an ally's snapshots only once the
 * client says it is watching them. So the client's own record of who is
 * watchable — clientSimAllyViewMask, which is the interpolation context's
 * "I have had a real record for this slot and it is alive" — never has that
 * ally's bit. While the client picked its own ally to watch, that mask was the
 * entry test, which made the one case the policy exists to serve (watching an
 * ally you cannot currently see) the one case it refused: which allies you
 * could reach was decided by who had happened to drive past. The server picks
 * now, from live state, and the client applies the answer.
 *
 * The case parks an allied in-process player 40+ squares away and outside
 * every rect the wire client has, confirms the client really is holding no
 * data for it, and then presses the ally key:
 *
 *   - arm 1: the ally is reached, and its records then start arriving, which
 *     is the server granting the rect the request asked for. That second half
 *     also shows the view surviving the per-tick upkeep rather than closing on
 *     the tick after it opened;
 *   - arm 2: the same press with the ally in its death wait reaches nobody and
 *     leaves the camera on the tank.
 *
 * Both arms would have failed against the client-side choice this replaced:
 * arm 1 because the sticky mask never had the ally's bit, arm 2 because
 * nothing stopped a dead ally being chosen.
 *
 * Per the harness contract the assertions are "converges within N pumps",
 * never a packet trace.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "bolo_map.h"
#include "game_sim.h"
#include "tank.h"        /* tankSetWorld, tankGetDeathWait, TANK_DEATH_WAIT */
#include "bases.h"       /* basesExistPos — keep the ally off a base square */
#include "pillbox.h"     /* pill positions — and out of any pill's reach */
#include "players.h"     /* playersIsAllie — the client's own view of it */
#include "view_policy.h"
#include "client_command.h"       /* VIEW_KIND_TANK / VIEW_KIND_ALLY */
#include "server_sim.h"
#include "server_sim_internal.h"  /* serverSimBuildViewports, ViewportRect, viewKind */
#include "client_sim.h"
#include "client_connect_state.h"
#include "client_net.h"          /* clientSimGetConnectState */
#include "client_sim_internal.h"  /* clientSimAllyViewMask */
#include "transport_udp.h"        /* the server's download-complete test hook */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Convergence ceilings on a clean path. Bounds to report a failure against,
 * not expected counts. */
#define ALLY_CONNECT_MAX 4000
#define ALLY_READY_MAX   4000

/* Snapshots have to have been flowing for a while before "the client holds no
 * data for the ally" says anything: an ally that was being sent would have
 * turned up in the mask many times over by here. The same wait is what carries
 * the alliance to the client. */
#define ALLY_SETTLE_PUMPS 200

/* Entering the view is a round trip: the request out, the server's answer
 * back. */
#define ALLY_VIEW_MAX 400

/* The first real record for the ally is another one, once the server has
 * started granting the rect. */
#define ALLY_STREAM_MAX 400

/* Returning to the tank view is reported on the next display tick and taken on
 * the server's next command drain. */
#define ALLY_TANK_VIEW_MAX 400

/* How long the dead-ally press is watched for a camera move that must never
 * come — several round trips at loopback timing. */
#define ALLY_DEAD_PUMPS 200

/* Nothing on a clean path draws from the impairment stream; the seed keeps the
 * run reproducible regardless. */
#define ALLY_SEED 0xA11EEDu

/* How far off the watcher's tank the ally is parked, in map squares. Well past
 * the 27-square half-width of a viewport rect. */
#define ALLY_FAR_MIN 40

/* And how far off any pillbox. A neutral pill shoots anything within
 * PILLBOX_RANGE, which is 8 squares; an ally killed by one part-way through
 * would fail arm 1 for a reason that has nothing to do with the view. */
#define ALLY_PILL_CLEAR 10

static bool allyConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* The server flips its own download-complete flag only once the map's bytes
 * are acked back on CHANNEL_BULK, a round-trip after the client says
 * CONNECTED. Snapshots have been flowing for a while by then, so the tank
 * position read afterwards is one the wire delivered. */
static bool allyServerReady(LoopbackHarness *h, void *user) {
    (void)user;
    return transportUdpServerTestDownloadComplete(
        (int)clientSimGetMyPlayerNum(h->cs));
}

static bool allyHaveTank(LoopbackHarness *h, void *user) {
    BYTE mx = 0, my = 0;
    (void)user;
    return clientSimGetMyTankMapPos(h->cs, &mx, &my) == TRUE;
}

/* One pump plus the client's display tick. The pump alone moves packets; the
 * display tick is what runs the item-view upkeep, advances the view clocks and
 * reports the current view to the server, so a case that depends on any of the
 * three has to drive it — loopbackHarnessPump does not. */
static void allyPumpTick(LoopbackHarness *h) {
    loopbackHarnessPump(h);
    clientSimDisplayTick(h->cs, false);
}

/* Does the client believe it is holding a live record for that slot? This is
 * the mask the client used to choose its own ally from. */
static bool allyMaskHas(const ClientSim *cs, BYTE slot) {
    return (clientSimAllyViewMask(cs) & ((PlayerBitMap)1 << slot)) != 0;
}

/* The world centre of a map square. */
static WORLD allyWorld(BYTE mapCoord) {
    return (WORLD)(((int)mapCoord << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
}

/* A square a tank can be parked on and left: plain land — no water to drown
 * in, no building — and off any base square. */
static bool allyIsUsableGround(GameSim *gs, int x, int y) {
    BYTE terrain = (*gs->mp).mapItem[x][y];
    if (terrain != SWAMP && terrain != CRATER && terrain != ROAD &&
        terrain != FOREST && terrain != RUBBLE && terrain != GRASS) {
        return false;
    }
    return !basesExistPos(&gs->bs, (BYTE)x, (BYTE)y);
}

/* Out of every pillbox's reach, so nothing shoots the parked ally. */
static bool allyClearOfPills(GameSim *gs, int x, int y) {
    BYTE np = pillsGetNumPills(&gs->pb);
    BYTE p;

    for (p = 0; p < np; p++) {
        int dx = x - (int)gs->pb->item[p].x;
        int dy = y - (int)gs->pb->item[p].y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx < ALLY_PILL_CLEAR && dy < ALLY_PILL_CLEAR) return false;
    }
    return true;
}

/* Somewhere to park the ally: usable ground well away from the watcher's tank,
 * outside every rect the watcher's viewports give it, and out of pill range.
 * The whole map is scanned rather than a block at a fixed offset from the tank:
 * the island is not square, so an offset lands in open sea as often as not. */
static bool allyFindHiddenGround(GameSim *gs, const ViewportRect *vps, int nVps,
                                 BYTE tankMX, BYTE tankMY, BYTE *outX,
                                 BYTE *outY) {
    int x, y;

    for (x = 1; x < MAP_ARRAY_SIZE - 1; x++) {
        for (y = 1; y < MAP_ARRAY_SIZE - 1; y++) {
            int dx = x - (int)tankMX;
            int dy = y - (int)tankMY;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            /* Chebyshev distance: far enough that no rect of a screen's
             * half-width reaches it from the tank. */
            if (dx < ALLY_FAR_MIN && dy < ALLY_FAR_MIN) continue;
            if (!allyIsUsableGround(gs, x, y)) continue;
            if (!allyClearOfPills(gs, x, y)) continue;
            if (inAnyViewport(vps, nVps, x, y)) continue;
            *outX = (BYTE)x;
            *outY = (BYTE)y;
            return true;
        }
    }
    return false;
}

int run_view_ally_loopback(void) {
    LoopbackHarness h;
    ViewportRect vps[MAX_VIEWPORTS];
    GameSim *gs;         /* the server's world */
    GameSim *clientGs;   /* and the watcher's own copy of who is who */
    BYTE slotWatcher, slotAlly;
    BYTE tankMX = 0, tankMY = 0;
    BYTE allyX = 0, allyY = 0;
    int n, i, at;

    if (loopbackHarnessStart(&h, "Watcher", /*lobbyMode*/ false,
                             /*impairSpec*/ NULL, ALLY_SEED) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (ally view) failed");
    }
    if (loopbackHarnessPumpUntil(&h, ALLY_CONNECT_MAX, allyConnected, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED within %d pumps",
                ALLY_CONNECT_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, ALLY_READY_MAX, allyServerReady, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server never saw the map download acked within %d pumps",
                ALLY_READY_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, ALLY_READY_MAX, allyHaveTank, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank within %d pumps", ALLY_READY_MAX);
    }

    gs = serverSimGetGameSim(h.sim);
    clientGs = clientSimGetGameSim(h.cs);
    slotWatcher = clientSimGetMyPlayerNum(h.cs);
    if (gs == NULL || clientGs == NULL || slotWatcher >= MAX_TANKS ||
        clientSimGetMyTankMapPos(h.cs, &tankMX, &tankMY) != TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("no sim, slot or tank position after convergence");
    }

    /* The ally is in-process: a slot with a tank and no wire client behind
     * it. Which ally the watcher may reach is the question; who is driving it
     * is not. */
    slotAlly = (BYTE)(slotWatcher == 0 ? 1 : 0);
    threadsWaitForMutex();
    serverSimAddPlayer(h.sim, slotAlly, "Ally", false);
    /* serverSimAcceptAlliance is playersAcceptAlliance plus the matching
     * CTRL_ALLIANCE_ACCEPT publish. The publish is not decoration here: the
     * client's item-view upkeep asks its own players table whether the target
     * is still an ally, so with only the server-side half the view would close
     * the moment the ally's first record arrived. */
    serverSimAcceptAlliance(h.sim, slotWatcher, slotAlly);
    serverSimSetViewPolicy(h.sim, viewCategoryAlly, viewPolicyKey,
                           VIEW_DECAY_DEFAULT_SECS);
    /* With both of these off, no pill or base rect can cover the ally's square
     * by accident and hand the case its answer for the wrong reason. */
    serverSimSetViewPolicy(h.sim, viewCategoryPill, viewPolicyOff,
                           VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(h.sim, viewCategoryBase, viewPolicyOff,
                           VIEW_DECAY_DEFAULT_SECS);
    threadsReleaseMutex();

    if (gs->tanks[slotAlly] == NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot %u has no tank after being added", (unsigned)slotAlly);
    }

    /* Park the ally where the watcher cannot see it. No pump has run since the
     * slot was added, so no snapshot has ever carried it at its start square
     * either. */
    n = serverSimBuildViewports(h.sim, slotWatcher, vps, MAX_VIEWPORTS);
    if (!allyFindHiddenGround(gs, vps, n, tankMX, tankMY, &allyX, &allyY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("no usable ground at least %d squares off the tank at %u,%u, "
                "outside its %d rects and clear of every pill", ALLY_FAR_MIN,
                (unsigned)tankMX, (unsigned)tankMY, n);
    }
    threadsWaitForMutex();
    tankSetWorld(gs, &gs->tanks[slotAlly], allyWorld(allyX), allyWorld(allyY),
                 0, false);
    threadsReleaseMutex();

    /* Let the world run for a while: long enough for the alliance to reach the
     * client, and long enough that an ally being sent would be unmistakable in
     * the mask read below. */
    for (i = 0; i < ALLY_SETTLE_PUMPS; i++) {
        allyPumpTick(&h);
    }
    if (playersIsAllie(&clientGs->plyrs, slotWatcher, slotAlly) != TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never learned it is allied to slot %u within %d "
                "pumps", (unsigned)slotAlly, ALLY_SETTLE_PUMPS);
    }
    /* Everything below is driven by the display tick, and clientUiOnTick does
     * nothing at all once the client has stopped running — which is also what
     * a game length that never arrived looks like. Say so here rather than
     * leaving a later wait to time out with no explanation. */
    if (clientSimIsRunning(h.cs) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client stopped running during the settle pumps, so no "
                "display tick will report a view or run the item-view upkeep");
    }

    /* ---- The two preconditions the case rests on ------------------------ */
    /* The rects the snapshot build actually runs against, so this cannot
     * quietly become "the ally was visible all along". */
    n = serverSimBuildViewports(h.sim, slotWatcher, vps, MAX_VIEWPORTS);
    if (inAnyViewport(vps, n, allyX, allyY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the ally's square %u,%u is inside the watcher's %d viewport "
                "rects", (unsigned)allyX, (unsigned)allyY, n);
    }
    /* And the client holds no data for the ally: its bit is absent from the
     * mask the client used to choose from. That absence is the whole point —
     * an ally in this state was unreachable, and it is the state viewPolicyKey
     * deliberately keeps every ally in until one is asked for. */
    if (allyMaskHas(h.cs, slotAlly)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client already holds live data for slot %u, so the case "
                "cannot show anything about an ally it has never been sent",
                (unsigned)slotAlly);
    }

    /* ---- Arm 1: the ally is reachable ----------------------------------- */
    clientSimAllyView(h.cs, 0, 0);
    at = -1;
    for (i = 1; i <= ALLY_VIEW_MAX; i++) {
        allyPumpTick(&h);
        if (clientSimGetViewKind(h.cs) == VIEW_KIND_ALLY) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the ally key never reached slot %u within %d pumps — the "
                "client stayed on view kind %u", (unsigned)slotAlly,
                ALLY_VIEW_MAX, (unsigned)clientSimGetViewKind(h.cs));
    }
    if (clientSimGetViewTarget(h.cs) != slotAlly) {
        loopbackHarnessStop(&h);
        UT_FAIL("the ally view landed on slot %u, expected %u",
                (unsigned)clientSimGetViewTarget(h.cs), (unsigned)slotAlly);
    }

    /* Reaching the ally is half of it; the server has to start sending them
     * too, which is the rect the request asked for. */
    for (i = 0; i < ALLY_STREAM_MAX; i++) {
        allyPumpTick(&h);
        if (allyMaskHas(h.cs, slotAlly)) break;
    }
    if (!allyMaskHas(h.cs, slotAlly)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the watcher is in the ally view but no record for slot %u "
                "arrived within %d pumps — the server never granted its rect",
                (unsigned)slotAlly, ALLY_STREAM_MAX);
    }
    /* And the view is still open, so the per-tick upkeep kept it once the ally
     * stopped being a stub rather than dropping it. */
    if (clientSimGetViewKind(h.cs) != VIEW_KIND_ALLY ||
        clientSimGetViewTarget(h.cs) != slotAlly) {
        loopbackHarnessStop(&h);
        UT_FAIL("the view closed as slot %u started arriving: kind %u target %u",
                (unsigned)slotAlly, (unsigned)clientSimGetViewKind(h.cs),
                (unsigned)clientSimGetViewTarget(h.cs));
    }

    /* ---- Arm 2: a dead ally is not offered ------------------------------ */
    clientSimTankView(h.cs);
    at = -1;
    for (i = 1; i <= ALLY_TANK_VIEW_MAX; i++) {
        allyPumpTick(&h);
        if (h.sim->viewKind[slotWatcher] == VIEW_KIND_TANK) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server never saw the watcher back in the tank view within "
                "%d pumps — it still holds kind %u", ALLY_TANK_VIEW_MAX,
                (unsigned)h.sim->viewKind[slotWatcher]);
    }

    /* The full death wait rather than a few ticks: the ally has to stay dead
     * for the whole window below, and sim time really passes over a pump
     * loop. */
    threadsWaitForMutex();
    gs->tanks[slotAlly]->deathWait = TANK_DEATH_WAIT;
    threadsReleaseMutex();

    clientSimAllyView(h.cs, 0, 0);
    for (i = 1; i <= ALLY_DEAD_PUMPS; i++) {
        allyPumpTick(&h);
        if (clientSimGetViewKind(h.cs) != VIEW_KIND_TANK) {
            loopbackHarnessStop(&h);
            UT_FAIL("a dead ally was offered on pump %d: the camera moved to "
                    "view kind %u target %u", i,
                    (unsigned)clientSimGetViewKind(h.cs),
                    (unsigned)clientSimGetViewTarget(h.cs));
        }
    }
    /* The ally was dead for all of it, so staying put was a refusal and not a
     * respawn arriving before the request did. */
    if (tankGetDeathWait(&gs->tanks[slotAlly]) == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot %u finished its death wait during the %d pumps of the "
                "dead-ally press, so nothing was proved by the camera staying "
                "put", (unsigned)slotAlly, ALLY_DEAD_PUMPS);
    }

    loopbackHarnessStop(&h);
    return 0;
}
