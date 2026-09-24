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
#include "blocks.h"   /* stream position and key — the seek entry point moves both */
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
        w->pills[count - 1].active = lv_pillsIsActive(&lv->pb, count) == TRUE;
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
        w->bases[count - 1].active = lv_basesIsActive(&lv->bs, count) == TRUE;
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
        w->starts[count - 1].active = lv_startsIsActive(&lv->ss, count) == TRUE;
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

/* Stand a decoder up on the file and hand it the bytes. On success the
 * decoder is live and has consumed the header and the opening snapshot, and
 * the caller owns it until lv_decoderDestroy. Returns NULL on any failure,
 * with nothing left to tear down. */
static LogViewerState *replayDecoderOpen(const char *path) {
    FILE *f;
    long sz;
    uint8_t *zipData;
    LogViewerState *lv;

    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    zipData = (uint8_t *) malloc((size_t) sz);
    if (zipData == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(zipData, 1, (size_t) sz, f) != (size_t) sz) {
        fclose(f);
        free(zipData);
        return NULL;
    }
    fclose(f);

    lv = lv_decoderCreate(false);
    if (lv == NULL) {
        free(zipData);
        return NULL;
    }
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);

    /* Takes ownership of zipData (freed when the log is closed). */
    if (lv_screenLoadMapFromMemory(zipData, (size_t) sz) != TRUE) {
        lv_decoderDestroy(lv);
        return NULL;
    }
    return lv;
}

bool replayHarnessDecodeFromLastSnapshot(const char *path, ReplayWorld *w) {
    LogViewerState *lv;
    size_t snapPos = 0;
    BYTE   snapKey = 0;
    bool   haveSnap = false;
    int    steps;

    if (path == NULL || path[0] == '\0' || w == NULL) {
        return false;
    }

    /* Pass one finds where the last snapshot sits. The load has already
       consumed the opening one, so this is the last mid-round snapshot, and
       a file without one is refused rather than quietly answered from the
       opening snapshot. Nothing is captured here. */
    lv = replayDecoderOpen(path);
    if (lv == NULL) {
        return false;
    }
    steps = 0;
    while (lv_screenIsPlaying() == TRUE && steps < REPLAY_DECODE_TICK_CAP) {
        size_t at  = lv_logGetCurrentPosition();
        BYTE   key = lv_blocksGetKey();
        bool   marker = (lv_screenLogTick() == TRUE);
        steps++;
        /* lv_screenLogTick reports TRUE for a snapshot and for the log's
           end; only the first leaves playback running. */
        if (marker && lv_screenIsPlaying() == TRUE) {
            snapPos  = at;
            snapKey  = key;
            haveSnap = true;
        }
    }
    if (lv_screenIsPlaying() == TRUE) {
        lv_decoderDestroy(lv);
        return false;
    }
    lv_decoderDestroy(lv);
    if (!haveSnap) {
        return false;
    }

    /* Pass two opens the file again and jumps straight to that snapshot, so
       every record before it goes unread — the same state a scrub back to
       this point leaves the viewer in. What the world says afterwards is what
       the snapshot and the records following it said, and nothing else. */
    lv = replayDecoderOpen(path);
    if (lv == NULL) {
        return false;
    }
    lv_logSetPosition(snapPos);
    lv_blocksSetKey(snapKey);
    steps = 0;
    while (lv_screenIsPlaying() == TRUE && steps < REPLAY_DECODE_TICK_CAP) {
        lv_screenLogTick();
        steps++;
    }
    if (lv_screenIsPlaying() == TRUE) {
        lv_decoderDestroy(lv);
        return false;
    }
    replayCaptureViewer(w);
    lv_decoderDestroy(lv);
    return true;
}

bool replayHarnessDecodeFile(const char *path, ReplayWorld *w,
                             ReplayFileInfo *info) {
    LogViewerState *lv;
    int steps;
    bool reachedEnd;

    if (path == NULL || path[0] == '\0' || w == NULL) {
        return false;
    }

    lv = replayDecoderOpen(path);
    if (lv == NULL) {
        return false;
    }

    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        /* The viewer's map name is at most MAP_STR_SIZE; the buffer is
         * larger than that. */
        lv_screenGetMapName(info->mapName);
        /* Computed by the load's byte walk, before any playback. */
        info->totalTimeMs = lv_screenGetState()->totalTimeMs;
        info->gameType = lv_screenGetState()->gt;
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
        replayCaptureViewer(w);
        if (info != NULL) {
            info->ticks = steps;
        }
    }

    lv_decoderDestroy(lv);   /* closes the log, freeing the zip buffer */
    return reachedEnd;
}

bool replayHarnessDecode(ReplayHarness *h) {
    if (h == NULL || h->path[0] == '\0') {
        return false;
    }
    if (h->replayed == NULL) {
        h->replayed = (ReplayWorld *) malloc(sizeof(ReplayWorld));
        if (h->replayed == NULL) {
            return false;
        }
    }
    return replayHarnessDecodeFile(h->path, h->replayed, NULL);
}

/* FNV-1a over the terrain array in memory order ([x][y]). */
static uint32_t replayTerrainHash(const ReplayWorld *w) {
    const uint8_t *p = &w->terrain[0][0];
    size_t n = sizeof(w->terrain);
    size_t i;
    uint32_t h = 2166136261u;
    for (i = 0; i < n; i++) {
        h ^= (uint32_t) p[i];
        h *= 16777619u;
    }
    return h;
}

void replayHarnessWriteSummary(const ReplayWorld *w, const ReplayFileInfo *info,
                               FILE *out) {
    int i;

    fprintf(out, "map: %s\n", info != NULL ? info->mapName : "");
    fprintf(out, "ticks: %d\n", info != NULL ? info->ticks : 0);
    fprintf(out, "terrain fnv1a: %08x\n", (unsigned) replayTerrainHash(w));

    fprintf(out, "pills: %u\n", (unsigned) w->numPills);
    for (i = 0; i < w->numPills && i < REPLAY_MAX_PILLS; i++) {
        const ReplayPill *p = &w->pills[i];
        fprintf(out, "pill %d: x=%u y=%u owner=%u armour=%u\n",
                i + 1, p->x, p->y, p->owner, p->armour);
    }

    fprintf(out, "bases: %u\n", (unsigned) w->numBases);
    for (i = 0; i < w->numBases && i < REPLAY_MAX_BASES; i++) {
        const ReplayBase *b = &w->bases[i];
        fprintf(out, "base %d: x=%u y=%u owner=%u armour=%u shells=%u mines=%u\n",
                i + 1, b->x, b->y, b->owner, b->armour, b->shells, b->mines);
    }

    fprintf(out, "starts: %u\n", (unsigned) w->numStarts);
    for (i = 0; i < w->numStarts && i < REPLAY_MAX_STARTS; i++) {
        const ReplayStart *s = &w->starts[i];
        fprintf(out, "start %d: x=%u y=%u dir=%u\n", i + 1, s->x, s->y, s->dir);
    }

    for (i = 0; i < REPLAY_MAX_TANKS; i++) {
        const ReplayTank *t = &w->tanks[i];
        if (!t->inUse) {
            continue;
        }
        fprintf(out, "tank %d: shells=%u mines=%u armour=%u trees=%u\n",
                i, t->shells, t->mines, t->armour, t->trees);
    }
}
