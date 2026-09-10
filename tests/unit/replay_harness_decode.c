/*
 * Record-and-decode replay fixture — decode side.
 *
 * Loads a recorded .wbv through the production log-viewer reader
 * (lv_screenLoadMapFromMemory, the same entry point the standalone viewer
 * opens a file with), plays it to end-of-log and reads the world it
 * decoded into the seam struct.
 *
 * This translation unit is viewer-world and deliberately includes none of
 * the sim headers: the two disagree about the world types. replay_harness.h
 * carries only plain integers for exactly that reason, and the sim side
 * lives in replay_harness.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_bolo_map.h"
#include "lv_pillbox.h"
#include "lv_bases.h"
#include "lv_starts.h"
#include "lv_players.h"
#include "replay_harness.h"

/* Same check the sim side makes, against this world's own constants. */
#if REPLAY_MAP_SIZE != MAP_ARRAY_SIZE
#error "REPLAY_MAP_SIZE disagrees with the viewer's MAP_ARRAY_SIZE"
#endif
#if REPLAY_MAX_TANKS != MAX_TANKS
#error "REPLAY_MAX_TANKS disagrees with the viewer's MAX_TANKS"
#endif
#if REPLAY_MAX_PILLS != MAX_PILLS
#error "REPLAY_MAX_PILLS disagrees with the viewer's MAX_PILLS"
#endif
#if REPLAY_MAX_BASES != MAX_BASES
#error "REPLAY_MAX_BASES disagrees with the viewer's MAX_BASES"
#endif
#if REPLAY_MAX_STARTS != MAX_STARTS
#error "REPLAY_MAX_STARTS disagrees with the viewer's MAX_STARTS"
#endif

/* Playback advances one 20ms tick per call, and a run of idle ticks costs
 * one call each, so the cap only has to exceed the recorded round's tick
 * count. Reaching it means the stream never terminated. */
#define REPLAY_DECODE_TICK_CAP 8192

/* Read the decoded world out of the viewer's own structures. */
static void replayCaptureViewer(ReplayWorld *w) {
    LogViewerState *lv = lv_screenGetState();
    int x;
    int y;
    BYTE count;
    BYTE total;

    memset(w, 0, sizeof(*w));
    if (lv == NULL) {
        return;
    }

    for (x = 0; x < REPLAY_MAP_SIZE; x++) {
        for (y = 0; y < REPLAY_MAP_SIZE; y++) {
            w->terrain[x][y] = lv_mapGetPos(&lv->mp, (BYTE) x, (BYTE) y);
        }
    }

    /* Numbered from 1, matching the sim side's getters. */
    total = lv_pillsGetNumPills(&lv->pb);
    if (total > REPLAY_MAX_PILLS) total = REPLAY_MAX_PILLS;
    w->numPills = total;
    for (count = 1; count <= total; count++) {
        pillbox item;
        memset(&item, 0, sizeof(item));
        lv_pillsGetPill(&lv->pb, &item, count);
        w->pills[count - 1].x = item.x;
        w->pills[count - 1].y = item.y;
        w->pills[count - 1].owner = item.owner;
        w->pills[count - 1].armour = item.armour;
    }

    total = lv_basesGetNumBases(&lv->bs);
    if (total > REPLAY_MAX_BASES) total = REPLAY_MAX_BASES;
    w->numBases = total;
    for (count = 1; count <= total; count++) {
        base item;
        memset(&item, 0, sizeof(item));
        lv_basesGetBase(&lv->bs, &item, count);
        w->bases[count - 1].x = item.x;
        w->bases[count - 1].y = item.y;
        w->bases[count - 1].owner = item.owner;
        w->bases[count - 1].armour = item.armour;
        w->bases[count - 1].shells = item.shells;
        w->bases[count - 1].mines = item.mines;
    }

    total = lv_startsGetNumStarts(&lv->ss);
    if (total > REPLAY_MAX_STARTS) total = REPLAY_MAX_STARTS;
    w->numStarts = total;
    for (count = 1; count <= total; count++) {
        start item;
        memset(&item, 0, sizeof(item));
        lv_startsGetStartStruct(&lv->ss, &item, count);
        w->starts[count - 1].x = item.x;
        w->starts[count - 1].y = item.y;
        w->starts[count - 1].dir = item.dir;
    }

    for (count = 0; count < REPLAY_MAX_TANKS; count++) {
        w->tanks[count].inUse = lv_playersIsInUse(count) == TRUE;
        if (w->tanks[count].inUse) {
            w->tanks[count].shells = lv->tankInv[count].shells;
            w->tanks[count].mines = lv->tankInv[count].mines;
            w->tanks[count].armour = lv->tankInv[count].armour;
            w->tanks[count].trees = lv->tankInv[count].trees;
        }
    }
}

bool replayHarnessDecode(ReplayHarness *h) {
    FILE *f;
    long sz;
    uint8_t *zipData;
    LogViewerState *lv;
    int steps;
    bool reachedEnd;

    if (h == NULL || h->path[0] == '\0') {
        return false;
    }

    f = fopen(h->path, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return false;
    }
    zipData = (uint8_t *) malloc((size_t) sz);
    if (zipData == NULL) {
        fclose(f);
        return false;
    }
    if (fread(zipData, 1, (size_t) sz, f) != (size_t) sz) {
        fclose(f);
        free(zipData);
        return false;
    }
    fclose(f);

    if (h->replayed == NULL) {
        h->replayed = (ReplayWorld *) malloc(sizeof(ReplayWorld));
        if (h->replayed == NULL) {
            free(zipData);
            return false;
        }
    }

    lv = lv_decoderCreate(false);
    if (lv == NULL) {
        free(zipData);
        return false;
    }
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);

    /* Takes ownership of zipData (freed when the log is closed). */
    if (lv_screenLoadMapFromMemory(zipData, (size_t) sz) != TRUE) {
        lv_decoderDestroy(lv);
        return false;
    }

    /* The load decodes the header and the opening snapshot only; the event
     * records are applied by playback. LOG_QUIT ends it, which is what
     * clears isPlaying. */
    steps = 0;
    while (lv_screenIsPlaying() == TRUE && steps < REPLAY_DECODE_TICK_CAP) {
        lv_screenLogTick();
        steps++;
    }
    reachedEnd = lv_screenIsPlaying() != TRUE;
    if (reachedEnd) {
        replayCaptureViewer(h->replayed);
    }

    lv_decoderDestroy(lv);   /* closes the log, freeing the zip buffer */
    return reachedEnd;
}
