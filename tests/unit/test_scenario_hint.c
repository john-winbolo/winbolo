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
 */

/*
 * The hint op: one flat table, delivered into one bot's brain VM.
 *
 *   run_scenario_hint_reaches_brain
 *       a fixture brain echoes the whole table back — the verb, the keys a
 *       standard verb names, and a key no standard verb mentions, which is
 *       the property the pass-through rests on. Also: every value arrives as
 *       text, a brain with no handler is not a failure, and a handler that
 *       raises is reported without unbalancing the stack.
 *   run_scenario_hint_refusals
 *       every code the arm can answer with: an empty seat, a seat off the
 *       end, a human, a count past the table's, and a key and a value that
 *       do not end inside their own field.
 *   run_scenario_hint_records
 *       the record the arm writes, read back off a real .wbv: the slot it
 *       was for and the verb it led with.
 *
 * The unit binary has no Lua brain — luabrainshandler.c stays out of it for
 * its dependency closure, and test_stubs.c's fixture brain leaves the
 * instance zeroed, so a bot in this binary has no VM at all. So the delivery
 * is driven at brainCoreCallScenarioHint, on a Lua state of the test's own,
 * the way test_bot_init_table.c drives the init table; the arm's own cases
 * drive serverSimApplyScenarioOp and read what came out of the recording.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType / BrainPath */
#include "server_sim_scenario.h"
#include "braincore.h"             /* brainCoreCallScenarioHint */
#include "scenario_table.h"
#include "log.h"                   /* log_ScnHint and the stream opcodes */
#include "replay_harness.h"
#include "test_harness.h"

/* ── The fixture brain ─────────────────────────────────────────────── */

/* Writes every pair it was handed into HINT_SEEN, sorted so the string is
   stable, and the Lua type of one value into HINT_TYPE. A brain reads a
   hint's numbers with tonumber, which is only true if they arrive as text —
   HINT_TYPE is what says they do. */
static const char *const HINT_BRAIN =
    "function on_scenario_hint(t)\n"
    "  if type(t) ~= 'table' then HINT_SEEN = 'not-a-table' return end\n"
    "  local keys = {}\n"
    "  for k in pairs(t) do keys[#keys + 1] = k end\n"
    "  table.sort(keys)\n"
    "  local out = {}\n"
    "  for _, k in ipairs(keys) do out[#out + 1] = k .. '=' .. tostring(t[k]) end\n"
    "  HINT_SEEN = table.concat(out, ';')\n"
    "  HINT_TYPE = type(t.x)\n"
    "end\n";

/* A brain that takes hints and is broken. */
static const char *const HINT_BRAIN_RAISES =
    "function on_scenario_hint(t)\n"
    "  error('the brain author wrote this')\n"
    "end\n";

static bool hintGlobalText(lua_State *L, const char *name, char *out,
                           size_t outCap) {
    const char *s;

    out[0] = '\0';
    lua_getglobal(L, name);
    s = lua_tostring(L, -1);
    if (s == NULL) { lua_pop(L, 1); return false; }
    snprintf(out, outCap, "%s", s);
    lua_pop(L, 1);
    return true;
}

/* ── Tables ───────────────────────────────────────────────────────── */

static void hintSet(ScnTable *t, const char *key, const char *value) {
    (void)scnTableSet(t, key, value);
}

/* ================================================================
 * 1. The table reaches the brain, whole.
 * ================================================================ */
int run_scenario_hint_reaches_brain(void) {
    lua_State *L;
    ScnTable   hint;
    char       err[256];
    char       seen[512];
    int        top;

    L = luaL_newstate();
    UT_ASSERT(L != NULL);
    luaL_openlibs(L);
    UT_ASSERT_MSG(luaL_dostring(L, HINT_BRAIN) == 0,
                  "the fixture brain would not load");

    /* A go-to-a-region hint: the verb, the four numbers a region expands
       into, and two keys no standard verb mentions. The brain is the only
       thing that knows what `squad` and `mood` mean, so the engine has to
       carry them through without reading them. */
    scnTableClear(&hint);
    hintSet(&hint, "verb", "goto");
    hintSet(&hint, "x", "120");
    hintSet(&hint, "y", "64");
    hintSet(&hint, "w", "10");
    hintSet(&hint, "h", "10");
    hintSet(&hint, "squad", "red");
    hintSet(&hint, "mood", "patient");

    top = lua_gettop(L);
    UT_ASSERT_MSG(brainCoreCallScenarioHint(L, &hint, err, sizeof(err)) ==
                      BRAIN_HINT_DELIVERED,
                  "the handler was not reached: %s", err);
    UT_ASSERT_MSG(lua_gettop(L) == top,
                  "the call left %d values on the stack",
                  lua_gettop(L) - top);

    UT_ASSERT(hintGlobalText(L, "HINT_SEEN", seen, sizeof(seen)));
    UT_ASSERT_MSG(strcmp(seen,
                         "h=10;mood=patient;squad=red;verb=goto;w=10;x=120;"
                         "y=64") == 0,
                  "the brain saw '%s'", seen);

    /* Every value is text, which is the contract a brain's tonumber rests
       on. */
    UT_ASSERT(hintGlobalText(L, "HINT_TYPE", seen, sizeof(seen)));
    UT_ASSERT_MSG(strcmp(seen, "string") == 0,
                  "x reached the brain as a %s", seen);

    /* A second hint on the same VM is the second hint, not the first with
       something added to it. */
    scnTableClear(&hint);
    hintSet(&hint, "verb", "hold");
    UT_ASSERT(brainCoreCallScenarioHint(L, &hint, err, sizeof(err)) ==
                  BRAIN_HINT_DELIVERED);
    UT_ASSERT(hintGlobalText(L, "HINT_SEEN", seen, sizeof(seen)));
    UT_ASSERT_MSG(strcmp(seen, "verb=hold") == 0,
                  "the second hint read '%s'", seen);

    /* An empty table is a table: the handler runs and sees nothing in it. */
    scnTableClear(&hint);
    UT_ASSERT(brainCoreCallScenarioHint(L, &hint, err, sizeof(err)) ==
                  BRAIN_HINT_DELIVERED);
    UT_ASSERT(hintGlobalText(L, "HINT_SEEN", seen, sizeof(seen)));
    UT_ASSERT_MSG(strcmp(seen, "") == 0, "an empty hint read '%s'", seen);
    lua_close(L);

    /* A brain that defines no handler. Not a failure — a scenario names a
       seat and cannot know which brain a server runs it with — and nothing
       is left on the stack. */
    L = luaL_newstate();
    UT_ASSERT(L != NULL);
    luaL_openlibs(L);
    scnTableClear(&hint);
    hintSet(&hint, "verb", "goto");
    top = lua_gettop(L);
    UT_ASSERT_MSG(brainCoreCallScenarioHint(L, &hint, err, sizeof(err)) ==
                      BRAIN_HINT_NO_HANDLER,
                  "a brain with no handler must answer NO_HANDLER");
    UT_ASSERT_MSG(err[0] == '\0', "NO_HANDLER wrote the message '%s'", err);
    UT_ASSERT(lua_gettop(L) == top);
    lua_close(L);

    /* A handler that raises. Reported with its message, and the stack is
       balanced so the next tick's brain call starts where it should. */
    L = luaL_newstate();
    UT_ASSERT(L != NULL);
    luaL_openlibs(L);
    UT_ASSERT(luaL_dostring(L, HINT_BRAIN_RAISES) == 0);
    top = lua_gettop(L);
    UT_ASSERT_MSG(brainCoreCallScenarioHint(L, &hint, err, sizeof(err)) ==
                      BRAIN_HINT_ERROR,
                  "a handler that raises must answer ERROR");
    UT_ASSERT_MSG(strstr(err, "the brain author wrote this") != NULL,
                  "the error read '%s'", err);
    UT_ASSERT_MSG(lua_gettop(L) == top,
                  "a raised handler left %d values on the stack",
                  lua_gettop(L) - top);
    lua_close(L);

    return 0;
}

/* ── A round with a bot in it ─────────────────────────────────────── */

/* A file for the brain path to name. The fixture brain never opens it; the
   spawn arm reads the path to refuse one that names nothing. The case's own
   name is in it because ctest runs the cases as concurrent processes in one
   working directory. */
static char shBrainPath[128];

static bool shMakeBrainFile(const char *tag) {
    FILE *f;

    SDL_snprintf(shBrainPath, sizeof(shBrainPath),
                 "test_scenario_hint_brain_%s.lua", tag);
    f = fopen(shBrainPath, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
}

static void shDropBrainFile(void) {
    remove(shBrainPath);
}

/* Queue a bot into a running round and tick it in. Answers the seat it took,
   or SCN_NONE. */
static BYTE shSeatABot(ServerSim *sim) {
    ScenarioOp op;
    int        i;

    memset(&op, 0, sizeof(op));
    op.type                    = SCN_OP_ROSTER_SPAWN_BOT;
    op.u.rosterSpawnBot.slot   = SCN_NONE;
    op.u.rosterSpawnBot.start  = SCN_NONE;
    SDL_strlcpy(op.u.rosterSpawnBot.brain, shBrainPath, SCN_PATH_MAX);
    if (serverSimApplyScenarioOp(sim, &op, NULL) != SCN_OP_QUEUED) {
        return SCN_NONE;
    }
    serverSimTick(sim);
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) return (BYTE)i;
    }
    return SCN_NONE;
}

static ScnOpResult shHint(ServerSim *sim, BYTE slot, const ScnTable *hint) {
    ScenarioOp op;

    memset(&op, 0, sizeof(op));
    op.type           = SCN_OP_BOT_HINT;
    op.u.botHint.slot = slot;
    if (hint != NULL) {
        op.u.botHint.hint = *hint;
    }
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* ================================================================
 * 2. Every refusal, under its own code.
 * ================================================================ */
int run_scenario_hint_refusals(void) {
    ServerSim *sim;
    ScnTable   hint;
    ScnTable   bad;
    BYTE       bot;

    UT_ASSERT(shMakeBrainFile("refusals"));
    ut_brain_stub_arm(true);

    sim = ut_make_running_sim("Human");
    UT_ASSERT(sim != NULL);
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, shBrainPath);

    scnTableClear(&hint);
    hintSet(&hint, "verb", "hold");

    /* A seat with nobody in it, and a seat off the end of the roster. */
    UT_ASSERT_MSG(shHint(sim, 5, &hint) == SCN_OP_NO_SUCH_PLAYER,
                  "a hint to an empty seat must be SCN_OP_NO_SUCH_PLAYER");
    UT_ASSERT_MSG(shHint(sim, MAX_TANKS, &hint) == SCN_OP_NO_SUCH_PLAYER,
                  "a hint to a seat off the end must be "
                  "SCN_OP_NO_SUCH_PLAYER");

    /* The human the sim was built with. A person is not ordered about by a
       script. */
    UT_ASSERT_MSG(shHint(sim, 0, &hint) == SCN_OP_IS_HUMAN,
                  "a hint aimed at a human must be SCN_OP_IS_HUMAN");

    bot = shSeatABot(sim);
    UT_ASSERT_MSG(bot != SCN_NONE, "the case could not seat a bot");

    /* The same hint, at the bot: everything past here is about the table
       rather than the seat. */
    UT_ASSERT_MSG(shHint(sim, bot, &hint) == SCN_OP_OK,
                  "a hint to a bot must be taken");

    /* A count past the pairs the table holds. Nothing the funnel's own rows
       can build, which is why the arm checks it: the payload is a struct a
       caller fills. */
    bad = hint;
    bad.count = SCN_TABLE_MAX + 1;
    UT_ASSERT_MSG(shHint(sim, bot, &bad) == SCN_OP_TOO_BIG,
                  "a count past the table must be SCN_OP_TOO_BIG");

    /* A key that does not end inside its own field. The table reaches a Lua
       VM, so it is refused rather than read past the end of it. */
    bad = hint;
    memset(bad.kv[0].key, 'k', SCN_TABLE_KEY_LEN);
    UT_ASSERT_MSG(shHint(sim, bot, &bad) == SCN_OP_TOO_BIG,
                  "an unterminated key must be SCN_OP_TOO_BIG");

    /* And a value that does not either. */
    bad = hint;
    memset(bad.kv[0].value, 'v', SCN_TABLE_VALUE_LEN);
    UT_ASSERT_MSG(shHint(sim, bot, &bad) == SCN_OP_TOO_BIG,
                  "an unterminated value must be SCN_OP_TOO_BIG");

    /* A hint carrying no verb at all is not the arm's business: the row
       raises on it before an op is ever built, and a table the arm can read
       is a table it takes. */
    scnTableClear(&bad);
    hintSet(&bad, "squad", "red");
    UT_ASSERT_MSG(shHint(sim, bot, &bad) == SCN_OP_OK,
                  "the arm judges the seat and the buffers, not the verb");

    serverSimDestroy(sim);
    shDropBrainFile();
    return 0;
}

/* ── What the recording holds ─────────────────────────────────────── */

#define SH_MAX_HITS 4
#define SH_MAX_PAYLOAD (1 + 1 + SCN_TABLE_VALUE_LEN)

typedef struct {
    int     count;
    uint8_t payload[SH_MAX_HITS][SH_MAX_PAYLOAD];
    int     payloadLen[SH_MAX_HITS];
} ShLogHits;

static int shReadByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Skip a snapshot body: startDelay+timeLimit, the count-prefixed pills,
   bases and starts, the map runs up to the deep-sea terminator, then
   MAX_TANKS player blocks. Plaintext, not length-framed. */
static bool shSkipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    int n, i;
    if (p + 8 > len) return false;
    p += 8;
    if ((n = shReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = shReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = shReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    while (1) {
        int dlen, y, sx, ex;
        if (p + 4 > len) return false;
        dlen = shReadByte(buf, len, p);
        y    = shReadByte(buf, len, p + 1);
        sx   = shReadByte(buf, len, p + 2);
        ex   = shReadByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if ((n = shReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Walk the .wbv's event stream and collect every record of type `want`, in
   the order they were written. False if the stream did not end on a clean
   LOG_QUIT. */
static bool shFindLogged(const char *path, uint8_t want, ShLogHits *hits) {
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
        int code = shReadByte(buf, len, pos);
        pos++;
        if (code < 0) break;
        if (code == LOG_QUIT) {
            ok = true;
            break;
        } else if (code == LOG_NOEVENTS) {
            if (shReadByte(buf, len, pos) < 0) break;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) break;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!shSkipSnapshot(buf, len, &pos)) break;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n, i;
            if (code == LOG_EVENT) {
                n = shReadByte(buf, len, pos);
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
                if (ev == want && hits->count < SH_MAX_HITS) {
                    int slot = hits->count;
                    int copy = plen;
                    if (copy > SH_MAX_PAYLOAD) copy = SH_MAX_PAYLOAD;
                    hits->payloadLen[slot] = plen;
                    memcpy(hits->payload[slot], buf + payloadStart,
                           (size_t)copy);
                    hits->count++;
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

/* ================================================================
 * 3. The record: the slot it was for and the verb it led with.
 * ================================================================ */
int run_scenario_hint_records(void) {
    ReplayHarness h;
    ServerSim    *sim;
    ScnTable      hint;
    ShLogHits     hits;
    BYTE          bot;

    UT_ASSERT(shMakeBrainFile("records"));
    ut_brain_stub_arm(true);

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "scnHintArm", "Hinter"),
                  "could not start recording");
    sim = h.sim;
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, shBrainPath);
    replayHarnessTick(&h, 4);

    bot = shSeatABot(sim);
    UT_ASSERT_MSG(bot != SCN_NONE, "the case could not seat a bot");

    /* One order, with keys past the verb. Only the verb goes down. */
    scnTableClear(&hint);
    hintSet(&hint, "verb", "defend");
    hintSet(&hint, "base", "3");
    hintSet(&hint, "squad", "red");
    UT_ASSERT(shHint(sim, bot, &hint) == SCN_OP_OK);

    /* A refused hint records nothing: the record says an order was given,
       and this one was not. */
    UT_ASSERT(shHint(sim, 0, &hint) == SCN_OP_IS_HUMAN);

    replayHarnessTick(&h, 4);
    UT_ASSERT(replayHarnessStopRecording(&h));

    UT_ASSERT_MSG(shFindLogged(h.path, (uint8_t)log_ScnHint, &hits),
                  "the recording did not end on a clean quit: %s", h.path);
    UT_ASSERT_MSG(hits.count == 1,
                  "the recording holds %d log_ScnHint record(s), expected 1",
                  hits.count);
    UT_ASSERT_MSG(hits.payloadLen[0] == 1 + 1 + 6,
                  "the record is %d bytes, expected %d",
                  hits.payloadLen[0], 1 + 1 + 6);
    UT_ASSERT_MSG(hits.payload[0][0] == bot,
                  "the record names slot %u, expected %u",
                  (unsigned)hits.payload[0][0], (unsigned)bot);
    UT_ASSERT_MSG(hits.payload[0][1] == 6,
                  "the verb's length byte reads %u, expected 6",
                  (unsigned)hits.payload[0][1]);
    UT_ASSERT_MSG(memcmp(hits.payload[0] + 2, "defend", 6) == 0,
                  "the record does not carry the verb");

    replayHarnessStop(&h);
    shDropBrainFile();
    return 0;
}
