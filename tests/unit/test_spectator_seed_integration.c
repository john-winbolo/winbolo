/*
 * Headless spectator seed integration — the full no-logStart chain.
 *
 * test_spectator_connect.c reaches tankless SPECTATING but exchanges no seed,
 * and test_spectator_ring_nolog.c proves the server ring seeds without logStart
 * but never crosses the wire. This test joins the two: a running ServerSim with
 * a registered spectator ring (no logStart, no .wbv) actually delivers a seed
 * over the real loopback transport, the client captures it, and the production
 * logviewer decode path loads it.
 *
 * The loopback harness registers the instance spectator ring (the production
 * serverInstanceCreateSpectatorRing call) and drives serverInstanceTick, which
 * ticks the running world so the ring records and the serve-cursor path copies
 * the delayed keyframe onto the spectator's bulk channel. logStart is never
 * called.
 *
 * The decode mirrors spectatorRun exactly: specDrainTakeSeed hands over the
 * captured blob, specSeedDecodeInfo recovers the game-info from its control
 * snapshot, and lv_specSeedLoad loads header + snapshot into a sized decoder.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"          /* lv_specSeedLoad / lv_specSeedControlClear / LvSpecSeedInfo */
#include "logviewer.h"        /* lv_decoderCreate / lv_decoderDestroy */
#include "lv_log.h"
#include "spectator_drain.h"  /* specDrain* / specSeedDecodeInfo — dep-free seam */
#include "test_harness.h"
#include "loopback_harness.h"

/* The handshake plus a delay-0 seed transfer lands well inside this bound; the
 * generous cap keeps a hang distinguishable from a slow pass. */
#define SPEC_SEED_MAX 3000

static bool pred_seed_ready(LoopbackHarness *h, void *user) {
    (void)user;
    return specDrainSeedReady(h->cs);
}

int run_spectator_seed_integration(void) {
    LoopbackHarness h;
    uint8_t *seed = NULL;
    uint32_t seedLen = 0;
    SpecSeedInfo sgi;
    LvSpecSeedInfo info;
    const LvSpecSeedInfo *infoPtr = NULL;
    LogViewerState *lv;
    int readyAt;
    bool loaded;

    UT_ASSERT_MSG(loopbackHarnessStartSpectator(&h, "Spectator", /*seed*/ 1u),
                  "spectator harness start failed");

    /* Tick server + client until the captured seed is fully received. The server
     * has specDelayTicks 0, so the serve cursor copies a keyframe as soon as the
     * ring holds one record; the rest is the bulk transfer round-trips. */
    readyAt = loopbackHarnessPumpUntil(&h, SPEC_SEED_MAX, pred_seed_ready, NULL);
    fprintf(stderr, "  spectator seed: ready after %d pump(s) (cap %d)\n",
            readyAt, SPEC_SEED_MAX);
    if (readyAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never captured a seed within %d pumps", SPEC_SEED_MAX);
    }
    UT_ASSERT_MSG(specDrainSeedReady(h.cs),
                  "specDrainSeedReady must be true once the seed is captured");

    /* Production take: ownership of the blob transfers to us (free it below). */
    UT_ASSERT_MSG(specDrainTakeSeed(h.cs, &seed, &seedLen),
                  "specDrainTakeSeed returned no seed after seedReady");
    UT_ASSERT_MSG(seed != NULL && seedLen > 8,
                  "captured seed too short (len %u)", (unsigned)seedLen);

    /* Decode + load exactly as spectatorRun does: recover the game-info from the
     * seed's control snapshot, then load with that info (NULL when absent). */
    if (specSeedDecodeInfo(seed, seedLen, &sgi) && sgi.haveInfo) {
        memset(&info, 0, sizeof(info));
        info.mapName          = sgi.mapName;
        info.gameType         = sgi.gameType;
        info.allowHiddenMines = sgi.allowHiddenMines;
        info.ai               = sgi.ai;
        infoPtr = &info;
    }

    /* Mirror logViewerRun / spectatorRun: create + size the decoder before load. */
    lv = lv_decoderCreate(false);
    UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);

    loaded = lv_specSeedLoad(infoPtr, seed, seedLen);
    free(seed);
    UT_ASSERT_MSG(loaded == TRUE,
                  "lv_specSeedLoad failed on a live-captured spectator seed");

    lv_specSeedControlClear();
    lv_decoderDestroy(lv);   /* closes the log (frees blocks + screen structures) */
    loopbackHarnessStop(&h);
    return 0;
}
