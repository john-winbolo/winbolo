/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Host
 *Filename:      scenario_host.c
 *Author:        John Morrison
 *Purpose:
 *  Finds the Lua script beside a map, boots a VM, runs its
 *  chunk, reads the scenario table it declares into one
 *  struct, and applies the rules that table sets through the
 *  op funnel from inside the sim's round-boot callback.
 *
 *  The round's shape: a fresh VM at the boot the start makes
 *  before it places anything, so a fresh set of globals and a
 *  round whose own state answers the placements; the rules
 *  applied there too, so the opening tanks are built under
 *  them; on_setup called after the tanks, inside the setup
 *  window the start opens; the roster audited; on_start
 *  called on the first running tick;
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
 *  and the answers to the policy questions registered below.
 *
 *  The script is read from disk once, at attach. Every round
 *  after that runs the bytes the host is holding, so a round
 *  start touches no file: it runs inside the start with the
 *  sim mutex held, which is no place for disk work, and an
 *  edit made to the file while the server is up does not
 *  reach a round on its own.
 *
 *  Nothing here publishes. The start opens its setup window
 *  across both of the calls it makes here, which holds the
 *  set-rule handler's own CTRL_SIM_RULES publish, and the
 *  publish the start makes at its end carries the whole
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

/* SDL_Mutex, SDL_ThreadID and SDL_GetCurrentThreadID, which the VM lock is
 * built from. server_sim.h brings SDL in as well; named here because this
 * file uses it directly. */
#include <SDL3/SDL.h>

#include "platform_types.h"        /* BOLO_STATIC_ASSERT */
#include "bolo_rand.h"             /* BoloRandState, bolo_rand_save */
#include "brain_list.h"            /* brainListResolve — the brain a team
                                    * names, against this server's own
                                    * brains directory */
#include "server_sim.h"            /* ServerSim, serverSimConsoleMessage */
#include "wire_limits.h"           /* LOBBY_MAP_UPLOAD_MAX_BYTES — how much of
                                    * a map file the chunk test reads — and
                                    * LOBBY_PACKAGE_UPLOAD_MAX_BYTES, how much
                                    * of one the attach reads */
#include "scenario_defs.h"         /* SCN_RULE_COUNT, ScenarioOp, ScnOpResult */
#include "sim_rules_names.h"       /* simRulesRuleIndex / simRulesRuleName */
#include "server_sim_scenario.h"   /* the funnel and the round-start hook-up */

#include "scenario_host.h"
#include "scenario_dir.h"          /* scnDirList — what the sim's scenario
                                    * lister is pointed at */
#include "scenario_manifest.h"
#include "scenario_events.h"
#include "scenario_lua.h"
#include "scenario_manifest_json.h" /* a package's manifest, read and held
                                     * against the table its script declares */
#include "scenario_package.h"      /* scnPackageFindInMap — the second way a
                                    * map can carry a script */
#include "scenario_sandbox.h"      /* the libraries a scenario state gets */
#include "scenario_validate.h"     /* ScnParseReport, and the parse this file
                                    * shares with the validator */

/* One operator line. Long enough for a Lua error, which carries the file
 * path and the line number ahead of its message. */
#define SCN_ERR_LEN 512

/* The manifest is sized in scenario_host.h, which cannot see the rule
 * count; this is where the two are visible together. */
BOLO_STATIC_ASSERT(SCN_MANIFEST_RULES_MAX >= (int)SCN_RULE_COUNT,
                   manifest_holds_every_rule_at_once);

/* A team's brain becomes the path a spawn carries, and the manifest sizes the
 * name without seeing the op. Held against the op's own length here, because
 * the template below writes the resolved path into a field of that width. */
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
    X(PING,             "on_ping")                                           \
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

    char             script[SCN_SCRIPT_PATH_MAX];
    /* The map the script was found for, kept because a reload decides where
     * the script comes from all over again and both answers start here.
     * Empty for a mod, which has no map: that one reloads from script
     * above, which is its own file in the scenarios directory. */
    char             mapPath[SCN_SCRIPT_PATH_MAX];
    /* Where this scenario came from: the committed map, or a file in the
     * server's scenarios directory the host picked. A reload reads the same
     * way the attach did, and this is what says which; it is also what the
     * lobby is told, so the settings event names a mod as a mod. */
    LobbyScenarioSource source;
    char            *src;     /* the script's bytes, read once at attach */
    size_t           srcLen;

    /* The script came out of the map's own container rather than off a file
     * beside it. What it turns on: the manifest below is pushed as the
     * scenario global before every chunk this host runs, and the table each
     * chunk leaves behind is held against it. */
    bool             fromPackage;

    /* The container's manifest, decoded once at the attach and kept. Distinct
     * from manifest above, which is the live table the game rows read and the
     * round start reads over from whatever this round's chunk declared: this
     * one is what the package says it is, does not change while the host is
     * attached, and is what a round's table has to agree with. Untouched for
     * a loose script, which has no manifest but its own. */
    ScenarioManifest pkgManifest;

    char             chunkName[SCN_SCRIPT_PATH_MAX + 2];
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

/* ── Finding the script ───────────────────────────────────────────── */

/* .../X.map is accompanied by .../X.scenario.lua. A path that does not end
 * in .map keeps its whole name and takes the suffix as it is, so a caller
 * that hands over a name without an extension still resolves. */
bool scnScriptPath(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    size_t base = n;
    size_t suffix = strlen(SCN_SCRIPT_SUFFIX);

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
    memcpy(out + base, SCN_SCRIPT_SUFFIX, suffix + 1);
    return true;
}

/* The whole script, into a buffer the caller owns. Returns false with err
 * set when the file is there but cannot be used; returns false with err
 * left empty when there is no file at all, which is not a fault.
 *
 * Read in one go rather than streamed: what is being read is hand-written
 * Lua, the cap below is what says so, and a file above it is refused before
 * a byte of it is kept. */
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
    if (size > (long)SCN_SCRIPT_MAX_BYTES) {
        fclose(f);
        scnFmt(err, errLen,
               "scenario: %s is %ld bytes and the limit is %ld — too large to "
               "be a script", path, size, (long)SCN_SCRIPT_MAX_BYTES);
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
       chunk, so math holds what the whitelist opened a line ago and no
       script has had a statement in which to touch it.
       math.randomseed is one of the names the whitelist takes, which is why
       the seal runs after this call rather than inside the open: the seed is
       drawn here and the function is gone before the chunk can reach it. */
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
    lua_State *L = scnSandboxNewState();
    if (L == NULL) {
        return NULL;
    }
    lua_atpanic(L, scnPanic);
    scnSandboxOpenLibs(L);
    scnSeedRandom(L);
    scnSandboxSealRandom(L);
    return L;
}

/* The other end of it. A state carries its memory count beside it, so every
 * close goes through here rather than through lua_close: the count is on the
 * heap and a plain close would leave it behind. NULL is nothing to close,
 * which is what lets the failure paths below close without asking. */
void scnCloseVm(lua_State *L) {
    scnSandboxCloseState(L);
}

/* A state with the script's own surface on it, ready for a chunk.
 *
 * The game table goes on before the chunk runs, and every state this host
 * boots gets one: a script may read the game at the top level rather than
 * from a hook, and a script that does must load the same way at an attach,
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
 * straight from the file produces.
 *
 * Text only. A precompiled chunk is a stream the VM trusts and does not
 * check, so bytes that arrived inside a map file are refused at the load
 * rather than read as instructions. Every state the host boots comes through
 * here, the validator's included, so WinBoloDS -validate refuses one too. */
bool scnRunChunk(lua_State *L, const char *src, size_t srcLen,
                 const char *chunkName, char *err, size_t errLen) {
    ScnSandboxCall saved;
    bool           ok = false;

    /* The chunk's top level is script code like any other, and the one place
       a loop in it would show is here: an attach that never returns. Armed
       across the load as well, which costs nothing — the parser is C and
       executes no instructions to count — and leaves one disarm to reach
       whichever way this goes. */
    scnSandboxArmCall(L, &saved);
    if (luaL_loadbufferx(L, src, srcLen, chunkName, "t") != 0) {
        scnFmt(err, errLen, "scenario: %s", scnLuaError(L));
        lua_pop(L, 1);
    } else if (lua_pcall(L, 0, 0, 0) != 0) {
        scnFmt(err, errLen, "scenario: %s", scnLuaError(L));
        lua_pop(L, 1);
    } else {
        ok = true;
    }
    scnSandboxDisarmCall(L, &saved);
    return ok;
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
                /* The table the team's bots are built with, through the one
                   reader spawn_bot's own init goes through, so a script
                   cannot find the two spelled differently. A pair that did
                   not fit costs that pair and is named on the team; the ones
                   read before it stay, the way scnReadRules keeps the rest of
                   a rules table past a bad key. An init that is not a table
                   at all is left alone with no pair named, as every other
                   reader here leaves a field of the wrong type. */
                (void)scenarioLuaReadTable(L, t, "init", &team->init,
                                           team->initBadKey,
                                           sizeof(team->initBadKey), NULL, 0);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);   /* teams */
    lua_pop(L, 1);   /* lobby */
}

/* A key that names no rule is refused by name and the rest of the table
 * still applies: one bad line in a script should not cost an author every
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
            int         idx = simRulesRuleIndex(key);
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
 * triggers is not read. Its schema is not settled, so a script that
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
    m->api        = scnReadInt(L, tbl, "api", 1);
    m->bound      = scnReadBool(L, tbl, "bound", true);
    m->fillToCaps = scnReadBool(L, tbl, "fill_to_caps", false);

    scnReadLobby(L, tbl, &m->lobby);
    scnReadRules(L, tbl, m, rep);
    scnReadTags(L, tbl, m, rep);
    scnReadRegions(L, tbl, m, rep);

    lua_pop(L, 1);
    return true;
}

/* ── Writing the table ────────────────────────────────────────────── */

/* The struct back out as the table a script would have written it as. The
 * five builders below take one piece each and leave it on the stack;
 * scnPushManifestGlobal at the end of them is the one the rest of this file
 * calls, and carries what the whole thing is for. */
static void scnPushTeams(lua_State *L, const ScnManifestLobby *lob) {
    int teams;
    int i;

    lua_newtable(L);
    teams = lua_gettop(L);
    for (i = 0; i < (int)lob->numTeams; i++) {
        const ScnManifestTeam *team = &lob->teams[i];
        int                    e;

        lua_newtable(L);
        e = lua_gettop(L);
        lua_pushinteger(L, (lua_Integer)team->id);
        lua_setfield(L, e, "id");
        lua_pushinteger(L, (lua_Integer)team->bots);
        lua_setfield(L, e, "bots");
        lua_pushinteger(L, (lua_Integer)team->maxBots);
        lua_setfield(L, e, "max_bots");
        lua_pushboolean(L, team->fielded ? 1 : 0);
        lua_setfield(L, e, "fielded");
        lua_pushstring(L, team->brain);
        lua_setfield(L, e, "brain");
        /* And the init the team's bots are built with, which was missing
           here: a packaged manifest that declares one, in a package whose
           script leaves the table to the manifest, would be read back with
           no init at all and the two forms held to disagree about it at
           every load.

           Every value goes over as a string, which is how the table reader
           stores one whatever the script wrote, so what comes back out of
           this is what went in. An empty table is left out entirely, no
           pairs meaning the same as no table. */
        if (team->init.count > 0) {
            int ini;
            int k;

            lua_newtable(L);
            ini = lua_gettop(L);
            for (k = 0; k < (int)team->init.count; k++) {
                lua_pushstring(L, team->init.kv[k].value);
                lua_setfield(L, ini, team->init.kv[k].key);
            }
            lua_setfield(L, e, "init");
        }
        /* An array, as the file writes it, so position is part of what the
           comparison holds the two forms to. */
        lua_rawseti(L, teams, i + 1);
    }
}

static void scnPushLobby(lua_State *L, const ScnManifestLobby *lob) {
    int t;

    lua_newtable(L);
    t = lua_gettop(L);
    lua_pushinteger(L, (lua_Integer)lob->maxPlayers);
    lua_setfield(L, t, "max_players");
    lua_pushboolean(L, lob->extraTeams ? 1 : 0);
    lua_setfield(L, t, "extra_teams");
    scnPushTeams(L, lob);
    lua_setfield(L, t, "teams");
}

/* Keyed by the rule's own name, which is the key scnReadRules resolves back
 * to the index. An index no name answers to is left out rather than written
 * under an empty key. */
static void scnPushRules(lua_State *L, const ScenarioManifest *m) {
    int      t;
    uint16_t i;

    lua_newtable(L);
    t = lua_gettop(L);
    for (i = 0; i < m->numRules; i++) {
        const char *name = simRulesRuleName((int)m->rules[i].rule);
        if (name == NULL || name[0] == '\0') {
            continue;
        }
        lua_pushnumber(L, (lua_Number)m->rules[i].value);
        lua_setfield(L, t, name);
    }
}

/* One kind's tags, keyed by the 1-based entity index the array is held at.
 * An entity with no tags is left out: an empty list and no list at all read
 * back the same, and the table stays the size the map's tags make it. */
static void scnPushTagKind(lua_State *L, const ScnManifestTags *arr,
                           int maxEntity) {
    int t;
    int e;

    lua_newtable(L);
    t = lua_gettop(L);
    for (e = 1; e <= maxEntity; e++) {
        int i;
        if (arr[e].count == 0) {
            continue;
        }
        lua_newtable(L);
        for (i = 0; i < (int)arr[e].count; i++) {
            lua_pushstring(L, arr[e].tag[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_rawseti(L, t, e);
    }
}

static void scnPushTags(lua_State *L, const ScenarioManifest *m) {
    int t;

    lua_newtable(L);
    t = lua_gettop(L);
    scnPushTagKind(L, m->pillTags, MAX_PILLS);
    lua_setfield(L, t, "pills");
    scnPushTagKind(L, m->baseTags, MAX_BASES);
    lua_setfield(L, t, "bases");
    scnPushTagKind(L, m->startTags, MAX_STARTS);
    lua_setfield(L, t, "starts");
}

static void scnPushRegions(lua_State *L, const ScenarioManifest *m) {
    int t;
    int i;

    lua_newtable(L);
    t = lua_gettop(L);
    for (i = 0; i < (int)m->numRegions; i++) {
        const ScnManifestRegion *r = &m->regions[i];
        int                      e;

        lua_newtable(L);
        e = lua_gettop(L);
        lua_pushinteger(L, (lua_Integer)r->x);
        lua_setfield(L, e, "x");
        lua_pushinteger(L, (lua_Integer)r->y);
        lua_setfield(L, e, "y");
        lua_pushinteger(L, (lua_Integer)r->w);
        lua_setfield(L, e, "w");
        lua_pushinteger(L, (lua_Integer)r->h);
        lua_setfield(L, e, "h");
        lua_setfield(L, t, r->name);
    }
}

/* A package's manifest as the scenario global, in the shape scnReadManifest
 * above reads and Appendix G of the plan specifies. Put on the state before
 * the chunk runs, and only for a script that came out of a container.
 *
 * What it buys: a packaged script may declare no scenario at all and still
 * play, because what the chunk leaves behind is then this table; and one that
 * restates the table for a reader of the source is held against the manifest
 * afterwards rather than believed. A script that assigns nothing reads back
 * exactly what went on, so that comparison has nothing to do. A loose script
 * gets nothing pushed and declares its own table, as it always has.
 *
 * triggers is not built. Nothing reads it and the struct does not carry it.
 *
 * The value goes on through the globals table itself rather than through
 * lua_setglobal, for the reason scnRawGlobal reads through it: a metatable on
 * _G is the script's business and running one here is not. */
static void scnPushManifestGlobal(lua_State *L, const ScenarioManifest *m) {
    int t;

    lua_newtable(L);
    t = lua_gettop(L);

    lua_pushstring(L, m->name);
    lua_setfield(L, t, "name");
    lua_pushstring(L, m->description);
    lua_setfield(L, t, "description");
    lua_pushinteger(L, (lua_Integer)m->api);
    lua_setfield(L, t, "api");
    lua_pushstring(L, m->game);
    lua_setfield(L, t, "game");
    lua_pushboolean(L, m->bound ? 1 : 0);
    lua_setfield(L, t, "bound");
    lua_pushboolean(L, m->fillToCaps ? 1 : 0);
    lua_setfield(L, t, "fill_to_caps");

    scnPushLobby(L, &m->lobby);
    lua_setfield(L, t, "lobby");
    scnPushRules(L, m);
    lua_setfield(L, t, "rules");
    scnPushTags(L, m);
    lua_setfield(L, t, "tags");
    scnPushRegions(L, m);
    lua_setfield(L, t, "regions");

    scnPushGlobals(L);
    lua_pushstring(L, "scenario");
    lua_pushvalue(L, t);
    lua_rawset(L, -3);
    lua_pop(L, 2);               /* the globals table, and the table itself */
}

/* What a package whose script restates the table and disagrees with it reads
 * like. scnManifestAgrees writes the sentence saying what differs and the key
 * separately; the key is the line of the table to go and look at, which the
 * sentence does not name, so the three places that refuse a load over one say
 * both and say them the same way. */
static void scnDisagreed(char *out, size_t outLen, const char *why,
                         const char *key) {
    scnFmt(out, outLen, "%s — the key is '%s'", why, key);
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
                   simRulesRuleName((int)h->manifest.rules[i].rule),
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

/* One more error, for a caller that has already said its piece. Apart from a
 * hook or a policy raising, the other thing that counts is a tick that found
 * the event queue full: however many events that tick lost, it is one thing
 * that went wrong and it counts once, so a single overflow cannot take a
 * scenario from nothing to switched off.
 *
 * The count cannot wrap. Past the limit the only thing left to do is switch
 * the scenario off, which the next line does, and a switched-off round still
 * counts the drops its queue goes on taking. */
static void scnErrorCounted(ScenarioHost *h) {
    if (h->errors < UINT_MAX) {
        h->errors++;
    }
    if (h->errors >= SCN_ERROR_LIMIT) {
        scnDisable(h, "%.32s is off for the rest of the round: %u errors in "
                      "a row.", scnSubject(h), h->errors);
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
    scnErrorCounted(h);
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
    ScnSandboxCall saved;
    int            rc;

    scnSandboxArmCall(h->L, &saved);
    rc = lua_pcall(h->L, nargs, 0, 0);
    scnSandboxDisarmCall(h->L, &saved);
    if (rc != 0) {
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

        case EVENT_PING:
            /* [sender, kind, xHi, xLo, yHi, yLo]. The two positions are in
               world units, 256 to a map square, and the hook is handed the
               map square, because that is what a script points at. The hook
               only watches: there is no scenario call that places a ping,
               and nothing a handler returns changes the marker. */
            if (!scnHookBegin(h, SCN_HOOK_PING)) return;
            lua_pushinteger(h->L, (lua_Integer)e->data[0]);
            lua_pushinteger(h->L, (lua_Integer)e->data[1]);
            lua_pushinteger(h->L,
                (lua_Integer)(((unsigned)e->data[2] << 8 |
                               (unsigned)e->data[3]) >> M_W_SHIFT_SIZE));
            lua_pushinteger(h->L,
                (lua_Integer)(((unsigned)e->data[4] << 8 |
                               (unsigned)e->data[5]) >> M_W_SHIFT_SIZE));
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_PING, 5);
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
            /* [player, index, mx, my] and the armour behind the wire at
               data[4], which this file reads as freely as it reads the mine's
               layer. The hook names the pillbox first: the two are the other
               way round from the event. The armour follows the player because
               it is what a script has to read to know which kind of placing
               this was — a builder finishing the job leaves it at the sim's
               cap, and a tank sinking, a builder dying or a player leaving
               leaves it at 0. */
            if (!scnHookBegin(h, SCN_HOOK_PILL_PLACED)) return;
            lua_pushinteger(h->L, scenarioLuaIndexToScript((int)e->data[1]));
            lua_pushinteger(h->L, (lua_Integer)e->data[0]);
            lua_pushinteger(h->L, (lua_Integer)e->data[4]);
            lua_pushboolean(h->L, scripted ? 1 : 0);
            scnHookCall(h, SCN_HOOK_PILL_PLACED, 4);
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

    /* A switched-off round has nothing to tell: no hook of its would be
       called with what the scan found. Read before the region count, so the
       sixteen tank reads and the rectangle tests behind them are work the
       round does not do at all. */
    if (h->disabled || m->numRegions == 0) {
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
    ScnSandboxCall saved;
    int refs[SCN_TIMERS_MAX];
    int n;
    int i;
    int rc;

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
            scnSandboxArmCall(h->L, &saved);
            rc = lua_pcall(h->L, 0, 0, 0);
            scnSandboxDisarmCall(h->L, &saved);
            if (rc != 0) {
                scnErrorRaised(h, "a timer", scnLuaError(h->L));
                lua_pop(h->L, 1);
            } else {
                scnErrorCleared(h);
            }
        }
        luaL_unref(h->L, LUA_REGISTRYINDEX, refs[i]);
    }
}

/* Say what the queue had no room for, once, and count the tick that lost
 * them as one error. The drop itself happens inside a publish where there is
 * nothing safe to say; this runs at the tick, after the drain, so events the
 * drain's own work pushed past the end are reported in the tick they were
 * lost in.
 *
 * One error and not one per event: a busy frame can lose hundreds at once,
 * and counting each of them would switch a scenario off in a single tick
 * over one overflow. The line still says how many were lost, which is the
 * number an operator wants. */
static void scnReportDrops(ScenarioHost *h) {
    uint32_t dropped = scenarioEventsTakeDropped(&h->events);

    if (dropped == 0) {
        return;
    }
    scnSay(h->lastError, sizeof(h->lastError),
           "scenario: %lu events arrived with the queue full at %d and were "
           "dropped", (unsigned long)dropped, SCN_EVENT_QUEUE_MAX);
    scnErrorCounted(h);
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
    /* First, inside the lock and before anything this tick runs: the console
       lines print may put out are counted per tick as well as per call, and
       this is the tick they are counted against. */
    if (h->L != NULL) {
        scnSandboxTickReset(h->L);
    }
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

/* The file name out of a path. What the lobby says a scenario came from is a
 * name; where the server keeps its maps is not something clients are told.
 * Both separators, because a Windows server holds the other one.
 *
 * A reload's message uses it for a second reason: what a reload says goes to
 * whoever asked for it as a single 128-byte line, and Lua puts the chunk's
 * name at the front of every message it raises, so a script under a deep map
 * directory would spend the whole line on a path the asker cannot see and
 * leave no room for the line number and the error itself. The round boot
 * keeps the full path — that one goes to the operator's console, where the
 * path is the useful part. */
static const char *scnFileNameOf(const char *path) {
    const char *last = path;
    const char *p;

    if (path == NULL) return "";
    for (p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

/* The path of the brain a team names, which is what the sim writes onto the
 * seat.
 *
 * A scenario names a brain rather than pathing to one: the name is a directory
 * under the server's own brains/, and a scenario shared with a server does not
 * know that server's layout. The name becomes a path once, here, because both
 * of the things that read a seat's brain back — the countdown's warm in
 * warmSeatBrainPath and a spawn that fields the seat in scenarioBrainPath —
 * open it as a file. Once here also keeps the string the same for the whole
 * round, so a parked runner goes on matching the brain it was built with.
 *
 * A name this server does not have leaves the field empty, which is the seat
 * on the server's own brain, and says one line naming the team and the name.
 * The round still plays. An operator who would rather know before starting one
 * hears the same thing from -validate, which reports the name against the
 * team's own key. An empty brain is the server's own and is left alone. */
static void scnTeamBrainPath(const ScnManifestTeam *team, char *out,
                             size_t outLen) {
    out[0] = '\0';
    if (team->brain[0] == '\0') {
        return;
    }
    if (brainListResolve(team->brain, out, outLen)) {
        return;
    }
    out[0] = '\0';
    if (strpbrk(team->brain, "/\\") != NULL) {
        scnSay(NULL, 0,
               "scenario: team %u writes the brain '%s' as a path, and a "
               "scenario names a brain — the directory under this server's "
               "brains/; its seats take the server's own",
               (unsigned)team->id, team->brain);
        return;
    }
    scnSay(NULL, 0,
           "scenario: team %u asks for the brain '%s', which this server does "
           "not have; its seats take the server's own",
           (unsigned)team->id, team->brain);
}

/* The lobby out of the manifest and into the shape the sim reads. A straight
 * copy of the four numbers and the init table, with the brain resolved above,
 * dropping the teams the sim has no seat for: the manifest holds what the file
 * said and this holds what the roster can hold.
 *
 * initBadKey is not here. What a bad pair is called is something to tell an
 * author about, which the validator does; the sim is handed the pairs that
 * were taken and has nothing to do with the one that was not.
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
        scnTeamBrainPath(src, dst->brain, sizeof(dst->brain));
        dst->init    = src->init;
    }
}

/* ── The policy ───────────────────────────────────────────────────── */

/* The decisions the sim puts to the script. Each is asked at the engine's
 * own site, between the sim's policy enter and leave, so an op issued from
 * inside an answer is refused SCN_OP_IN_POLICY by the funnel rather than
 * mutating state the caller is halfway through reading.
 *
 * Classic is what a script that says nothing gets, and there are four ways
 * to say nothing: no function of that name, a scenario switched off for the
 * round, a nil return, and a call that raises. Only the raise is a failure,
 * and only the raise counts toward the limit.
 *
 * The lobby asks allow_extra_teams from the GUI thread while the sim ticks on
 * another, which is the reason the VM lock exists at all. The lock knows the
 * thread that holds it, so a question the engine asks from inside an op a
 * script issued — an alliance line published from a set_team, say — enters
 * the lock it is already inside rather than stopping dead on it.
 *
 * A name is looked up per call rather than held as a reference the way a
 * hook is. There is no one moment every policy could be resolved at: the
 * lobby asks before a round has booted its VM, and each round start boots a
 * fresh one, so the lookup goes to whichever state is current. */

/* Push the policy function of this name, ready for its arguments. False when
 * there is nothing to call, which is the ordinary case: a scenario defines
 * the few policies it cares about and none of the rest.
 *
 * The caller holds the VM lock, and must reach scnPolicyBool for every true
 * this answers — the function it pushed is on the stack until then. */
static bool scnPolicyBegin(ScenarioHost *h, const char *name) {
    if (h->disabled || h->L == NULL) {
        return false;
    }
    scnRawGlobal(h->L, name);
    if (lua_isfunction(h->L, -1)) {
        return true;
    }
    lua_pop(h->L, 1);
    return false;
}

/* Make the call with the nargs the caller has pushed since, and read the one
 * boolean back. A raise carries the error count toward the limit and answers
 * classic; a return puts the count back to zero, and answers classic only
 * where the script answered nil. */
static bool scnPolicyBool(ScenarioHost *h, const char *name, int nargs,
                          bool classic) {
    ScnSandboxCall saved;
    bool           answer = classic;
    int            rc;

    scnSandboxArmCall(h->L, &saved);
    rc = lua_pcall(h->L, nargs, 1, 0);
    scnSandboxDisarmCall(h->L, &saved);
    if (rc != 0) {
        scnErrorRaised(h, name, scnLuaError(h->L));
        lua_pop(h->L, 1);
        return classic;
    }
    if (!lua_isnil(h->L, -1)) {
        answer = lua_toboolean(h->L, -1) != 0;
    }
    lua_pop(h->L, 1);
    scnErrorCleared(h);
    return answer;
}

/* Make the call and leave the one answer on the stack for the caller to
 * read, which is what a policy carrying a value out needs and a predicate
 * does not. True with the answer at the top of the stack, and the caller
 * pops it; false with the stack as it was.
 *
 * A raise counts toward the limit and a nil return does not, exactly as the
 * boolean form has it — the difference is only that "said nothing" cannot be
 * spelled as a value here, so it is spelled as false.
 *
 * The count is not put back to zero for an answer that arrived: whether the
 * call succeeded is not yet known at this point, because the value still has
 * to be one the site can use. The caller clears it where it takes the
 * answer, so a script answering with something unusable every time climbs
 * toward the limit rather than resetting itself on each call. */
static bool scnPolicyAnswer(ScenarioHost *h, const char *name, int nargs) {
    ScnSandboxCall saved;
    int            rc;

    scnSandboxArmCall(h->L, &saved);
    rc = lua_pcall(h->L, nargs, 1, 0);
    scnSandboxDisarmCall(h->L, &saved);
    if (rc != 0) {
        scnErrorRaised(h, name, scnLuaError(h->L));
        lua_pop(h->L, 1);
        return false;
    }
    if (lua_isnil(h->L, -1)) {
        lua_pop(h->L, 1);
        scnErrorCleared(h);   /* no opinion is not a failure */
        return false;
    }
    return true;
}

/* An answer the site cannot use: a start that is not on the map, a loadout
 * table missing an amount, a percent outside what the damage arithmetic
 * takes. Counted the way a raise is, because a script answering with
 * something unusable every time is failing every call as surely as one that
 * raises, and the operator needs to see which. The decision itself is the
 * classic one, which the caller has already written down. */
static void scnPolicyBadAnswer(ScenarioHost *h, const char *name,
                               const char *detail) {
    scnSay(h->lastError, sizeof(h->lastError),
           "scenario: %s answered %s; the classic rule stands", name, detail);
    scnErrorCounted(h);
}

/* The largest magnitude any policy answer can mean, so the cast below is
 * always defined. Each caller bounds its own answer further. */
#define SCN_POLICY_NUMBER_MAX 1000000.0

/* The answer at the top of the stack as a whole number. False for anything
 * that is not a number and for one no answer could mean, a NaN included —
 * written as a negated in-range test so a NaN takes the false branch rather
 * than falling through both.
 *
 * Truncates toward zero rather than asking Lua for an integer: PUC 5.4 reads
 * 3.5 as no integer at all and LuaJIT reads it as 3, and a policy answers the
 * same under either build. */
static bool scnPolicyWhole(lua_State *L, long *out) {
    lua_Number v;

    if (lua_type(L, -1) != LUA_TNUMBER) {
        return false;
    }
    v = lua_tonumber(L, -1);
    if (!(v >= -SCN_POLICY_NUMBER_MAX && v <= SCN_POLICY_NUMBER_MAX)) {
        return false;
    }
    *out = (long)v;
    return true;
}

/* One amount out of a loadout table, raw so a metatable on the table the
 * script returned cannot run script code in the middle of the read. False
 * for a field that is absent or is not a stock a tank could hold. */
static bool scnLoadoutAmount(ScenarioHost *h, const char *key, BYTE *out) {
    long v  = 0;
    bool ok = false;

    scnRawField(h->L, -1, key);
    if (scnPolicyWhole(h->L, &v) && v >= 0 && v <= 255) {
        *out = (BYTE)v;
        ok   = true;
    }
    lua_pop(h->L, 1);
    return ok;
}

/* An item index as Lua counts them, or nil for a number that is no index.
 * Pills, bases and starts are 1-based in Lua and 0-based on the policy
 * surface; the build order's "no pill" sentinel is neither and must not be
 * turned into one. */
static void scnPushItemIndex(ScenarioHost *h, BYTE idx, int count) {
    if ((int)idx < count) {
        lua_pushinteger(h->L, scenarioLuaIndexToScript((int)idx));
    } else {
        lua_pushnil(h->L);
    }
}

/* A word, or nil where the site named nothing this surface spells. */
static void scnPushWord(ScenarioHost *h, const char *word) {
    if (word != NULL) {
        lua_pushstring(h->L, word);
    } else {
        lua_pushnil(h->L);
    }
}

/* May a slot be put on a team that no other slot is on? */
static bool scnAllowExtraTeams(void *ctx) {
    ScenarioHost *h     = (ScenarioHost *)ctx;
    bool          allow = true;

    if (h == NULL) {
        return true;
    }
    scnLockEnter(&h->lock);
    if (scnPolicyBegin(h, "allow_extra_teams")) {
        allow = scnPolicyBool(h, "allow_extra_teams", 0, true);
    }
    scnLockLeave(&h->lock);
    return allow;
}

/* May the round end on one side owning every base? False takes the sweep out
 * of the round's endings and leaves every other one alone. */
static bool scnAllowBaseWin(void *ctx) {
    ScenarioHost *h     = (ScenarioHost *)ctx;
    bool          allow = true;

    if (h == NULL) {
        return true;
    }
    scnLockEnter(&h->lock);
    if (scnPolicyBegin(h, "allow_base_win")) {
        allow = scnPolicyBool(h, "allow_base_win", 0, true);
    }
    scnLockLeave(&h->lock);
    return allow;
}

/* May this dead tank come back? Asked on the last tick of the death wait, so
 * a no holds the tank where it is and the question is put again next tick. */
static bool scnCanRespawn(void *ctx, BYTE player) {
    ScenarioHost *h   = (ScenarioHost *)ctx;
    bool          may = true;

    if (h == NULL) {
        return true;
    }
    scnLockEnter(&h->lock);
    if (scnPolicyBegin(h, "can_respawn")) {
        lua_pushinteger(h->L, (lua_Integer)player);
        may = scnPolicyBool(h, "can_respawn", 1, true);
    }
    scnLockLeave(&h->lock);
    return may;
}

/* May this build order go ahead? The action is the one the player asked for,
 * before the engine's substitutions, and a request code this surface has no
 * word for is not put to the script at all: an order nobody can name is one
 * a policy cannot answer about, and the classic answer is to allow it. */
static bool scnCanBuild(void *ctx, BYTE player, BYTE action, BYTE x, BYTE y,
                        BYTE idx) {
    ScenarioHost *h    = (ScenarioHost *)ctx;
    const char   *word = scenarioLuaBuildOrderWord((int)action);
    bool          may  = true;

    if (h == NULL || word == NULL) {
        return true;
    }
    scnLockEnter(&h->lock);
    if (scnPolicyBegin(h, "can_build")) {
        lua_pushinteger(h->L, (lua_Integer)player);
        lua_pushstring(h->L, word);
        lua_pushinteger(h->L, (lua_Integer)x);
        lua_pushinteger(h->L, (lua_Integer)y);
        scnPushItemIndex(h, idx, MAX_PILLS);
        may = scnPolicyBool(h, "can_build", 5, true);
    }
    scnLockLeave(&h->lock);
    return may;
}

/* May this pill or base change hands? A no leaves the objective where it is
 * and does not stop the tank. */
static bool scnCanCapture(void *ctx, BYTE kind, BYTE idx, BYTE player) {
    ScenarioHost *h    = (ScenarioHost *)ctx;
    const char   *word = scenarioLuaCaptureKindWord((int)kind);
    bool          may  = true;

    if (h == NULL || word == NULL) {
        return true;
    }
    scnLockEnter(&h->lock);
    if (scnPolicyBegin(h, "can_capture")) {
        lua_pushstring(h->L, word);
        scnPushItemIndex(h, idx,
                         (kind == CAPTURE_KIND_PILL) ? MAX_PILLS : MAX_BASES);
        lua_pushinteger(h->L, (lua_Integer)player);
        may = scnPolicyBool(h, "can_capture", 3, true);
    }
    scnLockLeave(&h->lock);
    return may;
}

/* May this newswire-worthy fact be shown to players? What the subject counts
 * as depends on the kind: a capture's is the objective, which Lua counts from
 * one, and every other kind's is a player slot or a team, which it does
 * not. */
static bool scnAnnounce(void *ctx, BYTE kind, BYTE subject, BYTE actor) {
    ScenarioHost *h    = (ScenarioHost *)ctx;
    const char   *word = scenarioLuaAnnounceKindWord((int)kind);
    bool          show = true;

    if (h == NULL || word == NULL) {
        return true;
    }
    scnLockEnter(&h->lock);
    if (scnPolicyBegin(h, "announce")) {
        lua_pushstring(h->L, word);
        if (kind == ANNOUNCE_KIND_BASE_CAPTURED) {
            scnPushItemIndex(h, subject, MAX_BASES);
        } else if (kind == ANNOUNCE_KIND_PILL_CAPTURED) {
            scnPushItemIndex(h, subject, MAX_PILLS);
        } else {
            lua_pushinteger(h->L, (lua_Integer)subject);
        }
        lua_pushinteger(h->L, (lua_Integer)actor);
        show = scnPolicyBool(h, "announce", 3, true);
    }
    scnLockLeave(&h->lock);
    return show;
}

/* May this blow destroy what it landed on? A tank's cause is a death cause
 * and a builder's or a pill's is a damage source; the two vocabularies spell
 * a shell and a mine alike, so the script reads one set of words, and a
 * cause the site could not name reaches it as nil. */
static bool scnCanDie(void *ctx, BYTE kind, BYTE index, BYTE killer,
                      BYTE cause) {
    ScenarioHost *h    = (ScenarioHost *)ctx;
    const char   *word = scenarioLuaDieKindWord((int)kind);
    bool          may  = true;

    if (h == NULL || word == NULL) {
        return true;
    }
    scnLockEnter(&h->lock);
    if (scnPolicyBegin(h, "can_die")) {
        lua_pushstring(h->L, word);
        if (kind == DIE_KIND_PILL) {
            scnPushItemIndex(h, index, MAX_PILLS);
        } else {
            lua_pushinteger(h->L, (lua_Integer)index);
        }
        lua_pushinteger(h->L, (lua_Integer)killer);
        scnPushWord(h, (kind == DIE_KIND_TANK)
                           ? scenarioLuaDeathCauseWord((int)cause)
                           : scenarioLuaDamageSourceWord((int)cause));
        may = scnPolicyBool(h, "can_die", 4, true);
    }
    scnLockLeave(&h->lock);
    return may;
}

/* Where this tank starts. The one policy that keeps a prefix, because the
 * arenas written against the prototype's surface already spell it this way.
 *
 * The script counts starts from one, as it counts pills and bases; the
 * out-parameter is the engine's own 0-based index, so the number goes out
 * through the read helper to be checked and the op helper to be handed over.
 * A number naming no live start is refused here rather than left for the
 * site's own fallback, so the operator is told which call named it. */
static bool scnChooseStart(void *ctx, BYTE player, BYTE *startIdx) {
    ScenarioHost *h     = (ScenarioHost *)ctx;
    bool          named = false;

    if (h == NULL || startIdx == NULL) {
        return false;
    }
    scnLockEnter(&h->lock);
    if (scnPolicyBegin(h, "on_choose_start")) {
        lua_pushinteger(h->L, (lua_Integer)player);
        if (scnPolicyAnswer(h, "on_choose_start", 1)) {
            ServerSimStartInfo info;
            long               n    = 0;
            BYTE               read = 0;

            if (!scnPolicyWhole(h->L, &n)) {
                scnPolicyBadAnswer(h, "on_choose_start",
                                   "with no start number");
            } else {
                read = scenarioLuaIndexToRead((lua_Integer)n);
                if (read == 0 ||
                    !serverSimGetStartInfo(h->sim, read, &info) ||
                    !info.active) {
                    scnSay(h->lastError, sizeof(h->lastError),
                           "scenario: on_choose_start named start %ld for "
                           "player %d, which is not a live start; the engine "
                           "picks", n, (int)player);
                    scnErrorCounted(h);
                } else {
                    *startIdx = scenarioLuaIndexToOp((lua_Integer)n);
                    named     = true;
                    scnErrorCleared(h);
                }
            }
            lua_pop(h->L, 1);
        }
    }
    scnLockLeave(&h->lock);
    return named;
}

/* What this spawning tank is handed. Two shapes: a word naming the rules the
 * loadout comes from, and a table of the four amounts. A word the set does
 * not hold, a table missing an amount and anything that is neither are all
 * answers the site cannot use, so the sim's own game type stands.
 *
 * All four amounts are required. A table naming some of them would leave the
 * rest at nothing, and a tank spawning with no armour because a script wrote
 * only its shells is a worse answer than being told the table is short. */
static bool scnSpawnLoadout(void *ctx, BYTE player, ScnLoadout *out) {
    ScenarioHost *h        = (ScenarioHost *)ctx;
    bool          answered = false;

    if (h == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    scnLockEnter(&h->lock);
    if (scnPolicyBegin(h, "spawn_loadout")) {
        lua_pushinteger(h->L, (lua_Integer)player);
        if (scnPolicyAnswer(h, "spawn_loadout", 1)) {
            if (lua_type(h->L, -1) == LUA_TSTRING) {
                const char *answer = lua_tostring(h->L, -1);
                int         type   = 0;
                if (scenarioLuaLoadoutFromWord(answer, &type)) {
                    out->useGameType = 1;
                    out->gameType    = (uint8_t)type;
                    answered         = true;
                    scnErrorCleared(h);
                } else {
                    scnSay(h->lastError, sizeof(h->lastError),
                           "scenario: spawn_loadout answered '%.24s', which "
                           "names no loadout; the game type stands",
                           answer);
                    scnErrorCounted(h);
                }
            } else if (lua_istable(h->L, -1)) {
                ScnLoadout want;
                memset(&want, 0, sizeof(want));
                if (scnLoadoutAmount(h, "shells", &want.shells) &&
                    scnLoadoutAmount(h, "mines",  &want.mines) &&
                    scnLoadoutAmount(h, "armour", &want.armour) &&
                    scnLoadoutAmount(h, "trees",  &want.trees)) {
                    *out     = want;
                    answered = true;
                    scnErrorCleared(h);
                } else {
                    scnPolicyBadAnswer(h, "spawn_loadout",
                                       "with a table that is not four amounts "
                                       "of 0 to 255");
                }
            } else {
                scnPolicyBadAnswer(h, "spawn_loadout",
                                   "with neither a loadout word nor a table "
                                   "of amounts");
            }
            lua_pop(h->L, 1);
        }
    }
    scnLockLeave(&h->lock);
    return answered;
}

/* What this blow is worth against this victim, as a percent. A hundred is
 * the classic amount and zero is a hit that costs no armour at all, so every
 * way of saying nothing has to come to a hundred: a missing function, a nil
 * answer, a raise and an answer outside the range below all leave the damage
 * arithmetic exactly where it was. Falling through to a zero instead would
 * make every tank in the round invulnerable and say nothing about it.
 *
 * The top of the range is a hundredfold. The site widens its own arithmetic
 * and caps the result at 255, so nothing above that changes an outcome a
 * smaller number has not already reached, and a bound is what keeps a number
 * a script names inside an int. */
#define SCN_DAMAGE_SCALE_MAX 10000

static int scnDamageScale(void *ctx, BYTE attacker, BYTE victim, BYTE cause) {
    ScenarioHost *h    = (ScenarioHost *)ctx;
    const char   *word = scenarioLuaDeathCauseWord((int)cause);
    int           pct  = 100;

    if (h == NULL || word == NULL) {
        return 100;
    }
    scnLockEnter(&h->lock);
    if (scnPolicyBegin(h, "damage_scale")) {
        lua_pushinteger(h->L, (lua_Integer)attacker);
        lua_pushinteger(h->L, (lua_Integer)victim);
        lua_pushstring(h->L, word);
        if (scnPolicyAnswer(h, "damage_scale", 3)) {
            long v = 0;
            if (scnPolicyWhole(h->L, &v) && v >= 0 &&
                v <= SCN_DAMAGE_SCALE_MAX) {
                pct = (int)v;
                scnErrorCleared(h);
            } else {
                scnPolicyBadAnswer(h, "damage_scale",
                                   "with no percent between 0 and "
                                   "a hundredfold");
            }
            lua_pop(h->L, 1);
        }
    }
    scnLockLeave(&h->lock);
    return pct;
}

/* A round the scenario takes no part in: no hooks, no policy answers, and
 * nothing owed to the first running tick. The operator has already been
 * told why.
 *
 * The table goes with them. What the host was holding described the round
 * before this one, and keeping it would have the lobby still showing that
 * scenario's name and description, the tick still walking its rectangles,
 * and the bits saying who was standing in them still set against a list
 * nothing is reading. A round without a scenario answers as a map with no
 * script beside it does. */
static void scnRoundWithoutScenario(ScenarioHost *h) {
    h->disabled     = true;
    h->startPending = false;
    scnHooksForget(h);
    /* Emptied without releasing anything, which is right for the one place
       this is reached from: a round start that has already closed the state
       its chunk could have set a timer in. There is nothing left to release
       them against, and the set must not carry them into the round. */
    scenarioLuaTimersReset(&h->timers);
    memset(&h->manifest, 0, sizeof(h->manifest));
    memset(h->inRegion, 0, sizeof(h->inRegion));
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

/* The round's own state, booted with the VM lock already held.
 *
 * A fresh VM for the round, the bytes read at attach run again in it, the
 * table read again, then the rules. The fresh VM is what gives the round a
 * fresh set of globals: nothing the last round's script left behind is
 * reachable from this one.
 *
 * The sim makes this call ahead of its start batch, before a start is picked
 * or a tank is built, so on_choose_start and spawn_loadout for the seats
 * already in the round are answered by the state that will play it and the
 * opening tanks are built under the table this file asked for. on_setup is
 * the other half of the round start and runs later, once there is a built
 * world for it to arrange.
 *
 * Nothing here goes near the disk: this runs inside the start with the sim
 * mutex held. A round whose chunk fails says so and plays classic rather
 * than carrying the previous round's table into it — and it fails before a
 * tank exists, so a round it leaves behind is a plain one from its first
 * placement onward. */
static void scnRoundBootLocked(ScenarioHost *h) {
    lua_State       *L;
    ScenarioManifest fresh;
    ScnParseReport   rep;
    char             err[SCN_ERR_LEN];
    char             why[SCN_ERR_LEN];
    char             key[SCN_VALIDATE_KEY_LEN];

    err[0] = '\0';

    /* Whatever the last round was holding, released against the state that
       holds it, before this one boots a state of its own. Every reference in
       the set belongs to h->L, and this is the last moment that is true: the
       chunk below runs in the new state and anything it sets from the top
       level belongs to that one, which is why the failure paths empty the
       set rather than releasing it. */
    scenarioLuaTimersDrop(h->L, &h->timers);

    /* An empty queue, on both channels: what the last round raised is no
       business of this one. Before the chunk as well as before the rules and
       the setup call, because the subscriber is registered at the attach and
       stays registered across a round boundary — so an op the chunk's top
       level issues raises an event straight away, and a reset behind the
       chunk would wipe it before the round's first drain ever saw it. */
    scenarioEventsReset(&h->events);

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
    /* A package's own table goes on before the chunk, exactly as it did at
       the attach: a packaged script that declares no scenario of its own
       would otherwise start every round short of one and play classic. */
    if (h->fromPackage) {
        scnPushManifestGlobal(L, &h->pkgManifest);
    }
    if (!scnRunChunk(L, h->src, h->srcLen, h->chunkName, err, sizeof(err)) ||
        !scnReadManifest(L, &fresh, h->script, err, sizeof(err), &rep)) {
        scnSay(h->lastError, sizeof(h->lastError), "%s", err);
        scnCloseVm(L);
        scnRoundWithoutScenario(h);
        return;
    }
    /* And the table the chunk left behind is held against it afterwards. The
       attach made this same test, so a round reaching it is one whose script
       computes its table rather than writing it down. */
    if (h->fromPackage &&
        !scnManifestAgrees(&h->pkgManifest, &fresh, key, sizeof(key),
                           why, sizeof(why))) {
        scnDisagreed(err, sizeof(err), why, key);
        scnSay(h->lastError, sizeof(h->lastError), "%s", err);
        scnCloseVm(L);
        scnRoundWithoutScenario(h);
        return;
    }
    if (fresh.api > SCENARIO_API_VERSION) {
        scnSay(h->lastError, sizeof(h->lastError),
               "scenario: %s asks for api %d and this server is api %d",
               h->script, fresh.api, SCENARIO_API_VERSION);
        scnCloseVm(L);
        scnRoundWithoutScenario(h);
        return;
    }

    if (h->L != NULL) {
        scnCloseVm(h->L);
    }
    h->L        = L;
    /* The whole table, read from the script's bytes again — which is how a
       region the last round defined stops existing without anything here
       having to remove it. */
    h->manifest = fresh;
    scnHooksResolve(h, L);

    /* Off for a round is off for that round alone. This one starts with the
       count at zero and the scenario running, whatever the last one did. */
    h->errors       = 0;
    h->disabled     = false;
    h->startPending = true;

    /* The roster copy the team change is measured against, and the state
       on_end watches: both are this round's, and both are read before the
       round is running, so a setup that moves a seat or ends the round is
       itself the first change either of them sees. */
    scnSeedTeams(h);
    h->lastState = serverSimGetState(h->sim);

    /* Nobody is inside anything until the first scan says so, which is the
       round's first tick. Cleared here rather than carried, because the
       regions themselves have just been read over and a bit set against the
       last round's list would name a different rectangle in this one. */
    memset(h->inRegion, 0, sizeof(h->inRegion));

    scnApplyRules(h);

    /* And, for a file that asked for it, the map's pills and bases brought up
       to the caps the table now holds. After the rules and nowhere else: this
       is the one point in a round where the scenario's own table is in force
       and no tank has been built yet, so a fill here is the world the round
       opens on. Run ahead of the rules it would fill to the numbers they are
       about to replace, and a map file cannot say "full" on its own, which is
       the whole reason the key exists.

       A scenario that did not ask changes nothing: the call is not made, and
       the map plays at the numbers its own file holds. */
    if (h->manifest.fillToCaps) {
        serverSimScenarioFillWorldToRules(h->sim);
    }
}

/* on_setup, with the VM lock already held.
 *
 * The sim makes this call at the point in the start where the world, the
 * tanks and the roster are built and the state already reads running, which
 * is the world a setup is there to arrange. It runs after the rules, so a
 * script's setup sees the table the file itself asked for, and inside the
 * setup window the start holds open, so the funnel takes the ops it issues —
 * every one but the six roster ops, which the window keeps refusing.
 *
 * A boot that failed left no hooks behind, so this is then a call that does
 * nothing and the round plays on without the scenario. */
static void scnRoundSetupLocked(ScenarioHost *h) {
    bool humans[MAX_TANKS];

    scnRosterHumans(h, humans);
    scnHookRun(h, SCN_HOOK_SETUP);
    scnRosterAudit(h, humans);
}

/* The sim's round-boot callback, ahead of the start batch. */
static void scnRoundBoot(void *ctx) {
    ScenarioHost *h = (ScenarioHost *)ctx;

    if (h == NULL) {
        return;
    }
    scnLockEnter(&h->lock);
    scnRoundBootLocked(h);
    scnLockLeave(&h->lock);
}

/* The sim's round-start callback, after the tanks. */
static void scnRoundStart(void *ctx) {
    ScenarioHost *h = (ScenarioHost *)ctx;

    if (h == NULL) {
        return;
    }
    scnLockEnter(&h->lock);
    scnRoundSetupLocked(h);
    scnLockLeave(&h->lock);
}

/* The sim's reload callback, for a lobby host asking through the command bus
 * rather than through a server console. scenarioHostReload takes the lock for
 * itself, so this is the call and nothing else. */
static bool scnReloadCb(void *ctx, char *err, size_t errLen) {
    ScenarioHost *h = (ScenarioHost *)ctx;

    if (h == NULL) {
        if (err != NULL && errLen > 0) {
            snprintf(err, errLen, "No scenario is attached to this map");
        }
        return false;
    }
    return scenarioHostReload(h, err, errLen);
}

/* ── The frontend surface ─────────────────────────────────────────── */

/* Whether an attach may load a script at all.
 *
 * On the library rather than on each caller, so the desktop client's own
 * host answers to it without a copy of the test, and so turning scripts off
 * for one more frontend later is a call rather than a patch to that
 * frontend.
 *
 * File scope rather than a member of the host or the sim, because what it
 * decides is whether a host should exist: a copy on the host would sit on
 * the thing it said not to build, and a copy on the sim would have to be
 * set on each one before the map that sim was built from was read. One
 * process runs one server, and this is that server's answer.
 *
 * Written once at startup, before a sim exists, and read from the map
 * commit thereafter. */
static bool scnEnabled = true;

void scenarioHostSetEnabled(bool enabled) {
    scnEnabled = enabled;
}

/* The narrower switch beside it: whether a map a client uploaded may bring a
 * script. One answer for the process for the same reasons the one above is,
 * and read at the same points. */
static bool scnUploadScripts = true;

void scenarioHostSetUploadScriptsEnabled(bool enabled) {
    scnUploadScripts = enabled;
}

/* One character of a path, for the comparison below: separators levelled and
 * case dropped. */
static char scnPathChar(char c) {
    if (c == '\\') {
        c = '/';
    }
    if (c >= 'A' && c <= 'Z') {
        c = (char)(c + 32);
    }
    return c;
}

/* Whether this map file sits in the server's uploads directory, and so
 * arrived from a client rather than out of the operator's own map directory.
 *
 * The path is the only signal there is. serverFinishUpload writes the bytes
 * it was sent under their final name and stamps nothing on the file, and the
 * in-memory preview path has no file at all, so a map is an upload exactly
 * when its file is the one an upload would have written.
 *
 * Separators are levelled and case is dropped, because the two strings are
 * built by different code — the uploads directory out of an operator's
 * configuration or a desktop preference, the map path out of a listing — and
 * on Windows either may spell a separator the other way. Dropping case can
 * only call a map an upload that is not one, which turns a script down; the
 * mistake the other way would run one the operator switched off. */
static bool scnMapIsUpload(const ServerSim *sim, const char *mapPath) {
    char   uploads[SCN_SCRIPT_PATH_MAX];
    size_t n;
    size_t i;

    if (sim == NULL || mapPath == NULL || mapPath[0] == '\0') {
        return false;
    }
    uploads[0] = '\0';
    serverSimGetUploadsDir(sim, uploads, sizeof(uploads));
    n = strlen(uploads);
    /* A trailing separator off the end, so "<dir>/" and "<dir>" are one
       directory. */
    while (n > 0 && (uploads[n - 1] == '/' || uploads[n - 1] == '\\')) {
        n--;
    }
    if (n == 0) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (mapPath[i] == '\0' ||
            scnPathChar(mapPath[i]) != scnPathChar(uploads[i])) {
            return false;
        }
    }
    /* A separator has to follow, so a sibling directory whose name starts
       with the uploads directory's is not inside it. */
    return mapPath[n] == '/' || mapPath[n] == '\\';
}

/* Whether a regular file is there, without opening it. The map lister asks
 * this once per entry on its way through a directory, so it is a stat and
 * not an open. A directory carrying the script's name is not a script:
 * calling one present would have the refusal below name a script a map
 * never had. */
static bool scnScriptExists(const char *path) {
    SDL_PathInfo info;

    if (!SDL_GetPathInfo(path, &info)) {
        return false;
    }
    return info.type == SDL_PATHTYPE_FILE;
}

/* How many map files have been opened to answer the map-script question, for
 * the case that holds a second listing to opening none. Written under the
 * cache's lock below, and read through scenarioHostMapScriptOpens. */
static unsigned long scnMapScriptOpens;

/* A copy of a string on the heap, which the cache below owns. Written out
 * rather than calling strdup, which is not C99. */
static char *scnStrDup(const char *src) {
    size_t n;
    char  *out;

    if (src == NULL) {
        return NULL;
    }
    n   = strlen(src) + 1;
    out = (char *)malloc(n);
    if (out != NULL) {
        memcpy(out, src, n);
    }
    return out;
}

/* How much of a map file is read when looking for a chunk in it: a whole map
 * body, plus the bytes the chunk header needs to be recognised. A map has to
 * fit inside that cap to reach a client at all, so this much of the front
 * holds every map and the header of whatever was appended after one. */
#define SCN_MAP_HEAD_BYTES (LOBBY_MAP_UPLOAD_MAX_BYTES + SCN_PACKAGE_HEADER_LEN)

/* Whether a container is appended to the map file itself. The magic answers
   the question this predicate asks; what is inside the container is the
   attach's business, so nothing here opens it. */
static bool scnMapHasChunk(const char *mapPath) {
    FILE          *f;
    unsigned long  opens;
    long           size;
    size_t         want;
    uint8_t       *buf;
    size_t         got;
    const uint8_t *chunk;
    size_t         chunkLen;
    bool           found;

    f = fopen(mapPath, "rb");
    if (f == NULL) {
        return false;
    }
    /* One more map file opened, for the case that proves the cache above
       answers without coming back here. Counted after the open succeeds, so
       what it counts is reads rather than attempts. */
    opens = scnMapScriptOpens + 1;
    scnMapScriptOpens = opens;
    /* Read the smaller of the file and the head bound: the lister asks this
       once per map in a directory, and a plain map should cost its own size
       rather than the bound's. */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    want = ((size_t)size < (size_t)SCN_MAP_HEAD_BYTES)
               ? (size_t)size
               : (size_t)SCN_MAP_HEAD_BYTES;

    buf = (uint8_t *)malloc(want);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    got = fread(buf, 1, want, f);
    fclose(f);

    found = scnPackageFindInMap(buf, got, &chunk, &chunkLen);
    free(buf);
    return found;
}

/* The answer for one map, worked out rather than remembered. */
static bool scnMapHasScriptUncached(const char *mapPath) {
    char script[SCN_SCRIPT_PATH_MAX];

    /* A loose script beside the map is the cheaper of the two tests — a stat
       against a read — so it goes first. */
    if (scnScriptPath(mapPath, script, sizeof(script)) &&
        scnScriptExists(script)) {
        return true;
    }
    return scnMapHasChunk(mapPath);
}

/* What the last answers were, so a listing need not work them out again.
 *
 * The question used to be two stats. It is a stat and a read of the map's
 * head now, because a script can be packed into the file, and it is asked
 * once per map on every listing and from the map chooser's render thread —
 * so a directory of two hundred maps opened two hundred files each time the
 * list was drawn.
 *
 * A row is keyed on everything the answer is a function of: the map file's
 * size and modify time, and whether there is a loose X.scenario.lua beside it
 * and when that was last written. Both halves are stats, which is what the
 * question cost before a script could be packed into a map at all; what the
 * cache is here to avoid is the open-and-read of the map's head. So a map
 * that is repacked, replaced or edited is worked out again, and so is one an
 * author drops a loose script next to or takes one away from — the tag on the
 * next listing is what it would have been with no cache at all.
 *
 * Round-robin replacement over a fixed set of rows, which is enough for the
 * directories a listing walks and costs nothing to keep. One cache for the
 * process, as the directory cache above is, and under a lock for the same
 * reason: the render thread and the tick thread both ask. */
#define SCN_MAP_SCRIPT_CACHE_MAX 256

typedef struct {
    char    *path;          /* owned; NULL for a row nothing is in */
    SDL_Time modified;      /* the map file's */
    Sint64   size;          /* the map file's */
    bool     looseSeen;     /* is an X.scenario.lua beside it */
    SDL_Time looseModified; /* and when it was last written; 0 for none */
    bool     scripted;
} ScnMapScriptRow;

typedef struct {
    ScnVmLock       lock;   /* m is NULL until the lister is registered */
    ScnMapScriptRow rows[SCN_MAP_SCRIPT_CACHE_MAX];
    int             next;   /* the row the next answer replaces */
} ScnMapScriptCache;

static ScnMapScriptCache scnMapScriptCache;

bool scenarioHostMapHasScript(const char *mapPath) {
    char         script[SCN_SCRIPT_PATH_MAX];
    SDL_PathInfo info;
    SDL_PathInfo looseInfo;
    bool         looseSeen;
    SDL_Time     looseModified = 0;
    int          i;
    bool         answer;

    if (mapPath == NULL || mapPath[0] == '\0') {
        return false;
    }
    /* What the tag means is that picking this map here runs its script, so a
       process with scripts off answers no for every map: the attach would
       refuse the file and the map would play plain. Without this the chooser
       marks maps Scripted on a server that will not run one. */
    if (!scnEnabled) {
        return false;
    }
    /* Nothing to key a row on, or nowhere to keep it: the question is
       answered, just not remembered. */
    if (scnMapScriptCache.lock.m == NULL ||
        !SDL_GetPathInfo(mapPath, &info) ||
        info.type != SDL_PATHTYPE_FILE) {
        return scnMapHasScriptUncached(mapPath);
    }
    /* The other half of the key, and the first half of the answer: a loose
       script beside the map outranks anything packed into it, and it is a
       stat either way. Taken before the row is looked for, so a script
       dropped next to a map the cache has already seen misses the row rather
       than being answered from it. */
    looseSeen = scnScriptPath(mapPath, script, sizeof(script)) &&
                SDL_GetPathInfo(script, &looseInfo) &&
                looseInfo.type == SDL_PATHTYPE_FILE;
    if (looseSeen) {
        looseModified = looseInfo.modify_time;
    }

    scnLockEnter(&scnMapScriptCache.lock);
    for (i = 0; i < SCN_MAP_SCRIPT_CACHE_MAX; i++) {
        ScnMapScriptRow *row = &scnMapScriptCache.rows[i];
        if (row->path != NULL && row->modified == info.modify_time &&
            row->size == info.size && row->looseSeen == looseSeen &&
            row->looseModified == looseModified &&
            strcmp(row->path, mapPath) == 0) {
            answer = row->scripted;
            scnLockLeave(&scnMapScriptCache.lock);
            return answer;
        }
    }
    /* Worked out under the lock, so the open this counts is counted once and
       two threads asking about the same new map do not both read it. The
       loose script has already been stated, so a map that has one is answered
       without the map file being opened at all. */
    answer = looseSeen ? true : scnMapHasChunk(mapPath);
    {
        ScnMapScriptRow *row = &scnMapScriptCache.rows[scnMapScriptCache.next];
        char            *copy = scnStrDup(mapPath);
        if (copy != NULL) {
            free(row->path);
            row->path          = copy;
            row->modified      = info.modify_time;
            row->size          = info.size;
            row->looseSeen     = looseSeen;
            row->looseModified = looseModified;
            row->scripted      = answer;
            scnMapScriptCache.next =
                (scnMapScriptCache.next + 1) % SCN_MAP_SCRIPT_CACHE_MAX;
        }
    }
    scnLockLeave(&scnMapScriptCache.lock);
    return answer;
}

unsigned long scenarioHostMapScriptOpens(void) {
    unsigned long n;

    if (scnMapScriptCache.lock.m == NULL) {
        return scnMapScriptOpens;
    }
    scnLockEnter(&scnMapScriptCache.lock);
    n = scnMapScriptOpens;
    scnLockLeave(&scnMapScriptCache.lock);
    return n;
}

/* The lister's question, in the shape the sim's setter takes. The context is
   the sim whose lister asked, because one part of the answer is not a fact
   about the path alone: whether the map came out of that server's uploads
   directory, which with uploaded scripts off makes it plain here however
   the file was built. Without this the chooser would tag an uploaded map
   Scripted and the attach would then refuse it. */
static bool scnMapScriptedCb(void *ctx, const char *mapPath) {
    const ServerSim *sim = (const ServerSim *)ctx;

    if (!scnUploadScripts && scnMapIsUpload(sim, mapPath)) {
        return false;
    }
    return scenarioHostMapHasScript(mapPath);
}

void scenarioHostRegisterMapScripted(ServerSim *sim) {
    /* The one place the map-script cache's lock is made, as the directory
       cache's is made where its lister is registered: registering happens
       once where a process decides whether it runs scripts at all, before
       any sim can be asked. A build where the mutex cannot be made answers
       the question every time rather than not at all. */
    if (scnMapScriptCache.lock.m == NULL) {
        (void)scnLockCreate(&scnMapScriptCache.lock);
    }
    serverSimSetScenarioMapScripted(sim, scnMapScriptedCb, sim);
}

/* The last read of the scenarios directory, kept so the next one need not be
 * made.
 *
 * What a read costs is the reason it is kept. scnDirList opens every file in
 * the directory, inflates each .scenario to reach its manifest, and boots a
 * Lua state per loose .lua to run that script's top level. A client's list
 * request and a host's pick both reach it on the thread that ticks the sim, so
 * a directory of twenty scenarios is twenty VM boots inside one tick, once per
 * request.
 *
 * Invalidation: the cache answers only when the directory is the same path as
 * last time and SDL_GetPathInfo reports the same modify time on it. A file
 * added to the directory, removed from it or renamed in it moves that time, so
 * the next read is a fresh one. A file edited in place does not — the
 * directory itself is untouched — so a scenario whose text changed keeps the
 * row it had until something else in the directory moves or the server is
 * restarted. That is the bargain the attach already makes with a script: read
 * once, and read again when something asks.
 *
 * The stamp is as fine as the kernel writes it, which is a few milliseconds on
 * an ordinary Linux filesystem rather than a nanosecond. A file that lands
 * inside the same tick as the read that kept the listing therefore leaves the
 * stamp alone and is not seen until the next change — an operator dropping a
 * scenario in cannot be that close to a read they did not make, and the file
 * after it, or the next thing to touch the directory, puts it right.
 *
 * A read that filled the caller's array is not kept. It may have stopped short
 * of the directory, and part of a directory is no answer to hand the next
 * caller with room for the rest.
 *
 * One cache for the process, as the scripts switch above is one answer for the
 * process: the directory is the server's, and a build hosting two sims would
 * have them read the same one. It is held under a lock because the read is not
 * the tick thread's alone — a client hosting in process reads it from the UI
 * thread through serverSimEnumerateScenarioDir. The lock and the rows live as
 * long as the process; there is nothing to free them at, and nothing that
 * would grow them past one directory's worth. */
typedef struct {
    ScnVmLock    lock;      /* m is NULL until the lister is registered */
    bool         valid;
    char         dir[SCN_SCRIPT_PATH_MAX];
    SDL_Time     modified;
    ScnDirEntry *rows;
    int          count;
} ScnDirCache;

static ScnDirCache scnDirCache;

static void scnDirCacheDrop(void) {
    free(scnDirCache.rows);
    scnDirCache.rows   = NULL;
    scnDirCache.count  = 0;
    scnDirCache.valid  = false;
    scnDirCache.dir[0] = '\0';
}

/* scnDirList, with the last answer kept under the rule above. */
static int scnDirListCached(const char *dir, ScnDirEntry *out, int max) {
    SDL_PathInfo info;
    int          n;

    /* Nothing to key a cache on, or nothing worth keying it to: the read still
       answers, including the refusals it makes for itself. */
    if (scnDirCache.lock.m == NULL || dir == NULL || dir[0] == '\0' ||
        out == NULL || max <= 0 || strlen(dir) >= sizeof(scnDirCache.dir) ||
        !SDL_GetPathInfo(dir, &info) ||
        info.type != SDL_PATHTYPE_DIRECTORY) {
        return scnDirList(dir, out, max);
    }

    scnLockEnter(&scnDirCache.lock);
    if (scnDirCache.valid && scnDirCache.count <= max &&
        scnDirCache.modified == info.modify_time &&
        strcmp(scnDirCache.dir, dir) == 0) {
        n = scnDirCache.count;
        if (n > 0) {
            memcpy(out, scnDirCache.rows, (size_t)n * sizeof(out[0]));
        }
        scnLockLeave(&scnDirCache.lock);
        return n;
    }

    n = scnDirList(dir, out, max);
    scnDirCacheDrop();
    if (n >= 0 && n < max) {
        bool kept = true;
        if (n > 0) {
            scnDirCache.rows =
                (ScnDirEntry *)malloc((size_t)n * sizeof(out[0]));
            kept = scnDirCache.rows != NULL;
            if (kept) {
                memcpy(scnDirCache.rows, out, (size_t)n * sizeof(out[0]));
            }
        }
        if (kept) {
            snprintf(scnDirCache.dir, sizeof(scnDirCache.dir), "%s", dir);
            /* The time as it stood before the read rather than after it: a
               file that landed while the read was running leaves the directory
               newer than this, so the next call reads again rather than
               keeping an answer that missed it. */
            scnDirCache.modified = info.modify_time;
            scnDirCache.count    = n;
            scnDirCache.valid    = true;
        }
    }
    scnLockLeave(&scnDirCache.lock);
    return n;
}

/* The directory read, in the shape the sim's setter takes. No context, for
   the same reason the map question carries none: what is in a directory is a
   fact about that directory and about nothing else. */
static int scnDirListCb(void *ctx, const char *dir, ScnDirEntry *out,
                        int max) {
    (void)ctx;
    return scnDirListCached(dir, out, max);
}

void scenarioHostRegisterScenarioLister(ServerSim *sim) {
    /* The one place the cache's lock is made. Registering happens once where a
       process decides whether it runs scripts at all, before any sim can be
       asked for a listing, so a lister call always finds the lock built. A
       second registration finds it built too. A build where the mutex cannot
       be made lists without the cache rather than not at all. */
    if (scnDirCache.lock.m == NULL) {
        (void)scnLockCreate(&scnDirCache.lock);
    }
    serverSimSetScenarioLister(sim, scnDirListCb, NULL);
}

/* ── Where the script comes from ──────────────────────────────────── */

/* A map's script and where it was found. The bytes are the caller's to free;
 * everything else is a copy, so by the time one of these comes back the map
 * file's bytes are freed and its container is closed. */
typedef struct {
    char            *src;        /* the script; the caller frees it */
    size_t           srcLen;
    bool             fromPackage;
    /* The container's manifest, for a script that came out of one. Zero for a
     * loose script, which has no manifest but the table it declares. */
    ScenarioManifest manifest;
    char             script[SCN_SCRIPT_PATH_MAX];   /* the file that was read */
    char             entry[SCN_MANIFEST_ENTRY_LEN]; /* "" for a loose script */
} ScnScriptSource;

static void scnSourceDrop(ScnScriptSource *s) {
    free(s->src);
    s->src    = NULL;
    s->srcLen = 0;
}

/* The name Lua puts at the front of every message the chunk raises. The
 * leading '@' is what makes Lua call it a file; a packaged script names the
 * entry after the map it came out of, so a line number belongs to something
 * a reader can open.
 *
 * path rather than the source's own, because the two callers want different
 * lengths of it: the console takes the whole path and a reload's single line
 * back to whoever asked has room only for the file name. */
static void scnChunkNameOf(const ScnScriptSource *s, const char *path,
                           char *out, size_t outLen) {
    if (s->entry[0] != '\0') {
        snprintf(out, outLen, "@%s:%s", path, s->entry);
    } else {
        snprintf(out, outLen, "@%s", path);
    }
}

/* The whole map file, for the container that may be appended to it. Bounded
 * by LOBBY_PACKAGE_UPLOAD_MAX_BYTES rather than by the script cap, because
 * what is being read is a map and everything the scenario ships with: the
 * map itself is a couple of dozen KB at most, and the rest is a manifest, a
 * script and whatever brain directories are in the container, all deflated.
 * The heaviest brain in this tree deflates to about a megabyte, so a map
 * shipping one comes to roughly that and the cap is room for several.
 *
 * It is the number an uploaded package is held to as well, so an operator
 * cannot build a package that plays on their own server and can never be
 * sent anywhere.
 *
 * A file that is not there leaves err empty and is not a fault, the way
 * scnReadFile treats a script that is not there. */
static bool scnReadMapBytes(const char *path, uint8_t **out, size_t *outLen,
                            char *err, size_t errLen) {
    FILE    *f;
    long     size;
    uint8_t *buf;
    size_t   got;

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
    if (size > (long)LOBBY_PACKAGE_UPLOAD_MAX_BYTES) {
        fclose(f);
        scnFmt(err, errLen,
               "scenario: %s is %ld bytes and the limit is %ld — too large to "
               "look in for a scenario", path, size,
               (long)LOBBY_PACKAGE_UPLOAD_MAX_BYTES);
        return false;
    }
    rewind(f);

    buf = (uint8_t *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        scnFmt(err, errLen, "scenario: no memory to read %s", path);
        return false;
    }
    got      = fread(buf, 1, (size_t)size, f);
    buf[got] = 0;
    fclose(f);

    *out    = buf;
    *outLen = got;
    return true;
}

/* The manifest and the script out of a container already open, whichever
 * file the container came from: a map with one appended to it, or a
 * .scenario file that is nothing else. Only the two entries named here are
 * read, and both are copied out, so the caller closes the archive and frees
 * the file's bytes as soon as this returns.
 *
 * from names the file in anything said and is what out->script is set to: an
 * entry inside a container has no path of its own. */
static bool scnScriptFromPackage(ScnPackage *p, const char *from,
                                 ScnScriptSource *out,
                                 char *err, size_t errLen) {
    uint8_t                *json      = NULL;
    size_t                  jsonLen   = 0;
    uint8_t                *luaBytes  = NULL;
    size_t                  luaLen    = 0;
    ScnManifestDoc         *doc       = NULL;
    const char             *entry;
    ScnParseReport          rep;
    char                    soft[SCN_ERR_LEN];
    /* The read's own reason, kept apart from err so the line an operator sees
       can name the file the container came out of as well. */
    char                    why[SCN_ERR_LEN];
    bool                    ok = false;

    why[0] = '\0';
    if (!scnPackageReadEntry(p, SCN_PACKAGE_MANIFEST_ENTRY,
                             SCN_PACKAGE_MANIFEST_MAX_BYTES, &json, &jsonLen,
                             why, sizeof(why))) {
        /* An entry that is simply not there gives no reason, which is the line
           below; a manifest above the cap or one that would not come back
           whole has said which, and that reason is the useful one. */
        if (why[0] != '\0') {
            scnFmt(err, errLen, "scenario: the container in %s: %s", from, why);
        } else {
            scnFmt(err, errLen, "scenario: the container in %s has no %s in it",
                   from, SCN_PACKAGE_MANIFEST_ENTRY);
        }
        goto done;
    }
    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = NULL;
    doc = scnManifestParse(json, jsonLen, &rep, err, errLen);
    if (doc == NULL) {
        goto done;
    }

    /* The same cap a loose script is read under: what is inside a container
       is hand-written Lua too, and a script above it is not one. Handed to
       the read rather than applied to what comes back, so a container
       declaring a script of any size at all is refused before the memory for
       it is asked for. */
    entry = scnManifestScriptEntry(doc);
    why[0] = '\0';
    if (!scnPackageReadEntry(p, entry, (size_t)SCN_SCRIPT_MAX_BYTES, &luaBytes,
                             &luaLen, why, sizeof(why))) {
        if (why[0] != '\0') {
            scnFmt(err, errLen, "scenario: the container in %s: %s", from, why);
        } else {
            scnFmt(err, errLen,
                   "scenario: the container in %s names its script '%s' and "
                   "has no such entry", from, entry);
        }
        goto done;
    }

    /* scnPackageReadEntry hands back a malloc'd buffer with a 0 past the
       content, which is what the script's bytes are kept as either way. */
    out->src         = (char *)luaBytes;
    out->srcLen      = luaLen;
    luaBytes         = NULL;
    out->fromPackage = true;
    out->manifest    = *scnManifestValues(doc);
    snprintf(out->script, sizeof(out->script), "%s", from);
    snprintf(out->entry, sizeof(out->entry), "%s", entry);
    ok = true;

done:
    free(luaBytes);
    free(json);
    scnManifestFree(doc);
    return ok;
}

/* The container appended to a map file, found and opened, and the manifest
 * and script read out of it above. The file's bytes are freed before this
 * returns; nothing else the container holds is read.
 *
 * False with err empty for a map that carries no container, which is the
 * ordinary case and the one every plain map takes. False with err set for a
 * map that carries one this cannot use: bad framing, no manifest, a manifest
 * that will not parse, or the script entry missing. That is a refusal an
 * operator should see, the way an unreadable loose script already is. */
static bool scnPackagedScript(const char *mapPath, ScnScriptSource *out,
                              char *err, size_t errLen) {
    uint8_t                *file     = NULL;
    size_t                  fileLen  = 0;
    const uint8_t          *chunk    = NULL;
    size_t                  chunkLen = 0;
    ScnPackage             *p        = NULL;
    bool                    ok;

    if (!scnReadMapBytes(mapPath, &file, &fileLen, err, errLen)) {
        return false;
    }
    if (!scnPackageFindInMap(file, fileLen, &chunk, &chunkLen)) {
        free(file);
        return false;            /* a plain map, and nothing to say about it */
    }
    p = scnPackageOpen(chunk, chunkLen, err, errLen);
    if (p == NULL) {
        free(file);
        return false;
    }
    ok = scnScriptFromPackage(p, mapPath, out, err, errLen);
    scnPackageClose(p);
    free(file);
    return ok;
}

/* What script this map has, and where it came from. The attach and the
 * reload both ask here, so the two cannot answer differently.
 *
 * In order:
 *
 *  1. A loose X.scenario.lua beside the map wins. That is the loop a script
 *     is written in — edit the file, reload, play — and it does not ask the
 *     author to re-pack the map between one round and the next. A map that
 *     carries a container as well says so once on the console, because the
 *     operator's own copy of the scenario is then not the one playing.
 *  2. Otherwise the container appended to the map file itself.
 *  3. Neither, and the map is a plain map: false, err empty, nothing said.
 *
 * A loose script that is there and cannot be read, and a container that is
 * there and cannot be used, are both false with the reason in err. */
static bool scnFindScript(const char *mapPath, ScnScriptSource *out,
                          char *err, size_t errLen) {
    char script[SCN_SCRIPT_PATH_MAX];

    memset(out, 0, sizeof(*out));
    if (!scnScriptPath(mapPath, script, sizeof(script))) {
        return false;
    }
    if (scnReadFile(script, &out->src, &out->srcLen, err, errLen)) {
        snprintf(out->script, sizeof(out->script), "%s", script);
        if (scnMapHasChunk(mapPath)) {
            scnSay(NULL, 0,
                   "scenario: %s is what runs, and the scenario packed into "
                   "%s is not", script, mapPath);
        }
        return true;
    }
    /* A script that is there and unusable has already said which; a script
       that is not there at all leaves err empty, and the map's own container
       is the next place to look. */
    if (err != NULL && errLen > 0 && err[0] != '\0') {
        return false;
    }
    return scnPackagedScript(mapPath, out, err, errLen);
}

/* Does name end with ext, ignoring case? The directory listing asks the same
   question of the same two extensions; this is the copy on the side that
   reads the file rather than the side that lists it. */
static bool scnHasExt(const char *name, const char *ext) {
    size_t n = strlen(name);
    size_t e = strlen(ext);

    if (n <= e) {
        return false;
    }
    return SDL_strcasecmp(name + n - e, ext) == 0;
}

/* A scenario the server offers on its own, read from the file the host
 * picked. Nothing here looks at a map: a mod is a file in the scenarios
 * directory and plays on whatever map is committed.
 *
 * A .scenario is a container and nothing else, so scnPackageOpen takes the
 * whole file — the hunt for a trailer is only for maps. A loose .lua is read
 * as a script and declares its own table, the way a script beside a map
 * does. Anything else is refused by name rather than guessed at.
 *
 * Unlike a map's own script, a file that is not there is a fault: the host
 * asked for this one by name, and the directory listing had it a moment ago.
 *
 * scnReadMapBytes is what reads a package file whole. It is named for the
 * map it was written for, but the cap it applies is the one every package is
 * held to, which is the right one here. */
static bool scnModScript(const char *path, ScnScriptSource *out,
                         char *err, size_t errLen) {
    uint8_t    *file    = NULL;
    size_t      fileLen = 0;
    ScnPackage *p;
    bool        ok;

    memset(out, 0, sizeof(*out));
    if (scnHasExt(path, SCN_SCENARIO_SCRIPT_EXT)) {
        if (!scnReadFile(path, &out->src, &out->srcLen, err, errLen)) {
            if (err != NULL && errLen > 0 && err[0] == '\0') {
                scnFmt(err, errLen, "scenario: %s is not there", path);
            }
            return false;
        }
        snprintf(out->script, sizeof(out->script), "%s", path);
        return true;
    }
    if (!scnHasExt(path, SCN_SCENARIO_PACKAGE_EXT)) {
        scnFmt(err, errLen,
               "scenario: %s is neither a %s package nor a %s script", path,
               SCN_SCENARIO_PACKAGE_EXT, SCN_SCENARIO_SCRIPT_EXT);
        return false;
    }
    if (!scnReadMapBytes(path, &file, &fileLen, err, errLen)) {
        if (err != NULL && errLen > 0 && err[0] == '\0') {
            scnFmt(err, errLen, "scenario: %s is not there", path);
        }
        return false;
    }
    p = scnPackageOpen(file, fileLen, err, errLen);
    if (p == NULL) {
        free(file);
        return false;
    }
    ok = scnScriptFromPackage(p, path, out, err, errLen);
    scnPackageClose(p);
    free(file);
    return ok;
}

/* What the lobby is told about a scenario: the seats its template asks for,
 * and the name it goes by. The attach hands both over and so does a reload,
 * so a host who edits a script's lobby block sees the change without having
 * to commit a map.
 *
 * The game type the manifest names is read by the same words a spawn op's
 * loadout takes, so one table answers both. A manifest naming none, or a
 * word that table does not hold, leaves it 0 — and 0 is no game type at all
 * rather than gameOpen, which is 1. The lobby does not keep the type it was
 * on: serverSimScenarioApplyLobbyRules moves it to gameScripted either way,
 * and gameTypeResolve reads the 0 as gameStrictTournament, so a scenario
 * that named no game plays strict. */
static void scnHandLobbyOver(ServerSim *sim, const ScenarioManifest *m,
                             LobbyScenarioSource source,
                             const char *fileName) {
    ScnLobbyTemplate t;
    int              base = 0;

    scnFillLobbyTemplate(&m->lobby, &t);
    if (m->game[0] != '\0' && scenarioLuaLoadoutFromWord(m->game, &base)) {
        t.baseGameType = (uint8_t)base;
    }
    /* The lobby goes over as data rather than as another callback: the sim
       seats and reconciles it at every point a lobby is built, and a question
       asked from inside the sim's own map change would otherwise have to
       reach back into a host that is being replaced at that moment. */
    serverSimSetScenarioLobbyTemplate(sim, &t);
    /* And what it is called, which the lobby says out loud. A scenario read
       from beside a map names that file, and a mod names its own; the path
       either was found at is the server's own business and does not go
       over. */
    serverSimSetScenarioIdentity(sim, source, m->name, fileName,
                                 m->description, m->lobby.extraTeams);
}

/* Everything an attach does once the script's bytes are in hand, whichever
 * file they came out of. The two entry points below differ only in where
 * they look; from here down a mod and a map's own scenario are the same
 * thing.
 *
 * Takes over from->src either way: a path out of here frees it.
 *
 * mapPath is the map the script was found for, and "" for a mod, which was
 * not found for any map. source is what the lobby is told, and is the one
 * other thing the two entry points disagree about. */
static ScenarioHost *scnAttachFrom(ServerSim *sim, ScnScriptSource *from,
                                   const char *mapPath,
                                   LobbyScenarioSource source,
                                   char *err, size_t errLen) {
    char             chunkName[SCN_SCRIPT_PATH_MAX + 2];
    char             soft[SCN_ERR_LEN];
    char             why[SCN_ERR_LEN];
    char             key[SCN_VALIDATE_KEY_LEN];
    ScenarioManifest m;
    ScnParseReport   rep;
    ScenarioHost    *h;
    lua_State       *L;

    scnChunkNameOf(from, from->script, chunkName, sizeof(chunkName));

    /* The host is built before the state is, because the game table's rows
       read through a struct on it and the table goes on before the chunk
       runs. Nothing can reach either of them yet: the registrations at the
       bottom are what make this host reachable, and a path out of here
       before them takes the whole thing back down again. */
    h = (ScenarioHost *)calloc(1, sizeof(*h));
    if (h == NULL) {
        scnFmt(err, errLen, "scenario: out of memory");
        scnSourceDrop(from);
        return NULL;
    }
    if (!scnLockCreate(&h->lock)) {
        scnFmt(err, errLen, "scenario: no mutex for the Lua state");
        free(h);
        scnSourceDrop(from);
        return NULL;
    }
    h->sim           = sim;
    h->lua.sim       = sim;
    h->lua.manifest  = &h->manifest;
    h->lua.timers    = &h->timers;
    /* Every state this host boots runs the scenario rather than checking it;
       the reload is the one that checks. */
    h->lua.checkOnly = false;
    /* calloc left every reference at 0, which is a reference to something.
       An empty set is LUA_NOREF throughout. */
    scenarioLuaTimersReset(&h->timers);
    /* The hooks belong to a round's VM, and this is the one the table is
       read in. The first round start resolves them. */
    scnHooksForget(h);
    /* Neither of these is the zero calloc left: no subscriber is -1. */
    h->sub        = SUBSCRIBER_HANDLE_INVALID;
    snprintf(h->script, sizeof(h->script), "%s", from->script);
    snprintf(h->mapPath, sizeof(h->mapPath), "%s",
             mapPath != NULL ? mapPath : "");
    h->source = source;
    snprintf(h->chunkName, sizeof(h->chunkName), "%s", chunkName);

    /* The one VM entry that takes no lock, because there is nothing yet to
       take one on: until the host below is registered no other thread can
       reach either. */
    L = scnBootVm(h);
    if (L == NULL) {
        scnFmt(err, errLen, "scenario: no memory for a Lua state");
        scnLockDestroy(&h->lock);
        free(h);
        scnSourceDrop(from);
        return NULL;
    }

    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = NULL;
    /* A package's manifest is the truth about what it is, and it goes on as
       the scenario global before the chunk runs. A script that came out of
       one may therefore declare no table of its own; a loose script gets
       nothing pushed and declares its own, as it always has. */
    if (from->fromPackage) {
        scnPushManifestGlobal(L, &from->manifest);
    }
    if (!scnRunChunk(L, from->src, from->srcLen, chunkName, err, errLen) ||
        !scnReadManifest(L, &m, from->script, err, errLen, &rep)) {
        scnCloseVm(L);
        scnLockDestroy(&h->lock);
        free(h);
        scnSourceDrop(from);
        return NULL;
    }
    /* And the table the chunk left behind is held against the manifest. A
       script that assigned nothing reads back what was pushed and passes with
       nothing to do; one that restated the table and said something else is
       refused here, by key, rather than playing a scenario the package does
       not describe. */
    if (from->fromPackage &&
        !scnManifestAgrees(&from->manifest, &m, key, sizeof(key), why,
                           sizeof(why))) {
        scnDisagreed(err, errLen, why, key);
        scnCloseVm(L);
        scnLockDestroy(&h->lock);
        free(h);
        scnSourceDrop(from);
        return NULL;
    }
    if (m.api > SCENARIO_API_VERSION) {
        scnFmt(err, errLen,
               "scenario: %s asks for api %d and this server is api %d — "
               "the server is too old to run it",
               from->script, m.api, SCENARIO_API_VERSION);
        scnCloseVm(L);
        scnLockDestroy(&h->lock);
        free(h);
        scnSourceDrop(from);
        return NULL;
    }
    /* A scenario that says it is bound was written for its own map, and the
       tags, regions and entity indices it uses are that map's. Played over
       another map they name items that are not there, so it is refused as a
       mod however it was asked for. The lobby pick refuses a bound entry
       before it ever reaches an attach and is what a host is told; this is
       the one place every other way in — a console, a startup flag, a test —
       meets the same answer. A map's own scenario is untouched: it is bound
       precisely because it belongs to the map it arrived with. */
    if (source == lobbyScenarioMod && m.bound) {
        scnFmt(err, errLen,
               "scenario: %s is bound to its own map, so it cannot be played "
               "over another one", from->script);
        scnCloseVm(L);
        scnLockDestroy(&h->lock);
        free(h);
        scnSourceDrop(from);
        return NULL;
    }

    h->L           = L;
    h->manifest    = m;
    h->src         = from->src;
    h->srcLen      = from->srcLen;
    h->fromPackage = from->fromPackage;
    h->pkgManifest = from->manifest;
    h->active      = true;
    snprintf(h->lastError, sizeof(h->lastError), "%s", soft);

    /* Registered here rather than at the first round start: the lobby asks
       two of these — the extra team and the player cap — before a round
       exists, and every row answers classic on its own while the host has no
       round's VM to ask.

       maxPlayers stays NULL. It is the one row of the policy table with no
       Lua function behind it: the cap is a number in the manifest, and the
       lobby template carries it to serverSimFindFreeSlot, which applies it.
       A pointer here would be a second path to the same answer. */
    h->policy.allowExtraTeams = scnAllowExtraTeams;
    h->policy.allowBaseWin    = scnAllowBaseWin;
    h->policy.canRespawn      = scnCanRespawn;
    h->policy.canBuild        = scnCanBuild;
    h->policy.canCapture      = scnCanCapture;
    h->policy.announce        = scnAnnounce;
    h->policy.canDie          = scnCanDie;
    h->policy.chooseStart     = scnChooseStart;
    h->policy.spawnLoadout    = scnSpawnLoadout;
    h->policy.damageScale     = scnDamageScale;
    h->policy.ctx             = h;

    serverSimSetScenarioRoundBoot(sim, scnRoundBoot, h);
    serverSimSetScenarioRoundStart(sim, scnRoundStart, h);
    serverSimSetScenarioTick(sim, scnTick, h);
    /* And what a lobby host's reload request runs, so a player editing a
       script has the loop the server console already has. */
    serverSimSetScenarioReload(sim, scnReloadCb, h);
    serverSimSetScenarioPolicy(sim, &h->policy);
    scnHandLobbyOver(sim, &m, source, scnFileNameOf(h->script));

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
               h->script);
    }
    return h;
}

ScenarioHost *scenarioHostAttach(ServerSim *sim, const char *mapPath,
                                 char *err, size_t errLen) {
    char            script[SCN_SCRIPT_PATH_MAX];
    ScnScriptSource from;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (sim == NULL || mapPath == NULL) {
        return NULL;
    }
    if (!scnScriptPath(mapPath, script, sizeof(script))) {
        return NULL;
    }
    /* Off, and the file is not read, parsed or run. The name above is string
       work on the map path and touches no disk; the one question asked here
       is whether there is a file, which is what tells a map that lost a
       script apart from a map that never had one. */
    if (!scnEnabled) {
        if (scnScriptExists(script)) {
            scnFmt(err, errLen,
                   "scenario: scripts are off; %s was not loaded", script);
        }
        return NULL;
    }
    /* A map out of this server's uploads directory, with scripts in uploaded
       maps off. Neither the container inside the file nor a loose script
       beside it is loaded. Said on the console rather than only in err,
       because the operator turned this off deliberately and the file that
       was turned down is the thing they want named; said only for a map that
       had something to run, the way the switch above is, so an uploaded
       plain map is as quiet as any other. The question reads the file's head
       and stats the name beside it, and parses neither. */
    if (!scnUploadScripts && scnMapIsUpload(sim, mapPath)) {
        if (scenarioHostMapHasScript(mapPath)) {
            scnSay(err, errLen,
                   "scenario: %s came from an upload and scripts in uploaded "
                   "maps are off; its script was not loaded", mapPath);
        }
        return NULL;
    }
    /* The one read of the script, from beside the map or from inside it. No
       script is the ordinary case: the map plays as a plain map and the
       operator is told nothing, because there is nothing to tell. A script
       that is there and cannot be used sets err and is reported. */
    if (!scnFindScript(mapPath, &from, err, errLen)) {
        return NULL;
    }
    /* An uploaded map's own scenario, about to run. Said whenever it happens
       and not only when something is wrong: this is the one case where the
       script a round plays by came from a client rather than out of the
       operator's own map directory, and an operator who would rather it did
       not has scenarioHostSetUploadScriptsEnabled to say so. */
    if (from.fromPackage && scnMapIsUpload(sim, mapPath)) {
        scnSay(NULL, 0,
               "scenario: %s came from an upload and carries a scenario; that "
               "is what runs", mapPath);
    }
    return scnAttachFrom(sim, &from, mapPath, lobbyScenarioMap, err, errLen);
}

ScenarioHost *scenarioHostAttachMod(ServerSim *sim, const char *dir,
                                    const char *file,
                                    char *err, size_t errLen) {
    char            path[SCN_SCRIPT_PATH_MAX];
    ScnScriptSource from;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (sim == NULL || dir == NULL || file == NULL || file[0] == '\0') {
        return NULL;
    }
    /* Off, and the file is not read either. Said rather than silent, unlike
       a map with no script: a host who picked this one is owed the reason it
       is not playing. */
    if (!scnEnabled) {
        scnFmt(err, errLen, "scenario: scripts are off; %s was not loaded",
               file);
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (!scnModScript(path, &from, err, errLen)) {
        return NULL;
    }
    /* No map path: this scenario was not found for any map and plays on
       whichever one is committed. */
    return scnAttachFrom(sim, &from, "", lobbyScenarioMod, err, errLen);
}

bool scenarioHostReload(ScenarioHost *h, char *err, size_t errLen) {
    char             soft[SCN_ERR_LEN];
    char             why[SCN_ERR_LEN];
    char             key[SCN_VALIDATE_KEY_LEN];
    char             chunkName[SCN_SCRIPT_PATH_MAX + 2];
    const char      *name;
    ScnScriptSource  from;
    ScenarioManifest m;
    ScnParseReport   rep;
    ScnLuaCtx        check;
    lua_State       *L;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (h == NULL) {
        return false;
    }
    /* Read from wherever this scenario came from. A mod is its own file in
       the scenarios directory and is re-read straight from it; for a map's
       own scenario, where the script comes from is asked again rather than
       remembered, because a loose script dropped beside a packed map is
       meant to take over at the next reload and one deleted from beside it
       is meant to hand the map back to its own container. */
    if (h->source == lobbyScenarioMod) {
        if (!scnModScript(h->script, &from, err, errLen)) {
            return false;
        }
    } else if (!scnUploadScripts && scnMapIsUpload(h->sim, h->mapPath)) {
        /* The same refusal the attach makes, because a reload reads the file
           again: without it a scenario the switch turned down at the commit
           would come back the moment anyone asked for a reload. */
        scnFmt(err, errLen,
               "scenario: %s came from an upload and scripts in uploaded maps "
               "are off", h->mapPath);
        return false;
    } else if (!scnFindScript(h->mapPath, &from, err, errLen)) {
        /* A map with nothing to read leaves err empty, because at an attach
           that is the ordinary case rather than a fault. Asked for by name it
           is a fault, so it is stated here. */
        if (err != NULL && errLen > 0 && err[0] == '\0') {
            scnFmt(err, errLen, "scenario: %s is no longer there",
                   scnFileNameOf(h->script));
        }
        return false;
    }
    name = scnFileNameOf(from.script);
    scnChunkNameOf(&from, name, chunkName, sizeof(chunkName));

    /* Everything below happens in a Lua state of its own, and nothing the
       host holds is touched until all of it has passed. A file with an
       error in it therefore changes nothing at all: the round that is
       running keeps its table, and so does the round after it.

       The state carries the game table like any other, so a script that
       reads the game at the top level loads here exactly as it loads at a
       round start rather than being turned down for a table this one
       state lacked.

       It carries a context of its own, though, and that context says it is
       checking. Every row that writes refuses while it does, so a file whose
       top level ends a round, seats a bot or sends the players a line is
       read for whether it loads and applies none of it. The two rows that
       write what the host is holding are turned away the same way: the
       manifest a region would be defined in is the local below, which is
       read over a few lines later, and a state with no timer set refuses a
       timer rather than leaving the round holding a function in a state this
       call is about to close. All of it is the one thing a reload promises —
       that nothing changes until the next round start.

       The rows that read are left working: what the check is for is finding
       out whether the file loads, and a file that reads the sim as it loads
       has to be able to. The console command holds the server mutex across
       the whole reload, so those reads see one picture of the round. */
    memset(&m, 0, sizeof(m));   /* the chunk can read it before it is read */
    check.sim       = h->sim;
    check.manifest  = &m;
    check.timers    = NULL;
    check.checkOnly = true;
    L = scnBootVmWith(&check);
    if (L == NULL) {
        scnFmt(err, errLen, "scenario: no memory for a Lua state");
        scnSourceDrop(&from);
        return false;
    }

    soft[0]     = '\0';
    rep.soft    = soft;
    rep.softLen = sizeof(soft);
    rep.sink    = NULL;
    /* The checking state gets the package's table the same way a round's
       state does, so a packaged script that declares none is checked as it
       will be run rather than turned down for a table it never writes. */
    if (from.fromPackage) {
        scnPushManifestGlobal(L, &from.manifest);
    }
    if (!scnRunChunk(L, from.src, from.srcLen, chunkName, err, errLen) ||
        !scnReadManifest(L, &m, name, err, errLen, &rep)) {
        scnCloseVm(L);
        scnSourceDrop(&from);
        return false;
    }
    if (from.fromPackage &&
        !scnManifestAgrees(&from.manifest, &m, key, sizeof(key), why,
                           sizeof(why))) {
        scnDisagreed(err, errLen, why, key);
        scnCloseVm(L);
        scnSourceDrop(&from);
        return false;
    }
    if (m.api > SCENARIO_API_VERSION) {
        scnFmt(err, errLen,
               "scenario: %s asks for api %d and this server is api %d — "
               "the server is too old to run it",
               name, m.api, SCENARIO_API_VERSION);
        scnCloseVm(L);
        scnSourceDrop(&from);
        return false;
    }
    /* The refusal the attach makes, made again: a mod is played over whatever
       map is committed, and a mod edited on disk to say it is bound was
       written for a map of its own. Without this a reload would seat over any
       map a table the attach would have turned away. */
    if (h->source == lobbyScenarioMod && m.bound) {
        scnFmt(err, errLen,
               "scenario: %s is bound to its own map, so it cannot be played "
               "over another one", name);
        scnCloseVm(L);
        scnSourceDrop(&from);
        return false;
    }
    scnCloseVm(L);

    /* The bytes are replaced, and with them where they came from. The VM and
       the table the round is running on stay as they are; the next round
       start builds both again from what is now here.

       Where they came from goes too, because the answer may have changed
       since the attach: a loose script dropped beside a packed map is read
       from a different file, runs under a different chunk name and stops
       being held against the container's manifest.

       Under the lock, because the round start reads all of this under it and
       a reload arrives on whichever thread the operator's command came in
       on. Only the swap: the reading and the checking above happen on a Lua
       state of their own, and holding the lock across a disk read would stop
       a tick for as long as the file took. */
    scnLockEnter(&h->lock);
    free(h->src);
    h->src         = from.src;
    h->srcLen      = from.srcLen;
    h->fromPackage = from.fromPackage;
    h->pkgManifest = from.manifest;
    snprintf(h->script, sizeof(h->script), "%s", from.script);
    scnChunkNameOf(&from, from.script, h->chunkName, sizeof(h->chunkName));
    snprintf(h->lastError, sizeof(h->lastError), "%s", soft);
    scnLockLeave(&h->lock);

    /* The seats as well as the rules. Until now a reload swapped the bytes
       the next round boots from and stopped there, so a host who edited the
       script's lobby block saw the same lobby until a map was committed. The
       template and the name go over again from the manifest this reload just
       checked, and the lobby is seated from them, so what was edited is what
       the lobby shows.

       h->manifest is left alone on purpose: the round that is running keeps
       the table it started with, and the round start reads the new one from
       the bytes swapped above. What changes here is what the lobby is told,
       which is not the running round's to keep. */
    scnHandLobbyOver(h->sim, &m, h->source, scnFileNameOf(from.script));
    /* Seated only from the lobby. The template is data either way and goes
       over above whatever the server is doing, but building the seats and
       moving the game type onto a round already running would change a game
       in progress — and the round that is running keeps what it started
       with, which is the whole of what a reload promises. A reload from the
       server console mid-round therefore lands its seats at the next return
       to the lobby, where the reconcile reads the template this just set. */
    if (serverSimGetState(h->sim) == serverStateLobby) {
        serverSimScenarioSeatLobby(h->sim);
        serverSimScenarioApplyLobbyRules(h->sim);
        serverSimPublishLobbySettings(h->sim);
    }
    return true;
}

/* Which scenario this lobby plays, decided in one place because more than
 * one thing changes the answer: a map commit and the host picking a mod both
 * come through here.
 *
 * In order:
 *
 *  1. A mod the host selected. It plays on whatever map is committed, so a
 *     bound map's own scenario gives way to it. The host who picked it is
 *     the one who decided, and the settings event says a mod is what is
 *     playing, so nobody has to guess why the map's own is not.
 *  2. Otherwise the committed map's own, from beside the file or inside it.
 *  3. Otherwise none, and the map plays plainly.
 *
 * Selecting none is therefore not the same as playing nothing: it is rule 2,
 * so a bound map picks its own scenario back up and a plain map is left
 * plain. That is the host's call to make either way.
 *
 * A mod that will not load plays nothing rather than falling back to the
 * map's own. The host asked for one scenario, and a lobby that quietly ran a
 * different one would be worse than a lobby that runs none; the reason is
 * said on the console.
 *
 * The caller's own pointer is the context because the host it names is the
 * one being replaced here. */
static void scnDecideScenario(ServerSim *sim, ScenarioHost **slot,
                              const char *mapPath) {
    const char *mod;
    char        err[512];

    if (slot == NULL) return;
    scenarioHostDetach(*slot);
    *slot = NULL;
    if (sim == NULL) return;

    err[0] = '\0';
    mod = serverSimGetSelectedScenario(sim);
    if (mod != NULL && mod[0] != '\0') {
        *slot = scenarioHostAttachMod(sim, serverSimGetScenarioDir(sim), mod,
                                      err, sizeof(err));
    } else if (mapPath != NULL && mapPath[0] != '\0') {
        /* A map that came from bytes rather than a file has nothing beside
           it to read, so with no mod selected it is played plainly. */
        *slot = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    }
    /* Said rather than returned: a map commit has nobody to answer, and
       without this line an operator rotating through a directory would have
       no way of telling which rounds ran a script. The attach names both the
       scenario and the file it came from; a refusal and a file that cannot
       be used arrive in err already said. A map with no script beside it and
       no mod selected sets neither and stays quiet, which is what keeps a
       rotation over plain maps as silent as it was. */
    if (*slot != NULL) {
        scnSay(NULL, 0, "scenario: %s loaded from %s",
               scenarioHostName(*slot), scenarioHostScriptPath(*slot));
    } else if (err[0] != '\0') {
        scnSay(NULL, 0, "%s", err);
    }
}

/* The sim's map change, answered — and the host's scenario pick, which
 * arrives the same way because the question it asks is the same one. */
static void scnMapChanged(void *ctx, ServerSim *sim, const char *mapPath) {
    scnDecideScenario(sim, (ScenarioHost **)ctx, mapPath);
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
        serverSimSetScenarioRoundBoot(h->sim, NULL, NULL);
        serverSimSetScenarioRoundStart(h->sim, NULL, NULL);
        serverSimSetScenarioTick(h->sim, NULL, NULL);
        serverSimSetScenarioReload(h->sim, NULL, NULL);
        serverSimSetScenarioPolicy(h->sim, NULL);
        /* The lobby goes with them. What is already seated is left where it
           is — emptying the roster is the map change's business, and a
           detach at shutdown has no lobby left to tidy. The map-change
           registration is not cleared here: it belongs to whoever owns this
           host, not to the host, and it is the thing that will attach the
           next one. */
        serverSimSetScenarioLobbyTemplate(h->sim, NULL);
        /* And what it was called, so a lobby left without a scenario says
           it has none. */
        serverSimSetScenarioIdentity(h->sim, lobbyScenarioNone, NULL, NULL,
                                     NULL, false);
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
        scnCloseVm(h->L);
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

const char *scenarioHostScriptPath(const ScenarioHost *h) {
    return (h != NULL) ? h->script : "";
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
