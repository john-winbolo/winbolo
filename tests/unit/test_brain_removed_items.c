/*
 * test_brain_removed_items.c - what a brain is told about a removed pill or
 * base.
 *
 * game.remove_pill and game.remove_base take an item off the map for good.
 * The slot stays, so the numbers above it do not move, and the item drops
 * out of the brain's object list. A brain that keeps its own list of items it
 * saw earlier cannot tell that apart from an item that went out of sight, so
 * BrainInfo carries pills_on_map / bases_on_map: bit n set means item n is
 * still on the map.
 *
 * The first case drives a real server and client over the loopback harness,
 * so the client learns of the removal the way a bot's client does, over the
 * wire. The second records a few ticks either side of a removal with the
 * -brain-debug recorder and walks the file back.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_scenario.h"
#include "game_sim.h"
#include "client_sim.h"
#include "pillbox.h"
#include "bases.h"
#include "brain.h"
#include "brain_data.h"
#include "brain_record.h"
#include "brain_record_walk.h"
#include "loopback_harness.h"
#include "test_harness.h"

/* brainDataMakeInfo allocates the variable-length members; free them the way
 * the headless loggers do. */
static void brFreeInfo(BrainInfo *bi) {
    free(bi->allies);
    free(bi->player_bots);
    free(bi->base);
    free(bi->pillview);
    free(bi->viewdata);
    free(bi->events);
    if (bi->message != NULL) {
        free(bi->message->receivers);
        free(bi->message->message);
        free(bi->message);
    }
}

typedef struct {
    BYTE pillNum;   /* 1 based */
    BYTE baseNum;   /* 1 based */
} BrRemovedWatch;

static bool brClientSawRemoval(LoopbackHarness *h, void *user) {
    BrRemovedWatch *w = (BrRemovedWatch *)user;
    GameSim *gs = clientSimGetGameSim(h->cs);
    return gs != NULL &&
           pillsIsActive(&gs->pb, w->pillNum) == FALSE &&
           basesIsActive(&gs->bs, w->baseNum) == FALSE;
}

int run_brain_removed_items_info_masks(void) {
    LoopbackHarness h;
    BrainInfo bi;
    GameSim *sgs;
    GameSim *cgs;
    ScenarioOp op;
    BrRemovedWatch w;
    BYTE numPills, numBases;
    uint32_t allPills, allBases;
    unsigned short i;
    BYTE k;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Removed", false, NULL, 2468),
                  "loopback start failed");
    loopbackHarnessPumpUntil(&h, 40, NULL, NULL);
    UT_ASSERT_MSG(h.cs != NULL && h.sim != NULL, "the harness produced no client");

    sgs = serverSimGetGameSim(h.sim);
    cgs = clientSimGetGameSim(h.cs);
    UT_ASSERT(sgs != NULL && cgs != NULL);
    numPills = pillsGetNumPills(&cgs->pb);
    numBases = basesGetNumBases(&cgs->bs);
    UT_ASSERT_MSG(numPills >= 2 && numBases >= 2,
                  "the harness map has %u pills and %u bases; the case needs two of each",
                  (unsigned)numPills, (unsigned)numBases);

    allPills = 0;
    for (k = 0; k < numPills; k++) {
        allPills |= ((uint32_t)1u << k);
    }
    allBases = 0;
    for (k = 0; k < numBases; k++) {
        allBases |= ((uint32_t)1u << k);
    }

    /* Before: every number is on the map. */
    memset(&bi, 0, sizeof(bi));
    brainDataMakeInfo(h.cs, &bi, true, aiNone);
    UT_ASSERT_MSG(bi.pills_on_map == allPills,
                  "before any removal pills_on_map is 0x%lx, wanted 0x%lx",
                  (unsigned long)bi.pills_on_map, (unsigned long)allPills);
    UT_ASSERT_MSG(bi.bases_on_map == allBases,
                  "before any removal bases_on_map is 0x%lx, wanted 0x%lx",
                  (unsigned long)bi.bases_on_map, (unsigned long)allBases);
    brFreeInfo(&bi);

    /* Remove the second pill and the second base, on the server. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_REMOVE_PILL;
    op.u.entityRemovePill.pill = 1;
    UT_ASSERT(serverSimApplyScenarioOp(h.sim, &op, NULL) == SCN_OP_OK);
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_REMOVE_BASE;
    op.u.entityRemoveBase.base = 1;
    UT_ASSERT(serverSimApplyScenarioOp(h.sim, &op, NULL) == SCN_OP_OK);

    w.pillNum = 2;
    w.baseNum = 2;
    loopbackHarnessPumpUntil(&h, 60, brClientSawRemoval, &w);
    UT_ASSERT_MSG(brClientSawRemoval(&h, &w),
                  "the client never learned of the removal");

    memset(&bi, 0, sizeof(bi));
    brainDataMakeInfo(h.cs, &bi, false, aiNone);
    UT_ASSERT_MSG(bi.pills_on_map == (allPills & ~(uint32_t)2u),
                  "after removing pill 1 pills_on_map is 0x%lx, wanted 0x%lx",
                  (unsigned long)bi.pills_on_map,
                  (unsigned long)(allPills & ~(uint32_t)2u));
    UT_ASSERT_MSG(bi.bases_on_map == (allBases & ~(uint32_t)2u),
                  "after removing base 1 bases_on_map is 0x%lx, wanted 0x%lx",
                  (unsigned long)bi.bases_on_map,
                  (unsigned long)(allBases & ~(uint32_t)2u));
    /* The removed items are not in the object list either. */
    for (i = 0; i < bi.num_objects; i++) {
        UT_ASSERT_MSG(!(bi.objects[i].object == PILLS_BRAIN_OBJECT_TYPE &&
                        bi.objects[i].idnum == 1),
                      "the removed pill is still in the brain's object list");
        UT_ASSERT_MSG(!(bi.objects[i].object == BASES_BRAIN_OBJECT_TYPE &&
                        bi.objects[i].idnum == 1),
                      "the removed base is still in the brain's object list");
    }
    brFreeInfo(&bi);

    loopbackHarnessStop(&h);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* The -brain-debug recording (brainrec.btr) carries the same two masks at   */
/* the end of each frame (format v7), so BrainTest can hide a removed pill   */
/* or base from the frame it went on. Before v7 the recorder wrote every     */
/* slot with nothing to say it was gone, and a replay drew removed items for */
/* the rest of the round.                                                    */
/* ------------------------------------------------------------------------ */

static size_t brGzRead(void *ctx, void *dst, size_t len) {
    int got = gzread((gzFile)ctx, dst, (unsigned)len);
    return got > 0 ? (size_t)got : 0;
}

static bool brGzSkip(void *ctx, size_t len) {
    return gzseek((gzFile)ctx, (z_off_t)len, SEEK_CUR) >= 0;
}

int run_brain_removed_items_recording_masks(void) {
    ServerSim *sim;
    ScenarioOp op;
    GameSim *sgs;
    char dir[512];
    char path[600];
    BYTE numPills, numBases;
    uint32_t allPills, allBases;
    uint32_t lastBefore;
    BYTE k;
    int n;

    UT_ASSERT_MSG(utScratchPath(dir, sizeof(dir), NULL), "no scratch dir");

    /* A bare sim ticked by hand. The loopback harness runs the server's
     * lifecycle, which opens its own debug_sessions/ block as soon as the
     * recorder is on. */
    sim = ut_make_running_sim("Recorded");
    UT_ASSERT(sim != NULL);
    sgs = serverSimGetGameSim(sim);
    UT_ASSERT(sgs != NULL);
    numPills = pillsGetNumPills(&sgs->pb);
    numBases = basesGetNumBases(&sgs->bs);
    UT_ASSERT_MSG(numPills >= 2 && numBases >= 2,
                  "the harness map has %u pills and %u bases; the case needs two of each",
                  (unsigned)numPills, (unsigned)numBases);
    allPills = 0;
    for (k = 0; k < numPills; k++) {
        allPills |= ((uint32_t)1u << k);
    }
    allBases = 0;
    for (k = 0; k < numBases; k++) {
        allBases |= ((uint32_t)1u << k);
    }

    /* Record a few ticks, remove pill index 1 and base index 1, record a few
     * more. */
    brainRecordSetSessionDir(dir);
    brainRecordSetEnabled(true);
    for (n = 0; n < 5; n++) {
        serverSimTick(sim);
        brainRecordTick(sim);
    }
    /* Every frame after this tick is recorded after the removal. */
    lastBefore = serverSimGetTick(sim);
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_REMOVE_PILL;
    op.u.entityRemovePill.pill = 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_REMOVE_BASE;
    op.u.entityRemoveBase.base = 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    for (n = 0; n < 5; n++) {
        serverSimTick(sim);
        brainRecordTick(sim);
    }
    brainRecordEndGame();
    brainRecordSetEnabled(false);
    brainRecordSetSessionDir(NULL);
    serverSimDestroy(sim);

    /* Walk the file back. */
    snprintf(path, sizeof(path), "%s/%s", dir, BRAINREC_FILENAME);
    {
        gzFile g = gzopen(path, "rb");
        BrainRecReader r;
        BrainRecFrameInfo fi;
        BrainRecWalkStatus st;
        int before = 0;
        int after = 0;

        UT_ASSERT_MSG(g != NULL, "the recorder wrote no %s", path);
        memset(&r, 0, sizeof(r));
        r.ctx = g;
        r.read = brGzRead;
        r.skip = brGzSkip;
        if (brainRecWalkPreamble(&r, NULL, NULL) != BRAINREC_WALK_OK) {
            gzclose(g);
            UT_ASSERT_MSG(false, "the recording's header did not read");
        }
        if (r.version != BRAINREC_VERSION) {
            gzclose(g);
            UT_ASSERT_MSG(false, "the recording is version %u, not %u",
                          (unsigned)r.version, (unsigned)BRAINREC_VERSION);
        }
        while ((st = brainRecWalkFrame(&r, &fi)) == BRAINREC_WALK_OK) {
            uint32_t wantP = (fi.tick > lastBefore) ? (allPills & ~(uint32_t)2u) : allPills;
            uint32_t wantB = (fi.tick > lastBefore) ? (allBases & ~(uint32_t)2u) : allBases;
            if (fi.pillsOnMap != wantP || fi.basesOnMap != wantB) {
                gzclose(g);
                UT_ASSERT_MSG(false,
                              "frame at tick %lu has pills 0x%lx bases 0x%lx, "
                              "wanted 0x%lx and 0x%lx (removal after tick %lu)",
                              (unsigned long)fi.tick, (unsigned long)fi.pillsOnMap,
                              (unsigned long)fi.basesOnMap, (unsigned long)wantP,
                              (unsigned long)wantB, (unsigned long)lastBefore);
            }
            if (fi.tick > lastBefore) {
                after++;
            } else {
                before++;
            }
        }
        gzclose(g);
        UT_ASSERT_MSG(st == BRAINREC_WALK_EOF, "the recording ended mid-frame");
        UT_ASSERT_MSG(before > 0 && after > 0,
                      "the recording has %d frames before the removal and %d after",
                      before, after);
    }
    return 0;
}
