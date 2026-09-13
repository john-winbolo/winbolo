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
 *  The round's shape: a fresh VM at every round start, so a
 *  fresh set of globals; the rules applied; on_setup called
 *  inside the setup window the start opens; the roster
 *  audited; and on_start called on the first running tick.
 *  Both hooks are optional — most scenarios define neither.
 *
 *  A script that keeps raising is switched off for the round
 *  at SCN_ERROR_LIMIT errors in a row, and so is one whose
 *  setup leaves the round short a human. Either way the sim
 *  answers classic for the rest of the round and the players
 *  are told once.
 *
 *  Every entry into the Lua state takes the host's own lock,
 *  which knows the thread that holds it: the lobby asks a
 *  policy from the GUI thread while ticks run on the timer
 *  thread, and a Lua state is not re-entrant across threads.
 *
 *  The game table a script reads the sim through is built in
 *  scenario_lua.c and goes on every state this file boots,
 *  before the chunk runs. Those rows read and never write:
 *  what the sim sees from here is one SCN_OP_SET_RULE per
 *  rule, one SCN_OP_MSG_ALL for a line the players are owed,
 *  and the answer to the one policy registered below.
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

#include <limits.h>
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

/* SDL_Mutex, SDL_ThreadID and SDL_GetCurrentThreadID, which the VM lock is
 * built from. server_sim.h brings SDL in as well; named here because this
 * file uses it directly. */
#include <SDL3/SDL.h>

#include "platform_types.h"        /* BOLO_STATIC_ASSERT */
#include "bolo_rand.h"             /* BoloRandState, bolo_rand_save */
#include "server_sim.h"            /* ServerSim, serverSimConsoleMessage */
#include "scenario_defs.h"         /* SCN_RULE_LIST, ScenarioOp, ScnOpResult */
#include "server_sim_scenario.h"   /* the funnel and the round-start hook-up */

#include "scenario_host.h"
#include "scenario_manifest.h"
#include "scenario_events.h"
#include "scenario_lua.h"

/* One operator line. Long enough for a Lua error, which carries the file
 * path and the line number ahead of its message. */
#define SCN_ERR_LEN 512

/* The manifest is sized in scenario_host.h, which cannot see the rule
 * count; this is where the two are visible together. */
BOLO_STATIC_ASSERT(SCN_MANIFEST_RULES_MAX >= (int)SCN_RULE_COUNT,
                   manifest_holds_every_rule_at_once);

/* A team's brain is the same text a spawn carries, and the manifest sizes
 * it without seeing the op. Held against the op's own length here. */
BOLO_STATIC_ASSERT(SCN_BRAIN_LEN == SCN_PATH_MAX,
                   manifest_brain_matches_the_op_path_length);

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

/* ── The VM lock ──────────────────────────────────────────────────── */

/* One Lua state, two threads that can arrive at it. When the desktop client
 * hosts, the lobby runs on the GUI thread and the sim ticks on an SDL timer
 * thread, so a policy the lobby asks and a hook a tick runs can be in flight
 * at once; a Lua state is not re-entrant across threads and the prototype
 * crashed on a Ready click with both inside one VM.
 *
 * A different thread waits. The thread that already holds it goes straight
 * through and the depth rises, because a policy asked from inside a hook is
 * the same thread arriving again and a plain mutex would stop dead there.
 *
 * The same owner-and-depth shape src/server/threads.c gives the server's own
 * lock, built here from SDL rather than borrowed: src/scenario/ sees
 * src/bolo/public/ and src/bolo/scenario_api/ and nothing under src/server/,
 * that lock is a process-wide singleton the server binary brings up and the
 * unit tests and the headless runner never do, and sharing one depth count
 * between the sim's lock and this one would let a release on either side
 * drop the other.
 *
 * Reading owner outside the lock is safe for the same reason it is there:
 * only the owning thread writes its own id, and it writes while holding the
 * mutex. */
typedef struct {
    SDL_Mutex   *m;
    SDL_ThreadID owner;
    unsigned     depth;
} ScnVmLock;

static bool scnLockCreate(ScnVmLock *l) {
    l->owner = 0;
    l->depth = 0;
    l->m     = SDL_CreateMutex();
    return l->m != NULL;
}

static void scnLockDestroy(ScnVmLock *l) {
    if (l->m != NULL) {
        SDL_DestroyMutex(l->m);
        l->m = NULL;
    }
    l->owner = 0;
    l->depth = 0;
}

static void scnLockEnter(ScnVmLock *l) {
    SDL_ThreadID me = SDL_GetCurrentThreadID();
    if (l->owner == me) {
        l->depth++;
        return;
    }
    SDL_LockMutex(l->m);
    l->owner = me;
    l->depth = 1;
}

static void scnLockLeave(ScnVmLock *l) {
    if (l->depth > 1) {
        l->depth--;
        return;
    }
    l->owner = 0;
    l->depth = 0;
    SDL_UnlockMutex(l->m);
}

/* ── The host ─────────────────────────────────────────────────────── */

struct ScenarioHost {
    ServerSim       *sim;
    lua_State       *L;       /* the live VM: the metadata one until a round
                               * starts, then that round's own */
    ScenarioManifest manifest;

    /* What the game table's rows read, handed to every VM this host boots.
     * It points at the manifest above rather than carrying a copy, so the
     * table a round start reads in is the one the next call answers from. */
    ScnLuaCtx        lua;

    char             sidecar[SCN_SIDECAR_PATH_MAX];
    char            *src;     /* the sidecar's bytes, read once at attach */
    size_t           srcLen;
    char             chunkName[SCN_SIDECAR_PATH_MAX + 2];
    char             lastError[SCN_ERR_LEN];
    bool             active;

    /* Taken across every entry into L, and across the line below, which
     * whichever thread hit the limit wrote and the tick thread says. */
    ScnVmLock        lock;

    /* The round's lifecycle functions, looked up once at the boot that
     * defines them. LUA_NOREF for a script that defines neither, which is
     * most of them. */
    int              onSetupRef;
    int              onStartRef;

    /* Hook and policy errors in a row, and what happens at
     * SCN_ERROR_LIMIT of them. Both are the round's, not the
     * attachment's: a round start puts them back. */
    unsigned         errors;
    bool             disabled;

    /* on_start is owed to the first running tick of the round the start
     * set this on. */
    bool             startPending;

    /* One line for the players, waiting for a tick to say it. */
    char             pending[SCN_TEXT_MAX];
    bool             hasPending;

    /* The decisions the sim asks. One entry filled; the sim holds the
     * pointer and does not own it, so it lives here for as long as the
     * registration does. */
    ScenarioPolicy   policy;

    /* What this round has raised on either channel, waiting for the tick
     * that drains it, and the subscriber slot both channels arrive
     * through. */
    ScnEventQueue    events;
    SubscriberHandle sub;
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

/* A state with the script's own surface on it, ready for a chunk.
 *
 * The game table goes on before the chunk runs, and every state this host
 * boots gets one: a script may read the game at the top level rather than
 * from a hook, and a sidecar that does must load the same way at an attach,
 * at a round start and at a reload's check.
 *
 * What the rows read through is the host's own struct, so a chunk that reads
 * one of the manifest rows reads the table the host is holding as it runs:
 * nothing at all at the attach, where the first table is what this chunk is
 * about to declare, and the last round's at a round start, which is the same
 * table again because every round runs the bytes read at attach. */
static lua_State *scnBootVm(ScenarioHost *h) {
    lua_State *L = scnNewVm();

    if (L != NULL) {
        scenarioLuaInstall(L, &h->lua);
    }
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
            int         idx = scenarioLuaRuleIndex(key);
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
                   scenarioLuaRuleName((int)h->manifest.rules[i].rule),
                   scnResultText(r));
        }
    }
}

/* ── Switching a scenario off for the round ───────────────────────── */

/* What a line to the players calls the scenario: its own name where the
 * table gave one, the plain words where it did not. */
static const char *scnSubject(const ScenarioHost *h) {
    return (h->manifest.name[0] != '\0') ? h->manifest.name : "The scenario";
}

/* Stop running this scenario's Lua for the rest of the round and hold one
 * line for the players saying so. The round carries on with the classic
 * answers; the next round start boots a fresh VM and begins again.
 *
 * The line waits for a tick rather than going out from here, and either of
 * two reasons on its own would be enough. The funnel refuses every op while
 * a policy call is on the stack, with SCN_OP_IN_POLICY, and a policy call
 * is where the error limit is reached most often — issued from here the
 * line would simply be turned down. And a policy the lobby asks runs on the
 * GUI thread, where the control bus is not this tree's to publish on: the
 * bus and its subscribers are the tick thread's. The tick callback is past
 * both. */
static void scnDisable(ScenarioHost *h, const char *fmt, ...) {
    va_list ap;

    if (h->disabled) {
        return;                  /* the round is already without it */
    }
    h->disabled = true;

    va_start(ap, fmt);
    vsnprintf(h->pending, sizeof(h->pending), fmt, ap);
    va_end(ap);
    h->hasPending = true;

    scnSay(h->lastError, sizeof(h->lastError), "scenario: %s", h->pending);
}

/* n more errors, for a caller that has already said its piece. Apart from a
 * hook or a policy raising, the other thing that counts is an event the
 * queue had no room for, and a tick that dropped many of those wants one
 * line rather than one per event. */
static void scnErrorsCounted(ScenarioHost *h, unsigned n) {
    if (n == 0) {
        return;
    }
    /* A count that cannot wrap: past the limit the only thing left to do is
       switch the scenario off, which the next line does. */
    if (h->errors > UINT_MAX - n) {
        h->errors = UINT_MAX;
    } else {
        h->errors += n;
    }
    if (h->errors >= SCN_ERROR_LIMIT) {
        scnDisable(h, "%.32s is off for the rest of the round: %d errors in "
                      "a row.", scnSubject(h), SCN_ERROR_LIMIT);
    }
}

/* One more call that raised. SCN_ERROR_LIMIT of them in a row and the
 * scenario is off. The operator sees every one on the way there, because
 * which calls they were is the whole of what a script author has to work
 * from until the bindings land. */
static void scnErrorRaised(ScenarioHost *h, const char *what,
                           const char *msg) {
    scnSay(h->lastError, sizeof(h->lastError), "scenario: %s raised: %s",
           what, msg);
    scnErrorsCounted(h, 1);
}

/* A call that returned. The count is of errors in a row, so one success
 * clears whatever came before it. */
static void scnErrorCleared(ScenarioHost *h) {
    h->errors = 0;
}

/* ── The lifecycle hooks ──────────────────────────────────────────── */

/* Hold on to a no-argument global at the boot that defines it, rather than
 * looking the name up per call. Absent is not an error: most scenarios
 * define neither hook. */
static int scnHookRef(lua_State *L, const char *name) {
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

/* Run one of them. A scenario that is off runs none, an absent one is not a
 * call at all and leaves the error count where it was, and one that raises
 * carries the count toward the limit.
 *
 * The caller holds the VM lock. */
static void scnHookRun(ScenarioHost *h, int ref, const char *name) {
    if (h->disabled || h->L == NULL || ref == LUA_NOREF) {
        return;
    }
    lua_rawgeti(h->L, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(h->L, 0, 0, 0) != 0) {
        scnErrorRaised(h, name, scnLuaError(h->L));
        lua_pop(h->L, 1);
        return;
    }
    scnErrorCleared(h);
}

/* ── The roster audit ─────────────────────────────────────────────── */

/* Which seats hold a human, so the check after a hook has something to hold
 * the roster against. */
static void scnRosterHumans(ScenarioHost *h, bool *out) {
    BYTE i;

    for (i = 0; i < MAX_TANKS; i++) {
        ServerSimRosterSlot slot;
        out[i] = serverSimGetRosterSlot(h->sim, i, &slot) && !slot.is_bot;
    }
}

/* Every human who was in the round is still in it, and every seat that is
 * taken sits on a team the lobby knows. A scenario that breaks either is off
 * for the round: the ops already refuse to remove a human, so anything that
 * gets around them has left the round a player short, and a connection that
 * has gone cannot be handed back.
 *
 * A team the lobby knows is one ServerSim.teams[] is keyed by. Zero is in
 * that range and is not a fault: it is the seat with no team, which the
 * lobby lets a player sit on and the round's alliance pass skips. */
static void scnRosterAudit(ScenarioHost *h, const bool *before) {
    BYTE i;

    for (i = 0; i < MAX_TANKS; i++) {
        ServerSimRosterSlot slot;
        bool                here = serverSimGetRosterSlot(h->sim, i, &slot);

        if (before[i] && (!here || slot.is_bot)) {
            scnDisable(h, "%.32s is off for the rest of the round: it dropped "
                          "player %d.", scnSubject(h), (int)i);
            return;
        }
        if (here && slot.team >= MAX_TANKS) {
            scnDisable(h, "%.32s is off for the rest of the round: player %d "
                          "is on team %d.", scnSubject(h), (int)i,
                          (int)slot.team);
            return;
        }
    }
}

/* ── The per-tick callback ────────────────────────────────────────── */

/* Say the line a switched-off scenario owes the players. Held until here
 * because the point it was written at could not issue an op; this one can.
 * A refusal keeps it for the next tick rather than dropping it, since what
 * turned it down is a state this tick is in and the next may not be. */
static void scnSayPending(ScenarioHost *h) {
    ScenarioOp op;

    if (!h->hasPending) {
        return;
    }
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MSG_ALL;
    snprintf(op.u.msgAll.text, sizeof(op.u.msgAll.text), "%s", h->pending);
    if (serverSimApplyScenarioOp(h->sim, &op, NULL) == SCN_OP_OK) {
        h->hasPending = false;
    }
}

/* ── The bus events ───────────────────────────────────────────────── */

/* The host listens to both of the server's channels and does the same thing
 * with each: copy into the one queue and return.
 *
 * This one runs inside serverSimPublishControl's deliver loop, which holds
 * the publishing flag across it and asserts that nothing publishes from
 * within. The other runs inside the mutation that raised a game event, with
 * the sim's state half-written and its fan-out asserting the same. Neither
 * calls Lua, issues an op or publishes, and everything that acts on an
 * event happens at the drain.
 *
 * One queue for the two, so what the drain hands out is the order the facts
 * happened in whichever channel carried each of them. */
static void scnDeliverControl(void *ctx, const struct ControlEvent *evt) {
    ScenarioHost *h = (ScenarioHost *)ctx;

    if (h == NULL) {
        return;
    }
    scenarioEventsQueueControl(&h->events, evt);
}

static void scnDeliverEvent(void *ctx, const GameEvent *evt) {
    ScenarioHost *h = (ScenarioHost *)ctx;

    if (h == NULL) {
        return;
    }
    scenarioEventsQueueGame(&h->events, evt);
}

/* What the drain does with one entry. Turning it into the script's hook for
 * that event is the binding table's work — including reading the channel to
 * tell which of the two numberings the type belongs to. Until then an entry
 * is read off the queue in order and the numbers on the queue are what it
 * leaves behind. */
static void scnEventConsume(void *ctx, const ScnQueuedEvent *e) {
    (void)ctx;
    (void)e;
}

/* Say what the queue had no room for, once, and count each of them. The
 * drop itself happens inside a publish where there is nothing safe to say;
 * this runs at the tick, after the drain, so events the drain's own work
 * pushed past the end are reported in the tick they were lost in. */
static void scnReportDrops(ScenarioHost *h) {
    uint32_t dropped = scenarioEventsTakeDropped(&h->events);

    if (dropped == 0) {
        return;
    }
    scnSay(h->lastError, sizeof(h->lastError),
           "scenario: %lu events arrived with the queue full at %d and were "
           "dropped", (unsigned long)dropped, SCN_EVENT_QUEUE_MAX);
    scnErrorsCounted(h, (unsigned)dropped);
}

/* ── The per-tick callback ────────────────────────────────────────── */

/* The callback serverSimTick makes at the end of every frame, in both the
 * running and the non-running branch.
 *
 * In order: the on_start call the round start left for the first running
 * tick, because a round's start comes before anything the round has
 * produced — the state tells the two branches apart and the flag tells the
 * first running tick from the ones after it; then the events that have
 * arrived since the last tick; then what the queue had no room for, after
 * the drain so a drop the drain caused is reported in its own tick; then
 * the line the round owes the players, last, so a line any of the three
 * above raised goes out in this tick rather than the next. */
static void scnTick(void *ctx) {
    ScenarioHost *h = (ScenarioHost *)ctx;

    if (h == NULL) {
        return;
    }
    scnLockEnter(&h->lock);
    if (h->startPending &&
        serverSimGetState(h->sim) == serverStateRunning) {
        h->startPending = false;
        scnHookRun(h, h->onStartRef, "on_start");
    }
    (void)scenarioEventsDrain(&h->events, scnEventConsume, h);
    scnReportDrops(h);
    scnSayPending(h);
    scnLockLeave(&h->lock);
}

/* ── The policy ───────────────────────────────────────────────────── */

/* May a slot be put on a team that no other slot is on? The lobby asks this
 * from the GUI thread while the sim ticks on another, which is the reason
 * the lock below exists at all.
 *
 * Classic is yes, and an absent function, a nil answer and an error all
 * come to it. An error counts toward the limit; the other two are not
 * failures and leave the count alone. */
static bool scnAllowExtraTeams(void *ctx) {
    ScenarioHost *h     = (ScenarioHost *)ctx;
    bool          allow = true;

    if (h == NULL) {
        return true;
    }
    scnLockEnter(&h->lock);
    if (!h->disabled && h->L != NULL) {
        lua_getglobal(h->L, "allow_extra_teams");
        if (lua_isfunction(h->L, -1)) {
            if (lua_pcall(h->L, 0, 1, 0) != 0) {
                scnErrorRaised(h, "allow_extra_teams", scnLuaError(h->L));
                lua_pop(h->L, 1);
            } else {
                if (!lua_isnil(h->L, -1)) {
                    allow = lua_toboolean(h->L, -1) != 0;
                }
                lua_pop(h->L, 1);
                scnErrorCleared(h);
            }
        } else {
            lua_pop(h->L, 1);
        }
    }
    scnLockLeave(&h->lock);
    return allow;
}

/* A round the scenario takes no part in: no hooks, no policy answers, and
 * nothing owed to the first running tick. The operator has already been
 * told why. */
static void scnRoundWithoutScenario(ScenarioHost *h) {
    h->disabled     = true;
    h->startPending = false;
    h->onSetupRef   = LUA_NOREF;
    h->onStartRef   = LUA_NOREF;
}

/* The round start's work, with the VM lock already held.
 *
 * A fresh VM for the round, the bytes read at attach run again in it, the
 * table read again, then the rules, then on_setup, then the audit. The
 * fresh VM is what gives the round a fresh set of globals: nothing the last
 * round's script left behind is reachable from this one.
 *
 * Nothing here goes near the disk: this runs inside the start with the sim
 * mutex held. A round whose chunk fails says so and plays classic rather
 * than carrying the previous round's table into it.
 *
 * on_setup runs after the rules, so a script's setup sees the table its own
 * sidecar asked for, and inside the setup window the start holds open, so
 * the funnel takes the ops it issues — every one but the six roster ops,
 * which the window keeps refusing. */
static void scnRoundStartLocked(ScenarioHost *h) {
    lua_State       *L;
    ScenarioManifest fresh;
    char             err[SCN_ERR_LEN];
    bool             humans[MAX_TANKS];

    err[0] = '\0';

    L = scnBootVm(h);
    if (L == NULL) {
        scnSay(h->lastError, sizeof(h->lastError),
               "scenario: no memory for this round's Lua state");
        scnRoundWithoutScenario(h);
        return;
    }
    if (!scnRunChunk(L, h->src, h->srcLen, h->chunkName, err, sizeof(err)) ||
        !scnReadManifest(L, &fresh, h->sidecar, err, sizeof(err),
                         h->lastError, sizeof(h->lastError))) {
        scnSay(h->lastError, sizeof(h->lastError), "%s", err);
        lua_close(L);
        scnRoundWithoutScenario(h);
        return;
    }
    if (fresh.api > SCENARIO_API_VERSION) {
        scnSay(h->lastError, sizeof(h->lastError),
               "scenario: %s asks for api %d and this server is api %d",
               h->sidecar, fresh.api, SCENARIO_API_VERSION);
        lua_close(L);
        scnRoundWithoutScenario(h);
        return;
    }

    if (h->L != NULL) {
        lua_close(h->L);
    }
    h->L        = L;
    h->manifest = fresh;
    h->onSetupRef = scnHookRef(L, "on_setup");
    h->onStartRef = scnHookRef(L, "on_start");

    /* Off for a round is off for that round alone. This one starts with the
       count at zero and the scenario running, whatever the last one did. */
    h->errors       = 0;
    h->disabled     = false;
    h->startPending = true;

    /* And with an empty queue, on both channels: what the last round raised
       is no business of this one. Before the rules and the setup call, so
       anything those do raise is this round's and reaches its first
       drain. */
    scenarioEventsReset(&h->events);

    scnApplyRules(h);

    scnRosterHumans(h, humans);
    scnHookRun(h, h->onSetupRef, "on_setup");
    scnRosterAudit(h, humans);
}

/* The sim's round-start callback. */
static void scnRoundStart(void *ctx) {
    ScenarioHost *h = (ScenarioHost *)ctx;

    if (h == NULL) {
        return;
    }
    scnLockEnter(&h->lock);
    scnRoundStartLocked(h);
    scnLockLeave(&h->lock);
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

    /* The host is built before the state is, because the game table's rows
       read through a struct on it and the table goes on before the chunk
       runs. Nothing can reach either of them yet: the registrations at the
       bottom are what make this host reachable, and a path out of here
       before them takes the whole thing back down again. */
    h = (ScenarioHost *)calloc(1, sizeof(*h));
    if (h == NULL) {
        scnFmt(err, errLen, "scenario: out of memory");
        free(src);
        return NULL;
    }
    if (!scnLockCreate(&h->lock)) {
        scnFmt(err, errLen, "scenario: no mutex for the Lua state");
        free(h);
        free(src);
        return NULL;
    }
    h->sim          = sim;
    h->lua.sim      = sim;
    h->lua.manifest = &h->manifest;
    /* The lifecycle hooks belong to a round's VM, and this is the one the
       table is read in. The first round start resolves them. */
    h->onSetupRef = LUA_NOREF;
    h->onStartRef = LUA_NOREF;
    /* Neither of these is the zero calloc left: no subscriber is -1. */
    h->sub        = SUBSCRIBER_HANDLE_INVALID;
    snprintf(h->sidecar, sizeof(h->sidecar), "%s", sidecar);
    snprintf(h->chunkName, sizeof(h->chunkName), "%s", chunkName);

    /* The one VM entry that takes no lock, because there is nothing yet to
       take one on: until the host below is registered no other thread can
       reach either. */
    L = scnBootVm(h);
    if (L == NULL) {
        scnFmt(err, errLen, "scenario: no memory for a Lua state");
        scnLockDestroy(&h->lock);
        free(h);
        free(src);
        return NULL;
    }

    soft[0] = '\0';
    if (!scnRunChunk(L, src, srcLen, chunkName, err, errLen) ||
        !scnReadManifest(L, &m, sidecar, err, errLen, soft, sizeof(soft))) {
        lua_close(L);
        scnLockDestroy(&h->lock);
        free(h);
        free(src);
        return NULL;
    }
    if (m.api > SCENARIO_API_VERSION) {
        scnFmt(err, errLen,
               "scenario: %s asks for api %d and this server is api %d — "
               "the server is too old to run it",
               sidecar, m.api, SCENARIO_API_VERSION);
        lua_close(L);
        scnLockDestroy(&h->lock);
        free(h);
        free(src);
        return NULL;
    }

    h->L        = L;
    h->manifest = m;
    h->src      = src;
    h->srcLen   = srcLen;
    h->active   = true;
    snprintf(h->lastError, sizeof(h->lastError), "%s", soft);

    /* One entry filled and the rest left NULL, which is what the sim reads
       as the classic rule. The lobby asks this one before a round starts,
       so it is registered here rather than at the first start. */
    h->policy.allowExtraTeams = scnAllowExtraTeams;
    h->policy.ctx             = h;

    serverSimSetScenarioRoundStart(sim, scnRoundStart, h);
    serverSimSetScenarioTick(sim, scnTick, h);
    serverSimSetScenarioPolicy(sim, &h->policy);

    /* The bus, in three steps and in this order. Registration hands the new
       subscriber the whole of the current server state through the control
       callback before it returns, and that is a picture of how things
       stand rather than a run of facts that happened, so the queue is
       emptied straight after it and the host starts from the next thing the
       server actually does. Then the second channel, which has been NULL
       for all of the above and so has added nothing of its own.

       An invalid handle is not worth refusing the map for — the rules, the
       hooks and the policy all still work — so it is said once and the
       scenario runs without events. */
    h->sub = serverSimRegisterSubscriber(sim, scnDeliverControl, h);
    scenarioEventsReset(&h->events);
    if (h->sub == SUBSCRIBER_HANDLE_INVALID ||
        !serverSimSetSubscriberEventDeliver(sim, h->sub, scnDeliverEvent)) {
        scnSay(h->lastError, sizeof(h->lastError),
               "scenario: no subscriber slot for %s, so it sees no events",
               sidecar);
    }
    return h;
}

bool scenarioHostReload(ScenarioHost *h, char *err, size_t errLen) {
    char             soft[SCN_ERR_LEN];
    ScenarioManifest m;
    lua_State       *L;
    char            *src    = NULL;
    size_t           srcLen = 0;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (h == NULL) {
        return false;
    }

    if (!scnReadFile(h->sidecar, &src, &srcLen, err, errLen)) {
        /* A file that has gone leaves err empty, because at attach that is
           the ordinary case rather than a fault. Asked for by name it is a
           fault, so it is stated here. */
        if (err != NULL && errLen > 0 && err[0] == '\0') {
            scnFmt(err, errLen, "scenario: %s is no longer there", h->sidecar);
        }
        return false;
    }

    /* Everything below happens in a Lua state of its own, and nothing the
       host holds is touched until all of it has passed. A file with an
       error in it therefore changes nothing at all: the round that is
       running keeps its table, and so does the round after it.

       The state carries the game table like any other, so a sidecar that
       reads the game at the top level loads here exactly as it loads at a
       round start rather than being turned down for a table this one
       state lacked. */
    L = scnBootVm(h);
    if (L == NULL) {
        scnFmt(err, errLen, "scenario: no memory for a Lua state");
        free(src);
        return false;
    }

    soft[0] = '\0';
    if (!scnRunChunk(L, src, srcLen, h->chunkName, err, errLen) ||
        !scnReadManifest(L, &m, h->sidecar, err, errLen, soft, sizeof(soft))) {
        lua_close(L);
        free(src);
        return false;
    }
    if (m.api > SCENARIO_API_VERSION) {
        scnFmt(err, errLen,
               "scenario: %s asks for api %d and this server is api %d — "
               "the server is too old to run it",
               h->sidecar, m.api, SCENARIO_API_VERSION);
        lua_close(L);
        free(src);
        return false;
    }
    lua_close(L);

    /* The bytes are replaced and nothing else is. The VM and the table the
       round is running on stay as they are; the next round start builds
       both again from what is now here.

       Under the lock, because the round start reads these bytes under it and
       a reload arrives on whichever thread the operator's command came in
       on. Only the swap: the reading and the checking above happen on a Lua
       state of their own, and holding the lock across a disk read would stop
       a tick for as long as the file took. */
    scnLockEnter(&h->lock);
    free(h->src);
    h->src    = src;
    h->srcLen = srcLen;
    snprintf(h->lastError, sizeof(h->lastError), "%s", soft);
    scnLockLeave(&h->lock);
    return true;
}

void scenarioHostDetach(ScenarioHost *h) {
    if (h == NULL) {
        return;
    }
    /* Every registration the host made, taken back before the memory it
       points at goes. The policy struct is a member of what free() is about
       to release, so leaving it registered would hand the sim a dangling
       vtable the first time the lobby asked a question. */
    if (h->sim != NULL) {
        serverSimSetScenarioRoundStart(h->sim, NULL, NULL);
        serverSimSetScenarioTick(h->sim, NULL, NULL);
        serverSimSetScenarioPolicy(h->sim, NULL);
        /* Both channels go with the slot, and an invalid handle is a
           no-op, so this needs no test of its own. */
        serverSimUnregisterSubscriber(h->sim, h->sub);
    }
    /* Outside the lock, and it has to be: the registrations are gone, so
       nothing can arrive at the VM any more, and a lock taken here would be
       destroyed on the next line with whoever was waiting on it still
       waiting. A detach while another thread is inside the VM is a lifetime
       question rather than a locking one — the caller detaches from the
       thread that owns the sim. */
    if (h->L != NULL) {
        lua_close(h->L);
    }
    scnLockDestroy(&h->lock);
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

const ScnEventQueue *scenarioHostEventQueue(const ScenarioHost *h) {
    return (h != NULL) ? &h->events : NULL;
}
