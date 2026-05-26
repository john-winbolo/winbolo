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
 *Name:          Transport Command Codec
 *Filename:      transport_command_codec.c
 *Purpose:
 *  Encoder/decoder pairs for every ClientCommandType.
 *  Each encoder produces a complete wire packet (header +
 *  body) with sequence 0; each decoder reads a wire packet
 *  and populates a ClientCommand the dispatcher consumes.
 *
 *  Wire byte layouts are byte-identical to what the existing
 *  transportUdpClientSend* helpers produce, EXCEPT for five
 *  commands (READY, ALLIANCE_REQUEST, ALLIANCE_ACCEPT,
 *  ALLIANCE_LEAVE, NAME_CHANGE) where the legacy
 *  fromPlayer/playerNum byte is written as 0. The dispatcher
 *  drives attribution from senderSlot in all cases, so the
 *  byte is ignored on receipt.
 *********************************************************/

#include "transport_command_codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "client_command.h"
#include "netpacks.h"
#include "transport_udp_internal.h"  /* packHeader, WBN_JOIN_KEY_WIRE_LEN */

/* ================================================================
 * Per-variant encoder + decoder pairs, in ClientCommandType order.
 * Each encoder writes a full wire packet (header + body) and
 * stores the byte count in *outLen. Each decoder validates the
 * wire bounds and populates *cmd; the header is consumed
 * internally and not surfaced.
 * ================================================================ */

/* CMD_TEAM_SET — PACKET_LOBBY_TEAM_SET
 * Wire: [header 8] [targetSlot 1] [teamNumber 1] */
static bool commandEncodeTeamSet(const ClientCommand *cmd,
                                 uint8_t *buf, size_t bufCap,
                                 size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_TEAM_SET, 0);
    buf[PACKET_HEADER_SIZE]     = cmd->u.teamSet.slot;
    buf[PACKET_HEADER_SIZE + 1] = cmd->u.teamSet.team;
    *outLen = needed;
    return true;
}

static bool commandDecodeTeamSet(const uint8_t *buf, size_t len,
                                 ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 2) return false;
    cmd->type = CMD_TEAM_SET;
    cmd->u.teamSet.slot = buf[PACKET_HEADER_SIZE];
    cmd->u.teamSet.team = buf[PACKET_HEADER_SIZE + 1];
    return true;
}

/* CMD_READY — PACKET_LOBBY_READY
 * Wire: [header 8] [playerNum 1 — legacy] [ready 1] */
static bool commandEncodeReady(const ClientCommand *cmd,
                               uint8_t *buf, size_t bufCap,
                               size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_READY, 0);
    buf[PACKET_HEADER_SIZE]     = 0;  /* legacy playerNum; dispatcher uses senderSlot */
    buf[PACKET_HEADER_SIZE + 1] = cmd->u.ready.ready ? 1 : 0;
    *outLen = needed;
    return true;
}

static bool commandDecodeReady(const uint8_t *buf, size_t len,
                               ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 2) return false;
    cmd->type = CMD_READY;
    cmd->u.ready.ready = buf[PACKET_HEADER_SIZE + 1] != 0;
    return true;
}

/* CMD_LOBBY_BOT_CONFIG — PACKET_LOBBY_BOT_CONFIG
 * Wire: [header 8] [slot 1] [difficulty 1] [personality 1]
 *       [nameLen 1] [name N] */
static bool commandEncodeLobbyBotConfig(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    uint8_t nameLen = cmd->u.lobbyBotConfig.nameLen;
    if (nameLen >= PACKET_MAX_PLAYER_NAME) nameLen = PACKET_MAX_PLAYER_NAME - 1;
    const size_t needed = PACKET_HEADER_SIZE + 4 + nameLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_BOT_CONFIG, 0);
    buf[PACKET_HEADER_SIZE + 0] = cmd->u.lobbyBotConfig.slot;
    buf[PACKET_HEADER_SIZE + 1] = cmd->u.lobbyBotConfig.difficulty;
    buf[PACKET_HEADER_SIZE + 2] = cmd->u.lobbyBotConfig.personality;
    buf[PACKET_HEADER_SIZE + 3] = nameLen;
    if (nameLen > 0) {
        memcpy(buf + PACKET_HEADER_SIZE + 4, cmd->u.lobbyBotConfig.name, nameLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyBotConfig(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 4) return false;
    uint8_t nameLen = buf[PACKET_HEADER_SIZE + 3];
    if (nameLen >= PACKET_MAX_PLAYER_NAME ||
        len < (size_t)PACKET_HEADER_SIZE + 4 + nameLen) {
        return false;
    }
    cmd->type = CMD_LOBBY_BOT_CONFIG;
    cmd->u.lobbyBotConfig.slot        = buf[PACKET_HEADER_SIZE + 0];
    cmd->u.lobbyBotConfig.difficulty  = buf[PACKET_HEADER_SIZE + 1];
    cmd->u.lobbyBotConfig.personality = buf[PACKET_HEADER_SIZE + 2];
    cmd->u.lobbyBotConfig.nameLen     = nameLen;
    if (nameLen > 0) {
        memcpy(cmd->u.lobbyBotConfig.name,
               buf + PACKET_HEADER_SIZE + 4, nameLen);
    }
    return true;
}

/* CMD_LOBBY_TEAM_META — PACKET_LOBBY_TEAM_META
 * Wire: [header 8] [teamId 1] [color 1] [namingPool 1]
 *       [nameLen 1] [name N] */
static bool commandEncodeLobbyTeamMeta(const ClientCommand *cmd,
                                       uint8_t *buf, size_t bufCap,
                                       size_t *outLen) {
    uint8_t nameLen = cmd->u.lobbyTeamMeta.nameLen;
    if (nameLen >= LOBBY_TEAM_NAME_LEN) nameLen = LOBBY_TEAM_NAME_LEN - 1;
    const size_t needed = PACKET_HEADER_SIZE + 4 + nameLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_TEAM_META, 0);
    buf[PACKET_HEADER_SIZE + 0] = cmd->u.lobbyTeamMeta.teamId;
    buf[PACKET_HEADER_SIZE + 1] = cmd->u.lobbyTeamMeta.color;
    buf[PACKET_HEADER_SIZE + 2] = cmd->u.lobbyTeamMeta.namingPool;
    buf[PACKET_HEADER_SIZE + 3] = nameLen;
    if (nameLen > 0) {
        memcpy(buf + PACKET_HEADER_SIZE + 4, cmd->u.lobbyTeamMeta.name, nameLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyTeamMeta(const uint8_t *buf, size_t len,
                                       ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 4) return false;
    uint8_t nameLen = buf[PACKET_HEADER_SIZE + 3];
    if (nameLen > LOBBY_TEAM_NAME_LEN - 1 ||
        len < (size_t)PACKET_HEADER_SIZE + 4 + nameLen) {
        return false;
    }
    cmd->type = CMD_LOBBY_TEAM_META;
    cmd->u.lobbyTeamMeta.teamId     = buf[PACKET_HEADER_SIZE + 0];
    cmd->u.lobbyTeamMeta.color      = buf[PACKET_HEADER_SIZE + 1];
    cmd->u.lobbyTeamMeta.namingPool = buf[PACKET_HEADER_SIZE + 2];
    cmd->u.lobbyTeamMeta.nameLen    = nameLen;
    if (nameLen > 0) {
        memcpy(cmd->u.lobbyTeamMeta.name,
               buf + PACKET_HEADER_SIZE + 4, nameLen);
    }
    return true;
}

/* CMD_LOBBY_TEAM_CLEAR — PACKET_LOBBY_TEAM_CLEAR
 * Wire: [header 8] [teamId 1] */
static bool commandEncodeLobbyTeamClear(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_TEAM_CLEAR, 0);
    buf[PACKET_HEADER_SIZE] = cmd->u.lobbyTeamClear.teamId;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyTeamClear(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 1) return false;
    cmd->type = CMD_LOBBY_TEAM_CLEAR;
    cmd->u.lobbyTeamClear.teamId = buf[PACKET_HEADER_SIZE];
    return true;
}

/* CMD_LOBBY_SETTING — PACKET_LOBBY_SET_SETTING
 * Wire: [header 8] [settingType 1] [valueLen 1] [value N] */
static bool commandEncodeLobbySetting(const ClientCommand *cmd,
                                      uint8_t *buf, size_t bufCap,
                                      size_t *outLen) {
    uint8_t valueLen = cmd->u.lobbySetting.valueLen;
    if (valueLen > 32) valueLen = 32;
    const size_t needed = PACKET_HEADER_SIZE + 2 + valueLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_SET_SETTING, 0);
    buf[PACKET_HEADER_SIZE]     = cmd->u.lobbySetting.settingType;
    buf[PACKET_HEADER_SIZE + 1] = valueLen;
    if (valueLen > 0) {
        memcpy(buf + PACKET_HEADER_SIZE + 2, cmd->u.lobbySetting.value, valueLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbySetting(const uint8_t *buf, size_t len,
                                      ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 2) return false;
    uint8_t valueLen = buf[PACKET_HEADER_SIZE + 1];
    if (valueLen > 32 ||
        len < (size_t)PACKET_HEADER_SIZE + 2 + valueLen) {
        return false;
    }
    cmd->type = CMD_LOBBY_SETTING;
    cmd->u.lobbySetting.settingType = buf[PACKET_HEADER_SIZE];
    cmd->u.lobbySetting.valueLen    = valueLen;
    if (valueLen > 0) {
        memcpy(cmd->u.lobbySetting.value,
               buf + PACKET_HEADER_SIZE + 2, valueLen);
    }
    return true;
}

/* CMD_CHAT — PACKET_CHAT_MESSAGE
 * Wire: [header 8] [destPlayer 1] [body bodyLen] — no length prefix;
 * the body length is implied by the packet length. */
static bool commandEncodeChat(const ClientCommand *cmd,
                              uint8_t *buf, size_t bufCap,
                              size_t *outLen) {
    uint16_t bodyLen = cmd->u.chat.bodyLen;
    if (bodyLen > PACKET_MAX_CHAT_MESSAGE) bodyLen = PACKET_MAX_CHAT_MESSAGE;
    const size_t needed = PACKET_HEADER_SIZE + 1 + bodyLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_CHAT_MESSAGE, 0);
    buf[PACKET_HEADER_SIZE] = cmd->u.chat.destPlayer;
    if (bodyLen > 0) {
        memcpy(buf + PACKET_HEADER_SIZE + 1, cmd->u.chat.body, bodyLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeChat(const uint8_t *buf, size_t len,
                              ClientCommand *cmd) {
    if (len <= PACKET_HEADER_SIZE + 1) return false;
    size_t bodyLen = len - PACKET_HEADER_SIZE - 1;
    if (bodyLen > PACKET_MAX_CHAT_MESSAGE) bodyLen = PACKET_MAX_CHAT_MESSAGE;
    cmd->type = CMD_CHAT;
    cmd->u.chat.destPlayer = buf[PACKET_HEADER_SIZE];
    cmd->u.chat.bodyLen    = (uint16_t)bodyLen;
    if (bodyLen > 0) {
        memcpy(cmd->u.chat.body, buf + PACKET_HEADER_SIZE + 1, bodyLen);
    }
    return true;
}

/* CMD_ALLIANCE_REQUEST — PACKET_ALLIANCE_REQUEST
 * Wire: [header 8] [fromPlayer 1 — legacy] [toPlayer 1] */
static bool commandEncodeAllianceRequest(const ClientCommand *cmd,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_ALLIANCE_REQUEST, 0);
    buf[PACKET_HEADER_SIZE]     = 0;  /* legacy fromPlayer; dispatcher uses senderSlot */
    buf[PACKET_HEADER_SIZE + 1] = cmd->u.allianceRequest.toPlayer;
    *outLen = needed;
    return true;
}

static bool commandDecodeAllianceRequest(const uint8_t *buf, size_t len,
                                         ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 2) return false;
    cmd->type = CMD_ALLIANCE_REQUEST;
    cmd->u.allianceRequest.toPlayer = buf[PACKET_HEADER_SIZE + 1];
    return true;
}

/* CMD_ALLIANCE_ACCEPT — PACKET_ALLIANCE_ACCEPT
 * Wire: [header 8] [fromPlayer 1 — legacy] [newMember 1] */
static bool commandEncodeAllianceAccept(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_ALLIANCE_ACCEPT, 0);
    buf[PACKET_HEADER_SIZE]     = 0;  /* legacy fromPlayer; dispatcher uses senderSlot */
    buf[PACKET_HEADER_SIZE + 1] = cmd->u.allianceAccept.newMember;
    *outLen = needed;
    return true;
}

static bool commandDecodeAllianceAccept(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 2) return false;
    cmd->type = CMD_ALLIANCE_ACCEPT;
    cmd->u.allianceAccept.newMember = buf[PACKET_HEADER_SIZE + 1];
    return true;
}

/* CMD_ALLIANCE_LEAVE — PACKET_ALLIANCE_LEAVE
 * Wire: [header 8] [playerNum 1 — legacy] */
static bool commandEncodeAllianceLeave(const ClientCommand *cmd,
                                       uint8_t *buf, size_t bufCap,
                                       size_t *outLen) {
    (void)cmd;
    const size_t needed = PACKET_HEADER_SIZE + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_ALLIANCE_LEAVE, 0);
    buf[PACKET_HEADER_SIZE] = 0;  /* legacy playerNum; dispatcher uses senderSlot */
    *outLen = needed;
    return true;
}

static bool commandDecodeAllianceLeave(const uint8_t *buf, size_t len,
                                       ClientCommand *cmd) {
    (void)buf;
    if (len < PACKET_HEADER_SIZE + 1) return false;
    cmd->type = CMD_ALLIANCE_LEAVE;
    cmd->u.allianceLeave._unused = 0;
    return true;
}

/* CMD_GAME_VOTE_TOGGLE — PACKET_GAME_VOTE_TOGGLE
 * Wire: [header 8] [kind 1] [toggleMode 1] */
static bool commandEncodeGameVoteToggle(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_GAME_VOTE_TOGGLE, 0);
    buf[PACKET_HEADER_SIZE]     = cmd->u.gameVoteToggle.kind;
    buf[PACKET_HEADER_SIZE + 1] = cmd->u.gameVoteToggle.toggleMode;
    *outLen = needed;
    return true;
}

static bool commandDecodeGameVoteToggle(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 2) return false;
    cmd->type = CMD_GAME_VOTE_TOGGLE;
    cmd->u.gameVoteToggle.kind       = buf[PACKET_HEADER_SIZE + 0];
    cmd->u.gameVoteToggle.toggleMode = buf[PACKET_HEADER_SIZE + 1];
    return true;
}

/* CMD_LOBBY_REMOVE_BOT — PACKET_LOBBY_REMOVE_BOT
 * Wire: [header 8] [playerNum 1] */
static bool commandEncodeLobbyRemoveBot(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_REMOVE_BOT, 0);
    buf[PACKET_HEADER_SIZE] = cmd->u.lobbyRemoveBot.slot;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyRemoveBot(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 1) return false;
    cmd->type = CMD_LOBBY_REMOVE_BOT;
    cmd->u.lobbyRemoveBot.slot = buf[PACKET_HEADER_SIZE];
    return true;
}

/* CMD_LOBBY_SET_BOT_BRAIN — PACKET_LOBBY_SET_BOT_BRAIN
 * Wire: [header 8] [slot 1] [brainIdx 1] */
static bool commandEncodeLobbySetBotBrain(const ClientCommand *cmd,
                                          uint8_t *buf, size_t bufCap,
                                          size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_SET_BOT_BRAIN, 0);
    buf[PACKET_HEADER_SIZE + 0] = cmd->u.lobbySetBotBrain.slot;
    buf[PACKET_HEADER_SIZE + 1] = cmd->u.lobbySetBotBrain.brainIdx;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbySetBotBrain(const uint8_t *buf, size_t len,
                                          ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 2) return false;
    cmd->type = CMD_LOBBY_SET_BOT_BRAIN;
    cmd->u.lobbySetBotBrain.slot     = buf[PACKET_HEADER_SIZE + 0];
    cmd->u.lobbySetBotBrain.brainIdx = buf[PACKET_HEADER_SIZE + 1];
    return true;
}

/* CMD_LOBBY_OPEN_HOST — PACKET_LOBBY_OPEN_HOST
 * Wire: [header 8] [openHost 1] */
static bool commandEncodeLobbyOpenHost(const ClientCommand *cmd,
                                       uint8_t *buf, size_t bufCap,
                                       size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_OPEN_HOST, 0);
    buf[PACKET_HEADER_SIZE] = cmd->u.lobbyOpenHost.openHost ? 1 : 0;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyOpenHost(const uint8_t *buf, size_t len,
                                       ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 1) return false;
    cmd->type = CMD_LOBBY_OPEN_HOST;
    cmd->u.lobbyOpenHost.openHost = buf[PACKET_HEADER_SIZE] != 0;
    return true;
}

/* CMD_MAP_SKIP_VOTE — PACKET_MAP_SKIP_VOTE
 * Wire: [header 8] (no body) */
static bool commandEncodeMapSkipVote(const ClientCommand *cmd,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    (void)cmd;
    if (bufCap < PACKET_HEADER_SIZE) return false;
    packHeader(buf, PACKET_MAP_SKIP_VOTE, 0);
    *outLen = PACKET_HEADER_SIZE;
    return true;
}

static bool commandDecodeMapSkipVote(const uint8_t *buf, size_t len,
                                     ClientCommand *cmd) {
    (void)buf;
    if (len < PACKET_HEADER_SIZE) return false;
    cmd->type = CMD_MAP_SKIP_VOTE;
    cmd->u.mapSkipVote._unused = 0;
    return true;
}

/* CMD_NAME_CHANGE — PACKET_NAME_CHANGE
 * Wire: [header 8] [playerNum 1 — legacy] [name PACKET_MAX_PLAYER_NAME] */
static bool commandEncodeNameChange(const ClientCommand *cmd,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_NAME_CHANGE, 0);
    buf[PACKET_HEADER_SIZE] = 0;  /* legacy playerNum; dispatcher uses senderSlot */
    memset(buf + PACKET_HEADER_SIZE + 1, 0, PACKET_MAX_PLAYER_NAME);
    {
        size_t nameLen = strnlen(cmd->u.nameChange.newName,
                                 PACKET_MAX_PLAYER_NAME - 1);
        if (nameLen > 0) {
            memcpy(buf + PACKET_HEADER_SIZE + 1,
                   cmd->u.nameChange.newName, nameLen);
        }
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeNameChange(const uint8_t *buf, size_t len,
                                    ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 1 + PACKET_MAX_PLAYER_NAME) return false;
    cmd->type = CMD_NAME_CHANGE;
    memcpy(cmd->u.nameChange.newName,
           buf + PACKET_HEADER_SIZE + 1, PACKET_MAX_PLAYER_NAME);
    cmd->u.nameChange.newName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    return true;
}

/* CMD_LOCK_TOGGLE — PACKET_LOCK_TOGGLE
 * Wire: [header 8] [allow 1] */
static bool commandEncodeLockToggle(const ClientCommand *cmd,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOCK_TOGGLE, 0);
    buf[PACKET_HEADER_SIZE] = cmd->u.lockToggle.allow ? 1 : 0;
    *outLen = needed;
    return true;
}

static bool commandDecodeLockToggle(const uint8_t *buf, size_t len,
                                    ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 1) return false;
    cmd->type = CMD_LOCK_TOGGLE;
    cmd->u.lockToggle.allow = buf[PACKET_HEADER_SIZE] != 0;
    return true;
}

/* CMD_LOBBY_ADD_BOT — PACKET_LOBBY_ADD_BOT
 * Wire: [header 8] [teamNumber 1] [pathLen 1 = 0] [nameLen 1] [name N]
 * Server keeps the [pathLen][path] pair on the wire for back-compat but
 * ignores the path bytes; the codec always emits pathLen = 0. */
static bool commandEncodeLobbyAddBot(const ClientCommand *cmd,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    uint8_t nameLen = cmd->u.lobbyAddBot.nameLen;
    if (nameLen >= PACKET_MAX_PLAYER_NAME) nameLen = PACKET_MAX_PLAYER_NAME - 1;
    const size_t needed = PACKET_HEADER_SIZE + 3 + nameLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_ADD_BOT, 0);
    buf[PACKET_HEADER_SIZE + 0] = cmd->u.lobbyAddBot.teamNumber;
    buf[PACKET_HEADER_SIZE + 1] = 0;  /* pathLen — server ignores brain payload */
    buf[PACKET_HEADER_SIZE + 2] = nameLen;
    if (nameLen > 0) {
        memcpy(buf + PACKET_HEADER_SIZE + 3, cmd->u.lobbyAddBot.name, nameLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyAddBot(const uint8_t *buf, size_t len,
                                     ClientCommand *cmd) {
    /* Lenient mirror of the server arm: accept any (pathLen, nameLen)
     * combination that fits within the packet. Empty/missing fields
     * decode to teamNumber=0, nameLen=0. */
    size_t pos = PACKET_HEADER_SIZE;
    uint8_t teamNumber = 0;
    uint8_t nameLen = 0;
    char clientBotName[PACKET_MAX_PLAYER_NAME];
    clientBotName[0] = '\0';
    if (len >= pos + 1) {
        teamNumber = buf[pos++];
        if (len >= pos + 1) {
            uint8_t pathLen = buf[pos++];
            if (len >= pos + pathLen) {
                pos += pathLen;
                if (len >= pos + 1) {
                    uint8_t nl = buf[pos++];
                    if (nl < sizeof(clientBotName) &&
                        len >= pos + nl) {
                        memcpy(clientBotName, buf + pos, nl);
                        clientBotName[nl] = '\0';
                        nameLen = nl;
                    }
                }
            }
        }
    }
    cmd->type = CMD_LOBBY_ADD_BOT;
    cmd->u.lobbyAddBot.teamNumber = teamNumber;
    cmd->u.lobbyAddBot.nameLen    = nameLen;
    if (nameLen > 0) {
        memcpy(cmd->u.lobbyAddBot.name, clientBotName, nameLen);
    }
    return true;
}

/* CMD_LOBBY_SET_MAP — PACKET_LOBBY_SET_MAP
 * Wire: [header 8] [pathLen 1] [path N] */
static bool commandEncodeLobbySetMap(const ClientCommand *cmd,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    uint8_t pathLen = cmd->u.lobbySetMap.relPathLen;
    if (pathLen > 255) pathLen = 255;
    const size_t needed = PACKET_HEADER_SIZE + 1 + pathLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_SET_MAP, 0);
    buf[PACKET_HEADER_SIZE] = pathLen;
    if (pathLen > 0) {
        memcpy(buf + PACKET_HEADER_SIZE + 1,
               cmd->u.lobbySetMap.relPath, pathLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbySetMap(const uint8_t *buf, size_t len,
                                     ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 1) return false;
    uint8_t pathLen = buf[PACKET_HEADER_SIZE];
    if (pathLen == 0 ||
        len < (size_t)PACKET_HEADER_SIZE + 1 + pathLen) {
        return false;
    }
    cmd->type = CMD_LOBBY_SET_MAP;
    cmd->u.lobbySetMap.relPathLen = pathLen;
    memcpy(cmd->u.lobbySetMap.relPath,
           buf + PACKET_HEADER_SIZE + 1, pathLen);
    return true;
}

/* CMD_LOBBY_PREVIEW_CANCEL — PACKET_LOBBY_PREVIEW_CANCEL
 * Wire: [header 8] (no body) */
static bool commandEncodeLobbyPreviewCancel(const ClientCommand *cmd,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)cmd;
    if (bufCap < PACKET_HEADER_SIZE) return false;
    packHeader(buf, PACKET_LOBBY_PREVIEW_CANCEL, 0);
    *outLen = PACKET_HEADER_SIZE;
    return true;
}

static bool commandDecodeLobbyPreviewCancel(const uint8_t *buf, size_t len,
                                            ClientCommand *cmd) {
    (void)buf;
    if (len < PACKET_HEADER_SIZE) return false;
    cmd->type = CMD_LOBBY_PREVIEW_CANCEL;
    cmd->u.lobbyPreviewCancel._unused = 0;
    return true;
}

/* CMD_LOBBY_PREVIEW_COMMIT — PACKET_LOBBY_PREVIEW_COMMIT
 * Wire: [header 8] (no body) */
static bool commandEncodeLobbyPreviewCommit(const ClientCommand *cmd,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)cmd;
    if (bufCap < PACKET_HEADER_SIZE) return false;
    packHeader(buf, PACKET_LOBBY_PREVIEW_COMMIT, 0);
    *outLen = PACKET_HEADER_SIZE;
    return true;
}

static bool commandDecodeLobbyPreviewCommit(const uint8_t *buf, size_t len,
                                            ClientCommand *cmd) {
    (void)buf;
    if (len < PACKET_HEADER_SIZE) return false;
    cmd->type = CMD_LOBBY_PREVIEW_COMMIT;
    cmd->u.lobbyPreviewCommit._unused = 0;
    return true;
}

/* CMD_LOBBY_PREVIEW_RANDOM — PACKET_LOBBY_PREVIEW_RANDOM
 * Wire: [header 8] [seedLen 1] [seed N] */
static bool commandEncodeLobbyPreviewRandom(const ClientCommand *cmd,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    uint8_t seedLen = cmd->u.lobbyPreviewRandom.seedLen;
    if (seedLen > 63) seedLen = 63;
    const size_t needed = PACKET_HEADER_SIZE + 1 + seedLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_PREVIEW_RANDOM, 0);
    buf[PACKET_HEADER_SIZE] = seedLen;
    if (seedLen > 0) {
        memcpy(buf + PACKET_HEADER_SIZE + 1,
               cmd->u.lobbyPreviewRandom.seed, seedLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyPreviewRandom(const uint8_t *buf, size_t len,
                                            ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 1) return false;
    uint8_t seedLen = buf[PACKET_HEADER_SIZE];
    if (seedLen > 63 ||
        len < (size_t)PACKET_HEADER_SIZE + 1 + seedLen) {
        return false;
    }
    cmd->type = CMD_LOBBY_PREVIEW_RANDOM;
    cmd->u.lobbyPreviewRandom.seedLen = seedLen;
    if (seedLen > 0) {
        memcpy(cmd->u.lobbyPreviewRandom.seed,
               buf + PACKET_HEADER_SIZE + 1, seedLen);
    }
    return true;
}

/* CMD_LOBBY_KICK — PACKET_LOBBY_KICK
 * Wire: [header 8] [slot 1] */
static bool commandEncodeLobbyKick(const ClientCommand *cmd,
                                   uint8_t *buf, size_t bufCap,
                                   size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_KICK, 0);
    buf[PACKET_HEADER_SIZE] = cmd->u.lobbyKick.slot;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyKick(const uint8_t *buf, size_t len,
                                   ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 1) return false;
    cmd->type = CMD_LOBBY_KICK;
    cmd->u.lobbyKick.slot = buf[PACKET_HEADER_SIZE];
    return true;
}

/* CMD_LOBBY_SET_PASSWORD — PACKET_LOBBY_SET_PASSWORD
 * Wire: [header 8] [pwLen 1] [pw N] */
static bool commandEncodeLobbySetPassword(const ClientCommand *cmd,
                                          uint8_t *buf, size_t bufCap,
                                          size_t *outLen) {
    uint8_t pwLen = cmd->u.lobbySetPassword.pwLen;
    if (pwLen > sizeof(cmd->u.lobbySetPassword.password)) {
        pwLen = (uint8_t)sizeof(cmd->u.lobbySetPassword.password);
    }
    const size_t needed = PACKET_HEADER_SIZE + 1 + pwLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_SET_PASSWORD, 0);
    buf[PACKET_HEADER_SIZE] = pwLen;
    if (pwLen > 0) {
        memcpy(buf + PACKET_HEADER_SIZE + 1,
               cmd->u.lobbySetPassword.password, pwLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbySetPassword(const uint8_t *buf, size_t len,
                                          ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 1) return false;
    uint8_t pwLen = buf[PACKET_HEADER_SIZE];
    if (len < (size_t)PACKET_HEADER_SIZE + 1 + pwLen) return false;
    cmd->type = CMD_LOBBY_SET_PASSWORD;
    cmd->u.lobbySetPassword.pwLen = pwLen;
    if (pwLen > 0 &&
        pwLen <= sizeof(cmd->u.lobbySetPassword.password)) {
        memcpy(cmd->u.lobbySetPassword.password,
               buf + PACKET_HEADER_SIZE + 1, pwLen);
    }
    return true;
}

/* CMD_BALANCE_REQUEST — PACKET_BALANCE_REQUEST
 * Wire: [header 8] [teamSize 1] [includeBots 1] */
static bool commandEncodeBalanceRequest(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_BALANCE_REQUEST, 0);
    buf[PACKET_HEADER_SIZE]     = cmd->u.balanceRequest.teamSize;
    buf[PACKET_HEADER_SIZE + 1] = cmd->u.balanceRequest.includeBots ? 1 : 0;
    *outLen = needed;
    return true;
}

static bool commandDecodeBalanceRequest(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + 2) return false;
    cmd->type = CMD_BALANCE_REQUEST;
    cmd->u.balanceRequest.teamSize    = buf[PACKET_HEADER_SIZE];
    cmd->u.balanceRequest.includeBots = buf[PACKET_HEADER_SIZE + 1] != 0;
    return true;
}

/* CMD_BALANCE_APPLY — PACKET_BALANCE_APPLY
 * Wire: [header 8] (no body) */
static bool commandEncodeBalanceApply(const ClientCommand *cmd,
                                      uint8_t *buf, size_t bufCap,
                                      size_t *outLen) {
    (void)cmd;
    if (bufCap < PACKET_HEADER_SIZE) return false;
    packHeader(buf, PACKET_BALANCE_APPLY, 0);
    *outLen = PACKET_HEADER_SIZE;
    return true;
}

static bool commandDecodeBalanceApply(const uint8_t *buf, size_t len,
                                      ClientCommand *cmd) {
    (void)buf;
    if (len < PACKET_HEADER_SIZE) return false;
    cmd->type = CMD_BALANCE_APPLY;
    cmd->u.balanceApply._unused = 0;
    return true;
}

/* CMD_BALANCE_DISMISS — PACKET_BALANCE_DISMISS
 * Wire: [header 8] (no body) */
static bool commandEncodeBalanceDismiss(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    (void)cmd;
    if (bufCap < PACKET_HEADER_SIZE) return false;
    packHeader(buf, PACKET_BALANCE_DISMISS, 0);
    *outLen = PACKET_HEADER_SIZE;
    return true;
}

static bool commandDecodeBalanceDismiss(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    (void)buf;
    if (len < PACKET_HEADER_SIZE) return false;
    cmd->type = CMD_BALANCE_DISMISS;
    cmd->u.balanceDismiss._unused = 0;
    return true;
}

/* CMD_WBN_REAUTH — PACKET_WBN_REAUTH
 * Wire: [header 8] [token WBN_JOIN_KEY_WIRE_LEN] */
static bool commandEncodeWbnReauth(const ClientCommand *cmd,
                                   uint8_t *buf, size_t bufCap,
                                   size_t *outLen) {
    const size_t needed = PACKET_HEADER_SIZE + WBN_JOIN_KEY_WIRE_LEN;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_WBN_REAUTH, 0);
    memcpy(buf + PACKET_HEADER_SIZE,
           cmd->u.wbnReauth.token, WBN_JOIN_KEY_WIRE_LEN);
    *outLen = needed;
    return true;
}

static bool commandDecodeWbnReauth(const uint8_t *buf, size_t len,
                                   ClientCommand *cmd) {
    if (len < PACKET_HEADER_SIZE + WBN_JOIN_KEY_WIRE_LEN) return false;
    cmd->type = CMD_WBN_REAUTH;
    memcpy(cmd->u.wbnReauth.token,
           buf + PACKET_HEADER_SIZE, WBN_JOIN_KEY_WIRE_LEN);
    cmd->u.wbnReauth.token[WBN_JOIN_KEY_WIRE_LEN - 1] = '\0';
    return true;
}

/* ================================================================
 * Public API — switch dispatch keyed off cmd->type for encode and
 * buf[2] (packet type) for decode. Mirrors transport_control_codec.c
 * which uses a switch for decoder dispatch; the encoder switch here
 * is symmetric (single signature per variant, no body-only flavor).
 * ================================================================ */

bool commandCodecEncode(const ClientCommand *cmd,
                        uint8_t *buf, size_t bufCap, size_t *outLen) {
    if (!cmd || !buf || !outLen) return false;
    switch (cmd->type) {
        case CMD_TEAM_SET:              return commandEncodeTeamSet(cmd, buf, bufCap, outLen);
        case CMD_READY:                 return commandEncodeReady(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_BOT_CONFIG:      return commandEncodeLobbyBotConfig(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_TEAM_META:       return commandEncodeLobbyTeamMeta(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_TEAM_CLEAR:      return commandEncodeLobbyTeamClear(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_SETTING:         return commandEncodeLobbySetting(cmd, buf, bufCap, outLen);
        case CMD_CHAT:                  return commandEncodeChat(cmd, buf, bufCap, outLen);
        case CMD_ALLIANCE_REQUEST:      return commandEncodeAllianceRequest(cmd, buf, bufCap, outLen);
        case CMD_ALLIANCE_ACCEPT:       return commandEncodeAllianceAccept(cmd, buf, bufCap, outLen);
        case CMD_ALLIANCE_LEAVE:        return commandEncodeAllianceLeave(cmd, buf, bufCap, outLen);
        case CMD_GAME_VOTE_TOGGLE:      return commandEncodeGameVoteToggle(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_REMOVE_BOT:      return commandEncodeLobbyRemoveBot(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_SET_BOT_BRAIN:   return commandEncodeLobbySetBotBrain(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_OPEN_HOST:       return commandEncodeLobbyOpenHost(cmd, buf, bufCap, outLen);
        case CMD_MAP_SKIP_VOTE:         return commandEncodeMapSkipVote(cmd, buf, bufCap, outLen);
        case CMD_NAME_CHANGE:           return commandEncodeNameChange(cmd, buf, bufCap, outLen);
        case CMD_LOCK_TOGGLE:           return commandEncodeLockToggle(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_ADD_BOT:         return commandEncodeLobbyAddBot(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_SET_MAP:         return commandEncodeLobbySetMap(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_PREVIEW_CANCEL:  return commandEncodeLobbyPreviewCancel(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_PREVIEW_COMMIT:  return commandEncodeLobbyPreviewCommit(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_PREVIEW_RANDOM:  return commandEncodeLobbyPreviewRandom(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_KICK:            return commandEncodeLobbyKick(cmd, buf, bufCap, outLen);
        case CMD_LOBBY_SET_PASSWORD:    return commandEncodeLobbySetPassword(cmd, buf, bufCap, outLen);
        case CMD_BALANCE_REQUEST:       return commandEncodeBalanceRequest(cmd, buf, bufCap, outLen);
        case CMD_BALANCE_APPLY:         return commandEncodeBalanceApply(cmd, buf, bufCap, outLen);
        case CMD_BALANCE_DISMISS:       return commandEncodeBalanceDismiss(cmd, buf, bufCap, outLen);
        case CMD_WBN_REAUTH:            return commandEncodeWbnReauth(cmd, buf, bufCap, outLen);
        case CMD_NONE:
        default:                        return false;
    }
}

bool commandCodecDecode(const uint8_t *buf, size_t len,
                        ClientCommand *cmd) {
    if (!buf || !cmd || len < PACKET_HEADER_SIZE) return false;
    if (buf[0] != BOLO_NEW_MAGIC_0 || buf[1] != BOLO_NEW_MAGIC_1) return false;
    uint8_t pktType = buf[2];
    switch (pktType) {
        case PACKET_LOBBY_TEAM_SET:        return commandDecodeTeamSet(buf, len, cmd);
        case PACKET_LOBBY_READY:           return commandDecodeReady(buf, len, cmd);
        case PACKET_LOBBY_BOT_CONFIG:      return commandDecodeLobbyBotConfig(buf, len, cmd);
        case PACKET_LOBBY_TEAM_META:       return commandDecodeLobbyTeamMeta(buf, len, cmd);
        case PACKET_LOBBY_TEAM_CLEAR:      return commandDecodeLobbyTeamClear(buf, len, cmd);
        case PACKET_LOBBY_SET_SETTING:     return commandDecodeLobbySetting(buf, len, cmd);
        case PACKET_CHAT_MESSAGE:          return commandDecodeChat(buf, len, cmd);
        case PACKET_ALLIANCE_REQUEST:      return commandDecodeAllianceRequest(buf, len, cmd);
        case PACKET_ALLIANCE_ACCEPT:       return commandDecodeAllianceAccept(buf, len, cmd);
        case PACKET_ALLIANCE_LEAVE:        return commandDecodeAllianceLeave(buf, len, cmd);
        case PACKET_GAME_VOTE_TOGGLE:      return commandDecodeGameVoteToggle(buf, len, cmd);
        case PACKET_LOBBY_REMOVE_BOT:      return commandDecodeLobbyRemoveBot(buf, len, cmd);
        case PACKET_LOBBY_SET_BOT_BRAIN:   return commandDecodeLobbySetBotBrain(buf, len, cmd);
        case PACKET_LOBBY_OPEN_HOST:       return commandDecodeLobbyOpenHost(buf, len, cmd);
        case PACKET_MAP_SKIP_VOTE:         return commandDecodeMapSkipVote(buf, len, cmd);
        case PACKET_NAME_CHANGE:           return commandDecodeNameChange(buf, len, cmd);
        case PACKET_LOCK_TOGGLE:           return commandDecodeLockToggle(buf, len, cmd);
        case PACKET_LOBBY_ADD_BOT:         return commandDecodeLobbyAddBot(buf, len, cmd);
        case PACKET_LOBBY_SET_MAP:         return commandDecodeLobbySetMap(buf, len, cmd);
        case PACKET_LOBBY_PREVIEW_CANCEL:  return commandDecodeLobbyPreviewCancel(buf, len, cmd);
        case PACKET_LOBBY_PREVIEW_COMMIT:  return commandDecodeLobbyPreviewCommit(buf, len, cmd);
        case PACKET_LOBBY_PREVIEW_RANDOM:  return commandDecodeLobbyPreviewRandom(buf, len, cmd);
        case PACKET_LOBBY_KICK:            return commandDecodeLobbyKick(buf, len, cmd);
        case PACKET_LOBBY_SET_PASSWORD:    return commandDecodeLobbySetPassword(buf, len, cmd);
        case PACKET_BALANCE_REQUEST:       return commandDecodeBalanceRequest(buf, len, cmd);
        case PACKET_BALANCE_APPLY:         return commandDecodeBalanceApply(buf, len, cmd);
        case PACKET_BALANCE_DISMISS:       return commandDecodeBalanceDismiss(buf, len, cmd);
        case PACKET_WBN_REAUTH:            return commandDecodeWbnReauth(buf, len, cmd);
        default:                           return false;
    }
}
