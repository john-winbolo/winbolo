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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CMD_NONE = 0,
    CMD_TEAM_SET
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

typedef struct ClientCommand {
    ClientCommandType type;
    union {
        CmdTeamSet teamSet;
    } u;
} ClientCommand;

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_COMMAND_H */
