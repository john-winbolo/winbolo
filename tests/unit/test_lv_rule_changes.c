/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The log viewer's rules at the playhead.
 *
 * A scenario can change a simulation rule mid-round, and the recording holds
 * each change as a log_RuleSet. The viewer collects every one of them when the
 * file loads, answers a rule's value at any time from that list, then from the
 * round's scripts.json, then from the classic table, and fills LvRules from
 * those answers wherever the playhead lands. Its snapshots carry no rules, so
 * the list is what lets a seek backwards put an earlier value back.
 *
 * The scripted cases record a real round through the scenario host
 * (lv_scripts_fixture.c) and read it back through the production reader. The
 * hostile cases build the log.dat bytes by hand, since no writer produces a
 * record that is wrong.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_log.h"
#include "lv_pillbox.h"
#include "sim_rules_names.h"
#include "zip.h"
#include "replay_harness.h"
#include "lv_scripts_fixture.h"
#include "test_harness.h"

/* ── What a decode saw ─────────────────────────────────────────────── */

/* The rule changes to one rule, and where the first of them landed. */
static int lvrcCountFor(int index, uint32_t *firstMs, double *firstValue) {
    const LvRuleChange *changes = NULL;
    int count = lv_screenGetRuleChanges(&changes);
    int found = 0;
    int i;

    for (i = 0; i < count; i++) {
        if (changes[i].index != index) continue;
        if (found == 0) {
            if (firstMs != NULL) *firstMs = changes[i].ms;
            if (firstValue != NULL) *firstValue = changes[i].value;
        }
        found++;
    }
    return found;
}

typedef struct {
    int      capChanges;       /* pill_max_armour changes in the list */
    uint32_t capMs;
    double   capValue;
    int      endCap;           /* LvRules at end-of-log */
    int      endShells;
    bool     scriptsSeen;      /* lv_screenGetScripts answered while open */
    int      beforeCap;        /* after a seek to just before the change */
    uint32_t beforeMs;
    int      atCap;            /* after a seek forward onto the change */
    uint32_t atMs;
    int      afterCap;         /* after a seek forward past it */
    int      queryBefore;      /* lv_screenRuleValueAt either side of it */
    int      queryAt;
} LvrcSeen;

/* Read the list and LvRules at end-of-log, then seek back before the change,
 * onto it and past it. */
static void lvrcReadAndSeek(void *ctx) {
    LvrcSeen       *seen = (LvrcSeen *)ctx;
    LogViewerState *lv   = lv_screenGetState();

    seen->capChanges   = lvrcCountFor(SIM_RULE_pill_max_armour, &seen->capMs,
                                      &seen->capValue);
    seen->endCap       = lv->rules.pillMaxArmour;
    seen->endShells    = lv->rules.tankFullShells;
    seen->scriptsSeen  = lv_screenGetScripts() != NULL;

    if (seen->capChanges != 1 || seen->capMs < 40) {
        return;
    }
    seen->queryBefore = (int)lv_screenRuleValueAt(SIM_RULE_pill_max_armour,
                                                  seen->capMs - 20);
    seen->queryAt     = (int)lv_screenRuleValueAt(SIM_RULE_pill_max_armour,
                                                  seen->capMs);

    lv_screenSeekToTimeMs(seen->capMs - 20);
    seen->beforeMs  = lv_screenGetTimeRunning();
    seen->beforeCap = lv->rules.pillMaxArmour;

    lv_screenSeekToTimeMs(seen->capMs);
    seen->atMs  = lv_screenGetTimeRunning();
    seen->atCap = lv->rules.pillMaxArmour;

    lv_screenSeekToTimeMs(seen->capMs + 40);
    seen->afterCap = lv->rules.pillMaxArmour;
}

/* The rule-change round recorded under tag and decoded into *seen. */
static bool lvrcDecodeRuleRound(const char *tag, LvrcSeen *seen) {
    char path[512];
    bool played;

    memset(seen, 0, sizeof(*seen));
    if (!lvScriptsRecordRuleRound(tag, path, sizeof(path))) {
        if (path[0] != '\0') remove(path);
        return false;
    }
    played = replayHarnessDecodeFileThen(path, lvrcReadAndSeek, seen);
    remove(path);
    return played;
}

/* ── 1. A change collected ─────────────────────────────────────────── */

int run_lv_rule_changes_collected(void) {
    LvrcSeen seen;

    UT_ASSERT_MSG(lvrcDecodeRuleRound("rule_collected", &seen),
                  "the rule-change round could not be recorded or played");

    UT_ASSERT_MSG(seen.capChanges == 1,
                  "%d pill_max_armour changes collected (want 1: the walk's, "
                  "with playback adding none)", seen.capChanges);
    UT_ASSERT_MSG(seen.capMs > 0, "the change landed at %u ms (want > 0)",
                  (unsigned)seen.capMs);
    UT_ASSERT_MSG(seen.capValue == (double)LVSF_PILL_CAP,
                  "the change set %g (want %d)", seen.capValue, LVSF_PILL_CAP);
    UT_ASSERT_MSG(seen.endCap == LVSF_PILL_CAP,
                  "pillMaxArmour %d at end-of-log (want %d)", seen.endCap,
                  LVSF_PILL_CAP);
    UT_ASSERT_MSG(seen.endShells == LVSF_RULE_SHELLS,
                  "tankFullShells %d at end-of-log (want scripts.json's %d)",
                  seen.endShells, LVSF_RULE_SHELLS);
    UT_ASSERT_MSG(seen.scriptsSeen,
                  "lv_screenGetScripts answered NULL with the log open");
    return 0;
}

/* ── 2. Seeking back and forward ───────────────────────────────────── */

int run_lv_rule_changes_seek(void) {
    LvrcSeen seen;

    UT_ASSERT_MSG(lvrcDecodeRuleRound("rule_seek", &seen),
                  "the rule-change round could not be recorded or played");
    UT_ASSERT_MSG(seen.capChanges == 1 && seen.capMs >= 40,
                  "%d changes, the first at %u ms (want one, at 40 ms or "
                  "later, so there is a frame before it to seek to)",
                  seen.capChanges, (unsigned)seen.capMs);

    UT_ASSERT_MSG(seen.queryBefore == 15,
                  "the query answered %d a frame before the change (want 15)",
                  seen.queryBefore);
    UT_ASSERT_MSG(seen.queryAt == LVSF_PILL_CAP,
                  "the query answered %d at the change (want %d)",
                  seen.queryAt, LVSF_PILL_CAP);

    UT_ASSERT_MSG(seen.beforeMs == seen.capMs - 20,
                  "the seek back landed at %u ms (want %u)",
                  (unsigned)seen.beforeMs, (unsigned)(seen.capMs - 20));
    UT_ASSERT_MSG(seen.beforeCap == 15,
                  "pillMaxArmour %d after seeking back before the change "
                  "(want the classic 15)", seen.beforeCap);
    UT_ASSERT_MSG(seen.atMs == seen.capMs,
                  "the seek forward landed at %u ms (want %u)",
                  (unsigned)seen.atMs, (unsigned)seen.capMs);
    UT_ASSERT_MSG(seen.atCap == LVSF_PILL_CAP,
                  "pillMaxArmour %d after seeking forward onto the change "
                  "(want %d)", seen.atCap, LVSF_PILL_CAP);
    UT_ASSERT_MSG(seen.afterCap == LVSF_PILL_CAP,
                  "pillMaxArmour %d after seeking forward past the change "
                  "(want %d)", seen.afterCap, LVSF_PILL_CAP);
    return 0;
}

/* ── 3. A plain round ──────────────────────────────────────────────── */

typedef struct {
    int     changes;
    LvRules rules;
} LvrcPlain;

static void lvrcReadPlain(void *ctx) {
    LvrcPlain *plain = (LvrcPlain *)ctx;
    plain->changes = lv_screenGetRuleChanges(NULL);
    plain->rules   = lv_screenGetState()->rules;
}

int run_lv_rule_changes_plain_round(void) {
    ReplayHarness h;
    LvrcPlain     plain;
    bool          recorded;
    bool          played = false;

    memset(&plain, 0, sizeof(plain));
    memset(&h, 0, sizeof(h));
    plain.changes = -1;
    recorded = replayHarnessStartRecording(&h, "lv_rule_plain", "Tester");
    if (recorded) {
        replayHarnessTick(&h, 6);
        recorded = replayHarnessStopRecording(&h);
    }
    if (recorded) {
        played = replayHarnessDecodeFileThen(h.path, lvrcReadPlain, &plain);
    }
    replayHarnessStop(&h);
    UT_ASSERT_MSG(recorded, "the plain round could not be recorded");
    UT_ASSERT_MSG(played, "the plain round did not play to end-of-log");

    UT_ASSERT_MSG(plain.changes == 0, "%d rule changes in a plain round",
                  plain.changes);
    UT_ASSERT_MSG(plain.rules.tankFullShells ==
                      (BYTE)simRulesClassicValue(SIM_RULE_tank_full_shells) &&
                  plain.rules.tankFullMines ==
                      (BYTE)simRulesClassicValue(SIM_RULE_tank_full_mines) &&
                  plain.rules.tankFullArmour ==
                      (BYTE)simRulesClassicValue(SIM_RULE_tank_full_armour) &&
                  plain.rules.tankFullTrees ==
                      (BYTE)simRulesClassicValue(SIM_RULE_tank_full_trees),
                  "tank caps %u/%u/%u/%u are not the classic values",
                  plain.rules.tankFullShells, plain.rules.tankFullMines,
                  plain.rules.tankFullArmour, plain.rules.tankFullTrees);
    UT_ASSERT_MSG(plain.rules.baseFullShells ==
                      (BYTE)simRulesClassicValue(SIM_RULE_base_full_shells) &&
                  plain.rules.baseFullMines ==
                      (BYTE)simRulesClassicValue(SIM_RULE_base_full_mines) &&
                  plain.rules.baseFullArmour ==
                      (BYTE)simRulesClassicValue(SIM_RULE_base_full_armour),
                  "base caps %u/%u/%u are not the classic values",
                  plain.rules.baseFullShells, plain.rules.baseFullMines,
                  plain.rules.baseFullArmour);
    UT_ASSERT_MSG(plain.rules.pillMaxArmour == 15,
                  "pillMaxArmour %d in a plain round (want 15)",
                  plain.rules.pillMaxArmour);
    return 0;
}

/* ── Hand-built logs ───────────────────────────────────────────────── */

typedef struct {
    uint8_t buf[2048];
    size_t  len;
} LvrcLog;

static void lvrcPut(LvrcLog *b, const void *bytes, size_t n) {
    memcpy(b->buf + b->len, bytes, n);
    b->len += n;
}

static void lvrcPutU8(LvrcLog *b, uint8_t v) { lvrcPut(b, &v, 1); }

static void lvrcPutZeros(LvrcLog *b, size_t n) {
    memset(b->buf + b->len, 0, n);
    b->len += n;
}

/* The v2 file header, as lv_logLoad reads it. */
static void lvrcPutHeader(LvrcLog *b) {
    lvrcPut(b, "WBOLOMOV", 8);
    lvrcPutU8(b, LOG_VERSION_V2);
    lvrcPutU8(b, 4);
    lvrcPut(b, "Test", 4);
    lvrcPutU8(b, 1);            /* game type */
    lvrcPutU8(b, 0);            /* hidden mines */
    lvrcPutU8(b, 0);            /* ai */
    lvrcPutU8(b, 0);            /* password */
    lvrcPutU8(b, MAX_TANKS);    /* max players */
    lvrcPutU8(b, 2); lvrcPutU8(b, 0); lvrcPutU8(b, 2);   /* game version */
    lvrcPutZeros(b, 4 + 2);     /* ip + port */
    lvrcPutZeros(b, 4);         /* create time */
    lvrcPutZeros(b, 32);        /* WBN key */
}

/* A running round's opening snapshot: no items, one map run (which marks it
 * as carrying a world, so no lobby is in front of the stream) and sixteen
 * slots not in use. */
static void lvrcPutSnapshot(LvrcLog *b) {
    static const uint8_t mapRun[5] = {5, 0, 0, 2, (8 << 4) | GRASS};
    static const uint8_t mapTerminator[4] = {4, 255, 255, 255};
    int i;

    lvrcPutU8(b, LOG_SNAPSHOT);
    lvrcPutZeros(b, 8);                          /* start delay + game length */
    lvrcPutU8(b, 1); lvrcPutU8(b, 0);            /* pills: count 0 */
    lvrcPutU8(b, 1); lvrcPutU8(b, 0);            /* bases: count 0 */
    lvrcPutU8(b, 1); lvrcPutU8(b, 0);            /* starts: count 0 */
    lvrcPut(b, mapRun, sizeof(mapRun));
    lvrcPut(b, mapTerminator, sizeof(mapTerminator));
    for (i = 0; i < MAX_TANKS; i++) {
        lvrcPutU8(b, 2); lvrcPutU8(b, 0); lvrcPutU8(b, 0);
    }
}

/* One LOG_EVENT frame holding one log_RuleSet, framed by its own length. */
static void lvrcPutRuleSet(LvrcLog *b, const uint8_t *payload, size_t n) {
    lvrcPutU8(b, LOG_EVENT);
    lvrcPutU8(b, 1);
    lvrcPutU8(b, log_RuleSet);
    lvrcPutU8(b, (uint8_t)(n >> 8));
    lvrcPutU8(b, (uint8_t)(n & 0xFF));
    lvrcPut(b, payload, n);
}

static void lvrcPutNoEvents(LvrcLog *b, uint8_t waitLen) {
    lvrcPutU8(b, LOG_NOEVENTS);
    lvrcPutU8(b, waitLen);
}

#define LVRC_CAP_HI ((uint8_t)(SIM_RULE_pill_max_armour >> 8))
#define LVRC_CAP_LO ((uint8_t)(SIM_RULE_pill_max_armour & 0xFF))

/* Three records the viewer must not take, then one it must:
 *
 *   rule set, length byte 7            -> tick  1 (20 ms)
 *   rule set, index SIM_RULE_COUNT     -> tick  2 (40 ms)
 *   rule set, value a NaN              -> tick  3 (60 ms)
 *   NOEVENTS 10                        -> ticks 4..14
 *   rule set, pill_max_armour 30.0     -> tick 15 (300 ms)
 *   NOEVENTS 5, then LOG_QUIT
 *
 * 30.0 is 0x403E000000000000. */
static void lvrcBuildHostile(LvrcLog *b) {
    const uint8_t shortBlob[3 + 7] = {
        LVRC_CAP_HI, LVRC_CAP_LO, 7,
        0x40, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t noRule[3 + 8] = {
        (uint8_t)(SIM_RULE_COUNT >> 8), (uint8_t)(SIM_RULE_COUNT & 0xFF), 8,
        0x40, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t notFinite[3 + 8] = {
        LVRC_CAP_HI, LVRC_CAP_LO, 8,
        0x7F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t good[3 + 8] = {
        LVRC_CAP_HI, LVRC_CAP_LO, 8,
        0x40, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    b->len = 0;
    lvrcPutHeader(b);
    lvrcPutSnapshot(b);
    lvrcPutRuleSet(b, shortBlob, sizeof(shortBlob));
    lvrcPutRuleSet(b, noRule, sizeof(noRule));
    lvrcPutRuleSet(b, notFinite, sizeof(notFinite));
    lvrcPutNoEvents(b, 10);
    lvrcPutRuleSet(b, good, sizeof(good));
    lvrcPutNoEvents(b, 5);
    lvrcPutU8(b, LOG_QUIT);
}

#define LVRC_GOOD_MS 300u

/* The log.dat bytes as a .wbv on disk, read back into a heap buffer for
 * lv_screenLoadMapFromMemory, which takes ownership of it. */
static uint8_t *lvrcZipToMemory(const LvrcLog *b, const char *path,
                                size_t *outLen) {
    zipFile      zf;
    zip_fileinfo zi;
    FILE        *f;
    long         sz;
    uint8_t     *out;

    remove(path);
    memset(&zi, 0, sizeof(zi));
    zf = zipOpen(path, 0);
    if (zf == NULL) return NULL;
    if (zipOpenNewFileInZip(zf, "log.dat", &zi, NULL, 0, NULL, 0, "",
                            Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK ||
        zipWriteInFileInZip(zf, b->buf, (unsigned)b->len) != ZIP_OK ||
        zipCloseFileInZip(zf) != ZIP_OK) {
        zipClose(zf, NULL);
        return NULL;
    }
    if (zipClose(zf, NULL) != ZIP_OK) return NULL;

    f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    out = (uint8_t *)malloc((size_t)sz);
    if (out == NULL || fread(out, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(out);
        return NULL;
    }
    fclose(f);
    *outLen = (size_t)sz;
    return out;
}

/* Tick until playback ends; FALSE if it never does. */
static bool lvrcPlayToEnd(void) {
    int steps = 0;
    while (lv_screenIsPlaying() == TRUE && steps < 4096) {
        lv_screenLogTick();
        steps++;
    }
    return lv_screenIsPlaying() != TRUE;
}

/* ── 4. Hostile records, from a file ───────────────────────────────── */

int run_lv_rule_changes_hostile(void) {
    const char     *path = "lv_rule_changes_hostile.wbv";
    LvrcLog         b;
    LogViewerState *lv;
    uint8_t        *zipData;
    size_t          zipLen = 0;
    uint32_t        ms = 0;
    double          value = 0.0;
    bool            loaded;
    bool            ended;
    int             count;
    int             capBefore = -1;
    int             capAt = -1;

    lvrcBuildHostile(&b);
    zipData = lvrcZipToMemory(&b, path, &zipLen);
    UT_ASSERT_MSG(zipData != NULL, "the hand-built log could not be zipped");

    lv = lv_decoderCreate(false);
    if (lv == NULL) {
        free(zipData);
        remove(path);
        UT_FAIL("lv_decoderCreate returned NULL");
    }
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);

    /* Nothing is open yet, so there is no recording to describe. */
    if (lv_screenGetScripts() != NULL) {
        lv_decoderDestroy(lv);
        free(zipData);
        remove(path);
        UT_FAIL("lv_screenGetScripts answered before any log was loaded");
    }

    loaded = lv_screenLoadMapFromMemory(zipData, zipLen) == TRUE;
    count  = lvrcCountFor(SIM_RULE_pill_max_armour, &ms, &value);
    if (loaded) {
        /* Walk to one tick short of the good record, then onto it. */
        while (lv_screenIsPlaying() == TRUE &&
               lv_screenGetTimeRunning() < LVRC_GOOD_MS - 20) {
            lv_screenLogTick();
        }
        capBefore = lv->rules.pillMaxArmour;
        lv_screenLogTick();
        capAt = lv->rules.pillMaxArmour;
    }
    ended = loaded && lvrcPlayToEnd();

    UT_ASSERT_MSG(loaded, "the hand-built log failed to load");
    UT_ASSERT_MSG(lv_screenGetScripts() != NULL,
                  "lv_screenGetScripts answered NULL with the log open");
    UT_ASSERT_MSG(ended, "playback did not reach end-of-log past the records");
    UT_ASSERT_MSG(lv_screenGetRuleChanges(NULL) == 1,
                  "%d rule changes collected (want 1: only the good record)",
                  lv_screenGetRuleChanges(NULL));
    UT_ASSERT_MSG(count == 1 && ms == LVRC_GOOD_MS && value == 30.0,
                  "the load walk collected %d changes, first at %u ms of %g "
                  "(want one, at %u ms, of 30)", count, (unsigned)ms, value,
                  LVRC_GOOD_MS);
    UT_ASSERT_MSG(capBefore == 15,
                  "pillMaxArmour %d a tick before the good record (want 15)",
                  capBefore);
    UT_ASSERT_MSG(capAt == 30,
                  "pillMaxArmour %d on the good record's tick (want 30)",
                  capAt);
    UT_ASSERT_MSG(lv->rules.pillMaxArmour == 30,
                  "pillMaxArmour %d at end-of-log (want 30)",
                  lv->rules.pillMaxArmour);

    lv_decoderDestroy(lv);
    remove(path);
    UT_ASSERT_MSG(lv_screenGetScripts() == NULL,
                  "lv_screenGetScripts answered with the decoder gone");
    return 0;
}

/* ── 5. The same records on a live feed ────────────────────────────── */

int run_lv_rule_changes_live_feed(void) {
    LvrcLog         b;
    LogViewerState *lv;
    uint32_t        ms = 0;
    double          value = 0.0;
    bool            loaded;
    bool            ended;
    int             atLoad;
    int             count;

    lvrcBuildHostile(&b);
    lv = lv_decoderCreate(false);
    UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);

    /* A stream has no load walk: the list starts empty and fills as
       playback reaches each record. */
    loaded = lv_screenLoadFromStream(b.buf, b.len) == TRUE;
    atLoad = lv_screenGetRuleChanges(NULL);
    ended  = loaded && lvrcPlayToEnd();
    count  = lvrcCountFor(SIM_RULE_pill_max_armour, &ms, &value);

    UT_ASSERT_MSG(loaded, "the stream failed to load");
    UT_ASSERT_MSG(atLoad == 0, "%d rule changes before playback on a feed",
                  atLoad);
    UT_ASSERT_MSG(ended, "playback did not reach end-of-log on the feed");
    UT_ASSERT_MSG(lv_screenGetRuleChanges(NULL) == 1,
                  "%d rule changes on the feed (want 1)",
                  lv_screenGetRuleChanges(NULL));
    UT_ASSERT_MSG(count == 1 && ms == LVRC_GOOD_MS && value == 30.0,
                  "playback added %d changes, first at %u ms of %g (want one, "
                  "at %u ms, of 30)", count, (unsigned)ms, value, LVRC_GOOD_MS);
    UT_ASSERT_MSG(lv->rules.pillMaxArmour == 30,
                  "pillMaxArmour %d at end of the feed (want 30)",
                  lv->rules.pillMaxArmour);

    lv_decoderDestroy(lv);
    return 0;
}

/* ── 6. The pill picture against the cap ───────────────────────────── */

int run_lv_rule_changes_armour_levels(void) {
    static const struct {
        int  armour;
        int  cap;
        BYTE want;
    } rows[] = {
        {15, 15, 15}, {7, 15, 7},   {0, 30, 0},
        {30, 30, 15}, {29, 30, 14}, {15, 30, 7},
        {200, 30, 15}, {5, 0, 5},
    };
    size_t i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        BYTE got = lv_pillsArmourLevel((BYTE)rows[i].armour, rows[i].cap);
        UT_ASSERT_MSG(got == rows[i].want,
                      "armour %d against a cap of %d drew level %u (want %u)",
                      rows[i].armour, rows[i].cap, got, rows[i].want);
    }
    return 0;
}
