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
#include <stdarg.h>
#include <time.h>

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
#include "messages.h"      /* messageInboxPush for botManagerDeliverInternalMessage */
#include "util.h"          /* utilCtoPString for the inbox payload */
#include "allience.h"
#include "mines.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "transport.h"
#include "client_snapshot.h"
#include "client_net.h"
#include "screenbrainmap.h"
#include "input_packet.h"
#include "bot_manager.h"
#include "bot_worker_pool.h"
#include "brain_worldsim.h"
#include <lua.h>
#include <lauxlib.h>   /* luaL_loadstring for botManagerExecLua */
#include "../common/wb_log.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "brain_record.h"
#include "../gui/sdl3/luabrainshandler.h"

/* View size for brain map updates — 15x15 centered on tank */
#define BOT_VIEW_HALF 7

/* Consecutive brain.think() Lua errors before we give up and kick the
 * bot. A single bad tick (e.g. transient pill-take edge case) just
 * skips that frame — bot survives. 100 in a row (= 2 s at 50 Hz) is
 * "this brain is unrecoverable, stop wasting CPU on it" and triggers
 * removal + a server-text broadcast naming the kicked bot. */
#define BOT_CRASH_KICK_THRESHOLD 100

void botManagerRequestThreads(ServerSim *sim, int total_runners) {
    if (sim == NULL) return;
    if (total_runners < 1) total_runners = 1;
    int cores = SDL_GetNumLogicalCPUCores();
    if (total_runners > cores)    total_runners = cores;
    if (total_runners > MAX_TANKS) total_runners = MAX_TANKS;
    sim->botMgr.pendingThreads = total_runners;
}

int botManagerGetThreads(const ServerSim *sim) {
    return sim ? sim->botMgr.threadsConfig : 1;
}
int botManagerGetPendingThreads(const ServerSim *sim) {
    return sim ? sim->botMgr.pendingThreads : -1;
}

/* Bot chat send callback.
 * The default clientSimDefaultChatSend → clientSimNetSendChat path takes
 * threadsMutex inside clientSimSubmitCommand (client_net.c:429).  Bot
 * brains run on worker threads inside the producer's botWorkerPoolRun
 * call, during which the producer (timer thread) is HOLDING threadsMutex
 * — the worker would block forever waiting for it, and the producer in
 * turn is blocked waiting for the worker, deadlocking the whole game.
 *
 * Instead of submitting directly, we queue a CMD_CHAT on the bot's
 * BotJobCtx and let Stage 3 (serial, on the producer thread, already
 * under the mutex) drain the queue via serverSimApplyCommand.  Same
 * end-result (CTRL_CHAT publish → in-process subscribers receive),
 * just routed through the deferred queue. */
static void botManagerQueueingChatSendCallback(ClientSim *cs,
                                               uint8_t fromPlayer,
                                               uint8_t destPlayer,
                                               const char *message) {
    (void)fromPlayer;
    if (cs == NULL || message == NULL || message[0] == '\0') return;
    ServerSim *sim = (ServerSim *)clientSimGetBoundServerSim(cs);
    if (sim == NULL) return;
    BYTE pn = clientSimGetMyPlayerNum(cs);
    if (pn >= MAX_TANKS) return;
    BotJobCtx *j = &sim->botMgr.jobs[pn];
    if (j->pendingCmdCount >= BOT_PENDING_CMD_MAX) {
        /* Queue full — drop. Two ticks back-to-back without a Stage-3
         * drain shouldn't happen (drain runs every botManagerTick), and
         * even if it did, dropping bot chat is preferable to allocating
         * unbounded memory inside a worker. */
        return;
    }
    ClientCommand *cmd = &j->pendingCmds[j->pendingCmdCount++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = CMD_CHAT;
    cmd->u.chat.destPlayer = destPlayer;
    size_t msgLen = strlen(message);
    if (msgLen > PACKET_MAX_CHAT_MESSAGE) msgLen = PACKET_MAX_CHAT_MESSAGE;
    cmd->u.chat.bodyLen = (uint16_t)msgLen;
    memcpy(cmd->u.chat.body, message, msgLen);
}

/* Apply any pending pool-resize request. Called from botManagerTick
 * before any dispatch happens, so the resize lands in the gap between
 * ticks. */
static void applyPendingThreadResize(ServerSim *sim) {
    int want = sim->botMgr.pendingThreads;
    if (want < 0 || want == sim->botMgr.threadsConfig) {
        sim->botMgr.pendingThreads = -1;
        return;
    }
    botWorkerPoolDestroy();
    int workers = want - 1;
    if (workers > 0 && !botWorkerPoolCreate(workers)) {
        WB_LOG_WARN(WB_LOG_CAT_PLATFORM,
                    "botManager: pool create(%d) failed; staying serial",
                    workers);
        sim->botMgr.threadsConfig = 1;
    } else {
        sim->botMgr.threadsConfig = want;
    }
    sim->botMgr.pendingThreads = -1;
}

/* Diagnostic toggle for the per-tick brain budget kill path. When 1
 * (default) the count hook is installed for every brain.think(), fires
 * tick_budget_exceeded on overrun, and C bindings poll the abort flag
 * for partial returns. When 0 the hook is never installed, abort_flag
 * is never stamped, and brain.think runs to completion regardless of
 * how long it takes — the producer's post-tick overrun telemetry
 * (overrunCount, rate-limited slow-tick warning) still fires, so
 * operators still see overruns, just without the truncation. */
/* ===== TEMPORARY (debug): budget kill DISABLED =====
 * Set to 0 so brain.think() runs to completion no matter how long a tick
 * takes — lets us sit in plan_position and watch overlays without bots being
 * killed on overruns. The sim just runs slower on heavy ticks; overrun
 * telemetry still fires. RESTORE TO 1 when done debugging. */
#ifndef BRAIN_BUDGET_ENFORCE
#define BRAIN_BUDGET_ENFORCE 0
#endif

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

void botManagerSetPreThinkHook(ServerSim *sim,
                               void (*hook)(int playerNum)) {
    if (sim == NULL) return;
    sim->botMgr.preThinkHook = hook;
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

/* Best-effort copy of _G.DEBUG_SESSION_DIR off the brain's lua_State so
 * killbot.log lands in the same per-session dir as the other logs.
 * Returns true if set & non-empty. */
static bool botReadSessionDir(lua_State *L, char *out, size_t outsz) {
    if (L == NULL || out == NULL || outsz == 0) return false;
    int top = lua_gettop(L);
    bool ok = false;
    out[0] = '\0';
    lua_getglobal(L, "DEBUG_SESSION_DIR");
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        if (s && s[0]) {
            strncpy(out, s, outsz - 1);
            out[outsz - 1] = '\0';
            ok = true;
        }
    }
    lua_settop(L, top);
    return ok;
}

/* Append one line to killbot.log recording a budget-kill: which bot,
 * how long its think actually ran, the budget/target it was given, the
 * running overrun count, and the tick. Opened in append mode and closed
 * each call so the file is complete even if the run is interrupted. */
static void botLogKill(ServerSim *sim, int botIndex) {
    BotContext *bot = &sim->botMgr.bots[botIndex];

    char session_dir[512];
    bool has_session = botReadSessionDir(bot->brain.L, session_dir,
                                         sizeof(session_dir));
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%s/killbot.log",
                 has_session ? session_dir : ".");

    FILE *f = fopen(path, "a");
    if (f == NULL) return;

    time_t now = time(NULL);
    struct tm tm_local;
#ifdef _WIN32
    localtime_s(&tm_local, &now);
#else
    localtime_r(&now, &tm_local);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_local);

    fprintf(f,
            "[%s] tick=%u bot=%d KILLED: took=%.2fms budget=%.2fms "
            "over=%.2fms (%.0f%% of budget) overruns=%u\n",
            ts, (unsigned)serverSimGetTick(sim), botIndex,
            bot->lastThinkMs, sim->botMgr.lastTargetMs,
            bot->lastThinkMs - sim->botMgr.lastTargetMs,
            sim->botMgr.lastTargetMs > 0.0
                ? (bot->lastThinkMs / sim->botMgr.lastTargetMs) * 100.0
                : 0.0,
            (unsigned)bot->overrunCount);
    fclose(f);
}

bool botManagerShouldAbort(struct lua_State *L) {
    BotContext *bot = botFromLua((lua_State *)L);
    if (bot == NULL) return false;
    return SDL_GetAtomicInt(&bot->abort_flag) != 0;
}

int botManagerActiveBotCountForLua(struct lua_State *L) {
    BotContext *bot = botFromLua((lua_State *)L);
    if (bot == NULL || bot->sim == NULL) return 0;
    return botManagerGetActiveBotCount(bot->sim);
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
            screenBrainMapSetPos((BYTE (*)[MAP_ARRAY_SIZE])clientSimGetBrainMap(bot->cs),
                                 (BYTE)x, (BYTE)y,
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

double botManagerComputePerBotTargetMs(const ServerSim *sim, int activeBots) {
    if (sim == NULL || activeBots <= 0) {
        return kTickMs;
    }
    double brainBudget = kTickMs - sim->botMgr.serialMsEwma - kReservedSimMs - kSafetyMs;
    if (brainBudget < 1.0) {
        brainBudget = 1.0;
    }
    /* Parallelism comes from the live worker pool (N workers + the
     * producer running one job inline = N+1 runners). Read the pool size
     * directly as ground truth rather than relying solely on the per-sim
     * threadsConfig: depending on init order, threadsConfig can lag the
     * actual pool (e.g. when the BotManager is set up before the global
     * pool is created), which would under-count runners and starve the
     * per-bot budget. Fall back to threadsConfig only when it's larger —
     * the BrainTest resize path keeps it in sync with the pool, and it
     * covers the inline-serial pool==0 case. */
    int runners = botWorkerPoolGetSize() + 1;
    if (sim->botMgr.threadsConfig > runners) {
        runners = sim->botMgr.threadsConfig;
    }
    double perBot = brainBudget * (double)runners
                    / (double)activeBots;
    if (perBot > brainBudget) {
        perBot = brainBudget;
    }
    return perBot;
}

void botManagerFlushBrainLogs(ServerSim *sim) {
    if (sim == NULL) return;
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        BotContext *bot = &sim->botMgr.bots[i];
        if (!bot->active || bot->brain.L == NULL) continue;
        lua_State *L = bot->brain.L;
        int top = lua_gettop(L);
        lua_getglobal(L, "__brain_flush_logs");
        if (lua_isfunction(L, -1)) {
            /* Best-effort: a flush failure must never disturb the sim. */
            (void)lua_pcall(L, 0, 0, 0);
        } else {
            lua_pop(L, 1);
        }
        lua_settop(L, top);
    }
}

void botManagerRecordSerialMs(ServerSim *sim, double ms) {
    if (sim == NULL) return;
    if (sim->botMgr.serialMsEwma <= 0.0) {
        sim->botMgr.serialMsEwma = ms;
    } else {
        sim->botMgr.serialMsEwma = kAlpha * ms
                                   + (1.0 - kAlpha) * sim->botMgr.serialMsEwma;
    }
}

bool botManagerInit(int threads) {
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
    return true;
}

void botManagerInitInSim(BotManager *bm, ServerSim *sim, int threads) {
    if (bm == NULL) return;
    memset(bm, 0, sizeof(*bm));
    bm->sim = sim;
    bm->pendingThreads = -1;
    if (threads <= 0) {
        int pool = botWorkerPoolGetSize();
        bm->threadsConfig = (pool > 0) ? pool + 1 : 1;
    } else {
        int cores = SDL_GetNumLogicalCPUCores();
        if (threads > cores)    threads = cores;
        if (threads > MAX_TANKS) threads = MAX_TANKS;
        bm->threadsConfig = threads;
    }
}

/* Extract a display name from a brain path. The path convention is
 *   "Brains/<DirName>/init.lua"
 * so the display name is the directory segment. Falls back to the
 * basename (sans .lua) when the path doesn't fit that shape. */
static void botManagerBrainNameFromPath(const char *brainPath,
                                         char *out, size_t outCap) {
    if (!out || outCap == 0) return;
    out[0] = '\0';
    if (!brainPath) return;
    size_t len = strlen(brainPath);
    if (len == 0) return;
    /* fname = last segment after a / or \. */
    const char *fname = brainPath + len;
    while (fname > brainPath && fname[-1] != '/' && fname[-1] != '\\') {
        fname--;
    }
    if (fname == brainPath) {
        SDL_strlcpy(out, brainPath, outCap);
        size_t ol = strlen(out);
        if (ol > 4 && SDL_strcasecmp(out + ol - 4, ".lua") == 0) {
            out[ol - 4] = '\0';
        }
        return;
    }
    /* Walk back through the separator and grab the previous segment. */
    const char *dirEnd   = fname - 1;
    const char *dirStart = dirEnd;
    while (dirStart > brainPath &&
           dirStart[-1] != '/' && dirStart[-1] != '\\') {
        dirStart--;
    }
    size_t dirLen = (size_t)(dirEnd - dirStart);
    if (dirLen >= outCap) dirLen = outCap - 1;
    memcpy(out, dirStart, dirLen);
    out[dirLen] = '\0';
}

/* Tear down a bot's brain instance and re-load it from `brainPath`.
 * Re-wires lua_getextraspace + abort flag + pathfinder/worldsim
 * abort plumbing — same setup botManagerAddBot does on first load.
 * Bot's transport / ClientSim / subscriber registration / lobby
 * slot stay intact. Returns false on brain-load failure; on failure
 * the bot's brain is inactive and caller should remove the bot. */
static bool botManagerReloadBrain(ServerSim *sim, BotContext *bot,
                                  const char *brainPath) {
    if (!sim || !bot || !brainPath) return false;
    char brainName[64];
    botManagerBrainNameFromPath(brainPath, brainName, sizeof(brainName));

    if (bot->brain.L != NULL || bot->brain.running) {
        luaBrainInstanceDestroy(&bot->brain);
    }

    SDL_strlcpy(bot->brainPath, brainPath, sizeof(bot->brainPath));

    if (!luaBrainInstanceCreate(&bot->brain, brainPath, brainName,
                                bot->cs, bot->ai,
                                sim->botMgr.defaultDebugMode,
                                (int)bot->playerNum)) {
        WB_LOG_WARN(WB_LOG_CAT_SIM,
                "botManager: failed to reload brain '%s' for bot %d",
                brainPath, (int)bot->playerNum);
        return false;
    }

    if (bot->brain.L != NULL) {
        *(BotContext **)lua_getextraspace(bot->brain.L) = bot;
    }
    SDL_SetAtomicInt(&bot->abort_flag, 0);
    bot->thinkDeadlineCounter = 0;
    bot->wasKilled = false;
    if (bot->brain.pathfinder != NULL) {
        brainPathfinderSetAbortFlag(bot->brain.pathfinder, &bot->abort_flag);
    }
    if (bot->brain.worldsim != NULL) {
        brainWorldSimSetAbortFlag(bot->brain.worldsim, &bot->abort_flag);
    }
    WB_LOG_INFO(WB_LOG_CAT_SIM,
            "botManager: bot %d reloaded with brain '%s'",
            (int)bot->playerNum, brainName);
    return true;
}

/* Swap the brain script for an already-added bot. Destroys the
 * existing Lua VM and re-loads from the disk path resolved through
 * the server's catalogue mirror so the bot starts ticking the chosen
 * brain immediately. No-op when the path is unchanged (avoids a
 * redundant reload on roster resyncs or duplicate UI events). */
bool botManagerSetBrainIdx(ServerSim *sim, BYTE playerNum,
                           uint8_t brainIdx) {
    const char *brainPath;
    if (sim == NULL || playerNum >= MAX_TANKS) return false;
    if (!sim->botMgr.bots[playerNum].active) return false;
    brainPath = serverSimGetBrainPathForIdx(sim, brainIdx);
    if (brainPath == NULL) return false;
    if (SDL_strcasecmp(sim->botMgr.bots[playerNum].brainPath, brainPath) == 0) {
        return true;
    }
    return botManagerReloadBrain(sim, &sim->botMgr.bots[playerNum], brainPath);
}

bool botManagerAddBot(ServerSim *sim, BYTE playerNum,
                      const char *brainPath, const char *brainName,
                      aiType ai, gameType game, bool hiddenMines) {
    BotContext *bot;

    if (sim == NULL || playerNum >= MAX_TANKS) {
        return false;
    }
    if (sim->botMgr.bots[playerNum].active) {
        botManagerRemoveBot(sim, playerNum);
    }

    bot = &sim->botMgr.bots[playerNum];
    memset(bot, 0, sizeof(BotContext));
    bot->sim = sim;
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

    /* Create the bot's ClientSim */
    bot->cs = clientSimAlloc();
    if (bot->cs == NULL) {
        serverSimRemovePlayer(sim, playerNum);
        return false;
    }
    clientSimCreate(bot->cs);
    clientSimSetIsBot(bot->cs, true);
    clientSimSetPlayerNum(bot->cs, playerNum);
    /* Chat send: override the default clientSimDefaultChatSend with our
     * own callback that QUEUES the chat as a pending CMD_CHAT on the
     * bot's BotJobCtx instead of calling clientSimSubmitCommand
     * directly.  The default path acquires threadsMutex inside
     * clientSimSubmitCommand, which would deadlock against the producer
     * thread (timer) that's currently holding it while waiting for the
     * worker to finish.  Stage 3 of botManagerTick drains the queue
     * serially under the held mutex. */
    clientSimSetChatSendFunc(bot->cs, botManagerQueueingChatSendCallback);

    /* Load map data from the server before tankCreate so the bot's local
     * starts/pills/bases are populated when startsGetStart() runs — without
     * starts loaded it early-returns on numStarts==0, leaving tankCreate's
     * out-params undefined. */
    if (!botLoadMapFromServer(bot, sim)) {
        WB_LOG_WARN(WB_LOG_CAT_SIM, "botManager: failed to load map for bot %d", playerNum);
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

    /* Create passive transport (does NOT tick the server). Bot
     * ClientSims drive their own snapshot pull through the existing
     * bot_manager loop rather than the per-tick auto-apply, so pass
     * NULL for cs to skip the localTick snapshot-apply hook. */
    bot->transport = transportLocalCreatePassive(sim, NULL, playerNum);

    /* Bind the transport into the bot's ClientSim so client_net.h
     * send wrappers (clientSimNetSendChat in particular) actually
     * fire instead of bailing on `!cs->hasTransport`.  Mirrors the
     * field setup at the bottom of clientSimNetConnectLocalSim
     * (client_net.c:199-204) — keep this sequence in lockstep with
     * that one so the host's local ClientSim and a bot's local
     * ClientSim look identical from the sim's POV.  Without these
     * bindings, brain.sendmessage was silently dropped at the
     * clientSimNetSendChat hasTransport gate — the symptom was
     * info.messages always empty on every bot. */
    bot->cs->transport      = bot->transport;
    bot->cs->hasTransport   = true;
    bot->cs->isUdpTransport = false;
    clientSimSetBoundServerSim(bot->cs, sim);
    clientSimSetLocalTransport(bot->cs, true);
    clientSimSetNetType(bot->cs, netSingle);

    /* Register this bot's ClientSim as a control-event subscriber so
     * out-of-band roster/lobby/phase state from the server reaches it
     * the same way snapshots do. Sync runs inside register and uses the
     * dispatcher's self-skip to leave the playersSetSelf record above
     * untouched. */
    bot->controlSub = serverSimRegisterClientSubscriber(sim, bot->cs);

    /* Initialize the brain map (fog-of-war) */
    /* screenBrainMapCreate already called by clientSimCreate,
     * which sets brainMap to TERRAIN_UNKNOWN. The bot's sim.brainMap
     * pointer is already set. */

    /* Create the Lua brain instance. debug_mode comes from the static
     * default (host-controlled): BrainTest sets it to true; the release
     * game leaves it false so brains load from stripped opt/ source. */
    if (!luaBrainInstanceCreate(&bot->brain, brainPath, brainName,
                                bot->cs, ai, sim->botMgr.defaultDebugMode,
                                playerNum)) {
        WB_LOG_WARN(WB_LOG_CAT_SIM, "botManager: failed to create brain for bot %d", playerNum);
        serverSimUnregisterSubscriber(sim, bot->controlSub);
        bot->controlSub = SUBSCRIBER_HANDLE_INVALID;
        /* Transport ownership moved to bot->cs (see binding above);
         * clientSimDestroy will tear it down via cs->transport. */
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
    sim->botMgr.numBots++;

    WB_LOG_INFO(WB_LOG_CAT_SIM, "botManager: bot %d started with brain '%s'",
            playerNum, brainName);
    return true;
}

/* Per-bot snapshot pull + ClientSim sync + brain-map refresh, run on
 * the worker thread. Reads the ServerSim (immutable during the brain
 * phase — serverSimTick already ran and inputs aren't applied until
 * Stage 3) and the shared game map read-only; writes only this bot's
 * own job buffers and ClientSim, so it parallelises safely across the
 * pool. Always returns true now (even for a dead tank): the dead tank still
 * gets a think (with info.dead set) so the brain can reset for respawn, and
 * runBotThinkJobImpl drops its output so no input is sent. */
static bool botSyncSnapshotForJob(BotJobCtx *j) {
    BotContext *bot = j->bot;
    ServerSim *sim  = j->sim;
    GameSim *gs     = serverSimGetGameSim(sim);

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

    /* NOTE: dead tanks (waiting to respawn) used to skip the think entirely.
     * We now still run the think so the brain can reset its own state for a
     * clean respawn. brainDataMakeInfo sets info.dead, the brain early-returns
     * without acting, and runBotThinkJobImpl drops the output (no input sent
     * for a dead tank). So this no longer early-returns on death. */

    /* Reset key state before brain runs */
    *clientSimGetBrainHoldKeys(bot->cs) = 0;
    *clientSimGetBrainTapKeys(bot->cs) = 0;
    {
        BuildInfo **bi = clientSimGetBrainBuildInfo(bot->cs);
        if (*bi != NULL) {
            (*bi)->action = 0;
        }
    }
    return true;
}

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
        /* Two failure modes:
         *   1. Budget abort (brain.wasKilled + atomic abort flag set
         *      by brainBudgetHook). Recoverable. brain.wasKilled
         *      signals the bot next tick so it can self-throttle.
         *      Does NOT count against the crash-streak — slow ticks
         *      aren't bugs, they're throttle signals.
         *   2. Real Lua error in brain.think(). A single error skips
         *      this tick — most are transient (one bad pill-take
         *      config, a rare race) and the bot recovers next frame.
         *      braincore.c writes a rate-limited crash file with the
         *      full traceback.
         *      We count consecutive Lua errors; once we hit
         *      BOT_CRASH_KICK_THRESHOLD in a row (= 2 s at 50 Hz of
         *      uninterrupted crashing), the brain is unrecoverable
         *      and we set needRemove for the producer phase to kick
         *      with a network broadcast.  A single successful tick
         *      resets the streak. */
        if (bot->brain.wasKilled &&
            SDL_GetAtomicInt(&bot->abort_flag) != 0) {
            j->wasKilled = true;
        } else {
            bot->consecutiveCrashes++;
            if (bot->consecutiveCrashes >= BOT_CRASH_KICK_THRESHOLD) {
                /* Producer broadcasts + removes — never call
                 * botManagerRemoveBot or serverSimPublishControl
                 * from a worker thread. */
                j->needRemove = true;
            }
        }
        /* Skip this tick. j->hasInput stays false → no input packet
         * built → bot is idle this frame but still in the roster. */
        return;
    }

    /* Successful tick — clear the crash streak so a flaky brain that
     * recovers between crashes never trips the kick threshold. */
    bot->consecutiveCrashes = 0;

    /* Dead-tick: the think ran only so the brain could reset its own state.
     * A dead tank can't act, so drop the output entirely — build no input
     * packet (j->hasInput stays false → producer sends nothing this frame). */
    if (bot->brain.bInfo.dead) {
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
    ServerSim *sim = (ServerSim *)userData;
    BotJobCtx *j = &sim->botMgr.jobs[botIndex];
    BotContext *bot = j->bot;

    /* Pull this bot's snapshot, sync its ClientSim, and refresh its
     * brain map — formerly serial Stage-1 work, now parallelised here.
     * A dead bot (respawn wait) syncs state but skips the think. Runs
     * before t0/hook so its cost lands in the brain-phase wall-clock,
     * not the per-bot think deadline (lastThinkMs stays brain-only). */
    if (!botSyncSnapshotForJob(j)) {
        return;
    }

    /* Optional pre-think hook (BrainTest viz). NULL in WinBoloDS.
     * Set once at host init and never modified after, so reading the
     * function pointer here from a worker thread is safe. */
    if (sim->botMgr.preThinkHook) sim->botMgr.preThinkHook(botIndex);

    /* Compute this bot's absolute deadline and arm the count hook
     * before the tick. Capturing t0 here (after the pre-think hook)
     * means lastThinkMs measures only the brain work itself. */
    Uint64 t0   = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    bot->thinkDeadlineCounter =
        t0 + (Uint64)(sim->botMgr.lastTargetMs * (double)freq / 1000.0);

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

    if (sim->botMgr.preThinkHook) sim->botMgr.preThinkHook(-1);
}

void botManagerTick(ServerSim *sim, aiType ai) {
    int activeCount = 0;

    if (sim == NULL) return;

    /* Apply any pending thread-pool resize from the BrainTest panel.
     * Lands here, before dispatch, so the pool destroy/create gap is
     * always between ticks — never mid-flight. */
    applyPendingThreadResize(sim);

    /* Capture the start of the serial setup stage. The EWMA of serial
     * cost is (brainStart - setupStart) + (sendEnd - brainEnd), i.e.
     * everything inside this function that is not the dispatched
     * brain-think stage. */
    Uint64 setupStart = SDL_GetPerformanceCounter();

    GameSim *gs = serverSimGetGameSim(sim);

    /* ---- Stage 1: select active bots (serial, cheap) ----
     * Only decide which bots tick this frame. The heavy per-bot work —
     * snapshot pull, ClientSim sync, and brain-map refresh — used to run
     * here on the producer thread and dominated the serial budget; it
     * now runs inside the worker job (botSyncSnapshotForJob), so it
     * parallelises across the pool instead of draining the 20ms tick.
     *
     * One consequence: the dead-bot (respawn-wait) skip moved into the
     * worker, because it reads ClientSim state that only exists after
     * the sync. So a respawning bot is briefly counted in activeCount
     * here (diluting the per-bot budget for the few ticks it's dead),
     * but its worker early-returns before the think, so it costs no
     * brain time. */
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        BotContext *bot = &sim->botMgr.bots[i];
        if (!bot->active) continue;
        if (gs->tanks[i] == NULL) continue;

        BotJobCtx *j = &sim->botMgr.jobs[i];
        j->bot = bot;
        j->sim = sim;
        j->needRemove = false;
        j->hasInput   = false;
        j->wasKilled  = false;
        j->pendingCmdCount = 0;

        sim->botMgr.jobIndices[activeCount++] = i;
    }

    /* Compute this tick's per-bot brain budget once, before dispatch.
     * Saved into sim->botMgr.lastTargetMs so the input-send overrun check
     * below (and any future server info command) reads the same value
     * the brains were given. */
    sim->botMgr.lastTargetMs = botManagerComputePerBotTargetMs(sim, activeCount);
    for (int k = 0; k < activeCount; k++) {
        int i = sim->botMgr.jobIndices[k];
        /* wasKilled is a one-tick edge signal: pass the persisted flag
         * from the previous tick, then reset so it does not stick. The
         * worker may set sim->botMgr.bots[i].wasKilled = true again
         * below if the count hook fires this tick. */
        luaBrainSetTickInputs(&sim->botMgr.bots[i].brain,
                              sim->botMgr.bots[i].lastThinkMs,
                              sim->botMgr.lastTargetMs,
                              sim->botMgr.bots[i].wasKilled);
        sim->botMgr.bots[i].wasKilled = false;
        /* Clean abort flag so the worker starts each tick unflagged.
         * Atomic store pairs with the worker's atomic load on the
         * other side of the pool's release/acquire on dispatch. */
        SDL_SetAtomicInt(&sim->botMgr.bots[i].abort_flag, 0);
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
        if (sim->botMgr.bots[sim->botMgr.jobIndices[k]].brain.isFirst) {
            anyFirstTick = true;
            break;
        }
    }
    Uint64 brainStart = SDL_GetPerformanceCounter();
    if (anyFirstTick) {
        for (int k = 0; k < activeCount; k++) {
            runBotThinkJob(sim->botMgr.jobIndices[k], sim);
        }
    } else {
        /* botWorkerPoolRun has its own serial fallback when the pool
         * has 0 workers, so the panel resize-to-1 case Just Works
         * without an extra check here. */
        botWorkerPoolRun(sim->botMgr.jobIndices, activeCount,
                         runBotThinkJob, sim);
    }
    Uint64 brainEnd = SDL_GetPerformanceCounter();

    /* ---- Stage 3: input dispatch (serial, on producer thread) ----
     * The pool's signal/wait pair on each worker's done semaphore acts
     * as a release/acquire, so every per-bot field a worker wrote
     * (j->pkt1, j->pkt2, j->needRemove, j->hasInput, bot->lastThinkMs)
     * is visible here without explicit barriers. */
    for (int k = 0; k < activeCount; k++) {
        int i = sim->botMgr.jobIndices[k];
        BotJobCtx *j = &sim->botMgr.jobs[i];

        if (j->needRemove) {
            /* Crash-streak kick. Broadcast a server-text message to all
             * connected clients (UDP + in-process subscribers) BEFORE
             * removing the bot, so the kick reason is visible and other
             * players know why a tank just disappeared. fromPlayer=0xFE
             * (server-originated English) is implicit in CTRL_SERVER_TEXT;
             * the codec produces PACKET_CHAT_BROADCAST on the UDP path
             * and in-process subscribers (the host's own newswire / chat
             * window) handle the event directly. */
            char msg[128];
            SDL_snprintf(msg, sizeof(msg),
                         "Bot at slot %d kicked: brain crashed %u ticks in a row",
                         i, (unsigned)sim->botMgr.bots[i].consecutiveCrashes);
            ControlEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_SERVER_TEXT;
            SDL_strlcpy(evt.u.serverText.text, msg,
                        sizeof(evt.u.serverText.text));
            serverSimPublishControl(sim, &evt);

            WB_LOG_WARN(WB_LOG_CAT_LUA,
                        "bot %d kicked: %u consecutive brain crashes",
                        i, (unsigned)sim->botMgr.bots[i].consecutiveCrashes);
            botManagerRemoveBot(sim, (BYTE)i);
            continue;
        }
        if (j->wasKilled) {
            /* Tick aborted by the budget hook. The bot stays alive; a
             * partial cs state isn't safe to convert into an input
             * packet, so j->hasInput stays false above. Surface the
             * kill to next tick's brain.wasKilled and bump the overrun
             * counter — these always go together. */
            sim->botMgr.bots[i].wasKilled = true;
            sim->botMgr.bots[i].overrunCount++;
            botLogKill(sim, i);
            if ((serverSimGetTick(sim) - sim->botMgr.bots[i].lastOverrunWarnTick) > 50) {
                WB_LOG_WARN(WB_LOG_CAT_LUA,
                            "bot %d think aborted (budget %.1fms exceeded; overruns=%u)",
                            i, sim->botMgr.lastTargetMs, sim->botMgr.bots[i].overrunCount);
                sim->botMgr.bots[i].lastOverrunWarnTick = serverSimGetTick(sim);
            }
            continue;
        }
        if (!j->hasInput) {
            continue;
        }

        sim->botMgr.bots[i].transport.sendInput(sim->botMgr.bots[i].transport.ctx, &j->pkt1);
        sim->botMgr.bots[i].transport.sendInput(sim->botMgr.bots[i].transport.ctx, &j->pkt2);

        /* Drain any commands the worker queued via
         * botManagerQueueingChatSendCallback.  We're on the producer
         * thread here with threadsMutex already held (the timer
         * callback acquired it before dispatching serverInstanceTick),
         * so serverSimApplyCommand can run inline. */
        for (int q = 0; q < j->pendingCmdCount; q++) {
            (void)serverSimApplyCommand(sim, i, &j->pendingCmds[q]);
        }
        j->pendingCmdCount = 0;

        /* Budget-overrun telemetry: every overrun bumps the per-bot
         * counter; logging is rate-limited to once per ~50 ticks per
         * bot so a chronically slow bot doesn't flood. Reads
         * bot->lastThinkMs which the worker wrote — visible here via
         * the pool's release/acquire on the done semaphore. */
        double ms = sim->botMgr.bots[i].lastThinkMs;
        if (ms > sim->botMgr.lastTargetMs * 1.5) {
            sim->botMgr.bots[i].overrunCount++;
            if ((serverSimGetTick(sim) - sim->botMgr.bots[i].lastOverrunWarnTick) > 50) {
                WB_LOG_WARN(WB_LOG_CAT_LUA,
                            "bot %d think %.1fms over target %.1fms (overruns=%u)",
                            i, ms, sim->botMgr.lastTargetMs, sim->botMgr.bots[i].overrunCount);
                sim->botMgr.bots[i].lastOverrunWarnTick = serverSimGetTick(sim);
            }
        }
    }

    /* End of the input-send stage. Feed the EWMA with the serial cost
     * (everything in this function except the dispatched brain-think
     * stage), and stash that stage's wall-clock for future use. */
    Uint64 sendEnd = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    sim->botMgr.lastBrainPhaseMs = (double)(brainEnd - brainStart) * 1000.0 / freq;
    if (sim->botMgr.brainPhaseMsEwma <= 0.0) {
        sim->botMgr.brainPhaseMsEwma = sim->botMgr.lastBrainPhaseMs;
    } else {
        sim->botMgr.brainPhaseMsEwma = kAlpha * sim->botMgr.lastBrainPhaseMs
                                + (1.0 - kAlpha) * sim->botMgr.brainPhaseMsEwma;
    }
    double serialMs = ((double)(brainStart - setupStart)
                       + (double)(sendEnd - brainEnd)) * 1000.0 / freq;
    sim->botMgr.lastSerialMs = serialMs;
    botManagerRecordSerialMs(sim, serialMs);

    /* Brain-decision recorder (winbolods -braindebug). Inert unless enabled.
     * Runs here, after the worker pool joined and input was dispatched, so
     * every bot's overlay buffer + Lua state is settled and single-thread
     * safe to read/eval. */
    brainRecordTick(sim);

    (void)ai;
}

void botManagerOnGameStart(ServerSim *sim) {
    BYTE i;
    if (sim == NULL) return;
    for (i = 0; i < MAX_TANKS; i++) {
        BotContext *bot = &sim->botMgr.bots[i];
        if (!bot->active) continue;

        /* Reload the bot's ClientSim map from the server (map was reset) */
        if (!botLoadMapFromServer(bot, sim)) {
            WB_LOG_WARN(WB_LOG_CAT_SIM, "botManager: failed to reload map for bot %d on game start", i);
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
                if (!sim->botMgr.bots[k].active) continue;
                allienceAdd(&clientSimGetGameSim(sim->botMgr.bots[k].cs)->plyrs->item[i].allie, j);
            }
        }
    }
}

void botManagerDeliverInternalMessage(ServerSim *sim, BYTE fromPlayer,
                                      const char *msg) {
    if (sim == NULL || msg == NULL || msg[0] == '\0') return;
    if (fromPlayer >= MAX_TANKS) return;
    GameSim *gs = serverSimGetGameSim(sim);
    if (gs == NULL || gs->plyrs == NULL) return;

    /* Author's own alliance bitmap is authoritative for "who is on my
     * team right now" — same source the broadcast path used when the
     * message still went out over the chat wire. Includes self; we
     * skip the self bit below. */
    PlayerBitMap allies = playersGetAlliesBitMap(&gs->plyrs, fromPlayer);

    /* Pre-build the inbox payload once; messageInboxPush copies it
     * into each receiver's ring slot. Clamp at the inbox buffer in
     * case a brain ever sends past PACKET_MAX_CHAT_MESSAGE — the
     * wire path enforces the same cap but this path skips that. */
    char pbuf[BRAIN_INBOX_MSG_LEN];
    size_t mlen = strlen(msg);
    if (mlen > BRAIN_INBOX_MSG_LEN - 2) {
        mlen = BRAIN_INBOX_MSG_LEN - 2;
    }
    pbuf[0] = (char)mlen;
    memcpy(pbuf + 1, msg, mlen);
    pbuf[mlen + 1] = '\0';

    int delivered = 0;
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        if (i == fromPlayer) continue;
        if (!(allies & ((PlayerBitMap)1u << i))) continue;
        BotContext *bc = &sim->botMgr.bots[i];
        /* Human allies (or any non-bot slot) are skipped intentionally:
         * the whole point of the internal channel is that humans never
         * see it. botMgr only has entries for managed bot slots so
         * `active` doubles as a "this is a bot we host" gate. */
        if (!bc->active || bc->cs == NULL) continue;
        MessageState *ms = clientSimGetMessages(bc->cs);
        if (ms == NULL) continue;
        messageInboxPush(ms, fromPlayer, pbuf);
        delivered++;
    }
    /* Audit hook: shows whether the internal fan-out actually reached anyone.
     * delivered=0 with a populated allies map = teammates aren't hosted bots or
     * their inbox is missing; allies=self-only (e.g. 0x20 from p5) = the bots
     * were never allied (no -allybots / no lobby teams) -> comms can't work. */
    botMsgDebugLog("BOTMSG fan-out from p%u: allies=0x%X delivered=%d: %.48s",
                   (unsigned)fromPlayer, (unsigned)allies, delivered, msg);
}

void botManagerRemoveBot(ServerSim *sim, BYTE playerNum) {
    BotContext *bot;
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    bot = &sim->botMgr.bots[playerNum];
    if (!bot->active) return;

    luaBrainInstanceDestroy(&bot->brain);
    serverSimUnregisterSubscriber(sim, bot->controlSub);
    bot->controlSub = SUBSCRIBER_HANDLE_INVALID;
    /* Transport ownership moved to bot->cs (botManagerAddBot binds
     * bot->cs->transport = bot->transport); clientSimDestroy tears it
     * down via cs->transport.  Don't call transportLocalDestroy on
     * bot->transport here — it's the same underlying TransportLocalCtx
     * and would double-free. */
    clientSimDestroy(bot->cs);
    bot->cs = NULL;
    serverSimRemovePlayer(sim, playerNum);

    bot->active = false;
    sim->botMgr.numBots--;

    WB_LOG_INFO(WB_LOG_CAT_SIM, "botManager: bot %d removed", playerNum);
}

void botManagerDestroy(ServerSim *sim) {
    BYTE i;
    if (sim == NULL) return;
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->botMgr.bots[i].active) {
            botManagerRemoveBot(sim, i);
        }
    }
}

BYTE botManagerGetNumBots(const ServerSim *sim) {
    return sim ? (BYTE)sim->botMgr.numBots : 0;
}

bool botManagerIsBot(const ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return false;
    return sim->botMgr.bots[playerNum].active;
}

BrainPathfinder *botManagerGetBrainPathfinder(const ServerSim *sim,
                                              BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return NULL;
    if (!sim->botMgr.bots[playerNum].active) return NULL;
    return sim->botMgr.bots[playerNum].brain.pathfinder;
}

OverlayCmdBuffer *botManagerGetOverlayCmds(const ServerSim *sim,
                                           BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return NULL;
    if (!sim->botMgr.bots[playerNum].active) return NULL;
    /* The brain instance owns the buffer; return a pointer into it
     * so callers can read this tick's commands. The buffer is
     * populated by overlay_* Lua calls during brain.think(). */
    if (!sim->botMgr.bots[playerNum].brain.running) return NULL;
    /* Cast away const for the return — the buffer itself is mutable
     * (BrainTest pumps commands into it); we just promise not to
     * mutate the BotManager. */
    return (OverlayCmdBuffer *)&sim->botMgr.bots[playerNum].brain.overlay;
}

double botManagerGetLastThinkMs(const ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return 0.0;
    if (!sim->botMgr.bots[playerNum].active) return 0.0;
    return sim->botMgr.bots[playerNum].lastThinkMs;
}

bool botManagerHasAnyBot(const ServerSim *sim) {
    if (sim == NULL) return false;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (sim->botMgr.bots[i].active) return true;
    }
    return false;
}

bool botManagerGetBotInfo(const ServerSim *sim, BYTE playerNum, BotInfo *out) {
    if (out == NULL) return false;
    if (sim == NULL || playerNum >= MAX_TANKS ||
        !sim->botMgr.bots[playerNum].active) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    const BotContext *bot = &sim->botMgr.bots[playerNum];

    /* Active count drives the per-bot target the next tick will use.
     * The console command is rare; recompute on each call rather than
     * caching. */
    int active = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (sim->botMgr.bots[i].active) active++;
    }

    out->isBot        = true;
    out->hasBrain     = (bot->ai == aiFull) && bot->brain.running;
    out->lastThinkMs  = bot->lastThinkMs;
    out->targetMs     = botManagerComputePerBotTargetMs(sim, active);
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

void botManagerGetPoolStats(const ServerSim *sim, BotPoolStats *out) {
    if (out == NULL) return;
    if (sim == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }

    int      active        = 0;
    uint32_t totalOverruns = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (sim->botMgr.bots[i].active) {
            active++;
            totalOverruns += sim->botMgr.bots[i].overrunCount;
        }
    }

    out->workerCount      = botWorkerPoolGetSize();
    out->activeBots       = active;
    out->ewmaSerialMs     = sim->botMgr.serialMsEwma;
    out->currentTargetMs  = botManagerComputePerBotTargetMs(sim, active);
    out->lastBrainPhaseMs = sim->botMgr.lastBrainPhaseMs;
    out->ewmaBrainPhaseMs = sim->botMgr.brainPhaseMsEwma;
    out->lastSerialMs     = sim->botMgr.lastSerialMs;
    out->totalOverruns    = totalOverruns;
}

int botManagerGetActiveBotCount(const ServerSim *sim) {
    int active = 0;
    if (sim == NULL) return 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (sim->botMgr.bots[i].active) active++;
    }
    return active;
}

/* Gate + sink for the bot-comms debug log. Enabled by SetDefaultDebugMode
 * (-braindebug). Appends one line per call to botmsg_debug.log in the CWD;
 * a no-op when off, so production pays nothing. Opened per-call (low volume:
 * a few /info messages per second across all bots) to avoid a held handle. */
static bool g_botMsgDebugLog = false;

void botMsgDebugLog(const char *fmt, ...) {
    if (!g_botMsgDebugLog) return;
    FILE *f = fopen("botmsg_debug.log", "a");
    if (f == NULL) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void botManagerSetDefaultDebugMode(ServerSim *sim, bool enabled) {
    if (sim == NULL) return;
    sim->botMgr.defaultDebugMode = enabled;
    sim->botMgr.brainDebugMode   = enabled;
    g_botMsgDebugLog             = enabled;
}

bool botManagerToggleAllBrainDebugMode(ServerSim *sim) {
    if (sim == NULL) return false;
    sim->botMgr.brainDebugMode = !sim->botMgr.brainDebugMode;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (sim->botMgr.bots[i].active && sim->botMgr.bots[i].brain.running) {
            luaBrainInstanceSetDebugMode(&sim->botMgr.bots[i].brain,
                                         sim->botMgr.brainDebugMode);
        }
    }
    return sim->botMgr.brainDebugMode;
}

bool botManagerExecLua(ServerSim *sim, BYTE playerNum, const char *src) {
    if (sim == NULL || playerNum >= MAX_TANKS) return false;
    if (!sim->botMgr.bots[playerNum].active) return false;
    if (!sim->botMgr.bots[playerNum].brain.running) return false;
    lua_State *L = sim->botMgr.bots[playerNum].brain.L;
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

char *botManagerEvalLuaString(ServerSim *sim, BYTE playerNum, const char *src) {
    if (sim == NULL || playerNum >= MAX_TANKS) return NULL;
    if (!sim->botMgr.bots[playerNum].active) return NULL;
    if (!sim->botMgr.bots[playerNum].brain.running) return NULL;
    lua_State *L = sim->botMgr.bots[playerNum].brain.L;
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
            WB_LOG_WARN(WB_LOG_CAT_LUA, "brain %d: panel eval compile error: %s",
                    playerNum, lua_tostring(L, -1));
            sLastErrLogMs = now;
        }
        lua_pop(L, 1);
        return NULL;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        Uint64 now = SDL_GetTicks();
        if (now - sLastErrLogMs > 2000) {
            WB_LOG_WARN(WB_LOG_CAT_LUA, "brain %d: panel eval runtime error: %s",
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

bool botManagerGetGoalInfo(const ServerSim *sim, BYTE playerNum,
                           BrainGoalInfo *out) {
    lua_State *L;
    if (sim == NULL || playerNum >= MAX_TANKS) return false;
    if (!sim->botMgr.bots[playerNum].active) return false;
    L = sim->botMgr.bots[playerNum].brain.L;
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
