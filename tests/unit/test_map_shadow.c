/*
 * Per-client copies of the terrain (test_map_shadow.c).
 *
 * The sim keeps one copy of the map per slot (ServerSim::clientKnownMap),
 * recording the terrain that slot has actually been given. Every map event is
 * broadcast to every client, so the copies hold exactly what the live map
 * holds; the snapshot header's checksum and the join/resync blob are taken
 * from the recipient's copy, so both ends compare the same bytes.
 *
 * These cases pin that: a copy tracks the live map across ticks of scattered
 * terrain changes, its checksum equals the live map's and equals what
 * serverSimBuildSnapshot stamps, serverSimGetCompressedMapFor produces the
 * same bytes as serverSimGetCompressedMap, and every point that installs a map
 * — a join, a round reset, a lobby map change — re-seeds the copies.
 *
 * A slot the UDP transport has marked culled is the exception: the tick leaves
 * its copy alone, because that transport sends it only the changes inside its
 * viewports and writes the copy as it sends. Three cases pin the sim side of
 * that — what the tick withholds, and the catch-up sweep that closes the gap
 * once the client can see the ground again. The transport side of the coupling
 * (in-viewport changes queued and written, everything else left owed) is driven
 * end-to-end in test_loopback_map_cull.c.
 *
 * The next three cases pin the round-start copy: it is re-taken wherever a map
 * is installed and does not follow the live map between those points, a slot
 * seeded from it holds — and compresses to — the round-start terrain rather
 * than the live map, and a sweep over the changed ground converges it. That is
 * what a player joining a running game downloads, so rejoining reveals nothing
 * about what has changed since the round started.
 *
 * The sim keeps a second per-slot record beside the terrain one: the pill
 * squares that slot has actually been sent (ServerSim::clientKnownPillX/Y),
 * with a round-start capture of its own. mapCalcChecksum folds the tile under
 * every pill, so the checksum a client can compute depends on the pill list it
 * holds as much as on its terrain, and the same three read sites — the snapshot
 * header's checksum, the join/resync blob, and the resync self-check — take
 * their pill list from that record. The last four cases pin it: it follows the
 * live list across frames in which pills move, its checksum equals the live
 * list's and equals what serverSimBuildSnapshot stamps,
 * serverSimGetCompressedMapFor produces the same bytes as
 * serverSimGetCompressedMap while it does, a slot seeded from the round-start
 * squares compresses to those instead, and a full-sync snapshot's pill entries
 * and pill events rebuild — in the client's own order — the list its checksum
 * was taken over.
 *
 * Every case drives ut_make_running_sim and pokes the GameSim directly (the
 * unittests profile permits T2-internal access).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "game_sim.h"
#include "gametype.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* clientKnownMap, the shadow entry points */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, ReloadCompressedInMemory */
#include "server/sim/server_sim_shared.h" /* serverSimSetActive, simMapChangeCallback */
#include "everard_map.h"
#include "test_harness.h"

/* One terrain write, as a tick's map-change callback would record it. */
typedef struct {
    BYTE x;
    BYTE y;
    BYTE terrain;
} MsChange;

/* Scattered interior tiles — away from the mined border, and spread far enough
 * apart that no single viewport could contain them all (the copies are written
 * unconditionally, so a cull creeping into the event path would show up here). */
static const MsChange kTick1[] = {
    { 60,  60, CRATER }, { 61,  60, ROAD }, { 200, 180, SWAMP },
};
static const MsChange kTick2[] = {
    { 60,  61, RUBBLE }, { 120, 45,  CRATER }, { 45, 200, ROAD }, { 200, 181, RUBBLE },
};
static const MsChange kTick3[] = {
    { 60,  60, ROAD }, { 199, 179, CRATER },
};

/* Arm the sim's map-change callback the way the running half-step does, so
 * mapSetPos records an EVENT_MAP_CHANGE the shadow tick can replay. */
static void ms_begin_tick(ServerSim *sim) {
    serverSimSetActive(sim);
    sim->mapEventCount = 0;
    mapSetChangeCallback(simMapChangeCallback);
}

/* Close the frame: drop the callback, then apply the frame's map events to
 * every slot's copy — the same order serverSimTick uses. */
static void ms_end_tick(ServerSim *sim) {
    mapSetChangeCallback(NULL);
    serverSimShadowTick(sim);
}

/* Run one frame's worth of terrain changes through mapSetPos. */
static void ms_run_tick(ServerSim *sim, const MsChange *changes, int n) {
    GameSim *gs = serverSimGetGameSim(sim);
    int i;
    ms_begin_tick(sim);
    for (i = 0; i < n; i++) {
        mapSetPos(gs, &gs->mp, changes[i].x, changes[i].y, changes[i].terrain,
                  FALSE, FALSE);
    }
    ms_end_tick(sim);
}

/* Put a byte into one slot's copy that the live map cannot be holding, so a
 * seed that never ran is visible rather than accidentally matching. */
static void ms_dirty(ServerSim *sim, BYTE slot, BYTE x, BYTE y) {
    GameSim *gs = serverSimGetGameSim(sim);
    sim->clientKnownMapObj[slot].mapItem[x][y] =
        (BYTE)((*gs->mp).mapItem[x][y] ^ 0xFFu);
}

/* What a slot's copy holds at one square, and what the live map holds. */
static BYTE ms_known(ServerSim *sim, BYTE slot, BYTE x, BYTE y) {
    return sim->clientKnownMapObj[slot].mapItem[x][y];
}

static BYTE ms_live(ServerSim *sim, BYTE x, BYTE y) {
    GameSim *gs = serverSimGetGameSim(sim);
    return (*gs->mp).mapItem[x][y];
}

/* What the round-start copy holds at one square, and whether it matches the
 * live map everywhere. */
static BYTE ms_round_start(ServerSim *sim, BYTE x, BYTE y) {
    return sim->roundStartMapObj.mapItem[x][y];
}

static bool ms_round_start_matches_live(ServerSim *sim) {
    GameSim *gs = serverSimGetGameSim(sim);
    return memcmp(sim->roundStartMapObj.mapItem, (*gs->mp).mapItem,
                  sizeof((*gs->mp).mapItem)) == 0;
}

/* -1 when every slot's copy matches the live map, else the first slot that
 * does not. */
static int ms_first_mismatch(ServerSim *sim) {
    GameSim *gs = serverSimGetGameSim(sim);
    BYTE slot;
    for (slot = 0; slot < MAX_TANKS; slot++) {
        if (memcmp(sim->clientKnownMapObj[slot].mapItem, (*gs->mp).mapItem,
                   sizeof((*gs->mp).mapItem)) != 0) {
            return (int)slot;
        }
    }
    return -1;
}

/* 1. A slot's copy follows the live map: identical when the sim comes up, and
 *    still identical after each of several frames of scattered changes —
 *    driven through mapSetPos, the same entry the game's terrain writes use,
 *    so the changes arrive as real map events. */
int run_map_shadow_tracks_real(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);

    UT_ASSERT_MSG(ms_first_mismatch(sim) < 0,
                  "slot %d's copy differs from the live map on a fresh sim",
                  ms_first_mismatch(sim));

    ms_run_tick(sim, kTick1, (int)(sizeof(kTick1) / sizeof(kTick1[0])));
    UT_ASSERT_MSG(sim->mapEventCount ==
                      (uint16_t)(sizeof(kTick1) / sizeof(kTick1[0])),
                  "mapSetPos recorded %u map events, expected %u — the change "
                  "callback did not fire, so this case proves nothing",
                  (unsigned)sim->mapEventCount,
                  (unsigned)(sizeof(kTick1) / sizeof(kTick1[0])));
    UT_ASSERT_MSG(ms_first_mismatch(sim) < 0,
                  "slot %d's copy missed the first frame's changes",
                  ms_first_mismatch(sim));

    ms_run_tick(sim, kTick2, (int)(sizeof(kTick2) / sizeof(kTick2[0])));
    UT_ASSERT_MSG(ms_first_mismatch(sim) < 0,
                  "slot %d's copy missed the second frame's changes",
                  ms_first_mismatch(sim));

    /* Third frame rewrites a tile the first frame already touched, so a copy
     * that applied changes out of order or skipped a repeat shows up here. */
    ms_run_tick(sim, kTick3, (int)(sizeof(kTick3) / sizeof(kTick3[0])));
    UT_ASSERT_MSG(ms_first_mismatch(sim) < 0,
                  "slot %d's copy missed the third frame's changes",
                  ms_first_mismatch(sim));

    /* And through the real call site: serverSimTick applies the frame's map
     * events itself, so whatever the sim changes is tracked without a test
     * driving the shadow directly. */
    {
        int t;
        for (t = 0; t < 20; t++) {
            serverSimTick(sim);
        }
    }
    UT_ASSERT_MSG(ms_first_mismatch(sim) < 0,
                  "slot %d's copy drifted while the sim ticked",
                  ms_first_mismatch(sim));

    serverSimDestroy(sim);
    return 0;
}

/* 2. The checksum over a slot's copy equals the checksum over the live map,
 *    and equals the value serverSimBuildSnapshot stamps into that slot's
 *    header on a full-sync tick. */
int run_map_shadow_crc_matches(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    ms_run_tick(sim, kTick1, (int)(sizeof(kTick1) / sizeof(kTick1[0])));
    ms_run_tick(sim, kTick2, (int)(sizeof(kTick2) / sizeof(kTick2[0])));

    uint16_t liveSum = mapCalcChecksum(&gs->mp, &gs->bs, &gs->pb);
    uint16_t slot0Sum = mapCalcChecksum(&sim->clientKnownMap[0], &gs->bs, &gs->pb);
    uint16_t slot1Sum = mapCalcChecksum(&sim->clientKnownMap[1], &gs->bs, &gs->pb);
    UT_ASSERT_MSG(slot0Sum == liveSum,
                  "slot 0 copy checksum %04x != live map checksum %04x",
                  slot0Sum, liveSum);
    UT_ASSERT_MSG(slot1Sum == liveSum,
                  "slot 1 copy checksum %04x != live map checksum %04x",
                  slot1Sum, liveSum);

    /* Drive the builder itself. lastFullSyncTick back to 0 forces the full
     * sync that carries the checksum; a non-sync tick stamps 0. */
    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));
    serverSimBuildSnapshot(sim, 0, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    UT_ASSERT_MSG(hdr.mapChecksum == slot0Sum,
                  "slot 0 header checksum %04x != its copy's checksum %04x",
                  hdr.mapChecksum, slot0Sum);

    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));
    serverSimBuildSnapshot(sim, 1, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    UT_ASSERT_MSG(hdr.mapChecksum == slot1Sum,
                  "slot 1 header checksum %04x != its copy's checksum %04x",
                  hdr.mapChecksum, slot1Sum);

    /* The recording build has no recipient copy behind it and reads the live
     * map. With every change broadcast the two agree, which is the invariant
     * this whole arrangement rests on. */
    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));
    serverSimBuildSnapshot(sim, 0, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, true);
    UT_ASSERT_MSG(hdr.mapChecksum == liveSum,
                  "noCull header checksum %04x != live map checksum %04x",
                  hdr.mapChecksum, liveSum);

    serverSimDestroy(sim);
    return 0;
}

/* 3. The blob compressed from a slot's copy is byte-identical to the blob
 *    compressed from the live map — before and after terrain changes. */
int run_map_shadow_blob_identical(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    /* 131,072 bytes is the headroom serverSimReloadMap allocates; two of them
     * are too much for the stack, so they come off the heap. */
    BYTE *live = (BYTE *)malloc(131072);
    BYTE *slotBlob = (BYTE *)malloc(131072);
    UT_ASSERT_MSG(live != NULL && slotBlob != NULL, "blob buffer allocation failed");

    int step;
    for (step = 0; step < 3; step++) {
        if (step == 1) {
            ms_run_tick(sim, kTick1, (int)(sizeof(kTick1) / sizeof(kTick1[0])));
        } else if (step == 2) {
            ms_run_tick(sim, kTick2, (int)(sizeof(kTick2) / sizeof(kTick2[0])));
        }

        int liveLen = serverSimGetCompressedMap(sim, live, 131072);
        int slotLen = serverSimGetCompressedMapFor(sim, 1, slotBlob, 131072);
        if (liveLen <= 0) {
            free(live); free(slotBlob); serverSimDestroy(sim);
            UT_FAIL("step %d: live compressed map is empty (%d)", step, liveLen);
        }
        if (slotLen != liveLen) {
            free(live); free(slotBlob); serverSimDestroy(sim);
            UT_FAIL("step %d: slot blob length %d != live blob length %d",
                    step, slotLen, liveLen);
        }
        if (memcmp(live, slotBlob, (size_t)liveLen) != 0) {
            free(live); free(slotBlob); serverSimDestroy(sim);
            UT_FAIL("step %d: slot blob bytes differ from the live blob", step);
        }
    }

    /* An out-of-range slot is refused rather than reading past the array. */
    UT_ASSERT_MSG(serverSimGetCompressedMapFor(sim, (BYTE)MAX_TANKS, slotBlob,
                                               131072) == 0,
                  "an out-of-range slot must produce no blob");

    free(live);
    free(slotBlob);
    serverSimDestroy(sim);
    return 0;
}

/* The square block the culling cases work over: interior ground, clear of the
 * mined border, and more squares than a small sweep cap can clear at once. */
#define MS_BLOCK_X0 60
#define MS_BLOCK_Y0 60
#define MS_BLOCK_N  4
#define MS_BLOCK_SQUARES (MS_BLOCK_N * MS_BLOCK_N)

/* Rewrite every square of the block to a terrain it is not already holding, as
 * one frame of map events. Returns how many squares actually moved, so a case
 * can refuse to draw conclusions from a write the map declined. */
static int ms_change_block(ServerSim *sim) {
    GameSim *gs = serverSimGetGameSim(sim);
    int changed = 0;
    int dx, dy;

    ms_begin_tick(sim);
    for (dx = 0; dx < MS_BLOCK_N; dx++) {
        for (dy = 0; dy < MS_BLOCK_N; dy++) {
            BYTE x = (BYTE)(MS_BLOCK_X0 + dx);
            BYTE y = (BYTE)(MS_BLOCK_Y0 + dy);
            BYTE before = (*gs->mp).mapItem[x][y];
            mapSetPos(gs, &gs->mp, x, y, (BYTE)(before == CRATER ? ROAD : CRATER),
                      FALSE, FALSE);
            if ((*gs->mp).mapItem[x][y] != before) changed++;
        }
    }
    ms_end_tick(sim);
    return changed;
}

/* How many squares of the block a slot's copy is behind the live map on. */
static int ms_block_stale(ServerSim *sim, BYTE slot) {
    int stale = 0;
    int dx, dy;
    for (dx = 0; dx < MS_BLOCK_N; dx++) {
        for (dy = 0; dy < MS_BLOCK_N; dy++) {
            BYTE x = (BYTE)(MS_BLOCK_X0 + dx);
            BYTE y = (BYTE)(MS_BLOCK_Y0 + dy);
            if (ms_known(sim, slot, x, y) != ms_live(sim, x, y)) stale++;
        }
    }
    return stale;
}

static void ms_set_rect(ViewportRect *r, int minMX, int maxMX, int minMY,
                        int maxMY) {
    r->minMX = minMX;
    r->maxMX = maxMX;
    r->minMY = minMY;
    r->maxMY = maxMY;
}

/* 4. Every point that installs a map re-seeds the copies: a player joining
 *    re-seeds that slot, a round reset and a lobby map change re-seed them
 *    all. Each is checked against a copy deliberately dirtied by hand first,
 *    so a seed that never ran cannot pass. */
int run_map_shadow_seed_lifecycle(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    /* Join: dirty slot 2's copy, then seat a player there. */
    ms_dirty(sim, 2, 70, 70);
    ms_dirty(sim, 2, 71, 70);
    UT_ASSERT_MSG(ms_first_mismatch(sim) == 2,
                  "hand-dirtying slot 2 should leave it as the first mismatch "
                  "(got %d)", ms_first_mismatch(sim));
    serverSimAddPlayer(sim, 2, "P2", false);
    UT_ASSERT_MSG(ms_first_mismatch(sim) < 0,
                  "slot %d's copy was not re-seeded by the join",
                  ms_first_mismatch(sim));

    /* Round reset: change the terrain, dirty several copies, then reset. The
     * reset reloads the map from the cached round-start data, so the copies
     * have to come from the reloaded map rather than from what they held. */
    ms_run_tick(sim, kTick1, (int)(sizeof(kTick1) / sizeof(kTick1[0])));
    ms_dirty(sim, 0, 80, 80);
    ms_dirty(sim, 5, 81, 80);
    UT_ASSERT_MSG(ms_first_mismatch(sim) == 0,
                  "hand-dirtying slot 0 should leave it as the first mismatch "
                  "(got %d)", ms_first_mismatch(sim));
    serverSimResetGameWorld(sim);
    UT_ASSERT_MSG(ms_first_mismatch(sim) < 0,
                  "slot %d's copy was not re-seeded by the round reset",
                  ms_first_mismatch(sim));

    serverSimDestroy(sim);

    /* Lobby map change: a fresh lobby sim, every copy dirtied, then a map
     * installed over it. */
    {
        BYTE emap[6000] = E_MAP;
        ServerSim *lobbySim = serverSimCreateCompressed(emap, 5097,
                                                        "Everard Island",
                                                        gameOpen, false, 0, -1);
        UT_ASSERT_MSG(lobbySim != NULL, "serverSimCreateCompressed returned NULL");
        serverSimSetLobbyEnabled(lobbySim, true);

        BYTE slot;
        for (slot = 0; slot < MAX_TANKS; slot++) {
            ms_dirty(lobbySim, slot, 90, 90);
        }
        UT_ASSERT_MSG(ms_first_mismatch(lobbySim) == 0,
                      "hand-dirtying every copy should leave slot 0 as the "
                      "first mismatch (got %d)", ms_first_mismatch(lobbySim));

        bool ok = serverSimReloadCompressedInMemory(lobbySim, emap, 5097,
                                                    "Everard Island");
        if (!ok) {
            serverSimDestroy(lobbySim);
            UT_FAIL("serverSimReloadCompressedInMemory rejected the map");
        }
        if (ms_first_mismatch(lobbySim) >= 0) {
            int bad = ms_first_mismatch(lobbySim);
            serverSimDestroy(lobbySim);
            UT_FAIL("slot %d's copy was not re-seeded by the map change", bad);
        }
        serverSimDestroy(lobbySim);
    }

    return 0;
}

/* 5. A culled slot's copy is not the tick's to write. With slot 1 marked, a
 *    frame of terrain changes lands in slot 0's copy and not in slot 1's,
 *    however far the change is from anything; the single-slot write the
 *    transport makes as it queues an event moves that one square and nothing
 *    else; and unmarking hands the copy back to the tick. */
int run_map_shadow_cull_withholds(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    /* Nothing is culled until a transport says so — the state every case above
     * runs in, and the one that keeps in-process clients whole. */
    UT_ASSERT_MSG(serverSimGetShadowCulledMask(sim) == 0,
                  "a fresh sim starts with slots culled (mask %04x)",
                  (unsigned)serverSimGetShadowCulledMask(sim));

    serverSimSetShadowCulled(sim, 1, true);
    UT_ASSERT_MSG(serverSimIsShadowCulled(sim, 1), "slot 1 did not take the mark");
    UT_ASSERT_MSG(!serverSimIsShadowCulled(sim, 0),
                  "marking slot 1 also marked slot 0");
    UT_ASSERT_MSG(serverSimGetShadowCulledMask(sim) == 0x0002u,
                  "mask %04x after marking slot 1 alone",
                  (unsigned)serverSimGetShadowCulledMask(sim));

    /* What slot 1 holds over the block before anything changes — the bytes it
     * must still be holding afterwards. */
    BYTE held[MS_BLOCK_N][MS_BLOCK_N];
    int dx, dy;
    for (dx = 0; dx < MS_BLOCK_N; dx++) {
        for (dy = 0; dy < MS_BLOCK_N; dy++) {
            held[dx][dy] = ms_known(sim, 1, (BYTE)(MS_BLOCK_X0 + dx),
                                    (BYTE)(MS_BLOCK_Y0 + dy));
        }
    }

    int changed = ms_change_block(sim);
    UT_ASSERT_MSG(changed == MS_BLOCK_SQUARES,
                  "only %d of %d squares moved — the frame proves nothing",
                  changed, MS_BLOCK_SQUARES);

    for (dx = 0; dx < MS_BLOCK_N; dx++) {
        for (dy = 0; dy < MS_BLOCK_N; dy++) {
            BYTE x = (BYTE)(MS_BLOCK_X0 + dx);
            BYTE y = (BYTE)(MS_BLOCK_Y0 + dy);
            UT_ASSERT_MSG(ms_known(sim, 0, x, y) == ms_live(sim, x, y),
                          "exempt slot 0 missed the change at %u,%u",
                          (unsigned)x, (unsigned)y);
            UT_ASSERT_MSG(ms_known(sim, 1, x, y) == held[dx][dy],
                          "culled slot 1 took the change at %u,%u from the tick",
                          (unsigned)x, (unsigned)y);
        }
    }

    /* The write the drain makes for a square it has actually queued: that slot,
     * that square, nothing else. */
    {
        BYTE x = (BYTE)MS_BLOCK_X0, y = (BYTE)MS_BLOCK_Y0;
        serverSimShadowApplySlot(sim, 1, x, y, ms_live(sim, x, y));
        UT_ASSERT_MSG(ms_known(sim, 1, x, y) == ms_live(sim, x, y),
                      "the per-slot write did not land at %u,%u",
                      (unsigned)x, (unsigned)y);
        UT_ASSERT_MSG(ms_known(sim, 1, x, (BYTE)(y + 1)) == held[0][1],
                      "the per-slot write spilled onto %u,%u",
                      (unsigned)x, (unsigned)(y + 1));
        UT_ASSERT_MSG(ms_block_stale(sim, 1) == MS_BLOCK_SQUARES - 1,
                      "slot 1 is behind on %d squares, expected %d",
                      ms_block_stale(sim, 1), MS_BLOCK_SQUARES - 1);
    }

    /* Unmarked — the slot is an in-process client's again and the tick writes
     * it. The frame below rewrites every square of the block, so a copy the
     * tick is feeding is current on all of them afterwards. */
    serverSimSetShadowCulled(sim, 1, false);
    UT_ASSERT_MSG(serverSimGetShadowCulledMask(sim) == 0,
                  "unmarking left mask %04x",
                  (unsigned)serverSimGetShadowCulledMask(sim));
    changed = ms_change_block(sim);
    UT_ASSERT_MSG(changed == MS_BLOCK_SQUARES,
                  "only %d of %d squares moved on the second frame",
                  changed, MS_BLOCK_SQUARES);
    UT_ASSERT_MSG(ms_block_stale(sim, 1) == 0,
                  "slot 1 is still behind on %d squares after being unmarked",
                  ms_block_stale(sim, 1));

    serverSimDestroy(sim);
    return 0;
}

/* 6. The catch-up sweep pays back what a culled slot is owed, inside the rects
 *    it is given and no faster than the cap allows, and converges to silence.
 *    Rects nowhere near the staleness say nothing at all. */
int run_map_shadow_sweep_converges(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    serverSimSetShadowCulled(sim, 0, true);
    int changed = ms_change_block(sim);
    UT_ASSERT_MSG(changed == MS_BLOCK_SQUARES,
                  "only %d of %d squares moved — nothing to catch up on",
                  changed, MS_BLOCK_SQUARES);
    UT_ASSERT_MSG(ms_block_stale(sim, 0) == MS_BLOCK_SQUARES,
                  "slot 0 is behind on %d squares, expected the whole block (%d)",
                  ms_block_stale(sim, 0), MS_BLOCK_SQUARES);

    ViewportRect over, away;
    ms_set_rect(&over, MS_BLOCK_X0, MS_BLOCK_X0 + MS_BLOCK_N - 1,
                MS_BLOCK_Y0, MS_BLOCK_Y0 + MS_BLOCK_N - 1);
    ms_set_rect(&away, MS_BLOCK_X0 + 100, MS_BLOCK_X0 + 120,
                MS_BLOCK_Y0 + 100, MS_BLOCK_Y0 + 120);

    GameEvent out[MS_BLOCK_SQUARES];

    /* Ground the client cannot see stays owed: a rect elsewhere emits nothing
     * and leaves the block exactly as stale as it was. */
    UT_ASSERT_MSG(serverSimShadowSweep(sim, 0, &away, 1, out, MS_BLOCK_SQUARES) == 0,
                  "a rect away from the staleness emitted something");
    UT_ASSERT_MSG(ms_block_stale(sim, 0) == MS_BLOCK_SQUARES,
                  "a rect away from the staleness moved the copy");

    /* Capped: five squares this call, and only those five caught up. */
    int got = serverSimShadowSweep(sim, 0, &over, 1, out, 5);
    UT_ASSERT_MSG(got == 5, "a cap of 5 emitted %d", got);
    int k;
    for (k = 0; k < got; k++) {
        BYTE x = out[k].data[0], y = out[k].data[1];
        UT_ASSERT_MSG(out[k].type == EVENT_MAP_CHANGE,
                      "emitted event %d has type %u, expected EVENT_MAP_CHANGE",
                      k, (unsigned)out[k].type);
        UT_ASSERT_MSG(x >= MS_BLOCK_X0 && x < MS_BLOCK_X0 + MS_BLOCK_N &&
                          y >= MS_BLOCK_Y0 && y < MS_BLOCK_Y0 + MS_BLOCK_N,
                      "emitted square %u,%u is outside the rect",
                      (unsigned)x, (unsigned)y);
        UT_ASSERT_MSG(out[k].data[2] == ms_live(sim, x, y),
                      "emitted square %u,%u carries terrain %u, live is %u",
                      (unsigned)x, (unsigned)y, (unsigned)out[k].data[2],
                      (unsigned)ms_live(sim, x, y));
        UT_ASSERT_MSG(ms_known(sim, 0, x, y) == ms_live(sim, x, y),
                      "emitted square %u,%u was not written into the copy",
                      (unsigned)x, (unsigned)y);
    }
    UT_ASSERT_MSG(ms_block_stale(sim, 0) == MS_BLOCK_SQUARES - 5,
                  "after one capped sweep %d squares are still owed, expected %d",
                  ms_block_stale(sim, 0), MS_BLOCK_SQUARES - 5);

    /* Repeated calls converge: the block clears and the sweep falls silent. */
    int total = got;
    int rounds = 0;
    while ((got = serverSimShadowSweep(sim, 0, &over, 1, out, 5)) > 0) {
        total += got;
        rounds++;
        UT_ASSERT_MSG(rounds <= MS_BLOCK_SQUARES,
                      "the sweep is still emitting after %d rounds (%d events) "
                      "— it is not converging", rounds, total);
    }
    UT_ASSERT_MSG(total == MS_BLOCK_SQUARES,
                  "the sweep emitted %d squares in all, expected %d",
                  total, MS_BLOCK_SQUARES);
    UT_ASSERT_MSG(ms_block_stale(sim, 0) == 0,
                  "%d squares of the block are still owed after convergence",
                  ms_block_stale(sim, 0));
    UT_ASSERT_MSG(serverSimShadowSweep(sim, 0, &over, 1, out, MS_BLOCK_SQUARES) == 0,
                  "a caught-up copy still emits");

    serverSimDestroy(sim);
    return 0;
}

/* 7. The sweep's edges: it walks the squares its rects name and no others, a
 *    rect running off the map is clamped rather than walked off, and a cap of
 *    zero emits nothing and moves nothing. */
int run_map_shadow_sweep_bounds(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    serverSimSetShadowCulled(sim, 0, true);

    /* Staleness put straight into the copy, so it can sit on squares near the
     * map border that the terrain rules would not let a change land on. */
    ms_dirty(sim, 0, 1, 1);
    ms_dirty(sim, 0, 3, 4);
    ms_dirty(sim, 0, 40, 40);

    GameEvent out[16];
    ViewportRect r;

    /* A cap of zero: nothing emitted, nothing written. */
    ms_set_rect(&r, 0, 10, 0, 10);
    UT_ASSERT_MSG(serverSimShadowSweep(sim, 0, &r, 1, out, 0) == 0,
                  "a cap of zero emitted something");
    UT_ASSERT_MSG(ms_known(sim, 0, 1, 1) != ms_live(sim, 1, 1),
                  "a cap of zero wrote the copy at 1,1");

    /* A rect stopping one square short of a stale square leaves it owed. */
    ms_set_rect(&r, 0, 39, 0, 39);
    UT_ASSERT_MSG(serverSimShadowSweep(sim, 0, &r, 1, out, 16) == 2,
                  "a rect covering two of the three stale squares emitted a "
                  "different number");
    UT_ASSERT_MSG(ms_known(sim, 0, 40, 40) != ms_live(sim, 40, 40),
                  "the square outside the rect was written anyway");

    /* A rect running off two edges of the map is clamped, not walked off. */
    ms_dirty(sim, 0, 0, 0);
    ms_dirty(sim, 0, 2, 3);
    ms_set_rect(&r, -30, 5, -30, 5);
    {
        int got = serverSimShadowSweep(sim, 0, &r, 1, out, 16);
        UT_ASSERT_MSG(got == 2, "a clamped rect emitted %d, expected 2", got);
        UT_ASSERT_MSG(ms_known(sim, 0, 0, 0) == ms_live(sim, 0, 0),
                      "the corner square was not caught up");
        UT_ASSERT_MSG(ms_known(sim, 0, 2, 3) == ms_live(sim, 2, 3),
                      "square 2,3 was not caught up");
    }

    /* A rect wholly off the map is skipped rather than clamped into range. */
    ms_dirty(sim, 0, 0, 0);
    ms_set_rect(&r, -50, -10, -50, -10);
    UT_ASSERT_MSG(serverSimShadowSweep(sim, 0, &r, 1, out, 16) == 0,
                  "a rect wholly off the map emitted something");
    UT_ASSERT_MSG(ms_known(sim, 0, 0, 0) != ms_live(sim, 0, 0),
                  "a rect wholly off the map wrote the corner square");

    /* Two rects over the same square emit it once: the first pass writes the
     * copy, so the second finds nothing to say. */
    {
        ViewportRect pair[2];
        int got;
        ms_set_rect(&pair[0], 0, 8, 0, 8);
        ms_set_rect(&pair[1], 0, 8, 0, 8);
        got = serverSimShadowSweep(sim, 0, pair, 2, out, 16);
        UT_ASSERT_MSG(got == 1, "overlapping rects emitted %d, expected 1", got);
    }

    serverSimDestroy(sim);
    return 0;
}

/* Blob buffers for the round-start cases. 131,072 bytes is the headroom
 * serverSimReloadMap allocates; static rather than on the stack because three
 * of them do not fit there. */
static BYTE msStartBlob[131072];
static BYTE msJoinBlob[131072];
static BYTE msLiveBlob[131072];

/* 8. The round-start copy is taken wherever a map is installed and stands
 *    still between those points: it matches the live map on a fresh sim, keeps
 *    the old terrain through a frame of changes the live map and the copies
 *    the tick owns both take, and is taken again by a round reset — from the
 *    reloaded map, not from what it was holding. */
int run_map_shadow_round_start_capture(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    UT_ASSERT_MSG(sim->roundStartMap == &sim->roundStartMapObj,
                  "the sim's map install did not bind the round-start handle");
    UT_ASSERT_MSG(ms_round_start_matches_live(sim),
                  "the round-start copy differs from the live map on a fresh sim");

    /* What the frame below moves those squares off, so the copy is checked
     * against the terrain rather than against itself. */
    const int n1 = (int)(sizeof(kTick1) / sizeof(kTick1[0]));
    BYTE before[sizeof(kTick1) / sizeof(kTick1[0])];
    int i;
    for (i = 0; i < n1; i++) {
        before[i] = ms_live(sim, kTick1[i].x, kTick1[i].y);
    }

    ms_run_tick(sim, kTick1, n1);
    UT_ASSERT_MSG(sim->mapEventCount == (uint16_t)n1,
                  "mapSetPos recorded %u map events, expected %d — the change "
                  "callback did not fire, so this case proves nothing",
                  (unsigned)sim->mapEventCount, n1);

    for (i = 0; i < n1; i++) {
        BYTE x = kTick1[i].x, y = kTick1[i].y;
        UT_ASSERT_MSG(ms_live(sim, x, y) != before[i],
                      "square %u,%u did not move — the frame proves nothing",
                      (unsigned)x, (unsigned)y);
        UT_ASSERT_MSG(ms_round_start(sim, x, y) == before[i],
                      "the round-start copy took the change at %u,%u",
                      (unsigned)x, (unsigned)y);
        UT_ASSERT_MSG(ms_known(sim, 0, x, y) == ms_live(sim, x, y),
                      "exempt slot 0's copy missed the change at %u,%u",
                      (unsigned)x, (unsigned)y);
    }
    UT_ASSERT_MSG(!ms_round_start_matches_live(sim),
                  "the round-start copy still matches the live map after a "
                  "frame of terrain changes");

    /* The reset reinstalls the map from the cached round-start data, so the
     * copy is taken again and comes out equal to the reloaded terrain. */
    serverSimResetGameWorld(sim);
    for (i = 0; i < n1; i++) {
        UT_ASSERT_MSG(ms_live(sim, kTick1[i].x, kTick1[i].y) == before[i],
                      "the reset did not restore the terrain at %u,%u, so the "
                      "re-capture below proves nothing",
                      (unsigned)kTick1[i].x, (unsigned)kTick1[i].y);
    }
    UT_ASSERT_MSG(ms_round_start_matches_live(sim),
                  "the round reset did not take the round-start copy again");

    serverSimDestroy(sim);
    return 0;
}

/* 9. What a player joining a running game is handed. The terrain moves
 *    mid-round, then the slot is seeded the way the join does it — the add's
 *    live-map seed, then the round-start re-seed — and its copy, and the blob
 *    compressed from it, hold the terrain the round started on rather than the
 *    terrain as it stands. Driving over the changed ground then pays it back:
 *    the sweep emits exactly the changed squares and converges. */
int run_map_shadow_join_seeds_round_start(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    /* The blob a joiner would have downloaded at round start, taken from an
     * untouched slot before anything moves. */
    int startLen = serverSimGetCompressedMapFor(sim, 1, msStartBlob,
                                                (int)sizeof(msStartBlob));
    UT_ASSERT_MSG(startLen > 0, "the round-start blob is empty (%d)", startLen);

    /* Mid-round, with slot 2 marked the way the UDP transport marks the slot it
     * takes, so nothing but the seed and the sweep writes its copy. */
    serverSimSetShadowCulled(sim, 2, true);
    int changed = ms_change_block(sim);
    UT_ASSERT_MSG(changed == MS_BLOCK_SQUARES,
                  "only %d of %d squares moved — the round has not diverged",
                  changed, MS_BLOCK_SQUARES);

    /* The join: the add seeds the slot from the live map, then the accept
     * restarts it from the round-start terrain. */
    serverSimShadowSeed(sim, 2);
    UT_ASSERT_MSG(ms_block_stale(sim, 2) == 0,
                  "the live-map seed left slot 2 behind on %d squares",
                  ms_block_stale(sim, 2));
    serverSimShadowSeedRoundStart(sim, 2);

    UT_ASSERT_MSG(memcmp(sim->clientKnownMapObj[2].mapItem,
                         sim->roundStartMapObj.mapItem,
                         sizeof(sim->roundStartMapObj.mapItem)) == 0,
                  "the joining slot's copy is not the round-start terrain");
    UT_ASSERT_MSG(ms_block_stale(sim, 2) == MS_BLOCK_SQUARES,
                  "the joining slot is behind on %d of the block's %d changed "
                  "squares — it is still holding the live map",
                  ms_block_stale(sim, 2), MS_BLOCK_SQUARES);

    /* So the blob it downloads is the round-start blob, byte for byte. */
    int joinLen = serverSimGetCompressedMapFor(sim, 2, msJoinBlob,
                                               (int)sizeof(msJoinBlob));
    UT_ASSERT_MSG(joinLen == startLen,
                  "the join blob is %d bytes, the round-start blob %d",
                  joinLen, startLen);
    UT_ASSERT_MSG(memcmp(msJoinBlob, msStartBlob, (size_t)startLen) == 0,
                  "the join blob's bytes differ from the round-start blob's");

    /* And is not the live map's: slot 0 took the frame, and its blob differs. */
    int liveLen = serverSimGetCompressedMapFor(sim, 0, msLiveBlob,
                                               (int)sizeof(msLiveBlob));
    UT_ASSERT_MSG(liveLen > 0, "the live blob is empty (%d)", liveLen);
    UT_ASSERT_MSG(liveLen != joinLen ||
                      memcmp(msLiveBlob, msJoinBlob, (size_t)joinLen) != 0,
                  "the join blob is identical to the live map's blob — the "
                  "round-start seed changed nothing");

    /* Ground the joiner drives up to: the sweep hands it the difference, all of
     * it inside the block that changed, and then falls silent. */
    ViewportRect over;
    GameEvent out[MS_BLOCK_SQUARES];
    int got, k;
    ms_set_rect(&over, MS_BLOCK_X0, MS_BLOCK_X0 + MS_BLOCK_N - 1,
                MS_BLOCK_Y0, MS_BLOCK_Y0 + MS_BLOCK_N - 1);
    got = serverSimShadowSweep(sim, 2, &over, 1, out, MS_BLOCK_SQUARES);
    UT_ASSERT_MSG(got == MS_BLOCK_SQUARES,
                  "the sweep emitted %d squares, expected the block's %d",
                  got, MS_BLOCK_SQUARES);
    for (k = 0; k < got; k++) {
        BYTE x = out[k].data[0], y = out[k].data[1];
        UT_ASSERT_MSG(out[k].type == EVENT_MAP_CHANGE,
                      "emitted event %d has type %u, expected EVENT_MAP_CHANGE",
                      k, (unsigned)out[k].type);
        UT_ASSERT_MSG(x >= MS_BLOCK_X0 && x < MS_BLOCK_X0 + MS_BLOCK_N &&
                          y >= MS_BLOCK_Y0 && y < MS_BLOCK_Y0 + MS_BLOCK_N,
                      "emitted square %u,%u is outside the changed block",
                      (unsigned)x, (unsigned)y);
        UT_ASSERT_MSG(out[k].data[2] == ms_live(sim, x, y),
                      "emitted square %u,%u carries terrain %u, live is %u",
                      (unsigned)x, (unsigned)y, (unsigned)out[k].data[2],
                      (unsigned)ms_live(sim, x, y));
    }
    UT_ASSERT_MSG(ms_block_stale(sim, 2) == 0,
                  "%d squares are still owed after the sweep",
                  ms_block_stale(sim, 2));
    UT_ASSERT_MSG(serverSimShadowSweep(sim, 2, &over, 1, out,
                                       MS_BLOCK_SQUARES) == 0,
                  "the sweep still emits once the joiner has caught up");

    serverSimDestroy(sim);
    return 0;
}

/* 10. With nothing captured — the state the struct is in before the first map
 *     is installed — the round-start seed is the live-map seed, so a slot can
 *     never be handed an empty map. */
int run_map_shadow_round_start_fallback(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    /* Every map install takes the copy, so the never-captured state cannot be
     * reached from the outside once the sim is up — put it back by hand. */
    sim->roundStartMap = NULL;

    /* Move the live map first, so a copy seeded from anything else shows up. */
    ms_run_tick(sim, kTick1, (int)(sizeof(kTick1) / sizeof(kTick1[0])));

    ms_dirty(sim, 3, 70, 70);
    ms_dirty(sim, 3, 71, 70);
    UT_ASSERT_MSG(ms_first_mismatch(sim) == 3,
                  "hand-dirtying slot 3 should leave it as the first mismatch "
                  "(got %d)", ms_first_mismatch(sim));

    serverSimShadowSeedRoundStart(sim, 3);
    UT_ASSERT_MSG(sim->clientKnownMap[3] == &sim->clientKnownMapObj[3],
                  "the fallback left slot 3's handle unbound");
    UT_ASSERT_MSG(ms_first_mismatch(sim) < 0,
                  "slot %d's copy is not the live map — the fallback did not "
                  "seed from it", ms_first_mismatch(sim));

    serverSimDestroy(sim);
    return 0;
}

/* What a slot's record says a pill's square is, and where the live pill is. */
static BYTE ms_pill_known_x(ServerSim *sim, BYTE slot, BYTE p) {
    return sim->clientKnownPillX[slot][p];
}

static BYTE ms_pill_known_y(ServerSim *sim, BYTE slot, BYTE p) {
    return sim->clientKnownPillY[slot][p];
}

static BYTE ms_pill_live_x(ServerSim *sim, BYTE p) {
    GameSim *gs = serverSimGetGameSim(sim);
    return (*gs->pb).item[p].x;
}

static BYTE ms_pill_live_y(ServerSim *sim, BYTE p) {
    GameSim *gs = serverSimGetGameSim(sim);
    return (*gs->pb).item[p].y;
}

/* -1 when every slot's recorded squares match the live list, else the first
 * slot that does not. */
static int ms_pill_first_mismatch(ServerSim *sim) {
    GameSim *gs = serverSimGetGameSim(sim);
    BYTE np = pillsGetNumPills(&gs->pb);
    BYTE slot, p;
    for (slot = 0; slot < MAX_TANKS; slot++) {
        for (p = 0; p < np && p < MAX_PILLS; p++) {
            if (ms_pill_known_x(sim, slot, p) != ms_pill_live_x(sim, p) ||
                ms_pill_known_y(sim, slot, p) != ms_pill_live_y(sim, p)) {
                return (int)slot;
            }
        }
    }
    return -1;
}

/* Move a pill and run one frame. The frame is what makes it a real move: the
 * tick's pill diff emits EVENT_PILL_UPDATE for it and the records take the new
 * square at the end of the frame, as they do for a pill an LGM drops. */
static void ms_move_pill(ServerSim *sim, BYTE p, BYTE x, BYTE y) {
    GameSim *gs = serverSimGetGameSim(sim);
    (*gs->pb).item[p].x = x;
    (*gs->pb).item[p].y = y;
    serverSimTick(sim);
}

/* The four terrains mapCalcChecksum rewrites the tile for when a pill stands on
 * it. A pill anywhere else leaves the hash unchanged, so a case that wants the
 * pill list to matter has to stand the pill on one of these. */
static bool ms_pill_fold_terrain(BYTE t) {
    return t == RIVER || t == DEEP_SEA || t == BUILDING || t == HALFBUILDING;
}

/* Find a square the fold applies to, clear of bases and of every pill: the
 * checksum rewrites a base tile before it looks at pills, so a pill standing on
 * one would leave the fold invisible. River and building are preferred over
 * deep sea so the pill ends up on ground the game itself could put it on.
 * Returns false if the map holds no such square. */
static bool ms_find_fold_square(ServerSim *sim, BYTE *outX, BYTE *outY) {
    GameSim *gs = serverSimGetGameSim(sim);
    bool haveSea = false;
    BYTE seaX = 0, seaY = 0;
    int x, y;

    for (x = 20; x < 236; x++) {
        for (y = 20; y < 236; y++) {
            BYTE t = ms_live(sim, (BYTE)x, (BYTE)y);
            if (!ms_pill_fold_terrain(t)) continue;
            if (basesExistPos(&gs->bs, (BYTE)x, (BYTE)y)) continue;
            if (pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y)) continue;
            if (t == DEEP_SEA) {
                if (!haveSea) {
                    seaX = (BYTE)x;
                    seaY = (BYTE)y;
                    haveSea = true;
                }
                continue;
            }
            *outX = (BYTE)x;
            *outY = (BYTE)y;
            return true;
        }
    }
    if (haveSea) {
        *outX = seaX;
        *outY = seaY;
        return true;
    }
    return false;
}

/* 11. Each slot's record of the pill squares follows the live list: identical
 *     when the sim comes up, and still identical after frames in which pills
 *     move. serverSimGetPillsForSlot hands that list back with the recorded
 *     square and every other field straight off the live pill, and refuses a
 *     slot outside the array. */
int run_pill_shadow_tracks_real(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);

    GameSim *gs = serverSimGetGameSim(sim);
    BYTE np = pillsGetNumPills(&gs->pb);
    UT_ASSERT_MSG(np >= 2, "the map carries %u pills; this case needs two",
                  (unsigned)np);

    UT_ASSERT_MSG(ms_pill_first_mismatch(sim) < 0,
                  "slot %d's pill squares differ from the live list on a fresh "
                  "sim", ms_pill_first_mismatch(sim));

    /* Three frames: one pill moves, then another, then the first back to where
     * it started, so a record written out of order or skipping a repeat shows
     * up here. */
    BYTE p0x = ms_pill_live_x(sim, 0), p0y = ms_pill_live_y(sim, 0);
    ms_move_pill(sim, 0, (BYTE)(p0x + 5), (BYTE)(p0y + 5));
    UT_ASSERT_MSG(ms_pill_live_x(sim, 0) == (BYTE)(p0x + 5) &&
                      ms_pill_live_y(sim, 0) == (BYTE)(p0y + 5),
                  "pill 0 is at %u,%u after the move — the frame proves nothing",
                  (unsigned)ms_pill_live_x(sim, 0),
                  (unsigned)ms_pill_live_y(sim, 0));
    UT_ASSERT_MSG(ms_pill_first_mismatch(sim) < 0,
                  "slot %d's record missed the first pill move",
                  ms_pill_first_mismatch(sim));

    BYTE p1x = ms_pill_live_x(sim, 1), p1y = ms_pill_live_y(sim, 1);
    ms_move_pill(sim, 1, (BYTE)(p1x + 3), (BYTE)(p1y + 4));
    UT_ASSERT_MSG(ms_pill_first_mismatch(sim) < 0,
                  "slot %d's record missed the second pill move",
                  ms_pill_first_mismatch(sim));

    ms_move_pill(sim, 0, p0x, p0y);
    UT_ASSERT_MSG(ms_pill_first_mismatch(sim) < 0,
                  "slot %d's record missed pill 0 moving back",
                  ms_pill_first_mismatch(sim));

    /* The list a reader is handed: the recorded square, and everything else as
     * the live pill holds it. */
    {
        struct pillsObj slotPills;
        BYTE slot, p;
        for (slot = 0; slot < MAX_TANKS; slot++) {
            memset(&slotPills, 0, sizeof(slotPills));
            UT_ASSERT_MSG(serverSimGetPillsForSlot(sim, slot, &slotPills),
                          "slot %u has no pill record", (unsigned)slot);
            UT_ASSERT_MSG(slotPills.numPills == np,
                          "slot %u's list holds %u pills, the live list %u",
                          (unsigned)slot, (unsigned)slotPills.numPills,
                          (unsigned)np);
            for (p = 0; p < np; p++) {
                UT_ASSERT_MSG(slotPills.item[p].x == ms_pill_live_x(sim, p) &&
                                  slotPills.item[p].y == ms_pill_live_y(sim, p),
                              "slot %u pill %u is at %u,%u, the live pill at "
                              "%u,%u", (unsigned)slot, (unsigned)p,
                              (unsigned)slotPills.item[p].x,
                              (unsigned)slotPills.item[p].y,
                              (unsigned)ms_pill_live_x(sim, p),
                              (unsigned)ms_pill_live_y(sim, p));
                UT_ASSERT_MSG(slotPills.item[p].owner == (*gs->pb).item[p].owner &&
                                  slotPills.item[p].armour == (*gs->pb).item[p].armour &&
                                  slotPills.item[p].inTank == (*gs->pb).item[p].inTank &&
                                  slotPills.item[p].speed == (*gs->pb).item[p].speed,
                              "slot %u pill %u's owner/armour/inTank/speed "
                              "differ from the live pill's",
                              (unsigned)slot, (unsigned)p);
            }
        }

        /* A slot outside the array, and a missing destination, are refused
         * rather than read or written past. */
        UT_ASSERT_MSG(!serverSimGetPillsForSlot(sim, (BYTE)MAX_TANKS, &slotPills),
                      "an out-of-range slot produced a pill list");
        UT_ASSERT_MSG(!serverSimGetPillsForSlot(sim, 0, NULL),
                      "a NULL destination produced a pill list");
    }

    serverSimDestroy(sim);
    return 0;
}

/* 12. The checksum over a slot's records — its terrain copy and its pill
 *     squares — equals the checksum over the live map and the live pills while
 *     the records track them, and equals the value serverSimBuildSnapshot
 *     stamps into that slot's header on a full-sync tick. */
int run_pill_shadow_crc_matches(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1, "the map carries no pills");

    /* A pill move and a frame of terrain changes, so both halves of the record
     * are exercised rather than compared at rest. */
    ms_move_pill(sim, 0, (BYTE)(ms_pill_live_x(sim, 0) + 4),
                 (BYTE)(ms_pill_live_y(sim, 0) + 4));
    ms_run_tick(sim, kTick1, (int)(sizeof(kTick1) / sizeof(kTick1[0])));

    struct pillsObj slot0Pills, slot1Pills;
    pillboxes slot0Pb = &slot0Pills;
    pillboxes slot1Pb = &slot1Pills;
    UT_ASSERT_MSG(serverSimGetPillsForSlot(sim, 0, &slot0Pills),
                  "slot 0 has no pill record");
    UT_ASSERT_MSG(serverSimGetPillsForSlot(sim, 1, &slot1Pills),
                  "slot 1 has no pill record");

    uint16_t liveSum = mapCalcChecksum(&gs->mp, &gs->bs, &gs->pb);
    uint16_t slot0Sum = mapCalcChecksum(&sim->clientKnownMap[0], &gs->bs, &slot0Pb);
    uint16_t slot1Sum = mapCalcChecksum(&sim->clientKnownMap[1], &gs->bs, &slot1Pb);
    UT_ASSERT_MSG(slot0Sum == liveSum,
                  "slot 0 record checksum %04x != live checksum %04x",
                  slot0Sum, liveSum);
    UT_ASSERT_MSG(slot1Sum == liveSum,
                  "slot 1 record checksum %04x != live checksum %04x",
                  slot1Sum, liveSum);

    /* Drive the builder itself. lastFullSyncTick back to 0 forces the full
     * sync that carries the checksum; a non-sync tick stamps 0. */
    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));
    serverSimBuildSnapshot(sim, 0, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    UT_ASSERT_MSG(hdr.mapChecksum == slot0Sum,
                  "slot 0 header checksum %04x != its records' checksum %04x",
                  hdr.mapChecksum, slot0Sum);

    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));
    serverSimBuildSnapshot(sim, 1, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    UT_ASSERT_MSG(hdr.mapChecksum == slot1Sum,
                  "slot 1 header checksum %04x != its records' checksum %04x",
                  hdr.mapChecksum, slot1Sum);

    /* The recording build has no recipient records behind it and reads the live
     * map and the live pills. */
    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));
    serverSimBuildSnapshot(sim, 0, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, true);
    UT_ASSERT_MSG(hdr.mapChecksum == liveSum,
                  "noCull header checksum %04x != live checksum %04x",
                  hdr.mapChecksum, liveSum);

    serverSimDestroy(sim);
    return 0;
}

/* 13. The blob compressed from a slot's records is byte-identical to the blob
 *     compressed from the live map and pills while the records track them —
 *     before and after a pill move — and carries the slot's own pill squares,
 *     not the live ones, once a round-start seed puts the two apart. */
int run_pill_shadow_blob_identical(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1, "the map carries no pills");

    /* 131,072 bytes is the headroom serverSimReloadMap allocates; two of them
     * are too much for the stack, so they come off the heap. */
    BYTE *live = (BYTE *)malloc(131072);
    BYTE *slotBlob = (BYTE *)malloc(131072);
    UT_ASSERT_MSG(live != NULL && slotBlob != NULL, "blob buffer allocation failed");

    int step;
    for (step = 0; step < 2; step++) {
        if (step == 1) {
            ms_move_pill(sim, 0, (BYTE)(ms_pill_live_x(sim, 0) + 6),
                         (BYTE)(ms_pill_live_y(sim, 0) + 6));
        }

        int stepLive = serverSimGetCompressedMap(sim, live, 131072);
        int stepSlot = serverSimGetCompressedMapFor(sim, 1, slotBlob, 131072);
        if (stepLive <= 0) {
            free(live); free(slotBlob); serverSimDestroy(sim);
            UT_FAIL("step %d: live compressed map is empty (%d)", step, stepLive);
        }
        if (stepSlot != stepLive ||
            memcmp(live, slotBlob, (size_t)stepLive) != 0) {
            free(live); free(slotBlob); serverSimDestroy(sim);
            UT_FAIL("step %d: slot blob (%d bytes) differs from the live blob "
                    "(%d bytes)", step, stepSlot, stepLive);
        }
    }

    /* Now put the two apart. Move the pill again, seed slot 1 from the
     * round-start squares, and cut its blob straight away without another
     * tick — the next tick's write would put the live square back, which is why
     * the join path cuts the blob immediately after seeding the slot. */
    BYTE rsX = sim->roundStartPillX[0];
    BYTE rsY = sim->roundStartPillY[0];
    ms_move_pill(sim, 0, (BYTE)(rsX + 9), (BYTE)(rsY + 9));
    if (ms_pill_live_x(sim, 0) == rsX && ms_pill_live_y(sim, 0) == rsY) {
        free(live); free(slotBlob); serverSimDestroy(sim);
        UT_FAIL("pill 0 is back on its round-start square %u,%u — the seed "
                "below would prove nothing", (unsigned)rsX, (unsigned)rsY);
    }
    serverSimPillShadowSeedRoundStart(sim, 1);

    int joinLen = serverSimGetCompressedMapFor(sim, 1, slotBlob, 131072);
    int liveLen = serverSimGetCompressedMap(sim, live, 131072);
    if (joinLen <= 0 || liveLen <= 0) {
        free(live); free(slotBlob); serverSimDestroy(sim);
        UT_FAIL("join blob %d bytes, live blob %d bytes", joinLen, liveLen);
    }

    /* Decode it: the pill sits on the square the round started on, while the
     * live pill sits on the new one. */
    {
        map rtMap;
        pillboxes rtPb;
        bases rtBs;
        starts rtSs;
        BYTE gotX, gotY;
        bool ok;

        mapCreate(&rtMap);
        pillsCreate(&rtPb);
        basesCreate(&rtBs);
        startsCreate(&rtSs);
        ok = mapLoadCompressedMap(&rtMap, &rtPb, &rtBs, &rtSs, slotBlob, joinLen);
        gotX = ok ? (*rtPb).item[0].x : (BYTE)0;
        gotY = ok ? (*rtPb).item[0].y : (BYTE)0;
        mapDestroy(&rtMap);
        pillsDestroy(&rtPb);
        basesDestroy(&rtBs);
        startsDestroy(&rtSs);

        if (!ok) {
            free(live); free(slotBlob); serverSimDestroy(sim);
            UT_FAIL("mapLoadCompressedMap rejected the join blob");
        }
        if (gotX != rsX || gotY != rsY) {
            free(live); free(slotBlob); serverSimDestroy(sim);
            UT_FAIL("the join blob puts pill 0 at %u,%u, the round-start square "
                    "is %u,%u", (unsigned)gotX, (unsigned)gotY,
                    (unsigned)rsX, (unsigned)rsY);
        }
        if (gotX == ms_pill_live_x(sim, 0) && gotY == ms_pill_live_y(sim, 0)) {
            free(live); free(slotBlob); serverSimDestroy(sim);
            UT_FAIL("the join blob carries the live square %u,%u — the seed "
                    "changed nothing", (unsigned)gotX, (unsigned)gotY);
        }
    }

    /* Slot 0 was not re-seeded, so its blob is still the live one byte for
     * byte: only the seeded slot moved. */
    {
        int slot0Len = serverSimGetCompressedMapFor(sim, 0, slotBlob, 131072);
        if (slot0Len != liveLen ||
            memcmp(live, slotBlob, (size_t)liveLen) != 0) {
            free(live); free(slotBlob); serverSimDestroy(sim);
            UT_FAIL("slot 0's blob (%d bytes) is no longer the live blob "
                    "(%d bytes)", slot0Len, liveLen);
        }
    }

    free(live);
    free(slotBlob);
    serverSimDestroy(sim);
    return 0;
}

/* 14. The ordering rail. On a full-sync tick the header's checksum is stamped
 *     over the recipient's records, and the pill entries and pill events in
 *     that same snapshot rebuild the list the checksum was taken over — applied
 *     in the client's own order, pill snapshots first and then the reliable
 *     events. The pill is stood on a square the checksum folds, so which list
 *     the hash is taken over actually changes the answer. */
int run_pill_shadow_fullsync_move_matches(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1, "the map carries no pills");

    BYTE fx = 0, fy = 0;
    UT_ASSERT_MSG(ms_find_fold_square(sim, &fx, &fy),
                  "the map holds no river/sea/building square clear of bases "
                  "and pills, so the fold would never fire");

    ms_move_pill(sim, 0, fx, fy);
    UT_ASSERT_MSG(ms_pill_live_x(sim, 0) == fx && ms_pill_live_y(sim, 0) == fy,
                  "pill 0 is at %u,%u, not on the chosen square %u,%u",
                  (unsigned)ms_pill_live_x(sim, 0),
                  (unsigned)ms_pill_live_y(sim, 0), (unsigned)fx, (unsigned)fy);
    UT_ASSERT_MSG(ms_pill_fold_terrain(ms_live(sim, fx, fy)),
                  "square %u,%u holds terrain %u, which the checksum does not "
                  "fold — the frame proves nothing", (unsigned)fx, (unsigned)fy,
                  (unsigned)ms_live(sim, fx, fy));

    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));
    serverSimBuildSnapshot(sim, 0, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    UT_ASSERT_MSG(hdr.mapChecksum != 0,
                  "the build stamped no checksum — it was not a full sync");
    UT_ASSERT_MSG(hdr.pillCount > 0,
                  "the full sync carried no pill entries");

    /* Rebuild the list a client would be holding when it checks the checksum,
     * in the order client_snapshot.c applies it. */
    struct pillsObj scratch;
    pillboxes scratchPb = &scratch;
    int i;

    memset(&scratch, 0, sizeof(scratch));
    scratch.numPills = hdr.pillCount;
    for (i = 0; i < hdr.pillCount && i < MAX_PILLS; i++) {
        scratch.item[i].x      = po[i].x;
        scratch.item[i].y      = po[i].y;
        scratch.item[i].owner  = po[i].owner;
        scratch.item[i].armour = pillArmourFromByte(po[i].armourInTank);
        scratch.item[i].inTank = pillInTankFromByte(po[i].armourInTank) ? TRUE : FALSE;
    }
    for (i = 0; i < hdr.reliableEventCount; i++) {
        BYTE idx;
        if (ev[i].type != EVENT_PILL_UPDATE) continue;
        idx = ev[i].data[0];
        if (idx >= MAX_PILLS) continue;
        scratch.item[idx].x      = ev[i].data[1];
        scratch.item[idx].y      = ev[i].data[2];
        scratch.item[idx].owner  = ev[i].data[3];
        scratch.item[idx].armour = pillArmourFromByte(ev[i].data[4]);
        scratch.item[idx].inTank = pillInTankFromByte(ev[i].data[4]) ? TRUE : FALSE;
    }

    UT_ASSERT_MSG(scratch.item[0].x == fx && scratch.item[0].y == fy,
                  "the snapshot leaves the client holding pill 0 at %u,%u, not "
                  "on the folded square %u,%u", (unsigned)scratch.item[0].x,
                  (unsigned)scratch.item[0].y, (unsigned)fx, (unsigned)fy);

    uint16_t clientSum = mapCalcChecksum(&sim->clientKnownMap[0], &gs->bs,
                                         &scratchPb);
    UT_ASSERT_MSG(clientSum == hdr.mapChecksum,
                  "the list rebuilt from the snapshot checksums %04x, the "
                  "header carries %04x", clientSum, hdr.mapChecksum);

    serverSimDestroy(sim);
    return 0;
}
