/*
 * On-demand generator for the committed spectator ring-seed fixture. Not part
 * of the fast suite: it brings up a ServerSim with a real in-use player, drives
 * game ticks so the spectator ring records a keyframe (no logStart, no .wbv),
 * reads the delayed cursor's keyframe blob — the exact bytes the server sends a
 * connecting spectator — and writes them verbatim to
 * <WB_WBV_FIXTURE_DIR>/spectator_seed.bin.
 *
 * The blob is a raw ring keyframe: [u32 bodyLen BE][world body][u32 ctrlLen BE]
 * [control snapshot], with NO trailing data — exactly what lv_specSeedLoad
 * parses. (The .wbv-derived seed in test_spec_seed_load carries trailing event
 * records that mask an over-read; this fixture ends precisely at the snapshot.)
 *
 * Regenerate with, from the repo root:
 *
 *   WB_WBV_FIXTURE_DIR=tests/fixtures/wbv \
 *       ./WinBoloUnitTests --test spectator_seed_capture
 *
 * then commit the resulting tests/fixtures/wbv/spectator_seed.bin. Dispatch-only
 * (like wbv_v2_capture): it writes a fixture, so it is not a CTest case.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "log.h"
#include "log_internal.h"     /* logSetSpectatorRing */
#include "spectator_ring.h"
#include "test_harness.h"

#ifndef WB_WBV_FIXTURE_DIR
#define WB_WBV_FIXTURE_DIR "tests/fixtures/wbv"
#endif

#define SSC_SLOT     0
#define SSC_CADENCE  8       /* keyframe every 8 game ticks within a segment */

static void ssc_feed(ServerSim *sim, uint32_t tick, uint8_t buttons) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = SSC_SLOT;
    pkt.buttons   = buttons;
    serverSimApplyInput(sim, &pkt);
}

/* One 20ms frame: two fresh inputs (keeps the input stream established) then a
   tick. buttons drive the tank for a real, moved in-use player record. */
static void ssc_tick(ServerSim *sim, uint32_t *it, uint8_t buttons) {
    ssc_feed(sim, (*it)++, buttons);
    ssc_feed(sim, (*it)++, buttons);
    serverSimTick(sim);
}

int run_spectator_seed_capture(void) {
    const char *dir = getenv("WB_WBV_FIXTURE_DIR");
    char path[512];
    ServerSim *sim;
    SpectatorRing *ring;
    SpectatorRingCursor cur;
    const uint8_t *kf = NULL;
    int kfLen = 0;
    uint32_t it = 1;
    int i;
    uint32_t bodyLen, ctrlLen;
    FILE *f;

    if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;
    snprintf(path, sizeof(path), "%s/spectator_seed.bin", dir);

    sim = ut_make_running_sim("Driver");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");

    /* Register a ring on a normal (non-recording) server: it records every tick
       with no logStart and no .wbv file (the just-landed ring fix). */
    logCreate();
    ring = spectatorRingCreate(SSC_CADENCE, 100000);
    UT_ASSERT_MSG(ring != NULL, "spectatorRingCreate failed");
    logSetSpectatorRing(ring, sim);

    /* Drive real motion so the captured world has a moved, in-use player tank. */
    for (i = 0; i < 16; i++) {
        ssc_tick(sim, &it, (uint8_t)(INPUT_BTN_ACCEL | INPUT_BTN_LEFT));
    }
    /* Then idle until the delayed cursor's seed keyframe is the head keyframe,
       so the captured blob reflects the post-motion world. Any keyframe is a
       valid ring seed; the head one is simply the freshest. */
    for (i = 0; i < 400; i++) {
        ssc_tick(sim, &it, 0);
        if (spectatorRingSeekDelayed(ring, 0, &cur) == SPECTATOR_RING_OK &&
            cur.seedIdx == cur.targetIdx) {
            kf = spectatorRingCursorKeyframe(&cur, &kfLen, NULL);
            if (kf != NULL && kfLen >= 8) {
                break;
            }
        }
    }
    UT_ASSERT_MSG(kf != NULL && kfLen >= 8,
                  "no head keyframe recorded after motion + idle ticks");

    /* Framing self-consistency: 8 + bodyLen + ctrlLen == kfLen, no trailing
       data — the exact shape lv_specSeedLoad parses. (Read before any further
       ring mutation; the cursor pointer stays valid until then.) */
    bodyLen = ((uint32_t) kf[0] << 24) | ((uint32_t) kf[1] << 16) |
              ((uint32_t) kf[2] << 8) | (uint32_t) kf[3];
    UT_ASSERT_MSG((size_t) bodyLen + 8 <= (size_t) kfLen,
                  "bodyLen=%u overruns kfLen=%d", (unsigned) bodyLen, kfLen);
    ctrlLen = ((uint32_t) kf[4 + bodyLen] << 24) |
              ((uint32_t) kf[5 + bodyLen] << 16) |
              ((uint32_t) kf[6 + bodyLen] << 8) | (uint32_t) kf[7 + bodyLen];
    UT_ASSERT_MSG((size_t) 8 + bodyLen + ctrlLen == (size_t) kfLen,
                  "framing inconsistent: 8 + bodyLen(%u) + ctrlLen(%u) != kfLen(%d)",
                  (unsigned) bodyLen, (unsigned) ctrlLen, kfLen);

    /* The bug is in the player-record parse, so a world with no in-use player
       would not reproduce it. */
    UT_ASSERT_MSG(serverSimIsPlayerConnected(sim, SSC_SLOT),
                  "captured world has no in-use player in slot 0");

    fprintf(stderr, "spectator_seed_capture: kfLen=%d bodyLen=%u ctrlLen=%u\n",
            kfLen, (unsigned) bodyLen, (unsigned) ctrlLen);

    /* Only write the fixture when explicitly asked (env set), like the v2
       capture — keeps the dispatch run side-effect-free otherwise. */
    if (getenv("WB_WBV_FIXTURE_DIR") != NULL) {
        remove(path);
        f = fopen(path, "wb");
        UT_ASSERT_MSG(f != NULL, "cannot open fixture for write: %s", path);
        UT_ASSERT_MSG(fwrite(kf, 1, (size_t) kfLen, f) == (size_t) kfLen,
                      "short write to %s", path);
        fclose(f);
        fprintf(stderr, "spectator_seed_capture: wrote %d bytes -> %s\n",
                kfLen, path);
    } else {
        fprintf(stderr, "spectator_seed_capture: WB_WBV_FIXTURE_DIR unset — not "
                        "writing (set it to regenerate %s)\n", path);
    }

    logSetSpectatorRing(NULL, NULL);
    spectatorRingDestroy(ring);
    logDestroy();
    serverSimDestroy(sim);
    return 0;
}
