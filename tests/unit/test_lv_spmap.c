/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The map the Log Viewer writes for "Open map at this time in single
 * player" (src/logviewer/lv_spmap.h, lv_spmap.c).
 *
 * lv_spmap_owner_table checks the pure rules: which owner a recorded owner
 * is written as, and a tank frame as a map start's direction.
 *
 * lv_spmap_reads_back records a round, writes its world twice (no player
 * selected, and the round's one player selected) and reads both files with
 * the game's own mapRead, the reader a single-player game uses. mapRead
 * moves a map so its bounds sit on the middle of the grid, so the test also
 * states how far each file was moved.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "types.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"

#include "lv_spmap.h"
#include "replay_harness.h"
#include "test_harness.h"

int run_lv_spmap_owner_table(void) {
    unsigned char allied[LV_SPMAP_SLOTS];
    unsigned char owns[LV_SPMAP_SLOTS];
    unsigned char t[LV_SPMAP_SLOTS];
    int           i;

    /* Player 3 selected, allied to 5. Slot 0 is an enemy. */
    memset(allied, 0, sizeof(allied));
    memset(owns, 0, sizeof(owns));
    allied[3] = 1;
    allied[5] = 1;
    lvSpmapOwnerTable(3, allied, owns, t);
    UT_ASSERT_MSG(t[3] == 0, "the selected player's items go to slot %u, not 0", t[3]);
    UT_ASSERT_MSG(t[5] == 0, "an ally's items go to slot %u, not 0", t[5]);
    UT_ASSERT_MSG(t[0] == 3, "the enemy in slot 0 goes to slot %u, not the freed slot 3", t[0]);
    for (i = 1; i < LV_SPMAP_SLOTS; i++) {
        if (i != 3 && i != 5) {
            UT_ASSERT_MSG(t[i] == i, "the enemy in slot %d moved to slot %u", i, t[i]);
        }
    }

    /* Player 0 selected: nothing but the allies moves. */
    memset(allied, 0, sizeof(allied));
    allied[0] = 1;
    allied[2] = 1;
    lvSpmapOwnerTable(0, allied, owns, t);
    UT_ASSERT_MSG(t[0] == 0 && t[2] == 0, "slot 0 selected: own and allied are %u and %u",
                  t[0], t[2]);
    UT_ASSERT_MSG(t[1] == 1 && t[15] == 15, "slot 0 selected: an enemy moved");

    /* Player 4 selected, allied to the player in slot 0: slot 0 stays the
       user's and no enemy is put in slot 4's place. */
    memset(allied, 0, sizeof(allied));
    allied[4] = 1;
    allied[0] = 1;
    lvSpmapOwnerTable(4, allied, owns, t);
    UT_ASSERT_MSG(t[0] == 0 && t[4] == 0, "an ally in slot 0 was written as slot %u", t[0]);

    /* Nobody selected: slot 0's items go to the highest slot that owns
       nothing, and every other owner stays. */
    memset(owns, 0, sizeof(owns));
    owns[0]  = 1;
    owns[1]  = 1;
    owns[15] = 1;
    lvSpmapOwnerTable(-1, NULL, owns, t);
    UT_ASSERT_MSG(t[0] == 14, "nobody selected: slot 0 went to slot %u, not 14", t[0]);
    UT_ASSERT_MSG(t[1] == 1 && t[15] == 15, "nobody selected: another owner moved");

    /* Nobody selected and every slot owns something: neutral, not the user's. */
    memset(owns, 1, sizeof(owns));
    lvSpmapOwnerTable(-1, NULL, owns, t);
    UT_ASSERT_MSG(t[0] == LV_SPMAP_NEUTRAL, "no free slot: slot 0 went to %u", t[0]);

    UT_ASSERT_MSG(lvSpmapOwner(t, LV_SPMAP_NEUTRAL) == LV_SPMAP_NEUTRAL,
                  "a neutral item did not stay neutral");
    UT_ASSERT_MSG(lvSpmapOwner(t, 7) == 7, "owner 7 was written as %u", lvSpmapOwner(t, 7));

    /* Every tank frame, through the game's own conversion, comes back. */
    for (i = 0; i < 16; i++) {
        BYTE back = startsConvertDir(lvSpmapStartDir((unsigned char) i));
        UT_ASSERT_MSG(back == i, "frame %d is written as a start the game reads as %u", i, back);
    }
    return 0;
}

/* One export read back with the game's reader. */
typedef struct {
    bool read;
    int  pills, bases, starts;
    BYTE pillX, pillY;     /* the first pillbox */
    BYTE startX, startY, startDir;
    BYTE pillOwner;
} SpmapRead;

static void spmapReadBack(const char *path, SpmapRead *out) {
    map       mp;
    pillboxes pb;
    bases     bs;
    starts    ss;
    char      name[300];

    memset(out, 0, sizeof(*out));
    snprintf(name, sizeof(name), "%s", path);
    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);
    out->read = mapRead(name, &mp, &pb, &bs, &ss);
    if (out->read) {
        out->pills  = pillsGetNumPills(&pb);
        out->bases  = basesGetNumBases(&bs);
        out->starts = startsGetNumStarts(&ss);
        if (out->pills > 0) {
            pillbox p;
            memset(&p, 0, sizeof(p));
            pillsGetPill(&pb, &p, 1);
            out->pillX     = p.x;
            out->pillY     = p.y;
            out->pillOwner = p.owner;
        }
        if (out->starts > 0) {
            start s;
            memset(&s, 0, sizeof(s));
            startsGetStartStruct(&ss, &s, 1);
            out->startX   = s.x;
            out->startY   = s.y;
            out->startDir = s.dir;
        }
    }
    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
}

int run_lv_spmap_reads_back(void) {
    ReplayHarness h;
    LvSpmapInfo   none, self;
    SpmapRead     rNone, rSelf;
    char          mapPath[300];
    bool          recorded;
    int           wroteNone = 0, wroteSelf = 0;
    int           dx, dy;

    memset(&h, 0, sizeof(h));
    memset(&none, 0, sizeof(none));
    memset(&self, 0, sizeof(self));
    memset(&rNone, 0, sizeof(rNone));
    memset(&rSelf, 0, sizeof(rSelf));
    recorded = replayHarnessStartRecording(&h, "lv_spmap", "Tester");
    if (recorded) {
        replayHarnessTick(&h, 600);
        recorded = replayHarnessStopRecording(&h);
    }
    if (recorded) {
        snprintf(mapPath, sizeof(mapPath), "%s.sp.map", h.path);
        wroteNone = lvSpmapWriteFromLog(h.path, 6000, -1, mapPath, &none);
        if (wroteNone) {
            spmapReadBack(mapPath, &rNone);
        }
        wroteSelf = lvSpmapWriteFromLog(h.path, 6000, 0, mapPath, &self);
        if (wroteSelf) {
            spmapReadBack(mapPath, &rSelf);
        }
        remove(mapPath);
    }
    replayHarnessStop(&h);
    UT_ASSERT_MSG(recorded, "the round could not be recorded");

    /* Nobody selected: the recording's own items and starts, where they were. */
    UT_ASSERT_MSG(wroteNone, "the map with nobody selected was not written");
    UT_ASSERT_MSG(rNone.read, "the game's mapRead refused the map with nobody selected");
    UT_ASSERT_MSG(none.self == -1 && none.tankStart == 0, "nobody selected, but a side was used");
    UT_ASSERT_MSG(none.pills > 0 && none.bases > 0 && none.starts > 0,
                  "the map has %d pillboxes, %d bases, %d starts", none.pills, none.bases,
                  none.starts);
    UT_ASSERT_MSG(rNone.pills == none.pills && rNone.bases == none.bases &&
                  rNone.starts == none.starts,
                  "mapRead found %d/%d/%d pillboxes/bases/starts; %d/%d/%d were written",
                  rNone.pills, rNone.bases, rNone.starts, none.pills, none.bases, none.starts);
    UT_ASSERT_MSG(rNone.pillX == none.firstPillX && rNone.pillY == none.firstPillY,
                  "mapRead moved the map: pillbox 1 written at %u,%u reads at %u,%u",
                  none.firstPillX, none.firstPillY, rNone.pillX, rNone.pillY);
    UT_ASSERT_MSG(rNone.pillOwner == NEUTRAL, "a neutral pillbox reads as owner %u",
                  rNone.pillOwner);

    /* The player selected: one start, on the tank's square. mapRead may
       move this map, since the starts that set its bounds are gone; the
       pillbox says how far, and the start must have moved the same. */
    UT_ASSERT_MSG(wroteSelf, "the map with the player selected was not written");
    UT_ASSERT_MSG(rSelf.read, "the game's mapRead refused the map with the player selected");
    UT_ASSERT_MSG(self.self == 0 && self.tankStart == 1,
                  "the player's live tank was not made the start (self %d, tankStart %d)",
                  self.self, self.tankStart);
    UT_ASSERT_MSG(rSelf.starts == 1, "the map has %d starts, not the tank's one", rSelf.starts);
    UT_ASSERT_MSG(rSelf.pills == self.pills && rSelf.bases == self.bases,
                  "mapRead found %d/%d pillboxes/bases; %d/%d were written", rSelf.pills,
                  rSelf.bases, self.pills, self.bases);
    dx = (int) rSelf.pillX - (int) self.firstPillX;
    dy = (int) rSelf.pillY - (int) self.firstPillY;
    UT_ASSERT_MSG((int) rSelf.startX == (int) self.startX + dx &&
                  (int) rSelf.startY == (int) self.startY + dy,
                  "the start written at %u,%u reads at %u,%u; the map moved by %d,%d",
                  self.startX, self.startY, rSelf.startX, rSelf.startY, dx, dy);
    UT_ASSERT_MSG(rSelf.startDir == self.startDir, "the start faces %u, written as %u",
                  rSelf.startDir, self.startDir);
    return 0;
}
