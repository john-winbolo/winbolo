/*
 * test_brainrec_split.c - brainrec_split cuts the brain logs at the right
 * brain tick.
 *
 * A brain's print2 log and player<N>.jsonl count brain ticks, and a brain
 * thinks every other engine tick. The splitter finds where each part starts
 * in a brain log from where the brain's run started in engine ticks.
 *
 * The old rule put brain tick 1 at the block's first frame. In a -brain-debug
 * block after the first, the bots keep running, so the log starts at a tick
 * such as 54056. Every cut then fell before the log's first line and the
 * whole log went to the last part. The splitter now anchors such a run on
 * its first tick, and a run with a BRAIN_OPEN line on that line.
 *
 * The case records a short session with the real recorder, writes brain logs
 * for two bots beside it (one as a later block, one with BRAIN_OPEN), with
 * part of each log gzip-compressed the way the server leaves a finished
 * block, and runs the whole tool on it.
 */

#define BRAINREC_SPLIT_ENTRY brainrecSplitMain
int brainrecSplitMain(int argc, char **argv);
#include "../../src/tools/brainrec_split.c"

#include "test_harness.h"

#define BRS_FRAMES  1100
#define BRS_LATE_B0 54056LL   /* first brain tick of the later block's log */
#define BRS_SEED    5000LL    /* bot 1's BRAIN_OPEN seed */
#define BRS_OPEN_AT 100LL     /* bot 1 is created this many engine ticks in */

/* Write brain ticks [from, to] as print2 lines (or jsonl tick rows) to an
 * open gzFile. */
static bool brsWriteTicks(gzFile g, bool jsonl, long long from, long long to) {
    char line[160];
    for (long long b = from; b <= to; b++) {
        int n = jsonl ? SDL_snprintf(line, sizeof line, "{\"type\":\"tick\",\"t\":%lld}\n", b)
                      : SDL_snprintf(line, sizeof line, "===TICK %lld===\nbody %lld\n", b, b);
        if (gzwrite(g, line, (unsigned)n) != n) return false;
    }
    return true;
}

/* A log with ticks [from, mid) in <path>.gz and [mid, to] in <path>, after
 * an optional first line. */
static bool brsWriteLog(const char *path, bool jsonl, const char *firstLine,
                        long long from, long long mid, long long to) {
    char gzName[SPLIT_PATH_MAX];
    SDL_snprintf(gzName, sizeof gzName, "%s.gz", path);
    gzFile a = gzopen(gzName, "wb6");
    if (!a) return false;
    bool ok = true;
    if (firstLine) ok = gzputs(a, firstLine) >= 0;
    ok = ok && brsWriteTicks(a, jsonl, from, mid - 1);
    if (gzclose(a) != Z_OK) ok = false;
    gzFile b = gzopen(path, "wbT");
    if (!b) return false;
    ok = ok && brsWriteTicks(b, jsonl, mid, to);
    if (gzclose(b) != Z_OK) ok = false;
    return ok;
}

/* First and last tick in a part's log (read as gzip or plain). */
static bool brsTickRange(const char *path, bool jsonl, long long *first, long long *last) {
    gzFile g = gzopen(path, "rb");
    if (!g) return false;
    char line[256];
    *first = *last = -1;
    while (gzgets(g, line, sizeof line)) {
        long long t = parseLineTick(jsonl ? TICK_JSONL : TICK_PRINT2, line, strlen(line));
        if (t < 0) continue;
        if (*first < 0) *first = t;
        *last = t;
    }
    gzclose(g);
    return true;
}

int run_brainrec_split_cuts_brain_logs_by_engine_tick(void) {
    char parent[512], sess[600], path[700];
    uint32_t firstFrame = 0, lastFrame = 0;
    const char *base = "20261104_000000_1_v7_split_test";

    UT_ASSERT_MSG(utScratchPath(parent, sizeof parent, NULL), "no scratch dir");
    SDL_snprintf(sess, sizeof sess, "%s/%s", parent, base);
    UT_ASSERT(utBtrRecordSession(sess, BRS_FRAMES, &firstFrame, &lastFrame));
    const long long F = (long long)firstFrame;
    const long long frames = (long long)lastFrame - F + 1;

    /* Bot 0: a later block. Brain tick BRS_LATE_B0 thinks at the first
     * frame. */
    const long long b0Last = BRS_LATE_B0 + frames / 2 - 1;
    SDL_snprintf(path, sizeof path, "%s/print2_bot0.log", sess);
    UT_ASSERT(brsWriteLog(path, false, NULL, BRS_LATE_B0, BRS_LATE_B0 + frames / 4, b0Last));
    SDL_snprintf(path, sizeof path, "%s/player0.jsonl", sess);
    UT_ASSERT(brsWriteLog(path, true, NULL, BRS_LATE_B0, BRS_LATE_B0 + frames / 4, b0Last));

    /* Bot 1: created BRS_OPEN_AT engine ticks in, tick seeded at
     * BRS_SEED; its first think is brain tick BRS_SEED + 1. */
    char openLine[128];
    SDL_snprintf(openLine, sizeof openLine, "BRAIN_OPEN t=%lld engine_tick=%lld\n", BRS_SEED,
                 F + BRS_OPEN_AT);
    const long long b1Last = BRS_SEED + (frames - BRS_OPEN_AT) / 2;
    SDL_snprintf(path, sizeof path, "%s/print2_bot1.log", sess);
    UT_ASSERT(brsWriteLog(path, false, openLine, BRS_SEED + 1, BRS_SEED + 100, b1Last));

    char *argv[] = { "brainrec_split", sess, "-parts", "2", NULL };
    int rc = brainrecSplitMain(4, argv);
    UT_ASSERT_MSG(rc == 0, "brainrec_split returned %d", rc);

    /* Where part 2 starts, in engine ticks. */
    char p1[700], p2[700];
    SDL_snprintf(p1, sizeof p1, "%s/%s_part1of2", parent, base);
    SDL_snprintf(p2, sizeof p2, "%s/%s_part2of2", parent, base);
    long long e2 = -1;
    {
        SDL_snprintf(path, sizeof path, "%s/brainrec.btr", p2);
        BrainRecIn *g = brainRecInOpen(path);
        UT_ASSERT_MSG(g != NULL, "no %s", path);
        BrainRecReader r;
        BrainRecFrameInfo fi;
        brainRecWalkReaderFromIn(&r, g);
        if (brainRecWalkPreamble(&r, NULL, NULL) == BRAINREC_WALK_OK &&
            brainRecWalkFrame(&r, &fi) == BRAINREC_WALK_OK) {
            e2 = (long long)fi.tick;
        }
        brainRecInClose(g);
        UT_ASSERT_MSG(e2 > F, "part 2 has no frames after the first");
    }

    struct {
        const char *name;
        bool        jsonl;
        long long   want;   /* first brain tick of part 2 */
        long long   first, last;
    } logs[3] = {
        { "print2_bot0.log", false, (e2 - F) / 2 + BRS_LATE_B0, BRS_LATE_B0, b0Last },
        { "player0.jsonl",   true,  (e2 - F) / 2 + BRS_LATE_B0, BRS_LATE_B0, b0Last },
        { "print2_bot1.log", false,
          (e2 - (F + BRS_OPEN_AT - 2 * BRS_SEED)) / 2 + 1, BRS_SEED + 1, b1Last },
    };
    for (int i = 0; i < 3; i++) {
        long long f1, l1, f2, l2;
        SDL_PathInfo pi;
        /* The input had a .gz part, so the parts are gzip and nothing
         * plain sits beside them. */
        SDL_snprintf(path, sizeof path, "%s/%s", p1, logs[i].name);
        UT_ASSERT_MSG(!SDL_GetPathInfo(path, &pi), "%s is plain in part 1", logs[i].name);
        SDL_snprintf(path, sizeof path, "%s/%s.gz", p1, logs[i].name);
        UT_ASSERT_MSG(brsTickRange(path, logs[i].jsonl, &f1, &l1), "no %s", path);
        SDL_snprintf(path, sizeof path, "%s/%s.gz", p2, logs[i].name);
        UT_ASSERT_MSG(brsTickRange(path, logs[i].jsonl, &f2, &l2), "no %s", path);
        UT_ASSERT_MSG(f1 == logs[i].first && l1 == logs[i].want - 1 &&
                      f2 == logs[i].want && l2 == logs[i].last,
                      "%s: part 1 has ticks %lld..%lld and part 2 %lld..%lld; wanted "
                      "%lld..%lld and %lld..%lld (part 2 starts at engine tick %lld)",
                      logs[i].name, f1, l1, f2, l2, logs[i].first, logs[i].want - 1,
                      logs[i].want, logs[i].last, e2);
    }
    return 0;
}
