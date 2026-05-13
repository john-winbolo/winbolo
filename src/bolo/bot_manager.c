/*
 * Copyright (c) 1998-2008 John Morrison.
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

/*********************************************************
 *Name:          Bot Manager
 *Filename:      bot_manager.c
 *Author:        John Morrison
 *Purpose:
 *  Manages AI bot players. Each bot has its own ClientSim
 *  connected via a passive transport_local, with a
 *  LuaBrainInstance driving it. Replaces server_brains.c.
 *********************************************************/

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <SDL3/SDL.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"
#include "players.h"
#include "allience.h"
#include "mines.h"
#include "client_sim.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "transport.h"
#include "client_snapshot.h"
#include "screenbrainmap.h"
#include "input_packet.h"
#include "bot_manager.h"
#include "bot_worker_pool.h"
#include "brain_worldsim.h"
#include <lua.h>
#include <lauxlib.h>   /* luaL_loadstring for botManagerExecLua */
#include "transport_udp.h"
#include "../common/wb_log.h"
#include "../server/server_sim.h"
#include "../gui/sdl3/luabrainshandler.h"

/* View size for brain map updates — 15x15 centered on tank */
#define BOT_VIEW_HALF 7

typedef struct {
    ClientSim      *cs;
    Transport       transport;
    LuaBrainInstance brain;
    /* Filesystem path to the brain script for this bot. Captured at
     * botManagerAddBot time so botManagerGetBotInfo can surface the
     * brain identity without confusing it with the player display
     * name (multiple bots commonly share one brain script). */
    char            brainPath[256];
    BYTE            playerNum;
    bool            active;
    aiType          ai;
    /* Wall-clock duration of this bot's most recent brain.think call,
     * in milliseconds. Updated every botManagerTick. Surfaced via
     * botManagerGetLastThinkMs so HUDs / perf graphs can read it. */
    double          lastThinkMs;
    /* Number of ticks this bot's think exceeded targetMs * 1.5. Counts
     * every overrun; see lastOverrunWarnTick for the rate-limited log. */
    Uint32          overrunCount;
    /* Last tick at which a budget-overrun warning was logged for this
     * bot. Limits the warning to at most one per ~50 ticks. */
    Uint32          lastOverrunWarnTick;
    /* Set by the count hook when SDL_GetPerformanceCounter() passes
     * thinkDeadlineCounter; polled by C bindings (cpf/wsim/NA) at
     * their outer-loop checkpoints so they can return a partial result
     * cheaply instead of running to completion past the budget. The
     * hook also raises luaL_error("tick_budget_exceeded") in the same
     * step, so once C returns to Lua the longjmp unwinds to the
     * brainCoreCallThink pcall within ~1000 instructions. Reset to 0
     * each tick before dispatch.  */
    SDL_AtomicInt   abort_flag;
    /* Absolute SDL_GetPerformanceCounter() value at which this bot's
     * think budget elapses. Set per-tick by runBotThinkJob from
     * t0 + s_lastTargetMs * SDL_GetPerformanceFrequency() / 1000 so
     * the count hook does one cheap compare against `now`. */
    Uint64          thinkDeadlineCounter;
    /* One-tick edge signal set by the producer when the previous tick
     * was aborted by the budget hook. Surfaced to the brain via
     * brain.wasKilled; reset to false immediately after being passed,
     * so a brain only sees it for the single tick that follows an
     * abort. */
    bool            wasKilled;
    /* The bot's lua_State stores BotContext * via lua_getextraspace(L).
     * Set up exactly once during botManagerAddBot after the brain
     * instance is created; the count hook and every cpf_/wsim_/NA
     * binding cast lua_getextraspace(L) back to BotContext * to read
     * abort_flag and thinkDeadlineCounter without per-binding plumbing. */
    SubscriberHandle controlSub;
} BotContext;

static void botDeliverControl(void *ctx, const ControlEvent *evt) {
    clientSimApplyControl((ClientSim *)ctx, evt);
}

static BotContext bots[MAX_TANKS];

/* Default debug mode for newly-created bots. Hosts override via
 * botManagerSetDefaultDebugMode (BrainTest sets true at startup;
 * release game leaves false). */
static bool s_default_debug_mode = false;

/* Runtime debug mode tracker. Flipped by botManagerToggleAllBrainDebugMode
 * so the toggle alternates correctly across calls. */
static bool s_brain_debug_mode   = false;
static int numBots = 0;

/* Total concurrent brain-tick runners including the producer thread.
 * Saved by botManagerInit after validation; read by the budget formula
 * to scale per-bot time when more bots than runners are active. */
static int g_threadsConfig = 1;

/* Pending thread-count request from the BrainTest panel. -1 = no
 * pending change. Applied at the top of botManagerTick (between ticks),
 * never mid-dispatch. Single-producer (main thread) so a plain int
 * is fine — no atomics needed. */
static int s_pending_threads = -1;

void botManagerRequestThreads(int total_runners) {
    if (total_runners < 1) total_runners = 1;
    int cores = SDL_GetNumLogicalCPUCores();
    if (total_runners > cores)    total_runners = cores;
    if (total_runners > MAX_TANKS) total_runners = MAX_TANKS;
    s_pending_threads = total_runners;
}

int botManagerGetThreads(void) { return g_threadsConfig; }
int botManagerGetPendingThreads(void) { return s_pending_threads; }

/* Apply any pending pool-resize request. Called from botManagerTick
 * before any dispatch happens, so the resize lands in the gap between
 * ticks. */
static void applyPendingThreadResize(void) {
    int want = s_pending_threads;
    if (want < 0 || want == g_threadsConfig) {
        s_pending_threads = -1;
        return;
    }
    botWorkerPoolDestroy();
    int workers = want - 1;
    if (workers > 0 && !botWorkerPoolCreate(workers)) {
        WB_LOG_WARN(WB_LOG_CAT_PLATFORM,
                    "botManager: pool create(%d) failed; staying serial",
                    workers);
        g_threadsConfig = 1;
    } else {
        g_threadsConfig = want;
    }
    s_pending_threads = -1;
}

/* Diagnostic toggle for the per-tick brain budget kill path. When 1
 * (default) the count hook is installed for every brain.think(), fires
 * tick_budget_exceeded on overrun, and C bindings poll the abort flag
 * for partial returns. When 0 the hook is never installed, abort_flag
 * is never stamped, and brain.think runs to completion regardless of
 * how long it takes — the producer's post-tick overrun telemetry
 * (overrunCount, rate-limited slow-tick warning) still fires, so
 * operators still see overruns, just without the truncation. */
#ifndef BRAIN_BUDGET_ENFORCE
#define BRAIN_BUDGET_ENFORCE 1
#endif

/* EWMA of the serial-stage cost (ms) of recent ticks. Seeded by the
 * first call to botManagerRecordSerialMs to avoid biasing toward zero. */
static double s_serialMsEwma = 0.0;
/* Last serial-stage cost (ms). Companion to s_serialMsEwma so the
 * server `info` summary can show last + EWMA together. */
static double s_lastSerialMs = 0.0;

/* Wall-clock cost of the most recent dispatched brain-think stage, in
 * milliseconds. Set per-tick at the end of botManagerTick; reserved
 * for future refinements (e.g. computing the EWMA from the full
 * serverInstanceTick instead of approximating it via kReservedSimMs). */
static double s_lastBrainPhaseMs = 0.0;
/* EWMA of the brain-think stage cost (ms). Display-only — the budget
 * formula reads s_serialMsEwma, not this. */
static double s_brainPhaseMsEwma = 0.0;

/* Per-bot brain-tick budget computed for the current tick. Set per-tick
 * before dispatch so the input-send overrun check and any future server
 * info command can read the same number the brains were given. */
static double s_lastTargetMs = 0.0;

/* EWMA smoothing factor — ~10-tick (200 ms) window. */
static const double kAlpha = 0.1;
/* Server tick target: 50 Hz → 20 ms. */
static const double kTickMs = 1000.0 / 50.0;
/* Conservative reserve for serverSimTick × 2 plus mutex acquire/release.
 * The EWMA only measures the serial stages inside botManagerTick itself,
 * so this fills in for the rest of serverInstanceTick. Refine via direct
 * measurement from server_lifecycle.c if profiling shows a mismatch. */
static const double kReservedSimMs = 6.0;
/* Headroom subtracted from the per-tick budget. */
static const double kSafetyMs = 2.0;

static void (*g_preThinkHook)(int playerNum) = NULL;

void botManagerSetPreThinkHook(void (*hook)(int playerNum)) {
    g_preThinkHook = hook;
}

/* Recover the BotContext from a brain's lua_State. NULL when the
 * lua_State has no associated bot (e.g. the singleton GUI brain). */
static BotContext *botFromLua(lua_State *L) {
    if (L == NULL) return NULL;
    void *slot = lua_getextraspace(L);
    return slot ? *(BotContext **)slot : NULL;
}

/* Lua count hook installed for the duration of brain.think(). Fires
 * every ~1000 VM instructions; checks the per-bot deadline and, on
 * overrun, sets abort_flag and longjmps out via luaL_error. */
static void brainBudgetHook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    BotContext *bot = botFromLua(L);
    if (bot == NULL) return;
    Uint64 now = SDL_GetPerformanceCounter();
    if (now >= bot->thinkDeadlineCounter) {
        SDL_SetAtomicInt(&bot->abort_flag, 1);
        /* Raise. Longjmps unwind to the lua_pcall in brainCoreCallThink,
         * which detects the suffix and reports "killed" rather than the
         * real-error removal path. */
        luaL_error(L, "tick_budget_exceeded");
    }
}

bool botManagerShouldAbort(struct lua_State *L) {
    BotContext *bot = botFromLua((lua_State *)L);
    if (bot == NULL) return false;
    return SDL_GetAtomicInt(&bot->abort_flag) != 0;
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* Updates the bot's brain map for the visible area around the tank.
 * Matches server_brains.c behavior (option b from plan). */
/* Updates the bot's brain map (fog-of-war) from the server's authoritative
 * map.  The bot's ClientSim map does not receive terrain change events
 * (boat placements, building destruction, etc.), so we must read from
 * the ServerSim's map to keep the brainMap current.
 *
 * For aiFull bots: refresh the entire 256x256 map every tick (~0.1ms).
 * For other AI modes: refresh only the visible rect around the tank. */
static void botUpdateBrainMap(BotContext *bot, ServerSim *sim) {
    BYTE tx, ty;
    BYTE left, right, top, bottom;
    int x, y;

    if (MY_TANK(bot->cs) == NULL) {
        return;
    }

    tx = tankGetMX(&MY_TANK(bot->cs));
    ty = tankGetMY(&MY_TANK(bot->cs));

    GameSim *gs = serverSimGetGameSim(sim);

    if (bot->ai == aiFull) {
        /* aiFull: refresh full map every tick from server */
        screenBrainMapFillFromMap(bot->cs, &gs->mp, &gs->mns);
        return;
    }

    left   = (tx > BOT_VIEW_HALF) ? tx - BOT_VIEW_HALF : 0;
    top    = (ty > BOT_VIEW_HALF) ? ty - BOT_VIEW_HALF : 0;
    right  = (tx + BOT_VIEW_HALF < 255) ? tx + BOT_VIEW_HALF : 255;
    bottom = (ty + BOT_VIEW_HALF < 255) ? ty + BOT_VIEW_HALF : 255;

    /* Update only the visible rect from server map */
    for (y = top; ; y++) {
        for (x = left; ; x++) {
            screenBrainMapSetPos((BYTE (*)[MAP_ARRAY_SIZE])clientSimGetBrainMap(bot->cs), (BYTE)x, (BYTE)y,
                                 mapGetPos(&gs->mp, (BYTE)x, (BYTE)y),
                                 minesExistPos(&gs->mns, &gs->mp, (BYTE)x, (BYTE)y));
            if ((BYTE)x == right) break;
        }
        if ((BYTE)y == bottom) break;
    }
}

/* Copies the map, bases, pills, and starts from the ServerSim
 * into the bot's ClientSim using compressed serialization. */
static bool botLoadMapFromServer(BotContext *bot, ServerSim *sim) {
    BYTE *buf;
    int len;
    bool ok;

    buf = (BYTE *)malloc(65536);
    if (buf == NULL) {
        return false;
    }

    len = serverSimGetCompressedMap(sim, buf);
    if (len <= 0) {
        free(buf);
        return false;
    }

    {
        GameSim *gs = clientSimGetGameSim(bot->cs);
        ok = mapLoadCompressedMap(&gs->mp, &gs->pb,
                                  &gs->bs, &gs->ss,
                                  buf, len);
    }
    free(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

double botManagerComputePerBotTargetMs(int activeBots) {
    if (activeBots <= 0) {
        return kTickMs;
    }
    double brainBudget = kTickMs - s_serialMsEwma - kReservedSimMs - kSafetyMs;
    if (brainBudget < 1.0) {
        brainBudget = 1.0;
    }
    double perBot = brainBudget * (double)g_threadsConfig
                    / (double)activeBots;
    if (perBot > brainBudget) {
        perBot = brainBudget;
    }
    return perBot;
}

void botManagerRecordSerialMs(double ms) {
    if (s_serialMsEwma <= 0.0) {
        s_serialMsEwma = ms;
    } else {
        s_serialMsEwma = kAlpha * ms + (1.0 - kAlpha) * s_serialMsEwma;
    }
}

bool botManagerInit(int threads) {
    memset(bots, 0, sizeof(bots));
    numBots = 0;
    /* Eager-init the shared sin/cos tables before any worker thread
     * could touch them. Single-threaded context here. */
    wsim_init_tables();

    int cores = SDL_GetNumLogicalCPUCores();
    if (threads <= 0) {
        threads = cores;
    }
    if (threads > cores) {
        WB_LOG_ERROR(WB_LOG_CAT_PLATFORM,
                     "botManagerInit: threads=%d exceeds %d logical cores",
                     threads, cores);
        return false;
    }
    if (threads > MAX_TANKS) {
        threads = MAX_TANKS;
    }

#if defined(__APPLE__) && defined(TARGET_OS_IOS) && TARGET_OS_IOS
    /* Leave headroom for the GPU/rendering thread on small devices:
     * cap at 4 total runners (3 workers + producer). */
    if (threads > 4) {
        threads = 4;
    }
#endif

    /* `threads` counts total runners including the producer; the pool
     * holds threads-1 workers and the producer runs one job inline. */
    int workers = threads - 1;
    if (workers > 0 && !botWorkerPoolCreate(workers)) {
        return false;
    }
    g_threadsConfig = threads;
    return true;
}

/* Swap the brain script path for an already-added bot without re-creating
 * its lobby slot. Used by PACKET_LOBBY_SET_BOT_BRAIN — the brain itself
 * isn't loaded until the game starts, so storing the new path is enough. */
bool botManagerSetBrainPath(BYTE playerNum, const char *brainPath) {
    if (playerNum >= MAX_TANKS || brainPath == NULL) return false;
    if (!bots[playerNum].active) return false;
    SDL_strlcpy(bots[playerNum].brainPath, brainPath,
                sizeof(bots[playerNum].brainPath));
    return true;
}

bool botManagerAddBot(ServerSim *sim, BYTE playerNum,
                      const char *brainPath, const char *brainName,
                      aiType ai, gameType game, bool hiddenMines) {
    BotContext *bot;

    if (playerNum >= MAX_TANKS) {
        return false;
    }
    if (bots[playerNum].active) {
        botManagerRemoveBot(sim, playerNum);
    }

    bot = &bots[playerNum];
    memset(bot, 0, sizeof(BotContext));
    bot->playerNum = playerNum;
    bot->ai = ai;
    bot->controlSub = SUBSCRIBER_HANDLE_INVALID;
    if (brainPath != NULL) {
        SDL_strlcpy(bot->brainPath, brainPath, sizeof(bot->brainPath));
    }

    {
        ServerSimBotConfig cfg = {
            .brainPath   = bot->brainPath,
            .brainName   = brainName,
            .ai          = ai,
            .gameType    = game,
            .hiddenMines = hiddenMines,
            .teamNumber  = 0,
        };
        if (!serverSimAddBot(sim, playerNum, &cfg)) {
            return false;
        }
    }

    /* Set bot name in transport client array for lobby broadcasts */
    transportUdpServerSetBotName(playerNum, brainName);

    /* Create the bot's ClientSim */
    bot->cs = clientSimAlloc();
    if (bot->cs == NULL) {
        serverSimRemovePlayer(sim, playerNum);
        return false;
    }
    clientSimCreate(bot->cs, game, hiddenMines, 0, -1);
    clientSimSetIsBot(bot->cs, true);
    clientSimSetPlayerNum(bot->cs, playerNum);

    /* Load map data from the server before tankCreate so the bot's local
     * starts/pills/bases are populated when startsGetStart() runs — without
     * starts loaded it early-returns on numStarts==0, leaving tankCreate's
     * out-params undefined. */
    if (!botLoadMapFromServer(bot, sim)) {
        fprintf(stderr, "botManager: failed to load map for bot %d\n", playerNum);
        clientSimDestroy(bot->cs);
        bot->cs = NULL;
        serverSimRemovePlayer(sim, playerNum);
        return false;
    }

    /* Set this bot's identity (creates the tank and writes the self
     * record on the bot's local ClientSim) */
    clientSimSetupSelf(bot->cs, playerNum, brainName, 0, 0);

    /* Set AI type on the ClientSim */
    *clientSimGetAllowComputerTanks(bot->cs) = ai;

    /* Create passive transport (does NOT tick the server) */
    bot->transport = transportLocalCreatePassive(sim, playerNum);

    /* Register this bot's ClientSim as a control-event subscriber so
     * out-of-band roster/lobby/phase state from the server reaches it
     * the same way snapshots do. Sync runs inside register and uses the
     * dispatcher's self-skip to leave the playersSetSelf record above
     * untouched. */
    bot->controlSub = serverSimRegisterSubscriber(sim, botDeliverControl,
                                                  bot->cs);

    /* Initialize the brain map (fog-of-war) */
    /* screenBrainMapCreate already called by clientSimCreate,
     * which sets brainMap to TERRAIN_UNKNOWN. The bot's sim.brainMap
     * pointer is already set. */

    /* Create the Lua brain instance. debug_mode comes from the static
     * default (host-controlled): BrainTest sets it to true; the release
     * game leaves it false so brains load from stripped opt/ source. */
    if (!luaBrainInstanceCreate(&bot->brain, brainPath, brainName,
                                bot->cs, ai, s_default_debug_mode)) {
        fprintf(stderr, "botManager: failed to create brain for bot %d\n", playerNum);
        serverSimUnregisterSubscriber(sim, bot->controlSub);
        bot->controlSub = SUBSCRIBER_HANDLE_INVALID;
        transportLocalDestroy(&bot->transport);
        clientSimDestroy(bot->cs);
        bot->cs = NULL;
        serverSimRemovePlayer(sim, playerNum);
        return false;
    }

    /* Stash BotContext* in lua_getextraspace so the count hook and
     * cpf_/wsim_/NA bindings can recover bot->abort_flag and
     * bot->thinkDeadlineCounter from a bare lua_State without per-
     * binding plumbing. Set exactly once here. */
    if (bot->brain.L != NULL) {
        *(BotContext **)lua_getextraspace(bot->brain.L) = bot;
    }
    SDL_SetAtomicInt(&bot->abort_flag, 0);
    bot->thinkDeadlineCounter = 0;
    bot->wasKilled = false;

    /* Wire the abort flag through to the C pathfinder/worldsim so their
     * inner search loops can poll it without going back through Lua.
     * Pointer outlives the bot — both structs are owned by bot->brain
     * and torn down before the BotContext is reused. */
    if (bot->brain.pathfinder != NULL) {
        brainPathfinderSetAbortFlag(bot->brain.pathfinder, &bot->abort_flag);
    }
    if (bot->brain.worldsim != NULL) {
        brainWorldSimSetAbortFlag(bot->brain.worldsim, &bot->abort_flag);
    }

    bot->active = true;
    numBots++;

    fprintf(stderr, "botManager: bot %d started with brain '%s'\n",
            playerNum, brainName);
    return true;
}

/* Per-bot scratch carried across the three within-tick stages
 * (snapshot/sync, brain tick, input dispatch). One instance per bot
 * slot lives in s_jobs[] so the worker thread sees only its own
 * indexed entry — packets and the needRemove flag stay thread-local
 * to that bot. */
typedef struct {
    BotContext *bot;
    ServerSim  *sim;
    SnapshotHeader      hdr;
    TankSnapshot        tanks[MAX_TANKS];
    ShellSnapshot       shells[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot tkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot        bases[MAX_SNAPSHOT_BASES];
    PillSnapshot        pills[MAX_SNAPSHOT_PILLS];
    GameEvent           events[MAX_SNAPSHOT_EVENTS];
    InputPacket         pkt1, pkt2;
    bool                needRemove;  /* worker → producer: brain tick failed */
    bool                hasInput;    /* worker → producer: pkt1/pkt2 valid */
    /* worker → producer: brain.think was aborted by the count hook this
     * tick (tick_budget_exceeded). The bot stays alive; the producer
     * surfaces the kill to the next tick via bot->wasKilled. */
    bool                wasKilled;
} BotJobCtx;

static BotJobCtx s_jobs[MAX_TANKS];
static int       s_jobIndices[MAX_TANKS];

/* Run brain.think for the bot, with the count hook already armed and
 * a deadline already populated by the wrapper. Single return point in
 * the wrapper guarantees the hook is uninstalled regardless of which
 * exit path this function takes. */
static void runBotThinkJobImpl(BotJobCtx *j, BotContext *bot, Uint64 t0) {
    ServerSim *sim = j->sim;

    bool ok = luaBrainInstanceTick(&bot->brain);
    Uint64 t1 = SDL_GetPerformanceCounter();

    bot->lastThinkMs = (double)(t1 - t0) * 1000.0
                       / (double)SDL_GetPerformanceFrequency();

    if (!ok) {
        /* Distinguish a budget abort (recoverable: bot stays, brain.wasKilled
         * fires next tick) from a real Lua error (existing path: producer
         * removes the bot in Phase 3). Both signals must agree: the
         * suffix match on the error string is brain-spoofable (a brain
         * could raise error("tick_budget_exceeded") to dodge removal),
         * but the atomic flag is only ever stamped by brainBudgetHook.
         * AND-ing them proves the count hook actually ran. */
        if (bot->brain.wasKilled &&
            SDL_GetAtomicInt(&bot->abort_flag) != 0) {
            j->wasKilled = true;
        } else {
            /* Producer handles botManagerRemoveBot in the input-send stage —
             * never call it from a worker thread. */
            j->needRemove = true;
        }
        return;
    }

    if (MY_TANK(bot->cs) != NULL) {
        MY_TANK(bot->cs)->newTank = FALSE;
    }

    /* Build both InputPackets into per-bot scratch — packets are
     * thread-local to this ctx so two workers cannot collide. */
    bool firstIsGame = (serverSimGetTick(sim) % 2) == 0;
    clientBuildInputPacket(bot->cs, &j->pkt1, 0, FALSE, FALSE,
                             TRUE, firstIsGame, bot->playerNum,
                             serverSimGetTick(sim));
    clientBuildInputPacket(bot->cs, &j->pkt2, 0, FALSE, FALSE,
                             TRUE, !firstIsGame, bot->playerNum,
                             serverSimGetTick(sim) + 1);
    j->hasInput = true;
}

static void runBotThinkJob(int botIndex, void *userData) {
    (void)userData;
    BotJobCtx *j = &s_jobs[botIndex];
    BotContext *bot = j->bot;

    /* Optional pre-think hook (BrainTest viz). NULL in WinBoloDS.
     * Set once at host init and never modified after, so reading the
     * function pointer here from a worker thread is safe. */
    if (g_preThinkHook) g_preThinkHook(botIndex);

    /* Compute this bot's absolute deadline and arm the count hook
     * before the tick. Capturing t0 here (after the pre-think hook)
     * means lastThinkMs measures only the brain work itself. */
    Uint64 t0   = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    bot->thinkDeadlineCounter =
        t0 + (Uint64)(s_lastTargetMs * (double)freq / 1000.0);

    /* Install for brain.think() only — brain.open() (one-time init,
     * called from luaBrainInstanceCreate) is allowed to be unbounded. */
#if BRAIN_BUDGET_ENFORCE
    if (bot->brain.L != NULL) {
        lua_sethook(bot->brain.L, brainBudgetHook, LUA_MASKCOUNT, 1000);
    }
#endif

    runBotThinkJobImpl(j, bot, t0);

    /* Single uninstall point. Leaking the hook into the next tick would
     * fire against a stale deadline and abort spuriously, so this must
     * run on the success path AND the error/budget-kill path. */
#if BRAIN_BUDGET_ENFORCE
    if (bot->brain.L != NULL) {
        lua_sethook(bot->brain.L, NULL, 0, 0);
    }
#endif

    if (g_preThinkHook) g_preThinkHook(-1);
}

void botManagerTick(ServerSim *sim, aiType ai) {
    int activeCount = 0;

    /* Apply any pending thread-pool resize from the BrainTest panel.
     * Lands here, before dispatch, so the pool destroy/create gap is
     * always between ticks — never mid-flight. */
    applyPendingThreadResize();

    /* Capture the start of the serial setup stage. The EWMA of serial
     * cost is (brainStart - setupStart) + (sendEnd - brainEnd), i.e.
     * everything inside this function that is not the dispatched
     * brain-think stage. */
    Uint64 setupStart = SDL_GetPerformanceCounter();

    GameSim *gs = serverSimGetGameSim(sim);

    /* ---- Stage 1: snapshot/sync (serial, on producer thread) ---- */
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        BotContext *bot = &bots[i];
        if (!bot->active) continue;
        if (gs->tanks[i] == NULL) continue;

        BotJobCtx *j = &s_jobs[i];
        j->bot = bot;
        j->sim = sim;
        j->needRemove = false;
        j->hasInput   = false;
        j->wasKilled  = false;

        bot->transport.getSnapshot(bot->transport.ctx, bot->playerNum,
                                   &j->hdr, j->tanks, MAX_TANKS,
                                   j->shells, MAX_SNAPSHOT_SHELLS,
                                   j->tkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                                   j->bases, MAX_SNAPSHOT_BASES,
                                   j->pills, MAX_SNAPSHOT_PILLS,
                                   j->events, MAX_SNAPSHOT_EVENTS);

        clientSimSyncFromSnapshot(bot->cs, &j->hdr,
                                  j->tanks, j->hdr.tankCount,
                                  j->shells, j->hdr.shellCount,
                                  j->tkExplosions, j->hdr.tkExplosionCount,
                                  j->bases, j->hdr.baseCount,
                                  j->pills, j->hdr.pillCount,
                                  j->events, j->hdr.reliableEventCount,
                                  bot->playerNum);

        if (bot->ai == aiFull && bot->brain.isFirst) {
            /* Full map on first tick */
            screenBrainMapFillFromMap(bot->cs, &gs->mp, &gs->mns);
        }
        botUpdateBrainMap(bot, sim);

        /* Skip brain while tank is dead (waiting to respawn). Excluding
         * dead bots here keeps the worker job branch-free on liveness
         * and stops a respawning bot from inheriting another bot's queue
         * slot mid-dispatch. */
        if (MY_TANK(bot->cs) != NULL &&
            tankGetDeathWait(&MY_TANK(bot->cs)) > 0) {
            continue;
        }

        /* Reset key state before brain runs */
        *clientSimGetBrainHoldKeys(bot->cs) = 0;
        *clientSimGetBrainTapKeys(bot->cs) = 0;
        {
            BuildInfo **bi = clientSimGetBrainBuildInfo(bot->cs);
            if (*bi != NULL) {
                (*bi)->action = 0;
            }
        }

        s_jobIndices[activeCount++] = i;
    }

    /* Compute this tick's per-bot brain budget once, before dispatch.
     * Saved into s_lastTargetMs so the input-send overrun check below
     * (and any future server info command) reads the same value the
     * brains were given. */
    s_lastTargetMs = botManagerComputePerBotTargetMs(activeCount);
    for (int k = 0; k < activeCount; k++) {
        int i = s_jobIndices[k];
        /* wasKilled is a one-tick edge signal: pass the persisted flag
         * from the previous tick, then reset so it does not stick. The
         * worker may set bots[i].wasKilled = true again below if the
         * count hook fires this tick. */
        luaBrainSetTickInputs(&bots[i].brain,
                              bots[i].lastThinkMs,
                              s_lastTargetMs,
                              bots[i].wasKilled);
        bots[i].wasKilled = false;
        /* Clean abort flag so the worker starts each tick unflagged.
         * Atomic store pairs with the worker's atomic load on the
         * other side of the pool's release/acquire on dispatch. */
        SDL_SetAtomicInt(&bots[i].abort_flag, 0);
    }

    /* ---- Stage 2: brain tick (parallel via pool, serial on first tick) ----
     * BrainTest first-tick rule: if any bot's brain hasn't run yet,
     * registration callbacks (panel / overlay_detail / shotsim_poi) may
     * fire during this tick, and they read globals that are only safe to
     * touch from one thread at a time. Run all jobs serially on this
     * tick to give those callbacks a deterministic, single-threaded
     * registration window. */
    bool anyFirstTick = false;
    for (int k = 0; k < activeCount; k++) {
        if (bots[s_jobIndices[k]].brain.isFirst) {
            anyFirstTick = true;
            break;
        }
    }
    Uint64 brainStart = SDL_GetPerformanceCounter();
    if (anyFirstTick) {
        for (int k = 0; k < activeCount; k++) {
            runBotThinkJob(s_jobIndices[k], NULL);
        }
    } else {
        /* botWorkerPoolRun has its own serial fallback when the pool
         * has 0 workers, so the panel resize-to-1 case Just Works
         * without an extra check here. */
        botWorkerPoolRun(s_jobIndices, activeCount, runBotThinkJob, NULL);
    }
    Uint64 brainEnd = SDL_GetPerformanceCounter();

    /* ---- Stage 3: input dispatch (serial, on producer thread) ----
     * The pool's signal/wait pair on each worker's done semaphore acts
     * as a release/acquire, so every per-bot field a worker wrote
     * (j->pkt1, j->pkt2, j->needRemove, j->hasInput, bot->lastThinkMs)
     * is visible here without explicit barriers. */
    for (int k = 0; k < activeCount; k++) {
        int i = s_jobIndices[k];
        BotJobCtx *j = &s_jobs[i];

        if (j->needRemove) {
            WB_LOG_WARN(WB_LOG_CAT_LUA,
                        "bot %d brain tick failed, removing", i);
            botManagerRemoveBot(sim, (BYTE)i);
            continue;
        }
        if (j->wasKilled) {
            /* Tick aborted by the budget hook. The bot stays alive; a
             * partial cs state isn't safe to convert into an input
             * packet, so j->hasInput stays false above. Surface the
             * kill to next tick's brain.wasKilled and bump the overrun
             * counter — these always go together. */
            bots[i].wasKilled = true;
            bots[i].overrunCount++;
            if ((serverSimGetTick(sim) - bots[i].lastOverrunWarnTick) > 50) {
                WB_LOG_WARN(WB_LOG_CAT_LUA,
                            "bot %d think aborted (budget %.1fms exceeded; overruns=%u)",
                            i, s_lastTargetMs, bots[i].overrunCount);
                bots[i].lastOverrunWarnTick = serverSimGetTick(sim);
            }
            continue;
        }
        if (!j->hasInput) {
            continue;
        }

        bots[i].transport.sendInput(bots[i].transport.ctx, &j->pkt1);
        bots[i].transport.sendInput(bots[i].transport.ctx, &j->pkt2);

        /* Budget-overrun telemetry: every overrun bumps the per-bot
         * counter; logging is rate-limited to once per ~50 ticks per
         * bot so a chronically slow bot doesn't flood. Reads
         * bot->lastThinkMs which the worker wrote — visible here via
         * the pool's release/acquire on the done semaphore. */
        double ms = bots[i].lastThinkMs;
        if (ms > s_lastTargetMs * 1.5) {
            bots[i].overrunCount++;
            if ((serverSimGetTick(sim) - bots[i].lastOverrunWarnTick) > 50) {
                WB_LOG_WARN(WB_LOG_CAT_LUA,
                            "bot %d think %.1fms over target %.1fms (overruns=%u)",
                            i, ms, s_lastTargetMs, bots[i].overrunCount);
                bots[i].lastOverrunWarnTick = serverSimGetTick(sim);
            }
        }
    }

    /* End of the input-send stage. Feed the EWMA with the serial cost
     * (everything in this function except the dispatched brain-think
     * stage), and stash that stage's wall-clock for future use. */
    Uint64 sendEnd = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    s_lastBrainPhaseMs = (double)(brainEnd - brainStart) * 1000.0 / freq;
    if (s_brainPhaseMsEwma <= 0.0) {
        s_brainPhaseMsEwma = s_lastBrainPhaseMs;
    } else {
        s_brainPhaseMsEwma = kAlpha * s_lastBrainPhaseMs
                             + (1.0 - kAlpha) * s_brainPhaseMsEwma;
    }
    double serialMs = ((double)(brainStart - setupStart)
                       + (double)(sendEnd - brainEnd)) * 1000.0 / freq;
    s_lastSerialMs = serialMs;
    botManagerRecordSerialMs(serialMs);

    (void)ai;
}

void botManagerOnGameStart(ServerSim *sim) {
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        BotContext *bot = &bots[i];
        if (!bot->active) continue;

        /* Reload the bot's ClientSim map from the server (map was reset) */
        if (!botLoadMapFromServer(bot, sim)) {
            fprintf(stderr, "botManager: failed to reload map for bot %d on game start\n", i);
            continue;
        }

        /* Destroy and recreate the bot's tank */
        if (MY_TANK(bot->cs) != NULL) {
            tankDestroy(clientSimGetGameSim(bot->cs), &MY_TANK(bot->cs));
            MY_TANK(bot->cs) = NULL;
        }
        tankCreate(clientSimGetGameSim(bot->cs), &MY_TANK(bot->cs));

        /* Reset brain so full-map fill triggers again for aiFull bots */
        bot->brain.isFirst = true;
    }
}

void botManagerSetTeams(ServerSim *sim, const BYTE *teamOf, BYTE numPlayers) {
    if (sim == NULL || teamOf == NULL || numPlayers < 2) return;
    if (numPlayers > MAX_TANKS) numPlayers = MAX_TANKS;

    for (BYTE i = 0; i < numPlayers; i++) {
        for (BYTE j = 0; j < numPlayers; j++) {
            if (i == j || teamOf[i] != teamOf[j]) continue;

            allienceAdd(&serverSimGetGameSim(sim)->plyrs->item[i].allie, j);

            for (BYTE k = 0; k < MAX_TANKS; k++) {
                if (!bots[k].active) continue;
                allienceAdd(&clientSimGetGameSim(bots[k].cs)->plyrs->item[i].allie, j);
            }
        }
    }
}

void botManagerRemoveBot(ServerSim *sim, BYTE playerNum) {
    BotContext *bot;
    if (playerNum >= MAX_TANKS) return;
    bot = &bots[playerNum];
    if (!bot->active) return;

    luaBrainInstanceDestroy(&bot->brain);
    serverSimUnregisterSubscriber(sim, bot->controlSub);
    bot->controlSub = SUBSCRIBER_HANDLE_INVALID;
    transportLocalDestroy(&bot->transport);
    clientSimDestroy(bot->cs);
    bot->cs = NULL;
    serverSimRemovePlayer(sim, playerNum);

    bot->active = false;
    numBots--;

    fprintf(stderr, "botManager: bot %d removed\n", playerNum);
}

void botManagerDestroy(ServerSim *sim) {
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (bots[i].active) {
            botManagerRemoveBot(sim, i);
        }
    }
    /* No-op when no pool was created (single-thread or alloc-failed). */
    botWorkerPoolDestroy();
}

BYTE botManagerGetNumBots(void) {
    return (BYTE)numBots;
}

bool botManagerIsBot(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) return false;
    return bots[playerNum].active;
}

BrainPathfinder *botManagerGetBrainPathfinder(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) return NULL;
    if (!bots[playerNum].active) return NULL;
    return bots[playerNum].brain.pathfinder;
}

OverlayCmdBuffer *botManagerGetOverlayCmds(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) return NULL;
    if (!bots[playerNum].active) return NULL;
    /* The brain instance owns the buffer; return a pointer into it
     * so callers can read this tick's commands. The buffer is
     * populated by overlay_* Lua calls during brain.think(). */
    if (!bots[playerNum].brain.running) return NULL;
    return &bots[playerNum].brain.overlay;
}

double botManagerGetLastThinkMs(BYTE playerNum) {
    if (playerNum >= MAX_TANKS) return 0.0;
    if (!bots[playerNum].active) return 0.0;
    return bots[playerNum].lastThinkMs;
}

bool botManagerHasAnyBot(void) {
    for (int i = 0; i < MAX_TANKS; i++) {
        if (bots[i].active) return true;
    }
    return false;
}

bool botManagerGetBotInfo(BYTE playerNum, BotInfo *out) {
    if (out == NULL) return false;
    if (playerNum >= MAX_TANKS || !bots[playerNum].active) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    BotContext *bot = &bots[playerNum];

    /* Active count drives the per-bot target the next tick will use.
     * The console command is rare; recompute on each call rather than
     * caching. */
    int active = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (bots[i].active) active++;
    }

    out->isBot        = true;
    out->hasBrain     = (bot->ai == aiFull) && bot->brain.running;
    out->lastThinkMs  = bot->lastThinkMs;
    out->targetMs     = botManagerComputePerBotTargetMs(active);
    out->overrunCount = bot->overrunCount;

    /* Brain identity: basename of the brain script path (multiple bots
     * commonly share one brain, so the player display name is the wrong
     * source — bot->brainPath was captured at botManagerAddBot time). */
    if (out->hasBrain && bot->brainPath[0] != '\0') {
        const char *fwd  = strrchr(bot->brainPath, '/');
        const char *bwd  = strrchr(bot->brainPath, '\\');
        const char *sep  = fwd;
        if (bwd != NULL && (sep == NULL || bwd > sep)) {
            sep = bwd;
        }
        const char *base = (sep != NULL) ? sep + 1 : bot->brainPath;
        SDL_strlcpy(out->brainName, base, sizeof(out->brainName));
        size_t blen = SDL_strlen(out->brainName);
        if (blen >= 4 &&
            SDL_strcasecmp(out->brainName + blen - 4, ".lua") == 0) {
            out->brainName[blen - 4] = '\0';
        }
        /* Brains in this repo are conventionally laid out as
         * brains/<Name>/init.lua. The bare basename in that case is
         * "init", which is unhelpful — walk up to the parent directory
         * for the actual brain identity. */
        if (sep != NULL && SDL_strcmp(out->brainName, "init") == 0) {
            const char *parentEnd   = sep;
            const char *parentStart = bot->brainPath;
            for (const char *p = bot->brainPath; p < parentEnd; p++) {
                if (*p == '/' || *p == '\\') parentStart = p + 1;
            }
            size_t parentLen = (size_t)(parentEnd - parentStart);
            if (parentLen > 0 && parentLen < sizeof(out->brainName)) {
                memcpy(out->brainName, parentStart, parentLen);
                out->brainName[parentLen] = '\0';
            }
            /* If the parent-dir lookup fails (e.g. just "init.lua" with
             * no parent), leave brainName as "init" — still better than
             * blank. */
        }
    } else {
        SDL_strlcpy(out->brainName, "(none)", sizeof(out->brainName));
    }
    return true;
}

void botManagerGetPoolStats(BotPoolStats *out) {
    if (out == NULL) return;

    int      active        = 0;
    uint32_t totalOverruns = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (bots[i].active) {
            active++;
            totalOverruns += bots[i].overrunCount;
        }
    }

    out->workerCount      = botWorkerPoolGetSize();
    out->activeBots       = active;
    out->ewmaSerialMs     = s_serialMsEwma;
    out->currentTargetMs  = botManagerComputePerBotTargetMs(active);
    out->lastBrainPhaseMs = s_lastBrainPhaseMs;
    out->ewmaBrainPhaseMs = s_brainPhaseMsEwma;
    out->lastSerialMs     = s_lastSerialMs;
    out->totalOverruns    = totalOverruns;
}

int botManagerGetActiveBotCount(void) {
    int active = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (bots[i].active) active++;
    }
    return active;
}

void botManagerSetDefaultDebugMode(bool enabled) {
    s_default_debug_mode = enabled;
    s_brain_debug_mode   = enabled;
}

bool botManagerToggleAllBrainDebugMode(void) {
    s_brain_debug_mode = !s_brain_debug_mode;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (bots[i].active && bots[i].brain.running) {
            luaBrainInstanceSetDebugMode(&bots[i].brain, s_brain_debug_mode);
        }
    }
    return s_brain_debug_mode;
}

bool botManagerExecLua(BYTE playerNum, const char *src) {
    if (playerNum >= MAX_TANKS) return false;
    if (!bots[playerNum].active) return false;
    if (!bots[playerNum].brain.running) return false;
    lua_State *L = bots[playerNum].brain.L;
    if (!L || !src) return false;
    /* Compile + run a chunk of Lua in this bot's state. Used by
     * BrainTest to push viz toggle state into the brain's globals
     * each frame. Errors are swallowed (best-effort push). */
    if (luaL_loadstring(L, src) != LUA_OK) {
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

char *botManagerEvalLuaString(BYTE playerNum, const char *src) {
    if (playerNum >= MAX_TANKS) return NULL;
    if (!bots[playerNum].active) return NULL;
    if (!bots[playerNum].brain.running) return NULL;
    lua_State *L = bots[playerNum].brain.L;
    if (!L || !src) return NULL;
    /* Caller-owned heap copy of whatever string the chunk returns.
     * On any error path (compile fail, runtime fail, non-string
     * result) we return NULL — the panel renderer treats that as
     * "no fresh data, keep showing the previous text".
     *
     * Errors are logged with rate limiting so a recurring brain bug
     * doesn't flood the console; without this the panel just goes
     * silent and the user can't tell why. */
    static Uint64 sLastErrLogMs = 0;
    if (luaL_loadstring(L, src) != LUA_OK) {
        Uint64 now = SDL_GetTicks();
        if (now - sLastErrLogMs > 2000) {
            SDL_Log("brain %d: panel eval compile error: %s",
                    playerNum, lua_tostring(L, -1));
            sLastErrLogMs = now;
        }
        lua_pop(L, 1);
        return NULL;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        Uint64 now = SDL_GetTicks();
        if (now - sLastErrLogMs > 2000) {
            SDL_Log("brain %d: panel eval runtime error: %s",
                    playerNum, lua_tostring(L, -1));
            sLastErrLogMs = now;
        }
        lua_pop(L, 1);
        return NULL;
    }
    char *result = NULL;
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        if (s) {
            size_t n = strlen(s);
            result = (char *)malloc(n + 1);
            if (result) memcpy(result, s, n + 1);
        }
    }
    lua_pop(L, 1);
    return result;
}

/* ------------------------------------------------------------------ */
/* Lua state query helpers for goal info                               */
/* ------------------------------------------------------------------ */

/* Read a string field from table at stack index tblIdx */
static void luaReadStringField(lua_State *L, int tblIdx, const char *key,
                                char *out, size_t outLen) {
    lua_getfield(L, tblIdx, key);
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        strncpy(out, s, outLen - 1);
        out[outLen - 1] = '\0';
    } else {
        out[0] = '\0';
    }
    lua_pop(L, 1);
}

/* Read an integer field from table at stack index tblIdx */
static int luaReadIntField(lua_State *L, int tblIdx, const char *key) {
    lua_getfield(L, tblIdx, key);
    int v = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1)
          : lua_isnumber(L, -1)  ? (int)lua_tonumber(L, -1)
          : 0;
    lua_pop(L, 1);
    return v;
}

bool botManagerGetGoalInfo(BYTE playerNum, BrainGoalInfo *out) {
    lua_State *L;
    if (playerNum >= MAX_TANKS || !bots[playerNum].active) return false;
    L = bots[playerNum].brain.L;
    if (!L) return false;

    memset(out, 0, sizeof(BrainGoalInfo));

    /* Call brain.get_debug_info() which returns a table with
     * kind, mx, my, substate, pool.
     * The brain table is stored as lowercase "brain" global
     * (set by luaBrainInstanceCreate). */
    lua_getglobal(L, "brain");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }

    lua_getfield(L, -1, "get_debug_info");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return false; }

    if (lua_pcall(L, 0, 1, 0) != 0) {
        /* Lua error — discard error message */
        lua_pop(L, 2); /* error msg + Brain table */
        return false;
    }

    /* Result table is on top of stack, Brain table below */
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return false; }

    luaReadStringField(L, -1, "kind", out->kind, sizeof(out->kind));
    out->mx = luaReadIntField(L, -1, "mx");
    out->my = luaReadIntField(L, -1, "my");
    out->target_id = luaReadIntField(L, -1, "target_id");
    luaReadStringField(L, -1, "substate", out->substate, sizeof(out->substate));

    /* Read pool array */
    lua_getfield(L, -1, "pool");
    if (lua_istable(L, -1)) {
        int n = (int)lua_rawlen(L, -1);
        if (n > BRAIN_GOAL_MAX_CANDIDATES) n = BRAIN_GOAL_MAX_CANDIDATES;
        out->num_candidates = n;
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                luaReadStringField(L, -1, "desc",
                    out->candidates[i-1].desc,
                    sizeof(out->candidates[i-1].desc));

                lua_getfield(L, -1, "cost");
                out->candidates[i-1].cost = lua_isnumber(L, -1)
                    ? (float)lua_tonumber(L, -1) : 0.0f;
                lua_pop(L, 1);

                lua_getfield(L, -1, "winner");
                out->candidates[i-1].winner = lua_toboolean(L, -1);
                lua_pop(L, 1);

                lua_getfield(L, -1, "phase_weight");
                out->candidates[i-1].phase_weight = lua_isnumber(L, -1)
                    ? (float)lua_tonumber(L, -1) : 1.0f;
                lua_pop(L, 1);
            }
            lua_pop(L, 1); /* pop candidate table */
        }
    }
    lua_pop(L, 1); /* pop pool */

    lua_pop(L, 2); /* pop result table + Brain table */
    return true;
}
