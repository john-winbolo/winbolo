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
 *Name:          Client Command
 *Filename:      client_command.h
 *Purpose:
 *  Tagged-union request a client submits to a server
 *  simulation. Single in-memory shape for both transports:
 *  UDP clients encode it on the wire, local/SP-host clients
 *  hand it directly to serverSimApplyCommand. One variant
 *  per command, keyed by ClientCommandType.
 *********************************************************/

#ifndef CLIENT_COMMAND_H
#define CLIENT_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

#include "wire_limits.h"  /* PACKET_MAX_PLAYER_NAME */

#ifndef LOBBY_TEAM_NAME_LEN
#define LOBBY_TEAM_NAME_LEN 32
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CMD_NONE = 0,
    CMD_TEAM_SET,
    CMD_READY,
    CMD_LOBBY_BOT_CONFIG,
    CMD_LOBBY_TEAM_META,
    CMD_LOBBY_TEAM_CLEAR,
    CMD_LOBBY_SETTING
} ClientCommandType;

/* Reject codes returned by serverSimApplyCommand. Codes 1-7 are
 * intentionally aligned 1:1 with the existing LOBBY_REJECT_*
 * constants in src/bolo/internal/netpacks.h so the UDP server
 * can forward them through PACKET_LOBBY_REJECT unchanged. */
typedef enum {
    CMD_OK = 0,
    CMD_REJECT_NOT_HOST,
    CMD_REJECT_LOCKED,
    CMD_REJECT_INVALID,
    CMD_REJECT_UPLOAD_BUSY,
    CMD_REJECT_UPLOAD_DISABLED,
    CMD_REJECT_UPLOAD_LIMIT_HIT,
    CMD_REJECT_COOLDOWN,
    CMD_REJECT_BAD_STATE
} CmdResult;

/* CMD_TEAM_SET — set the team number for a lobby slot. Sender must
 * be moving its own slot, or be host/admin/openHost to move another
 * slot. Both fields validated against MAX_TANKS. */
typedef struct {
    uint8_t slot;
    uint8_t team;
} CmdTeamSet;

/* CMD_READY — toggle ready state for the sender's slot. The wire
 * carries a playerNum byte for backward compatibility but the
 * server uses senderSlot per the attribution contract. */
typedef struct {
    bool ready;
} CmdReady;

/* CMD_LOBBY_BOT_CONFIG — update difficulty/personality (+ optional
 * rename) for a bot slot. nameLen == 0 means "keep current name".
 * Bot-config validation runs against the connected-player table
 * via transportUdpServerGetPlayerName; the stub for non-server
 * binaries returns NULL (no collision) on every slot. */
typedef struct {
    uint8_t slot;
    uint8_t difficulty;
    uint8_t personality;
    uint8_t nameLen;
    char    name[PACKET_MAX_PLAYER_NAME];
} CmdLobbyBotConfig;

/* CMD_LOBBY_TEAM_META — set color, naming pool, and optional name
 * for a team. nameLen == 0 means "no name set". teamId is 1..MAX_TANKS-1
 * (team 0 is unassigned). */
typedef struct {
    uint8_t teamId;
    uint8_t color;
    uint8_t namingPool;
    uint8_t nameLen;
    char    name[LOBBY_TEAM_NAME_LEN];
} CmdLobbyTeamMeta;

/* CMD_LOBBY_TEAM_CLEAR — drop a team's metadata back to defaults. */
typedef struct {
    uint8_t teamId;
} CmdLobbyTeamClear;

/* CMD_LOBBY_SETTING — set one of the LST_* settings. valueLen is
 * the actual payload length (max 32). */
typedef struct {
    uint8_t settingType;
    uint8_t valueLen;
    uint8_t value[32];
} CmdLobbySetting;

typedef struct ClientCommand {
    ClientCommandType type;
    union {
        CmdTeamSet         teamSet;
        CmdReady           ready;
        CmdLobbyBotConfig  lobbyBotConfig;
        CmdLobbyTeamMeta   lobbyTeamMeta;
        CmdLobbyTeamClear  lobbyTeamClear;
        CmdLobbySetting    lobbySetting;
    } u;
} ClientCommand;

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_COMMAND_H */
