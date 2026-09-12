/*
 * Record-and-decode replay fixture — sim side.
 *
 * Drives the recorder the way the dedicated server does (logCreate ->
 * logStart -> serverSimTick -> logStop), captures the sim's world, and
 * diffs two captures. The decode half lives in replay_harness_decode.c,
 * because the viewer headers cannot share a translation unit with these
 * ones; replay_harness.h explains the split and what the diff leaves out.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "players.h"
#include "tank.h"
#include "log.h"
#include "replay_harness.h"
#include "test_harness.h"

/* The seam's sizes have to be this world's sizes, or a capture would
 * silently describe a different world than the one being recorded. */
#if REPLAY_MAP_SIZE != MAP_ARRAY_SIZE
#error "REPLAY_MAP_SIZE disagrees with MAP_ARRAY_SIZE"
#endif
#if REPLAY_MAX_TANKS != MAX_TANKS
#error "REPLAY_MAX_TANKS disagrees with MAX_TANKS"
#endif
#if REPLAY_MAX_PILLS != MAX_PILLS
#error "REPLAY_MAX_PILLS disagrees with MAX_PILLS"
#endif
#if REPLAY_MAX_BASES != MAX_BASES
#error "REPLAY_MAX_BASES disagrees with MAX_BASES"
#endif
#if REPLAY_MAX_STARTS != MAX_STARTS
#error "REPLAY_MAX_STARTS disagrees with MAX_STARTS"
#endif

/* Read the sim's world into the seam struct. Terrain goes through
 * mapGetPos, the accessor the recorder serialises through. */
static void replayCaptureSim(ServerSim *sim, ReplayWorld *w) {
    GameSim *gs = serverSimGetGameSim(sim);
    int x;
    int y;
    BYTE count;
    BYTE total;

    memset(w, 0, sizeof(*w));
    if (gs == NULL) {
        return;
    }

    for (x = 0; x < REPLAY_MAP_SIZE; x++) {
        for (y = 0; y < REPLAY_MAP_SIZE; y++) {
            w->terrain[x][y] = mapGetPos(&gs->mp, (BYTE) x, (BYTE) y);
        }
    }

    /* Pills, bases and starts are numbered from 1 on both sides. */
    total = pillsGetNumPills(&gs->pb);
    if (total > REPLAY_MAX_PILLS) total = REPLAY_MAX_PILLS;
    w->numPills = total;
    for (count = 1; count <= total; count++) {
        pillbox item;
        memset(&item, 0, sizeof(item));
        pillsGetPill(&gs->pb, &item, count);
        w->pills[count - 1].x = item.x;
        w->pills[count - 1].y = item.y;
        w->pills[count - 1].owner = item.owner;
        w->pills[count - 1].armour = item.armour;
        w->pills[count - 1].active = pillsIsActive(&gs->pb, count) == TRUE;
    }

    total = basesGetNumBases(&gs->bs);
    if (total > REPLAY_MAX_BASES) total = REPLAY_MAX_BASES;
    w->numBases = total;
    for (count = 1; count <= total; count++) {
        base item;
        memset(&item, 0, sizeof(item));
        basesGetBase(&gs->bs, &item, count);
        w->bases[count - 1].x = item.x;
        w->bases[count - 1].y = item.y;
        w->bases[count - 1].owner = item.owner;
        w->bases[count - 1].armour = item.armour;
        w->bases[count - 1].shells = item.shells;
        w->bases[count - 1].mines = item.mines;
        w->bases[count - 1].active = basesIsActive(&gs->bs, count) == TRUE;
    }

    total = startsGetNumStarts(&gs->ss);
    if (total > REPLAY_MAX_STARTS) total = REPLAY_MAX_STARTS;
    w->numStarts = total;
    for (count = 1; count <= total; count++) {
        start item;
        memset(&item, 0, sizeof(item));
        startsGetStartStruct(&gs->ss, &item, count);
        w->starts[count - 1].x = item.x;
        w->starts[count - 1].y = item.y;
        w->starts[count - 1].dir = item.dir;
        w->starts[count - 1].active = startsIsActive(&gs->ss, count) == TRUE;
    }

    for (count = 0; count < MAX_TANKS; count++) {
        w->tanks[count].inUse = playersIsInUse(&gs->plyrs, count) == TRUE;
        if (w->tanks[count].inUse && gs->tanks[count] != NULL) {
            w->tanks[count].shells = tankGetShells(&gs->tanks[count]);
            w->tanks[count].mines = tankGetMines(&gs->tanks[count]);
            w->tanks[count].armour = tankGetArmour(&gs->tanks[count]);
            w->tanks[count].trees = tankGetTrees(&gs->tanks[count]);
        }
    }
}

bool replayHarnessPrepare(ReplayHarness *h, const char *tag,
                          const char *playerName) {
    if (h == NULL) {
        return false;
    }
    memset(h, 0, sizeof(*h));
    snprintf(h->path, sizeof(h->path), "replay_harness_%s.wbv",
             tag != NULL ? tag : "round");
    remove(h->path);

    h->sim = ut_make_running_sim(playerName != NULL ? playerName : "Tester");
    return h->sim != NULL;
}

bool replayHarnessBeginRecording(ReplayHarness *h) {
    if (h == NULL || h->sim == NULL || h->recording) {
        return false;
    }
    logCreate();
    /* Lobby mode drops every world event and writes an empty-world
     * snapshot. It is process state an earlier test may have left on, so
     * say which one this round wants. */
    logSetLobbyMode(FALSE);
    if (logStart(h->path, h->sim, /*ai*/ 0, /*maxPlayers*/ MAX_TANKS,
                 /*usePassword*/ FALSE) != TRUE) {
        logDestroy();
        return false;
    }
    h->recording = true;
    /* One recorded tick, so the round is under way before the caller
     * changes anything: it is the tick that pins the writer to this thread,
     * and it leaves the caller's changes to land in later ticks, after the
     * opening snapshot rather than alongside it. */
    replayHarnessTick(h, 2);
    return true;
}

bool replayHarnessStartRecording(ReplayHarness *h, const char *tag,
                                 const char *playerName) {
    if (!replayHarnessPrepare(h, tag, playerName)) {
        return false;
    }
    return replayHarnessBeginRecording(h);
}

void replayHarnessTick(ReplayHarness *h, int simTicks) {
    int i;
    if (h == NULL || h->sim == NULL) {
        return;
    }
    for (i = 0; i < simTicks; i++) {
        serverSimTick(h->sim);
    }
}

void replayHarnessSnapshot(ReplayHarness *h) {
    if (h == NULL || h->sim == NULL || !h->recording) {
        return;
    }
    logWriteSnapshot(h->sim, TRUE);
}

bool replayHarnessStopRecording(ReplayHarness *h) {
    if (h == NULL || h->sim == NULL || !h->recording) {
        return false;
    }
    if (h->recorded == NULL) {
        h->recorded = (ReplayWorld *) malloc(sizeof(ReplayWorld));
        if (h->recorded == NULL) {
            return false;
        }
    }
    /* Capture before the log closes, so the two sides describe the same
     * moment: the world as it stood at the last recorded tick. */
    replayCaptureSim(h->sim, h->recorded);
    logStop();
    logDestroy();
    h->recording = false;
    return true;
}

/* First difference wins, so the caller is told one concrete thing rather
 * than a count. Order runs from the largest structure down to the tanks. */
bool replayHarnessCompare(ReplayHarness *h) {
    const ReplayWorld *a;
    const ReplayWorld *b;
    int x;
    int y;
    int i;

    if (h == NULL) {
        return false;
    }
    h->diff[0] = '\0';
    if (h->recorded == NULL || h->replayed == NULL) {
        snprintf(h->diff, sizeof(h->diff),
                 "nothing to compare (recorded=%s replayed=%s)",
                 h->recorded != NULL ? "yes" : "no",
                 h->replayed != NULL ? "yes" : "no");
        return false;
    }
    a = h->recorded;
    b = h->replayed;

    for (x = 0; x < REPLAY_MAP_SIZE; x++) {
        for (y = 0; y < REPLAY_MAP_SIZE; y++) {
            if (a->terrain[x][y] != b->terrain[x][y]) {
                snprintf(h->diff, sizeof(h->diff),
                         "terrain at (%d,%d): sim %d, replay %d",
                         x, y, a->terrain[x][y], b->terrain[x][y]);
                return false;
            }
        }
    }

    if (a->numPills != b->numPills) {
        snprintf(h->diff, sizeof(h->diff), "pill count: sim %d, replay %d",
                 a->numPills, b->numPills);
        return false;
    }
    for (i = 0; i < a->numPills; i++) {
        const ReplayPill *pa = &a->pills[i];
        const ReplayPill *pb = &b->pills[i];
        if (pa->active != pb->active) {
            snprintf(h->diff, sizeof(h->diff),
                     "pill %d on the map: sim %s, replay %s",
                     i + 1, pa->active ? "yes" : "no",
                     pb->active ? "yes" : "no");
            return false;
        }
        if (pa->x != pb->x || pa->y != pb->y) {
            snprintf(h->diff, sizeof(h->diff),
                     "pill %d cell: sim (%d,%d), replay (%d,%d)",
                     i + 1, pa->x, pa->y, pb->x, pb->y);
            return false;
        }
        if (pa->owner != pb->owner) {
            snprintf(h->diff, sizeof(h->diff),
                     "pill %d owner: sim %d, replay %d",
                     i + 1, pa->owner, pb->owner);
            return false;
        }
        if (pa->armour != pb->armour) {
            snprintf(h->diff, sizeof(h->diff),
                     "pill %d armour: sim %d, replay %d",
                     i + 1, pa->armour, pb->armour);
            return false;
        }
    }

    if (a->numBases != b->numBases) {
        snprintf(h->diff, sizeof(h->diff), "base count: sim %d, replay %d",
                 a->numBases, b->numBases);
        return false;
    }
    for (i = 0; i < a->numBases; i++) {
        const ReplayBase *ba = &a->bases[i];
        const ReplayBase *bb = &b->bases[i];
        if (ba->active != bb->active) {
            snprintf(h->diff, sizeof(h->diff),
                     "base %d on the map: sim %s, replay %s",
                     i + 1, ba->active ? "yes" : "no",
                     bb->active ? "yes" : "no");
            return false;
        }
        if (ba->x != bb->x || ba->y != bb->y) {
            snprintf(h->diff, sizeof(h->diff),
                     "base %d cell: sim (%d,%d), replay (%d,%d)",
                     i + 1, ba->x, ba->y, bb->x, bb->y);
            return false;
        }
        if (ba->owner != bb->owner) {
            snprintf(h->diff, sizeof(h->diff),
                     "base %d owner: sim %d, replay %d",
                     i + 1, ba->owner, bb->owner);
            return false;
        }
        if (ba->armour != bb->armour || ba->shells != bb->shells ||
            ba->mines != bb->mines) {
            snprintf(h->diff, sizeof(h->diff),
                     "base %d stock: sim %d/%d/%d, replay %d/%d/%d "
                     "(armour/shells/mines)",
                     i + 1, ba->armour, ba->shells, ba->mines,
                     bb->armour, bb->shells, bb->mines);
            return false;
        }
    }

    if (a->numStarts != b->numStarts) {
        snprintf(h->diff, sizeof(h->diff), "start count: sim %d, replay %d",
                 a->numStarts, b->numStarts);
        return false;
    }
    for (i = 0; i < a->numStarts; i++) {
        const ReplayStart *sa = &a->starts[i];
        const ReplayStart *sb = &b->starts[i];
        if (sa->active != sb->active) {
            snprintf(h->diff, sizeof(h->diff),
                     "start %d on the map: sim %s, replay %s",
                     i + 1, sa->active ? "yes" : "no",
                     sb->active ? "yes" : "no");
            return false;
        }
        if (sa->x != sb->x || sa->y != sb->y || sa->dir != sb->dir) {
            snprintf(h->diff, sizeof(h->diff),
                     "start %d: sim (%d,%d) dir %d, replay (%d,%d) dir %d",
                     i + 1, sa->x, sa->y, sa->dir, sb->x, sb->y, sb->dir);
            return false;
        }
    }

    for (i = 0; i < REPLAY_MAX_TANKS; i++) {
        const ReplayTank *ta = &a->tanks[i];
        const ReplayTank *tb = &b->tanks[i];
        if (ta->inUse != tb->inUse) {
            snprintf(h->diff, sizeof(h->diff),
                     "tank %d in use: sim %s, replay %s",
                     i, ta->inUse ? "yes" : "no", tb->inUse ? "yes" : "no");
            return false;
        }
        if (!ta->inUse) {
            continue;
        }
        if (ta->shells != tb->shells || ta->mines != tb->mines ||
            ta->armour != tb->armour || ta->trees != tb->trees) {
            snprintf(h->diff, sizeof(h->diff),
                     "tank %d stocks: sim %d/%d/%d/%d, replay %d/%d/%d/%d "
                     "(shells/mines/armour/trees)",
                     i, ta->shells, ta->mines, ta->armour, ta->trees,
                     tb->shells, tb->mines, tb->armour, tb->trees);
            return false;
        }
    }

    return true;
}

const char *replayHarnessDiff(const ReplayHarness *h) {
    if (h == NULL) {
        return "";
    }
    return h->diff;
}

void replayHarnessStop(ReplayHarness *h) {
    if (h == NULL) {
        return;
    }
    if (h->recording) {
        logStop();
        logDestroy();
        h->recording = false;
    }
    if (h->sim != NULL) {
        serverSimDestroy(h->sim);
        h->sim = NULL;
    }
    free(h->recorded);
    h->recorded = NULL;
    free(h->replayed);
    h->replayed = NULL;
    if (h->path[0] != '\0') {
        remove(h->path);
        h->path[0] = '\0';
    }
}
