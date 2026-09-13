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
#include "server_sim_shared.h"  /* serverSimSetActive — the fielding path's tank build */
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
        /* The one occupied seat an add may land on is one held for a bot
           that is not on the field yet, and landing on it is what fields
           it. The roster already holds the seat, its name and its team, and
           every client was told about all three when it was seated, so
           there is no join to announce and nothing to pick again. What the
           seat lacks is the tank, which a fielding inside a round has to
           build here — a fielding in the lobby gets one from the start
           sequence with everybody else's. */
        if (sim->lobbyPlayers[playerNum].fielded) {
            return false;
        }
        playersSetClientFlags(&sim->sim.plyrs, playerNum, PLAYER_FLAG_BOT);
        sim->lobbyPlayers[playerNum].fielded = true;
        sim->lobbyPlayers[playerNum].isBot   = true;
        sim->lobbyPlayers[playerNum].ready   = true;
        if (sim->state == serverStateRunning &&
            sim->sim.tanks[playerNum] == NULL) {
            serverSimSetActive(sim);
            tankCreate(&sim->sim, &sim->sim.tanks[playerNum]);
            sim->sim.lgmen[playerNum] = lgmCreate(playerNum);
            basesUpdateTimer(&sim->sim, playerNum);
        }
        return true;
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

bool serverSimAddUnfieldedSeat(ServerSim *sim, BYTE playerNum,
                               const char *name, BYTE teamNumber) {
    if (sim == NULL || playerNum >= MAX_TANKS || name == NULL) {
        return false;
    }
    if (sim->playerConnected[playerNum]) {
        return false;
    }

    /* The same stamp a fielded bot gets, for the same reason: the join event
       below carries the bot identity. */
    playersSetClientFlags(&sim->sim.plyrs, playerNum, PLAYER_FLAG_BOT);
    addPlayerInternal(sim, playerNum, name, NULL, false);
    fillAndPublishPlayerJoin(sim, playerNum);

    sim->lobbyPlayers[playerNum].isBot      = true;
    sim->lobbyPlayers[playerNum].ready      = true;
    sim->lobbyPlayers[playerNum].teamNumber = teamNumber;
    sim->lobbyPlayers[playerNum].fielded    = false;
    sim->lobbyPlayers[playerNum].keepSeat   = true;
    /* A seat outside a round still reserves a start, so the bot that fields
       it lands with the rest of its team rather than wherever is free at the
       moment the wave arrives. Re-picked with the final team in hand, as the
       fielded add does. */
    serverSimAssignLobbyStartOnJoin(sim, playerNum);
    /* addPlayerInternal builds a tank for a slot that joins mid-round, and
       arms that slot's base restock cycle with it. This seat is not on the
       field, so both go back: a seat with no tank must not go on speeding
       the bases up for everyone who has one. */
    if (sim->sim.tanks[playerNum] != NULL) {
        tankDestroy(&sim->sim, &sim->sim.tanks[playerNum]);
        sim->sim.tanks[playerNum] = NULL;
    }
    if (sim->sim.lgmen[playerNum] != NULL) {
        lgmDestroy(&sim->sim.lgmen[playerNum]);
        sim->sim.lgmen[playerNum] = NULL;
    }
    basesRemoveTimer(&sim->sim, (int)playerNum);
    serverSimPublishLobbySlot(sim, playerNum);
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
    if (botManagerIsBot(sim, playerNum)) {
        botManagerRemoveBot(sim, playerNum);
    } else if (sim->playerConnected[playerNum] &&
               sim->lobbyPlayers[playerNum].isBot) {
        /* A seat held for a bot with nothing behind it to tear down. The
           roster entry is the whole of it, so taking it out is the leave
           path on its own. */
        serverSimRemovePlayer(sim, playerNum);
    }
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
    if (botManagerIsBot(sim, playerNum)) return true;
    /* A seat held for a bot that has not been fielded yet has no bot manager
       entry to find, and is a bot seat all the same: it is what the roster
       draws, what the human count leaves out and what a host may remove. */
    if (sim == NULL || playerNum >= MAX_TANKS) return false;
    return sim->playerConnected[playerNum] && sim->lobbyPlayers[playerNum].isBot;
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

void serverSimSetBotBrainPath(ServerSim *sim, const char *path) {
    if (path == NULL || path[0] == '\0') {
        sim->botBrainPath[0] = '\0';
        return;
    }
    strncpy(sim->botBrainPath, path, sizeof(sim->botBrainPath) - 1);
    sim->botBrainPath[sizeof(sim->botBrainPath) - 1] = '\0';
}
