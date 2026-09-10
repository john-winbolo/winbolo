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
#include "round_stats.h"  /* ROUND_STATS_LOGKEY_LEN — CmdRatingPosted key */

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
    CMD_LOBBY_SETTING,
    CMD_CHAT,
    CMD_ALLIANCE_REQUEST,
    CMD_ALLIANCE_ACCEPT,
    CMD_ALLIANCE_LEAVE,
    CMD_GAME_VOTE_TOGGLE,
    CMD_LOBBY_REMOVE_BOT,
    CMD_LOBBY_SET_BOT_BRAIN,
    CMD_LOBBY_OPEN_HOST,
    CMD_MAP_SKIP_VOTE,
    CMD_NAME_CHANGE,
    CMD_LOCK_TOGGLE,
    CMD_LOBBY_ADD_BOT,
    CMD_LOBBY_SET_MAP,
    CMD_LOBBY_PREVIEW_CANCEL,
    CMD_LOBBY_PREVIEW_COMMIT,
    CMD_LOBBY_PREVIEW_RANDOM,
    CMD_LOBBY_KICK,
    CMD_LOBBY_TRANSFER_HOST,
    CMD_LOBBY_SET_PASSWORD,
    CMD_BALANCE_REQUEST,
    CMD_BALANCE_APPLY,
    CMD_BALANCE_DISMISS,
    CMD_WBN_REAUTH,
    CMD_LOBBY_CLAIM_START,
    CMD_RATING_POSTED,
    CMD_VIEW_STATE,
    CMD_VIEW_CYCLE,
    CMD_PLAYER_MUTE,
    CMD_VOICE_STATE,
    CMD_PING
} ClientCommandType;

/* Reject codes returned by serverSimApplyCommand. The dispatcher
 * surfaces these to the originating client via CTRL_COMMAND_REJECTED.
 * Codes 1-7 are intentionally aligned 1:1 with the existing
 * LOBBY_REJECT_* constants in src/bolo/internal/netpacks.h so the
 * UDP server's map-upload arms can reuse the same numeric values. */
typedef enum {
    CMD_OK = 0,
    CMD_REJECT_NOT_HOST,
    CMD_REJECT_LOCKED,
    CMD_REJECT_INVALID,
    CMD_REJECT_UPLOAD_BUSY,
    CMD_REJECT_UPLOAD_DISABLED,
    CMD_REJECT_UPLOAD_LIMIT_HIT,
    CMD_REJECT_COOLDOWN,
    CMD_REJECT_BAD_STATE,
    /* NameChange-specific reject codes. Surfaced to the originating
     * client via CTRL_COMMAND_REJECTED.reasonCode; the lobby toast
     * (lobbyRenderRejectToast in imgui_lobby.cpp) maps each onto a
     * STR_NAME_INVALID_* / STR_DLGSETNAME_INUSE_ERR lang string. */
    CMD_REJECT_NAME_EMPTY,
    CMD_REJECT_NAME_RESERVED_PREFIX,
    CMD_REJECT_NAME_RESERVED_SUFFIX,
    CMD_REJECT_NAME_MIXED_SCRIPTS,
    CMD_REJECT_NAME_INVALID,
    CMD_REJECT_NAME_TAKEN,
    /* Lobby add-bot refused because the server's -maxbots cap is
     * already reached. Surfaced to the host via the reject toast as a
     * dedicated "bot limit reached" line rather than the generic
     * CMD_REJECT_INVALID. */
    CMD_REJECT_BOT_LIMIT
} CmdResult;

/* CMD_TEAM_SET — set the team number for a lobby slot. Sender must
 * be moving its own slot, or be host/admin/openHost to move another
 * slot. Both fields validated against MAX_TANKS. */
typedef struct {
    uint8_t slot;
    uint8_t team;
} CmdTeamSet;

/* CMD_LOBBY_CLAIM_START — set a slot's reserved start. targetSlot ==
 * senderSlot is a self-claim (target start must be free and one the
 * sender's team side accepts, or a sentinel below); targetSlot != senderSlot
 * is host-only (lobbyClientMayEdit) and swaps when the target start is
 * occupied. startIdx is 1-based; 0xFF releases the slot's start and leaves
 * it empty until the next lobby event moves reservations, while
 * START_CLAIM_TEAM_SIDE releases it and picks a fresh start on the slot's
 * team side at once (0xFF when the side is full). */
#define START_CLAIM_TEAM_SIDE 0xFE
typedef struct {
    uint8_t targetSlot;
    uint8_t startIdx;
} CmdLobbyClaimStart;

/* CMD_READY — toggle ready state for the sender's slot. The wire
 * carries a playerNum byte for backward compatibility but the
 * server uses senderSlot per the attribution contract. */
typedef struct {
    bool ready;
} CmdReady;

/* CMD_LOBBY_BOT_CONFIG — update mode/difficulty/personality (+ optional
 * rename) for a bot slot. nameLen == 0 means "keep current name".
 * mode indexes the brain's own mode list (brain_list.h) and difficulty
 * indexes that mode's level list.
 * Bot-config validation runs against the connected-player table
 * via transportUdpServerGetPlayerName; the stub for non-server
 * binaries returns NULL (no collision) on every slot. */
typedef struct {
    uint8_t slot;
    uint8_t difficulty;
    uint8_t personality;
    uint8_t mode;
    uint8_t nameLen;
    char    name[PACKET_MAX_PLAYER_NAME];
} CmdLobbyBotConfig;

/* CMD_LOBBY_TEAM_META — set color, naming pool, start side, and
 * optional name for a team. nameLen == 0 means "no name set". teamId is
 * 1..MAX_TANKS-1 (team 0 is unassigned). */
typedef struct {
    uint8_t teamId;
    uint8_t color;
    uint8_t namingPool;
    uint8_t startSide;   /* START_SIDE_* (start_sides.h) */
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

/* CMD_CHAT — broadcast or directed chat. destPlayer == 0xFF means
 * "all players"; any other value targets a specific slot. body is
 * not NUL-terminated; bodyLen counts the used prefix. */
typedef struct {
    uint8_t  destPlayer;
    uint16_t bodyLen;
    char     body[PACKET_MAX_CHAT_MESSAGE];
} CmdChat;

/* Team-addressed chat. destPlayer carries the target team in a bounded
 * range above the slot/sentinel space: CHAT_DEST_TEAM_BASE + teamNumber,
 * team 1..16 -> 0x81..0x90. Use the bounded predicate, NOT (d & 0x80) —
 * 0x80 aliases the 0xFF broadcast sentinel and would swallow broadcast
 * chat. Every routing site checks 0xFF (broadcast) first, then
 * CHAT_DEST_IS_TEAM, then slot unicast. */
#define CHAT_DEST_TEAM_BASE 0x80
#define CHAT_DEST_IS_TEAM(d) ((d) >= 0x81 && (d) <= 0x90)
#define CHAT_DEST_TEAM_OF(d) ((uint8_t)((d) - CHAT_DEST_TEAM_BASE))

/* CMD_ALLIANCE_REQUEST — sender wants to ally with toPlayer.
 * Sender is senderSlot (the wire's fromPlayer byte is vestigial). */
typedef struct {
    uint8_t toPlayer;
} CmdAllianceRequest;

/* CMD_ALLIANCE_ACCEPT — sender (the accepter) admits newMember
 * to their alliance. Wire's fromPlayer = accepter is vestigial;
 * sender comes from senderSlot. */
typedef struct {
    uint8_t newMember;
} CmdAllianceAccept;

/* CMD_ALLIANCE_LEAVE — sender leaves their alliance. No payload —
 * the leaver is always senderSlot. */
typedef struct {
    uint8_t _unused;
} CmdAllianceLeave;

/* CMD_GAME_VOTE_TOGGLE — toggle / set a game-time vote (e.g.
 * surrender). kind selects the vote; toggleMode is the per-vote
 * payload semantic (0/1/2 in current usage). */
typedef struct {
    uint8_t kind;
    uint8_t toggleMode;
} CmdGameVoteToggle;

/* CMD_LOBBY_REMOVE_BOT — host-gated removal of a bot from the
 * lobby. slot must reference an active bot. */
typedef struct {
    uint8_t slot;
} CmdLobbyRemoveBot;

/* CMD_LOBBY_SET_BOT_BRAIN — host-gated reassignment of a bot's
 * brain. brainIdx == 0xFF resolves to the CLI-configured default;
 * other values must index into the server's brain catalogue. */
typedef struct {
    uint8_t slot;
    uint8_t brainIdx;
} CmdLobbySetBotBrain;

/* CMD_LOBBY_OPEN_HOST — slot-0 only: toggle the "any connected
 * player may edit lobby state" mode. Gated by LOBBY_LOCK_OPEN_HOST. */
typedef struct {
    bool openHost;
} CmdLobbyOpenHost;

/* CMD_MAP_SKIP_VOTE — no payload; the sender is voting to skip
 * the current map. Gated by LOBBY_LOCK_MAP. */
typedef struct {
    uint8_t _unused;
} CmdMapSkipVote;

typedef struct {
    char newName[PACKET_MAX_PLAYER_NAME];
} CmdNameChange;

typedef struct {
    bool allow;
} CmdLockToggle;

/* CMD_LOBBY_ADD_BOT — host-gated add. teamNumber == 0 → server picks;
 * name empty → "Bot N" fallback. Path byte from the wire is dropped
 * (server uses its own configured brain path). */
typedef struct {
    uint8_t teamNumber;
    uint8_t nameLen;
    char    name[PACKET_MAX_PLAYER_NAME];
} CmdLobbyAddBot;

/* CMD_LOBBY_SET_MAP — host-gated map reload. relPath is relative to
 * data/maps/; the arm rejects absolute paths, Windows drive letters,
 * and ".." segments before calling serverSimReloadMap. */
typedef struct {
    uint8_t relPathLen;
    char    relPath[256];
} CmdLobbySetMap;

typedef struct {
    uint8_t _unused;
} CmdLobbyPreviewCancel;

typedef struct {
    uint8_t _unused;
} CmdLobbyPreviewCommit;

typedef struct {
    uint8_t seedLen;
    char    seed[64];
} CmdLobbyPreviewRandom;

/* CMD_LOBBY_KICK — host-gated. Cannot kick host (slot 0) or self. */
typedef struct {
    uint8_t slot;
} CmdLobbyKick;

/* CMD_LOBBY_TRANSFER_HOST — host-only (openHost does NOT grant this).
 * Hand the host role to another connected human. */
typedef struct {
    uint8_t slot;
} CmdLobbyTransferHost;

/* CMD_LOBBY_SET_PASSWORD — host or admin only (openHost does NOT
 * grant this — would let a connected player lock the host out).
 * pwLen == 0 clears the password. */
typedef struct {
    uint8_t pwLen;
    char    password[64];
} CmdLobbySetPassword;

/* CMD_BALANCE_REQUEST — host-only ranked-server WBN matchmaking
 * request. Spawns a worker thread that calls WBN and writes the
 * proposed teams back into the sim's BalanceProposal. */
typedef struct {
    uint8_t teamSize;
    bool    includeBots;
} CmdBalanceRequest;

/* CMD_BALANCE_APPLY — host-only: accept the pending BalanceProposal
 * and apply the team assignments. */
typedef struct {
    uint8_t _unused;
} CmdBalanceApply;

/* CMD_BALANCE_DISMISS — host-only: discard the pending BalanceProposal. */
typedef struct {
    uint8_t _unused;
} CmdBalanceDismiss;

/* CMD_WBN_REAUTH — sender re-presents their WBN join token to
 * upgrade their connected-player flags after a Steam linkage or
 * supporter-tier change. Token is the same fixed-size wire envelope
 * the join handshake uses (see WBN_JOIN_KEY_WIRE_LEN in
 * internal/transport_udp.h — pinned to 65 here so this public header
 * does not reach into an internal one). */
typedef struct {
    char token[65];
} CmdWbnReauth;

/* CMD_RATING_POSTED — the sender's rating or comment on the finished
 * round's WinBolo.net page has just been accepted. key names the round
 * (the summary's wbnLogKey); the server passes it through untouched so
 * the receiving clients can tell whether the nudge is for the round
 * their own recap is showing. */
typedef struct {
    char key[ROUND_STATS_LOGKEY_LEN];
} CmdRatingPosted;
/* CMD_PLAYER_MUTE — mute or unmute one player for the sending client
 * only. The server stops forwarding that player's voice and chat to the
 * sender. Session-scoped: nothing is persisted, and the mask is cleared
 * when the sender's slot is released. */
typedef struct {
    uint8_t targetPlayer;
    uint8_t muted;        /* 0 = unmute, non-zero = mute */
} CmdPlayerMute;

/* CMD_VOICE_STATE — the sender's own mic status. Sent when it changes,
 * not per tick. Self-reported and untrusted, like the client hint bits:
 * a client lying about its own mic costs nothing. */
typedef struct {
    uint8_t hasMic;      /* 0/1 — voice enabled and an input device opened */
    uint8_t selfMuted;   /* 0/1 — has a mic but is not transmitting */
} CmdVoiceState;

/* CmdViewState.kind — which kind of thing the sender is looking
 * through. Values are on the wire, so they are fixed. */
typedef enum {
    VIEW_KIND_TANK = 0,
    VIEW_KIND_PILL = 1,
    VIEW_KIND_BASE = 2,
    VIEW_KIND_ALLY = 3
} ViewStateKind;

/* CMD_VIEW_CYCLE.direction — which way the request steps through the
 * watchable items. Values are on the wire, so they are fixed. */
typedef enum {
    VIEW_CYCLE_NEXT  = 0,
    VIEW_CYCLE_PREV  = 1,
    VIEW_CYCLE_LEFT  = 2,
    VIEW_CYCLE_RIGHT = 3,
    VIEW_CYCLE_UP    = 4,
    VIEW_CYCLE_DOWN  = 5
} ViewCycleDirection;

/* CmdViewCycle.from when the sender is not watching anything yet. */
#define VIEW_CYCLE_FROM_NONE 0xFF

/* CMD_VIEW_STATE — which view the sender's client is in. kind:
 * 0=tank, 1=pill, 2=base, 3=ally; target is the item index (pill/
 * base) or player number (ally), ignored for tank. The server
 * stores the claim and grants at most the rect the view policies
 * allow — it never trusts the claim itself. */
typedef struct {
    uint8_t kind;
    uint8_t target;
} CmdViewState;

/* CMD_VIEW_CYCLE — ask the server for the next thing to watch. kind
 * says which sort of item the sender wants (VIEW_KIND_*); direction is
 * a ViewCycleDirection. from is what the sender is stepping away from
 * (VIEW_CYCLE_FROM_NONE when it is watching nothing yet); the server
 * copies it back in its answer, so a late reply to an earlier press can
 * be told apart from the reply to the current one. */
typedef struct {
    uint8_t kind;
    uint8_t direction;
    uint8_t from;
} CmdViewCycle;

/* CMD_PING — place a smart ping on the map for the sender's team. kind is
 * a PING_KIND_* (input_packet.h); worldX/worldY are WORLD units (256 per map
 * tile), so the marker sits where the cursor was, not on a tile centre. The
 * server checks the game is running, the sender still occupies a player slot
 * (dead is fine — a player waiting to respawn may still ping; only an empty
 * slot is refused), the kind is known and the point is on the map, then
 * rate-limits before turning it into an EVENT_PING. */
typedef struct {
    uint8_t  kind;
    uint16_t worldX;
    uint16_t worldY;
} CmdPing;

/* ClientCommand — variant tag + payload that travels client→server.
 *
 * cmdSeq: per-command sequence number assigned by the client; used
 * for reject correlation and inbound dedupe on the reliable bus.
 * Zero from callers that do not yet maintain a counter. */
typedef struct ClientCommand {
    ClientCommandType type;
    uint32_t cmdSeq;
    union {
        CmdTeamSet           teamSet;
        CmdReady             ready;
        CmdLobbyBotConfig    lobbyBotConfig;
        CmdLobbyTeamMeta     lobbyTeamMeta;
        CmdLobbyTeamClear    lobbyTeamClear;
        CmdLobbySetting      lobbySetting;
        CmdChat              chat;
        CmdAllianceRequest   allianceRequest;
        CmdAllianceAccept    allianceAccept;
        CmdAllianceLeave     allianceLeave;
        CmdGameVoteToggle    gameVoteToggle;
        CmdLobbyRemoveBot    lobbyRemoveBot;
        CmdLobbySetBotBrain  lobbySetBotBrain;
        CmdLobbyOpenHost     lobbyOpenHost;
        CmdMapSkipVote       mapSkipVote;
        CmdNameChange          nameChange;
        CmdLockToggle          lockToggle;
        CmdLobbyAddBot         lobbyAddBot;
        CmdLobbySetMap         lobbySetMap;
        CmdLobbyPreviewCancel  lobbyPreviewCancel;
        CmdLobbyPreviewCommit  lobbyPreviewCommit;
        CmdLobbyPreviewRandom  lobbyPreviewRandom;
        CmdLobbyKick           lobbyKick;
        CmdLobbyTransferHost   lobbyTransferHost;
        CmdLobbySetPassword    lobbySetPassword;
        CmdBalanceRequest      balanceRequest;
        CmdBalanceApply        balanceApply;
        CmdBalanceDismiss      balanceDismiss;
        CmdWbnReauth           wbnReauth;
        CmdLobbyClaimStart     lobbyClaimStart;
        CmdRatingPosted        ratingPosted;
        CmdViewState           viewState;
        CmdViewCycle           viewCycle;
        CmdPlayerMute          playerMute;
        CmdVoiceState          voiceState;
        CmdPing                ping;
    } u;
} ClientCommand;

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_COMMAND_H */
