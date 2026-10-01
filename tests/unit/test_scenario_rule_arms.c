/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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
 *   run_scenario_rule_clamps_world     — a cap pulled under the records the
 *       sim is holding brings them down to it, a rule that strands nothing
 *       leaves both lists alone, and a cap put back up hands nothing back
 *   run_scenario_rule_clamp_records    — what the clamp moved reaches the
 *       replay, so the viewer holds the new armour rather than the old
 *
 * The byte-for-byte checks compare the whole table rather than the field the
 * op named: an arm that wrote its field and then refused would pass a check
 * that only looked at the field it was told about. The two clamp cases
 * compare the whole pill and base lists for the same reason.
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
#include "game_sim.h"              /* GameSim.rules, pb and bs */
#include "sim_rules.h"             /* SimRules, simRulesClassic */
#include "pillbox.h"               /* the pill list a lowered cap reaches */
#include "bases.h"                 /* the base list a lowered cap reaches */
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

/* Caps under what the map's own pills and bases carry, and inside every row
 * and pair that names them: pill_repair_amount is 4, so a pill cap of 8 sits
 * above it, and base_min_shells is 0 with base_shells_give 1, so a shells cap
 * of 40 leaves both of those alone. Each case checks the map really is
 * carrying records above them rather than assuming it. */
#define RA_PILL_ARMOUR_CAP 8
#define RA_BASE_SHELLS_CAP 40

/* ── Fixtures ─────────────────────────────────────────────────────────── */

/* A lobby that has not started. The arm takes no state, so a lobby is as
 * good a place to ask from as a round and is the cheaper sim to build. */
static ServerSim *raSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
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
#define RA_FIELD_ROW(name, kind, unit)                                       \
    { #name, offsetof(SimRules, name), sizeof(((SimRules *)0)->name) },
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

/* ================================================================
 * 6. A cap the new table lowers, against the records the sim holds.
 *
 * Three questions, because the clamp is a shared pass called on every rule
 * rather than on the ones that bear on a cap: a cap pulled under the map's
 * own records brings each of them down to it and leaves the ones already
 * below it where they are; a change that strands nothing moves neither list;
 * and a cap put back up hands nothing back, because a clamp is a ceiling and
 * not a setting.
 *
 * The untouched checks compare the whole pill and base lists rather than the
 * two fields the caps name: a pass that wrote a field it was not asked about
 * would walk past a check that only looked at armour and shells.
 * ================================================================ */

/* Both lists against the copies taken before the change. */
static int raListsUnmoved(GameSim *gs, const struct pillsObj *pills,
                          const struct basesObj *bases, const char *what) {
    UT_ASSERT_MSG(memcmp(pills, gs->pb, sizeof(*pills)) == 0,
                  "%s moved a pillbox record", what);
    UT_ASSERT_MSG(memcmp(bases, gs->bs, sizeof(*bases)) == 0,
                  "%s moved a base record", what);
    return 0;
}

int run_scenario_rule_clamps_world(void) {
    ServerSim      *sim = raSim();
    GameSim        *gs;
    struct pillsObj beforePills;
    struct basesObj beforeBases;
    BYTE            pillWas[MAX_PILLS];
    BYTE            baseWas[MAX_BASES];
    BYTE            numPills, numBases, i;
    int32_t         pillCapWas;
    int             pillsAbove = 0;
    int             basesAbove = 0;
    int             rc;

    UT_ASSERT_MSG(sim != NULL, "raSim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "the sim has no GameSim");

    numPills   = pillsGetNumPills(&gs->pb);
    numBases   = basesGetNumBases(&gs->bs);
    pillCapWas = gs->rules.pill_max_armour;
    UT_ASSERT_MSG(numPills > 0 && numBases > 0,
                  "setup: the map carries %u pillbox(es) and %u base(s)",
                  (unsigned)numPills, (unsigned)numBases);

    for (i = 0; i < numPills; i++) {
        pillWas[i] = (*gs->pb).item[i].armour;
        if (pillWas[i] > RA_PILL_ARMOUR_CAP) pillsAbove++;
    }
    for (i = 0; i < numBases; i++) {
        baseWas[i] = (*gs->bs).item[i].shells;
        if (baseWas[i] > RA_BASE_SHELLS_CAP) basesAbove++;
    }
    UT_ASSERT_MSG(pillsAbove > 0,
                  "setup: no pillbox on the map holds more than %d armour, so "
                  "the cap below would strand nothing", RA_PILL_ARMOUR_CAP);
    UT_ASSERT_MSG(basesAbove > 0,
                  "setup: no base on the map holds more than %d shells, so the "
                  "cap below would strand nothing", RA_BASE_SHELLS_CAP);

    UT_ASSERT_MSG(raSetRule(sim, SCN_RULE_pill_max_armour,
                            (double)RA_PILL_ARMOUR_CAP) == SCN_OP_OK,
                  "a pill armour cap of %d is inside the row and above "
                  "pill_repair_amount, so it must be accepted",
                  RA_PILL_ARMOUR_CAP);
    for (i = 0; i < numPills; i++) {
        BYTE want = (pillWas[i] > RA_PILL_ARMOUR_CAP)
                        ? (BYTE)RA_PILL_ARMOUR_CAP : pillWas[i];
        BYTE got  = (*gs->pb).item[i].armour;
        UT_ASSERT_MSG(got == want,
                      "pillbox %u was at %u armour and is at %u under a cap of "
                      "%d, expected %u — the table was committed without "
                      "clamping the records it caps", (unsigned)i,
                      (unsigned)pillWas[i], (unsigned)got, RA_PILL_ARMOUR_CAP,
                      (unsigned)want);
    }

    UT_ASSERT_MSG(raSetRule(sim, SCN_RULE_base_full_shells,
                            (double)RA_BASE_SHELLS_CAP) == SCN_OP_OK,
                  "a base shells cap of %d is inside the row and above "
                  "base_min_shells, so it must be accepted",
                  RA_BASE_SHELLS_CAP);
    for (i = 0; i < numBases; i++) {
        BYTE want = (baseWas[i] > RA_BASE_SHELLS_CAP)
                        ? (BYTE)RA_BASE_SHELLS_CAP : baseWas[i];
        BYTE got  = (*gs->bs).item[i].shells;
        UT_ASSERT_MSG(got == want,
                      "base %u was holding %u shells and is holding %u under a "
                      "cap of %d, expected %u", (unsigned)i,
                      (unsigned)baseWas[i], (unsigned)got, RA_BASE_SHELLS_CAP,
                      (unsigned)want);
    }

    /* Everything is now inside the caps, so from here nothing the set-rule op
       does should reach a record. */
    memcpy(&beforePills, gs->pb, sizeof(beforePills));
    memcpy(&beforeBases, gs->bs, sizeof(beforeBases));

    UT_ASSERT(raSetRule(sim, SCN_RULE_pill_max_armour,
                        (double)RA_PILL_ARMOUR_CAP) == SCN_OP_OK);
    rc = raListsUnmoved(gs, &beforePills, &beforeBases, "the same cap again");
    if (rc != 0) return rc;

    UT_ASSERT(raSetRule(sim, SCN_RULE_tank_reload_ticks,
                        (double)RA_RELOAD_SET) == SCN_OP_OK);
    rc = raListsUnmoved(gs, &beforePills, &beforeBases,
                        "a rule no record answers to");
    if (rc != 0) return rc;

    UT_ASSERT(raSetRule(sim, SCN_RULE_pill_max_armour, (double)pillCapWas) ==
              SCN_OP_OK);
    rc = raListsUnmoved(gs, &beforePills, &beforeBases, "the cap put back up");
    if (rc != 0) return rc;

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 7. What the clamp moved, in the recording.
 *
 * The pillbox is the half of this that only the clamp's own record can
 * settle. Nothing else in a round of idle ticks writes a pill's armour — the
 * paths that do are a shell landing on one and the builder patching one up —
 * and a .wbv carries one snapshot, at its head, which states the armour the
 * map gave the pill. So a viewer that ends up holding the new armour was told
 * about the clamp. A base is weaker evidence and is here for the round trip
 * rather than for the record: the periodic stock update restates every base
 * within a regen period whatever the clamp writes.
 * ================================================================ */

#define RA_CLAMP_TAG "scnRuleClamp"

int run_scenario_rule_clamp_records(void) {
    ReplayHarness h;
    GameSim      *gs;
    BYTE          numPills, i;
    int           stranded = -1;
    BYTE          strandedWas = 0;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, RA_CLAMP_TAG, RA_PLAYER),
                  "could not start recording");

    /* Let the round settle after the opening snapshot, which is what states
       the armour the clamp is about to take off the pill. */
    replayHarnessTick(&h, 4);

    gs = serverSimGetGameSim(h.sim);
    UT_ASSERT_MSG(gs != NULL, "the round has no GameSim");
    numPills = pillsGetNumPills(&gs->pb);
    for (i = 0; i < numPills; i++) {
        if ((*gs->pb).item[i].armour > RA_PILL_ARMOUR_CAP) {
            stranded    = (int)i;
            strandedWas = (*gs->pb).item[i].armour;
            break;
        }
    }
    UT_ASSERT_MSG(stranded >= 0,
                  "setup: no pillbox on the map holds more than %d armour, so "
                  "the cap below would strand nothing", RA_PILL_ARMOUR_CAP);

    UT_ASSERT(raSetRule(h.sim, SCN_RULE_pill_max_armour,
                        (double)RA_PILL_ARMOUR_CAP) == SCN_OP_OK);
    UT_ASSERT(raSetRule(h.sim, SCN_RULE_base_full_shells,
                        (double)RA_BASE_SHELLS_CAP) == SCN_OP_OK);
    UT_ASSERT_MSG((*gs->pb).item[stranded].armour == RA_PILL_ARMOUR_CAP,
                  "setup: pillbox %d was at %u armour and the sim left it at "
                  "%u under a cap of %d, so there is nothing for the record to "
                  "carry", stranded, (unsigned)strandedWas,
                  (unsigned)(*gs->pb).item[stranded].armour,
                  RA_PILL_ARMOUR_CAP);

    /* Two recorded ticks, so the records reach the file. */
    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");
    UT_ASSERT_MSG(replayHarnessDecode(&h), "could not decode the recording");
    UT_ASSERT_MSG(h.replayed != NULL, "the decode captured no world");

    UT_ASSERT_MSG(h.replayed->pills[stranded].armour == RA_PILL_ARMOUR_CAP,
                  "the viewer holds %u armour for pillbox %d and the sim holds "
                  "%u — the clamp moved the record and wrote nothing to say so",
                  (unsigned)h.replayed->pills[stranded].armour, stranded,
                  (unsigned)(*gs->pb).item[stranded].armour);

    UT_ASSERT_MSG(replayHarnessCompare(&h),
                  "the replay disagrees with the sim: %s",
                  replayHarnessDiff(&h));

    replayHarnessStop(&h);
    return 0;
}
