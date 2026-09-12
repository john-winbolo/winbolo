/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Host
 *Filename:      scenario_host.c
 *Author:        John Morrison
 *Purpose:
 *  Finds the Lua sidecar beside a map, boots a VM, runs its
 *  chunk, reads the scenario table it declares into one
 *  struct, and applies the rules that table sets through the
 *  op funnel from inside the sim's round-start callback.
 *
 *  The VM is a codec and nothing more here: the chunk runs,
 *  a table is read, and the host calls nothing back into the
 *  engine on the script's behalf. The only thing that
 *  reaches the sim is one SCN_OP_SET_RULE per rule.
 *
 *  The sidecar is read from disk once, at attach. Every round
 *  after that runs the bytes the host is holding, so a round
 *  start touches no file: it runs inside the start with the
 *  sim mutex held, which is no place for disk work, and an
 *  edit made to the file while the server is up does not
 *  reach a round on its own.
 *
 *  Nothing here publishes. The round start opens its setup
 *  window across the callback, which holds the set-rule
 *  handler's own CTRL_SIM_RULES publish, and the publish the
 *  start makes immediately afterwards carries the whole
 *  table. A publish from here would be a second one saying
 *  the same thing.
 *
 *  This file compiles under the scenario_host profile: it
 *  sees src/bolo/public/ and src/bolo/scenario_api/, and
 *  nothing under src/bolo/internal/.
 *********************************************************/

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "platform_types.h"        /* BOLO_STATIC_ASSERT */
#include "bolo_rand.h"             /* BoloRandState, bolo_rand_save */
#include "server_sim.h"            /* ServerSim, serverSimConsoleMessage */
#include "scenario_defs.h"         /* SCN_RULE_LIST, ScenarioOp, ScnOpResult */
#include "server_sim_scenario.h"   /* the funnel and the round-start hook-up */

#include "scenario_host.h"
#include "scenario_manifest.h"

/* One operator line. Long enough for a Lua error, which carries the file
 * path and the line number ahead of its message. */
#define SCN_ERR_LEN 512

/* ── The rule names ───────────────────────────────────────────────── */

/* A rule's name in a sidecar is its name in the rule list, so the list is
 * the only place the spelling exists. A rule added there is resolvable
 * here with nothing to update. */
static const char *const kScnRuleNames[] = {
#define SCN_RULE_NAME_ROW(name) #name,
    SCN_RULE_LIST(SCN_RULE_NAME_ROW)
#undef SCN_RULE_NAME_ROW
};

BOLO_STATIC_ASSERT(
    (int)(sizeof(kScnRuleNames) / sizeof(kScnRuleNames[0])) == (int)SCN_RULE_COUNT,
    rule_name_table_is_the_whole_rule_list);

/* The manifest is sized in scenario_host.h, which cannot see the rule
 * count; this is where the two are visible together. */
BOLO_STATIC_ASSERT(SCN_MANIFEST_RULES_MAX >= (int)SCN_RULE_COUNT,
                   manifest_holds_every_rule_at_once);

/* A team's brain is the same text a spawn carries, and the manifest sizes
 * it without seeing the op. Held against the op's own length here. */
BOLO_STATIC_ASSERT(SCN_BRAIN_LEN == SCN_PATH_MAX,
                   manifest_brain_matches_the_op_path_length);

static int scnRuleIndexOf(const char *key) {
    int i;
    for (i = 0; i < (int)SCN_RULE_COUNT; i++) {
        if (strcmp(kScnRuleNames[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

/* What a refusal was, in words. A script author reads these, so each is
 * the reason rather than the enumerator's spelling. */
static const char *scnResultText(ScnOpResult r) {
    switch (r) {
        case SCN_OP_OK:             return "accepted";
        case SCN_OP_QUEUED:         return "queued";
        case SCN_OP_UNSUPPORTED:    return "not supported";
        case SCN_OP_IN_POLICY:      return "issued from inside a policy call";
        case SCN_OP_WRONG_STATE:    return "wrong state";
        case SCN_OP_NO_SUCH_PLAYER: return "no such player";
        case SCN_OP_TANK_DEAD:      return "the tank is dead";
        case SCN_OP_IS_HUMAN:       return "that seat is a human";
        case SCN_OP_NO_SUCH_ITEM:   return "no such item";
        case SCN_OP_BAD_SQUARE:     return "square off the map";
        case SCN_OP_BAD_TERRAIN:    return "the square will not take it";
        case SCN_OP_RANGE:          return "outside the value's range";
        case SCN_OP_PAIR:           return "breaks the pair the rule belongs to";
        case SCN_OP_CARRIED:        return "the pill is carried";
        case SCN_OP_FULL:           return "full";
        case SCN_OP_ALREADY:        return "already so";
        case SCN_OP_TOO_BIG:        return "too big";
        case SCN_OP_RATE:           return "over the tick's budget";
        case SCN_OP_NOT_FOUND:      return "not found";
        case SCN_OP_NO_STOCK:       return "not enough stock";
        case SCN_OP_BAD_CALL:       return "malformed call";
    }
    return "refused";
}

/* ── Operator lines ───────────────────────────────────────────────── */

/* Write one line into a caller's buffer and nowhere else. Used for the
 * refusals attach reports, which the caller prints in its own words. */
static void scnFmt(char *buf, size_t len, const char *fmt, ...) {
    va_list ap;
    if (buf == NULL || len == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

/* Say one line to the operator and keep a copy in buf, which may be NULL.
 * Used for everything a live scenario complains about, where there is no
 * caller waiting on a return value to print it. */
static void scnSay(char *buf, size_t len, const char *fmt, ...) {
    char    line[SCN_ERR_LEN];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    serverSimConsoleMessage(line);
    if (buf != NULL && len > 0) {
        snprintf(buf, len, "%s", line);
    }
}

/* ── The host ─────────────────────────────────────────────────────── */

struct ScenarioHost {
    ServerSim       *sim;
    lua_State       *L;       /* the live VM: the metadata one until a round
                               * starts, then that round's own */
    ScenarioManifest manifest;
    char             sidecar[SCN_SIDECAR_PATH_MAX];
    char            *src;     /* the sidecar's bytes, read once at attach */
    size_t           srcLen;
    char             chunkName[SCN_SIDECAR_PATH_MAX + 2];
    char             lastError[SCN_ERR_LEN];
    bool             active;
};

/* ── Finding the sidecar ──────────────────────────────────────────── */

/* .../X.map is accompanied by .../X.scenario.lua. A path that does not end
 * in .map keeps its whole name and takes the suffix as it is, so a caller
 * that hands over a name without an extension still resolves. */
static bool scnSidecarPath(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    size_t base = n;
    size_t suffix = strlen(SCN_SIDECAR_SUFFIX);

    if (n >= 4) {
        const char *ext = mapPath + n - 4;
        if (ext[0] == '.' &&
            (ext[1] == 'm' || ext[1] == 'M') &&
            (ext[2] == 'a' || ext[2] == 'A') &&
            (ext[3] == 'p' || ext[3] == 'P')) {
            base = n - 4;
        }
    }
    if (base + suffix + 1 > outLen) {
        return false;
    }
    memcpy(out, mapPath, base);
    memcpy(out + base, SCN_SIDECAR_SUFFIX, suffix + 1);
    return true;
}

/* The whole sidecar, into a buffer the caller owns. Returns false with err
 * set when the file is there but cannot be used; returns false with err
 * left empty when there is no file at all, which is not a fault.
 *
 * Read in one go rather than streamed: a sidecar is a script, the cap below
 * is what says so, and a file above it is refused before a byte of it is
 * kept. */
static bool scnReadFile(const char *path, char **out, size_t *outLen,
                        char *err, size_t errLen) {
    FILE  *f;
    long   size;
    char  *buf;
    size_t got;

    *out    = NULL;
    *outLen = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        scnFmt(err, errLen, "scenario: %s could not be read", path);
        return false;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        scnFmt(err, errLen, "scenario: %s could not be measured", path);
        return false;
    }
    if (size > (long)SCN_SIDECAR_MAX_BYTES) {
        fclose(f);
        scnFmt(err, errLen,
               "scenario: %s is %ld bytes and the limit is %ld — too large to "
               "be a script", path, size, (long)SCN_SIDECAR_MAX_BYTES);
        return false;
    }
    rewind(f);

    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        scnFmt(err, errLen, "scenario: no memory to read %s", path);
        return false;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    /* Short of what the size said is a text-mode translation or a file being
       written underneath us; either way what was read is what there is. */
    buf[got] = '\0';

    *out    = buf;
    *outLen = got;
    return true;
}

/* ── The VM ───────────────────────────────────────────────────────── */

/* Hand math.random a seed derived from the process PRNG's state as it
 * stands at this boot, read with bolo_rand_save rather than drawn.
 *
 * Reading rather than drawing leaves every other consumer's sequence
 * exactly where it was, so no fixed-seed baseline moves. Under a fixed
 * -seed the whole process is deterministic, so the state at a round start
 * is deterministic and this value is too: that is what lets a round replay
 * the same under the headless tests. The state always advances between
 * rounds, so two rounds in one process do not get the same seed.
 *
 * The value is not a stable identifier and nothing should read it as one —
 * it depends on how many draws the rest of the process has made by the time
 * the VM boots. */
static void scnSeedRandom(lua_State *L) {
    BoloRandState st;
    uint64_t      mixed;
    int           i;

    bolo_rand_save(&st);
    mixed = 0xcbf29ce484222325ULL;
    for (i = 0; i < 4; i++) {
        mixed ^= (uint64_t)st.s[i];
        mixed *= 0x100000001b3ULL;
    }
    /* Trimmed to what a double holds exactly, so LuaJIT's number model and
       PUC's hand math.randomseed the same value. */
    mixed &= 0x1FFFFFFFFFFFFFULL;

    lua_getglobal(L, "math");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "randomseed");
        if (lua_isfunction(L, -1)) {
            lua_pushnumber(L, (lua_Number)mixed);
            if (lua_pcall(L, 1, 0, 0) != 0) {
                lua_pop(L, 1);   /* the error it raised */
            }
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);               /* math, or whatever the global held */
}

static lua_State *scnNewVm(void) {
    lua_State *L = luaL_newstate();
    if (L == NULL) {
        return NULL;
    }
    luaL_openlibs(L);
    scnSeedRandom(L);
    return L;
}

static const char *scnLuaError(lua_State *L) {
    const char *s = lua_tostring(L, -1);
    return (s != NULL) ? s : "unknown error";
}

/* Load and run the bytes the host is holding. Lua's own message already
 * names the source and the line for both a syntax error and one the chunk
 * raises, so it is passed through rather than summarised.
 *
 * chunkName carries the leading '@' that tells Lua the name is a file, so
 * an error reads as path:line: message — the same text a chunk loaded
 * straight from the file produces. */
static bool scnRunChunk(lua_State *L, const char *src, size_t srcLen,
                        const char *chunkName, char *err, size_t errLen) {
    if (luaL_loadbuffer(L, src, srcLen, chunkName) != 0) {
        scnFmt(err, errLen, "scenario: %s", scnLuaError(L));
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        scnFmt(err, errLen, "scenario: %s", scnLuaError(L));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

/* ── Reading the table ────────────────────────────────────────────── */

static void scnReadStr(lua_State *L, int tbl, const char *key,
                       char *out, size_t outLen) {
    lua_getfield(L, tbl, key);
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *s = lua_tostring(L, -1);
        size_t      n = strlen(s);
        if (n >= outLen) {
            n = outLen - 1;
        }
        memcpy(out, s, n);
        out[n] = '\0';
    }
    lua_pop(L, 1);
}

static int scnReadInt(lua_State *L, int tbl, const char *key, int def) {
    int v = def;
    lua_getfield(L, tbl, key);
    if (lua_type(L, -1) == LUA_TNUMBER) {
        v = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    return v;
}

static bool scnReadBool(lua_State *L, int tbl, const char *key, bool def) {
    bool v = def;
    lua_getfield(L, tbl, key);
    if (lua_isboolean(L, -1)) {
        v = lua_toboolean(L, -1) ? true : false;
    }
    lua_pop(L, 1);
    return v;
}

static void scnReadLobby(lua_State *L, int tbl, ScnManifestLobby *lob) {
    int lt;

    lua_getfield(L, tbl, "lobby");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lt = lua_gettop(L);
    lob->maxPlayers = (uint8_t)scnReadInt(L, lt, "max_players", 0);
    lob->extraTeams = scnReadBool(L, lt, "extra_teams", false);

    lua_getfield(L, lt, "teams");
    if (lua_istable(L, -1)) {
        int teams = lua_gettop(L);
        int n     = (int)lua_rawlen(L, teams);
        int i;
        for (i = 1; i <= n && lob->numTeams < MAX_TANKS; i++) {
            lua_rawgeti(L, teams, i);
            if (lua_istable(L, -1)) {
                int              t    = lua_gettop(L);
                ScnManifestTeam *team = &lob->teams[lob->numTeams];
                lob->numTeams++;
                team->id      = (uint8_t)scnReadInt(L, t, "id", 0);
                team->bots    = (uint8_t)scnReadInt(L, t, "bots", 0);
                team->maxBots = (uint8_t)scnReadInt(L, t, "max_bots", 0);
                team->fielded = scnReadBool(L, t, "fielded", true);
                scnReadStr(L, t, "brain", team->brain, sizeof(team->brain));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);   /* teams */
    lua_pop(L, 1);   /* lobby */
}

/* A key that names no rule is refused by name and the rest of the table
 * still applies: one bad line in a sidecar should not cost an author every
 * other line in it. */
static void scnReadRules(lua_State *L, int tbl, ScenarioManifest *m,
                         char *soft, size_t softLen) {
    int rules;

    lua_getfield(L, tbl, "rules");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    rules = lua_gettop(L);

    lua_pushnil(L);
    while (lua_next(L, rules) != 0) {
        /* key at -2, value at -1. The key is tested before it is read as a
           string: lua_tostring on a number key would rewrite it in place
           and break the traversal. */
        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TNUMBER) {
            const char *key = lua_tostring(L, -2);
            int         idx = scnRuleIndexOf(key);
            if (idx < 0) {
                scnSay(soft, softLen, "scenario: no rule is named '%s'", key);
            } else if (m->numRules >= SCN_MANIFEST_RULES_MAX) {
                scnSay(soft, softLen,
                       "scenario: more than %d rules; '%s' dropped",
                       SCN_MANIFEST_RULES_MAX, key);
            } else {
                m->rules[m->numRules].rule  = (uint16_t)idx;
                m->rules[m->numRules].value = (double)lua_tonumber(L, -1);
                m->numRules++;
            }
        }
        lua_pop(L, 1);   /* the value; the key stays for the next step */
    }
    lua_pop(L, 1);
}

static void scnAddTag(ScnManifestTags *out, const char *s, const char *kind,
                      int entity, char *soft, size_t softLen) {
    size_t n;

    if (out->count >= SCN_TAGS_PER_ENTITY) {
        scnSay(soft, softLen,
               "scenario: %s %d already carries %d tags; '%s' dropped",
               kind, entity, SCN_TAGS_PER_ENTITY, s);
        return;
    }
    n = strlen(s);
    if (n >= SCN_TAG_LEN) {
        n = SCN_TAG_LEN - 1;
    }
    memcpy(out->tag[out->count], s, n);
    out->tag[out->count][n] = '\0';
    out->count++;
}

/* One kind's tag table: keyed by the 1-based entity index, each entry a
 * single tag or an array of them. The indices are kept as the file writes
 * them, so entry 3 in the file is entry 3 here. */
static void scnReadTagKind(lua_State *L, int tags, const char *key,
                           ScnManifestTags *arr, int maxEntity,
                           const char *kind, char *soft, size_t softLen) {
    int kt;

    lua_getfield(L, tags, key);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    kt = lua_gettop(L);

    lua_pushnil(L);
    while (lua_next(L, kt) != 0) {
        if (lua_type(L, -2) == LUA_TNUMBER) {
            int e = (int)lua_tointeger(L, -2);
            if (e < 1 || e > maxEntity) {
                scnSay(soft, softLen,
                       "scenario: %s %d is not an index this map can hold",
                       kind, e);
            } else if (lua_type(L, -1) == LUA_TSTRING) {
                scnAddTag(&arr[e], lua_tostring(L, -1), kind, e, soft, softLen);
            } else if (lua_istable(L, -1)) {
                int list = lua_gettop(L);
                int n    = (int)lua_rawlen(L, list);
                int i;
                for (i = 1; i <= n; i++) {
                    lua_rawgeti(L, list, i);
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        scnAddTag(&arr[e], lua_tostring(L, -1), kind, e,
                                  soft, softLen);
                    }
                    lua_pop(L, 1);
                }
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void scnReadTags(lua_State *L, int tbl, ScenarioManifest *m,
                        char *soft, size_t softLen) {
    int tags;

    lua_getfield(L, tbl, "tags");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    tags = lua_gettop(L);
    scnReadTagKind(L, tags, "pills",  m->pillTags,  MAX_PILLS,  "pill",
                   soft, softLen);
    scnReadTagKind(L, tags, "bases",  m->baseTags,  MAX_BASES,  "base",
                   soft, softLen);
    scnReadTagKind(L, tags, "starts", m->startTags, MAX_STARTS, "start",
                   soft, softLen);
    lua_pop(L, 1);
}

/* Regions are keyed by name, so they arrive in whatever order the table
 * iterates in — which differs between the two Lua builds. A reader looks a
 * region up by name rather than by position. */
static void scnReadRegions(lua_State *L, int tbl, ScenarioManifest *m,
                           char *soft, size_t softLen) {
    int rt;

    lua_getfield(L, tbl, "regions");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    rt = lua_gettop(L);

    lua_pushnil(L);
    while (lua_next(L, rt) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) {
            const char *name = lua_tostring(L, -2);
            if (m->numRegions >= SCN_REGIONS_MAX) {
                scnSay(soft, softLen,
                       "scenario: more than %d regions; '%s' dropped",
                       SCN_REGIONS_MAX, name);
            } else {
                int                r   = lua_gettop(L);
                ScnManifestRegion *reg = &m->regions[m->numRegions];
                size_t             n   = strlen(name);
                m->numRegions++;
                if (n >= SCN_REGION_NAME_LEN) {
                    n = SCN_REGION_NAME_LEN - 1;
                }
                memcpy(reg->name, name, n);
                reg->name[n] = '\0';
                reg->x = (uint8_t)scnReadInt(L, r, "x", 0);
                reg->y = (uint8_t)scnReadInt(L, r, "y", 0);
                reg->w = (uint8_t)scnReadInt(L, r, "w", 0);
                reg->h = (uint8_t)scnReadInt(L, r, "h", 0);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/* The whole table into the struct. A file with no scenario table at all is
 * refused: it ran, but it is not a scenario.
 *
 * triggers is not read. Its schema is not settled, so a sidecar that
 * carries one is neither parsed nor refused for it. */
static bool scnReadManifest(lua_State *L, ScenarioManifest *m,
                            const char *path, char *err, size_t errLen,
                            char *soft, size_t softLen) {
    int tbl;

    memset(m, 0, sizeof(*m));

    lua_getglobal(L, "scenario");
    if (!lua_istable(L, -1)) {
        scnFmt(err, errLen, "scenario: %s declares no scenario table", path);
        lua_pop(L, 1);
        return false;
    }
    tbl = lua_gettop(L);

    scnReadStr(L, tbl, "name", m->name, sizeof(m->name));
    scnReadStr(L, tbl, "description", m->description, sizeof(m->description));
    scnReadStr(L, tbl, "game", m->game, sizeof(m->game));
    m->api   = scnReadInt(L, tbl, "api", 1);
    m->bound = scnReadBool(L, tbl, "bound", true);

    scnReadLobby(L, tbl, &m->lobby);
    scnReadRules(L, tbl, m, soft, softLen);
    scnReadTags(L, tbl, m, soft, softLen);
    scnReadRegions(L, tbl, m, soft, softLen);

    lua_pop(L, 1);
    return true;
}

/* ── Applying the rules ───────────────────────────────────────────── */

/* One op per rule. Nothing is published here: the setup window the round
 * start opens across this call holds the set-rule handler's own publish,
 * and the start states the whole table once on the way out. */
static void scnApplyRules(ScenarioHost *h) {
    uint16_t i;

    for (i = 0; i < h->manifest.numRules; i++) {
        ScenarioOp  op;
        ScnOpResult r;

        memset(&op, 0, sizeof(op));
        op.type            = SCN_OP_SET_RULE;
        op.u.setRule.rule  = h->manifest.rules[i].rule;
        op.u.setRule.value = h->manifest.rules[i].value;

        r = serverSimApplyScenarioOp(h->sim, &op, NULL);
        if (r != SCN_OP_OK) {
            scnSay(h->lastError, sizeof(h->lastError),
                   "scenario: rule '%s' refused: %s",
                   kScnRuleNames[h->manifest.rules[i].rule], scnResultText(r));
        }
    }
}

/* The sim's round-start callback. A fresh VM for the round, the bytes read
 * at attach run again in it, the table read again, and then the rules.
 * Nothing here goes near the disk: this runs inside the start with the sim
 * mutex held. A round whose chunk fails says so and plays classic rather
 * than carrying the previous round's table into it. */
static void scnRoundStart(void *ctx) {
    ScenarioHost    *h = (ScenarioHost *)ctx;
    lua_State       *L;
    ScenarioManifest fresh;
    char             err[SCN_ERR_LEN];

    if (h == NULL) {
        return;
    }
    err[0] = '\0';

    L = scnNewVm();
    if (L == NULL) {
        scnSay(h->lastError, sizeof(h->lastError),
               "scenario: no memory for this round's Lua state");
        return;
    }
    if (!scnRunChunk(L, h->src, h->srcLen, h->chunkName, err, sizeof(err)) ||
        !scnReadManifest(L, &fresh, h->sidecar, err, sizeof(err),
                         h->lastError, sizeof(h->lastError))) {
        scnSay(h->lastError, sizeof(h->lastError), "%s", err);
        lua_close(L);
        return;
    }
    if (fresh.api > SCENARIO_API_VERSION) {
        scnSay(h->lastError, sizeof(h->lastError),
               "scenario: %s asks for api %d and this server is api %d",
               h->sidecar, fresh.api, SCENARIO_API_VERSION);
        lua_close(L);
        return;
    }

    if (h->L != NULL) {
        lua_close(h->L);
    }
    h->L        = L;
    h->manifest = fresh;
    scnApplyRules(h);
}

/* ── The frontend surface ─────────────────────────────────────────── */

ScenarioHost *scenarioHostAttach(ServerSim *sim, const char *mapPath,
                                 char *err, size_t errLen) {
    char             sidecar[SCN_SIDECAR_PATH_MAX];
    char             chunkName[SCN_SIDECAR_PATH_MAX + 2];
    char             soft[SCN_ERR_LEN];
    ScenarioManifest m;
    ScenarioHost    *h;
    lua_State       *L;
    char            *src    = NULL;
    size_t           srcLen = 0;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (sim == NULL || mapPath == NULL) {
        return NULL;
    }
    if (!scnSidecarPath(mapPath, sidecar, sizeof(sidecar))) {
        return NULL;
    }
    /* The one read of the file. No sidecar is the ordinary case: the map
       plays as a plain map and the operator is told nothing, because there
       is nothing to tell. A file that is there and cannot be used sets err
       and is reported. */
    if (!scnReadFile(sidecar, &src, &srcLen, err, errLen)) {
        return NULL;
    }
    /* The leading '@' is what makes Lua call this a file in its messages. */
    snprintf(chunkName, sizeof(chunkName), "@%s", sidecar);

    L = scnNewVm();
    if (L == NULL) {
        scnFmt(err, errLen, "scenario: no memory for a Lua state");
        free(src);
        return NULL;
    }

    soft[0] = '\0';
    if (!scnRunChunk(L, src, srcLen, chunkName, err, errLen) ||
        !scnReadManifest(L, &m, sidecar, err, errLen, soft, sizeof(soft))) {
        lua_close(L);
        free(src);
        return NULL;
    }
    if (m.api > SCENARIO_API_VERSION) {
        scnFmt(err, errLen,
               "scenario: %s asks for api %d and this server is api %d — "
               "the server is too old to run it",
               sidecar, m.api, SCENARIO_API_VERSION);
        lua_close(L);
        free(src);
        return NULL;
    }

    h = (ScenarioHost *)calloc(1, sizeof(*h));
    if (h == NULL) {
        scnFmt(err, errLen, "scenario: out of memory");
        lua_close(L);
        free(src);
        return NULL;
    }
    h->sim      = sim;
    h->L        = L;
    h->manifest = m;
    h->src      = src;
    h->srcLen   = srcLen;
    h->active   = true;
    snprintf(h->sidecar, sizeof(h->sidecar), "%s", sidecar);
    snprintf(h->chunkName, sizeof(h->chunkName), "%s", chunkName);
    snprintf(h->lastError, sizeof(h->lastError), "%s", soft);

    serverSimSetScenarioRoundStart(sim, scnRoundStart, h);
    return h;
}

void scenarioHostDetach(ScenarioHost *h) {
    if (h == NULL) {
        return;
    }
    if (h->sim != NULL) {
        serverSimSetScenarioRoundStart(h->sim, NULL, NULL);
    }
    if (h->L != NULL) {
        lua_close(h->L);
    }
    free(h->src);
    free(h);
}

bool scenarioHostIsActive(const ScenarioHost *h) {
    return h != NULL && h->active;
}

const char *scenarioHostName(const ScenarioHost *h) {
    return (h != NULL) ? h->manifest.name : "";
}

const char *scenarioHostDescription(const ScenarioHost *h) {
    return (h != NULL) ? h->manifest.description : "";
}

const char *scenarioHostSidecarPath(const ScenarioHost *h) {
    return (h != NULL) ? h->sidecar : "";
}

const char *scenarioHostLastError(const ScenarioHost *h) {
    return (h != NULL) ? h->lastError : "";
}

const ScenarioManifest *scenarioHostManifest(const ScenarioHost *h) {
    return (h != NULL) ? &h->manifest : NULL;
}
