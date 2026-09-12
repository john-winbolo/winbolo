/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * The rules op:
 *
 *   run_scenario_rule_index_matches_table — the index list is exactly the
 *       SimRules fields, in the struct's own order, each named once
 *   run_scenario_rule_set              — a rule is written and the sim's
 *       table shows it, and nothing else in the table moves
 *   run_scenario_rule_set_float        — a rate the op's double can carry
 *       and an int32 could not, set and read back
 *   run_scenario_rule_refusals         — the value outside a row's range,
 *       the value that breaks a pair, the index that names no rule, and the
 *       table byte for byte across each of them
 *   run_scenario_rule_arm_records      — the record's bytes on disk, more
 *       records behind it, and both of the viewer's readers over them
 *
 * The byte-for-byte checks compare the whole table rather than the field the
 * op named: an arm that wrote its field and then refused would pass a check
 * that only looked at the field it was told about.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_scenario.h"
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "scenario_defs.h"         /* SCN_RULE_LIST, ScnRuleIndex */
#include "game_sim.h"              /* GameSim.rules */
#include "sim_rules.h"             /* SimRules, simRulesClassic */
#include "everard_map.h"           /* E_MAP */
#include "log.h"                   /* log_RuleSet, the stream opcodes */
#include "replay_harness.h"
#include "test_harness.h"

/* A rate whose classic value is 0.25, set to a quarter an int32_t could not
 * have carried either. Both are exact in a float and in a double, so the
 * read-back and the recorded bytes are exact numbers rather than nearly. */
#define RA_ACCEL_CLASSIC 0.25
#define RA_ACCEL_SET     0.75

/* An integer rule and a value well inside its 0..255 row. */
#define RA_RELOAD_SET    20

/* Outside tank_reload_ticks' row, but not outside the window a double can be
 * converted to a field in — this is the range check refusing it, not the
 * conversion guard. */
#define RA_RELOAD_RANGE  300.0

/* gunsight_min's own row is 1..255, so 20 passes every range check and then
 * meets the pair that says it cannot sit above gunsight_max, which classic
 * leaves at 14. The two refusals therefore differ in the check that caught
 * them and not in how far outside anything the value is. */
#define RA_GUNSIGHT_PAIR 20.0

/* ── Fixtures ─────────────────────────────────────────────────────────── */

/* A lobby that has not started. The arm takes no state, so a lobby is as
 * good a place to ask from as a round and is the cheaper sim to build. */
static ServerSim *raSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

static ScnOpResult raSetRule(ServerSim *sim, uint16_t rule, double value) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_SET_RULE;
    op.u.setRule.rule  = rule;
    op.u.setRule.value = value;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* ================================================================
 * 1. The index list against the struct it indexes.
 *
 * Each entry's offset and width are taken from SimRules itself, so a name
 * that is not a field does not compile. What the case adds is that the
 * offsets tile the struct from its first byte to its last with no gap and no
 * overlap, which is true only when the list holds every field, in the order
 * the struct declares them, and each of them once. The count assertion in
 * server_sim_scenario.c catches a field added without a line here; this
 * catches a line in the wrong place.
 * ================================================================ */

typedef struct {
    const char *name;
    size_t      offset;
    size_t      size;
} RaRuleField;

static const RaRuleField kRaRuleFields[] = {
#define RA_FIELD_ROW(name) { #name, offsetof(SimRules, name), \
                             sizeof(((SimRules *)0)->name) },
    SCN_RULE_LIST(RA_FIELD_ROW)
#undef RA_FIELD_ROW
};

int run_scenario_rule_index_matches_table(void) {
    size_t i;
    size_t expect = 0;

    UT_ASSERT_MSG(sizeof(kRaRuleFields) / sizeof(kRaRuleFields[0]) ==
                      (size_t)SCN_RULE_COUNT,
                  "the list holds %u entries and the enum counts %d",
                  (unsigned)(sizeof(kRaRuleFields) / sizeof(kRaRuleFields[0])),
                  (int)SCN_RULE_COUNT);

    for (i = 0; i < (size_t)SCN_RULE_COUNT; i++) {
        UT_ASSERT_MSG(kRaRuleFields[i].offset == expect,
                      "rule %u (%s) is at byte %u of SimRules, expected %u — "
                      "the list is not the struct's own field order",
                      (unsigned)i, kRaRuleFields[i].name,
                      (unsigned)kRaRuleFields[i].offset, (unsigned)expect);
        expect += kRaRuleFields[i].size;
    }
    UT_ASSERT_MSG(expect == sizeof(SimRules),
                  "the list covers %u bytes of a %u-byte table — a field is "
                  "missing from SCN_RULE_LIST", (unsigned)expect,
                  (unsigned)sizeof(SimRules));

    UT_ASSERT_MSG(kRaRuleFields[0].offset == 0,
                  "the first rule is at byte %u, not the front of the table",
                  (unsigned)kRaRuleFields[0].offset);
    return 0;
}

/* ================================================================
 * 2. A rule is written, and only that rule.
 * ================================================================ */
int run_scenario_rule_set(void) {
    ServerSim *sim = raSim();
    GameSim   *gs;
    SimRules   before;
    SimRules   expect;

    UT_ASSERT_MSG(sim != NULL, "raSim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "the sim has no GameSim");
    before = gs->rules;

    UT_ASSERT_MSG(raSetRule(sim, SCN_RULE_tank_reload_ticks,
                            (double)RA_RELOAD_SET) == SCN_OP_OK,
                  "a value inside tank_reload_ticks' row must be accepted");
    UT_ASSERT_MSG(gs->rules.tank_reload_ticks == RA_RELOAD_SET,
                  "tank_reload_ticks is %ld, expected %d",
                  (long)gs->rules.tank_reload_ticks, RA_RELOAD_SET);

    /* Every other field is where it was: the arm commits the whole copy, so
       a field it disturbed on the way would ship with the one it was asked
       about. */
    expect = before;
    expect.tank_reload_ticks = RA_RELOAD_SET;
    UT_ASSERT_MSG(memcmp(&expect, &gs->rules, sizeof(expect)) == 0,
                  "the write moved a field other than the one it named");

    /* An integer rule takes the whole part of the value it is given, and
       what it ends up holding is what reads back. */
    UT_ASSERT_MSG(raSetRule(sim, SCN_RULE_tank_reload_ticks, 30.7) ==
                      SCN_OP_OK,
                  "a value inside the row must be accepted whether or not it "
                  "is a whole number");
    UT_ASSERT_MSG(gs->rules.tank_reload_ticks == 30,
                  "tank_reload_ticks is %ld, expected 30",
                  (long)gs->rules.tank_reload_ticks);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. A float rule through the op.
 *
 * The payload is a double for this: tank_accel_rate holds 0.25 and an
 * int32_t payload could not have carried either that or the value set here.
 * ================================================================ */
int run_scenario_rule_set_float(void) {
    ServerSim *sim = raSim();
    GameSim   *gs;

    UT_ASSERT_MSG(sim != NULL, "raSim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "the sim has no GameSim");

    UT_ASSERT_MSG(gs->rules.tank_accel_rate == (float)RA_ACCEL_CLASSIC,
                  "setup: tank_accel_rate starts at %g, expected %g",
                  (double)gs->rules.tank_accel_rate, RA_ACCEL_CLASSIC);

    UT_ASSERT_MSG(raSetRule(sim, SCN_RULE_tank_accel_rate, RA_ACCEL_SET) ==
                      SCN_OP_OK,
                  "a rate inside tank_accel_rate's row must be accepted");
    UT_ASSERT_MSG(gs->rules.tank_accel_rate == (float)RA_ACCEL_SET,
                  "tank_accel_rate is %g, expected %g",
                  (double)gs->rules.tank_accel_rate, RA_ACCEL_SET);

    /* A fraction that is not a quarter reaches the field as well: the row
       allows it and the field is a float. */
    UT_ASSERT_MSG(raSetRule(sim, SCN_RULE_turn_river, 0.125) == SCN_OP_OK,
                  "a turn rate inside its row must be accepted");
    UT_ASSERT_MSG(gs->rules.turn_river == 0.125f,
                  "turn_river is %g, expected 0.125",
                  (double)gs->rules.turn_river);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. What the arm will not write, and what a refusal leaves behind.
 * ================================================================ */
int run_scenario_rule_refusals(void) {
    ServerSim *sim = raSim();
    GameSim   *gs;
    SimRules   before;

    UT_ASSERT_MSG(sim != NULL, "raSim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "the sim has no GameSim");
    before = gs->rules;

    /* Outside the row's own range. */
    UT_ASSERT_MSG(raSetRule(sim, SCN_RULE_tank_reload_ticks,
                            RA_RELOAD_RANGE) == SCN_OP_RANGE,
                  "a value outside tank_reload_ticks' row must be refused "
                  "SCN_OP_RANGE");
    UT_ASSERT_MSG(memcmp(&before, &gs->rules, sizeof(before)) == 0,
                  "a refused range left the table changed");

    /* Inside its own row, and against another field. */
    UT_ASSERT_MSG(gs->rules.gunsight_max < (int32_t)RA_GUNSIGHT_PAIR,
                  "setup: gunsight_max is %ld, so %g would not break the "
                  "pair", (long)gs->rules.gunsight_max, RA_GUNSIGHT_PAIR);
    UT_ASSERT_MSG(raSetRule(sim, SCN_RULE_gunsight_min, RA_GUNSIGHT_PAIR) ==
                      SCN_OP_PAIR,
                  "a gunsight minimum above the maximum must be refused "
                  "SCN_OP_PAIR — its own row allows the number");
    UT_ASSERT_MSG(memcmp(&before, &gs->rules, sizeof(before)) == 0,
                  "a refused pair left the table changed");

    /* An index that names no rule. */
    UT_ASSERT_MSG(raSetRule(sim, (uint16_t)SCN_RULE_COUNT, 1.0) ==
                      SCN_OP_NO_SUCH_ITEM,
                  "one past the last rule names no rule");
    UT_ASSERT_MSG(raSetRule(sim, 0xFFFF, 1.0) == SCN_OP_NO_SUCH_ITEM,
                  "an index nowhere near the table names no rule");
    UT_ASSERT_MSG(memcmp(&before, &gs->rules, sizeof(before)) == 0,
                  "a refused index left the table changed");

    /* A value the conversion could not make at all, refused before the write
       rather than wrapped into the field. */
    UT_ASSERT_MSG(raSetRule(sim, SCN_RULE_tree_grow_ticks, 1e300) ==
                      SCN_OP_RANGE,
                  "a value past the width of every field must be refused");
    UT_ASSERT_MSG(memcmp(&before, &gs->rules, sizeof(before)) == 0,
                  "a refused conversion left the table changed");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 5. The record the arm leaves, on disk and through the viewer.
 *
 * Two rule changes and then a server line, so there is a second rule record
 * and a record of another type behind the first — each of them only found
 * where it is if the record in front of it was the length it said. The
 * expected bytes are written out here rather than encoded by the writer under
 * test: 0.75 and 20 are 0x3FE8000000000000 and 0x4034000000000000 as
 * IEEE-754 doubles.
 *
 * The idle ticks after the two ops are what the viewer's byte walk is
 * measured against. The walk sizes every record by its own type, with no
 * framed length to fall back on, so a walk that cannot size a rule record
 * stops at the first one and reports a file a fraction as long as it is.
 * ================================================================ */

#define RA_TAG          "scnRuleArms"
#define RA_PLAYER       "Scripter"
#define RA_IDLE_TICKS   100   /* sim ticks; two of them to a recorded tick */
#define RA_MIN_TOTAL_MS 800   /* far above the few ticks before the records */
#define RA_RECORD_LEN   11    /* rule u16 + length byte + eight value bytes */

static const uint8_t kRaAccelBytes[8] = {
    0x3F, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t kRaReloadBytes[8] = {
    0x40, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

#define RA_MAX_HITS 8

typedef struct {
    int     count;                            /* log_RuleSet records found */
    int     others;                           /* records of any other type */
    uint8_t payload[RA_MAX_HITS][RA_RECORD_LEN];
    int     payloadLen[RA_MAX_HITS];
} RaLogHits;

static int raReadByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Skip a snapshot body: startDelay+timeLimit, the count-prefixed pills, bases
 * and starts, the map runs up to the deep-sea terminator, then MAX_TANKS
 * player blocks. Plaintext, not length-framed. */
static bool raSkipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    int n, i;
    if (p + 8 > len) return false;
    p += 8;
    if ((n = raReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = raReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = raReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    for (;;) {
        int dlen, y, sx, ex;
        if (p + 4 > len) return false;
        dlen = raReadByte(buf, len, p);
        y    = raReadByte(buf, len, p + 1);
        sx   = raReadByte(buf, len, p + 2);
        ex   = raReadByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if ((n = raReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Walk the .wbv's event stream and collect every log_RuleSet in it, in the
 * order it was written, counting everything else on the way past. Returns
 * false if the stream did not end on a clean LOG_QUIT. */
static bool raCollect(const char *path, RaLogHits *hits) {
    uint8_t *buf = NULL;
    size_t   len = 0;
    size_t   pos;
    bool     ok = false;

    memset(hits, 0, sizeof(*hits));
    if (!extractLogDat(path, &buf, &len)) return false;
    /* Header: WBOLOMOV(8) + version(1) + mapname pstr + game(8) + addr(4) +
       port(2) + time(4) + WBN key(32). */
    if (len < 10 || memcmp(buf, "WBOLOMOV", 8) != 0 || buf[8] != LOG_VERSION) {
        free(buf);
        return false;
    }
    pos = 8 + 1;
    pos += 1 + buf[pos];
    pos += 8 + 4 + 2 + 4 + 32;

    while (pos < len) {
        int code = raReadByte(buf, len, pos);
        pos++;
        if (code < 0) break;
        if (code == LOG_QUIT) {
            ok = true;
            break;
        } else if (code == LOG_NOEVENTS) {
            if (raReadByte(buf, len, pos) < 0) break;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) break;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!raSkipSnapshot(buf, len, &pos)) break;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n, i;
            if (code == LOG_EVENT) {
                n = raReadByte(buf, len, pos);
                pos += 1;
                if (n < 0) break;
            } else {
                if (pos + 2 > len) break;
                /* The writer stores data[1]=low, data[2]=high and the reader
                   rebuilds the count as (lo << 8) | hi. */
                n = (buf[pos + 1] << 8) | buf[pos];
                pos += 2;
            }
            for (i = 0; i < n; i++) {
                int ev, plen;
                size_t payloadStart;
                if (pos + 3 > len) { n = -1; break; }
                ev   = buf[pos];
                plen = (buf[pos + 1] << 8) | buf[pos + 2];
                payloadStart = pos + 3;
                if (payloadStart + (size_t)plen > len) { n = -1; break; }
                if (ev == (int)log_RuleSet) {
                    if (hits->count < RA_MAX_HITS) {
                        int copy = plen;
                        if (copy > RA_RECORD_LEN) copy = RA_RECORD_LEN;
                        hits->payloadLen[hits->count] = plen;
                        memcpy(hits->payload[hits->count], buf + payloadStart,
                               (size_t)copy);
                    }
                    hits->count++;
                } else {
                    hits->others++;
                }
                pos = payloadStart + (size_t)plen;
            }
            if (n < 0) break;
        } else {
            break;
        }
    }

    free(buf);
    return ok;
}

/* One recorded rule change against the bytes it should have written. */
static int raCheckRecord(const RaLogHits *hits, int which, uint16_t rule,
                         const uint8_t *value, const char *what) {
    const uint8_t *p = hits->payload[which];
    int i;

    UT_ASSERT_MSG(hits->payloadLen[which] == RA_RECORD_LEN,
                  "the %s record is %d payload byte(s), expected %d",
                  what, hits->payloadLen[which], RA_RECORD_LEN);
    UT_ASSERT_MSG(p[0] == (uint8_t)(rule >> 8) && p[1] == (uint8_t)(rule & 0xFF),
                  "the %s record names rule %u, expected %u", what,
                  (unsigned)((p[0] << 8) | p[1]), (unsigned)rule);
    UT_ASSERT_MSG(p[2] == 8,
                  "the %s record's value is %u bytes long, expected 8",
                  what, (unsigned)p[2]);
    for (i = 0; i < 8; i++) {
        UT_ASSERT_MSG(p[3 + i] == value[i],
                      "the %s record's value byte %d is 0x%02X, expected "
                      "0x%02X", what, i, (unsigned)p[3 + i],
                      (unsigned)value[i]);
    }
    return 0;
}

/* A line, so the stream holds a record of another type behind the two rule
 * records. A rule record sized wrong lands in the middle of this one. */
static ScnOpResult raSayLine(ServerSim *sim, const char *text) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MSG_ALL;
    SDL_strlcpy(op.u.msgAll.text, text, sizeof(op.u.msgAll.text));
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

int run_scenario_rule_arm_records(void) {
    ReplayHarness   h;
    RaLogHits       hits;
    ReplayWorld    *world;
    ReplayFileInfo  info;
    bool            decoded;
    int             rc;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, RA_TAG, RA_PLAYER),
                  "could not start recording");

    /* Let the round settle after the opening snapshot. */
    replayHarnessTick(&h, 4);

    UT_ASSERT(raSetRule(h.sim, SCN_RULE_tank_accel_rate, RA_ACCEL_SET) ==
              SCN_OP_OK);
    UT_ASSERT(raSetRule(h.sim, SCN_RULE_tank_reload_ticks,
                        (double)RA_RELOAD_SET) == SCN_OP_OK);
    /* A refused change writes nothing, so the file holds two records and not
       three. */
    UT_ASSERT(raSetRule(h.sim, SCN_RULE_tank_reload_ticks, RA_RELOAD_RANGE) ==
              SCN_OP_RANGE);
    UT_ASSERT(raSayLine(h.sim, "rules changed") == SCN_OP_OK);

    /* The ticks the byte walk is measured over, and what flushes the two
       records into the stream. */
    replayHarnessTick(&h, RA_IDLE_TICKS);

    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");
    UT_ASSERT_MSG(raCollect(h.path, &hits),
                  "the recording did not end on a clean quit: %s", h.path);
    UT_ASSERT_MSG(hits.count == 2,
                  "the recording holds %d rule record(s), expected 2",
                  hits.count);
    UT_ASSERT_MSG(hits.others > 0,
                  "the recording holds nothing but rule records, so a length "
                  "mistake would have nothing behind it to land in");

    rc = raCheckRecord(&hits, 0, SCN_RULE_tank_accel_rate, kRaAccelBytes,
                       "rate");
    if (rc != 0) return rc;
    rc = raCheckRecord(&hits, 1, SCN_RULE_tank_reload_ticks, kRaReloadBytes,
                       "reload");
    if (rc != 0) return rc;

    /* And through the viewer: playback reads every record to end-of-log, and
       the load's byte walk sizes them all well enough to reach the end of the
       file rather than stopping at the first rule record. */
    world = (ReplayWorld *)malloc(sizeof(ReplayWorld));
    UT_ASSERT_MSG(world != NULL, "could not allocate a decoded world");
    memset(&info, 0, sizeof(info));
    decoded = replayHarnessDecodeFile(h.path, world, &info);
    free(world);
    UT_ASSERT_MSG(decoded,
                  "the viewer did not play the recording to its end: %s",
                  h.path);
    UT_ASSERT_MSG(info.totalTimeMs >= RA_MIN_TOTAL_MS,
                  "the viewer's byte walk made the recording %ums long, "
                  "expected at least %ums — the walk stopped at a record it "
                  "could not size", (unsigned)info.totalTimeMs,
                  RA_MIN_TOTAL_MS);

    replayHarnessStop(&h);
    return 0;
}
