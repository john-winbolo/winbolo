/*
 * specSeedDecodeInfo unit test — recover the lobby game-info from a real
 * spectator seed's control snapshot.
 *
 * Mirrors test_spectator_seed_capture.c's production setup: a running ServerSim
 * (map "Everard Island", gameOpen, no hidden mines, no AI) with a registered
 * spectator ring, no logStart and no .wbv file. The captured ring keyframe is
 * the exact [u32 bodyLen][world body][u32 ctrlLen][control snapshot] blob the
 * server sends a connecting spectator; the snapshot carries the sync-replay
 * lobby-settings event a normal joiner would receive. specSeedDecodeInfo
 * (client_sim.c, dep-free seam) must recover the map name and settings from it.
 *
 * Asserts the positive decode against the sim's known map/settings, and the
 * negative path: a seed whose control snapshot holds no lobby-settings event
 * (the same world body, an empty control slice) -> returns false, output zeroed.
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
#include "spectator_drain.h"  /* specSeedDecodeInfo + SpecSeedInfo (dep-free) */
#include "test_harness.h"

#define SDI_SLOT     0
#define SDI_CADENCE  8       /* keyframe every 8 game ticks within a segment */

static void sdi_feed(ServerSim *sim, uint32_t tick, uint8_t buttons) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = SDI_SLOT;
    pkt.buttons   = buttons;
    serverSimApplyInput(sim, &pkt);
}

/* One 20ms frame: two fresh inputs (keeps the input stream established) then a
   tick, exactly the cadence the seed-capture fixture generator uses. */
static void sdi_tick(ServerSim *sim, uint32_t *it, uint8_t buttons) {
    sdi_feed(sim, (*it)++, buttons);
    sdi_feed(sim, (*it)++, buttons);
    serverSimTick(sim);
}

int run_spec_seed_decode_info(void) {
    ServerSim *sim;
    SpectatorRing *ring;
    SpectatorRingCursor cur;
    const uint8_t *kf = NULL;
    int kfLen = 0;
    uint32_t it = 1;
    int i;
    SpecSeedInfo info;
    bool ok;

    sim = ut_make_running_sim("Driver");   /* map "Everard Island", gameOpen */
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");

    /* Register a ring on a normal (non-recording) server: it records every tick
       with no logStart and no .wbv file. */
    logCreate();
    ring = spectatorRingCreate(SDI_CADENCE, 100000);
    UT_ASSERT_MSG(ring != NULL, "spectatorRingCreate failed");
    logSetSpectatorRing(ring, sim);

    /* Tick until a delay-0 seek returns a keyframe seed (the blob a connecting
       spectator receives). Every keyframe's control snapshot carries the
       lobby-settings event, so the first available one suffices. */
    for (i = 0; i < 400; i++) {
        sdi_tick(sim, &it, 0);
        if (spectatorRingSeekDelayed(ring, 0, &cur) == SPECTATOR_RING_OK) {
            kf = spectatorRingCursorKeyframe(&cur, &kfLen, NULL);
            if (kf != NULL && kfLen > 8) {
                break;
            }
        }
    }
    UT_ASSERT_MSG(kf != NULL && kfLen > 8,
                  "no spectator seed keyframe recorded (kfLen %d)", kfLen);

    /* Positive: the control snapshot's lobby-settings event yields the sim's
       real map name and game settings. */
    memset(&info, 0, sizeof(info));
    ok = specSeedDecodeInfo(kf, (size_t)kfLen, &info);
    UT_ASSERT_MSG(ok && info.haveInfo,
                  "specSeedDecodeInfo found no lobby-settings in a real seed");
    UT_ASSERT_MSG(strcmp(info.mapName, "Everard Island") == 0,
                  "decoded map name '%s' != 'Everard Island'", info.mapName);
    UT_ASSERT_MSG(info.gameType == (uint8_t)gameOpen,
                  "decoded gameType %u != gameOpen", (unsigned)info.gameType);
    UT_ASSERT_MSG(info.allowHiddenMines == 0,
                  "decoded allowHiddenMines %u, expected 0",
                  (unsigned)info.allowHiddenMines);
    UT_ASSERT_MSG(info.ai == 0,    /* aiNone */
                  "decoded ai %u, expected 0 (aiNone)", (unsigned)info.ai);

    /* Negative: rebuild the seed with the same world body but an EMPTY control
       snapshot (ctrlLen 0) — no lobby-settings event present. Decode must fail
       and zero the output even though it was poisoned beforehand. */
    {
        uint32_t bodyLen = ((uint32_t)kf[0] << 24) | ((uint32_t)kf[1] << 16) |
                           ((uint32_t)kf[2] << 8) | (uint32_t)kf[3];
        size_t noCtrlLen;
        uint8_t *noCtrl;
        SpecSeedInfo neg;
        bool negOk;

        UT_ASSERT_MSG((size_t)bodyLen + 8 <= (size_t)kfLen,
                      "fixture framing: bodyLen %u overruns kfLen %d",
                      (unsigned)bodyLen, kfLen);
        noCtrlLen = (size_t)8 + bodyLen;   /* [u32 bodyLen][body][u32 ctrlLen=0] */
        noCtrl = (uint8_t *)malloc(noCtrlLen);
        UT_ASSERT_MSG(noCtrl != NULL, "alloc failed");
        memcpy(noCtrl, kf, (size_t)4 + bodyLen);   /* bodyLen header + world body */
        noCtrl[4 + bodyLen + 0] = 0;               /* ctrlLen = 0 */
        noCtrl[4 + bodyLen + 1] = 0;
        noCtrl[4 + bodyLen + 2] = 0;
        noCtrl[4 + bodyLen + 3] = 0;

        memset(&neg, 0xAB, sizeof(neg));   /* poison: decode must zero it */
        negOk = specSeedDecodeInfo(noCtrl, noCtrlLen, &neg);
        free(noCtrl);
        UT_ASSERT_MSG(!negOk && !neg.haveInfo,
                      "specSeedDecodeInfo must fail when no lobby-settings event "
                      "is present");
    }

    logSetSpectatorRing(NULL, NULL);
    spectatorRingDestroy(ring);
    logDestroy();
    serverSimDestroy(sim);
    return 0;
}
