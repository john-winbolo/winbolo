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
 *  The triggers are held against the same catalogue the
 *  round runs them from. Nothing the router cannot make
 *  sense of raises at run time — it is skipped, silently,
 *  which is right in the middle of a round and useless to
 *  an author — so a hook, a field, an operator, an op or an
 *  argument count that will not work is refused here
 *  instead, before a round is ever started on it.
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
#include "scenario_manifest_json.h" /* scnManifestTrigOpName — an operator's
                                     * own word in a message about it */
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

/* Whether a character can stand inside a rule's name. The names are lower
 * case, digits and underscore, so anything else either side of a match ends
 * the word. */
static bool scnRuleNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/* Where a rule's name stands in text as a word of its own, or NULL. A pair's
 * reason names both of its rules, and one name can be the tail of another, so
 * a bare substring would match inside the longer one and tie the report to
 * the wrong row. */
static const char *scnNameWholeIn(const char *text, const char *name) {
    const size_t len = strlen(name);
    const char  *at  = text;

    if (len == 0) {
        return NULL;
    }
    while ((at = strstr(at, name)) != NULL) {
        if ((at == text || !scnRuleNameChar(at[-1])) &&
            !scnRuleNameChar(at[len])) {
            return at;
        }
        at++;
    }
    return NULL;
}

/* The rule of the table that a reason names first. simRulesCheck answers a
 * fault and a sentence and nothing else, so which rule broke is read back out
 * of the sentence: a pair states the rule that has to move ahead of the one
 * it is measured against, so the first name in it is the row an author has to
 * go to. -1 where the sentence names none of the table's rules, which leaves
 * the caller to key the report on the table itself. */
static int scnRuleNamedFirst(const ScenarioManifest *m, const char *why) {
    const char *first = NULL;
    int         rule  = -1;
    uint16_t    i;

    for (i = 0; i < m->numRules && i < SCN_MANIFEST_RULES_MAX; i++) {
        const char *name = simRulesRuleName((int)m->rules[i].rule);
        const char *at;

        if (name == NULL || name[0] == '\0') {
            continue;
        }
        at = scnNameWholeIn(why, name);
        if (at != NULL && (first == NULL || at < first)) {
            first = at;
            rule  = (int)m->rules[i].rule;
        }
    }
    return rule;
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
        const int named = scnRuleNamedFirst(m, why);

        /* Under the rule the reason names, so the editor's row for the rule
           that has to move is the row the mark lands on: the form ties a row
           to a report by "rules.<name>" and nothing else, and a report keyed
           on the table itself can never match one. That key is what is left
           when the sentence names no rule the table sets. */
        if (named >= 0) {
            snprintf(key, sizeof(key), "rules.%s", simRulesRuleName(named));
            scnIssueAdd(out, key, "%s", why);
        } else {
            scnIssueAdd(out, "rules", "%s", why);
        }
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

/* ── Triggers ─────────────────────────────────────────────────────── */

/* Every field one catalogue row can earn. Five parameters is the widest the
 * catalogue has and each earns at most one derived field beside itself, so
 * ten is the most any row reaches and this has room over. */
#define SCN_VALIDATE_TRIG_FIELDS 16

/* The names one hook's payload carries, read off the catalogue once per
 * trigger and then asked about a row at a time. */
typedef struct {
    ScnLuaFnField field[SCN_VALIDATE_TRIG_FIELDS];
    size_t        count;
} ScnTrigFields;

/* The catalogue row a name answers to, hook or policy, and NULL for a name
 * the surface has not got. *row comes back as the index scenarioLuaFnFields
 * takes, which is the position in the same list. */
static const ScnLuaFnRow *scnFnNamed(const char *name, size_t *row) {
    const ScnLuaFnRow *rows;
    size_t             n = 0;
    size_t             i;

    rows = scenarioLuaFunctions(&n);
    for (i = 0; i < n; i++) {
        if (strcmp(rows[i].name, name) == 0) {
            *row = i;
            return &rows[i];
        }
    }
    return NULL;
}

/* The op row a name answers to, and NULL for a name the game table has not
 * got. */
static const ScnLuaRow *scnOpNamed(const char *name) {
    const ScnLuaRow *rows;
    size_t           n = 0;
    size_t           i;

    rows = scenarioLuaRows(&n);
    for (i = 0; i < n; i++) {
        if (strcmp(rows[i].name, name) == 0) {
            return &rows[i];
        }
    }
    return NULL;
}

static void scnTrigFieldsOf(size_t row, ScnTrigFields *out) {
    out->count = scenarioLuaFnFields(row, out->field,
                                     (size_t)SCN_VALIDATE_TRIG_FIELDS);
    if (out->count > (size_t)SCN_VALIDATE_TRIG_FIELDS) {
        out->count = (size_t)SCN_VALIDATE_TRIG_FIELDS;
    }
}

static const ScnLuaFnField *scnTrigFieldNamed(const ScnTrigFields *f,
                                              const char *name) {
    size_t i;

    for (i = 0; i < f->count; i++) {
        if (strcmp(f->field[i].name, name) == 0) {
            return &f->field[i];
        }
    }
    return NULL;
}

/* Whether a field answers a set of names rather than one value. It is the
 * derivation that decides, not the type alone: on_enter_region's own name
 * parameter is typed as a region and is a plain string the payload carries,
 * which the router reads through derive == nil as the one value it is. So eq
 * on it holds, and reading the type alone would refuse a test that runs. */
static bool scnTrigFieldIsSet(const ScnLuaFnField *f) {
    return f->derived &&
           (f->type == SCN_PARAM_TAG || f->type == SCN_PARAM_REGION);
}

static bool scnTagOnKind(const ScnManifestTags *arr, int maxEntity,
                         const char *tag) {
    int e;
    int i;

    for (e = 1; e <= maxEntity; e++) {
        for (i = 0; i < (int)arr[e].count; i++) {
            if (strcmp(arr[e].tag[i], tag) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool scnTagCarried(const ScenarioManifest *m, const char *tag) {
    return scnTagOnKind(m->pillTags, MAX_PILLS, tag) ||
           scnTagOnKind(m->baseTags, MAX_BASES, tag) ||
           scnTagOnKind(m->startTags, MAX_STARTS, tag);
}

/* Whether something has already been said about this key. The reader
 * reports into this same list, under the same key, and runs before any of
 * the checks below, so a row it could not take whole is one this pass has
 * nothing to add to. */
static bool scnSaidAlready(const ScnValidateResult *out, const char *key) {
    uint16_t i;

    if (out == NULL) {
        return false;
    }
    for (i = 0; i < out->count; i++) {
        if (strcmp(out->issues[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

/* One test of one trigger: the field it names, the operator it asks with,
 * and the name on its right where that name is one the table answers for.
 *
 * A region a test names is not one of those. game.define_region names a
 * rectangle for the rest of the round, so a name the table does not carry
 * may still be a region by the time the trigger fires, and refusing it here
 * would refuse a scenario that runs. */
static void scnCheckTrigCond(const ScenarioManifest *m, const ScnTrigCond *c,
                             const ScnTrigFields *fields, const char *when,
                             unsigned trig, unsigned at,
                             ScnValidateResult *out) {
    const ScnLuaFnField *f;
    char                 key[SCN_VALIDATE_KEY_LEN];

    /* A row that names no field at all is the reader's report rather than
       this one's, and saying it twice would be the same fault twice. */
    if (c->field[0] == '\0') {
        return;
    }
    snprintf(key, sizeof(key), "triggers[%u].where[%u]", trig, at);

    f = scnTrigFieldNamed(fields, c->field);
    if (f == NULL) {
        scnIssueAdd(out, key, "'%s' is no field of %s, so this test never "
                              "holds", c->field, when);
        return;
    }

    /* What the test asks with, before what it asks about. An operator none
       of the seven match is stored as one the table cannot name, so there is
       no word to quote back: the key names the row, and the seven are listed
       for the author to read their own row against.

       A row that named no operator at all arrives here the same way, and is
       the reader's report rather than this one's, the way a row that named
       no field is. Only the reader can tell the two apart, so its having
       spoken about this row is what says which this is. */
    if (c->op == SCN_TRIG_CMP_UNKNOWN) {
        if (!scnSaidAlready(out, key)) {
            scnIssueAdd(out, key,
                        "the operator is not one the surface has, so this "
                        "test never holds; the seven are 'eq', 'ne', 'lt', "
                        "'lte', 'gt', 'gte' and 'in'");
        }
        return;
    }

    if (scnTrigFieldIsSet(f)) {
        if (c->op != SCN_TRIG_CMP_IN && c->op != SCN_TRIG_CMP_NE) {
            const char *op = scnManifestTrigOpName(c->op);
            scnIssueAdd(out, key,
                        "'%s' is a set of names and '%s' asks what a set "
                        "cannot answer; only 'in' and 'ne' hold on one",
                        c->field, (op != NULL) ? op : "");
        }
    } else if (c->op == SCN_TRIG_CMP_IN) {
        scnIssueAdd(out, key,
                    "'in' asks whether a set holds a name and '%s' is one "
                    "value; a test states one value and never a list",
                    c->field);
    }

    /* The right-hand side. A reference is read off the same payload the
       field is, so it is held to the same set of names, and to answering one
       value: a set has none to be compared against, and the router gives up
       on the row rather than guess which of the names was meant. */
    if (c->value.kind == SCN_TRIG_VAL_FIELD) {
        if (c->value.text[0] != '\0') {
            const ScnLuaFnField *v = scnTrigFieldNamed(fields, c->value.text);

            if (v == NULL) {
                scnIssueAdd(out, key,
                            "the value names '%s', which is no field of %s, "
                            "so this test never holds", c->value.text, when);
            } else if (scnTrigFieldIsSet(v)) {
                scnIssueAdd(out, key,
                            "the value names '%s', which is a set of names "
                            "and not one value, so this test never holds",
                            c->value.text);
            }
        }
        return;
    }

    /* And a tag, which only the table itself answers for: nothing puts one
       on an entity at run time, so a tag a test names is in the table or
       nowhere. Only a literal is knowable here — a reference is whatever the
       payload carries when the trigger fires. */
    if (c->value.kind != SCN_TRIG_VAL_STRING || c->value.text[0] == '\0') {
        return;
    }
    if (f->type == SCN_PARAM_TAG && !scnTagCarried(m, c->value.text)) {
        scnIssueAdd(out, key, "nothing is tagged '%s', so this test never "
                              "holds", c->value.text);
    }
}

/* The arguments of one action from `from` on, where one names a field rather
 * than stating a value. The router reads a reference as the trigger fires and
 * gives up on the whole action where it cannot — a name the hook has not got,
 * and a name that answers a set rather than the one value an argument is — so
 * an action written either way does nothing and says nothing about it.
 *
 * key is the action's own, and a report goes under the argument's position on
 * the end of it, spelled as both readers spell it and counted from zero. from
 * is 0 for an op of the game table and 1 for call, whose first argument is a
 * function's name and is held to being written down rather than read off the
 * payload.
 *
 * fields is NULL where the hook is one the surface has not got: there is
 * nothing to hold a reference to until the hook is known, and the trigger's
 * own key has already been told that it is not.
 *
 * op is the row the action names, for the one place a reference's own type
 * has to answer to the argument it is written into: every seat on a payload
 * carries the team it is on as a field beside it, and a team written where a
 * seat goes is a small number the op reads as a seat. It is NULL for call,
 * whose function is the script's own and takes whatever the script reads. */
static void scnCheckTrigActArgs(const ScnTrigAct *a,
                                const ScnTrigFields *fields, const char *when,
                                const ScnLuaRow *op, const char *key,
                                size_t from, ScnValidateResult *out) {
    size_t i;

    if (fields == NULL) {
        return;
    }
    for (i = from; i < (size_t)a->numArgs; i++) {
        const ScnLuaFnField *f;
        char                 slot[SCN_VALIDATE_KEY_LEN];

        if (a->args[i].kind != SCN_TRIG_VAL_FIELD ||
            a->args[i].text[0] == '\0') {
            continue;
        }
        snprintf(slot, sizeof(slot), "%s[%u]", key, (unsigned)i);

        f = scnTrigFieldNamed(fields, a->args[i].text);
        if (f == NULL) {
            scnIssueAdd(out, slot, "'%s' is no field of %s, so this action "
                                   "never runs", a->args[i].text, when);
        } else if (scnTrigFieldIsSet(f)) {
            scnIssueAdd(out, slot,
                        "'%s' is a set of names and an argument is one "
                        "value, so this action never runs", a->args[i].text);
        } else if (op != NULL && i < op->paramCount &&
                   f->type == SCN_PARAM_TEAM &&
                   (op->params[i].type == SCN_PARAM_SLOT ||
                    op->params[i].type == SCN_PARAM_OWNER)) {
            scnIssueAdd(out, slot,
                        "'%s' is a team and %s takes a seat as its '%s', so "
                        "this action reaches the seat numbered like that "
                        "team rather than the player meant", a->args[i].text,
                        a->op, op->params[i].name);
        }
    }
}

/* One action of one trigger: the op it names and the arguments it was
 * given.
 *
 * fields is the hook's payload and when its name, or both NULL where the
 * hook is one the surface has not got. */
static void scnCheckTrigAct(const ScnTrigAct *a, const ScnTrigFields *fields,
                            const char *when, unsigned trig, unsigned at,
                            ScnValidateResult *out) {
    const ScnLuaRow *op;
    char             key[SCN_VALIDATE_KEY_LEN];
    size_t           required = 0;
    size_t           i;

    /* An action naming no op is the reader's report, as above. */
    if (a->op[0] == '\0') {
        return;
    }
    snprintf(key, sizeof(key), "triggers[%u].actions[%u]", trig, at);

    /* call runs a top-level function of the author's own script rather than
       a row of the game table, so none of what an op is held to applies to
       it. Two things do. The name is one the author writes down: read off the
       payload instead, it is whichever function the payload happened to carry
       when the trigger fired, and a hook handed a line of chat would let the
       player who typed it pick. What that function does with the arguments
       after the name is the script's own business, but a reference among them
       is read off the payload like any other and is held to the same
       fields.

       A name that is there but empty names no function, the way no first
       argument at all names none, so the two share a sentence. A name too
       long to sit in an argument rides on the action's own text and leaves
       the argument's empty, which is why the flag is read and not the
       bytes. */
    if (strcmp(a->op, "call") == 0) {
        if (a->numArgs > 0 && a->args[0].kind == SCN_TRIG_VAL_FIELD) {
            scnIssueAdd(out, key,
                        "call states the name of the function it runs, and "
                        "'%s' reads one off the payload as the trigger fires; "
                        "the name is written here instead", a->args[0].text);
        } else if (a->numArgs == 0 ||
                   a->args[0].kind != SCN_TRIG_VAL_STRING ||
                   (!a->args[0].inText && a->args[0].text[0] == '\0')) {
            scnIssueAdd(out, key,
                        "call runs a function of the script's own, and its "
                        "first argument is that function's name");
        }
        scnCheckTrigActArgs(a, fields, when, NULL, key, 1, out);
        return;
    }

    op = scnOpNamed(a->op);
    if (op == NULL) {
        scnIssueAdd(out, key, "'%s' is no op the game table carries, so this "
                              "action does nothing", a->op);
        return;
    }
    if (!scenarioLuaOpIsScalar(op)) {
        scnIssueAdd(out, key,
                    "%s takes a table or a function, which an action cannot "
                    "state; a script's own Lua reaches it through the call "
                    "action", a->op);
        return;
    }

    /* The last argument the op insists on. Read as a position rather than a
       count, so an optional one written before a required one still leaves
       the required one counted. */
    for (i = 0; i < op->paramCount; i++) {
        if (!op->params[i].optional) {
            required = i + 1;
        }
    }
    if ((size_t)a->numArgs < required ||
        (size_t)a->numArgs > op->paramCount) {
        const char *s = (a->numArgs == 1) ? "" : "s";

        if (required == op->paramCount) {
            scnIssueAdd(out, key, "%s is given %u argument%s and takes %u",
                        a->op, (unsigned)a->numArgs, s,
                        (unsigned)op->paramCount);
        } else {
            scnIssueAdd(out, key, "%s is given %u argument%s and takes %u to "
                                  "%u", a->op, (unsigned)a->numArgs, s,
                        (unsigned)required, (unsigned)op->paramCount);
        }
    }

    /* And every argument, from the first: an op of the game table names its
       function here rather than in an argument. */
    scnCheckTrigActArgs(a, fields, when, op, key, 0, out);
}

/* Whether a trigger could do anything at all: the hook it listens on, the
 * fields its tests name, the operators those fields take, the ops its
 * actions call and the arguments those ops were given.
 *
 * The router skips what it cannot make sense of and says nothing about it,
 * which is right at run time — raising there would spend one of the
 * scenario's errors on a fault nobody can fix mid-round — and leaves an
 * author with a trigger that never fires and no reason why. This is where
 * they are told instead, so every skip the router makes is unreachable by
 * the time a round starts.
 *
 * All of it is answered by the catalogue and the table itself, so an editor
 * holding a script and no map is told what a server would tell it. A
 * trigger has no name, so the key is its position, counted from zero as the
 * two readers count it. */
static void scnCheckTriggers(const ScenarioManifest *m,
                             ScnValidateResult *out) {
    uint8_t i;

    for (i = 0; i < m->numTriggers; i++) {
        const ScnTrigger  *t     = &m->triggers[i];
        const ScnLuaFnRow *fn    = NULL;
        ScnTrigFields      fields;
        char               key[SCN_VALIDATE_KEY_LEN];
        size_t             row   = 0;
        uint8_t            j;

        fields.count = 0;
        snprintf(key, sizeof(key), "triggers[%u]", (unsigned)i);

        /* A trigger with no when at all is the reader's report. */
        if (t->when[0] != '\0') {
            fn = scnFnNamed(t->when, &row);
            if (fn == NULL) {
                scnIssueAdd(out, key, "'%s' names no hook, so this trigger "
                                      "never runs", t->when);
            } else if (fn->kind == SCN_FN_POLICY) {
                /* Real name, wrong half of the surface, and the reason is
                   not the one an unknown name gets: the host asks a policy
                   a question and reads the answer, and a list of actions
                   has none to give it. */
                scnIssueAdd(out, key,
                            "'%s' is a policy, which answers a question a "
                            "list of actions cannot, so this trigger never "
                            "runs", t->when);
                fn = NULL;
            } else {
                scnTrigFieldsOf(row, &fields);
            }
        }

        /* The tests are read against the hook's payload, so there is nothing
           to hold them to until the hook is one. The actions are held to the
           catalogue either way, and to the payload as well where there is
           one: an argument may name a field the way a test does. */
        if (fn != NULL) {
            for (j = 0; j < t->numWhere; j++) {
                scnCheckTrigCond(m, &t->where[j], &fields, t->when,
                                 (unsigned)i, (unsigned)j, out);
            }
        }
        for (j = 0; j < t->numActions; j++) {
            scnCheckTrigAct(&t->actions[j], (fn != NULL) ? &fields : NULL,
                            (fn != NULL) ? t->when : NULL, (unsigned)i,
                            (unsigned)j, out);
        }
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
    /* The triggers read no map either: a hook, a field, an operator, an op
       and an argument count are the catalogue's business, and a tag a test
       names is the table's own. Last of the checks because a
       table of 64 triggers can report several faults apiece, and the list
       holds 64 in all — anything after this could be pushed off the end by
       a trigger table nobody has fixed yet. */
    scnCheckTriggers(&out->manifest, out);

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
