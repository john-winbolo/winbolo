/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A pillbox a tank carries, in the Log Viewer's world.
 *
 * The decoder keeps a carried pillbox in its slot with the place it was
 * picked up at, marked inTank. lv_pillsGetPill has to hand that mark out:
 * the single-player map (lv_spmap.c) and the text dump (lv_dump.c) read it
 * to tell a pillbox on the map from one in a tank.
 *
 *   lv_pill_get_in_tank            - the getter copies the mark, both ways
 *   lv_spmap_carried_pill_left_out - a recorded round with one pillbox
 *                                    marked carried: the map written for
 *                                    single player has one pillbox fewer
 *
 * Viewer-world: no sim header here (the two worlds disagree on the types).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_pillbox.h"
#include "lv_spmap.h"
#include "replay_harness.h"
#include "test_harness.h"

int run_lv_pill_get_in_tank(void) {
    pillboxes pb;
    pillbox   in, out;

    lv_pillsCreate(&pb);
    lv_pillsSetNumPills(&pb, 2);

    memset(&in, 0, sizeof(in));
    in.x = 60; in.y = 70; in.owner = 3; in.armour = 15; in.speed = 50;
    in.inTank = FALSE;
    lv_pillsSetPill(&pb, &in, 1);
    in.x = 61; in.y = 71; in.owner = 4;
    in.inTank = TRUE;
    lv_pillsSetPill(&pb, &in, 2);

    /* On the map: the getter must clear a mark left in the caller's struct. */
    memset(&out, 0, sizeof(out));
    out.inTank = TRUE;
    lv_pillsGetPill(&pb, &out, 1);
    UT_ASSERT_MSG(out.x == 60 && out.y == 70 && out.owner == 3,
                  "pillbox 1 read as %u,%u owner %u", out.x, out.y, out.owner);
    UT_ASSERT_MSG(out.inTank == FALSE, "a pillbox on the map reads as carried");

    /* Carried. */
    memset(&out, 0, sizeof(out));
    lv_pillsGetPill(&pb, &out, 2);
    UT_ASSERT_MSG(out.x == 61 && out.y == 71 && out.owner == 4,
                  "pillbox 2 read as %u,%u owner %u", out.x, out.y, out.owner);
    UT_ASSERT_MSG(out.inTank != FALSE, "a carried pillbox reads as on the map");

    /* Picked up and put down again, through the setter a playback record
       uses (it counts from zero). */
    lv_pillsSetInTank(&pb, 0, TRUE);
    lv_pillsGetPill(&pb, &out, 1);
    UT_ASSERT_MSG(out.inTank != FALSE, "pillbox 1 was picked up but reads as on the map");
    lv_pillsSetInTank(&pb, 0, FALSE);
    lv_pillsGetPill(&pb, &out, 1);
    UT_ASSERT_MSG(out.inTank == FALSE, "pillbox 1 was put down but reads as carried");

    lv_pillsDestroy(&pb);
    return 0;
}

int run_lv_spmap_carried_pill_left_out(void) {
    ReplayHarness    h;
    LogViewerState  *lv = NULL;
    LvDecodeObserver quiet;
    LvSpmapInfo      before, after;
    pillbox          first;
    char             logPath[FILENAME_MAX];
    char             mapPath[FILENAME_MAX];
    bool             recorded;
    int              hideLobby;
    int              loaded = 0, wroteBefore = 0, wroteAfter = 0;
    int              n;

    memset(&h, 0, sizeof(h));
    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    memset(&first, 0, sizeof(first));
    recorded = replayHarnessStartRecording(&h, "lv_pill_carried", "Tester");
    if (recorded) {
        replayHarnessTick(&h, 300);
        recorded = replayHarnessStopRecording(&h);
    }
    if (recorded) {
        n = snprintf(mapPath, sizeof(mapPath), "%s.sp.map", h.path);
        (void)n;
        snprintf(logPath, sizeof(logPath), "%s", h.path);

        /* Load the round the way lvSpmapWriteFromLog does, so the world can
           be changed between the load and the write. */
        lv = lv_decoderCreate(false);
        if (lv != NULL) {
            hideLobby = lv_screenGetHideLobby();
            lv_screenSetHideLobby(0);
            memset(&quiet, 0, sizeof(quiet));
            lv_screenSetObserver(&quiet);
            if (lv_screenLoadMap(logPath, 4) == TRUE) {
                loaded = 1;
                lv->fastForwarding = TRUE;
                while (lv->isPlaying == TRUE && lv->timeRunning + 20 <= 3000) {
                    lv_screenLogTick();
                }
                wroteBefore = lvSpmapWrite(mapPath, -1, &before);

                /* A tank picks up pillbox 1. The decoder leaves its place
                   as it was. */
                lv_pillsGetPill(&lv->pb, &first, 1);
                lv_pillsSetInTank(&lv->pb, 0, TRUE);
                wroteAfter = lvSpmapWrite(mapPath, -1, &after);
            }
            lv_screenSetObserver(NULL);
            lv_decoderDestroy(lv);
            lv_screenSetHideLobby(hideLobby);
        }
        remove(mapPath);
    }
    replayHarnessStop(&h);

    UT_ASSERT_MSG(recorded, "the round could not be recorded");
    UT_ASSERT_MSG(lv != NULL && loaded, "the recorded round could not be loaded");
    UT_ASSERT_MSG(wroteBefore && wroteAfter, "the map was not written (%d, %d)",
                  wroteBefore, wroteAfter);
    UT_ASSERT_MSG(before.pills >= 2 && before.pillsCarried == 0,
                  "before the pick-up: %d pillboxes written, %d carried",
                  before.pills, before.pillsCarried);
    UT_ASSERT_MSG(before.firstPillX == first.x && before.firstPillY == first.y,
                  "pillbox 1 at %u,%u was not the first one written (%u,%u)",
                  first.x, first.y, before.firstPillX, before.firstPillY);
    UT_ASSERT_MSG(after.pillsCarried == 1,
                  "the carried pillbox was not counted as left out (%d)",
                  after.pillsCarried);
    UT_ASSERT_MSG(after.pills == before.pills - 1,
                  "%d pillboxes written with one carried; %d without",
                  after.pills, before.pills);
    UT_ASSERT_MSG(after.firstPillX != first.x || after.firstPillY != first.y,
                  "the carried pillbox was written at its pick-up square %u,%u",
                  first.x, first.y);
    return 0;
}
