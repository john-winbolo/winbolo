/*
 * Map-event culling by viewport, over the real loopback transport.
 *
 * The server sends a wire client only the terrain changes inside that client's
 * viewports, and keeps a copy of what it sent (the slot's clientKnownMap). The
 * two halves have to stay welded together or a client desyncs and resync-loops:
 *
 *   - a change the client cannot see is not queued AND not written into its
 *     copy, so the copy stays the record of what it was really given;
 *   - the checksum in its snapshot header and the blob a resync hands it both
 *     come off that copy, so a client that is behind on ground it cannot see
 *     never sees a divergence and never asks for a resync;
 *   - once it can see the ground again, the catch-up sweep queues the
 *     difference and the client converges on the live map.
 *
 * This case drives all three over real sockets: a wire client B, an in-process
 * player A parked far away, a terrain change beside A, and then B driving over
 * to it. A is there for two reasons — it is what makes the change "somewhere
 * else" rather than "nowhere", and an in-process slot is exempt from culling,
 * so its copy must take the change B's does not.
 *
 * Changes are staged the way a running frame produces them (the map-change
 * callback armed around a mapSetPos, then the frame applied to the copies the
 * tick owns) and handed to the real transportUdpServerDrainEvents, so the cull,
 * the queue and the copy write are the shipping ones. Per the harness contract
 * the assertions are "converges within N pumps", never a packet trace.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "bolo_map.h"
#include "game_sim.h"
#include "tank.h"
#include "bases.h"    /* basesExistPos — keep the change off a base square */
#include "pillbox.h"  /* pillsExistPos — and off a pillbox square */
#include "server_sim.h"
#include "server_sim_internal.h"          /* clientKnownMap, ViewportRect, the shadow API */
#include "server/sim/server_sim_shared.h" /* serverSimSetActive, simMapChangeCallback */
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "client_sim_internal.h"          /* ClientSim::transport — the client test hooks */
#include "transport_udp.h"                /* the server drain + its map-queue test hooks */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Convergence ceilings on a clean path. Bounds to report a failure against,
 * not expected counts. */
#define CULL_CONNECT_MAX 4000
#define CULL_READY_MAX   4000
#define CULL_MAP_MAX     4000

/* Long enough to cross several FULL_SYNC_INTERVAL boundaries (250 sim ticks,
 * which is 125 pumps), so the client has compared its checksum against the
 * server's often enough to have tripped the resync debounce if the copy and
 * the client's map had drifted apart. */
#define CULL_QUIET_PUMPS 600

/* Nothing on a clean path draws from the impairment stream; the seed keeps the
 * run reproducible regardless. */
#define CULL_SEED 0xC011EDu

/* How far off B's tank the hidden square has to be, in map squares. Well past
 * the 27-square half-width of a viewport rect, and further than a tank covers
 * in the time this case runs. */
#define CULL_FAR_MIN 60

static bool cullConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* The server flips its own download-complete flag only once the map's bytes are
 * acked back on CHANNEL_BULK, a round-trip after the client says CONNECTED.
 * The drain skips a still-downloading slot's sweep, so wait for it. */
static bool cullServerReady(LoopbackHarness *h, void *user) {
    (void)user;
    return transportUdpServerTestDownloadComplete(
        (int)clientSimGetMyPlayerNum(h->cs));
}

static bool cullHaveTank(LoopbackHarness *h, void *user) {
    BYTE mx = 0, my = 0;
    (void)user;
    return clientSimGetMyTankMapPos(h->cs, &mx, &my) == TRUE;
}

typedef struct {
    BYTE x;
    BYTE y;
    BYTE terrain;
} CullTerrainWait;

static bool cullTerrainApplied(LoopbackHarness *h, void *user) {
    const CullTerrainWait *w = (const CullTerrainWait *)user;
    return clientSimGetMapTerrain(h->cs, w->x, w->y) == w->terrain;
}

/* A square a terrain write can be read back off: plain land — no water, no
 * building, no mine — and clear of any base or pill, whose own square renders
 * from its owner rather than from the ground under it. */
static bool cullIsUsableGround(GameSim *gs, int x, int y) {
    BYTE terrain = (*gs->mp).mapItem[x][y];
    if (terrain != SWAMP && terrain != CRATER && terrain != ROAD &&
        terrain != FOREST && terrain != RUBBLE && terrain != GRASS) {
        return false;
    }
    if (basesExistPos(&gs->bs, (BYTE)x, (BYTE)y)) return false;
    if (pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y)) return false;
    return true;
}

/* First usable square scanning out from (cx,cy) within `half`, or false if the
 * area holds none. */
static bool cullFindGroundNear(GameSim *gs, int cx, int cy, int half, BYTE *outX,
                               BYTE *outY) {
    int r;
    for (r = 0; r <= half; r++) {
        int dx, dy;
        for (dx = -r; dx <= r; dx++) {
            for (dy = -r; dy <= r; dy++) {
                int x = cx + dx, y = cy + dy;
                if (x < 1 || y < 1 || x > MAP_ARRAY_SIZE - 2 ||
                    y > MAP_ARRAY_SIZE - 2) {
                    continue;
                }
                if (!cullIsUsableGround(gs, x, y)) continue;
                *outX = (BYTE)x;
                *outY = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* A usable square well away from the client's tank and outside every rect its
 * viewports give it — the ground a change must not reach it on. The whole map
 * is scanned rather than a block at a fixed offset from the tank: the island is
 * not square, so an offset lands in open sea as often as not. */
static bool cullFindHiddenGround(GameSim *gs, const ViewportRect *vps, int nVps,
                                 BYTE tankMX, BYTE tankMY, BYTE *outX,
                                 BYTE *outY) {
    int x, y;
    for (x = 1; x < MAP_ARRAY_SIZE - 1; x++) {
        for (y = 1; y < MAP_ARRAY_SIZE - 1; y++) {
            int dx = x - (int)tankMX;
            int dy = y - (int)tankMY;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            /* Chebyshev distance: far enough that the tank cannot drift onto
             * it, and far enough that no rect of a screen's half-width reaches
             * it either. */
            if (dx < CULL_FAR_MIN && dy < CULL_FAR_MIN) continue;
            if (!cullIsUsableGround(gs, x, y)) continue;
            if (inAnyViewport(vps, nVps, x, y)) continue;
            *outX = (BYTE)x;
            *outY = (BYTE)y;
            return true;
        }
    }
    return false;
}

/* A terrain the square is not already holding, so a write is always visible. */
static BYTE cullFlip(BYTE current) {
    return (BYTE)(current == CRATER ? ROAD : CRATER);
}

/* Stage one terrain change as a running frame produces it and run the real
 * drain over it: arm the change callback so mapSetPos records a map event,
 * move the server's map, apply the frame to the copies the tick owns, then let
 * the transport cull/queue/write. Returns false if the square was already
 * holding `terrain` (nothing to prove from it). */
static bool cullStageChange(LoopbackHarness *h, BYTE x, BYTE y, BYTE terrain) {
    GameSim *gs = serverSimGetGameSim(h->sim);
    BYTE before;

    threadsWaitForMutex();
    before = (*gs->mp).mapItem[x][y];
    serverSimSetActive(h->sim);
    h->sim->mapEventCount = 0;
    mapSetChangeCallback(simMapChangeCallback);
    mapSetPos(gs, &gs->mp, x, y, terrain, FALSE, FALSE);
    mapSetChangeCallback(NULL);
    serverSimShadowTick(h->sim);
    transportUdpServerDrainEvents(h->sim);
    /* The next running tick clears this itself; clearing it here keeps the
     * frame from being drained twice if one never runs. */
    h->sim->mapEventCount = 0;
    threadsReleaseMutex();

    return before != terrain;
}

/* The checksum serverSimBuildSnapshot stamps into a slot's header on a
 * full-sync tick — the value the client compares its own map against. */
static uint16_t cullHeaderChecksum(ServerSim *sim, BYTE slot) {
    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    memset(&hdr, 0, sizeof(hdr));
    threadsWaitForMutex();
    sim->lastFullSyncTick[slot] = 0;   /* force the full sync that carries it */
    serverSimBuildSnapshot(sim, slot, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    threadsReleaseMutex();
    return hdr.mapChecksum;
}

int run_loopback_map_cull(void) {
    LoopbackHarness h;
    static BYTE blobBefore[131072];
    static BYTE blobStale[131072];
    static BYTE blobLive[131072];
    CullTerrainWait wait;
    ViewportRect vps[MAX_VIEWPORTS];
    GameSim *gs;
    Transport *ct;
    BYTE slotB, slotA;
    BYTE tankMX = 0, tankMY = 0;
    BYTE farX = 0, farY = 0, nearX = 0, nearY = 0;
    BYTE farBefore, farLive, nearTerrain;
    uint32_t gen0 = 0, resync0 = 0, gen1 = 0, resync1 = 0;
    int blobBeforeLen, n, at;

    if (loopbackHarnessStart(&h, "Cull", /*lobbyMode*/ false,
                             /*impairSpec*/ NULL, CULL_SEED) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (map cull) failed");
    }
    if (loopbackHarnessPumpUntil(&h, CULL_CONNECT_MAX, cullConnected, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED within %d pumps",
                CULL_CONNECT_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, CULL_READY_MAX, cullServerReady, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server never saw the map download acked within %d pumps",
                CULL_READY_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, CULL_READY_MAX, cullHaveTank, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank within %d pumps", CULL_READY_MAX);
    }

    gs = serverSimGetGameSim(h.sim);
    ct = &h.cs->transport;
    slotB = clientSimGetMyPlayerNum(h.cs);
    if (gs == NULL || slotB >= MAX_TANKS ||
        clientSimGetMyTankMapPos(h.cs, &tankMX, &tankMY) != TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("no sim, slot or tank position after convergence");
    }

    /* The wire client is the transport's, so its copy is culled; the slot the
     * in-process player takes below is not. */
    if (!serverSimIsShadowCulled(h.sim, slotB)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the joined wire slot %u was not marked culled",
                (unsigned)slotB);
    }

    /* Player A, in-process: no wire client behind it, so the tick keeps
     * writing its copy however far away the change is. */
    slotA = (BYTE)(slotB == 0 ? 1 : 0);
    threadsWaitForMutex();
    serverSimAddPlayer(h.sim, slotA, "Ally", false);
    threadsReleaseMutex();
    if (serverSimIsShadowCulled(h.sim, slotA)) {
        loopbackHarnessStop(&h);
        UT_FAIL("in-process slot %u was marked culled", (unsigned)slotA);
    }

    /* A square several screens off B's tank and outside every rect it has, and
     * one right beside it. */
    n = serverSimBuildViewports(h.sim, slotB, vps, MAX_VIEWPORTS);
    if (!cullFindHiddenGround(gs, vps, n, tankMX, tankMY, &farX, &farY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("no usable ground at least %d squares off the tank at %u,%u and "
                "outside its %d rects", CULL_FAR_MIN, (unsigned)tankMX,
                (unsigned)tankMY, n);
    }
    if (!cullFindGroundNear(gs, tankMX, tankMY, 6, &nearX, &nearY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("no usable ground beside the tank at %u,%u",
                (unsigned)tankMX, (unsigned)tankMY);
    }

    /* Park A on the far square, so the change below happens under a player who
     * can see it and nowhere near B. */
    if (gs->tanks[slotA] != NULL) {
        threadsWaitForMutex();
        tankSetWorld(gs, &gs->tanks[slotA],
                     (WORLD)(((int)farX << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE),
                     (WORLD)(((int)farY << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE),
                     0, false);
        threadsReleaseMutex();
    }

    /* The rects the cull actually runs against, so the case cannot quietly
     * become "both squares were visible all along". */
    n = serverSimBuildViewports(h.sim, slotB, vps, MAX_VIEWPORTS);
    if (inAnyViewport(vps, n, farX, farY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the far square %u,%u is inside B's %d viewport rects",
                (unsigned)farX, (unsigned)farY, n);
    }
    if (!inAnyViewport(vps, n, nearX, nearY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the near square %u,%u is outside B's %d viewport rects",
                (unsigned)nearX, (unsigned)nearY, n);
    }

    farBefore = h.sim->clientKnownMapObj[slotB].mapItem[farX][farY];
    blobBeforeLen = serverSimGetCompressedMapFor(h.sim, slotB, blobBefore);
    if (blobBeforeLen <= 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("B's copy did not compress (%d)", blobBeforeLen);
    }
    transportUdpClientTestMapState(ct, &gen0, &resync0);

    /* ---- A change B cannot see ------------------------------------------ */
    farLive = cullFlip((*gs->mp).mapItem[farX][farY]);
    if (farBefore == farLive) {
        loopbackHarnessStop(&h);
        UT_FAIL("the far square reads the same before and after — the case "
                "proves nothing");
    }
    if (!cullStageChange(&h, farX, farY, farLive)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the far square %u,%u already held the terrain being written",
                (unsigned)farX, (unsigned)farY);
    }

    if (transportUdpServerTestMapQueueHasSquare(slotB, farX, farY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("B's map-event queue carries the far square %u,%u",
                (unsigned)farX, (unsigned)farY);
    }
    if (h.sim->clientKnownMapObj[slotB].mapItem[farX][farY] != farBefore) {
        loopbackHarnessStop(&h);
        UT_FAIL("B's copy took a change it was not sent at %u,%u",
                (unsigned)farX, (unsigned)farY);
    }
    if ((*gs->mp).mapItem[farX][farY] != farLive) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server's own map did not take the change at %u,%u",
                (unsigned)farX, (unsigned)farY);
    }
    if (h.sim->clientKnownMapObj[slotA].mapItem[farX][farY] != farLive) {
        loopbackHarnessStop(&h);
        UT_FAIL("the in-process slot %u missed the change at %u,%u",
                (unsigned)slotA, (unsigned)farX, (unsigned)farY);
    }

    /* The checksum B is sent is taken over B's copy, so it still describes the
     * map B holds — and no longer the live map. */
    {
        uint16_t hdrSum = cullHeaderChecksum(h.sim, slotB);
        uint16_t knownSum = mapCalcChecksum(&h.sim->clientKnownMap[slotB],
                                            &gs->bs, &gs->pb);
        uint16_t liveSum = mapCalcChecksum(&gs->mp, &gs->bs, &gs->pb);
        if (hdrSum != knownSum) {
            loopbackHarnessStop(&h);
            UT_FAIL("B's header checksum %04x is not its copy's %04x",
                    hdrSum, knownSum);
        }
        if (knownSum == liveSum) {
            loopbackHarnessStop(&h);
            UT_FAIL("B's copy and the live map still check the same (%04x) — "
                    "the change did not move the checksum, so this proves "
                    "nothing", liveSum);
        }
    }

    /* And the blob a resync would hand B is unchanged, while the live map's is
     * not — B's copy is the whole record of what B was given. */
    {
        int staleLen = serverSimGetCompressedMapFor(h.sim, slotB, blobStale);
        int liveLen = serverSimGetCompressedMap(h.sim, blobLive);
        if (staleLen != blobBeforeLen ||
            memcmp(blobStale, blobBefore, (size_t)staleLen) != 0) {
            loopbackHarnessStop(&h);
            UT_FAIL("B's blob moved with a change B was never sent");
        }
        if (liveLen == staleLen &&
            memcmp(blobLive, blobStale, (size_t)liveLen) == 0) {
            loopbackHarnessStop(&h);
            UT_FAIL("the live blob and B's blob are identical — the change did "
                    "not reach the live map");
        }
    }

    /* Left alone, B stays put: the change never arrives and, over enough full
     * syncs to trip the resync debounce several times over, B never asks for a
     * resync. */
    loopbackHarnessPumpUntil(&h, CULL_QUIET_PUMPS, NULL, NULL);
    if (clientSimGetMapTerrain(h.cs, farX, farY) != farBefore) {
        loopbackHarnessStop(&h);
        UT_FAIL("B's own map took the far change at %u,%u anyway",
                (unsigned)farX, (unsigned)farY);
    }
    transportUdpClientTestMapState(ct, &gen1, &resync1);
    if (resync1 != resync0) {
        loopbackHarnessStop(&h);
        UT_FAIL("B resynced %u time(s) over %d quiet pumps — its checksum and "
                "the server's disagree", (unsigned)(resync1 - resync0),
                CULL_QUIET_PUMPS);
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("B left CONNECTED (state %d) while sitting still",
                (int)clientSimGetConnectState(h.cs));
    }

    /* ---- A change B can see --------------------------------------------- */
    /* B's tank may have drifted, so re-check the near square is still in the
     * rects the cull will use. */
    n = serverSimBuildViewports(h.sim, slotB, vps, MAX_VIEWPORTS);
    if (!inAnyViewport(vps, n, nearX, nearY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the near square %u,%u drifted out of B's rects before the "
                "visible change", (unsigned)nearX, (unsigned)nearY);
    }
    nearTerrain = cullFlip((*gs->mp).mapItem[nearX][nearY]);
    if (!cullStageChange(&h, nearX, nearY, nearTerrain)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the near square %u,%u already held the terrain being written",
                (unsigned)nearX, (unsigned)nearY);
    }
    if (!transportUdpServerTestMapQueueHasSquare(slotB, nearX, nearY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("B's map-event queue is missing the visible square %u,%u",
                (unsigned)nearX, (unsigned)nearY);
    }
    if (h.sim->clientKnownMapObj[slotB].mapItem[nearX][nearY] != nearTerrain) {
        loopbackHarnessStop(&h);
        UT_FAIL("B's copy did not take the change it was sent at %u,%u",
                (unsigned)nearX, (unsigned)nearY);
    }
    wait.x = nearX;
    wait.y = nearY;
    wait.terrain = nearTerrain;
    at = loopbackHarnessPumpUntil(&h, CULL_MAP_MAX, cullTerrainApplied, &wait);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the visible change at %u,%u never reached B within %d pumps",
                (unsigned)nearX, (unsigned)nearY, CULL_MAP_MAX);
    }

    /* ---- B drives over to the far square -------------------------------- */
    threadsWaitForMutex();
    if (gs->tanks[slotB] != NULL) {
        tankSetWorld(gs, &gs->tanks[slotB],
                     (WORLD)(((int)farX << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE),
                     (WORLD)(((int)farY << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE),
                     0, false);
    }
    threadsReleaseMutex();

    wait.x = farX;
    wait.y = farY;
    wait.terrain = farLive;
    at = loopbackHarnessPumpUntil(&h, CULL_MAP_MAX, cullTerrainApplied, &wait);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the catch-up sweep never delivered %u,%u after B drove to it "
                "(%d pumps)", (unsigned)farX, (unsigned)farY, CULL_MAP_MAX);
    }
    if (h.sim->clientKnownMapObj[slotB].mapItem[farX][farY] != farLive) {
        loopbackHarnessStop(&h);
        UT_FAIL("the sweep sent %u,%u without writing B's copy",
                (unsigned)farX, (unsigned)farY);
    }
    if (!transportUdpServerTestMapQueueHasSquare(slotB, farX, farY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("B has the far square but its queue never carried it");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* The blob a resync hands a culled client is its copy, not the live map: the
 * ground it has never been shown comes back as it last knew it, so installing
 * the blob leaves it exactly where the checksum it is sent says it should be.
 * Separate from the case above because it has to run while B is still behind,
 * and because a resync round-trip is its own convergence to wait on.
 */
int run_loopback_map_cull_resync(void) {
    LoopbackHarness h;
    GameSim *gs;
    Transport *ct;
    BYTE slotB;
    BYTE tankMX = 0, tankMY = 0;
    BYTE farX = 0, farY = 0;
    BYTE farBefore, farLive;
    ViewportRect vps[MAX_VIEWPORTS];
    uint32_t gen0 = 0, resync0 = 0, gen1 = 0, resync1 = 0;
    int n, i;

    if (loopbackHarnessStart(&h, "CullRs", /*lobbyMode*/ false,
                             /*impairSpec*/ NULL, CULL_SEED) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (map cull resync) failed");
    }
    if (loopbackHarnessPumpUntil(&h, CULL_CONNECT_MAX, cullConnected, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED within %d pumps",
                CULL_CONNECT_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, CULL_READY_MAX, cullServerReady, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server never saw the map download acked within %d pumps",
                CULL_READY_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, CULL_READY_MAX, cullHaveTank, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank within %d pumps", CULL_READY_MAX);
    }

    gs = serverSimGetGameSim(h.sim);
    ct = &h.cs->transport;
    slotB = clientSimGetMyPlayerNum(h.cs);
    if (gs == NULL || slotB >= MAX_TANKS ||
        clientSimGetMyTankMapPos(h.cs, &tankMX, &tankMY) != TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("no sim, slot or tank position after convergence");
    }

    n = serverSimBuildViewports(h.sim, slotB, vps, MAX_VIEWPORTS);
    if (!cullFindHiddenGround(gs, vps, n, tankMX, tankMY, &farX, &farY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("no usable ground at least %d squares off the tank at %u,%u and "
                "outside its %d rects", CULL_FAR_MIN, (unsigned)tankMX,
                (unsigned)tankMY, n);
    }

    farBefore = h.sim->clientKnownMapObj[slotB].mapItem[farX][farY];
    farLive = cullFlip((*gs->mp).mapItem[farX][farY]);
    if (farBefore == farLive) {
        loopbackHarnessStop(&h);
        UT_FAIL("the square reads the same before and after — the case proves "
                "nothing");
    }
    if (!cullStageChange(&h, farX, farY, farLive)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the far square %u,%u already held the terrain being written",
                (unsigned)farX, (unsigned)farY);
    }

    transportUdpClientTestMapState(ct, &gen0, &resync0);
    if (!transportUdpClientTestBeginResync(ct)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the resync hook refused to start a resync");
    }

    /* Wait for the install, not just the request: mapResyncCount advances only
     * once a blob has been reassembled and taken. */
    for (i = 0; i < CULL_MAP_MAX; i++) {
        loopbackHarnessPump(&h);
        transportUdpClientTestMapState(ct, &gen1, &resync1);
        if (resync1 != resync0) break;
    }
    if (resync1 == resync0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the forced resync never completed within %d pumps",
                CULL_MAP_MAX);
    }

    /* The freshly installed map holds the square as B last knew it — the blob
     * came off B's copy, not off the live map. */
    if (clientSimGetMapTerrain(h.cs, farX, farY) != farBefore) {
        loopbackHarnessStop(&h);
        UT_FAIL("the resync blob handed B %u at %u,%u; B's copy holds %u — the "
                "blob came off the live map",
                (unsigned)clientSimGetMapTerrain(h.cs, farX, farY),
                (unsigned)farX, (unsigned)farY, (unsigned)farBefore);
    }

    loopbackHarnessStop(&h);
    return 0;
}
