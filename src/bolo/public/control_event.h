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
#include "wire_limits.h"  /* PACKET_MAX_PLAYER_NAME */
#include "client_enums.h" /* netStatus, gameType */
#include "client_sim.h"   /* ClientLobbySlot */
#include "brain_list.h"   /* BrainList for CTRL_LOBBY_BRAIN_LIST */

#ifndef LOBBY_TEAM_NAME_LEN
#define LOBBY_TEAM_NAME_LEN 32
#endif

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
    CTRL_MAP_SKIP_STATE,
    CTRL_GAME_PHASE,
    CTRL_GAME_OVER,
    CTRL_SERVER_SHUTDOWN,
    CTRL_CHAT,
    CTRL_PLAYER_LEAVE,
    /* Lobby state-change variants — per-team metadata, per-bot config,
     * per-bot brain path, brain-list catalogue. */
    CTRL_LOBBY_TEAM_META,
    CTRL_LOBBY_BOT_CONFIG,
    CTRL_LOBBY_BOT_BRAIN,
    CTRL_LOBBY_BRAIN_LIST,
    CTRL_GAME_VOTE_STATE,
    CTRL_SERVER_TEXT,
    CTRL_EVENT_TYPE_COUNT   /* sentinel — must stay last */
} ControlEventType;

/* Body capacity for CTRL_CHAT.  Worst case is the localized server
 * message: 2 langid + 1 argCount + 4 * (1 lenByte + (PLAYER_NAME_LEN-1)
 * name bytes) = 263 bytes; rounded up for headroom. fromPlayer and
 * destPlayer are separate struct fields, not part of body[]. */
#define CHAT_BODY_MAX 272

typedef enum {
    CTRL_PHASE_LOBBY,
    CTRL_PHASE_COUNTDOWN,
    CTRL_PHASE_RUNNING,
    CTRL_PHASE_GAME_OVER
} ControlGamePhase;

typedef struct ControlEvent {
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

        /* CTRL_PLAYER_LEAVE — server announces a player has disconnected.
         * Wire counterpart is PACKET_PLAYER_LEFT. */
        struct {
            BYTE playerNum;
            char name[PACKET_MAX_PLAYER_NAME];
            char country[3];            /* 2 chars + NUL */
        } playerLeave;

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
            /* Layout A flags */
            bool     lobbyOpenHost;
            bool     lobbyAutoLockOnGameStart;
            bool     lobbyRanked;
            bool     lobbyAllowNewPlayers;
            bool     lobbyWbnAvailable;  /* host's winbolonetIsRunning() —
                                          * gates WBN-only UI (Balance
                                          * from WBN) on remote clients */
            uint16_t lobbyServerLocks;
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

        /* CTRL_MAP_SKIP_STATE — one byte per slot, 0 or 1, mirrors wire */
        struct {
            BYTE votes[MAX_TANKS];
        } mapSkipState;

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

        /* CTRL_CHAT — server-fanned PACKET_CHAT_BROADCAST.  fromPlayer
         * discriminates the body interpretation: 0..MAX_TANKS-1 = real
         * player chat (raw text), 0xFE = server raw English, 0xFF =
         * server localized (packed langid+args).  body[] is opaque to
         * the codec; consumers interpret it according to fromPlayer.
         * destPlayer is 0xFF for broadcast or a slot index for unicast. */
        struct {
            BYTE     fromPlayer;
            BYTE     destPlayer;
            uint16_t bodyLen;
            uint8_t  body[CHAT_BODY_MAX];
        } chat;

        /* CTRL_LOBBY_TEAM_META — per-team presentation (name, color,
         * naming pool, in_use). teamId 0 is the unassigned sentinel
         * and is never carried by this event. */
        struct {
            uint8_t teamId;        /* 1..MAX_TANKS-1 */
            uint8_t in_use;
            uint8_t color;
            uint8_t namingPool;
            char    name[LOBBY_TEAM_NAME_LEN];
        } lobbyTeamMeta;

        /* CTRL_LOBBY_BOT_CONFIG — per-bot difficulty/personality +
         * the display name pulled from the players table at fill
         * time. (Name is informational here — players.c remains the
         * source of truth via CTRL_PLAYER_NAME / lobbySlot.) */
        struct {
            uint8_t slot;
            uint8_t difficulty;
            uint8_t personality;
            char    name[PACKET_MAX_PLAYER_NAME];
        } lobbyBotConfig;

        /* CTRL_LOBBY_BOT_BRAIN — per-bot brain script path. Empty
         * path means "fall back to the server's global bot brain". */
        struct {
            uint8_t slot;
            char    path[BRAIN_LIST_PATH_LEN];
        } lobbyBotBrain;

        /* CTRL_LOBBY_BRAIN_LIST — server's discovered brain catalogue,
         * used to populate the AiConfig combobox. */
        struct {
            BrainList list;
        } lobbyBrainList;

        /* CTRL_SERVER_TEXT — server-originated chat broadcast.
         * Mirrors what UDP clients receive as
         * PACKET_CHAT_BROADCAST(fromPlayer=0xFE). Lets in-process
         * subscribers (SP / host) see the same lines. */
        struct {
            char text[PACKET_MAX_CHAT_MESSAGE + 1];
        } serverText;

        /* CTRL_GAME_VOTE_STATE — mirrors PACKET_GAME_VOTE_STATE. */
        struct {
            uint8_t  kind;             /* GAME_VOTE_KIND_* */
            uint8_t  active;           /* GAME_VOTE_ACTIVE_* */
            uint8_t  triggerSrc;       /* GAME_VOTE_TRIGGER_* */
            uint8_t  teamId;           /* surrender only; 0 = all-teams */
            uint8_t  threshold;        /* yes-count needed to pass */
            uint8_t  yesCount;
            uint8_t  noCount;
            uint8_t  eligibleCount;
            uint8_t  secondsRemaining; /* 0..60 */
            uint16_t votes;            /* bitmask of slots that voted yes */
        } gameVoteState;
    } u;
} ControlEvent;

#endif /* CONTROL_EVENT_H */
