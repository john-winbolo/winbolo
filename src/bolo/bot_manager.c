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

#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"
#include "players.h"
#include "mines.h"
#include "client_sim.h"
#include "transport.h"
#include "screen.h"
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
    ClientSim       cs;
    Transport       transport;
    LuaBrainInstance brain;
    BYTE            playerNum;
    bool            active;
    aiType          ai;
    /* Wall-clock duration of this bot's most recent brain.think call,
     * in milliseconds. Updated every botManagerTick. Surfaced via
     * botManagerGetLastThinkMs so HUDs / perf graphs can read it. */
    double          lastThinkMs;
} BotContext;

static BotContext bots[MAX_TANKS];
static int numBots = 0;

/* Per-bot last tick at which a slow-think warning was emitted, used to
 * rate-limit the warning to at most once per ~50 ticks. Type matches
 * ServerSim::tick (uint32_t) so the subtraction is well-defined. */
static uint32_t lastWarnTick[MAX_TANKS] = {0};

static void (*g_preThinkHook)(int playerNum) = NULL;

void botManagerSetPreThinkHook(void (*hook)(int playerNum)) {
    g_preThinkHook = hook;
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

    if (MY_TANK(&bot->cs) == NULL) {
        return;
    }

    tx = tankGetMX(&MY_TANK(&bot->cs));
    ty = tankGetMY(&MY_TANK(&bot->cs));

    if (bot->ai == aiFull) {
        /* aiFull: refresh full map every tick from server */
        screenBrainMapFillFromMap(&bot->cs, &sim->sim.mp, &sim->sim.mns);
        return;
    }

    left   = (tx > BOT_VIEW_HALF) ? tx - BOT_VIEW_HALF : 0;
    top    = (ty > BOT_VIEW_HALF) ? ty - BOT_VIEW_HALF : 0;
    right  = (tx + BOT_VIEW_HALF < 255) ? tx + BOT_VIEW_HALF : 255;
    bottom = (ty + BOT_VIEW_HALF < 255) ? ty + BOT_VIEW_HALF : 255;

    /* Update only the visible rect from server map */
    for (y = top; ; y++) {
        for (x = left; ; x++) {
            screenBrainMapSetPos(bot->cs.brainMap, (BYTE)x, (BYTE)y,
                                 mapGetPos(&sim->sim.mp, (BYTE)x, (BYTE)y),
                                 minesExistPos(&sim->sim.mns, &sim->sim.mp, (BYTE)x, (BYTE)y));
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

    ok = mapLoadCompressedMap(&bot->cs.sim.mp, &bot->cs.sim.pb,
                              &bot->cs.sim.bs, &bot->cs.sim.ss,
                              buf, len);
    free(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

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

    /* `threads` counts total runners including the producer; the pool
     * holds threads-1 workers and the producer runs one job inline. */
    int workers = threads - 1;
    if (workers > 0 && !botWorkerPoolCreate(workers)) {
        return false;
    }
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

    /* Register the player in the server (creates tank + lgm) */
    serverSimAddPlayer(sim, playerNum, brainName, false);

    /* Mark as bot in lobby state (must come after serverSimAddPlayer which resets defaults) */
    sim->lobbyPlayers[playerNum].isBot = true;
    sim->lobbyPlayers[playerNum].ready = true;  /* Bots are always ready */

    /* Set bot name in transport client array for lobby broadcasts */
    transportUdpServerSetBotName(playerNum, brainName);

    /* Create the bot's ClientSim */
    clientSimCreate(&bot->cs, game, hiddenMines, 0, -1);
    bot->cs.isBot = true;
    clientSimSetPlayerNum(&bot->cs, playerNum);

    /* Create a tank at slot 0 for this ClientSim */
    if (MY_TANK(&bot->cs) != NULL) {
        tankDestroy(&bot->cs.sim, &MY_TANK(&bot->cs));
        MY_TANK(&bot->cs) = NULL;
    }
    tankCreate(&bot->cs.sim, &MY_TANK(&bot->cs));

    /* Set this bot's identity */
    playersSetSelf(NULL, &bot->cs.sim, &bot->cs.sim.plyrs, playerNum,
                   (char *)brainName, TRUE);

    /* Load map data from the server */
    if (!botLoadMapFromServer(bot, sim)) {
        fprintf(stderr, "botManager: failed to load map for bot %d\n", playerNum);
        clientSimDestroy(&bot->cs);
        serverSimRemovePlayer(sim, playerNum);
        return false;
    }

    /* Set AI type on the ClientSim */
    bot->cs.allowComputerTanks = ai;

    /* Create passive transport (does NOT tick the server) */
    bot->transport = transportLocalCreatePassive(sim, playerNum);

    /* Initialize the brain map (fog-of-war) */
    /* screenBrainMapCreate already called by clientSimCreate,
     * which sets brainMap to TERRAIN_UNKNOWN. The bot's sim.brainMap
     * pointer is already set. */

    /* Create the Lua brain instance */
    if (!luaBrainInstanceCreate(&bot->brain, brainPath, brainName,
                                &bot->cs, ai)) {
        fprintf(stderr, "botManager: failed to create brain for bot %d\n", playerNum);
        transportLocalDestroy(&bot->transport);
        clientSimDestroy(&bot->cs);
        serverSimRemovePlayer(sim, playerNum);
        return false;
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
} BotJobCtx;

static BotJobCtx s_jobs[MAX_TANKS];
static int       s_jobIndices[MAX_TANKS];

static void runBotThinkJob(int botIndex, void *userData) {
    (void)userData;
    BotJobCtx *j = &s_jobs[botIndex];
    BotContext *bot = j->bot;
    ServerSim  *sim = j->sim;

    /* Optional pre-think hook (BrainTest viz). NULL in WinBoloDS.
     * Set once at host init and never modified after, so reading the
     * function pointer here from a worker thread is safe. */
    if (g_preThinkHook) g_preThinkHook(botIndex);

    Uint64 t0 = SDL_GetPerformanceCounter();
    bool ok = luaBrainInstanceTick(&bot->brain);
    Uint64 t1 = SDL_GetPerformanceCounter();

    if (g_preThinkHook) g_preThinkHook(-1);

    bot->lastThinkMs = (double)(t1 - t0) * 1000.0
                       / (double)SDL_GetPerformanceFrequency();

    if (!ok) {
        /* Producer handles botManagerRemoveBot in the input-send stage —
         * never call it from a worker thread. */
        j->needRemove = true;
        return;
    }

    if (MY_TANK(&bot->cs) != NULL) {
        MY_TANK(&bot->cs)->newTank = FALSE;
    }

    /* Build both InputPackets into per-bot scratch — packets are
     * thread-local to this ctx so two workers cannot collide. */
    bool firstIsGame = (sim->tick % 2) == 0;
    screenBuildInputPacketCS(&bot->cs, &j->pkt1, 0, FALSE, FALSE,
                             TRUE, firstIsGame, bot->playerNum,
                             sim->tick);
    screenBuildInputPacketCS(&bot->cs, &j->pkt2, 0, FALSE, FALSE,
                             TRUE, !firstIsGame, bot->playerNum,
                             sim->tick + 1);
    j->hasInput = true;
}

void botManagerTick(ServerSim *sim, aiType ai) {
    int activeCount = 0;

    /* ---- Stage 1: snapshot/sync (serial, on producer thread) ---- */
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        BotContext *bot = &bots[i];
        if (!bot->active) continue;
        if (sim->sim.tanks[i] == NULL) continue;

        BotJobCtx *j = &s_jobs[i];
        j->bot = bot;
        j->sim = sim;
        j->needRemove = false;
        j->hasInput   = false;

        bot->transport.getSnapshot(bot->transport.ctx, bot->playerNum,
                                   &j->hdr, j->tanks, MAX_TANKS,
                                   j->shells, MAX_SNAPSHOT_SHELLS,
                                   j->tkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                                   j->bases, MAX_SNAPSHOT_BASES,
                                   j->pills, MAX_SNAPSHOT_PILLS,
                                   j->events, MAX_SNAPSHOT_EVENTS);

        clientSimSyncFromSnapshot(&bot->cs, &j->hdr,
                                  j->tanks, j->hdr.tankCount,
                                  j->shells, j->hdr.shellCount,
                                  j->tkExplosions, j->hdr.tkExplosionCount,
                                  j->bases, j->hdr.baseCount,
                                  j->pills, j->hdr.pillCount,
                                  j->events, j->hdr.reliableEventCount,
                                  bot->playerNum);

        if (bot->ai == aiFull && bot->brain.isFirst) {
            /* Full map on first tick */
            screenBrainMapFillFromMap(&bot->cs, &sim->sim.mp, &sim->sim.mns);
        }
        botUpdateBrainMap(bot, sim);

        /* Skip brain while tank is dead (waiting to respawn). Excluding
         * dead bots here keeps the worker job branch-free on liveness
         * and stops a respawning bot from inheriting another bot's queue
         * slot mid-dispatch. */
        if (MY_TANK(&bot->cs) != NULL &&
            tankGetDeathWait(&MY_TANK(&bot->cs)) > 0) {
            continue;
        }

        /* Reset key state before brain runs */
        bot->cs.brainHoldKeys = 0;
        bot->cs.brainTapKeys = 0;
        if (bot->cs.brainBuildInfo != NULL) {
            bot->cs.brainBuildInfo->action = 0;
        }

        s_jobIndices[activeCount++] = i;
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
    if (anyFirstTick) {
        for (int k = 0; k < activeCount; k++) {
            runBotThinkJob(s_jobIndices[k], NULL);
        }
    } else {
        botWorkerPoolRun(s_jobIndices, activeCount, runBotThinkJob, NULL);
    }

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
        if (!j->hasInput) {
            continue;
        }

        bots[i].transport.sendInput(bots[i].transport.ctx, &j->pkt1);
        bots[i].transport.sendInput(bots[i].transport.ctx, &j->pkt2);

        /* Slow-tick warning: now reads bot->lastThinkMs (worker-written,
         * visible via the pool's release/acquire), so logging never
         * happens on a worker thread. Same 5.0 ms threshold and 50-tick
         * rate limit as the previous serial path. */
        double ms = bots[i].lastThinkMs;
        if (ms > 5.0 && (sim->tick - lastWarnTick[i]) > 50) {
            WB_LOG_WARN(WB_LOG_CAT_LUA,
                        "bot %d think took %.1fms (tick %u)", i, ms, sim->tick);
            lastWarnTick[i] = sim->tick;
        }
    }
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
        if (MY_TANK(&bot->cs) != NULL) {
            tankDestroy(&bot->cs.sim, &MY_TANK(&bot->cs));
            MY_TANK(&bot->cs) = NULL;
        }
        tankCreate(&bot->cs.sim, &MY_TANK(&bot->cs));

        /* Reset brain so full-map fill triggers again for aiFull bots */
        bot->brain.isFirst = true;
    }
}

void botManagerRemoveBot(ServerSim *sim, BYTE playerNum) {
    BotContext *bot;
    if (playerNum >= MAX_TANKS) return;
    bot = &bots[playerNum];
    if (!bot->active) return;

    luaBrainInstanceDestroy(&bot->brain);
    transportLocalDestroy(&bot->transport);
    clientSimDestroy(&bot->cs);
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
