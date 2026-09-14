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
 *  audited; on_start called on the first running tick;
 *  on_tick on every running one; and on_end where the round
 *  moves into game over. Every hook is optional — a scenario
 *  defines the few it cares about.
 *
 *  Between those, the events. Each entry the drain hands back
 *  becomes the script's hook for that fact, with the payload
 *  the engine published and a trailing boolean saying whether
 *  the scenario itself caused it, so a handler that ignores
 *  its own edits is one line. The four lifecycle calls take
 *  no such boolean: there is no event behind them.
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
#include "scenario_validate.h"     /* ScnParseReport, and the parse this file
                                    * shares with the validator */

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

/* The region scan keeps one bit per region in a 64-bit word per seat. A
 * sixty-fifth region would be a region no tank could ever be recorded in. */
BOLO_STATIC_ASSERT(SCN_REGIONS_MAX <= 64,
                   region_membership_fits_one_word_per_seat);

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

/* Say one line about the table being read. The operator and the soft buffer
 * hear it as they always have; a validator collecting problems gets it as an
 * issue under key, which is the dotted path of the thing at fault.
 *
 * The three readers below go through here rather than through scnSay, because
 * what they complain about is the file's own content and that is exactly what
 * an author asking for a check wants listed. */
static void scnReport(ScnParseReport *rep, const char *key,
                      const char *fmt, ...) {
    char    line[SCN_ERR_LEN];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    serverSimConsoleMessage(line);
    if (rep == NULL) {
        return;
    }
    if (rep->soft != NULL && rep->softLen > 0) {
        snprintf(rep->soft, rep->softLen, "%s", line);
    }
    if (rep->sink != NULL) {
        scnIssueAdd(rep->sink, key, "%s", line);
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

/* ── The hooks ────────────────────────────────────────────────────── */

/* Every function a round may call, in one list: the four lifecycle calls
 * and one per event the host turns into a hook. The name a script writes it
 * under is here and nowhere else, so the name the boot resolves and the name
 * a case defines cannot drift apart.
 *
 * The four at the top are not events. They are made where the round reaches
 * the point each of them names, and they take no trailing scripted boolean
 * because there is no event behind them to have caused.
 *
 * Nor do the last two, and for the same reason read the other way round.
 * Every hook between them comes off the queue, where the entry carries the
 * mark saying whether the scenario caused the fact. The two region hooks
 * come off no queue at all: the host samples where the tanks are once a
 * tick and compares that against the sample before it, so what it has is a
 * difference between two pictures and not an action anybody took. A
 * boolean there could only be a guess about which of the things that moved
 * a tank since the last sample mattered, and a script reading it would be
 * reading a guess. See scnScanRegions for what bounds the loop instead. */
#define SCN_HOOK_LIST(X)                                                     \
    X(SETUP,            "on_setup")                                          \
    X(START,            "on_start")                                          \
    X(TICK,             "on_tick")                                           \
    X(END,              "on_end")                                            \
    X(LOBBY,            "on_lobby")                                          \
    X(PLAYER_JOIN,      "on_player_join")                                    \
    X(PLAYER_LEAVE,     "on_player_leave")                                   \
    X(TEAM_CHANGED,     "on_team_changed")                                   \
    X(CHAT,             "on_chat")                                           \
    X(TANK_SPAWNED,     "on_tank_spawned")                                   \
    X(TANK_KILLED,      "on_tank_killed")                                    \
    X(LGM_DIED,         "on_lgm_died")                                       \
    X(LGM_LANDED,       "on_lgm_landed")                                     \
    X(BASE_CAPTURED,    "on_base_captured")                                  \
    X(BASE_NEUTRALIZED, "on_base_neutralized")                               \
    X(PILL_CAPTURED,    "on_pill_captured")                                  \
    X(PILL_PLACED,      "on_pill_placed")                                    \
    X(PILL_PICKED_UP,   "on_pill_picked_up")                                 \
    X(PILL_KILLED,      "on_pill_killed")                                    \
    X(BUILT,            "on_built")                                          \
    X(MINE_LAID,        "on_mine_laid")                                      \
    X(MINE_EXPLOSION,   "on_mine_explosion")                                 \
    X(ENTER_REGION,     "on_enter_region")                                   \
    X(LEAVE_REGION,     "on_leave_region")

typedef enum {
#define SCN_HOOK_ID_ROW(id, name) SCN_HOOK_##id,
    SCN_HOOK_LIST(SCN_HOOK_ID_ROW)
#undef SCN_HOOK_ID_ROW
    SCN_HOOK_COUNT
} ScnHookId;

static const char *const kScnHookNames[] = {
#define SCN_HOOK_NAME_ROW(id, name) name,
    SCN_HOOK_LIST(SCN_HOOK_NAME_ROW)
#undef SCN_HOOK_NAME_ROW
};

BOLO_STATIC_ASSERT(
    (int)(sizeof(kScnHookNames) / sizeof(kScnHookNames[0])) ==
        (int)SCN_HOOK_COUNT,
    hook_name_table_is_the_whole_hook_list);

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

    /* The round's functions, looked up once at the boot that defines them.
     * LUA_NOREF for every name the script does not use, which for most
     * scripts is most of them. */
    int              hooks[SCN_HOOK_COUNT];

    /* Hook and policy errors in a row, and what happens at
     * SCN_ERROR_LIMIT of them. Both are the round's, not the
     * attachment's: a round start puts them back. */
    unsigned         errors;
    bool             disabled;

    /* on_start is owed to the first running tick of the round the start
     * set this on. */
    bool             startPending;

    /* What the round was doing the last time a tick looked at it. on_end is
     * the move from anything else into game over, and this is the half of
     * that comparison the sim does not hold: the state is read at the end
     * of the tick the round ended in, whether the frame ended it or a hook
     * did from the drain a few lines earlier. */
    ServerState      lastState;

    /* What team each seat was on the last time the host looked. The lobby
     * slot event carries the team a seat is on and nothing about the team
     * it was on, so the change on_team_changed reports is the difference
     * between the event and this. Filled from the roster at the attach and
     * again at every round start, so a round opens agreeing with the world
     * rather than reporting a change for every seat that is on a team at
     * all. */
    uint8_t          teams[MAX_TANKS];

    /* Which regions each seat's tank was standing in at the last scan, one
     * bit per region index. The enter and leave hooks are the difference
     * between this and the next sample, so it is the whole of what the scan
     * remembers; a round start clears it, along with the regions themselves.
     *
     * A bit is an index into the manifest's region list. Nothing removes a
     * region inside a round and a defined one replaces by name rather than
     * appending a second, so an index means the same rectangle for as long
     * as the bits do. */
    uint64_t         inRegion[MAX_TANKS];

    /* The timers this round is holding, and the functions they hold with
     * them. Handed to every VM through h->lua, and emptied against a state
     * before that state is closed. */
    ScnTimerSet      timers;

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
bool scnSidecarPath(const char *mapPath, char *out, size_t outLen) {
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
bool scnReadFile(const char *path, char **out, size_t *outLen,
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

    /* The two reads in this file that still run a metamethod, and the pair
       for which that costs nothing: this runs inside the boot, ahead of the
       chunk, so math holds what luaL_openlibs put there a line ago and no
       script has had a statement in which to touch it. */
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

/* Where an error with no lua_pcall between it and the VM ends up. Lua calls
 * abort() if this returns, so it does not return.
 *
 * Every call this file makes into a script is a lua_pcall and every read of
 * the script's own table is a raw one, so what is left to arrive here is
 * what neither covers: a state out of memory, or an error raised by a C
 * call made outside a protected one. Neither leaves a round to carry on
 * with.
 *
 * Unwinding back to a caller was the other shape and is not what this does.
 * The entries into the state are the sim's round-start callback with the
 * sim mutex held, the tick, and a policy the lobby asks from another
 * thread; a jump out of any of them leaves that mutex and the VM lock held
 * and the round half applied, and the process is wedged rather than gone.
 * This ends it instead, and says why first: today the operator is told
 * nothing at all. The abort leaves a core behind to read it from. */
static int scnPanic(lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    char        line[SCN_ERR_LEN];

    snprintf(line, sizeof(line), "scenario: Lua panic: %s",
             (msg != NULL) ? msg : "unknown error");
    /* Both, because the two hosts watch different ones: the dedicated server
       and the headless runner have stderr, and the desktop client's console
       is where the rest of this file's lines go. */
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
    serverSimConsoleMessage(line);
    /* Last statement on purpose. abort does not return, and a return after
       it is unreachable code, which this tree builds as an error. */
    abort();
}

lua_State *scnNewVm(void) {
    lua_State *L = luaL_newstate();
    if (L == NULL) {
        return NULL;
    }
    lua_atpanic(L, scnPanic);
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
static lua_State *scnBootVmWith(const ScnLuaCtx *ctx) {
    lua_State *L = scnNewVm();

    if (L != NULL) {
        scenarioLuaInstall(L, ctx);
    }
    return L;
}

static lua_State *scnBootVm(ScenarioHost *h) {
    return scnBootVmWith(&h->lua);
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
bool scnRunChunk(lua_State *L, const char *src, size_t srcLen,
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

/* The reads below want the value the file wrote and nothing else.
 *
 * lua_getfield runs __index, so a table carrying a metatable turns what the
 * readers treat as a lookup into a call into script code: it runs with the
 * game table in scope and the ops open, in the middle of a read that is not
 * protected, and one that raises from there reaches scnPanic. A script
 * needs no ill intent to have a metatable on it. These push the key and
 * rawget instead, which asks the table only.
 *
 * A negative table index is biased by one for the key pushed ahead of it. A
 * pseudo-index is absolute and left alone. */
static void scnRawField(lua_State *L, int tbl, const char *key) {
    if (tbl < 0 && tbl > LUA_REGISTRYINDEX) {
        tbl -= 1;
    }
    lua_pushstring(L, key);
    lua_rawget(L, tbl);
}

/* The globals table itself, for the same reason and with the same care: a
 * script that puts a metatable on _G would otherwise have one run on every
 * global the host reads. Where the table lives is the one place the two Lua
 * builds differ — 5.1 and LuaJIT keep a pseudo-index for it, 5.4 keeps it
 * in the registry — and WINBOLO_LUAJIT is what the tree tells them apart
 * by, the same define the force-included shim is written against. */
static void scnPushGlobals(lua_State *L) {
#ifdef WINBOLO_LUAJIT
    lua_pushvalue(L, LUA_GLOBALSINDEX);
#else
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
#endif
}

/* One global, left on the stack where lua_getglobal would have left it. */
static void scnRawGlobal(lua_State *L, const char *name) {
    scnPushGlobals(L);
    lua_pushstring(L, name);
    lua_rawget(L, -2);
    lua_remove(L, -2);           /* the globals table, under the value */
}

static void scnReadStr(lua_State *L, int tbl, const char *key,
                       char *out, size_t outLen) {
    scnRawField(L, tbl, key);
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
    scnRawField(L, tbl, key);
    if (lua_type(L, -1) == LUA_TNUMBER) {
        v = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    return v;
}

static bool scnReadBool(lua_State *L, int tbl, const char *key, bool def) {
    bool v = def;
    scnRawField(L, tbl, key);
    if (lua_isboolean(L, -1)) {
        v = lua_toboolean(L, -1) ? true : false;
    }
    lua_pop(L, 1);
    return v;
}

static void scnReadLobby(lua_State *L, int tbl, ScnManifestLobby *lob) {
    int lt;

    scnRawField(L, tbl, "lobby");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lt = lua_gettop(L);
    lob->maxPlayers = (uint8_t)scnReadInt(L, lt, "max_players", 0);
    lob->extraTeams = scnReadBool(L, lt, "extra_teams", false);

    scnRawField(L, lt, "teams");
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
                         ScnParseReport *rep) {
    int rules;

    scnRawField(L, tbl, "rules");
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
                char where[SCN_VALIDATE_KEY_LEN];
                snprintf(where, sizeof(where), "rules.%s", key);
                scnReport(rep, where, "scenario: no rule is named '%s'", key);
            } else if (m->numRules >= SCN_MANIFEST_RULES_MAX) {
                scnReport(rep, "rules",
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
                      int entity, const char *where, ScnParseReport *rep) {
    size_t n;

    if (out->count >= SCN_TAGS_PER_ENTITY) {
        scnReport(rep, where,
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
                           const char *kind, ScnParseReport *rep) {
    int kt;

    scnRawField(L, tags, key);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    kt = lua_gettop(L);

    lua_pushnil(L);
    while (lua_next(L, kt) != 0) {
        if (lua_type(L, -2) == LUA_TNUMBER) {
            int  e = (int)lua_tointeger(L, -2);
            char where[SCN_VALIDATE_KEY_LEN];
            snprintf(where, sizeof(where), "tags.%s[%d]", key, e);
            if (e < 1 || e > maxEntity) {
                scnReport(rep, where,
                          "scenario: %s %d is not an index this map can hold",
                          kind, e);
            } else if (lua_type(L, -1) == LUA_TSTRING) {
                scnAddTag(&arr[e], lua_tostring(L, -1), kind, e, where, rep);
            } else if (lua_istable(L, -1)) {
                int list = lua_gettop(L);
                int n    = (int)lua_rawlen(L, list);
                int i;
                for (i = 1; i <= n; i++) {
                    lua_rawgeti(L, list, i);
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        scnAddTag(&arr[e], lua_tostring(L, -1), kind, e,
                                  where, rep);
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
                        ScnParseReport *rep) {
    int tags;

    scnRawField(L, tbl, "tags");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    tags = lua_gettop(L);
    scnReadTagKind(L, tags, "pills",  m->pillTags,  MAX_PILLS,  "pill",  rep);
    scnReadTagKind(L, tags, "bases",  m->baseTags,  MAX_BASES,  "base",  rep);
    scnReadTagKind(L, tags, "starts", m->startTags, MAX_STARTS, "start", rep);
    lua_pop(L, 1);
}

/* Regions are keyed by name, so they arrive in whatever order the table
 * iterates in — which differs between the two Lua builds. A reader looks a
 * region up by name rather than by position. */
static void scnReadRegions(lua_State *L, int tbl, ScenarioManifest *m,
                           ScnParseReport *rep) {
    int rt;

    scnRawField(L, tbl, "regions");
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
                char where[SCN_VALIDATE_KEY_LEN];
                snprintf(where, sizeof(where), "regions.%s", name);
                scnReport(rep, where,
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
bool scnReadManifest(lua_State *L, ScenarioManifest *m,
                     const char *path, char *err, size_t errLen,
                     ScnParseReport *rep) {
    int tbl;

    memset(m, 0, sizeof(*m));

    scnRawGlobal(L, "scenario");
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
    scnReadRules(L, tbl, m, rep);
    scnReadTags(L, tbl, m, rep);
    scnReadRegions(L, tbl, m, rep);

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

/* ── The hooks ────────────────────────────────────────────────────── */

/* Hold on to one function at the boot that defines it, rather than looking
 * the name up per call. A script may write it as a global or as a field of
 * its own scenario table, and the global wins where a script does both.
 * Absent is not an error and is the ordinary case: a scenario defines the
 * few hooks it cares about and none of the rest. */
static int scnHookRef(lua_State *L, const char *name) {
    scnRawGlobal(L, name);
    if (lua_isfunction(L, -1)) {
        return luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_pop(L, 1);

    scnRawGlobal(L, "scenario");
    if (lua_istable(L, -1)) {
        scnRawField(L, -1, name);
        if (lua_isfunction(L, -1)) {
            int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            lua_pop(L, 1);          /* the scenario table */
            return ref;
        }
        lua_pop(L, 1);              /* the field, which is not a function */
    }
    lua_pop(L, 1);                  /* scenario, or whatever the global held */
    return LUA_NOREF;
}

/* Resolve every name at once, at the boot of the state that defines them. */
static void scnHooksResolve(ScenarioHost *h, lua_State *L) {
    int i;

    for (i = 0; i < (int)SCN_HOOK_COUNT; i++) {
        h->hooks[i] = scnHookRef(L, kScnHookNames[i]);
    }
}

static void scnHooksForget(ScenarioHost *h) {
    int i;

    for (i = 0; i < (int)SCN_HOOK_COUNT; i++) {
        h->hooks[i] = LUA_NOREF;
    }
}

/* Push one hook, ready for its arguments. False when there is nothing to
 * call: a scenario switched off for the round runs none of them, and a name
 * the script never defined is not a call at all and leaves the error count
 * where it was.
 *
 * The caller holds the VM lock, and must reach scnHookCall for every true
 * this answers — the function it pushed is on the stack until then. */
static bool scnHookBegin(ScenarioHost *h, ScnHookId id) {
    if (h->disabled || h->L == NULL || h->hooks[id] == LUA_NOREF) {
        return false;
    }
    lua_rawgeti(h->L, LUA_REGISTRYINDEX, h->hooks[id]);
    return true;
}

/* Make the call, with the nargs the caller has pushed since. One that
 * raises carries the error count toward the limit and one that returns puts
 * it back to zero. */
static void scnHookCall(ScenarioHost *h, ScnHookId id, int nargs) {
    if (lua_pcall(h->L, nargs, 0, 0) != 0) {
        scnErrorRaised(h, kScnHookNames[id], scnLuaError(h->L));
        lua_pop(h->L, 1);
        return;
    }
    scnErrorCleared(h);
}

/* One that takes nothing: the lifecycle calls with no payload. */
static void scnHookRun(ScenarioHost *h, ScnHookId id) {
    if (!scnHookBegin(h, id)) {
        return;
    }
    scnHookCall(h, id, 0);
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

/* Who caused what is being queued. Neither channel carries it, so it is
 * read off the sim here, which is the only moment it can be: both callbacks
 * run from inside the work that produced the fact — the op handler, or the
 * spawn the drain is making — and by the time a hook runs for the entry
 * that work is a tick behind and the sim says nothing about it. */
static uint8_t scnActorNow(const ScenarioHost *h) {
    return serverSimIsScenarioActing(h->sim) ? SCN_EVENT_ACTOR_SCRIPT
                                             : SCN_EVENT_ACTOR_NONE;
}

/* The host listens to both of the server's channels and does the same thing
 * with each: read the mark, copy into the one queue and return.
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
    scenarioEventsQueueControl(&h->events, evt, scnActorNow(h));
}

static void scnDeliverEvent(void *ctx, const GameEvent *evt) {
    ScenarioHost *h = (ScenarioHost *)ctx;

    if (h == NULL) {
        return;
    }
    scenarioEventsQueueGame(&h->events, evt, scnActorNow(h));
}

/* ── One entry, one hook ──────────────────────────────────────────── */

/* A game event's eight bytes are kept whole, so an index into data is the
 * offset the emitting callback wrote at — including the bytes past
 * gameEventDataSize, which never reach a client and are where the mine's
 * layer and the builder's death square sit. A control event's is the front
 * of its variant, which is why it is read back through the variant below
 * rather than at an offset of this file's own.
 *
 * Item indices on an event are 0-based; every one of them goes into Lua
 * through the one helper that adds. Player slots are 0-based on both sides
 * and go through nothing. */

/* The event the entry was copied from, as the struct that named its fields.
 * Only the first SCN_EVENT_DATA_MAX bytes of the variant were kept, which
 * scenario_events.c holds against the two variants read deepest into; the
 * rest is left zero. */
static void scnControlEventOf(const ScnQueuedEvent *e, ControlEvent *out) {
    memset(out, 0, sizeof(*out));
    out->type = (ControlEventType)e->type;
    memcpy(&out->u, e->data, SCN_EVENT_DATA_MAX);
}

/* One tank's death. The wire order is [killer, killed, cause], and the hook
 * takes the victim first: it is the hook's subject, and the killer is what
 * happened to it. */
static void scnHookTankKilled(ScenarioHost *h, const ScnQueuedEvent *e,
                              bool scripted) {
    const char *cause = scenarioLuaDeathCauseWord((int)e->data[2]);

    if (cause == NULL || !scnHookBegin(h, SCN_HOOK_TANK_KILLED)) {
        return;
    }
    lua_pushinteger(h->L, (lua_Integer)e->data[1]);   /* the victim */
    lua_pushinteger(h->L, (lua_Integer)e->data[0]);   /* the killer */
    lua_pushstring(h->L, cause);
    lua_pushboolean(h->L, scripted ? 1 : 0);
    scnHookCall(h, SCN_HOOK_TANK_KILLED, 4);
}

/* A base changed hands. One event carries both facts and the new owner is
 * what tells them apart: a capture names who took it, a neutralisation has
 * nobody to name and says who lost it. */
static void scnHookBaseOwner(ScenarioHost *h, const ScnQueuedEvent *e,
                             bool scripted) {
    lua_Integer index = scenarioLuaIndexToScript((int)e->data[2]);

    if (e->data[0] == NEUTRAL) {
        if (!scnHookBegin(h, SCN_HOOK_BASE_NEUTRALIZED)) {
            return;
        }
        lua_pushinteger(h->L, index);
        lua_pushinteger(h->L, (lua_Integer)e->data[1]);   /* the old owner */
        lua_pushboolean(h->L, scripted ? 1 : 0);
        scnHookCall(h, SCN_HOOK_BASE_NEUTRALIZED, 3);
        return;
    }
    if (!scnHookBegin(h, SCN_HOOK_BASE_CAPTURED)) {
        return;
    }
    lua_pushinteger(h->L, index);
    lua_pushinteger(h->L, (lua_Integer)e->data[1]);       /* the old owner */
    lua_pushinteger(h->L, (lua_Integer)e->data[0]);       /* the new owner */
    lua_pushboolean(h->L, scripted ? 1 : 0);
    scnHookCall(h, SCN_HOOK_BASE_CAPTURED, 4);
}

/* A build that finished. The action byte is the builder's own request code
 * and a number that names none is a fact the host cannot describe, so it
 * runs no hook for it rather than handing a script a word of its own. */
static void scnHookBuilt(ScenarioHost *h, const ScnQueuedEvent *e,
                         bool scripted) {
    const char *action = scenarioLuaBuiltActionWord((int)e->data[1]);

    if (action == NULL || !scnHookBegin(h, SCN_HOOK_BUILT)) {
        return;
    }
    lua_pushinteger(h->L, (lua_Integer)e->data[0]);
    lua_pushstring(h->L, action);
    lua_pushinteger(h->L, (lua_Integer)e->data[2]);
    lua_pushinteger(h->L, (lua_Integer)e->data[3]);
    lua_pushboolean(h->L, scripted ? 1 : 0);
    scnHookCall(h, SCN_HOOK_BUILT, 5);
}

/* One game event, as the hook written for it. Each case states the payload
 * the emitting callback wrote, in its own order, above the arguments the
 * hook takes — the two are not always the same order, and where they differ
 * is where an off-by-one would otherwise hide. An event no hook is written
 * for falls out of the bottom. */
static void scnGameEventHook(ScenarioHost *h, const ScnQueuedEvent *e,
                             bool scripted) {
    switch (e->type) {
        case EVENT_TANK_KILLED:
            scnHookTankKilled(h, e, scripted);
            return;

        case EVENT_TANK_SPAWNED:
            /* [player, mx, my, respawn] — the last byte is a flag and
               reaches Lua as one. */
            if (!scnHookBegin(h, SCN_HOOK_TANK_SPAWNED)) return;
            lua_pushinteger(h->L, (lua_Integer)e->data[0]);
            lua_pushinteger(h->L, (lua_Integer)e->data[1]);
            lua_pushinteger(h->L, (lua_Integer)e->data[2]);
            lua_pushboolean(h->L, e->data[3] != 0);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_TANK_SPAWNED, 5);
            return;

        case EVENT_LGM_LOST:
            /* [victim, killer, quiet, mx, my]. The quiet byte is the
               announce policy's answer to the players and is no business of
               a hook; the square behind it is where the man died, which the
               emit captures before the respawn overwrites his position. */
            if (!scnHookBegin(h, SCN_HOOK_LGM_DIED)) return;
            lua_pushinteger(h->L, (lua_Integer)e->data[0]);
            lua_pushinteger(h->L, (lua_Integer)e->data[1]);
            lua_pushinteger(h->L, (lua_Integer)e->data[3]);
            lua_pushinteger(h->L, (lua_Integer)e->data[4]);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_LGM_DIED, 5);
            return;

        case EVENT_LGM_LANDED:
            /* [player, mx, my] */
            if (!scnHookBegin(h, SCN_HOOK_LGM_LANDED)) return;
            lua_pushinteger(h->L, (lua_Integer)e->data[0]);
            lua_pushinteger(h->L, (lua_Integer)e->data[1]);
            lua_pushinteger(h->L, (lua_Integer)e->data[2]);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_LGM_LANDED, 4);
            return;

        case EVENT_BASE_CAPTURED:
            scnHookBaseOwner(h, e, scripted);
            return;

        case EVENT_PILL_CAPTURED:
            /* [new, old, index, quiet]. A pillbox has no hook of its own
               for being neutralised, so this one carries every change of
               owner, with game.NEUTRAL as the new one where nobody took
               it. */
            if (!scnHookBegin(h, SCN_HOOK_PILL_CAPTURED)) return;
            lua_pushinteger(h->L, scenarioLuaIndexToScript((int)e->data[2]));
            lua_pushinteger(h->L, (lua_Integer)e->data[1]);
            lua_pushinteger(h->L, (lua_Integer)e->data[0]);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_PILL_CAPTURED, 4);
            return;

        case EVENT_PILL_PLACED:
            /* [player, index, mx, my], and the hook names the pillbox
               first: the two are the other way round from the event. */
            if (!scnHookBegin(h, SCN_HOOK_PILL_PLACED)) return;
            lua_pushinteger(h->L, scenarioLuaIndexToScript((int)e->data[1]));
            lua_pushinteger(h->L, (lua_Integer)e->data[0]);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_PILL_PLACED, 3);
            return;

        case EVENT_PILL_PICKED_UP:
            /* [player, index], and the same swap. */
            if (!scnHookBegin(h, SCN_HOOK_PILL_PICKED_UP)) return;
            lua_pushinteger(h->L, scenarioLuaIndexToScript((int)e->data[1]));
            lua_pushinteger(h->L, (lua_Integer)e->data[0]);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_PILL_PICKED_UP, 3);
            return;

        case EVENT_PILL_KILLED:
            /* [index, attacker] — this one already leads with the item. */
            if (!scnHookBegin(h, SCN_HOOK_PILL_KILLED)) return;
            lua_pushinteger(h->L, scenarioLuaIndexToScript((int)e->data[0]));
            lua_pushinteger(h->L, (lua_Integer)e->data[1]);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_PILL_KILLED, 3);
            return;

        case EVENT_BUILT:
            scnHookBuilt(h, e, scripted);
            return;

        case EVENT_MINE_PLACED:
            /* [player, mx, my]. The event never reaches a client — it would
               hand every recipient a map of the minefield — and the host
               hears it because it is not the wire. */
            if (!scnHookBegin(h, SCN_HOOK_MINE_LAID)) return;
            lua_pushinteger(h->L, (lua_Integer)e->data[0]);
            lua_pushinteger(h->L, (lua_Integer)e->data[1]);
            lua_pushinteger(h->L, (lua_Integer)e->data[2]);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_MINE_LAID, 4);
            return;

        case EVENT_MINE_EXPLODED:
            /* [mx, my, layer]. The layer sits past the event's wire size
               for the same reason the event above is local-only, so a hook
               reads it and a client cannot. */
            if (!scnHookBegin(h, SCN_HOOK_MINE_EXPLOSION)) return;
            lua_pushinteger(h->L, (lua_Integer)e->data[0]);
            lua_pushinteger(h->L, (lua_Integer)e->data[1]);
            lua_pushinteger(h->L, (lua_Integer)e->data[2]);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_MINE_EXPLOSION, 4);
            return;

        default:
            /* Every other game event is one no hook is written for. */
            return;
    }
}

/* A lobby slot event, which is two facts in one: that the seat changed at
 * all, and — where the team differs from the copy the host keeps — that it
 * changed team.
 *
 * The copy is brought up to date before either hook runs, and the change is
 * reported first. A hook is free to move the seat again, and that lands as
 * its own event next tick; reporting the change first means the team a
 * script is told about is the team the event carried rather than one an
 * on_lobby handler has already moved on from. */
static void scnHookLobbySlot(ScenarioHost *h, const ControlEvent *evt,
                             bool scripted) {
    BYTE slot = evt->u.lobbySlot.playerNum;
    BYTE team = evt->u.lobbySlot.slot.teamNumber;
    bool changed;

    if (slot >= MAX_TANKS) {
        return;
    }
    changed        = (h->teams[slot] != team);
    h->teams[slot] = team;

    if (changed && scnHookBegin(h, SCN_HOOK_TEAM_CHANGED)) {
        lua_pushinteger(h->L, (lua_Integer)slot);
        lua_pushinteger(h->L, (lua_Integer)team);
        lua_pushboolean(h->L, scripted ? 1 : 0);
        scnHookCall(h, SCN_HOOK_TEAM_CHANGED, 3);
    }
    if (scnHookBegin(h, SCN_HOOK_LOBBY)) {
        lua_pushinteger(h->L, (lua_Integer)slot);
        lua_pushboolean(h->L, scripted ? 1 : 0);
        scnHookCall(h, SCN_HOOK_LOBBY, 2);
    }
}

static void scnControlEventHook(ScenarioHost *h, const ScnQueuedEvent *e,
                                bool scripted) {
    ControlEvent evt;

    scnControlEventOf(e, &evt);

    switch (evt.type) {
        case CTRL_PLAYER_JOIN:
            if (!scnHookBegin(h, SCN_HOOK_PLAYER_JOIN)) return;
            lua_pushinteger(h->L, (lua_Integer)evt.u.playerJoin.playerNum);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_PLAYER_JOIN, 2);
            return;

        case CTRL_PLAYER_LEAVE:
            if (!scnHookBegin(h, SCN_HOOK_PLAYER_LEAVE)) return;
            lua_pushinteger(h->L, (lua_Integer)evt.u.playerLeave.playerNum);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_PLAYER_LEAVE, 2);
            return;

        case CTRL_LOBBY_SLOT:
            scnHookLobbySlot(h, &evt, scripted);
            return;

        case CTRL_CHAT: {
            /* fromPlayer says what body holds. A seat sent raw text; the
               two sentinels above the seats are the server talking to
               itself, in a packed form with no sender to name, and there is
               no on_chat for those.

               The length is held against what the queue kept rather than
               against the field, which is wider than a player's line can
               be: a line is bytes, and a hook is handed the bytes that are
               there. */
            size_t len = evt.u.chat.bodyLen;

            if (evt.u.chat.fromPlayer >= MAX_TANKS) return;
            if (len > (size_t)PACKET_MAX_CHAT_MESSAGE) {
                len = (size_t)PACKET_MAX_CHAT_MESSAGE;
            }
            if (!scnHookBegin(h, SCN_HOOK_CHAT)) return;
            lua_pushinteger(h->L, (lua_Integer)evt.u.chat.fromPlayer);
            lua_pushlstring(h->L, (const char *)evt.u.chat.body, len);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_CHAT, 3);
            return;
        }

        default:
            /* Every other control event is one no hook is written for. */
            return;
    }
}

/* What the drain does with one entry. The channel is read first, because a
 * type byte is a CTRL_* on one and an EVENT_* on the other and the two
 * numberings overlap. */
static void scnEventConsume(void *ctx, const ScnQueuedEvent *e) {
    ScenarioHost *h = (ScenarioHost *)ctx;
    bool          scripted;

    if (h == NULL) {
        return;
    }
    scripted = (e->actor == SCN_EVENT_ACTOR_SCRIPT);

    if (e->channel == SCN_EVENT_CHANNEL_CONTROL) {
        scnControlEventHook(h, e, scripted);
    } else {
        scnGameEventHook(h, e, scripted);
    }
}

/* ── The regions a tank is standing in ────────────────────────────── */

/* Which regions one seat's tank is inside right now, one bit per region
 * index. A seat with nobody in it, and a seat whose tank is not in the
 * world — the countdown before a round, and the wait after a death — are
 * both inside nothing: serverSimGetTankInfo leaves the position at zero for
 * those, and zero is a real square, so has_tank is what decides rather than
 * the numbers. This is the same test game.tank(p) answers nil on, so a
 * script that reads a tank and a script that handles the hook agree about
 * which tanks have a place at all. */
static uint64_t scnRegionsHolding(ScenarioHost *h, BYTE slot) {
    const ScenarioManifest *m = &h->manifest;
    TankInfo                info;
    uint64_t                bits = 0;
    int                     i;

    if (!serverSimGetTankInfo(h->sim, slot, &info) || !info.has_tank) {
        return 0;
    }
    for (i = 0; i < (int)m->numRegions; i++) {
        if (scenarioLuaRegionHolds(&m->regions[i], (int)info.map_x,
                                   (int)info.map_y)) {
            bits |= (uint64_t)1 << i;
        }
    }
    return bits;
}

/* The enter and leave hooks, which are a difference rather than a fact the
 * engine reported. Once a tick the host takes a picture of where every tank
 * is standing and runs a hook for each bit that changed since the last one.
 *
 * What it costs: one tank read per seat, and one rectangle test per seat per
 * region. Sixteen seats against the sixty-four regions a round may name is
 * 1,024 four-comparison tests a tick, and a round that names none does
 * sixteen reads and nothing else. That is small enough that there is no
 * index here to go stale, and this comment is so a later reader does not
 * have to measure it.
 *
 * The picture is stored before any hook runs. That is what stops a handler
 * that moves a tank from spinning: the move lands after this tick's sample
 * was taken, so the region it moves into is seen by the next tick's scan
 * and costs one more call then — one per tick, visible in the log, and
 * never a loop inside a single tick. It is the rule the event drain follows
 * for the same reason, and it holds whatever the handler does, including
 * one that never asks who caused anything. Which is the other half of why
 * these two hooks are handed no such answer: between one sample and the
 * next a tank can be both moved by the script and driven under its own
 * power, and there is no honest value to give a handler that asks which of
 * the two this transition was.
 *
 * A tank that stops having a place leaves everything it was in. The
 * alternative — clearing the bits and saying nothing — would leave a script
 * counting who is inside a region permanently one too high the first time
 * somebody is killed standing in it, because the enter it saw would never
 * be matched. Every enter is followed by exactly one leave, which is the
 * thing a handler can rely on. */
static void scnScanRegions(ScenarioHost *h) {
    const ScenarioManifest *m = &h->manifest;
    BYTE                    slot;

    if (m->numRegions == 0) {
        return;
    }
    for (slot = 0; slot < MAX_TANKS; slot++) {
        uint64_t now     = scnRegionsHolding(h, slot);
        uint64_t changed = now ^ h->inRegion[slot];
        int      i;

        h->inRegion[slot] = now;
        if (changed == 0) {
            continue;
        }
        for (i = 0; i < (int)m->numRegions; i++) {
            uint64_t bit = (uint64_t)1 << i;
            ScnHookId id;

            if ((changed & bit) == 0) {
                continue;
            }
            id = (now & bit) ? SCN_HOOK_ENTER_REGION : SCN_HOOK_LEAVE_REGION;
            if (!scnHookBegin(h, id)) {
                continue;
            }
            lua_pushinteger(h->L, (lua_Integer)slot);
            lua_pushstring(h->L, m->regions[i].name);
            scnHookCall(h, id, 2);
        }
    }
}

/* ── The timers ───────────────────────────────────────────────────── */

/* Every timer the round has reached the tick for, oldest first. The due set
 * is read once, so a timer one of these sets waits for the next tick
 * however short its delay — the same bound the event drain has, and for the
 * same reason.
 *
 * The reference comes out of the set and is released here whether the call
 * returned or raised, so a function that fails is still let go of: a timer
 * runs once. */
static void scnRunTimers(ScenarioHost *h) {
    int refs[SCN_TIMERS_MAX];
    int n;
    int i;

    if (h->L == NULL) {
        return;
    }
    n = scenarioLuaTimersTakeDue(&h->timers, serverSimGetTick(h->sim), refs,
                                 SCN_TIMERS_MAX);
    for (i = 0; i < n; i++) {
        /* A scenario switched off part way through the run still has to be
           handed back what the rest of the timers were holding. */
        if (!h->disabled) {
            lua_rawgeti(h->L, LUA_REGISTRYINDEX, refs[i]);
            if (lua_pcall(h->L, 0, 0, 0) != 0) {
                scnErrorRaised(h, "a timer", scnLuaError(h->L));
                lua_pop(h->L, 1);
            } else {
                scnErrorCleared(h);
            }
        }
        luaL_unref(h->L, LUA_REGISTRYINDEX, refs[i]);
    }
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
 * arrived since the last tick; then the regions the frame moved tanks in
 * and out of, which are a fact of the same frame and belong with the rest
 * of them; then the timers the frame has reached; then on_tick, which is
 * the script's own frame and comes after everything the frame produced;
 * then on_end where the round has just finished; then what the queue had no
 * room for, after the drain so a drop the drain caused is reported in its
 * own tick; then the line the round owes the players, last, so a line any
 * of the above raised goes out in this tick rather than the next.
 *
 * The state is read again between the calls rather than once at the top. A
 * hook can end the round from the drain and on_tick can end it too, and
 * neither the frame the script gets nor the end it is told about should be
 * decided from a state that has already moved on. */
static void scnTick(void *ctx) {
    ScenarioHost *h = (ScenarioHost *)ctx;
    ServerState   state;

    if (h == NULL) {
        return;
    }
    scnLockEnter(&h->lock);
    if (h->startPending &&
        serverSimGetState(h->sim) == serverStateRunning) {
        h->startPending = false;
        scnHookRun(h, SCN_HOOK_START);
    }
    (void)scenarioEventsDrain(&h->events, scnEventConsume, h);
    scnScanRegions(h);
    scnRunTimers(h);

    if (serverSimGetState(h->sim) == serverStateRunning &&
        scnHookBegin(h, SCN_HOOK_TICK)) {
        lua_pushinteger(h->L, (lua_Integer)serverSimGetTick(h->sim));
        scnHookCall(h, SCN_HOOK_TICK, 1);
    }

    state = serverSimGetState(h->sim);
    if (state == serverStateGameOver && h->lastState != serverStateGameOver) {
        scnHookRun(h, SCN_HOOK_END);
    }
    h->lastState = state;

    scnReportDrops(h);
    scnSayPending(h);
    scnLockLeave(&h->lock);
}

/* The lobby out of the manifest and into the shape the sim reads. A straight
 * copy of the four numbers and the brain, dropping the teams the sim has no
 * seat for: the manifest holds what the file said and this holds what the
 * roster can hold.
 *
 * extra_teams is not here — the sim asks that one through the policy, as a
 * question about what a host may do rather than a description of the lobby
 * to build. */
static void scnFillLobbyTemplate(const ScnManifestLobby *lob,
                                 ScnLobbyTemplate *out) {
    uint8_t i;

    memset(out, 0, sizeof(*out));
    if (lob == NULL) return;
    out->maxPlayers = lob->maxPlayers;
    for (i = 0; i < lob->numTeams && out->numTeams < MAX_TANKS; i++) {
        const ScnManifestTeam *src = &lob->teams[i];
        ScnLobbyTeam          *dst;
        if (src->id == 0 || src->id >= MAX_TANKS) continue;
        dst = &out->teams[out->numTeams];
        out->numTeams++;
        dst->id      = src->id;
        dst->bots    = src->bots;
        dst->maxBots = src->maxBots;
        dst->fielded = src->fielded;
        SDL_strlcpy(dst->brain, src->brain, sizeof(dst->brain));
    }
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
        scnRawGlobal(h->L, "allow_extra_teams");
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
    scnHooksForget(h);
    /* Emptied without releasing anything, which is right for the one place
       this is reached from: a round start that has already closed the state
       its chunk could have set a timer in. There is nothing left to release
       them against, and the set must not carry them into the round. */
    scenarioLuaTimersReset(&h->timers);
}

/* The teams the roster is on right now. Called wherever the host starts
 * again from what the world says — at the attach, and at every round start —
 * so the first lobby slot event of a round is measured against the round's
 * own roster rather than against nothing.
 *
 * An empty seat reads as team zero here and on the event, which is what the
 * lobby calls a seat with no team, so the two agree either way. */
static void scnSeedTeams(ScenarioHost *h) {
    BYTE i;

    for (i = 0; i < MAX_TANKS; i++) {
        ServerSimRosterSlot slot;
        h->teams[i] = serverSimGetRosterSlot(h->sim, i, &slot) ? slot.team : 0;
    }
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
    ScnParseReport   rep;
    char             err[SCN_ERR_LEN];
    bool             humans[MAX_TANKS];

    err[0] = '\0';

    /* Whatever the last round was holding, released against the state that
       holds it, before this one boots a state of its own. Every reference in
       the set belongs to h->L, and this is the last moment that is true: the
       chunk below runs in the new state and anything it sets from the top
       level belongs to that one, which is why the failure paths empty the
       set rather than releasing it. */
    scenarioLuaTimersDrop(h->L, &h->timers);

    L = scnBootVm(h);
    if (L == NULL) {
        scnSay(h->lastError, sizeof(h->lastError),
               "scenario: no memory for this round's Lua state");
        scnRoundWithoutScenario(h);
        return;
    }
    rep.soft    = h->lastError;
    rep.softLen = sizeof(h->lastError);
    rep.sink    = NULL;
    if (!scnRunChunk(L, h->src, h->srcLen, h->chunkName, err, sizeof(err)) ||
        !scnReadManifest(L, &fresh, h->sidecar, err, sizeof(err), &rep)) {
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
    /* The whole table, read from the sidecar's bytes again — which is how a
       region the last round defined stops existing without anything here
       having to remove it. */
    h->manifest = fresh;
    scnHooksResolve(h, L);

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

    /* The same for the roster copy the team change is measured against, and
       for the state on_end watches: both are this round's, and both are
       read before on_setup so a setup that moves a seat or ends the round
       is itself the first change. */
    scnSeedTeams(h);
    h->lastState = serverSimGetState(h->sim);

    /* Nobody is inside anything until the first scan says so, which is the
       round's first tick. Cleared here rather than carried, because the
       regions themselves have just been read over and a bit set against the
       last round's list would name a different rectangle in this one. */
    memset(h->inRegion, 0, sizeof(h->inRegion));

    scnApplyRules(h);

    scnRosterHumans(h, humans);
    scnHookRun(h, SCN_HOOK_SETUP);
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
    ScnParseReport   rep;
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
    h->lua.timers   = &h->timers;
    /* calloc left every reference at 0, which is a reference to something.
       An empty set is LUA_NOREF throughout. */
    scenarioLuaTimersReset(&h->timers);
    /* The hooks belong to a round's VM, and this is the one the table is
       read in. The first round start resolves them. */
    scnHooksForget(h);
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

    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = NULL;
    if (!scnRunChunk(L, src, srcLen, chunkName, err, errLen) ||
        !scnReadManifest(L, &m, sidecar, err, errLen, &rep)) {
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
    /* The lobby goes over as data rather than as another callback: the sim
       seats and reconciles it at every point a lobby is built, and a question
       asked from inside the sim's own map change would otherwise have to
       reach back into a host that is being replaced at that moment. */
    {
        ScnLobbyTemplate t;
        scnFillLobbyTemplate(&m.lobby, &t);
        serverSimSetScenarioLobbyTemplate(sim, &t);
    }

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
    /* And the roster copy, for the same reason: the replay above is a
       picture of how things stand, so the teams it named are what the host
       starts from rather than changes to report. */
    scnSeedTeams(h);
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
    ScnParseReport   rep;
    ScnLuaCtx        check;
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
       state lacked.

       It carries a context of its own, though. The rows that read the sim
       are the same; the two that write what the host is holding are not
       given it. A chunk that defined a region from the top level would move
       a rectangle under the round that is playing, and one that set a timer
       would leave the round holding a function in a state this call is
       about to close. Both would break the one thing a reload promises,
       which is that nothing changes until the next round start. The
       manifest it writes into is the local below, which is read over a few
       lines later. */
    memset(&m, 0, sizeof(m));   /* the chunk can read it before it is read */
    check.sim      = h->sim;
    check.manifest = &m;
    check.timers   = NULL;
    L = scnBootVmWith(&check);
    if (L == NULL) {
        scnFmt(err, errLen, "scenario: no memory for a Lua state");
        free(src);
        return false;
    }

    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = NULL;
    if (!scnRunChunk(L, src, srcLen, h->chunkName, err, errLen) ||
        !scnReadManifest(L, &m, h->sidecar, err, errLen, &rep)) {
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

/* The sim's map change, answered. The caller's own pointer is the context
 * because the host it names is the one being replaced here. */
static void scnMapChanged(void *ctx, ServerSim *sim, const char *mapPath) {
    ScenarioHost **slot = (ScenarioHost **)ctx;
    char           err[512];

    if (slot == NULL) return;
    scenarioHostDetach(*slot);
    *slot = NULL;
    /* A map that came from bytes rather than a file has nothing beside it to
       read, so the map is played plainly and the detach above is the whole
       of the work. */
    if (sim == NULL || mapPath == NULL || mapPath[0] == '\0') return;

    err[0] = '\0';
    *slot = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    if (*slot == NULL && err[0] != '\0') {
        /* Said rather than returned: a map commit has nobody to answer. */
        scnSay(NULL, 0, "%s", err);
    }
}

void scenarioHostFollowMap(ServerSim *sim, ScenarioHost **slot) {
    if (sim == NULL) return;
    if (slot == NULL) {
        serverSimSetScenarioMapChanged(sim, NULL, NULL);
        return;
    }
    serverSimSetScenarioMapChanged(sim, scnMapChanged, slot);
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
        /* The lobby goes with them. What is already seated is left where it
           is — emptying the roster is the map change's business, and a
           detach at shutdown has no lobby left to tidy. The map-change
           registration is not cleared here: it belongs to whoever owns this
           host, not to the host, and it is the thing that will attach the
           next one. */
        serverSimSetScenarioLobbyTemplate(h->sim, NULL);
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
    scenarioLuaTimersDrop(h->L, &h->timers);
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
