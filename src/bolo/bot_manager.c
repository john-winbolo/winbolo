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
#include <lua.h>
#include <lauxlib.h>   /* luaL_loadstring for botManagerExecLua */
#include "transport_udp.h"
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

void botManagerInit(void) {
    memset(bots, 0, sizeof(bots));
    numBots = 0;
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

void botManagerTick(ServerSim *sim, aiType ai) {
    BYTE i;
    SnapshotHeader hdr;
    TankSnapshot tanks[MAX_TANKS];
    ShellSnapshot shells[MAX_SNAPSHOT_SHELLS];
    BaseSnapshot bases[MAX_SNAPSHOT_BASES];
    PillSnapshot pills[MAX_SNAPSHOT_PILLS];
    GameEvent events[MAX_SNAPSHOT_EVENTS];
    InputPacket pkt;

    for (i = 0; i < MAX_TANKS; i++) {
        BotContext *bot = &bots[i];
        TkExplosionSnapshot tkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
        if (!bot->active) continue;
        if (sim->sim.tanks[i] == NULL) continue;

        /* Get snapshot from server for this bot's player */
        bot->transport.getSnapshot(bot->transport.ctx, bot->playerNum,
                                   &hdr, tanks, MAX_TANKS,
                                   shells, MAX_SNAPSHOT_SHELLS,
                                   tkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                                   bases, MAX_SNAPSHOT_BASES,
                                   pills, MAX_SNAPSHOT_PILLS,
                                   events, MAX_SNAPSHOT_EVENTS);

        /* Sync the bot's ClientSim from the snapshot */
        clientSimSyncFromSnapshot(&bot->cs, &hdr,
                                  tanks, hdr.tankCount,
                                  shells, hdr.shellCount,
                                  tkExplosions, hdr.tkExplosionCount,
                                  bases, hdr.baseCount,
                                  pills, hdr.pillCount,
                                  events, hdr.reliableEventCount,
                                  bot->playerNum);

        /* Update brain map (fog-of-war) from server's authoritative map.
         * The bot's ClientSim map doesn't receive terrain change events
         * (boat placements, building destruction, etc.) so we must read
         * from the ServerSim. */
        if (bot->ai == aiFull && bot->brain.isFirst) {
            /* Full map on first tick */
            screenBrainMapFillFromMap(&bot->cs, &sim->sim.mp, &sim->sim.mns);
        }
        botUpdateBrainMap(bot, sim);

        /* Skip brain while tank is dead (waiting to respawn) */
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

        /* Run the brain */
        {
            Uint64 t0 = SDL_GetPerformanceCounter();
            bool ok = luaBrainInstanceTick(&bot->brain);
            Uint64 t1 = SDL_GetPerformanceCounter();
            double ms = (double)(t1 - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
            bot->lastThinkMs = ms;
            if (ms > 5.0) {
                fprintf(stderr, "serverBrains: bot %d think took %.1fms\n", i, ms);
            }
            if (!ok) {
                fprintf(stderr, "botManager: bot %d brain tick failed, removing\n", i);
                botManagerRemoveBot(sim, i);
                continue;
            }
        }

        /* Clear newTank on the bot's ClientSim tank after the brain has
         * seen it.  The snapshot sync doesn't carry newTank, so without
         * this the flag stays TRUE forever and the brain sees perpetual
         * respawns. */
        if (MY_TANK(&bot->cs) != NULL) {
            MY_TANK(&bot->cs)->newTank = FALSE;
        }

        /* Build and send 2 input packets (keys tick + game tick).
         * sim->tick is the first tick about to be processed. The parity
         * determines keys-only vs full-game processing:
         *   even tick → game tick
         *   odd tick  → keys tick */
        {
            bool firstIsGame = (sim->tick % 2) == 0;

            screenBuildInputPacketCS(&bot->cs, &pkt, 0, FALSE, FALSE,
                                    TRUE, firstIsGame, bot->playerNum,
                                    sim->tick);
            bot->transport.sendInput(bot->transport.ctx, &pkt);

            screenBuildInputPacketCS(&bot->cs, &pkt, 0, FALSE, FALSE,
                                    TRUE, !firstIsGame, bot->playerNum,
                                    sim->tick + 1);
            bot->transport.sendInput(bot->transport.ctx, &pkt);
        }
    }
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
     * "no fresh data, keep showing the previous text". */
    if (luaL_loadstring(L, src) != LUA_OK) {
        lua_pop(L, 1);
        return NULL;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
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
