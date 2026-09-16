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

#include <SDL3/SDL.h>          /* SDL_GetPathInfo — the warm's brain-path test */

#include "server_sim_internal.h"
#include "server_sim_shared.h"  /* serverSimSetActive — the fielding path's tank build */
#include "bot_manager.h"
#include "bot_worker_pool.h"   /* botWorkerPoolDestroy */
#include "server_sim_join.h"   /* addPlayerInternal, fillAndPublishPlayerJoin, serverSimAssignLobbyStartOnJoin */
#include "server_sim_scenario.h"  /* serverSimAddUnfieldedSeat and
                                   * serverSimUnfieldBot, which are defined
                                   * here and declared for a scenario */
#include "../../common/wb_log.h"  /* WB_LOG_WARN — the skipped-seat line */

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

void serverSimUnfieldBot(ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    if (!sim->playerConnected[playerNum]) return;
    if (!sim->lobbyPlayers[playerNum].fielded) return;

    /* The mirror of serverSimAddBot's occupied-seat branch above: that one
       builds the tank, the man and the base timer for a seat the roster
       already holds, and this one takes the same three back. Everything the
       roster knows stays — the connection, the players-table identity, the
       team, the alliance — so there is no leave to announce, nothing the
       seat owns changes hands, and no client has to be resynced to find
       that out.

       The bot that was driving the tank does not go with it. The tank has
       to: it leaves the world. What was behind it does not — the ClientSim,
       the control subscription and the brain instance are parked in the bot
       pool and handed back to the next spawn that fields this seat, so a
       wave transition costs the tank and not a VM per seat. The parked
       runner is released when the round ends, if the seat leaves the roster,
       or if the refield names a different brain or a different init table
       (bot_manager.c). */
    if (botManagerIsBot(sim, playerNum)) {
        botManagerRemoveBotKeepSeat(sim, playerNum);
    }
    /* As the fielding path does before it builds one: the routing the tank
       teardown reaches out through reads the active sim. */
    serverSimSetActive(sim);
    if (sim->sim.tanks[playerNum] != NULL) {
        tankDestroy(&sim->sim, &sim->sim.tanks[playerNum]);
        sim->sim.tanks[playerNum] = NULL;
    }
    if (sim->sim.lgmen[playerNum] != NULL) {
        /* Put down the pillbox he was carrying before he goes, the same as
           the leave path does (issue #340). tankDestroy above drops the
           tank's own cargo, but a pillbox handed to the man has already left
           that list — it exists only in his hands, and deleting him without
           this loses it for the rest of the round: the record stays marked as
           carried, so it is neither on the map nor anyone's to pick up. It
           matters more here than on a leave, because a leave migrates what
           the slot owned and this deliberately does not — a pill stranded
           here would stay under the name of a seat that is off the field. It
           lands owned by the seat, which is what "nothing the seat owns
           changes hands" means for a pillbox. */
        lgmDropCarriedPill(&sim->sim, &sim->sim.lgmen[playerNum]);
        lgmDestroy(&sim->sim.lgmen[playerNum]);
        sim->sim.lgmen[playerNum] = NULL;
    }
    /* A seat with no tank must not go on speeding the bases up for everyone
       who has one — the same reason serverSimAddUnfieldedSeat takes the
       timer back. */
    basesRemoveTimer(&sim->sim, (int)playerNum);

    /* The queue the bot that just went left behind: kept, its last few
       inputs would be applied to the tank the next fielding builds. */
    sim->inputQueueHead[playerNum] = 0;
    sim->inputQueueTail[playerNum] = 0;
    sim->lastProcessedInput[playerNum] = 0;
    sim->lastInputButtons[playerNum] = 0;
    sim->lastActionAppliedTick[playerNum] = 0;
    sim->pendingHarvestActions[playerNum] = 0;
    sim->pendingHarvestBuildAction[playerNum] = 0;
    sim->pendingHarvestBuildX[playerNum] = 0;
    sim->pendingHarvestBuildY[playerNum] = 0;
    sim->inputBufferFilled[playerNum] = 0;
    sim->inputDryTicks[playerNum] = 0;

    sim->lobbyPlayers[playerNum].fielded = false;
    serverSimPublishLobbySlot(sim, playerNum);
}

/* The brain a held seat would run if something fielded it now: the one its
   team was written with, falling back to the server's. Answers NULL when
   neither resolves to a file on disk, which is the test the spawn arm makes
   before it builds anything — a path that will not resolve there must not
   resolve here either, or the warm would build a runner the spawn refuses. */
static const char *warmSeatBrainPath(const ServerSim *sim, BYTE slot) {
    const char *path;
    SDL_PathInfo info;

    path = (sim->seatBrain[slot][0] != '\0') ? sim->seatBrain[slot]
                                             : serverSimGetBotBrainPath(sim);
    if (path == NULL || path[0] == '\0') return NULL;
    /* A brain carried inside the scenario, which nothing loads yet. */
    if (SDL_strncmp(path, "package:", 8) == 0) return NULL;
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) {
        return NULL;
    }
    return path;
}

bool serverSimWarmOneHeldSeat(ServerSim *sim) {
    BYTE i;

    if (sim == NULL) return false;
    /* A server with no bot AI runs no brains, which is the same answer the
       spawn arm gives a script on such a server. */
    if (serverSimGetBotAiType(sim) == aiNone) return false;

    for (i = 0; i < MAX_TANKS; i++) {
        char        name[PLAYER_NAME_LEN];
        const char *brain;

        if (!sim->playerConnected[i]) continue;
        if (!sim->lobbyPlayers[i].keepSeat) continue;
        if (sim->lobbyPlayers[i].fielded) continue;
        /* One this pass has already refused: the answer cannot change while
           the countdown runs, and the line naming it has been written. */
        if (sim->warmSkippedSlots & (uint16_t)(1u << i)) continue;
        /* One that already has a runner — warmed on an earlier tick, or
           parked by a fielding this round. */
        if (botManagerHasRunner(sim, i)) continue;

        brain = warmSeatBrainPath(sim, i);
        if (brain == NULL) {
            /* Skipped, and the seats after it are still warmed: a seat whose
               brain has gone costs that seat's wave a build, not the round
               its warm. */
            sim->warmSkippedSlots |= (uint16_t)(1u << i);
            WB_LOG_WARN(WB_LOG_CAT_SIM,
                        "scenario: seat %d names no brain that loads; its "
                        "runner is not being built ahead of the round",
                        (int)i);
            continue;
        }
        playersGetPlayerName(&sim->sim.plyrs, i, name, sizeof(name), TRUE);
        /* The seat's own table, so the runner is built with what the wave
           spawning this seat will carry and the fielding is a resume. */
        if (botManagerWarmRunner(sim, i, brain, name,
                                 serverSimGetBotAiType(sim),
                                 &sim->seatInit[i])) {
            return true;
        }
        /* The build itself failed and said so. Nothing else this tick — a
           second seat would put two builds in one frame, which is the whole
           thing this pass exists to avoid. */
        sim->warmSkippedSlots |= (uint16_t)(1u << i);
        return false;
    }
    return false;
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
        /* A seat held for a bot, which the pool does not see: it has no
           active entry to remove. What it can still have is a runner parked
           across an unfielding, and the seat is leaving, so that goes with
           it — a no-op for a seat that was never fielded. The roster entry
           is the rest of it, and taking that out is the leave path on its
           own. */
        botManagerReleaseParkedRunner(sim, playerNum);
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
