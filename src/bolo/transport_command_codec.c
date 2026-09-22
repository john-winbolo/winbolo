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
#include "transport_udp_internal.h"  /* packHeader, packU32, unpackU32, WBN_JOIN_KEY_WIRE_LEN */

/* Command packets reserve a 4-byte cmdSeq immediately after the
 * standard 8-byte header. Per-variant encoders/decoders read and
 * write the variant body starting at this offset. */
#define CMD_PACKET_BODY_OFFSET (PACKET_HEADER_SIZE + 4)

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
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_TEAM_SET, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = cmd->u.teamSet.slot;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.teamSet.team;
    *outLen = needed;
    return true;
}

static bool commandDecodeTeamSet(const uint8_t *buf, size_t len,
                                 ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    cmd->type = CMD_TEAM_SET;
    cmd->u.teamSet.slot = buf[CMD_PACKET_BODY_OFFSET];
    cmd->u.teamSet.team = buf[CMD_PACKET_BODY_OFFSET + 1];
    return true;
}

/* CMD_LOBBY_CLAIM_START — PACKET_LOBBY_CLAIM_START
 * Wire: [header 8] [targetSlot 1] [startIdx 1] */
static bool commandEncodeLobbyClaimStart(const ClientCommand *cmd,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_CLAIM_START, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = cmd->u.lobbyClaimStart.targetSlot;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.lobbyClaimStart.startIdx;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyClaimStart(const uint8_t *buf, size_t len,
                                         ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    cmd->type = CMD_LOBBY_CLAIM_START;
    cmd->u.lobbyClaimStart.targetSlot = buf[CMD_PACKET_BODY_OFFSET];
    cmd->u.lobbyClaimStart.startIdx   = buf[CMD_PACKET_BODY_OFFSET + 1];
    return true;
}

/* CMD_READY — PACKET_LOBBY_READY
 * Wire: [header 8] [playerNum 1 — legacy] [ready 1] */
static bool commandEncodeReady(const ClientCommand *cmd,
                               uint8_t *buf, size_t bufCap,
                               size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_READY, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = 0;  /* legacy playerNum; dispatcher uses senderSlot */
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.ready.ready ? 1 : 0;
    *outLen = needed;
    return true;
}

static bool commandDecodeReady(const uint8_t *buf, size_t len,
                               ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    cmd->type = CMD_READY;
    cmd->u.ready.ready = buf[CMD_PACKET_BODY_OFFSET + 1] != 0;
    return true;
}

/* CMD_LOBBY_BOT_CONFIG — PACKET_LOBBY_BOT_CONFIG
 * Wire: [header 8] [slot 1] [difficulty 1] [personality 1] [mode 1]
 *       [nameLen 1] [name N]
 * mode sits after personality so the older fields kept their offsets;
 * nameLen stays the last fixed byte. Same order as the control-event
 * body in transport_control_codec.c. */
static bool commandEncodeLobbyBotConfig(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    uint8_t nameLen = cmd->u.lobbyBotConfig.nameLen;
    if (nameLen >= PACKET_MAX_PLAYER_NAME) nameLen = PACKET_MAX_PLAYER_NAME - 1;
    const size_t needed = CMD_PACKET_BODY_OFFSET + 5 + nameLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_BOT_CONFIG, 0);
    buf[CMD_PACKET_BODY_OFFSET + 0] = cmd->u.lobbyBotConfig.slot;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.lobbyBotConfig.difficulty;
    buf[CMD_PACKET_BODY_OFFSET + 2] = cmd->u.lobbyBotConfig.personality;
    buf[CMD_PACKET_BODY_OFFSET + 3] = cmd->u.lobbyBotConfig.mode;
    buf[CMD_PACKET_BODY_OFFSET + 4] = nameLen;
    if (nameLen > 0) {
        memcpy(buf + CMD_PACKET_BODY_OFFSET + 5, cmd->u.lobbyBotConfig.name, nameLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyBotConfig(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 5) return false;
    uint8_t nameLen = buf[CMD_PACKET_BODY_OFFSET + 4];
    if (nameLen >= PACKET_MAX_PLAYER_NAME ||
        len < (size_t)CMD_PACKET_BODY_OFFSET + 5 + nameLen) {
        return false;
    }
    cmd->type = CMD_LOBBY_BOT_CONFIG;
    cmd->u.lobbyBotConfig.slot        = buf[CMD_PACKET_BODY_OFFSET + 0];
    cmd->u.lobbyBotConfig.difficulty  = buf[CMD_PACKET_BODY_OFFSET + 1];
    cmd->u.lobbyBotConfig.personality = buf[CMD_PACKET_BODY_OFFSET + 2];
    cmd->u.lobbyBotConfig.mode        = buf[CMD_PACKET_BODY_OFFSET + 3];
    cmd->u.lobbyBotConfig.nameLen     = nameLen;
    if (nameLen > 0) {
        memcpy(cmd->u.lobbyBotConfig.name,
               buf + CMD_PACKET_BODY_OFFSET + 5, nameLen);
    }
    return true;
}

/* CMD_LOBBY_TEAM_META — PACKET_LOBBY_TEAM_META
 * Wire: [header 8] [teamId 1] [color 1] [namingPool 1] [startSide 1]
 *       [nameLen 1] [name N] */
static bool commandEncodeLobbyTeamMeta(const ClientCommand *cmd,
                                       uint8_t *buf, size_t bufCap,
                                       size_t *outLen) {
    uint8_t nameLen = cmd->u.lobbyTeamMeta.nameLen;
    if (nameLen >= LOBBY_TEAM_NAME_LEN) nameLen = LOBBY_TEAM_NAME_LEN - 1;
    const size_t needed = CMD_PACKET_BODY_OFFSET + 5 + nameLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_TEAM_META, 0);
    buf[CMD_PACKET_BODY_OFFSET + 0] = cmd->u.lobbyTeamMeta.teamId;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.lobbyTeamMeta.color;
    buf[CMD_PACKET_BODY_OFFSET + 2] = cmd->u.lobbyTeamMeta.namingPool;
    buf[CMD_PACKET_BODY_OFFSET + 3] = cmd->u.lobbyTeamMeta.startSide;
    buf[CMD_PACKET_BODY_OFFSET + 4] = nameLen;
    if (nameLen > 0) {
        memcpy(buf + CMD_PACKET_BODY_OFFSET + 5, cmd->u.lobbyTeamMeta.name, nameLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyTeamMeta(const uint8_t *buf, size_t len,
                                       ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 5) return false;
    uint8_t nameLen = buf[CMD_PACKET_BODY_OFFSET + 4];
    if (nameLen > LOBBY_TEAM_NAME_LEN - 1 ||
        len < (size_t)CMD_PACKET_BODY_OFFSET + 5 + nameLen) {
        return false;
    }
    cmd->type = CMD_LOBBY_TEAM_META;
    cmd->u.lobbyTeamMeta.teamId     = buf[CMD_PACKET_BODY_OFFSET + 0];
    cmd->u.lobbyTeamMeta.color      = buf[CMD_PACKET_BODY_OFFSET + 1];
    cmd->u.lobbyTeamMeta.namingPool = buf[CMD_PACKET_BODY_OFFSET + 2];
    cmd->u.lobbyTeamMeta.startSide  = buf[CMD_PACKET_BODY_OFFSET + 3];
    cmd->u.lobbyTeamMeta.nameLen    = nameLen;
    if (nameLen > 0) {
        memcpy(cmd->u.lobbyTeamMeta.name,
               buf + CMD_PACKET_BODY_OFFSET + 5, nameLen);
    }
    return true;
}

/* CMD_LOBBY_TEAM_CLEAR — PACKET_LOBBY_TEAM_CLEAR
 * Wire: [header 8] [teamId 1] */
static bool commandEncodeLobbyTeamClear(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_TEAM_CLEAR, 0);
    buf[CMD_PACKET_BODY_OFFSET] = cmd->u.lobbyTeamClear.teamId;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyTeamClear(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    cmd->type = CMD_LOBBY_TEAM_CLEAR;
    cmd->u.lobbyTeamClear.teamId = buf[CMD_PACKET_BODY_OFFSET];
    return true;
}

/* CMD_LOBBY_SETTING — PACKET_LOBBY_SET_SETTING
 * Wire: [header 8] [settingType 1] [valueLen 1] [value N] */
static bool commandEncodeLobbySetting(const ClientCommand *cmd,
                                      uint8_t *buf, size_t bufCap,
                                      size_t *outLen) {
    uint8_t valueLen = cmd->u.lobbySetting.valueLen;
    if (valueLen > 32) valueLen = 32;
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2 + valueLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_SET_SETTING, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = cmd->u.lobbySetting.settingType;
    buf[CMD_PACKET_BODY_OFFSET + 1] = valueLen;
    if (valueLen > 0) {
        memcpy(buf + CMD_PACKET_BODY_OFFSET + 2, cmd->u.lobbySetting.value, valueLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbySetting(const uint8_t *buf, size_t len,
                                      ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    uint8_t valueLen = buf[CMD_PACKET_BODY_OFFSET + 1];
    if (valueLen > 32 ||
        len < (size_t)CMD_PACKET_BODY_OFFSET + 2 + valueLen) {
        return false;
    }
    cmd->type = CMD_LOBBY_SETTING;
    cmd->u.lobbySetting.settingType = buf[CMD_PACKET_BODY_OFFSET];
    cmd->u.lobbySetting.valueLen    = valueLen;
    if (valueLen > 0) {
        memcpy(cmd->u.lobbySetting.value,
               buf + CMD_PACKET_BODY_OFFSET + 2, valueLen);
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
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1 + bodyLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_CHAT_MESSAGE, 0);
    buf[CMD_PACKET_BODY_OFFSET] = cmd->u.chat.destPlayer;
    if (bodyLen > 0) {
        memcpy(buf + CMD_PACKET_BODY_OFFSET + 1, cmd->u.chat.body, bodyLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeChat(const uint8_t *buf, size_t len,
                              ClientCommand *cmd) {
    if (len <= CMD_PACKET_BODY_OFFSET + 1) return false;
    size_t bodyLen = len - CMD_PACKET_BODY_OFFSET - 1;
    if (bodyLen > PACKET_MAX_CHAT_MESSAGE) bodyLen = PACKET_MAX_CHAT_MESSAGE;
    cmd->type = CMD_CHAT;
    cmd->u.chat.destPlayer = buf[CMD_PACKET_BODY_OFFSET];
    cmd->u.chat.bodyLen    = (uint16_t)bodyLen;
    if (bodyLen > 0) {
        memcpy(cmd->u.chat.body, buf + CMD_PACKET_BODY_OFFSET + 1, bodyLen);
    }
    return true;
}

/* CMD_ALLIANCE_REQUEST — PACKET_ALLIANCE_REQUEST
 * Wire: [header 8] [fromPlayer 1 — legacy] [toPlayer 1] */
static bool commandEncodeAllianceRequest(const ClientCommand *cmd,
                                         uint8_t *buf, size_t bufCap,
                                         size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_ALLIANCE_REQUEST, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = 0;  /* legacy fromPlayer; dispatcher uses senderSlot */
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.allianceRequest.toPlayer;
    *outLen = needed;
    return true;
}

static bool commandDecodeAllianceRequest(const uint8_t *buf, size_t len,
                                         ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    cmd->type = CMD_ALLIANCE_REQUEST;
    cmd->u.allianceRequest.toPlayer = buf[CMD_PACKET_BODY_OFFSET + 1];
    return true;
}

/* CMD_ALLIANCE_ACCEPT — PACKET_ALLIANCE_ACCEPT
 * Wire: [header 8] [fromPlayer 1 — legacy] [newMember 1] */
static bool commandEncodeAllianceAccept(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_ALLIANCE_ACCEPT, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = 0;  /* legacy fromPlayer; dispatcher uses senderSlot */
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.allianceAccept.newMember;
    *outLen = needed;
    return true;
}

static bool commandDecodeAllianceAccept(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    cmd->type = CMD_ALLIANCE_ACCEPT;
    cmd->u.allianceAccept.newMember = buf[CMD_PACKET_BODY_OFFSET + 1];
    return true;
}

/* CMD_ALLIANCE_LEAVE — PACKET_ALLIANCE_LEAVE
 * Wire: [header 8] [playerNum 1 — legacy] */
static bool commandEncodeAllianceLeave(const ClientCommand *cmd,
                                       uint8_t *buf, size_t bufCap,
                                       size_t *outLen) {
    (void)cmd;
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_ALLIANCE_LEAVE, 0);
    buf[CMD_PACKET_BODY_OFFSET] = 0;  /* legacy playerNum; dispatcher uses senderSlot */
    *outLen = needed;
    return true;
}

static bool commandDecodeAllianceLeave(const uint8_t *buf, size_t len,
                                       ClientCommand *cmd) {
    (void)buf;
    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    cmd->type = CMD_ALLIANCE_LEAVE;
    cmd->u.allianceLeave._unused = 0;
    return true;
}

/* CMD_GAME_VOTE_TOGGLE — PACKET_GAME_VOTE_TOGGLE
 * Wire: [header 8] [kind 1] [toggleMode 1] */
static bool commandEncodeGameVoteToggle(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_GAME_VOTE_TOGGLE, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = cmd->u.gameVoteToggle.kind;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.gameVoteToggle.toggleMode;
    *outLen = needed;
    return true;
}

static bool commandDecodeGameVoteToggle(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    cmd->type = CMD_GAME_VOTE_TOGGLE;
    cmd->u.gameVoteToggle.kind       = buf[CMD_PACKET_BODY_OFFSET + 0];
    cmd->u.gameVoteToggle.toggleMode = buf[CMD_PACKET_BODY_OFFSET + 1];
    return true;
}

/* CMD_LOBBY_REMOVE_BOT — PACKET_LOBBY_REMOVE_BOT
 * Wire: [header 8] [playerNum 1] */
static bool commandEncodeLobbyRemoveBot(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_REMOVE_BOT, 0);
    buf[CMD_PACKET_BODY_OFFSET] = cmd->u.lobbyRemoveBot.slot;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyRemoveBot(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    cmd->type = CMD_LOBBY_REMOVE_BOT;
    cmd->u.lobbyRemoveBot.slot = buf[CMD_PACKET_BODY_OFFSET];
    return true;
}

/* CMD_LOBBY_SET_BOT_BRAIN — PACKET_LOBBY_SET_BOT_BRAIN
 * Wire: [header 8] [slot 1] [brainIdx 1] */
static bool commandEncodeLobbySetBotBrain(const ClientCommand *cmd,
                                          uint8_t *buf, size_t bufCap,
                                          size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_SET_BOT_BRAIN, 0);
    buf[CMD_PACKET_BODY_OFFSET + 0] = cmd->u.lobbySetBotBrain.slot;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.lobbySetBotBrain.brainIdx;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbySetBotBrain(const uint8_t *buf, size_t len,
                                          ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    cmd->type = CMD_LOBBY_SET_BOT_BRAIN;
    cmd->u.lobbySetBotBrain.slot     = buf[CMD_PACKET_BODY_OFFSET + 0];
    cmd->u.lobbySetBotBrain.brainIdx = buf[CMD_PACKET_BODY_OFFSET + 1];
    return true;
}

/* CMD_LOBBY_OPEN_HOST — PACKET_LOBBY_OPEN_HOST
 * Wire: [header 8] [openHost 1] */
static bool commandEncodeLobbyOpenHost(const ClientCommand *cmd,
                                       uint8_t *buf, size_t bufCap,
                                       size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_OPEN_HOST, 0);
    buf[CMD_PACKET_BODY_OFFSET] = cmd->u.lobbyOpenHost.openHost ? 1 : 0;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyOpenHost(const uint8_t *buf, size_t len,
                                       ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    cmd->type = CMD_LOBBY_OPEN_HOST;
    cmd->u.lobbyOpenHost.openHost = buf[CMD_PACKET_BODY_OFFSET] != 0;
    return true;
}

/* CMD_MAP_SKIP_VOTE — PACKET_MAP_SKIP_VOTE
 * Wire: [header 8] (no body) */
static bool commandEncodeMapSkipVote(const ClientCommand *cmd,
                                     uint8_t *buf, size_t bufCap,
                                     size_t *outLen) {
    (void)cmd;
    if (bufCap < CMD_PACKET_BODY_OFFSET) return false;
    packHeader(buf, PACKET_MAP_SKIP_VOTE, 0);
    *outLen = CMD_PACKET_BODY_OFFSET;
    return true;
}

static bool commandDecodeMapSkipVote(const uint8_t *buf, size_t len,
                                     ClientCommand *cmd) {
    (void)buf;
    if (len < CMD_PACKET_BODY_OFFSET) return false;
    cmd->type = CMD_MAP_SKIP_VOTE;
    cmd->u.mapSkipVote._unused = 0;
    return true;
}

/* CMD_NAME_CHANGE — PACKET_NAME_CHANGE
 * Wire: [header 8] [playerNum 1 — legacy] [name PACKET_MAX_PLAYER_NAME] */
static bool commandEncodeNameChange(const ClientCommand *cmd,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1 + PACKET_MAX_PLAYER_NAME;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_NAME_CHANGE, 0);
    buf[CMD_PACKET_BODY_OFFSET] = 0;  /* legacy playerNum; dispatcher uses senderSlot */
    memset(buf + CMD_PACKET_BODY_OFFSET + 1, 0, PACKET_MAX_PLAYER_NAME);
    {
        size_t nameLen = strnlen(cmd->u.nameChange.newName,
                                 PACKET_MAX_PLAYER_NAME - 1);
        if (nameLen > 0) {
            memcpy(buf + CMD_PACKET_BODY_OFFSET + 1,
                   cmd->u.nameChange.newName, nameLen);
        }
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeNameChange(const uint8_t *buf, size_t len,
                                    ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 1 + PACKET_MAX_PLAYER_NAME) return false;
    cmd->type = CMD_NAME_CHANGE;
    memcpy(cmd->u.nameChange.newName,
           buf + CMD_PACKET_BODY_OFFSET + 1, PACKET_MAX_PLAYER_NAME);
    cmd->u.nameChange.newName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    return true;
}

/* CMD_LOCK_TOGGLE — PACKET_LOCK_TOGGLE
 * Wire: [header 8] [allow 1] */
static bool commandEncodeLockToggle(const ClientCommand *cmd,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOCK_TOGGLE, 0);
    buf[CMD_PACKET_BODY_OFFSET] = cmd->u.lockToggle.allow ? 1 : 0;
    *outLen = needed;
    return true;
}

static bool commandDecodeLockToggle(const uint8_t *buf, size_t len,
                                    ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    cmd->type = CMD_LOCK_TOGGLE;
    cmd->u.lockToggle.allow = buf[CMD_PACKET_BODY_OFFSET] != 0;
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
    const size_t needed = CMD_PACKET_BODY_OFFSET + 3 + nameLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_ADD_BOT, 0);
    buf[CMD_PACKET_BODY_OFFSET + 0] = cmd->u.lobbyAddBot.teamNumber;
    buf[CMD_PACKET_BODY_OFFSET + 1] = 0;  /* pathLen — server ignores brain payload */
    buf[CMD_PACKET_BODY_OFFSET + 2] = nameLen;
    if (nameLen > 0) {
        memcpy(buf + CMD_PACKET_BODY_OFFSET + 3, cmd->u.lobbyAddBot.name, nameLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyAddBot(const uint8_t *buf, size_t len,
                                     ClientCommand *cmd) {
    /* Lenient mirror of the server arm: accept any (pathLen, nameLen)
     * combination that fits within the packet. Empty/missing fields
     * decode to teamNumber=0, nameLen=0. */
    size_t pos = CMD_PACKET_BODY_OFFSET;
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
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1 + pathLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_SET_MAP, 0);
    buf[CMD_PACKET_BODY_OFFSET] = pathLen;
    if (pathLen > 0) {
        memcpy(buf + CMD_PACKET_BODY_OFFSET + 1,
               cmd->u.lobbySetMap.relPath, pathLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbySetMap(const uint8_t *buf, size_t len,
                                     ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    uint8_t pathLen = buf[CMD_PACKET_BODY_OFFSET];
    if (pathLen == 0 ||
        len < (size_t)CMD_PACKET_BODY_OFFSET + 1 + pathLen) {
        return false;
    }
    cmd->type = CMD_LOBBY_SET_MAP;
    cmd->u.lobbySetMap.relPathLen = pathLen;
    memcpy(cmd->u.lobbySetMap.relPath,
           buf + CMD_PACKET_BODY_OFFSET + 1, pathLen);
    return true;
}

/* CMD_LOBBY_SET_SCENARIO — PACKET_LOBBY_SET_SCENARIO
 * Wire: [header 8] [pathLen 1] [path N]
 * The shape CMD_LOBBY_SET_MAP uses, with one difference: pathLen 0 is
 * carried rather than refused, because an empty path is the message
 * that selects no scenario. */
static bool commandEncodeLobbySetScenario(const ClientCommand *cmd,
                                          uint8_t *buf, size_t bufCap,
                                          size_t *outLen) {
    uint8_t pathLen = cmd->u.lobbySetScenario.relPathLen;
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1 + pathLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_SET_SCENARIO, 0);
    buf[CMD_PACKET_BODY_OFFSET] = pathLen;
    if (pathLen > 0) {
        memcpy(buf + CMD_PACKET_BODY_OFFSET + 1,
               cmd->u.lobbySetScenario.relPath, pathLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbySetScenario(const uint8_t *buf, size_t len,
                                          ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    uint8_t pathLen = buf[CMD_PACKET_BODY_OFFSET];
    if (len < (size_t)CMD_PACKET_BODY_OFFSET + 1 + pathLen) return false;
    cmd->type = CMD_LOBBY_SET_SCENARIO;
    cmd->u.lobbySetScenario.relPathLen = pathLen;
    if (pathLen > 0) {
        memcpy(cmd->u.lobbySetScenario.relPath,
               buf + CMD_PACKET_BODY_OFFSET + 1, pathLen);
    }
    return true;
}

/* CMD_SET_SCRIPT_LIST - PACKET_SET_SCRIPT_LIST
 * Wire: [header 8] [cmdSeq 4] [count 1] then count * [fileLen 1] [file N]
 *
 * The whole list in one command. An index-and-file shape would be smaller,
 * but two hosts editing at the same moment would then interleave into a list
 * neither of them asked for; with the whole list the later command simply
 * wins, and the server's state is an assignment rather than a splice.
 *
 * Each name is a one-byte length and that many bytes with no terminator, the
 * shape CMD_LOBBY_SET_SCENARIO uses beside it. A count past
 * CMD_SCRIPT_LIST_MAX is refused rather than trimmed on both sides: a list
 * the encoder cut short is a different list, and the caller would never
 * learn which scripts it lost. */
static bool commandEncodeSetScriptList(const ClientCommand *cmd,
                                       uint8_t *buf, size_t bufCap,
                                       size_t *outLen) {
    size_t pos;
    size_t i;
    size_t count = cmd->u.setScriptList.count;

    if (count > (size_t)CMD_SCRIPT_LIST_MAX) return false;
    if (bufCap < CMD_PACKET_BODY_OFFSET + 1) return false;
    packHeader(buf, PACKET_SET_SCRIPT_LIST, 0);
    pos = CMD_PACKET_BODY_OFFSET;
    buf[pos++] = (uint8_t)count;
    for (i = 0; i < count; i++) {
        size_t fileLen = strnlen(cmd->u.setScriptList.files[i],
                                 CMD_SCRIPT_LIST_FILE_LEN - 1);
        if (bufCap < pos + 1 + fileLen) return false;
        buf[pos++] = (uint8_t)fileLen;
        if (fileLen > 0) {
            memcpy(buf + pos, cmd->u.setScriptList.files[i], fileLen);
            pos += fileLen;
        }
    }
    *outLen = pos;
    return true;
}

static bool commandDecodeSetScriptList(const uint8_t *buf, size_t len,
                                       ClientCommand *cmd) {
    size_t  pos;
    size_t  i;
    uint8_t count;

    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    count = buf[CMD_PACKET_BODY_OFFSET];
    if (count > CMD_SCRIPT_LIST_MAX) return false;
    /* Zeroed first: the names above count have to be empty, or a shorter
       list would leave the tail of a longer one behind it for anything that
       reads past the count. */
    memset(&cmd->u.setScriptList, 0, sizeof(cmd->u.setScriptList));
    cmd->type = CMD_SET_SCRIPT_LIST;
    cmd->u.setScriptList.count = count;
    pos = CMD_PACKET_BODY_OFFSET + 1;
    for (i = 0; i < count; i++) {
        uint8_t fileLen;
        if (len < pos + 1) return false;
        fileLen = buf[pos++];
        if (fileLen > CMD_SCRIPT_LIST_FILE_LEN - 1) return false;
        if (len < pos + fileLen) return false;
        if (fileLen > 0) {
            memcpy(cmd->u.setScriptList.files[i], buf + pos, fileLen);
        }
        cmd->u.setScriptList.files[i][fileLen] = '\0';
        pos += fileLen;
    }
    return true;
}

/* CMD_LOBBY_PREVIEW_CANCEL — PACKET_LOBBY_PREVIEW_CANCEL
 * Wire: [header 8] (no body) */
static bool commandEncodeLobbyPreviewCancel(const ClientCommand *cmd,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)cmd;
    if (bufCap < CMD_PACKET_BODY_OFFSET) return false;
    packHeader(buf, PACKET_LOBBY_PREVIEW_CANCEL, 0);
    *outLen = CMD_PACKET_BODY_OFFSET;
    return true;
}

static bool commandDecodeLobbyPreviewCancel(const uint8_t *buf, size_t len,
                                            ClientCommand *cmd) {
    (void)buf;
    if (len < CMD_PACKET_BODY_OFFSET) return false;
    cmd->type = CMD_LOBBY_PREVIEW_CANCEL;
    cmd->u.lobbyPreviewCancel._unused = 0;
    return true;
}

/* CMD_LOBBY_RELOAD_SCENARIO — PACKET_LOBBY_RELOAD_SCENARIO
 * Wire: [header 8] (no body) */
static bool commandEncodeLobbyReloadScenario(const ClientCommand *cmd,
                                             uint8_t *buf, size_t bufCap,
                                             size_t *outLen) {
    (void)cmd;
    if (bufCap < CMD_PACKET_BODY_OFFSET) return false;
    packHeader(buf, PACKET_LOBBY_RELOAD_SCENARIO, 0);
    *outLen = CMD_PACKET_BODY_OFFSET;
    return true;
}

static bool commandDecodeLobbyReloadScenario(const uint8_t *buf, size_t len,
                                             ClientCommand *cmd) {
    (void)buf;
    if (len < CMD_PACKET_BODY_OFFSET) return false;
    cmd->type = CMD_LOBBY_RELOAD_SCENARIO;
    cmd->u.lobbyReloadScenario._unused = 0;
    return true;
}

/* CMD_LOBBY_PREVIEW_COMMIT — PACKET_LOBBY_PREVIEW_COMMIT
 * Wire: [header 8] (no body) */
static bool commandEncodeLobbyPreviewCommit(const ClientCommand *cmd,
                                            uint8_t *buf, size_t bufCap,
                                            size_t *outLen) {
    (void)cmd;
    if (bufCap < CMD_PACKET_BODY_OFFSET) return false;
    packHeader(buf, PACKET_LOBBY_PREVIEW_COMMIT, 0);
    *outLen = CMD_PACKET_BODY_OFFSET;
    return true;
}

static bool commandDecodeLobbyPreviewCommit(const uint8_t *buf, size_t len,
                                            ClientCommand *cmd) {
    (void)buf;
    if (len < CMD_PACKET_BODY_OFFSET) return false;
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
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1 + seedLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_PREVIEW_RANDOM, 0);
    buf[CMD_PACKET_BODY_OFFSET] = seedLen;
    if (seedLen > 0) {
        memcpy(buf + CMD_PACKET_BODY_OFFSET + 1,
               cmd->u.lobbyPreviewRandom.seed, seedLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyPreviewRandom(const uint8_t *buf, size_t len,
                                            ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    uint8_t seedLen = buf[CMD_PACKET_BODY_OFFSET];
    if (seedLen > 63 ||
        len < (size_t)CMD_PACKET_BODY_OFFSET + 1 + seedLen) {
        return false;
    }
    cmd->type = CMD_LOBBY_PREVIEW_RANDOM;
    cmd->u.lobbyPreviewRandom.seedLen = seedLen;
    if (seedLen > 0) {
        memcpy(cmd->u.lobbyPreviewRandom.seed,
               buf + CMD_PACKET_BODY_OFFSET + 1, seedLen);
    }
    return true;
}

/* CMD_LOBBY_KICK — PACKET_LOBBY_KICK
 * Wire: [header 8] [slot 1] */
static bool commandEncodeLobbyKick(const ClientCommand *cmd,
                                   uint8_t *buf, size_t bufCap,
                                   size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_KICK, 0);
    buf[CMD_PACKET_BODY_OFFSET] = cmd->u.lobbyKick.slot;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyKick(const uint8_t *buf, size_t len,
                                   ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    cmd->type = CMD_LOBBY_KICK;
    cmd->u.lobbyKick.slot = buf[CMD_PACKET_BODY_OFFSET];
    return true;
}

/* CMD_LOBBY_TRANSFER_HOST — PACKET_LOBBY_TRANSFER_HOST
 * Wire: [header 8] [slot 1] */
static bool commandEncodeLobbyTransferHost(const ClientCommand *cmd,
                                           uint8_t *buf, size_t bufCap,
                                           size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_TRANSFER_HOST, 0);
    buf[CMD_PACKET_BODY_OFFSET] = cmd->u.lobbyTransferHost.slot;
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbyTransferHost(const uint8_t *buf, size_t len,
                                           ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    cmd->type = CMD_LOBBY_TRANSFER_HOST;
    cmd->u.lobbyTransferHost.slot = buf[CMD_PACKET_BODY_OFFSET];
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
    const size_t needed = CMD_PACKET_BODY_OFFSET + 1 + pwLen;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_LOBBY_SET_PASSWORD, 0);
    buf[CMD_PACKET_BODY_OFFSET] = pwLen;
    if (pwLen > 0) {
        memcpy(buf + CMD_PACKET_BODY_OFFSET + 1,
               cmd->u.lobbySetPassword.password, pwLen);
    }
    *outLen = needed;
    return true;
}

static bool commandDecodeLobbySetPassword(const uint8_t *buf, size_t len,
                                          ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 1) return false;
    uint8_t pwLen = buf[CMD_PACKET_BODY_OFFSET];
    if (len < (size_t)CMD_PACKET_BODY_OFFSET + 1 + pwLen) return false;
    cmd->type = CMD_LOBBY_SET_PASSWORD;
    cmd->u.lobbySetPassword.pwLen = pwLen;
    if (pwLen > 0 &&
        pwLen <= sizeof(cmd->u.lobbySetPassword.password)) {
        memcpy(cmd->u.lobbySetPassword.password,
               buf + CMD_PACKET_BODY_OFFSET + 1, pwLen);
    }
    return true;
}

/* CMD_BALANCE_REQUEST — PACKET_BALANCE_REQUEST
 * Wire: [header 8] [teamSize 1] [includeBots 1] */
static bool commandEncodeBalanceRequest(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_BALANCE_REQUEST, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = cmd->u.balanceRequest.teamSize;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.balanceRequest.includeBots ? 1 : 0;
    *outLen = needed;
    return true;
}

static bool commandDecodeBalanceRequest(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    cmd->type = CMD_BALANCE_REQUEST;
    cmd->u.balanceRequest.teamSize    = buf[CMD_PACKET_BODY_OFFSET];
    cmd->u.balanceRequest.includeBots = buf[CMD_PACKET_BODY_OFFSET + 1] != 0;
    return true;
}

/* CMD_BALANCE_APPLY — PACKET_BALANCE_APPLY
 * Wire: [header 8] (no body) */
static bool commandEncodeBalanceApply(const ClientCommand *cmd,
                                      uint8_t *buf, size_t bufCap,
                                      size_t *outLen) {
    (void)cmd;
    if (bufCap < CMD_PACKET_BODY_OFFSET) return false;
    packHeader(buf, PACKET_BALANCE_APPLY, 0);
    *outLen = CMD_PACKET_BODY_OFFSET;
    return true;
}

static bool commandDecodeBalanceApply(const uint8_t *buf, size_t len,
                                      ClientCommand *cmd) {
    (void)buf;
    if (len < CMD_PACKET_BODY_OFFSET) return false;
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
    if (bufCap < CMD_PACKET_BODY_OFFSET) return false;
    packHeader(buf, PACKET_BALANCE_DISMISS, 0);
    *outLen = CMD_PACKET_BODY_OFFSET;
    return true;
}

static bool commandDecodeBalanceDismiss(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    (void)buf;
    if (len < CMD_PACKET_BODY_OFFSET) return false;
    cmd->type = CMD_BALANCE_DISMISS;
    cmd->u.balanceDismiss._unused = 0;
    return true;
}

/* CMD_WBN_REAUTH — PACKET_WBN_REAUTH
 * Wire: [header 8] [token WBN_JOIN_KEY_WIRE_LEN] */
static bool commandEncodeWbnReauth(const ClientCommand *cmd,
                                   uint8_t *buf, size_t bufCap,
                                   size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + WBN_JOIN_KEY_WIRE_LEN;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_WBN_REAUTH, 0);
    memcpy(buf + CMD_PACKET_BODY_OFFSET,
           cmd->u.wbnReauth.token, WBN_JOIN_KEY_WIRE_LEN);
    *outLen = needed;
    return true;
}

static bool commandDecodeWbnReauth(const uint8_t *buf, size_t len,
                                   ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + WBN_JOIN_KEY_WIRE_LEN) return false;
    cmd->type = CMD_WBN_REAUTH;
    memcpy(cmd->u.wbnReauth.token,
           buf + CMD_PACKET_BODY_OFFSET, WBN_JOIN_KEY_WIRE_LEN);
    cmd->u.wbnReauth.token[WBN_JOIN_KEY_WIRE_LEN - 1] = '\0';
    return true;
}

/* CMD_RATING_POSTED — PACKET_RATING_POSTED
 * Wire: [header 8] [key RATING_POSTED_KEY_WIRE] — fixed length, no length
 * prefix; a short key is NUL-padded out to fill the field. */
#define RATING_POSTED_KEY_WIRE (ROUND_STATS_LOGKEY_LEN - 1)

static bool commandEncodeRatingPosted(const ClientCommand *cmd,
                                      uint8_t *buf, size_t bufCap,
                                      size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + RATING_POSTED_KEY_WIRE;
    size_t keyLen = strnlen(cmd->u.ratingPosted.key, RATING_POSTED_KEY_WIRE);
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_RATING_POSTED, 0);
    memset(buf + CMD_PACKET_BODY_OFFSET, 0, RATING_POSTED_KEY_WIRE);
    memcpy(buf + CMD_PACKET_BODY_OFFSET, cmd->u.ratingPosted.key, keyLen);
    *outLen = needed;
    return true;
}

/* CMD_PLAYER_MUTE — PACKET_PLAYER_MUTE
 * Wire: [header 8] [targetPlayer 1] [muted 1] */
static bool commandEncodePlayerMute(const ClientCommand *cmd,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_PLAYER_MUTE, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = cmd->u.playerMute.targetPlayer;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.playerMute.muted ? 1 : 0;
    *outLen = needed;
    return true;
}

static bool commandDecodeRatingPosted(const uint8_t *buf, size_t len,
                                      ClientCommand *cmd) {
    /* Fixed-length body: anything shorter or longer is not this command. */
    if (len != CMD_PACKET_BODY_OFFSET + RATING_POSTED_KEY_WIRE) return false;
    cmd->type = CMD_RATING_POSTED;
    memcpy(cmd->u.ratingPosted.key, buf + CMD_PACKET_BODY_OFFSET,
           RATING_POSTED_KEY_WIRE);
    cmd->u.ratingPosted.key[RATING_POSTED_KEY_WIRE] = '\0';
    return true;
}

static bool commandDecodePlayerMute(const uint8_t *buf, size_t len,
                                    ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    uint8_t targetPlayer = buf[CMD_PACKET_BODY_OFFSET];
    if (targetPlayer >= MAX_TANKS) return false;
    cmd->type = CMD_PLAYER_MUTE;
    cmd->u.playerMute.targetPlayer = targetPlayer;
    cmd->u.playerMute.muted = buf[CMD_PACKET_BODY_OFFSET + 1] ? 1 : 0;
    return true;
}

/* CMD_PLAYER_PING_MUTE — PACKET_PLAYER_PING_MUTE
 * Wire: [header 8] [targetPlayer 1] [muted 1]. Byte-for-byte the same shape as
 * PLAYER_MUTE; a separate packet id keeps the two mutes independent. */
static bool commandEncodePlayerPingMute(const ClientCommand *cmd,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_PLAYER_PING_MUTE, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = cmd->u.playerPingMute.targetPlayer;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.playerPingMute.muted ? 1 : 0;
    *outLen = needed;
    return true;
}

static bool commandDecodePlayerPingMute(const uint8_t *buf, size_t len,
                                        ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    uint8_t targetPlayer = buf[CMD_PACKET_BODY_OFFSET];
    if (targetPlayer >= MAX_TANKS) return false;
    cmd->type = CMD_PLAYER_PING_MUTE;
    cmd->u.playerPingMute.targetPlayer = targetPlayer;
    cmd->u.playerPingMute.muted = buf[CMD_PACKET_BODY_OFFSET + 1] ? 1 : 0;
    return true;
}

/* CMD_VOICE_STATE — PACKET_VOICE_STATE
 * Wire: [header 8] [hasMic 1] [selfMuted 1] */
static bool commandEncodeVoiceState(const ClientCommand *cmd,
                                    uint8_t *buf, size_t bufCap,
                                    size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_VOICE_STATE, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = cmd->u.voiceState.hasMic ? 1 : 0;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.voiceState.selfMuted ? 1 : 0;
    *outLen = needed;
    return true;
}

static bool commandDecodeVoiceState(const uint8_t *buf, size_t len,
                                    ClientCommand *cmd) {
    if (len < CMD_PACKET_BODY_OFFSET + 2) return false;
    cmd->type = CMD_VOICE_STATE;
    cmd->u.voiceState.hasMic    = buf[CMD_PACKET_BODY_OFFSET] ? 1 : 0;
    cmd->u.voiceState.selfMuted = buf[CMD_PACKET_BODY_OFFSET + 1] ? 1 : 0;
    return true;
}

/* CMD_VIEW_STATE — PACKET_VIEW_STATE
 * Wire: [header 8] [kind 1] [target 1] — fixed length. */
static bool commandEncodeViewState(const ClientCommand *cmd,
                                   uint8_t *buf, size_t bufCap,
                                   size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 2;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_VIEW_STATE, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = cmd->u.viewState.kind;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.viewState.target;
    *outLen = needed;
    return true;
}

static bool commandDecodeViewState(const uint8_t *buf, size_t len,
                                   ClientCommand *cmd) {
    /* Fixed-length body: anything shorter or longer is not this command. */
    if (len != CMD_PACKET_BODY_OFFSET + 2) return false;
    cmd->type = CMD_VIEW_STATE;
    cmd->u.viewState.kind   = buf[CMD_PACKET_BODY_OFFSET];
    cmd->u.viewState.target = buf[CMD_PACKET_BODY_OFFSET + 1];
    return true;
}

/* CMD_VIEW_CYCLE — PACKET_VIEW_CYCLE
 * Wire: [header 8] [kind 1] [direction 1] [from 1] — fixed length. */
static bool commandEncodeViewCycle(const ClientCommand *cmd,
                                   uint8_t *buf, size_t bufCap,
                                   size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 3;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_VIEW_CYCLE, 0);
    buf[CMD_PACKET_BODY_OFFSET]     = cmd->u.viewCycle.kind;
    buf[CMD_PACKET_BODY_OFFSET + 1] = cmd->u.viewCycle.direction;
    buf[CMD_PACKET_BODY_OFFSET + 2] = cmd->u.viewCycle.from;
    *outLen = needed;
    return true;
}

static bool commandDecodeViewCycle(const uint8_t *buf, size_t len,
                                   ClientCommand *cmd) {
    /* Fixed-length body: anything shorter or longer is not this command. */
    if (len != CMD_PACKET_BODY_OFFSET + 3) return false;
    cmd->type = CMD_VIEW_CYCLE;
    cmd->u.viewCycle.kind      = buf[CMD_PACKET_BODY_OFFSET];
    cmd->u.viewCycle.direction = buf[CMD_PACKET_BODY_OFFSET + 1];
    cmd->u.viewCycle.from      = buf[CMD_PACKET_BODY_OFFSET + 2];
    return true;
}

/* CMD_PING — PACKET_MAP_PING
 * Wire: [header 8] [kind 1] [worldX 2] [worldY 2] — fixed length. The
 * position is in WORLD units (256 per map tile) so the marker lands where
 * the cursor was rather than on a tile centre. */
static bool commandEncodePing(const ClientCommand *cmd,
                              uint8_t *buf, size_t bufCap,
                              size_t *outLen) {
    const size_t needed = CMD_PACKET_BODY_OFFSET + 5;
    if (bufCap < needed) return false;
    packHeader(buf, PACKET_MAP_PING, 0);
    buf[CMD_PACKET_BODY_OFFSET] = cmd->u.ping.kind;
    packU16(buf + CMD_PACKET_BODY_OFFSET + 1, cmd->u.ping.worldX);
    packU16(buf + CMD_PACKET_BODY_OFFSET + 3, cmd->u.ping.worldY);
    *outLen = needed;
    return true;
}

static bool commandDecodePing(const uint8_t *buf, size_t len,
                              ClientCommand *cmd) {
    /* Fixed-length body: anything shorter or longer is not this command. */
    if (len != CMD_PACKET_BODY_OFFSET + 5) return false;
    cmd->type = CMD_PING;
    cmd->u.ping.kind   = buf[CMD_PACKET_BODY_OFFSET];
    cmd->u.ping.worldX = unpackU16(buf + CMD_PACKET_BODY_OFFSET + 1);
    cmd->u.ping.worldY = unpackU16(buf + CMD_PACKET_BODY_OFFSET + 3);
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
    bool ok = false;
    switch (cmd->type) {
        case CMD_TEAM_SET:              ok = commandEncodeTeamSet(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_CLAIM_START:     ok = commandEncodeLobbyClaimStart(cmd, buf, bufCap, outLen); break;
        case CMD_READY:                 ok = commandEncodeReady(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_BOT_CONFIG:      ok = commandEncodeLobbyBotConfig(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_TEAM_META:       ok = commandEncodeLobbyTeamMeta(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_TEAM_CLEAR:      ok = commandEncodeLobbyTeamClear(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_SETTING:         ok = commandEncodeLobbySetting(cmd, buf, bufCap, outLen); break;
        case CMD_CHAT:                  ok = commandEncodeChat(cmd, buf, bufCap, outLen); break;
        case CMD_ALLIANCE_REQUEST:      ok = commandEncodeAllianceRequest(cmd, buf, bufCap, outLen); break;
        case CMD_ALLIANCE_ACCEPT:       ok = commandEncodeAllianceAccept(cmd, buf, bufCap, outLen); break;
        case CMD_ALLIANCE_LEAVE:        ok = commandEncodeAllianceLeave(cmd, buf, bufCap, outLen); break;
        case CMD_GAME_VOTE_TOGGLE:      ok = commandEncodeGameVoteToggle(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_REMOVE_BOT:      ok = commandEncodeLobbyRemoveBot(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_SET_BOT_BRAIN:   ok = commandEncodeLobbySetBotBrain(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_OPEN_HOST:       ok = commandEncodeLobbyOpenHost(cmd, buf, bufCap, outLen); break;
        case CMD_MAP_SKIP_VOTE:         ok = commandEncodeMapSkipVote(cmd, buf, bufCap, outLen); break;
        case CMD_NAME_CHANGE:           ok = commandEncodeNameChange(cmd, buf, bufCap, outLen); break;
        case CMD_LOCK_TOGGLE:           ok = commandEncodeLockToggle(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_ADD_BOT:         ok = commandEncodeLobbyAddBot(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_SET_MAP:         ok = commandEncodeLobbySetMap(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_SET_SCENARIO:    ok = commandEncodeLobbySetScenario(cmd, buf, bufCap, outLen); break;
        case CMD_SET_SCRIPT_LIST:       ok = commandEncodeSetScriptList(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_PREVIEW_CANCEL:  ok = commandEncodeLobbyPreviewCancel(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_RELOAD_SCENARIO: ok = commandEncodeLobbyReloadScenario(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_PREVIEW_COMMIT:  ok = commandEncodeLobbyPreviewCommit(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_PREVIEW_RANDOM:  ok = commandEncodeLobbyPreviewRandom(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_KICK:            ok = commandEncodeLobbyKick(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_TRANSFER_HOST:   ok = commandEncodeLobbyTransferHost(cmd, buf, bufCap, outLen); break;
        case CMD_LOBBY_SET_PASSWORD:    ok = commandEncodeLobbySetPassword(cmd, buf, bufCap, outLen); break;
        case CMD_BALANCE_REQUEST:       ok = commandEncodeBalanceRequest(cmd, buf, bufCap, outLen); break;
        case CMD_BALANCE_APPLY:         ok = commandEncodeBalanceApply(cmd, buf, bufCap, outLen); break;
        case CMD_BALANCE_DISMISS:       ok = commandEncodeBalanceDismiss(cmd, buf, bufCap, outLen); break;
        case CMD_WBN_REAUTH:            ok = commandEncodeWbnReauth(cmd, buf, bufCap, outLen); break;
        case CMD_RATING_POSTED:         ok = commandEncodeRatingPosted(cmd, buf, bufCap, outLen); break;
        case CMD_PLAYER_MUTE:           ok = commandEncodePlayerMute(cmd, buf, bufCap, outLen); break;
        case CMD_VOICE_STATE:           ok = commandEncodeVoiceState(cmd, buf, bufCap, outLen); break;
        case CMD_VIEW_STATE:            ok = commandEncodeViewState(cmd, buf, bufCap, outLen); break;
        case CMD_VIEW_CYCLE:            ok = commandEncodeViewCycle(cmd, buf, bufCap, outLen); break;
        case CMD_PING:                  ok = commandEncodePing(cmd, buf, bufCap, outLen); break;
        case CMD_PLAYER_PING_MUTE:      ok = commandEncodePlayerPingMute(cmd, buf, bufCap, outLen); break;
        case CMD_NONE:
        default:                        return false;
    }
    if (!ok) return false;
    packU32(buf + PACKET_HEADER_SIZE, cmd->cmdSeq);
    return true;
}

bool commandCodecDecode(const uint8_t *buf, size_t len,
                        ClientCommand *cmd) {
    if (!buf || !cmd || len < CMD_PACKET_BODY_OFFSET) return false;
    if (buf[0] != BOLO_NEW_MAGIC_0 || buf[1] != BOLO_NEW_MAGIC_1) return false;
    cmd->cmdSeq = unpackU32(buf + PACKET_HEADER_SIZE);
    uint8_t pktType = buf[2];
    switch (pktType) {
        case PACKET_LOBBY_TEAM_SET:        return commandDecodeTeamSet(buf, len, cmd);
        case PACKET_LOBBY_CLAIM_START:     return commandDecodeLobbyClaimStart(buf, len, cmd);
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
        case PACKET_LOBBY_SET_SCENARIO:    return commandDecodeLobbySetScenario(buf, len, cmd);
        case PACKET_SET_SCRIPT_LIST:       return commandDecodeSetScriptList(buf, len, cmd);
        case PACKET_LOBBY_PREVIEW_CANCEL:  return commandDecodeLobbyPreviewCancel(buf, len, cmd);
        case PACKET_LOBBY_RELOAD_SCENARIO: return commandDecodeLobbyReloadScenario(buf, len, cmd);
        case PACKET_LOBBY_PREVIEW_COMMIT:  return commandDecodeLobbyPreviewCommit(buf, len, cmd);
        case PACKET_LOBBY_PREVIEW_RANDOM:  return commandDecodeLobbyPreviewRandom(buf, len, cmd);
        case PACKET_LOBBY_KICK:            return commandDecodeLobbyKick(buf, len, cmd);
        case PACKET_LOBBY_TRANSFER_HOST:   return commandDecodeLobbyTransferHost(buf, len, cmd);
        case PACKET_LOBBY_SET_PASSWORD:    return commandDecodeLobbySetPassword(buf, len, cmd);
        case PACKET_BALANCE_REQUEST:       return commandDecodeBalanceRequest(buf, len, cmd);
        case PACKET_BALANCE_APPLY:         return commandDecodeBalanceApply(buf, len, cmd);
        case PACKET_BALANCE_DISMISS:       return commandDecodeBalanceDismiss(buf, len, cmd);
        case PACKET_WBN_REAUTH:            return commandDecodeWbnReauth(buf, len, cmd);
        case PACKET_RATING_POSTED:         return commandDecodeRatingPosted(buf, len, cmd);
        case PACKET_PLAYER_MUTE:           return commandDecodePlayerMute(buf, len, cmd);
        case PACKET_VOICE_STATE:           return commandDecodeVoiceState(buf, len, cmd);
        case PACKET_VIEW_STATE:            return commandDecodeViewState(buf, len, cmd);
        case PACKET_VIEW_CYCLE:            return commandDecodeViewCycle(buf, len, cmd);
        case PACKET_MAP_PING:              return commandDecodePing(buf, len, cmd);
        case PACKET_PLAYER_PING_MUTE:      return commandDecodePlayerPingMute(buf, len, cmd);
        default:                           return false;
    }
}
