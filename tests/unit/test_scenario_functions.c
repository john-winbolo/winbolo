/*
 * The function catalogue: the one list of what a scenario author writes,
 * and whether it describes the functions the host actually calls.
 *
 * scenarioLuaFunctions answers a row per hook and a row per policy, each
 * carrying the author's own parameter names. Nothing in the host reads
 * those names — they are there for a document and for the editor — so a row
 * that claims a parameter list the dispatch site does not push would be
 * wrong and would go on working. That is the failure these two cases are
 * for.
 *
 * run_scenario_functions_table
 *      — the shape of the rows: names, counts, types, which kind carries an
 *        answer, and the hook half against the hook list the host resolves
 *        its names from
 * run_scenario_functions_fields
 *      — the names a trigger may test on a row: the parameters themselves
 *        and the fields their types earn
 * run_scenario_functions_match_dispatch
 *      — every hook the harness can fire, with a handler that records how
 *        many arguments it was handed, held against what its row claims
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.callbacks, for the console
                                    * watcher the records are read off */
#include "server_sim_lifecycle.h"  /* SetLobbyEnabled, StartGame */
#include "control_event.h"
#include "input_packet.h"
#include "everard_map.h"
#include "scenario_host.h"
#include "scenario_lua.h"          /* the catalogue, and the two lists it is
                                    * built from */
#include "test_harness.h"

/* ── The lists, expanded here ─────────────────────────────────────── */

/* The hook names as the host resolves them. kScnHookNames is static to
 * scenario_host.c and nothing outside that file can read it, so this is the
 * same list expanded a second way rather than a second list: a name that
 * differs between this array and the table can only mean the table was not
 * built from SCN_HOOK_LIST. */
static const char *const kSfnHookNames[] = {
#define SFN_HOOK_NAME(id, name, kind, params) name,
    SCN_HOOK_LIST(SFN_HOOK_NAME)
#undef SFN_HOOK_NAME
};

static const char *const kSfnPolicyNames[] = {
#define SFN_POLICY_NAME(id, name, params, returns) name,
    SCN_POLICY_LIST(SFN_POLICY_NAME)
#undef SFN_POLICY_NAME
};

#define SFN_HOOK_NAME_COUNT                                                  \
    (sizeof(kSfnHookNames) / sizeof(kSfnHookNames[0]))
#define SFN_POLICY_NAME_COUNT                                                \
    (sizeof(kSfnPolicyNames) / sizeof(kSfnPolicyNames[0]))

/* An array of names holds this one. */
static bool sfnHolds(const char *const *names, size_t count,
                     const char *name) {
    size_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* ── 1. The shape of the rows ─────────────────────────────────────── */

int run_scenario_functions_table(void) {
    const ScnLuaFnRow *rows;
    size_t             count = 0;
    size_t             hooks = 0;
    size_t             policies = 0;
    size_t             i;
    size_t             j;

    rows = scenarioLuaFunctions(&count);
    UT_ASSERT_MSG(rows != NULL, "the catalogue answered nothing");
    UT_ASSERT_MSG(count == SFN_HOOK_NAME_COUNT + SFN_POLICY_NAME_COUNT,
                  "the catalogue holds %d rows; the lists hold %d hooks and "
                  "%d policies", (int)count, (int)SFN_HOOK_NAME_COUNT,
                  (int)SFN_POLICY_NAME_COUNT);

    for (i = 0; i < count; i++) {
        const ScnLuaFnRow *r = &rows[i];
        size_t             walked = 0;

        UT_ASSERT_MSG(r->name != NULL && r->name[0] != '\0',
                      "row %d has no name", (int)i);

        /* Every name is its own, across both halves: the editor inserts a
           stub by name and a document lists one entry per name, so two rows
           answering to one name would leave one of them unreachable. */
        for (j = 0; j < i; j++) {
            UT_ASSERT_MSG(strcmp(rows[j].name, r->name) != 0,
                          "'%s' is in the catalogue twice, at rows %d and %d",
                          r->name, (int)j, (int)i);
        }

        /* The names run to the terminator the array was built with, and the
           count is how many there are. */
        UT_ASSERT_MSG(r->params != NULL, "'%s' has no parameter array",
                      r->name);
        while (walked <= r->paramCount && r->params[walked].name != NULL) {
            UT_ASSERT_MSG(r->params[walked].name[0] != '\0',
                          "'%s' names an empty parameter at %d", r->name,
                          (int)walked);
            /* The terminator's type and nothing else. A real parameter left
               untyped would reach the field list as a value meaning "the end
               of the array" and be read as one. */
            UT_ASSERT_MSG(r->params[walked].type != SCN_PARAM_NONE,
                          "'%s' gives its parameter '%s' no type", r->name,
                          r->params[walked].name);
            walked++;
        }
        UT_ASSERT_MSG(walked == r->paramCount,
                      "'%s' claims %d parameters and names %d", r->name,
                      (int)r->paramCount, (int)walked);

        if (r->kind == SCN_FN_POLICY) {
            /* A policy is asked a question, so what it answers is part of
               what it is. A hook answers nothing at all. */
            UT_ASSERT_MSG(r->returns != NULL && r->returns[0] != '\0',
                          "the policy '%s' does not say what it answers",
                          r->name);
            UT_ASSERT_MSG(sfnHolds(kSfnPolicyNames, SFN_POLICY_NAME_COUNT,
                                   r->name),
                          "'%s' is a policy row the policy list does not "
                          "name", r->name);
            policies++;
        } else {
            UT_ASSERT_MSG(r->returns == NULL,
                          "the hook '%s' says it answers '%s'; a hook answers "
                          "nothing", r->name, r->returns);
            UT_ASSERT_MSG(sfnHolds(kSfnHookNames, SFN_HOOK_NAME_COUNT,
                                   r->name),
                          "'%s' is a hook row the hook list does not name",
                          r->name);
            hooks++;
        }
    }

    UT_ASSERT_MSG(hooks == SFN_HOOK_NAME_COUNT,
                  "the catalogue holds %d hook rows for %d hooks",
                  (int)hooks, (int)SFN_HOOK_NAME_COUNT);
    UT_ASSERT_MSG(policies == SFN_POLICY_NAME_COUNT,
                  "the catalogue holds %d policy rows for %d policies",
                  (int)policies, (int)SFN_POLICY_NAME_COUNT);

    /* And the other way round, so a hook dropped from the table is caught as
       well as one invented for it. */
    for (i = 0; i < SFN_HOOK_NAME_COUNT; i++) {
        bool found = false;
        for (j = 0; j < count && !found; j++) {
            found = (rows[j].kind != SCN_FN_POLICY &&
                     strcmp(rows[j].name, kSfnHookNames[i]) == 0);
        }
        UT_ASSERT_MSG(found, "the hook '%s' has no row in the catalogue",
                      kSfnHookNames[i]);
    }
    for (i = 0; i < SFN_POLICY_NAME_COUNT; i++) {
        bool found = false;
        for (j = 0; j < count && !found; j++) {
            found = (rows[j].kind == SCN_FN_POLICY &&
                     strcmp(rows[j].name, kSfnPolicyNames[i]) == 0);
        }
        UT_ASSERT_MSG(found, "the policy '%s' has no row in the catalogue",
                      kSfnPolicyNames[i]);
    }

    return 0;
}

/* ── 2. The fields a trigger may test ─────────────────────────────── */

/* Room for any row's fields. The widest the catalogue holds is can_build's
   five parameters and the two they earn, so this is well clear of it and
   leaves the truncation checks below room to ask for less. */
#define SFN_FIELD_MAX 32

/* One field as it is expected back: what it is called, what may be in it,
   and whether the accessor built the name or copied it. */
typedef struct {
    const char     *name;
    ScnLuaParamType type;
    bool            derived;
} SfnWantField;

static size_t sfnRowByName(const ScnLuaFnRow *rows, size_t count,
                           const char *name) {
    size_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(rows[i].name, name) == 0) {
            return i;
        }
    }
    return count;
}

/* The accessor's answer for one named row, against a list written out by
   hand. Answers 0 where they agree, as a case does. */
static int sfnFieldsAre(const char *name, const SfnWantField *want,
                        size_t wantCount) {
    const ScnLuaFnRow *rows;
    ScnLuaFnField      got[SFN_FIELD_MAX];
    size_t             count = 0;
    size_t             row;
    size_t             n;
    size_t             i;

    rows = scenarioLuaFunctions(&count);
    row  = sfnRowByName(rows, count, name);
    UT_ASSERT_MSG(row < count, "the catalogue has no row named '%s'", name);

    n = scenarioLuaFnFields(row, got, SFN_FIELD_MAX);
    UT_ASSERT_MSG(n == wantCount, "'%s' answers %d fields; expected %d",
                  name, (int)n, (int)wantCount);
    for (i = 0; i < wantCount; i++) {
        UT_ASSERT_MSG(strcmp(got[i].name, want[i].name) == 0,
                      "'%s' field %d is '%s'; expected '%s'", name, (int)i,
                      got[i].name, want[i].name);
        UT_ASSERT_MSG(got[i].type == want[i].type,
                      "'%s' field '%s' is type %d; expected %d", name,
                      want[i].name, (int)got[i].type, (int)want[i].type);
        UT_ASSERT_MSG(got[i].derived == want[i].derived,
                      "'%s' field '%s' says derived %d; expected %d", name,
                      want[i].name, (int)got[i].derived,
                      (int)want[i].derived);
    }
    return 0;
}

int run_scenario_functions_fields(void) {
    /* The four rows written out in full: one with two slot-shaped
       parameters, one with an item and two owners, one with a square pair,
       and one carrying both a square pair and an owner, which is where the
       order of the derived half shows. Between them they reach every rule
       there is. */
    static const SfnWantField kTankKilled[] = {
        { "victim",      SCN_PARAM_SLOT,  false },
        { "killer",      SCN_PARAM_OWNER, false },
        { "cause",       SCN_PARAM_WORD,  false },
        { "victim_team", SCN_PARAM_TEAM,  true  },
        { "killer_team", SCN_PARAM_TEAM,  true  },
    };
    static const SfnWantField kBaseCaptured[] = {
        { "n",         SCN_PARAM_BASE,  false },
        { "old",       SCN_PARAM_OWNER, false },
        { "new",       SCN_PARAM_OWNER, false },
        { "tag",       SCN_PARAM_TAG,   true  },
        { "old_team",  SCN_PARAM_TEAM,  true  },
        { "new_team",  SCN_PARAM_TEAM,  true  },
    };
    static const SfnWantField kPing[] = {
        { "p",      SCN_PARAM_SLOT,     false },
        { "kind",   SCN_PARAM_NUMBER,   false },
        { "mx",     SCN_PARAM_SQUARE_X, false },
        { "my",     SCN_PARAM_SQUARE_Y, false },
        { "p_team", SCN_PARAM_TEAM,     true  },
        { "region", SCN_PARAM_REGION,   true  },
    };
    static const SfnWantField kMineExplosion[] = {
        { "mx",         SCN_PARAM_SQUARE_X, false },
        { "my",         SCN_PARAM_SQUARE_Y, false },
        { "layer",      SCN_PARAM_OWNER,    false },
        { "region",     SCN_PARAM_REGION,   true  },
        { "layer_team", SCN_PARAM_TEAM,     true  },
    };

    const ScnLuaFnRow *rows;
    ScnLuaFnField      got[SFN_FIELD_MAX];
    size_t             count = 0;
    size_t             i;
    size_t             j;
    size_t             n;

    rows = scenarioLuaFunctions(&count);
    UT_ASSERT_MSG(rows != NULL && count > 0, "the catalogue answered nothing");

    for (i = 0; i < count; i++) {
        const ScnLuaFnRow *r = &rows[i];

        n = scenarioLuaFnFields(i, got, SFN_FIELD_MAX);
        UT_ASSERT_MSG(n >= r->paramCount,
                      "'%s' takes %d parameters and answers %d fields",
                      r->name, (int)r->paramCount, (int)n);
        UT_ASSERT_MSG(n <= SFN_FIELD_MAX,
                      "'%s' answers %d fields, more than this case holds",
                      r->name, (int)n);

        /* The parameters themselves come first, in the order the function
           takes them, so a form drawing the row's own arguments can stop at
           paramCount. */
        for (j = 0; j < r->paramCount; j++) {
            UT_ASSERT_MSG(strcmp(got[j].name, r->params[j].name) == 0,
                          "'%s' field %d is '%s'; the parameter there is "
                          "'%s'", r->name, (int)j, got[j].name,
                          r->params[j].name);
            UT_ASSERT_MSG(got[j].type == r->params[j].type,
                          "'%s' field '%s' is type %d; the parameter is %d",
                          r->name, got[j].name, (int)got[j].type,
                          (int)r->params[j].type);
            UT_ASSERT_MSG(!got[j].derived,
                          "'%s' calls its own parameter '%s' derived",
                          r->name, got[j].name);
            UT_ASSERT_MSG(got[j].from == j,
                          "'%s' parameter '%s' reads parameter %d", r->name,
                          got[j].name, (int)got[j].from);
        }

        for (j = 0; j < n; j++) {
            size_t k;

            UT_ASSERT_MSG(got[j].name[0] != '\0',
                          "'%s' answers an empty field name at %d", r->name,
                          (int)j);
            UT_ASSERT_MSG(got[j].type != SCN_PARAM_NONE,
                          "'%s' field '%s' has no type", r->name,
                          got[j].name);

            /* A where-row names one field, so two of them answering to one
               name would leave one untestable. */
            for (k = 0; k < j; k++) {
                UT_ASSERT_MSG(strcmp(got[k].name, got[j].name) != 0,
                              "'%s' answers '%s' twice, at %d and %d",
                              r->name, got[j].name, (int)k, (int)j);
            }

            if (!got[j].derived) {
                continue;
            }
            UT_ASSERT_MSG(got[j].from < r->paramCount,
                          "'%s' field '%s' reads parameter %d of %d",
                          r->name, got[j].name, (int)got[j].from,
                          (int)r->paramCount);

            /* Each derived field against the parameter it says it came
               from: the type it reads has to be one the rules derive it
               from. */
            switch (got[j].type) {
                case SCN_PARAM_TEAM:
                    UT_ASSERT_MSG(
                        r->params[got[j].from].type == SCN_PARAM_SLOT ||
                            r->params[got[j].from].type == SCN_PARAM_OWNER,
                        "'%s' derives the team '%s' from '%s', which is "
                        "neither a slot nor an owner", r->name, got[j].name,
                        r->params[got[j].from].name);
                    break;

                case SCN_PARAM_TAG:
                    UT_ASSERT_MSG(
                        r->params[got[j].from].type == SCN_PARAM_PILL ||
                            r->params[got[j].from].type == SCN_PARAM_BASE,
                        "'%s' derives a tag from '%s', which is neither a "
                        "pillbox nor a base", r->name,
                        r->params[got[j].from].name);
                    break;

                case SCN_PARAM_REGION:
                    UT_ASSERT_MSG(
                        r->params[got[j].from].type == SCN_PARAM_SQUARE_X &&
                            got[j].from + 1 < r->paramCount &&
                            r->params[got[j].from + 1].type ==
                                SCN_PARAM_SQUARE_Y,
                        "'%s' derives a region from '%s', which is not the "
                        "x of a square pair", r->name,
                        r->params[got[j].from].name);
                    break;

                default:
                    UT_ASSERT_MSG(false,
                                  "'%s' derives '%s' as type %d, which no "
                                  "rule derives", r->name, got[j].name,
                                  (int)got[j].type);
                    break;
            }
        }
    }

    if (sfnFieldsAre("on_tank_killed", kTankKilled,
                     sizeof(kTankKilled) / sizeof(kTankKilled[0])) != 0) {
        return 1;
    }
    if (sfnFieldsAre("on_base_captured", kBaseCaptured,
                     sizeof(kBaseCaptured) / sizeof(kBaseCaptured[0])) != 0) {
        return 1;
    }
    if (sfnFieldsAre("on_ping", kPing,
                     sizeof(kPing) / sizeof(kPing[0])) != 0) {
        return 1;
    }
    if (sfnFieldsAre("on_mine_explosion", kMineExplosion,
                     sizeof(kMineExplosion) /
                         sizeof(kMineExplosion[0])) != 0) {
        return 1;
    }

    /* Asked for less than there is: the answer is still the whole count, and
       nothing past the room given is touched. A caller sizes an array off
       the first answer and asks again. */
    {
        size_t row  = sfnRowByName(rows, count, "on_ping");
        size_t full = 0;

        UT_ASSERT_MSG(row < count, "the catalogue has no row named 'on_ping'");
        full = scenarioLuaFnFields(row, NULL, 0);
        UT_ASSERT_MSG(full == sizeof(kPing) / sizeof(kPing[0]),
                      "'on_ping' answers %d fields for a NULL out; expected "
                      "%d", (int)full,
                      (int)(sizeof(kPing) / sizeof(kPing[0])));

        memset(got, 0, sizeof(got));
        n = scenarioLuaFnFields(row, got, 2);
        UT_ASSERT_MSG(n == full,
                      "'on_ping' answers %d fields when asked for two; "
                      "expected %d", (int)n, (int)full);
        UT_ASSERT_MSG(strcmp(got[0].name, kPing[0].name) == 0 &&
                          strcmp(got[1].name, kPing[1].name) == 0,
                      "the two fields written are '%s' and '%s'", got[0].name,
                      got[1].name);
        for (i = 2; i < SFN_FIELD_MAX; i++) {
            UT_ASSERT_MSG(got[i].name[0] == '\0',
                          "a field was written past the room given, at %d: "
                          "'%s'", (int)i, got[i].name);
        }
    }

    /* A row that is not there has no fields. */
    memset(got, 0, sizeof(got));
    n = scenarioLuaFnFields(count, got, SFN_FIELD_MAX);
    UT_ASSERT_MSG(n == 0, "the row past the last answered %d fields", (int)n);
    UT_ASSERT_MSG(got[0].name[0] == '\0',
                  "the row past the last wrote '%s'", got[0].name);

    return 0;
}

/* ── Fixtures for the dispatch case ───────────────────────────────── */

static void sfnScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

/* A handler says what it was handed by printing a marked line, the same way
 * the hook cases next door do: a scenario state has no io, and its print
 * goes to the server console. */
#define SFN_NOTE_MARK "note:"

/* How long to wait on something the round has to reach by itself. */
#define SFN_TICK_LIMIT 600

static char   sfnNote[8192];
static size_t sfnNoteLen;

static void sfnReset(void) {
    sfnNote[0] = '\0';
    sfnNoteLen = 0;
}

/* One console line, kept if a handler wrote it, with the newline the
   record's readers count whole lines by. */
static void sfnNoteLine(const char *msg) {
    size_t mark = strlen(SFN_NOTE_MARK);
    size_t room;
    size_t n;

    if (msg == NULL || strncmp(msg, SFN_NOTE_MARK, mark) != 0 ||
        sfnNoteLen + 2 > sizeof(sfnNote)) {
        return;
    }
    n    = strlen(msg + mark);
    room = sizeof(sfnNote) - 2 - sfnNoteLen;
    if (n > room) {
        n = room;
    }
    memcpy(sfnNote + sfnNoteLen, msg + mark, n);
    sfnNoteLen += n;
    sfnNote[sfnNoteLen++] = '\n';
    sfnNote[sfnNoteLen]   = '\0';
}

static void (*sfnConsolePrev)(void *ctx, char *msg) = NULL;

static void sfnConsoleCb(void *ctx, char *msg) {
    if (sfnConsolePrev != NULL) {
        sfnConsolePrev(ctx, msg);
    }
    sfnNoteLine(msg);
}

static void sfnWatchConsole(ServerSim *sim) {
    sfnConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = sfnConsoleCb;
}

static void sfnUnwatchConsole(ServerSim *sim) {
    sim->sim.callbacks.consoleMessage = sfnConsolePrev;
    sfnConsolePrev = NULL;
}

static bool sfnPut(const char *mapPath, const char *lua) {
    char  path[512];
    FILE *f;

    sfnScriptFor(mapPath, path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

static void sfnDrop(const char *mapPath) {
    char path[512];
    sfnScriptFor(mapPath, path, sizeof(path));
    remove(path);
}

static ServerSim *sfnSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    sfnWatchConsole(sim);
    sfnReset();
    return sim;
}

static void sfnRead(char *out, size_t outLen) {
    snprintf(out, outLen, "%s", sfnNote);
}

/* How many whole lines the record has that begin with what was asked for.
 * Anchored at the start of a line, so a name that ends with another name
 * does not count that other one's lines as its own. */
static int sfnLines(const char *text, const char *line) {
    const char *p     = text;
    size_t      n     = strlen(line);
    int         count = 0;

    if (n == 0) {
        return 0;
    }
    while ((p = strstr(p, line)) != NULL) {
        if (p == text || p[-1] == '\n') {
            count++;
        }
        p += n;
    }
    return count;
}

static void sfnRaise(ServerSim *sim, uint8_t type, const uint8_t *data,
                     size_t len) {
    GameEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    if (len > sizeof(ev.data)) {
        len = sizeof(ev.data);
    }
    memcpy(ev.data, data, len);
    serverSimAddEvent(sim, &ev);
}

static void sfnPublishSlot(ServerSim *sim, BYTE slot, BYTE team) {
    ControlEvent evt;

    memset(&evt, 0, sizeof(evt));
    evt.type                        = CTRL_LOBBY_SLOT;
    evt.u.lobbySlot.playerNum       = slot;
    evt.u.lobbySlot.slot.connected  = true;
    evt.u.lobbySlot.slot.teamNumber = team;
    serverSimPublishControl(sim, &evt);
}

/* ── The script, written from the catalogue itself ────────────────── */

/* A handler per hook row, each recording how many arguments it was handed
 * and nothing else. Written from the rows rather than by hand, so a row
 * whose name is not the name the host resolves defines a function nothing
 * ever calls and its line never arrives.
 *
 * The record is capped at one line per name. Several of these run on every
 * tick of a round that lasts hundreds of them, and the case cares only that
 * each was handed what its row claims. */
static bool sfnBody(char *out, size_t outLen) {
    const ScnLuaFnRow *rows;
    size_t             count = 0;
    size_t             i;
    size_t             used;
    int                wrote;

    wrote = snprintf(out, outLen,
                     "local seen = {}\n"
                     "local function argc(name, n)\n"
                     "  if not seen[name] then\n"
                     "    seen[name] = true\n"
                     "    print(\"" SFN_NOTE_MARK "argc \"..name..\" \"..n)\n"
                     "  end\n"
                     "end\n");
    if (wrote < 0 || (size_t)wrote >= outLen) {
        return false;
    }
    used = (size_t)wrote;

    rows = scenarioLuaFunctions(&count);
    for (i = 0; i < count; i++) {
        if (rows[i].kind == SCN_FN_POLICY) {
            continue;
        }
        wrote = snprintf(out + used, outLen - used,
                         "function %s(...) argc(\"%s\", select(\"#\", ...))"
                         " end\n", rows[i].name, rows[i].name);
        if (wrote < 0 || (size_t)wrote >= outLen - used) {
            return false;
        }
        used += (size_t)wrote;
    }
    return true;
}

/* What a handler for this row must have been handed: its own parameters,
 * and the trailing scripted flag where the kind carries one. */
static size_t sfnWantArgs(const ScnLuaFnRow *r) {
    return r->paramCount + (r->kind == SCN_FN_EVENT_HOOK ? 1u : 0u);
}

/* ── 3. Every hook the harness can fire ───────────────────────────── */

/* One event per hook that comes off one, with the payload the emitting
 * callback writes. The values are immaterial here — what is being read is
 * how many arguments arrived — but two of the bytes are not: a death cause
 * and a build action the surface has no word for are facts the host runs no
 * hook for at all, so both are named ones. */
typedef struct {
    uint8_t type;
    uint8_t data[8];
    size_t  len;
} SfnGameCase;

static const SfnGameCase kSfnGame[] = {
    /* [killer, killed, cause, carriedPills, trees, mapX, mapY] */
    { EVENT_TANK_KILLED,   { 5, 2, LAST_DEATH_BY_SHELL, 1, 0, 40, 41 }, 7 },
    /* [player, mx, my, respawn] */
    { EVENT_TANK_SPAWNED,  { 3, 44, 45, 1 }, 4 },
    /* [sender, kind, xHi, xLo, yHi, yLo] — the two positions are world
       units and the hook is handed the map square behind them. */
    { EVENT_PING,          { 2, 1, 20, 0, 21, 0 }, 6 },
    /* [victim, killer, quiet, mx, my] */
    { EVENT_LGM_LOST,      { 6, 7, 0, 48, 49 }, 5 },
    /* [player, mx, my] */
    { EVENT_LGM_LANDED,    { 8, 50, 51 }, 3 },
    /* [new, old, index, quiet, class, mapX, mapY] — a new owner that is
       somebody is a capture, and nobody is the other hook. */
    { EVENT_BASE_CAPTURED, { 9, 10, 3, 0, CAPTURE_CLASS_ENEMY, 52, 53 }, 7 },
    { EVENT_BASE_CAPTURED, { NEUTRAL, 11, 4, 0, CAPTURE_CLASS_NEUTRAL, 54,
                             55 }, 7 },
    { EVENT_PILL_CAPTURED, { 12, 13, 5, 0, CAPTURE_CLASS_ENEMY, 56, 57 }, 7 },
    /* [player, index, mx, my, armour] */
    { EVENT_PILL_PLACED,   { 14, 6, 58, 59, 15 }, 5 },
    /* [player, index] */
    { EVENT_PILL_PICKED_UP,{ 1, 7 }, 2 },
    /* [index, attacker] */
    { EVENT_PILL_KILLED,   { 8, 4 }, 2 },
    /* [player, action, mx, my] */
    { EVENT_BUILT,         { 0, (uint8_t)builderJobRoad, 60, 61 }, 4 },
    /* [player, mx, my] */
    { EVENT_MINE_PLACED,   { 15, 64, 65 }, 3 },
    /* [mx, my, layer] */
    { EVENT_MINE_EXPLODED, { 66, 67, 9 }, 3 },
};

/* The scenario table both rounds below are booted from. The region covers
 * the whole map, so a tank that has a place anywhere is inside it: the
 * enter hook is then the tank arriving and the leave hook is the seat going
 * away, which needs no square to be picked and no tank to be driven. */
static const char *const kSfnTable =
    "scenario = { name = \"Functions\", api = 1,\n"
    "  regions = { all = { x = 0, y = 0, w = 255, h = 255 } } }\n";

/* Everything the round produces on its own or is handed: the round's own
 * moments, a tank arriving and the seat leaving again, one event per hook
 * that comes off one, and the four control facts. */
static int sfnRunRound(char *rec, size_t recLen) {
    static const char *const kMap = "scnfn_dispatch.map";
    ServerSim    *sim;
    ScenarioHost *h;
    ControlEvent  evt;
    char          lua[16384];
    char          body[8192];
    char          err[512];
    size_t        i;
    int           t;

    UT_ASSERT_MSG(sfnBody(body, sizeof(body)),
                  "the catalogue did not fit in a script");
    snprintf(lua, sizeof(lua), "%s%s", kSfnTable, body);

    sfnReset();
    UT_ASSERT(sfnPut(kMap, lua));
    sim = sfnSim();
    UT_ASSERT(sim != NULL);

    /* Attached before the start, because a round's hooks are resolved at
       the round start and on_setup is made inside it. */
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Human", false);

    /* Until the seat has a tank, which is what the region scan reads. */
    for (t = 0; t < SFN_TICK_LIMIT; t++) {
        TankInfo info;
        if (serverSimGetTankInfo(sim, 0, &info) && info.has_tank) {
            break;
        }
        serverSimTick(sim);
    }
    /* One more, so the scan that follows the arrival has run. */
    serverSimTick(sim);

    for (i = 0; i < sizeof(kSfnGame) / sizeof(kSfnGame[0]); i++) {
        sfnRaise(sim, kSfnGame[i].type, kSfnGame[i].data, kSfnGame[i].len);
    }

    memset(&evt, 0, sizeof(evt));
    evt.type                   = CTRL_PLAYER_JOIN;
    evt.u.playerJoin.playerNum = 11;
    serverSimPublishControl(sim, &evt);

    memset(&evt, 0, sizeof(evt));
    evt.type                    = CTRL_PLAYER_LEAVE;
    evt.u.playerLeave.playerNum = 12;
    serverSimPublishControl(sim, &evt);

    /* One slot event is both hooks it can be: the seat changed, and the
       team differs from the one the round opened with. */
    sfnPublishSlot(sim, 13, 6);

    memset(&evt, 0, sizeof(evt));
    evt.type              = CTRL_CHAT;
    evt.u.chat.fromPlayer = 4;
    evt.u.chat.destPlayer = 0xFF;
    evt.u.chat.bodyLen    = 5;
    memcpy(evt.u.chat.body, "hello", 5);
    serverSimPublishControl(sim, &evt);

    serverSimTick(sim);

    /* The seat goes, which takes its tank out of the world and so out of
       every region it was in. */
    serverSimRemovePlayer(sim, 0);
    serverSimTick(sim);
    serverSimTick(sim);

    sfnRead(rec, recLen);

    scenarioHostDetach(h);
    sfnUnwatchConsole(sim);
    serverSimDestroy(sim);
    sfnDrop(kMap);
    sfnReset();
    return 0;
}

/* The round's end, which no event raises and which the round above runs too
 * long to reach. A second round with the same handlers, ended from its own
 * third tick the way the hook cases end one.
 *
 * on_tick is written over afterwards, so this round records nothing for it;
 * the round above is where that row is read. */
static int sfnRunEnd(char *rec, size_t recLen) {
    static const char *const kMap = "scnfn_end.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          lua[16384];
    char          body[8192];
    char          err[512];
    int           t;

    UT_ASSERT_MSG(sfnBody(body, sizeof(body)),
                  "the catalogue did not fit in a script");
    snprintf(lua, sizeof(lua),
             "%s%s"
             "local n = 0\n"
             "function on_tick(t)\n"
             "  n = n + 1\n"
             "  if n == 3 then game.end_round(\"done\") end\n"
             "end\n", kSfnTable, body);

    sfnReset();
    UT_ASSERT(sfnPut(kMap, lua));
    sim = sfnSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Human", false);

    for (t = 0; t < SFN_TICK_LIMIT; t++) {
        if (serverSimGetState(sim) == serverStateGameOver) {
            break;
        }
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "the round never ended, so on_end was never made");

    sfnRead(rec, recLen);

    scenarioHostDetach(h);
    sfnUnwatchConsole(sim);
    serverSimDestroy(sim);
    sfnDrop(kMap);
    sfnReset();
    return 0;
}

int run_scenario_functions_match_dispatch(void) {
    const ScnLuaFnRow *rows;
    char               round[8192];
    char               ended[8192];
    char               rec[16384];
    char               want[128];
    size_t             count = 0;
    size_t             i;
    int                rc;

    rc = sfnRunRound(round, sizeof(round));
    if (rc != 0) {
        return rc;
    }
    rc = sfnRunEnd(ended, sizeof(ended));
    if (rc != 0) {
        return rc;
    }
    snprintf(rec, sizeof(rec), "%s%s", round, ended);

    /* Every hook is reached by one of the two rounds, so a row with no line
       of its own is a name the host never resolved — which is what a
       catalogue naming a function that does not exist looks like. The count
       on the line is what the handler was handed, and the row is what says
       what it should have been. */
    rows = scenarioLuaFunctions(&count);
    for (i = 0; i < count; i++) {
        const ScnLuaFnRow *r = &rows[i];

        if (r->kind == SCN_FN_POLICY) {
            continue;   /* a question, asked at its own site, not from here */
        }
        snprintf(want, sizeof(want), "argc %s %d\n", r->name,
                 (int)sfnWantArgs(r));
        UT_ASSERT_MSG(sfnLines(rec, want) >= 1,
                      "'%s' claims %d parameters%s, so its handler should "
                      "have been handed %d arguments; the record was:\n%s",
                      r->name, (int)r->paramCount,
                      (r->kind == SCN_FN_EVENT_HOOK)
                          ? " and the trailing scripted flag" : "",
                      (int)sfnWantArgs(r), rec);
    }

    return 0;
}
