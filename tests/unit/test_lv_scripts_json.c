/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The log viewer reading a recording's scripts.json into its LvScripts
 * holder.
 *
 *   lv_scripts_json_scripted_round — a real scripted round (the map's own
 *       scenario and a mod) decodes with both scripts in load order, the
 *       scenario's rule resolved to its index and its region.
 *   lv_scripts_json_plain_round — a plain round has no member: the holder is
 *       empty and the round plays to end-of-log.
 *   lv_scripts_json_old_recording — the committed v2 fixture, written before
 *       the member existed, opens with the holder empty.
 *   lv_scripts_json_malformed — a member that is not JSON leaves the holder
 *       empty and the round still plays.
 *   lv_scripts_json_wrong_version — a member of another version is not read.
 *   lv_scripts_json_hostile_values — twelve script rows keep ten, an unknown
 *       rule and an infinite value are dropped, a region off the map, one
 *       that runs off its edge and an empty one are dropped, one that ends on
 *       the edge is kept, and an over-long string is cut to its buffer.
 *   lv_scripts_json_over_cap — a member one byte over SCN_RECORD_TEXT_MAX is
 *       refused whole and the round still plays.
 *   lv_scripts_json_close_clears — a rule the member set is read at the
 *       playhead while the recording is open, and reads classic again once it
 *       is closed.
 *
 * The members of the last four are written by hand below and added to a
 * plain recording with minizip's append mode; none is produced by the
 * writer. The scripted round is recorded on the sim side by
 * lv_scripts_fixture.c, because this file reads viewer headers.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "sim_rules_names.h"   /* simRulesRuleIndex */
#include "scripts_record.h"    /* SCRIPTS_RECORD_MEMBER, SCN_RECORD_TEXT_MAX */
#include "zip.h"
#include "replay_harness.h"
#include "lv_scripts_fixture.h"
#include "test_harness.h"

#ifndef WB_WBV_FIXTURE_DIR
#define WB_WBV_FIXTURE_DIR "tests/fixtures/wbv"
#endif

/* One holder per case, off the stack: it is a few tens of kilobytes. */
static LvScripts lvsjGot;

/* Decode path through the harness into lvsjGot. The holder is set to a
 * marker first — count -1 — so a case can tell "the file opened and carried
 * nothing" from "the file never opened". *played is whether playback reached
 * end-of-log. */
static bool lvsjDecode(const char *path, bool *played) {
    ReplayWorld   *w;
    ReplayFileInfo info;

    memset(&lvsjGot, 0, sizeof(lvsjGot));
    lvsjGot.count   = -1;
    lvsjGot.present = true;
    *played = false;

    w = (ReplayWorld *)malloc(sizeof(ReplayWorld));
    if (w == NULL) return false;
    *played = replayHarnessDecodeFileScripts(path, w, &info, &lvsjGot);
    free(w);
    return lvsjGot.count != -1;
}

/* A plain round recorded through the harness. */
static bool lvsjPlainRound(ReplayHarness *h, const char *tag) {
    if (!replayHarnessStartRecording(h, tag, "Tester")) return false;
    replayHarnessTick(h, 6);
    return replayHarnessStopRecording(h);
}

/* text added to the archive at path as scripts.json. */
static bool lvsjAppendMember(const char *path, const char *text, size_t len) {
    zipFile      zf;
    zip_fileinfo zi;
    bool         ok;

    zf = zipOpen(path, APPEND_STATUS_ADDINZIP);
    if (zf == NULL) return false;
    memset(&zi, 0, sizeof(zi));
    if (zipOpenNewFileInZip(zf, SCRIPTS_RECORD_MEMBER, &zi, NULL, 0, NULL, 0,
                            NULL, Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK) {
        zipClose(zf, NULL);
        return false;
    }
    ok = zipWriteInFileInZip(zf, text, (unsigned)len) == ZIP_OK;
    if (zipCloseFileInZip(zf) != ZIP_OK) ok = false;
    if (zipClose(zf, NULL) != ZIP_OK) ok = false;
    return ok;
}

/* A plain round with text added as its scripts.json, decoded. */
static bool lvsjDecodeWithMember(const char *tag, const char *text, size_t len,
                                 bool *opened, bool *played) {
    ReplayHarness h;

    *opened = false;
    *played = false;
    if (!lvsjPlainRound(&h, tag)) {
        replayHarnessStop(&h);
        return false;
    }
    if (!lvsjAppendMember(h.path, text, len)) {
        replayHarnessStop(&h);
        return false;
    }
    *opened = lvsjDecode(h.path, played);
    replayHarnessStop(&h);
    return true;
}

/* ── 1. A scripted round ───────────────────────────────────────────── */

int run_lv_scripts_json_scripted_round(void) {
    char  path[512];
    bool  recorded;
    bool  opened;
    bool  played;
    const LvScriptRow    *s;
    const LvScriptRegion *r;

    recorded = lvScriptsRecordScriptedRound("scripted", path, sizeof(path));
    if (!recorded) {
        if (path[0] != '\0') remove(path);
        UT_FAIL("the scripted round could not be recorded");
    }
    opened = lvsjDecode(path, &played);
    remove(path);

    UT_ASSERT_MSG(opened, "the recording did not open");
    UT_ASSERT_MSG(played, "the recording did not play to end-of-log");
    UT_ASSERT_MSG(lvsjGot.present, "the holder is empty");
    UT_ASSERT_MSG(strcmp(lvsjGot.map, LVSF_MAP_LEAF) == 0,
                  "map is \"%s\", wanted %s", lvsjGot.map, LVSF_MAP_LEAF);
    UT_ASSERT_MSG(lvsjGot.modsEnabled, "modsEnabled is false");

    UT_ASSERT_MSG(lvsjGot.count == 2, "count is %d, wanted 2", lvsjGot.count);
    s = &lvsjGot.scripts[0];
    UT_ASSERT_MSG(strcmp(s->file, LVSF_MAP_SCRIPT) == 0 &&
                      strcmp(s->source, "map") == 0 &&
                      strcmp(s->kind, "scenario") == 0 &&
                      strcmp(s->name, LVSF_SCENARIO_NAME) == 0,
                  "script 0 is %s/%s/%s/%s, wanted the map's scenario",
                  s->file, s->source, s->kind, s->name);
    s = &lvsjGot.scripts[1];
    UT_ASSERT_MSG(strcmp(s->file, LVSF_MOD_FILE) == 0 &&
                      strcmp(s->source, "server") == 0 &&
                      strcmp(s->kind, "mod") == 0 &&
                      strcmp(s->name, LVSF_MOD_NAME) == 0,
                  "script 1 is %s/%s/%s/%s, wanted the server's mod",
                  s->file, s->source, s->kind, s->name);

    UT_ASSERT_MSG(lvsjGot.ruleCount == 1, "ruleCount is %d, wanted 1",
                  lvsjGot.ruleCount);
    UT_ASSERT_MSG(lvsjGot.rules[0].index == simRulesRuleIndex(LVSF_RULE_NAME) &&
                      lvsjGot.rules[0].value == (double)LVSF_RULE_VALUE,
                  "rule 0 is index %d value %g, wanted %s = %d",
                  lvsjGot.rules[0].index, lvsjGot.rules[0].value,
                  LVSF_RULE_NAME, LVSF_RULE_VALUE);

    UT_ASSERT_MSG(lvsjGot.regionCount == 1, "regionCount is %d, wanted 1",
                  lvsjGot.regionCount);
    r = &lvsjGot.regions[0];
    UT_ASSERT_MSG(strcmp(r->name, LVSF_REGION_NAME) == 0 &&
                      strcmp(r->file, LVSF_MAP_SCRIPT) == 0 &&
                      r->x == LVSF_REGION_X && r->y == LVSF_REGION_Y &&
                      r->w == LVSF_REGION_W && r->h == LVSF_REGION_H,
                  "region 0 is %s %u,%u %ux%u from %s", r->name,
                  (unsigned)r->x, (unsigned)r->y, (unsigned)r->w,
                  (unsigned)r->h, r->file);
    return 0;
}

/* ── 2. A plain round ──────────────────────────────────────────────── */

int run_lv_scripts_json_plain_round(void) {
    ReplayHarness h;
    bool          opened;
    bool          played;

    if (!lvsjPlainRound(&h, "lvScriptsPlain")) {
        replayHarnessStop(&h);
        UT_FAIL("could not record the plain round");
    }
    opened = lvsjDecode(h.path, &played);
    replayHarnessStop(&h);

    UT_ASSERT_MSG(opened, "the recording did not open");
    UT_ASSERT_MSG(played, "the plain round did not play to end-of-log");
    UT_ASSERT_MSG(!lvsjGot.present && lvsjGot.count == 0,
                  "a plain round filled the holder");
    return 0;
}

/* ── 3. A recording from before the member ─────────────────────────── */

int run_lv_scripts_json_old_recording(void) {
    const char *dir = getenv("WB_WBV_FIXTURE_DIR");
    char        path[512];
    bool        opened;
    bool        played;

    if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;
    snprintf(path, sizeof(path), "%s/spectator_v2.wbv", dir);

    /* Whether it plays to its end is the fixture's own business and not
       this case's; that it opens and reads no scripts is. */
    opened = lvsjDecode(path, &played);
    (void)played;
    UT_ASSERT_MSG(opened, "the v2 fixture did not open: %s", path);
    UT_ASSERT_MSG(!lvsjGot.present && lvsjGot.count == 0,
                  "an old recording filled the holder");
    return 0;
}

/* ── 4. A member that is not JSON ──────────────────────────────────── */

int run_lv_scripts_json_malformed(void) {
    static const char kText[] = "{\"version\":1,\"scripts\":[";
    bool opened;
    bool played;

    UT_ASSERT_MSG(lvsjDecodeWithMember("lvScriptsMalformed", kText,
                                       sizeof(kText) - 1, &opened, &played),
                  "could not build the recording");
    UT_ASSERT_MSG(opened, "the recording did not open");
    UT_ASSERT_MSG(played, "a malformed member stopped playback");
    UT_ASSERT_MSG(!lvsjGot.present && lvsjGot.count == 0,
                  "a malformed member filled the holder");
    return 0;
}

/* ── 5. A member of another version ────────────────────────────────── */

int run_lv_scripts_json_wrong_version(void) {
    static const char kText[] =
        "{\"version\":2,\"map\":\"Other.map\",\"mods_enabled\":true,"
        "\"rules\":{\"tank_death_ticks\":400},"
        "\"regions\":[{\"name\":\"keep\",\"x\":1,\"y\":2,\"w\":3,\"h\":4,"
        "\"file\":\"a.lua\"}],"
        "\"scripts\":[{\"file\":\"a.lua\",\"source\":\"map\","
        "\"kind\":\"scenario\",\"manifest\":{\"name\":\"A\"}}]}";
    bool opened;
    bool played;

    UT_ASSERT_MSG(lvsjDecodeWithMember("lvScriptsVersion2", kText,
                                       sizeof(kText) - 1, &opened, &played),
                  "could not build the recording");
    UT_ASSERT_MSG(opened, "the recording did not open");
    UT_ASSERT_MSG(played, "a version 2 member stopped playback");
    UT_ASSERT_MSG(!lvsjGot.present && lvsjGot.count == 0 &&
                      lvsjGot.ruleCount == 0 && lvsjGot.regionCount == 0 &&
                      lvsjGot.map[0] == '\0',
                  "a version 2 member was read");
    return 0;
}

/* ── 6. Values past what the holder takes ──────────────────────────── */

#define LVSJ_ROW(n) \
    "{\"file\":\"s" #n ".lua\",\"source\":\"server\",\"kind\":\"mod\"," \
    "\"manifest\":{\"name\":\"S" #n "\"}}"
#define LVSJ_M20 "mmmmmmmmmmmmmmmmmmmm"
#define LVSJ_M200 LVSJ_M20 LVSJ_M20 LVSJ_M20 LVSJ_M20 LVSJ_M20 \
                  LVSJ_M20 LVSJ_M20 LVSJ_M20 LVSJ_M20 LVSJ_M20

int run_lv_scripts_json_hostile_values(void) {
    static const char kText[] =
        "{\"version\":1,"
        "\"map\":\"" LVSJ_M200 "\","
        "\"mods_enabled\":\"yes\","
        "\"rules\":{\"no_such_rule\":5,\"tank_death_ticks\":400,"
        "\"tank_reload_ticks\":\"fast\",\"pill_max_armour\":1e999},"
        "\"regions\":["
        "{\"name\":\"far\",\"x\":300,\"y\":1,\"w\":1,\"h\":1,\"file\":\"s1.lua\"},"
        "{\"name\":\"half\",\"x\":1.5,\"y\":1,\"w\":1,\"h\":1,\"file\":\"s1.lua\"},"
        "{\"name\":\"keep\",\"x\":10,\"y\":12,\"w\":6,\"h\":4,\"file\":\"s1.lua\"},"
        "{\"name\":\"overrun\",\"x\":200,\"y\":1,\"w\":200,\"h\":1,\"file\":\"s1.lua\"},"
        "{\"name\":\"tall\",\"x\":1,\"y\":100,\"w\":1,\"h\":157,\"file\":\"s1.lua\"},"
        "{\"name\":\"thin\",\"x\":5,\"y\":5,\"w\":0,\"h\":3,\"file\":\"s1.lua\"},"
        "{\"name\":\"flat\",\"x\":5,\"y\":5,\"w\":3,\"h\":0,\"file\":\"s1.lua\"},"
        "{\"name\":\"edge\",\"x\":1,\"y\":0,\"w\":255,\"h\":2,\"file\":\"s1.lua\"}"
        "],"
        "\"scripts\":["
        LVSJ_ROW(1) "," LVSJ_ROW(2) "," LVSJ_ROW(3) "," LVSJ_ROW(4) ","
        LVSJ_ROW(5) "," LVSJ_ROW(6) "," LVSJ_ROW(7) "," LVSJ_ROW(8) ","
        LVSJ_ROW(9) "," LVSJ_ROW(10) "," LVSJ_ROW(11) "," LVSJ_ROW(12)
        "]}";
    bool opened;
    bool played;

    UT_ASSERT_MSG(lvsjDecodeWithMember("lvScriptsHostile", kText,
                                       sizeof(kText) - 1, &opened, &played),
                  "could not build the recording");
    UT_ASSERT_MSG(opened, "the recording did not open");
    UT_ASSERT_MSG(played, "the member stopped playback");
    UT_ASSERT_MSG(lvsjGot.present, "a version 1 member was not read");

    /* Twelve rows, ten kept, in order. */
    UT_ASSERT_MSG(lvsjGot.count == LV_SCRIPTS_MAX,
                  "count is %d, wanted %d", lvsjGot.count, LV_SCRIPTS_MAX);
    UT_ASSERT_MSG(strcmp(lvsjGot.scripts[0].file, "s1.lua") == 0 &&
                      strcmp(lvsjGot.scripts[9].file, "s10.lua") == 0 &&
                      strcmp(lvsjGot.scripts[9].name, "S10") == 0,
                  "the kept rows are not the first ten in order");

    /* The unknown rule, the rule that is not a number and the rule whose
       value is infinite are dropped. */
    UT_ASSERT_MSG(lvsjGot.ruleCount == 1, "ruleCount is %d, wanted 1",
                  lvsjGot.ruleCount);
    UT_ASSERT_MSG(lvsjGot.rules[0].index ==
                          simRulesRuleIndex("tank_death_ticks") &&
                      lvsjGot.rules[0].value == 400.0,
                  "the kept rule is not tank_death_ticks = 400");

    /* x = 300 and x = 1.5 are dropped, as are a region that runs off the
       right edge, one that runs off the bottom and the two with no area. The
       region on the map is kept, and so is the one that ends on the edge. */
    UT_ASSERT_MSG(lvsjGot.regionCount == 2, "regionCount is %d, wanted 2",
                  lvsjGot.regionCount);
    UT_ASSERT_MSG(strcmp(lvsjGot.regions[0].name, "keep") == 0 &&
                      lvsjGot.regions[0].x == 10 && lvsjGot.regions[0].y == 12,
                  "the first kept region is not keep at 10,12");
    UT_ASSERT_MSG(strcmp(lvsjGot.regions[1].name, "edge") == 0 &&
                      lvsjGot.regions[1].x == 1 && lvsjGot.regions[1].w == 255,
                  "the second kept region is not edge at x 1, 255 wide");

    /* A 200-character map name is cut to the buffer and terminated. */
    UT_ASSERT_MSG(strlen(lvsjGot.map) == LV_SCRIPTS_MAP_LEN - 1,
                  "map is %zu characters, wanted %d", strlen(lvsjGot.map),
                  LV_SCRIPTS_MAP_LEN - 1);

    /* mods_enabled that is not a bool reads false. */
    UT_ASSERT_MSG(!lvsjGot.modsEnabled, "a string read as mods enabled");
    return 0;
}

/* ── 7. A member over the cap ──────────────────────────────────────── */

int run_lv_scripts_json_over_cap(void) {
    static const char kHead[] = "{\"version\":1,\"map\":\"";
    static const char kTail[] = "\"}";
    const size_t      total   = (size_t)SCN_RECORD_TEXT_MAX + 1;
    size_t            pad;
    char             *text;
    bool              built;
    bool              opened;
    bool              played;

    /* Valid JSON of version 1, one byte longer than the writer ever writes:
       read, it would fill the holder, so an empty one is the cap at work. */
    text = (char *)malloc(total);
    UT_ASSERT_MSG(text != NULL, "no memory for the over-cap member");
    pad = total - (sizeof(kHead) - 1) - (sizeof(kTail) - 1);
    memcpy(text, kHead, sizeof(kHead) - 1);
    memset(text + sizeof(kHead) - 1, 'x', pad);
    memcpy(text + sizeof(kHead) - 1 + pad, kTail, sizeof(kTail) - 1);

    built = lvsjDecodeWithMember("lvScriptsOverCap", text, total, &opened,
                                 &played);
    free(text);
    UT_ASSERT_MSG(built, "could not build the recording");
    UT_ASSERT_MSG(opened, "the recording did not open");
    UT_ASSERT_MSG(played, "an over-cap member stopped playback");
    UT_ASSERT_MSG(!lvsjGot.present && lvsjGot.map[0] == '\0',
                  "a member over SCN_RECORD_TEXT_MAX was read");
    return 0;
}

/* ── 8. Closing the recording drops its scripts ───────────────────── */

/* The .wbv at path read into a heap buffer, which
   lv_screenLoadMapFromMemory takes ownership of. */
static uint8_t *lvsjReadFile(const char *path, size_t *outLen) {
    FILE    *f = fopen(path, "rb");
    long     sz;
    uint8_t *out;

    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    out = (sz > 0) ? (uint8_t *)malloc((size_t)sz) : NULL;
    if (out == NULL || fread(out, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(out);
        return NULL;
    }
    fclose(f);
    *outLen = (size_t)sz;
    return out;
}

int run_lv_scripts_json_close_clears(void) {
    static const char kText[] =
        "{\"version\":1,\"rules\":{\"tank_death_ticks\":400}}";
    ReplayHarness   h;
    LogViewerState *lv;
    uint8_t        *data;
    size_t          len = 0;
    int             rule = simRulesRuleIndex("tank_death_ticks");
    double          open;
    double          closed;
    bool            loaded;

    UT_ASSERT_MSG(rule >= 0, "tank_death_ticks has no index");
    UT_ASSERT_MSG(simRulesClassicValue(rule) != 400.0,
                  "the classic value is the one the member sets");
    if (!lvsjPlainRound(&h, "lvScriptsClose") ||
        !lvsjAppendMember(h.path, kText, sizeof(kText) - 1)) {
        replayHarnessStop(&h);
        UT_FAIL("could not build the recording");
    }
    data = lvsjReadFile(h.path, &len);
    replayHarnessStop(&h);
    UT_ASSERT_MSG(data != NULL, "could not read the recording back");

    lv = lv_decoderCreate(false);
    if (lv == NULL) {
        free(data);
        UT_FAIL("lv_decoderCreate returned NULL");
    }
    loaded = lv_screenLoadMapFromMemory(data, len) == TRUE;
    open   = lv_screenRuleValueAt(rule, 0);
    lv_screenCloseLog();
    closed = lv_screenRuleValueAt(rule, 0);
    lv_decoderDestroy(lv);

    UT_ASSERT_MSG(loaded, "the recording did not open");
    UT_ASSERT_MSG(open == 400.0, "tank_death_ticks read %g while open "
                  "(want 400)", open);
    UT_ASSERT_MSG(closed == simRulesClassicValue(rule),
                  "tank_death_ticks read %g after the close (want the classic "
                  "%g)", closed, simRulesClassicValue(rule));
    return 0;
}
