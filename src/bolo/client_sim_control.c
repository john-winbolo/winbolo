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
 *Name:          Client Sim Control
 *Filename:      client_sim_control.c
 *Purpose:
 *  Dispatcher implementation. Mutates ClientSim game state
 *  only — UI, achievement, transport-internal, and wire-
 *  protocol housekeeping live in their respective callers.
 *********************************************************/

#include <string.h>
#include "client_sim_control.h"
#include "players.h"

void clientSimApplyControl(ClientSim *cs, const ControlEvent *evt) {
    if (cs == NULL || evt == NULL) {
        return;
    }

    /* Self-skip on identity-shaped events: the recipient's own slot is
     * initialized by the code that creates the ClientSim and must not
     * be overwritten by sync or live publish. */
    switch (evt->type) {
    case CTRL_PLAYER_JOIN:
        if (evt->u.playerJoin.playerNum == cs->myPlayerNum) return;
        break;
    case CTRL_PLAYER_NAME:
        if (evt->u.playerName.playerNum == cs->myPlayerNum) return;
        break;
    case CTRL_LOBBY_SLOT:
        if (evt->u.lobbySlot.playerNum == cs->myPlayerNum) return;
        break;
    default:
        break;
    }

    switch (evt->type) {
    case CTRL_ALLIANCE_REQUEST:
        /* No state mutation — request is UI/dialog territory. */
        break;

    case CTRL_ALLIANCE_ACCEPT:
        playersAcceptAlliance(&cs->sim, &cs->sim.plyrs, cs->myPlayerNum,
                              evt->u.allianceAccept.acceptedBy,
                              evt->u.allianceAccept.newMember,
                              FALSE);
        break;

    case CTRL_ALLIANCE_LEAVE:
        playersLeaveAlliance(&cs->sim, &cs->sim.plyrs, cs->myPlayerNum,
                             evt->u.allianceLeave.playerNum, FALSE);
        break;

    case CTRL_PLAYER_JOIN: {
        BYTE pNum = evt->u.playerJoin.playerNum;
        char nameBuf[PACKET_MAX_PLAYER_NAME];
        char ccBuf[3];
        BYTE allies[MAX_TANKS];
        BYTE numAllies = evt->u.playerJoin.numAllies;
        memcpy(nameBuf, evt->u.playerJoin.name, sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        ccBuf[0] = evt->u.playerJoin.country[0];
        ccBuf[1] = evt->u.playerJoin.country[1];
        ccBuf[2] = '\0';
        if (numAllies > MAX_TANKS) numAllies = MAX_TANKS;
        if (numAllies > 0) {
            memcpy(allies, evt->u.playerJoin.allies, numAllies);
        }
        playersSetClientType(&cs->sim.plyrs, pNum, evt->u.playerJoin.clientType);
        playersSetClientFlags(&cs->sim.plyrs, pNum, evt->u.playerJoin.clientFlags);
        playersSetPlayer(cs, &cs->sim.plyrs, cs->myPlayerNum, pNum,
                         nameBuf, ccBuf,
                         0, 0, 0, 0, 0, FALSE,
                         numAllies, numAllies > 0 ? allies : NULL, FALSE);
        break;
    }

    case CTRL_PLAYER_NAME: {
        char nameBuf[PACKET_MAX_PLAYER_NAME];
        memcpy(nameBuf, evt->u.playerName.name, sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        playersSetPlayerName(cs, &cs->sim, &cs->sim.plyrs, cs->myPlayerNum,
                             evt->u.playerName.playerNum, nameBuf, FALSE);
        break;
    }

    case CTRL_LOBBY_SLOT:
        cs->lobbySlots[evt->u.lobbySlot.playerNum] = evt->u.lobbySlot.slot;
        break;

    case CTRL_LOBBY_SETTINGS:
        strncpy(cs->mapName, evt->u.lobbySettings.mapName, MAP_STR_SIZE - 1);
        cs->mapName[MAP_STR_SIZE - 1] = '\0';
        cs->lobbyGameType    = evt->u.lobbySettings.lobbyGameType;
        cs->lobbyHiddenMines = evt->u.lobbySettings.lobbyHiddenMines;
        cs->lobbyAiType      = evt->u.lobbySettings.lobbyAiType;
        cs->lobbyTimeLimit   = evt->u.lobbySettings.lobbyTimeLimit;
        cs->lobbyPillCount   = evt->u.lobbySettings.lobbyPillCount;
        cs->lobbyBaseCount   = evt->u.lobbySettings.lobbyBaseCount;
        cs->lobbyStartCount  = evt->u.lobbySettings.lobbyStartCount;
        cs->mapSkipAvailable = evt->u.lobbySettings.mapSkipAvailable;
        cs->netStat          = evt->u.lobbySettings.netStat;
        cs->inLobby          = evt->u.lobbySettings.inLobby;
        break;

    case CTRL_LOBBY_MAP_CHANGE:
        cs->mapDownloadComplete = false;
        memset(cs->mapSkipVotes, 0, sizeof(cs->mapSkipVotes));
        cs->mapSkipMyVote = false;
        break;

    case CTRL_GAME_PHASE:
        switch (evt->u.gamePhase.phase) {
        case CTRL_PHASE_LOBBY:
            cs->netStat = netLobby;
            cs->countdownSeconds = 0;
            break;
        case CTRL_PHASE_COUNTDOWN:
            cs->netStat = netLobbyCountdown;
            cs->countdownSeconds = evt->u.gamePhase.countdownSeconds;
            break;
        case CTRL_PHASE_RUNNING:
            cs->netStat = netRunning;
            cs->countdownSeconds = 0;
            break;
        case CTRL_PHASE_GAME_OVER:
            /* Lobby-branch reset; non-lobby end-of-game is transport-internal. */
            if (cs->inLobby) {
                cs->netStat = netLobby;
                cs->countdownSeconds = 0;
            }
            break;
        }
        break;

    case CTRL_GAME_OVER:
        if (cs->inLobby) {
            cs->netStat = netLobby;
            cs->countdownSeconds = 0;
            cs->lobbyChatHistory[0] = '\0';
        }
        break;

    case CTRL_SERVER_SHUTDOWN:
        /* No ClientSim field maps to UDP joinState; that field stays
         * transport-internal per the architectural commitment. */
        break;
    }
}
