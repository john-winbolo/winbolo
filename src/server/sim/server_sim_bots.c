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
 *Name:          Server Simulation Bots
 *Filename:      server_sim_bots.c
 *Author:        John Morrison
 *Purpose:
 *  The bot-slot constructor and the wrappers that forward
 *  to bot_manager.c and bot_worker_pool.c. Callers reach
 *  the bot pool through the serverSim* names declared in
 *  server_sim.h, so nothing outside the server needs to
 *  include bot_manager.h itself.
 *********************************************************/

#include <string.h>

#include "server_sim_internal.h"
#include "bot_manager.h"
#include "bot_worker_pool.h"   /* botWorkerPoolDestroy */
#include "server_sim_join.h"   /* addPlayerInternal, fillAndPublishPlayerJoin, serverSimAssignLobbyStartOnJoin */

bool serverSimAddBot(ServerSim *sim, BYTE playerNum,
                     const ServerSimBotConfig *cfg) {
    if (cfg == NULL || cfg->brainPath == NULL) {
        return false;
    }
    if (playerNum >= MAX_TANKS) {
        return false;
    }
    if (sim->playerConnected[playerNum]) {
        return false;
    }

    /* Stamp PLAYER_FLAG_BOT before addPlayerInternal so the log_PlayerJoined
     * event it emits, and the CTRL_PLAYER_JOIN that fillAndPublishPlayerJoin
     * fans out, both carry the bot identity. A bot slot inherits no human
     * identity bits — set rather than OR. */
    playersSetClientFlags(&sim->sim.plyrs, playerNum, PLAYER_FLAG_BOT);
    addPlayerInternal(sim, playerNum, cfg->brainName, NULL, false);
    fillAndPublishPlayerJoin(sim, playerNum);

    sim->lobbyPlayers[playerNum].isBot      = true;
    sim->lobbyPlayers[playerNum].ready      = true;
    sim->lobbyPlayers[playerNum].teamNumber = cfg->teamNumber;
    /* Re-pick the reservation now the bot's final team is known — the
     * earlier pick in addPlayerInternal ran while it still held the
     * default team. The slot republishes on its next lobby change. */
    serverSimAssignLobbyStartOnJoin(sim, playerNum);
    return true;
}

void serverSimSetBotAiType(ServerSim *sim, aiType ai) {
    sim->botAiType = ai;
}

/* Bot pool wrappers — forward to bot_manager.c. Declarations
 * live in server_sim.h so non-server callers don't include
 * bot_manager.h directly. */

bool serverSimBotPoolInit(int threads) {
    return botManagerInit(threads);
}

void serverSimBotPoolDestroy(void) {
    botWorkerPoolDestroy();
}

void serverSimRequestBotThreads(ServerSim *sim, int total_runners) {
    botManagerRequestThreads(sim, total_runners);
}

int serverSimGetBotThreads(ServerSim *sim) {
    return botManagerGetThreads(sim);
}

int serverSimGetPendingBotThreads(ServerSim *sim) {
    return botManagerGetPendingThreads(sim);
}

void serverSimSetBotDefaultDebugMode(ServerSim *sim, bool enabled) {
    botManagerSetDefaultDebugMode(sim, enabled);
}

void serverSimSetBotPreThinkHook(ServerSim *sim,
                                 void (*hook)(int playerNum)) {
    botManagerSetPreThinkHook(sim, hook);
}

bool serverSimCreateBot(ServerSim *sim, BYTE playerNum,
                        const char *brainPath, const char *brainName,
                        aiType ai, gameType game, bool hiddenMines,
                        BYTE team, const ScnTable *init) {
    return botManagerAddBot(sim, playerNum, brainPath, brainName,
                            ai, game, hiddenMines, team, init);
}

void serverSimRemoveBot(ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    botManagerRemoveBot(sim, playerNum);
    serverSimPublishLobbySlot(sim, playerNum);
}

void serverSimDestroyBots(ServerSim *sim) {
    botManagerDestroy(sim);
}

void serverSimSetBotTeams(ServerSim *sim,
                          const BYTE *teamOf, BYTE numPlayers) {
    botManagerSetTeams(sim, teamOf, numPlayers);
}

void serverSimBotTick(ServerSim *sim, aiType ai) {
    botManagerTick(sim, ai);
}

BYTE serverSimGetNumBots(ServerSim *sim) {
    return botManagerGetNumBots(sim);
}

bool serverSimHasAnyBot(ServerSim *sim) {
    return botManagerHasAnyBot(sim);
}

bool serverSimIsBot(ServerSim *sim, BYTE playerNum) {
    return botManagerIsBot(sim, playerNum);
}

double serverSimGetBotLastThinkMs(ServerSim *sim, BYTE playerNum) {
    return botManagerGetLastThinkMs(sim, playerNum);
}

bool serverSimGetBotInfo(ServerSim *sim, BYTE playerNum, BotInfo *out) {
    return botManagerGetBotInfo(sim, playerNum, out);
}

void serverSimGetBotPoolStats(ServerSim *sim, BotPoolStats *out) {
    botManagerGetPoolStats(sim, out);
}

bool serverSimToggleAllBrainDebugMode(ServerSim *sim) {
    return botManagerToggleAllBrainDebugMode(sim);
}

int serverSimGetActiveBotCount(ServerSim *sim) {
    return botManagerGetActiveBotCount(sim);
}

BrainPathfinder *serverSimGetBotBrainPathfinder(ServerSim *sim, BYTE playerNum) {
    return botManagerGetBrainPathfinder(sim, playerNum);
}

OverlayCmdBuffer *serverSimGetBotOverlayCmds(ServerSim *sim, BYTE playerNum) {
    return botManagerGetOverlayCmds(sim, playerNum);
}

bool serverSimGetBotGoalInfo(ServerSim *sim, BYTE playerNum,
                             BrainGoalInfo *out) {
    return botManagerGetGoalInfo(sim, playerNum, out);
}

bool serverSimBotExecLua(ServerSim *sim, BYTE playerNum, const char *src) {
    return botManagerExecLua(sim, playerNum, src);
}

char *serverSimBotEvalLuaString(ServerSim *sim, BYTE playerNum,
                                const char *src) {
    return botManagerEvalLuaString(sim, playerNum, src);
}

bool serverSimBotSetLuaGlobalString(ServerSim *sim, BYTE playerNum,
                                    const char *name, const char *value) {
    return botManagerSetLuaGlobalString(sim, playerNum, name, value);
}

void serverSimSetBotBrainPath(ServerSim *sim, const char *path) {
    if (path == NULL || path[0] == '\0') {
        sim->botBrainPath[0] = '\0';
        return;
    }
    strncpy(sim->botBrainPath, path, sizeof(sim->botBrainPath) - 1);
    sim->botBrainPath[sizeof(sim->botBrainPath) - 1] = '\0';
}
