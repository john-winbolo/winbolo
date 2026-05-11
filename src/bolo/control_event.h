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
 *Name:          Control Event
 *Filename:      control_event.h
 *Author:        John Morrison
 *Purpose:
 *  In-memory tagged-union event delivered from a server
 *  simulation to its in-process subscribers (bot ClientSims
 *  and, in single-player, the human ClientSim). Carries the
 *  out-of-band roster / lobby / phase information that does
 *  not ride on snapshots. Has no wire encoding.
 *********************************************************/

#ifndef CONTROL_EVENT_H
#define CONTROL_EVENT_H

#include "global.h"
#include "netpacks.h"     /* PACKET_MAX_PLAYER_NAME */
#include "bolo_packets.h" /* netStatus, gameType */
#include "client_sim.h"   /* ClientLobbySlot */

typedef enum {
    CTRL_ALLIANCE_REQUEST,
    CTRL_ALLIANCE_ACCEPT,
    CTRL_ALLIANCE_LEAVE,
    CTRL_PLAYER_JOIN,
    CTRL_PLAYER_NAME,
    CTRL_LOBBY_SLOT,
    CTRL_LOBBY_SETTINGS,
    CTRL_LOBBY_MAP_CHANGE,
    CTRL_MAP_DOWNLOAD_COMPLETE,
    CTRL_BALANCE_PROPOSAL,
    CTRL_GAME_PHASE,
    CTRL_GAME_OVER,
    CTRL_SERVER_SHUTDOWN
} ControlEventType;

typedef enum {
    CTRL_PHASE_LOBBY,
    CTRL_PHASE_COUNTDOWN,
    CTRL_PHASE_RUNNING,
    CTRL_PHASE_GAME_OVER
} ControlGamePhase;

typedef struct {
    ControlEventType type;
    union {
        /* CTRL_ALLIANCE_REQUEST */
        struct {
            BYTE fromPlayer;
            BYTE toPlayer;
        } allianceRequest;

        /* CTRL_ALLIANCE_ACCEPT — acceptedBy invites newMember (a/b in plan) */
        struct {
            BYTE acceptedBy;
            BYTE newMember;
        } allianceAccept;

        /* CTRL_ALLIANCE_LEAVE — alliance leave, not player leave */
        struct {
            BYTE playerNum;
        } allianceLeave;

        /* CTRL_PLAYER_JOIN */
        struct {
            BYTE  playerNum;
            char  name[PACKET_MAX_PLAYER_NAME];
            char  country[3];           /* 2 chars + NUL */
            uint8_t clientType;
            uint8_t clientFlags;
            BYTE  numAllies;
            BYTE  allies[MAX_TANKS];
        } playerJoin;

        /* CTRL_PLAYER_NAME */
        struct {
            BYTE playerNum;
            char name[PACKET_MAX_PLAYER_NAME];
        } playerName;

        /* CTRL_LOBBY_SLOT */
        struct {
            BYTE             playerNum;
            ClientLobbySlot  slot;
        } lobbySlot;

        /* CTRL_LOBBY_SETTINGS */
        struct {
            char     mapName[MAP_STR_SIZE];
            gameType lobbyGameType;
            bool     lobbyHiddenMines;
            uint8_t  lobbyAiType;
            int32_t  lobbyTimeLimit;
            uint8_t  lobbyPillCount;
            uint8_t  lobbyBaseCount;
            uint8_t  lobbyStartCount;
            bool     mapSkipAvailable;
            netStatus netStat;
            bool     inLobby;
        } lobbySettings;

        /* CTRL_LOBBY_MAP_CHANGE — no payload fields needed */
        struct {
            uint8_t _unused;
        } lobbyMapChange;

        /* CTRL_MAP_DOWNLOAD_COMPLETE — no payload fields needed */
        struct {
            uint8_t _unused;
        } mapDownloadComplete;

        /* CTRL_BALANCE_PROPOSAL — proposed team per slot (0 = none) */
        struct {
            BYTE teamForSlot[MAX_TANKS];
        } balanceProposal;

        /* CTRL_GAME_PHASE */
        struct {
            ControlGamePhase phase;
            int countdownSeconds;       /* meaningful when phase == COUNTDOWN */
        } gamePhase;

        /* CTRL_GAME_OVER */
        struct {
            uint8_t _unused;
        } gameOver;

        /* CTRL_SERVER_SHUTDOWN */
        struct {
            uint8_t _unused;
        } serverShutdown;
    } u;
} ControlEvent;

#endif /* CONTROL_EVENT_H */
