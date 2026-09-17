/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Validate
 *Filename:      scenario_validate.c
 *Author:        John Morrison
 *Purpose:
 *  Checks a script without running it as a scenario: the
 *  chunk is loaded and run once in a state of its own, so
 *  the scenario table it declares exists, and everything
 *  after that is read off the struct the parse produced.
 *
 *  The state carries a game table of the right shape and no
 *  substance. Every row of the real surface is there as a
 *  call that does nothing and answers nothing, and the two
 *  sets of numbers are there as themselves, so a chunk that
 *  reads game.TERRAIN.grass at its top level loads here as
 *  it loads at a round start. Nothing on the table reaches a
 *  sim, no hook is called and no tick is ever run.
 *
 *  The rule check goes through the scenario surface rather
 *  than through the rule table directly: this file sees
 *  src/bolo/public/ and src/bolo/scenario_api/, and asking
 *  the sim what a set would do is what that surface is for.
 *
 *  The parse's own complaints — a key naming no rule, an
 *  entity index the map could not hold, a fifth tag on an
 *  entity that carries four — arrive here through the report
 *  the reader is handed, so an author is told about them in
 *  the same list as everything below.
 *********************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

#include "brain_list.h"          /* brainListResolve — whether this server has
                                  * the brain a team names */
#include "server_sim.h"          /* the entity counts, MAX_TANKS,
                                  * MAP_ARRAY_SIZE */
#include "server_sim_scenario.h" /* serverSimCheckScenarioRules, SCN_PATH_MAX */

#include "scenario_host.h"
#include "scenario_manifest.h"
#include "scenario_lua.h"
#include "sim_rules_names.h"      /* simRulesRuleName — a rule in a reason */
#include "scenario_validate.h"

/* One line from the parse or from Lua, before it becomes an issue. Lua's own
 * messages carry the file path and the line ahead of the text, so this is
 * longer than the message field it is copied into. */
#define SCN_VALIDATE_LINE_LEN 512

/* ── The stub table ───────────────────────────────────────────────── */

/* Every call on the surface, as one function. A chunk that calls one from its
 * top level gets nothing back, which is what a read of a sim that is not
 * there has to be. */
static int scnStubRow(lua_State *L) {
    (void)L;
    return 0;
}

/* The shape of the game table with none of its substance. The names come off
 * the registry the real table is built from, so a row added to the surface is
 * on this table too with no second edit. */
static void scnInstallStubGame(lua_State *L) {
    const ScnLuaRow       *rows;
    const ScnLuaConst     *consts;
    const ScnLuaTerrain   *terrain;
    const ScnLuaWordTable *words;
    size_t                 n;
    size_t                 i;

    lua_newtable(L);

    rows = scenarioLuaRows(&n);
    for (i = 0; i < n; i++) {
        lua_pushcclosure(L, scnStubRow, 0);
        lua_setfield(L, -2, rows[i].name);
    }

    /* The numbers are the real ones. A chunk comparing against one of them,
       or indexing a table of its own by a terrain code, gets the value the
       round would give it rather than a nil it would raise on. */
    consts = scenarioLuaConsts(&n);
    for (i = 0; i < n; i++) {
        lua_pushinteger(L, (lua_Integer)consts[i].value);
        lua_setfield(L, -2, consts[i].name);
    }

    lua_newtable(L);
    terrain = scenarioLuaTerrain(&n);
    for (i = 0; i < n; i++) {
        lua_pushinteger(L, (lua_Integer)terrain[i].code);
        lua_setfield(L, -2, terrain[i].name);
    }
    lua_setfield(L, -2, "TERRAIN");

    /* And the word tables beside it, for the same reason: a chunk reading
       game.COLOUR.red at its top level reads the number the round would give
       it rather than indexing a nil. */
    words = scenarioLuaWordTables(&n);
    for (i = 0; i < n; i++) {
        size_t w;
        lua_newtable(L);
        for (w = 0; w < words[i].count; w++) {
            lua_pushinteger(L, (lua_Integer)words[i].words[w].value);
            lua_setfield(L, -2, words[i].words[w].word);
        }
        lua_setfield(L, -2, words[i].name);
    }

    lua_setglobal(L, "game");
}

/* ── Which line a problem is on ───────────────────────────────────── */

/* Whether a character can be part of a name, which is what makes a word found
 * in the source the whole word rather than the tail of a longer one. */
static bool scnIsWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* The 1-based line the word first appears on as a word of its own, or 0 where
 * it does not appear at all. */
static int scnLineOfWord(const char *src, size_t srcLen, const char *word) {
    size_t wordLen = strlen(word);
    size_t i;
    int    line = 1;

    if (wordLen == 0) {
        return 0;
    }
    for (i = 0; i < srcLen; i++) {
        if (src[i] == '\n') {
            line++;
            continue;
        }
        if (src[i] != word[0] || i + wordLen > srcLen) {
            continue;
        }
        if (memcmp(src + i, word, wordLen) != 0) {
            continue;
        }
        if (i > 0 && scnIsWordChar(src[i - 1])) {
            continue;
        }
        if (i + wordLen < srcLen && scnIsWordChar(src[i + wordLen])) {
            continue;
        }
        return line;
    }
    return 0;
}

/* The line a key points at: its last name first, then the one before it, and
 * so on up. "lobby.teams[2].bots" asks for bots, then teams, then lobby, so a
 * key whose last name the author did not write — a rule set by a table the
 * file builds elsewhere — still lands on the block it belongs to. A subscript
 * is not part of a name. */
static int scnLineForKey(const char *src, size_t srcLen, const char *key) {
    char  work[SCN_VALIDATE_KEY_LEN];
    char *name[SCN_VALIDATE_KEY_LEN / 2];
    int   n = 0;
    int   i;
    char *p;

    if (src == NULL || key == NULL || key[0] == '\0') {
        return 0;
    }
    snprintf(work, sizeof(work), "%s", key);

    name[n++] = work;
    for (p = work; *p != '\0'; p++) {
        if (*p == '.' && n < (int)(sizeof(name) / sizeof(name[0]))) {
            *p        = '\0';
            name[n++] = p + 1;
        }
    }
    for (i = 0; i < n; i++) {
        char *bracket = strchr(name[i], '[');
        if (bracket != NULL) {
            *bracket = '\0';
        }
    }

    for (i = n - 1; i >= 0; i--) {
        int line = scnLineOfWord(src, srcLen, name[i]);
        if (line > 0) {
            return line;
        }
    }
    return 0;
}

/* One pass at the end over everything still without a line, so an issue that
 * came with a line Lua gave keeps it. */
static void scnAttributeLines(ScnValidateResult *out, const char *src,
                              size_t srcLen) {
    uint16_t i;

    for (i = 0; i < out->count; i++) {
        if (out->issues[i].line == 0) {
            out->issues[i].line =
                scnLineForKey(src, srcLen, out->issues[i].key);
        }
    }
}

/* The line out of a Lua message, which reads path:LINE: text. Looked for past
 * the end of the path rather than from the first colon, because a path can
 * carry one of its own. 0 for a message with no line in it. */
static int scnLineFromLuaError(const char *msg, const char *path) {
    const char *p = NULL;

    if (msg == NULL) {
        return 0;
    }
    if (path != NULL && path[0] != '\0') {
        p = strstr(msg, path);
        if (p != NULL) {
            p += strlen(path);
        }
    }
    if (p == NULL) {
        p = msg;
    }
    for (; *p != '\0'; p++) {
        if (*p == ':' && p[1] >= '0' && p[1] <= '9') {
            const char *d    = p + 1;
            int         line = 0;
            while (*d >= '0' && *d <= '9') {
                line = line * 10 + (*d - '0');
                d++;
            }
            if (*d == ':') {
                return line;
            }
        }
    }
    return 0;
}

/* ── The checks ───────────────────────────────────────────────────── */

static void scnCheckApi(const ScenarioManifest *m, ScnValidateResult *out) {
    if (m->api > SCENARIO_API_VERSION) {
        scnIssueAdd(out, "api",
                    "api %d is newer than this server's api %d",
                    m->api, SCENARIO_API_VERSION);
    }
}

/* The game type the table asks for. The attach reads it through the same
 * word set a spawn op's loadout takes, and a word that set does not hold is
 * dropped there without a sound — the round plays strict tournament, as a
 * round naming no game does, and the author is never told the line did
 * nothing. */
static void scnCheckGame(const ScenarioManifest *m, ScnValidateResult *out) {
    int base;

    if (m->game[0] == '\0') {
        return;
    }
    if (!scenarioLuaLoadoutFromWord(m->game, &base)) {
        scnIssueAdd(out, "game",
                    "game is '%s', and the game types are 'open', "
                    "'tournament' and 'strict'; the round plays strict",
                    m->game);
    }
}

/* The template the lobby is seated from: the human cap, and per team the
 * number it is, the bots it asks for, the brain it names them with and the
 * init table they are built with. */
static void scnCheckLobby(const ScenarioManifest *m, ScnValidateResult *out) {
    const ScnManifestLobby *lob = &m->lobby;
    char                    key[SCN_VALIDATE_KEY_LEN];
    uint8_t                 i;
    uint8_t                 j;

    if (lob->maxPlayers != 0 && lob->maxPlayers > MAX_TANKS) {
        scnIssueAdd(out, "lobby.max_players",
                    "max_players is %u; 0 leaves the server's own cap and a "
                    "stated one runs 1 to %d",
                    (unsigned)lob->maxPlayers, MAX_TANKS);
    }

    for (i = 0; i < lob->numTeams; i++) {
        const ScnManifestTeam *t = &lob->teams[i];

        /* One below MAX_TANKS is the top the engine seats: the template drops
           an id at MAX_TANKS or above, and the sim's team table is keyed by
           the same range. A team past it would be dropped without a word,
           which is the case this check exists to catch. The bound is derived
           rather than written out, so the two cannot drift apart. */
        snprintf(key, sizeof(key), "lobby.teams[%u].id", (unsigned)(i + 1));
        if (t->id < 1 || t->id >= MAX_TANKS) {
            scnIssueAdd(out, key, "team id %u is outside 1 to %d",
                        (unsigned)t->id, MAX_TANKS - 1);
        } else {
            for (j = 0; j < i; j++) {
                if (lob->teams[j].id == t->id) {
                    scnIssueAdd(out, key, "team id %u is team %u's as well",
                                (unsigned)t->id, (unsigned)(j + 1));
                    break;
                }
            }
        }

        /* A max_bots of zero states no ceiling, which is not the same as
           allowing no bots. */
        snprintf(key, sizeof(key), "lobby.teams[%u].bots", (unsigned)(i + 1));
        if (t->maxBots != 0 && t->bots > t->maxBots) {
            scnIssueAdd(out, key, "%u bots, above the max_bots of %u",
                        (unsigned)t->bots, (unsigned)t->maxBots);
        } else if (t->bots > MAX_TANKS) {
            scnIssueAdd(out, key, "%u bots, and a game holds %d players",
                        (unsigned)t->bots, MAX_TANKS);
        }

        /* The brain the team names, held against the brains this server has.
           Neither of these refuses the scenario: the map still loads and the
           team's seats fall back to the server's own brain. What they are for
           is telling an operator they need GoalHunter_1.7 before a round is
           started, rather than leaving them to find it out from a wave that
           seats bots which field nothing.

           A "package:NAME" carries no separator, so it arrives here as a name
           like any other and is reported as a brain this server does not have,
           which is what it is. */
        if (t->brain[0] != '\0') {
            char path[SCN_PATH_MAX];

            snprintf(key, sizeof(key), "lobby.teams[%u].brain",
                     (unsigned)(i + 1));
            if (strpbrk(t->brain, "/\\") != NULL) {
                scnIssueAdd(out, key,
                            "'%s' is a path; a scenario names a brain, which "
                            "is the directory under the server's brains/ — "
                            "'GoalHunter_1.7', not a path to it", t->brain);
            } else if (!brainListResolve(t->brain, path, sizeof(path))) {
                scnIssueAdd(out, key,
                            "'%s' names no brain this server has; the team's "
                            "seats take the server's own brain", t->brain);
            }
        }

        /* A pair of the init table the read could not take. The reader keeps
           the pairs before it and names this one, so the team still has an
           init and the author is told what fell out of it. The caps are
           derived from the table's own, so the sentence cannot drift from
           what the reader will accept. */
        if (t->initBadKey[0] != '\0') {
            snprintf(key, sizeof(key), "lobby.teams[%u].init",
                     (unsigned)(i + 1));
            scnIssueAdd(out, key,
                        "'%s' does not fit: an init pair is a name of at most "
                        "%d bytes with a string or number value of at most "
                        "%d, and a team holds at most %d pairs",
                        t->initBadKey, (int)SCN_TABLE_KEY_LEN - 1,
                        (int)SCN_TABLE_VALUE_LEN - 1, SCN_TABLE_MAX);
        }
    }
}

/* What a set of rules would do, asked of the round where there is one and of
 * the classic table where there is not. A rule's bounds belong to the field
 * it is declared in rather than to a game, so the answer an editor gets with
 * no sim is the answer the server gives with one. */
static ScnOpResult scnCheckRuleSet(const ServerSim *sim, const uint16_t *rules,
                                   const double *values, uint16_t count,
                                   char *why, size_t whyLen) {
    if (sim == NULL) {
        return scenarioCheckRulesFromClassic(rules, values, count, why,
                                             whyLen);
    }
    return serverSimCheckScenarioRules(sim, rules, values, count, why, whyLen);
}

/* Every rule the table sets, one at a time and then all at once. One at a
 * time says which rule an author has to move; all at once is what finds a
 * pair two of them break between them, where each on its own stands up. */
static void scnCheckRules(const ServerSim *sim, const ScenarioManifest *m,
                          ScnValidateResult *out) {
    uint16_t    rules[SCN_MANIFEST_RULES_MAX];
    double      values[SCN_MANIFEST_RULES_MAX];
    char        why[SCN_VALIDATE_MSG_LEN];
    char        key[SCN_VALIDATE_KEY_LEN];
    bool        sawPair = false;
    uint16_t    i;
    ScnOpResult r;

    for (i = 0; i < m->numRules && i < SCN_MANIFEST_RULES_MAX; i++) {
        const char *name = simRulesRuleName((int)m->rules[i].rule);

        rules[i]  = m->rules[i].rule;
        values[i] = m->rules[i].value;

        snprintf(key, sizeof(key), "rules.%s", name);
        r = scnCheckRuleSet(sim, &rules[i], &values[i], 1, why, sizeof(why));
        if (r == SCN_OP_RANGE || r == SCN_OP_PAIR) {
            if (why[0] != '\0') {
                scnIssueAdd(out, key, "%s", why);
            } else {
                /* A value no field can take at all is refused before the
                   check that words a reason. */
                scnIssueAdd(out, key, "%s is set to %g, which no rule holds",
                            name, values[i]);
            }
            if (r == SCN_OP_PAIR) {
                sawPair = true;
            }
        }
    }

    if (i == 0) {
        return;
    }
    r = scnCheckRuleSet(sim, rules, values, i, why, sizeof(why));
    /* Only a pair, and only one the pass above did not already name. A range
       is stated against the rule that carries it, and saying it again here
       under the whole table would be the same fault twice. */
    if (r == SCN_OP_PAIR && !sawPair && why[0] != '\0') {
        scnIssueAdd(out, "rules", "%s", why);
    }
}

/* One kind's tags against the map's own list. The parse has already refused an
 * index no map could hold; this is the narrower question of what this map
 * actually carries. */
static void scnCheckTagKind(const ScnManifestTags *arr, int maxEntity,
                            BYTE have, const char *key, const char *kind,
                            ScnValidateResult *out) {
    char where[SCN_VALIDATE_KEY_LEN];
    int  e;

    for (e = 1; e <= maxEntity; e++) {
        if (arr[e].count == 0 || e <= (int)have) {
            continue;
        }
        snprintf(where, sizeof(where), "tags.%s[%d]", key, e);
        scnIssueAdd(out, where, "%s %d is tagged and the map has %u",
                    kind, e, (unsigned)have);
    }
}

static void scnCheckTags(const ServerSim *sim, const ScenarioManifest *m,
                         ScnValidateResult *out) {
    scnCheckTagKind(m->pillTags, MAX_PILLS, serverSimGetPillCount(sim),
                    "pills", "pill", out);
    scnCheckTagKind(m->baseTags, MAX_BASES, serverSimGetBaseCount(sim),
                    "bases", "base", out);
    scnCheckTagKind(m->startTags, MAX_STARTS, serverSimGetStartCount(sim),
                    "starts", "start", out);
}

static void scnCheckRegions(const ScenarioManifest *m,
                            ScnValidateResult *out) {
    char    key[SCN_VALIDATE_KEY_LEN];
    uint8_t i;
    uint8_t j;

    for (i = 0; i < m->numRegions; i++) {
        const ScnManifestRegion *r = &m->regions[i];

        snprintf(key, sizeof(key), "regions.%s", r->name);

        if (r->w == 0 || r->h == 0) {
            scnIssueAdd(out, key,
                        "region '%s' is %u by %u and holds no square",
                        r->name, (unsigned)r->w, (unsigned)r->h);
        }
        if ((int)r->x + (int)r->w > MAP_ARRAY_SIZE ||
            (int)r->y + (int)r->h > MAP_ARRAY_SIZE) {
            scnIssueAdd(out, key,
                        "region '%s' runs to %d,%d and a map is %d square",
                        r->name, (int)r->x + (int)r->w,
                        (int)r->y + (int)r->h, MAP_ARRAY_SIZE);
        }
        for (j = 0; j < i; j++) {
            if (strcmp(m->regions[j].name, r->name) == 0) {
                scnIssueAdd(out, key, "region '%s' is named twice", r->name);
                break;
            }
        }
    }
}

static bool scnTagsAnywhere(const ScnManifestTags *arr, int maxEntity) {
    int e;

    for (e = 1; e <= maxEntity; e++) {
        if (arr[e].count > 0) {
            return true;
        }
    }
    return false;
}

/* A table that says it is not tied to its map, and then names things only one
 * map has. */
static void scnCheckBound(const ScenarioManifest *m, ScnValidateResult *out) {
    if (m->bound) {
        return;
    }
    if (scnTagsAnywhere(m->pillTags, MAX_PILLS) ||
        scnTagsAnywhere(m->baseTags, MAX_BASES) ||
        scnTagsAnywhere(m->startTags, MAX_STARTS)) {
        scnIssueAdd(out, "bound",
                    "bound is false and the table tags entities, which only "
                    "the map they are on has");
    }
    if (m->numRegions > 0) {
        scnIssueAdd(out, "bound",
                    "bound is false and the table names regions, which are "
                    "squares of the map they were measured on");
    }
}

/* ── The whole check ──────────────────────────────────────────────── */

/* The checks themselves, against source that has already been named and an out
 * the caller has cleared. Every entry point below comes through here, so a
 * script checked beside a map, one named directly and one still in an editor's
 * buffer are read the same way and report the same things.
 *
 * push is the manifest that goes on as the scenario global before the chunk,
 * or NULL. The three sites in scenario_host.c that load a packaged script do
 * the same thing in the same place, so a caller holding both halves of a
 * package is told what the load will say about them.
 *
 * The bytes are the caller's. Nothing here frees them. */
static bool scnValidateSource(const ServerSim *sim, const char *src,
                              size_t srcLen, const char *name,
                              const ScenarioManifest *push,
                              ScnValidateResult *out) {
    char           chunkName[SCN_SCRIPT_PATH_MAX + 2];
    char           err[SCN_VALIDATE_LINE_LEN];
    lua_State     *L;
    ScnParseReport rep;

    L = scnNewVm();
    if (L == NULL) {
        scnIssueAdd(out, "", "no memory for a Lua state to check %s", name);
        return false;
    }
    scnInstallStubGame(L);

    /* The manifest the caller holds, before the chunk, exactly where the host
       puts it. A script that declares no table of its own then reads back this
       one; a script that declares one replaces it. */
    if (push != NULL) {
        scnPushManifestGlobal(L, push);
    }

    /* The top level and no further. What the chunk defines is what the table
       below is read out of; the functions it left behind are never called. */
    snprintf(chunkName, sizeof(chunkName), "@%s", name);
    if (!scnRunChunk(L, src, srcLen, chunkName, err, sizeof(err))) {
        scnIssueAdd(out, "", "%s", err);
        if (out->count > 0) {
            out->issues[out->count - 1].line = scnLineFromLuaError(err, name);
        }
        scnCloseVm(L);
        return false;
    }

    rep.soft    = NULL;
    rep.softLen = 0;
    rep.sink    = out;
    if (!scnReadManifest(L, &out->manifest, name, err, sizeof(err), &rep)) {
        scnIssueAdd(out, "", "%s", err);
        scnCloseVm(L);
        return false;
    }
    out->haveManifest = true;

    scnCheckApi(&out->manifest, out);
    scnCheckGame(&out->manifest, out);
    scnCheckLobby(&out->manifest, out);
    /* The rules are checked whether or not there is a map: a rule's bounds
       and the pairs it sits in are the table's own, so the classic table
       stands in for a round's. The tags are the check that genuinely reads
       one — it asks how many pills and bases this map carries — so an editor
       holding a script and no map is told about its rules and left to find
       out about its tags when the script meets a map. */
    scnCheckRules(sim, &out->manifest, out);
    if (sim != NULL) {
        scnCheckTags(sim, &out->manifest, out);
    }
    scnCheckRegions(&out->manifest, out);
    scnCheckBound(&out->manifest, out);

    scnAttributeLines(out, src, srcLen);

    scnCloseVm(L);
    return out->count == 0;
}

/* The bytes, and the one case that is not a fault. The read is the whole of
 * what the two file entries do beyond the checks above. */
static bool scnValidateFile(const ServerSim *sim, const char *script,
                            ScnValidateResult *out) {
    char   err[SCN_VALIDATE_LINE_LEN];
    char  *src    = NULL;
    size_t srcLen = 0;
    bool   ok;

    /* No file at all is the ordinary case: a plain map, and nothing to check
       about it. A caller handing over an empty buffer is a different thing,
       and is checked as the empty script it is. */
    err[0] = '\0';
    if (!scnReadFile(script, &src, &srcLen, err, sizeof(err))) {
        if (err[0] == '\0') {
            return true;
        }
        scnIssueAdd(out, "", "%s", err);
        return false;
    }

    /* Nothing is pushed: a script found on disk beside a map is a loose
       script, which declares its own table or is not a scenario. */
    ok = scnValidateSource(sim, src, srcLen, script, NULL, out);
    free(src);
    return ok;
}

bool scenarioValidateMap(const ServerSim *sim, const char *mapPath,
                         ScnValidateResult *out) {
    char script[SCN_SCRIPT_PATH_MAX];

    if (out == NULL || mapPath == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (!scnScriptPath(mapPath, script, sizeof(script))) {
        scnIssueAdd(out, "", "%s leaves no room for a script name beside it",
                    mapPath);
        return false;
    }
    return scnValidateFile(sim, script, out);
}

bool scenarioValidateScript(const ServerSim *sim, const char *scriptPath,
                            ScnValidateResult *out) {
    if (out == NULL || scriptPath == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    return scnValidateFile(sim, scriptPath, out);
}

bool scenarioValidateSource(const ServerSim *sim, const char *text, size_t len,
                            const char *name, const ScenarioManifest *push,
                            ScnValidateResult *out) {
    if (out == NULL || text == NULL || name == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    return scnValidateSource(sim, text, len, name, push, out);
}
