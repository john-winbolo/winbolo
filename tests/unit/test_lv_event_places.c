/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The map place of an Events panel line, in the Log Viewer's decoder.
 *
 * A click on the selected line puts the line's place in the middle of the
 * map, and the marker over the seek bar takes the same place. Two kinds of
 * line came out with none:
 *
 *   lv_event_pill_capture_place - a pillbox is captured or stolen by picking
 *                                 it up: it is in the tank when the owner
 *                                 changes, and the line takes the square it
 *                                 was picked up from
 *   lv_event_builder_lost_place - log_LostMan comes in the middle of a tick,
 *                                 after the tick's wipe of every builder's
 *                                 place: the line takes the square the
 *                                 builder was on in the tick before
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
#include "lv_players.h"
#include "lv_event_kind.h"
#include "test_harness.h"

/* The last line the decoder made. */
typedef struct {
    int count;
    int kind;
    int mapX;
    int mapY;
} PlaceSeen;

static void place_seen_event(void *ctx, int kind, int mapX, int mapY, const char *text) {
    PlaceSeen *seen = (PlaceSeen *)ctx;
    (void)text;
    seen->count++;
    seen->kind = kind;
    seen->mapX = mapX;
    seen->mapY = mapY;
}

int run_lv_event_pill_capture_place(void) {
    LogViewerState  *lv = lv_decoderCreate(false);
    LvDecodeObserver obs;
    PlaceSeen        seen;
    pillboxes        pb;
    pillbox          in;
    char             location[1] = "";

    UT_ASSERT(lv != NULL);
    /* The players table is a file static: start it empty, as a load does. */
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);
    lv_screenSetup();
    lv_playersSetPlayer(2, "Two", location, 0, 0, 0, 0, 0, FALSE, 0, NULL, FALSE, FALSE, 0);
    lv_playersSetPlayer(5, "Five", location, 0, 0, 0, 0, 0, FALSE, 0, NULL, FALSE, FALSE, 0);

    lv_pillsCreate(&pb);
    lv_pillsSetNumPills(&pb, 2);
    memset(&in, 0, sizeof(in));
    in.x = 60; in.y = 70; in.owner = NEUTRAL; in.armour = 0; in.speed = 50;
    in.inTank = FALSE;
    lv_pillsSetPill(&pb, &in, 1);

    memset(&seen, 0, sizeof(seen));
    memset(&obs, 0, sizeof(obs));
    obs.event = place_seen_event;
    obs.ctx = &seen;
    lv_screenSetObserver(&obs);

    /* A neutral pillbox picked up: the game writes log_PillSetInTank, then
       log_PillSetOwner. */
    lv_pillsSetInTank(&pb, 0, TRUE);
    lv_pillsSetPillOwner(&pb, 0, 2, FALSE);
    UT_ASSERT_MSG(seen.count == 1 && seen.kind == LV_EVK_PILL_CAPTURED,
                  "capture: %d lines, kind %d", seen.count, seen.kind);
    UT_ASSERT_MSG(seen.mapX == 60 && seen.mapY == 70, "capture place %d,%d",
                  seen.mapX, seen.mapY);

    /* Put down again at another square, then picked up by an enemy. */
    lv_pillsSetInTank(&pb, 0, FALSE);
    lv_pillsSetPos(&pb, 0, 80, 90);
    lv_pillsSetInTank(&pb, 0, TRUE);
    lv_pillsSetPillOwner(&pb, 0, 5, FALSE);
    UT_ASSERT_MSG(seen.count == 2 && seen.kind == LV_EVK_PILL_STOLEN,
                  "steal: %d lines, kind %d", seen.count, seen.kind);
    UT_ASSERT_MSG(seen.mapX == 80 && seen.mapY == 90, "steal place %d,%d",
                  seen.mapX, seen.mapY);

    /* A pillbox that was never on the map has no place. */
    memset(&in, 0, sizeof(in));
    in.owner = NEUTRAL;
    in.inTank = TRUE;
    lv_pillsSetPill(&pb, &in, 2);
    lv_pillsSetPillOwner(&pb, 1, 2, FALSE);
    UT_ASSERT_MSG(seen.count == 3 && seen.mapX == -1 && seen.mapY == -1,
                  "no place: %d lines, %d,%d", seen.count, seen.mapX, seen.mapY);

    lv_screenSetObserver(NULL);
    lv_pillsDestroy(&pb);
    lv_decoderDestroy(lv);
    return 0;
}

int run_lv_event_builder_lost_place(void) {
    LogViewerState *lv = lv_decoderCreate(false);
    char            location[1] = "";
    BYTE            mx = 0, my = 0, px = 0, py = 0, frame = 0;

    UT_ASSERT(lv != NULL);
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);
    lv_screenSetup();
    lv_playersSetPlayer(3, "Three", location, 0, 0, 0, 0, 0, FALSE, 0, NULL, FALSE, FALSE, 0);

    /* No trip yet. */
    lv_playersGetLgmLastPlace(3, &mx, &my);
    UT_ASSERT_MSG(mx == 0 && my == 0, "no trip: %u,%u", mx, my);

    /* Tick T-1: the builder is out at (40,50). */
    lv_playersLgmZero();
    lv_playersUpdateLgm(3, 40, 50, 2, 3, 0);
    /* Tick T: the wipe, then log_LostMan before the tick's log_LgmLocation.
       His place now is (0,0); the last place is where he died. */
    lv_playersLgmZero();
    lv_playersGetLgmDetails(3, &mx, &my, &px, &py, &frame);
    UT_ASSERT_MSG(mx == 0 && my == 0, "the wipe left %u,%u", mx, my);
    lv_playersGetLgmLastPlace(3, &mx, &my);
    UT_ASSERT_MSG(mx == 40 && my == 50, "death place %u,%u", mx, my);
    lv_playersSetLgmDead(3);

    /* The helicopter that brings him back is named in this tick and later
       ones: it does not move the last place. */
    lv_playersUpdateLgm(3, 70, 80, 0, 0, LGM_HELICOPTER_FRAME);
    lv_playersGetLgmLastPlace(3, &mx, &my);
    UT_ASSERT_MSG(mx == 40 && my == 50, "helicopter moved it to %u,%u", mx, my);

    /* A tick that does not name him (he is in his tank), then one more
       wipe: the old trip's place is gone. */
    lv_playersLgmZero();
    lv_playersLgmZero();
    lv_playersGetLgmLastPlace(3, &mx, &my);
    UT_ASSERT_MSG(mx == 0 && my == 0, "an old trip kept %u,%u", mx, my);

    /* A slot not in use has no place. */
    lv_playersGetLgmLastPlace(9, &mx, &my);
    UT_ASSERT_MSG(mx == 0 && my == 0, "empty slot: %u,%u", mx, my);

    lv_decoderDestroy(lv);
    return 0;
}
