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
 * viewports and writes the copy as it sends. The last three cases pin the sim
 * side of that — what the tick withholds, and the catch-up sweep that closes
 * the gap once the client can see the ground again. The transport side of the
 * coupling (in-viewport changes queued and written, everything else left owed)
 * is driven end-to-end in test_loopback_map_cull.c.
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

        int liveLen = serverSimGetCompressedMap(sim, live);
        int slotLen = serverSimGetCompressedMapFor(sim, 1, slotBlob);
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
    UT_ASSERT_MSG(serverSimGetCompressedMapFor(sim, (BYTE)MAX_TANKS, slotBlob) == 0,
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
