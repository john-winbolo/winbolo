/*
 * $Id$
 *
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
*Name:          Network Packet Types
*Filename:      netpacks.h
*Author:        John Morrison
*Creation Date: 30/10/99
*Last Modified: 20/09/02
*Purpose:
*  Defines all the different network packet types
*********************************************************/

#ifndef _NETPACKS_H
#define _NETPACKS_H

#include "global.h"
#include "platform_net.h"  /* struct in_addr */
#include "wire_limits.h"   /* PACKET_MAX_CHAT_MESSAGE */
#include "view_policy.h"   /* ViewPolicy — INFO_PACKET.view_policies helpers */
#include "server_voice_mode.h"  /* ServerVoiceMode — INFO_PACKET.flags voice bits */
#include "playername_validate.h"  /* playerNameValidate / playerNameCompare */

#define MAX_UDPPACKET_SIZE 1024
#define MAX_TCPPACKET_SIZE 1024

/* Version bytes derived from WINBOLO_VERSION via CMake.
 * Encoding: each digit of the version string (skipping dots) becomes one byte.
 * E.g. "1.19" -> MAJOR=0x01, MINOR=0x01, REVISION=0x09.
 * BOLO_VERSION_MAJOR/MINOR/REVISION are defined as compile definitions by CMake. */
#define BOLO_VERSION_MAJORPOS    4
#define BOLO_VERSION_MINORPOS    5
#define BOLO_VERSION_REVISIONPOS 6

/* Bolo header — first 8 bytes of every legacy packet */
#ifndef _BOLOHEADER_DEFINED
#define _BOLOHEADER_DEFINED
typedef struct {
  BYTE signature[4];    /* 'Bolo'                */
  BYTE versionMajor;    /* BOLO_VERSION_MAJOR    */
  BYTE versionMinor;    /* BOLO_VERSION_MINOR    */
  BYTE versionRevision; /* BOLO_VERSION_REVISION */
  BYTE type;            /* Packet Type           */
} BOLOHEADER;
BOLO_STATIC_ASSERT(sizeof(BOLOHEADER) == 8, BOLOHEADER_must_be_8_bytes);
#endif

/* Game ID — uniquely identifies a game session */
#ifndef _GAMEID_DEFINED
#define _GAMEID_DEFINED
#pragma pack(push, 1)
typedef struct BOLO_PACK_ATTR {
  struct in_addr serveraddress;
  unsigned short serverport;
  uint16_t _padding;
  uint32_t start_time;
} GAMEID;
#pragma pack(pop)
#endif

/* Info packet response — 76-byte wire format for game browser */
#ifndef _INFO_PACKET_DEFINED
#define _INFO_PACKET_DEFINED
#pragma pack(push, 1)
typedef struct BOLO_PACK_ATTR {
  BOLOHEADER h;
  char mapname[MAP_STR_SIZE];
  GAMEID gameid;
  BYTE gametype;
  BYTE allow_mines;
  BYTE allow_AI;
  BYTE flags;             /* INFO_FLAG_* bits (was spare1)                */
  int32_t start_delay;
  int32_t time_limit;
  WORD num_players;
  WORD free_pills;
  WORD free_bases;
  BYTE has_password;
  BYTE spectator_count;   /* spectators present (0 for now — future work) */
  BYTE num_humans;        /* human players among num_players              */
  BYTE num_bots;          /* AI bots among num_players                    */
  BYTE max_players;       /* server's join-slot cap (MAX_TANKS when unset) */
  char map_md5[32];       /* 32 lowercase hex chars, no NUL; zero-filled  */
                          /* when the map is random/unknown               */
  BYTE view_policies;     /* 2 bits per ViewCategory: pill = bits 0-1,    */
                          /* base = bits 2-3, ally = bits 4-5; bit 6 is   */
                          /* classic mode, bit 7 is allies in trees       */
  BYTE view_policies2;    /* overviewWindow = bits 0-1, lineOfSight =     */
                          /* bits 2-3; bits 4-7 spare                     */
} INFO_PACKET;
#pragma pack(pop)
BOLO_STATIC_ASSERT(sizeof(INFO_PACKET) == 113, INFO_PACKET_must_be_113_bytes);

/* Historical INFO_PACKET wire size, before the flags/count/md5 fields were
 * appended. Servers older than those additions send this; discovery accepts
 * it and parses only the common prefix. */
#define INFO_PACKET_LEGACY_SIZE 76

/* INFO_PACKET wire size before view_policies was appended. Discovery
 * accepts this length and reports the built-in view defaults for it. */
#define INFO_PACKET_PRE_VIEWS_SIZE 111

/* INFO_PACKET wire size before view_policies2 was appended. Discovery
 * accepts this length, reads view_policies from it, and reports the
 * built-in overview defaults for the byte it does not have. */
#define INFO_PACKET_PRE_VIEWS2_SIZE 112

/* Pack the three per-category policies, the classic-mode flag and the
 * allies-in-trees flag into INFO_PACKET.view_policies. Classic mode
 * rides bit 6 and allies in trees bit 7, beside the ally visibility
 * they sit alongside. */
static inline BYTE infoPacketPackViewPolicies(ViewPolicy pill,
                                              ViewPolicy base,
                                              ViewPolicy ally,
                                              bool classic,
                                              bool alliesInTrees) {
  return (BYTE)(((unsigned)pill & 0x3u)
              | (((unsigned)base & 0x3u) << 2)
              | (((unsigned)ally & 0x3u) << 4)
              | (classic ? 0x40u : 0u)
              | (alliesInTrees ? 0x80u : 0u));
}

/* Read the three policies, the classic-mode flag and the allies-in-trees
 * flag back out of a received INFO_PACKET. INFO_PACKET_PRE_VIEWS2_SIZE
 * is the shortest length that carries the byte, so that is what this
 * needs rather than the full layout — a packet that stops before
 * view_policies2 still reports everything it does carry. Anything
 * shorter predates view_policies, so it reports the built-in defaults
 * (pill always, base off, ally always, classic mode off, allies in
 * trees off) instead of whatever the short read left in the struct. */
static inline void infoPacketReadViewPolicies(const INFO_PACKET *info,
                                              size_t len,
                                              ViewPolicy *pill,
                                              ViewPolicy *base,
                                              ViewPolicy *ally,
                                              bool *classic,
                                              bool *alliesInTrees) {
  if (info == NULL || len < (size_t)INFO_PACKET_PRE_VIEWS2_SIZE) {
    if (pill) *pill = viewPolicyAlways;
    if (base) *base = viewPolicyOff;
    if (ally) *ally = viewPolicyAlways;
    if (classic) *classic = false;
    if (alliesInTrees) *alliesInTrees = false;
    return;
  }
  if (pill) *pill = (ViewPolicy)(info->view_policies & 0x3u);
  if (base) *base = (ViewPolicy)((info->view_policies >> 2) & 0x3u);
  if (ally) *ally = (ViewPolicy)((info->view_policies >> 4) & 0x3u);
  if (classic) *classic = (info->view_policies & 0x40u) != 0;
  if (alliesInTrees) *alliesInTrees = (info->view_policies & 0x80u) != 0;
}

/* Pack the overview window and the line-of-sight mode into
 * INFO_PACKET.view_policies2. Two bits each — the window at bits 0-1,
 * line of sight at bits 2-3 — so bits 4-7 stay clear for whatever needs
 * them next. Both are masked, so a value from a newer sender cannot
 * reach the spare bits. */
static inline BYTE infoPacketPackViewPolicies2(uint8_t overviewWindow,
                                               uint8_t lineOfSight) {
  return (BYTE)(((unsigned)overviewWindow & 0x3u)
              | (((unsigned)lineOfSight & 0x3u) << 2));
}

/* Read the overview window and the line-of-sight mode back out of a
 * received INFO_PACKET. A packet shorter than the full layout predates
 * the byte, so it reports the built-in defaults — the expanded window
 * with nothing blocking sight inside it. Two bits can also hold a value
 * neither enum names; that reports the default as well, so a browser row
 * never shows a mode this build cannot name. */
static inline void infoPacketReadViewPolicies2(const INFO_PACKET *info,
                                               size_t len,
                                               uint8_t *overviewWindow,
                                               uint8_t *lineOfSight) {
  unsigned window = (unsigned)overviewWindowExpanded;
  unsigned sight  = (unsigned)lineOfSightOff;
  if (info != NULL && len >= sizeof(INFO_PACKET)) {
    unsigned w = (unsigned)info->view_policies2 & 0x3u;
    unsigned s = ((unsigned)info->view_policies2 >> 2) & 0x3u;
    if (w < (unsigned)OVERVIEW_WINDOW_COUNT) window = w;
    if (s < (unsigned)LINE_OF_SIGHT_COUNT) sight = s;
  }
  if (overviewWindow) *overviewWindow = (uint8_t)window;
  if (lineOfSight) *lineOfSight = (uint8_t)sight;
}
#endif

/* INFO_PACKET.flags bit values (the byte that was spare1). */
#define INFO_FLAG_ALLOW_NEW_PLAYERS 0x01u
#define INFO_FLAG_LOCKED            0x02u
#define INFO_FLAG_RANKED            0x04u
#define INFO_FLAG_RANDOM_MAP        0x08u
#define INFO_FLAG_ALLOW_SPECTATORS  0x10u
#define INFO_FLAG_IN_LOBBY          0x20u

/* The server's voice mode rides the top two bits of the same byte, which
 * were the only two left. serverVoiceOn is 0, so a server built before
 * this encoding existed sends both bits clear and reads back as on —
 * which is what those servers do. The two bits can hold a fourth value
 * the enum does not use; it reads as on for the same reason. */
#define INFO_FLAG_VOICE_MASK        0xC0u
#define INFO_FLAG_VOICE_SHIFT       6

/* Pack a voice mode into the bits to OR into INFO_PACKET.flags. Only
 * bits 6-7 are returned, so an out-of-range mode cannot reach the other
 * INFO_FLAG_* bits sharing the byte. */
static inline BYTE infoPacketPackVoiceMode(ServerVoiceMode mode) {
  return (BYTE)(((unsigned)mode << INFO_FLAG_VOICE_SHIFT) & INFO_FLAG_VOICE_MASK);
}

/* Read the voice mode back out of a received flags byte. The reserved
 * fourth value reports serverVoiceOn rather than a mode that does not
 * exist, so an encoding this build does not recognise reads as the mode
 * every server ran before the field existed. */
static inline ServerVoiceMode infoPacketReadVoiceMode(BYTE flags) {
  unsigned mode = ((unsigned)flags & INFO_FLAG_VOICE_MASK) >> INFO_FLAG_VOICE_SHIFT;
  switch (mode) {
    case (unsigned)serverVoiceOff:       return serverVoiceOff;
    case (unsigned)serverVoiceProximity: return serverVoiceProximity;
    default:                             return serverVoiceOn;
  }
}

/* Packet types */
/* Info Packet */
#define BOLOPACKET_INFOREQUEST   13
#define BOLOPACKET_INFORESPONSE  14
/* Ping packet */
#define BOLOPACKET_PINGREQUEST 15
#define BOLOPACKET_PINGRESPONSE 16
/* Password Packet */
#define BOLOPACKET_PASSWORDCHECK 17  /* Check password  */
#define BOLOPACKET_PASSWORDACCEPT 18 /* Password OK     */
#define BOLOPACKET_PASSWORDFAIL 19   /* Password failed */
/* Check for player name availability */
#define BOLOPACKET_NAMECHECK 20       /* Check name        */
#define BOLOPACKET_NAMEACCEPT 21      /* Name check OK     */
#define BOLOPACKET_NAMEFAIL 22        /* Name check failed */
/* Player Data */
#define BOLOPACKET_PLAYERDATAREQUEST 23  /* Check name      */
#define BOLOPACKET_PLAYERDATARESPONSE 24 /* Name check OK   */
/* Player number */
#define BOLOPACKET_PLAYERNUMREQUEST 25   /* Check name      */
#define BOLOPACKET_PLAYERNUMRESPONSE 26  /* Name check OK   */
/* New player packet info */
#define BOLOPACKET_PLAYERNEWPLAYER 27    /* New Player data */
/* Base Data */
#define BOLOPACKET_BASESDATAREQUEST 29
#define BOLOPACKET_BASESDATARESPONSE 30
/* Starts Data */
#define BOLOPACKET_STARTSDATAREQUEST 31
#define BOLOPACKET_STARTSDATARESPONSE 32
/* Pillbox Data */
#define BOLOPACKET_PILLSDATAREQUEST 33
#define BOLOPACKET_PILLSDATARESPONSE 34
/* Map Download data */
#define BOLOPACKET_MAPDATAREQUEST 35
#define BOLOPACKET_MAPDATARESPONSE 36
/* Message Packet to a single player packet */
#define BOLOPACKET_MESSAGE 37
/* Change player name Packet */
#define BOLOCHANGENAME_DATA 38
/* Packet OK/Fail */
#define BOLOPACKET_VALID 39
#define BOLOPACKET_INVALID 40
#define BOLOPACKET_PLAYERLEAVE 41
/* Game Time request/Response */
#define BOLOPACKET_TIMEREQUEST 42
#define BOLOPACKET_TIMERESPONSE 43
/* Message to all players */
#define BOLOPACKET_MESSAGE_ALL_PLAYERS 44

/* Data Packet */
#define BOLOPACKET_TOKEN 50
#define BOLOPACKET_DATA 28
/* Player Position/LGM/Shells etc */
#define BOLOPOSITION_DATA 29
/* Client Data - Shells etc */
#define BOLOCLIENT_DATA 45

/* Alliance packets */
#define BOLOLEAVEALLIANCE_DATA 46
#define BOLOREQUESTALLIANCE_DATA 47
#define BOLOACCEPTALLIANCE_DATA 48

#define BOLOALLOWNEWPLAYERS 51
#define BOLONOALLOWNEWPLAYERS 52
#define BOLOREJOINREQUEST 53

#define BOLOLGMRETURN 54

#define BOLOREQUEST_STARTPOS 55
#define BOLORESPONSE_STARTPOS 56

#define BOLOPACKET_SERVERKEYREQUEST 57
#define BOLOPACKET_SERVERKEYRESPONSE 58

#define BOLOPACKET_CLIENTKEY 59

#define BOLOPACKET_PACKETREREQUEST 60
#define BOLOPACKET_PACKETQUIT 61



/* LGM Out working */
#define BOLOPACKET_LGM_OUTWORKING 62

/* Game is Locked */
#define BOLOPACKET_GAMELOCKED 63

/* Max Players reached */
#define BOLOPACKET_MAXPLAYERS 64

/* All your missing packets */
#define BOLOPACKET_RETRANSMITTED_PACKETS 65

/* Server message packet */
#define BOLOSERVERMESSAGE 49

/* RSA message packet */
#define BOLOPACKET_RSACHECK 66
#define BOLOPACKET_RSARESPONSE 67
#define BOLOPACKET_RSAACCEPT 68
#define BOLOPACKET_RSAFAIL 69

/* ---- New input-based protocol (Phase 6) ---- */

/* Client -> Server */
#define PACKET_INPUT           101   /* InputPacket (with redundancy) */
#define PACKET_JOIN_REQUEST    102   /* Player name + version */
#define PACKET_CHAT_MESSAGE    103   /* Text message */
#define PACKET_PING            104   /* Ping request */

/* Server -> Client */
#define PACKET_STATE_SNAPSHOT  110   /* Tank positions + events */
#define PACKET_JOIN_ACCEPT     111   /* Player number + map data */
/* PACKET_JOIN_REJECT — server tells joiner why join failed.
 * Wire format (Phase 9d, unversioned/lockstep):
 *   [header 8] [langid 2 BE] [argCount 1] [per arg: lenByte (0..PLAYER_NAME_LEN-1) + bytes]
 * argCount is 0..4. Arg slots map to MessageArgs in order:
 *   #1 -> playerName, #2 -> otherName, #3 -> string1, #4 -> string2.
 * Client renders via langGetTextFmt(langid, &args). */
#define PACKET_JOIN_REJECT     112
#define PACKET_PLAYER_JOINED   113   /* New player info */
#define PACKET_PLAYER_LEFT     114   /* Player disconnected */
/* PACKET_CHAT_BROADCAST — server fan-out for chat AND server-source
 * messages.  Wire format depends on the first byte after the header
 * (fromPlayer):
 *   fromPlayer < MAX_TANKS  : player chat — unchanged
 *     [header 8] [fromPlayer 1] [destPlayer 1] [message N]
 *   fromPlayer == 0xFF      : server-source localized (Phase 9d)
 *     [header 8] [0xFF] [destPlayer=0xFF] [langid 2 BE] [argCount 1] [args...]
 *     Same arg encoding as PACKET_JOIN_REJECT above.
 *   fromPlayer == 0xFE      : server-source raw English (transitional)
 *     [header 8] [0xFE] [destPlayer=0xFF] [message N]
 *     Used by un-localized server-ops broadcasts (admin "say", lock
 *     toggle, ping enforcement). Phase 9d only localized the join
 *     reject + kick/rename announce paths; remaining ops messages stay
 *     English on the wire and are migrated to 0xFF as new langids land. */
#define PACKET_CHAT_BROADCAST  115
#define PACKET_FULL_STATE      116   /* Periodic full reconciliation */
#define PACKET_MAP_DELTA       117   /* Terrain changes */
#define PACKET_BASE_STATE      118   /* Base ownership/stock change */
#define PACKET_PILL_STATE      119   /* Pill health/ownership change */
#define PACKET_PONG            120   /* Ping response */
#define PACKET_GAME_EVENT      121   /* Explosion, mine hit, etc. */
/* 122 (MAP_DOWNLOAD) and 105 (MAP_ACK) retired: the compressed map now streams
 * on CHANNEL_BULK behind a bulk-transfer stream header (join download +
 * resync), so there is no per-chunk carrier or ack packet. */
#define PACKET_MAP_RESYNC_REQUEST 158 /* client -> server: re-send the live map
                                       * after the client detects its terrain has
                                       * diverged (a dropped EVENT_MAP_CHANGE).
                                       * Body: [resyncGen u32] — a client-chosen
                                       * nonzero id carried in the resync stream
                                       * header so a superseded resync is dropped
                                       * by the client's gen gate. */
#define PACKET_QUIT            106   /* Graceful disconnect (client -> server) */
/* 107 retired (PLAYER_LIST): the roster is maintained by reliable
 * CTRL_PLAYER_JOIN/CTRL_PLAYER_LEFT events on CHANNEL_CONTROL. */
#define PACKET_NAME_CHANGE     123   /* Player name change (bidirectional) */

/* Alliance packets (new protocol) */
#define PACKET_ALLIANCE_REQUEST  124  /* client -> server: request alliance */
#define PACKET_ALLIANCE_ACCEPT   125  /* client -> server: accept alliance */
#define PACKET_ALLIANCE_LEAVE    126  /* client -> server: leave alliance */
#define PACKET_ALLIANCE_UPDATE   127  /* server -> clients: alliance state broadcast */
#define PACKET_LOCK_TOGGLE       128  /* client -> server: toggle allow new players */
#define PACKET_SERVER_SHUTDOWN   129  /* server -> clients: server is shutting down */

/* Lobby packets — Client -> Server */
#define PACKET_LOBBY_TEAM_SET    130  /* { playerNum, teamNumber } */
#define PACKET_LOBBY_READY       131  /* { playerNum, ready } */
#define PACKET_LOBBY_ADD_BOT     132  /* { brainPath } — request server add a bot */
#define PACKET_LOBBY_REMOVE_BOT  133  /* { playerNum } — request server remove a bot */

/* Lobby packets — Server -> Client.  Slot 140 (formerly the composite
 * PACKET_LOBBY_STATE) is retired: slot and settings updates are now
 * encoded as separate packets via the control-event codec. */
#define PACKET_LOBBY_UPDATE      141  /* Single-player delta: { playerNum, teamNumber, ready, isBot } */
#define PACKET_COUNTDOWN         142  /* { secondsRemaining } */
#define PACKET_GAME_START        143  /* Signal to transition from lobby to game */
#define PACKET_GAME_OVER         144  /* Signal game ended, return to lobby */
#define PACKET_LOBBY_MAP_CHANGE  145  /* Server changed map, clients must re-download */
#define PACKET_WBN_REAUTH       146  /* Client -> Server: re-authenticate WBN token after lobby reset */
#define PACKET_WBN_REKEY        160  /* Server -> Client: rotate WBN session key (Phase 7/8 encodes/decodes; see plans/fixwbn.md "Lobby key rotation") */

/* Team balance packets */
#define PACKET_BALANCE_REQUEST   147  /* Client(host) -> Server: request WBN balance
                                       * body: [teamSize 1] [includeBots 1]
                                       * includeBots: 1 = bots take part (WBN
                                       * marks them non-WBN players and
                                       * places them into teams); 0 = bots
                                       * are removed from the lobby before
                                       * the proposal is applied. */
#define PACKET_BALANCE_PROPOSAL  148  /* Server -> Clients: proposed team assignments */
#define PACKET_BALANCE_APPLY     149  /* Client(host) -> Server: confirm and apply proposal */
#define PACKET_BALANCE_DISMISS   150  /* Client(host) -> Server: dismiss proposal */
#define PACKET_MAP_SKIP_VOTE    151  /* Client -> Server: toggle skip vote */
#define PACKET_MAP_SKIP_STATE   152  /* Server -> Clients: current vote tally */

/* Phase 3 — UDP hole-punching coordination via tracker. Numbered to
 * match tracker/udp.h (153-157 range chosen to avoid the existing
 * 130-133 lobby packet collision). Constants 153-155 are reserved
 * here for cross-codebase numbering but NOT implemented in this
 * commit — only the probe round-trip (156/157) ships now. */
#define PACKET_PUNCH_REQUEST        153   /* joiner → tracker */
#define PACKET_PUNCH_NOTIFY         154   /* tracker → host */
#define PACKET_PUNCH_REQUEST_ACK    155   /* tracker → joiner */
#define PACKET_PUNCH_PROBE_REQUEST  156   /* host → tracker, this commit */
#define PACKET_PUNCH_PROBE_REPLY    157   /* tracker → host, this commit */

/* Server -> Client: lobby-wide settings (map, game type, limits, ...).
 * Produced by the CTRL_LOBBY_SETTINGS codec encoder; replaces the
 * settings-tail portion of the legacy composite PACKET_LOBBY_STATE.
 * Layout matches the per-field shape of serverSimFillLobbySettingsEvent. */
#define PACKET_LOBBY_SETTINGS       159

/* ── Lobby Layout A — Client → Server (160-174) ─────────────────── */
#define PACKET_LOBBY_SET_SETTING    199  /* { settingType 1, valueLen 1, value N } */
#define PACKET_LOBBY_OPEN_HOST      161  /* { bool 1 } */
#define PACKET_LOBBY_TEAM_META      162  /* { teamId 1, color 1, namingPool 1,
                                          *   startSide 1, nameLen 1, name N } */
#define PACKET_LOBBY_TEAM_CLEAR     163  /* { teamId 1 } */
#define PACKET_LOBBY_BOT_CONFIG     164  /* { slot 1, difficulty 1,
                                          *   personality 1, nameLen 1,
                                          *   name N } */
#define PACKET_LOBBY_KICK           165  /* { slot 1 } */
#define PACKET_KICKED               198  /* server -> kicked client: immediate
                                          *   disconnect notification with
                                          *   "you were kicked" semantics */
#define PACKET_LOBBY_SET_BOT_BRAIN  166  /* { slot 1, pathLen 1, path N } */
#define PACKET_LOBBY_SET_MAP        167  /* { pathLen 1, path N } */
#define PACKET_LOBBY_MAP_LIST_REQ   168  /* { pathLen 1, path N } */
#define PACKET_LOBBY_MAP_UPLOAD_BEGIN  169  /* { totalLen 4, nameLen 1, name N }
                                              * the map bytes then stream over
                                              * CHANNEL_BULK behind a bulk-
                                              * transfer stream header; 170 (the
                                              * old CHUNK carrier) is retired. */
#define PACKET_LOBBY_MAP_USE_LOCAL     196  /* client -> server: "I want to
                                              * install this map; if you
                                              * already have a file with
                                              * the same MD5 at this rel
                                              * path under data/maps/,
                                              * use it instead and skip
                                              * the upload."
                                              * { totalLen 4, nameLen 1,
                                              *   name N, relPathLen 1,
                                              *   relPath M, md5 16 } */
#define PACKET_LOBBY_MAP_USE_LOCAL_NACK 197  /* server -> client: "I do
                                              * not have a matching file
                                              * — please upload."
                                              * { nameLen 1, name N } */
#define PACKET_LOBBY_MAP_SEARCH_REQ    171  /* { pathLen 1, path N, queryLen 1, query M } */
#define PACKET_LOBBY_PREVIEW_CANCEL    172  /* (no payload) */
#define PACKET_LOBBY_PREVIEW_COMMIT    173  /* (no payload) */
#define PACKET_LOBBY_PREVIEW_RANDOM    174  /* { seedLen 1, seed N } */

/* ── Lobby Layout A — Server → Client (175-186) ─────────────────── */
#define PACKET_LOBBY_SETTING_CHG    175  /* echo of CLIENT SET_SETTING */
#define PACKET_LOBBY_OPEN_HOST_CHG  176  /* { bool 1 } */
#define PACKET_LOBBY_TEAM_META_CHG  177  /* same payload as TEAM_META */
#define PACKET_LOBBY_BOT_CONFIG_CHG 178  /* same payload as BOT_CONFIG */
#define PACKET_LOBBY_TRANSFER_HOST  179  /* { slot 1 } client(host) -> server */
#define PACKET_LOBBY_BRAIN_LIST     181  /* { count 1, for each: nameLen 1, name,
                                          *   verLen 1, ver, pathLen 1, path } */
#define PACKET_LOBBY_BOT_BRAIN_CHG  182  /* { slot 1, pathLen 1, path N } */
#define PACKET_LOBBY_MAP_LIST_RSP   183  /* server reply to MAP_LIST_REQ */
#define PACKET_LOBBY_MAP_UPLOAD_ACK 184  /* { status 1 } */
#define PACKET_LOBBY_MAP_UPLOAD_DONE 185 /* { status 1, pathLen 1, path N } */
#define PACKET_LOBBY_MAP_SEARCH_RSP 186  /* server reply to MAP_SEARCH_REQ */
#define PACKET_LOBBY_SYNC_COMPLETE  180  /* server -> joiner: final event of the
                                          * join sync replay; marks the roster
                                          * burst done so live lobby sounds can
                                          * resume.  No payload. */

/* 187/188 retired: control events ride CHANNEL_CONTROL, acked by the
 * channel-frame trailer.  The former standalone carrier and its ACK are gone. */

/* Server Maps preview-fetch protocol. The client never reads
 * server map files directly: in MP the file lives on a remote
 * host, so the server sources the bytes and streams them back.
 * The client then rasterises locally via its existing
 * minimapRenderPixels path, so neither side pays a hard dep on
 * an image encoder. */
#define PACKET_LOBBY_MAP_PREVIEW_REQ   189  /* client → server
                                              { pathLen 1, path N }
                                              path is relative to
                                              data/maps/ (e.g.
                                              "Uploads/Foo.map").
                                              The map bytes stream back
                                              over CHANNEL_BULK behind a
                                              bulk-transfer stream header;
                                              190/191 (the old BEGIN/CHUNK
                                              carriers) are retired. */
#define PACKET_LOBBY_MAP_PREVIEW_ERR   192  /* server → client
                                              { pathLen 1, path N,
                                                err 1 } 1=not-found
                                              2=too-large 3=internal */

#define PACKET_LOBBY_SET_PASSWORD      193  /* client(host) → server
                                              { pwLen 1, pw N }
                                              pwLen 0 clears the
                                              password. Server stores
                                              the new value, updates
                                              its hasPassword flag,
                                              and rebroadcasts the
                                              lobby state so all
                                              clients see the
                                              password lock indicator
                                              flip. Password text is
                                              never echoed to other
                                              clients. */

/* In-game vote system (back-to-lobby + surrender). See docs/voting_plan.md.
 * Vote-kind values match GAME_VOTE_KIND_*. */
#define PACKET_GAME_VOTE_TOGGLE        194  /* client → server
                                              { kind 1, on 1 }
                                              on: 0=no, 1=yes,
                                                  2=open-widget-only
                                              (open-only is used when
                                              re-pressing the menu
                                              while a vote is already
                                              running — leaves the
                                              caller's existing vote
                                              alone). For surrender,
                                              the server infers the
                                              voter's team from their
                                              own slot. */
#define PACKET_COMMAND_REJECTED        200  /* server → client (unicast)
                                              { origCmdSeq u32, origCmdType u8,
                                                reasonCode u8, origSlot u8 } */
#define PACKET_COMMAND_TICK            201  /* client → server
                                              { count u8, for each:
                                                entryLen u16,
                                                codecPacket entryLen bytes
                                                  (per commandCodecEncode) } */
#define PACKET_COMMAND_ACK             202  /* server → client (unicast)
                                              { highestProcessedCmdSeq u32 } */
#define PACKET_BALANCE_FAILED          203  /* server → host (unicast)
                                              { reasonCode u8 } — fired when
                                              the balance worker finishes
                                              without a usable proposal */
#define PACKET_SHELL_DEATH             204  /* server → shell owner (unicast)
                                              { fireTick u32, impactWX u16,
                                                impactWY u16, owner u8,
                                                outcome u8 } — the firing
                                              client culls its predicted shell
                                              and draws the impact */
#define PACKET_LOBBY_CLAIM_START       205  /* client → server
                                              { targetSlot, startIdx } */

#define PACKET_JOIN_CHALLENGE          206  /* server → joiner: a retry cookie
                                              proving the joiner's source
                                              address before any slot is
                                              allocated (anti-spoof) */
#define JOIN_COOKIE_LEN                16   /* HMAC-MD5 digest carried in the
                                              JOIN_REQUEST tail and the
                                              JOIN_CHALLENGE body */

#define PACKET_CHANNEL                 207  /* bidirectional: one channel frame
                                              (the format defined in
                                              channel_mux.c) directly after
                                              PACKET_HEADER_SIZE; no other
                                              fields. Sent standalone when no
                                              snapshot / input rides this tick;
                                              otherwise the same frame is
                                              appended as a trailer on the
                                              snapshot / input datagram. */

#define PACKET_LOBBY_BOT_POOL_CHUNK    208  /* server → joiner: one fragment of
                                              the compressed bot-pool catalog.
                                              { seq 1, count 1, fragLen 2 BE,
                                                frag N }. Reassembled seq
                                                0..count-1 then installed. */

#define PACKET_GAME_VOTE_STATE         195  /* server → all clients
                                              { kind 1, active 1,
                                                triggerSrc 1, teamId 1,
                                                threshold 1, yesCount 1,
                                                secondsRemaining 1,
                                                votes 2 (uint16 BE
                                                bitmask of slot
                                                votes) }
                                              broadcast on every
                                              state change AND once
                                              per second while a vote
                                              is running (drives the
                                              countdown). */

#define PACKET_ROUND_STATS             209  /* server → all: end-of-round
                                              scoreboard + awards */

#define PACKET_ROUND_LOG_REQ           210  /* client → server
                                              { reqSeq 4 BE } — ask for the
                                              last completed round's .wbv.
                                              The bytes stream back over
                                              CHANNEL_BULK behind a
                                              BULK_KIND_ROUND_LOG stream
                                              header whose gen echoes reqSeq,
                                              so a reply to a superseded
                                              request is droppable. */
#define PACKET_ROUND_LOG_ERR           211  /* server → client
                                              { reqSeq 4 BE, code 1 } — a
                                              refused ROUND_LOG_REQ. Every
                                              refusal is answered, so the
                                              client can tell "no" from a
                                              lost request. */

/* PACKET_ROUND_LOG_ERR codes. BUSY and RATE_LIMITED are transient — the
 * client may ask again — while DISABLED, NONE and TOO_LARGE hold for as
 * long as the round does. */
#define ROUND_LOG_ERR_DISABLED      1  /* server does not serve round logs, or
                                        * is not in a state where it can */
#define ROUND_LOG_ERR_NONE          2  /* no completed round to serve */
#define ROUND_LOG_ERR_BUSY          3  /* concurrent-transfer cap reached */
#define ROUND_LOG_ERR_TOO_LARGE     4  /* round log exceeds ROUND_LOG_MAX_BYTES */
#define ROUND_LOG_ERR_RATE_LIMITED  5  /* this client asked too soon, or too
                                        * many times this round */

#define PACKET_RATING_POSTED           212  /* client → server
                                              { key 32 } — the sender has just
                                              rated or commented on the
                                              finished round's WinBolo.net
                                              page. The server neither reads
                                              nor checks the key; it fans the
                                              event out so the other clients
                                              re-read that page. */

#define PACKET_MAP_DL_READY            213  /* client → server
                                              { connId 8 } — "my join-download
                                              buffers are armed; begin the map
                                              stream". Sent when JOIN_ACCEPT
                                              arms the download, and re-sent by
                                              the client's download watchdog.
                                              A re-ask while a stream is (or
                                              was) in flight is answered with a
                                              full restart behind a
                                              CHANNEL_BULK re-base, so a
                                              transfer whose head the client
                                              missed (accept lost, stream
                                              already flowing) is recoverable.
                                              connId must match the slot's; a
                                              mismatch is dropped (an
                                              address-spoofed READY could
                                              otherwise reset a healthy
                                              client's stream). */
#define PACKET_VIEW_STATE              214  /* client → server
                                              { kind 1, target 1 } — the view
                                              the sender's client is in: 0=tank,
                                              1=pill, 2=base, 3=ally, with the
                                              item index (pill/base) or player
                                              number (ally) in target. The
                                              server stores the claim per slot
                                              and grants at most what the view
                                              policies allow. */

#define PACKET_VIEW_CYCLE              215  /* client → server
                                              { kind 1, direction 1, from 1 } —
                                              "give me the next thing to watch".
                                              kind is the sort of item wanted
                                              (0=tank, 1=pill, 2=base, 3=ally),
                                              direction is a ViewCycleDirection
                                              (next/previous plus the four
                                              scroll directions), and from is
                                              what the sender is watching now
                                              (0xFF when it is watching
                                              nothing). The server picks from
                                              live state and answers with
                                              CTRL_VIEW_TARGET. */

#define PACKET_PLAYER_MUTE             216  /* client → server
                                              { targetPlayer 1, muted 1 }
                                              per-recipient mute: the
                                              server stops forwarding
                                              that player's voice and
                                              chat to the sender. Not
                                              echoed to anyone else. */

#define PACKET_VOICE_STATE             217  /* client → server
                                              { hasMic 1, selfMuted 1 }
                                              the sender's own mic status,
                                              sent when it changes. Lands
                                              in the sender's clientFlags
                                              and rides the snapshot and
                                              lobby slot from there. */

#define PACKET_MAP_PING                218  /* client → server
                                              { kind 1, worldX 2, worldY 2 } —
                                              a smart ping the sender wants
                                              placed on the map for its team.
                                              kind is a PING_KIND_*
                                              (input_packet.h) and the position
                                              is in WORLD units (256 per map
                                              tile), big-endian like every
                                              other u16 on this bus. Named
                                              MAP_PING because PACKET_PING is
                                              already the latency probe. */

#define PACKET_PLAYER_PING_MUTE        220  /* client → server
                                              { targetPlayer 1, muted 1 }
                                              per-recipient smart-ping mute,
                                              independent of PLAYER_MUTE: the
                                              server stops delivering that
                                              player's EVENT_PING to the
                                              sender. Not echoed to anyone
                                              else. */

#ifndef GAME_VOTE_KIND_BACK_TO_LOBBY
#define GAME_VOTE_KIND_BACK_TO_LOBBY  1
#define GAME_VOTE_KIND_SURRENDER      2

#define GAME_VOTE_TOGGLE_NO          0
#define GAME_VOTE_TOGGLE_YES         1
#define GAME_VOTE_TOGGLE_OPEN_ONLY   2

#define GAME_VOTE_ACTIVE_NONE        0
#define GAME_VOTE_ACTIVE_RUNNING     1
#define GAME_VOTE_ACTIVE_PASSED      2
#define GAME_VOTE_ACTIVE_FAILED      3
#define GAME_VOTE_ACTIVE_CANCELLED   4

#define GAME_VOTE_TRIGGER_MANUAL          0
#define GAME_VOTE_TRIGGER_BASE_MONOPOLY   1
#define GAME_VOTE_TRIGGER_POST_SURRENDER  2
#endif

/* Vote timeout in seconds */
#define GAME_VOTE_DEADLINE_SECONDS  30

/* Grace period after a vote becomes unanimous before the effect
 * fires. Voters can change their mind during this window. */
#define GAME_VOTE_PASS_GRACE_SECONDS 5

/* Percentage of eligible YES votes needed for a vote to pass.
 * Compared as `yesCount * 100 >= eligibleCount * PCT_x100`, so
 * fractional percentages like 50.1 are expressed by multiplying
 * by 10 (e.g. 50.1 → 501 with PCT_DENOM=1000). For now we ship
 * exact unanimity. */
#define GAME_VOTE_PASS_PCT_NUM    100
#define GAME_VOTE_PASS_PCT_DENOM  100

/* LobbySettingType (LST_*) lives in public/wire_limits.h so the GUI
 * lobby can reach the enum without including internal/netpacks.h —
 * same pattern as LOBBY_LOCK_* and LOBBY_TIME_MINUTES_*.
 *
 * allowNewPlayers stays on PACKET_LOCK_TOGGLE — not duplicated here.
 * serverLocks is read-only (CLI on bolod) — no SET_SETTING for it. */

/* LST_TIME_MINUTES accepted range: 1..4320 minutes (72 hours).
 * Defended at the wire so downstream ticks arithmetic
 * (minutes * 60 * GAME_NUMGAMETICKS_SEC) can't be coaxed
 * toward int32_t overflow by a malicious client.
 * LOBBY_TIME_MINUTES_MIN/MAX are in public/wire_limits.h so the
 * GUI lobby can pre-validate before sending. */
static inline bool lobbyTimeMinutesIsValid(uint16_t minutes) {
    return minutes >= LOBBY_TIME_MINUTES_MIN &&
           minutes <= LOBBY_TIME_MINUTES_MAX;
}

/* Callback shape matching transportUdpServerGetPlayerName so the
 * ADD_BOT / BOT_CONFIG handlers can pass it through verbatim. May
 * return NULL for slots without a connected player; lobbyBotNameAcceptable
 * skips NULL returns rather than treating them as collisions. */
typedef const char *(*LobbyBotNameLookupFn)(BYTE slot);

/* Validate + uniqueness-check a candidate bot name from the wire.
 *
 * Pure: no globals, no I/O. Callers handle the empty-name branch
 * externally (ADD_BOT falls through to "Bot N" pool naming;
 * BOT_CONFIG treats empty nameLen as "no name change"), so rawName
 * is expected to be non-empty.
 *
 * Step 1: playerNameValidate(rawName -> validatedOut). On failure,
 * writes *validateErrOut (if non-NULL) and returns false.
 *
 * Step 2: uniqueness — for each j in [0, MAX_TANKS) with j != skipSlot,
 * fetch getName(j) and compare via playerNameCompare. A NULL return
 * from getName means "no player in that slot" and is skipped. On a
 * match, writes *collisionSlotOut (if non-NULL) and returns false.
 *
 * skipSlot: pass the bot's own slot from BOT_CONFIG so a no-op or
 * normalization-preserving rename doesn't self-collide. ADD_BOT
 * passes -1 (or any out-of-range index) so every slot participates.
 *
 * Returns true if the name is acceptable. validateErrOut /
 * collisionSlotOut are not touched on the success path; on failure
 * exactly one of them is written (validator failures don't reach
 * the uniqueness loop). */
static inline bool lobbyBotNameAcceptable(
    const char *rawName,
    char *validatedOut, size_t validatedOutSize,
    int skipSlot,
    LobbyBotNameLookupFn getName,
    PlayerNameValidationError *validateErrOut,
    int *collisionSlotOut) {
    PlayerNameValidationError err = PLAYER_NAME_OK;
    if (!playerNameValidate(rawName, validatedOut, validatedOutSize, &err)) {
        if (validateErrOut) *validateErrOut = err;
        return false;
    }
    if (getName != NULL) {
        BYTE j;
        for (j = 0; j < MAX_TANKS; j++) {
            if ((int)j == skipSlot) continue;
            const char *other = getName(j);
            if (other != NULL &&
                playerNameCompare(other, validatedOut) == 0) {
                if (collisionSlotOut) *collisionSlotOut = (int)j;
                return false;
            }
        }
    }
    return true;
}

/* Reject reason codes. Carried in PACKET_LOBBY_MAP_UPLOAD_ACK and
 * in CTRL_COMMAND_REJECTED control events. */
#define LOBBY_REJECT_NOT_HOST          1   /* sender lacks authority */
#define LOBBY_REJECT_LOCKED            2   /* setting is in serverLocks bitmask */
#define LOBBY_REJECT_INVALID           3   /* malformed payload / out-of-range value */
#define LOBBY_REJECT_UPLOAD_BUSY       4   /* another client's map upload is in flight */
#define LOBBY_REJECT_UPLOAD_DISABLED   5   /* host disabled map uploads */
#define LOBBY_REJECT_UPLOAD_LIMIT_HIT  6   /* per-map storage cap reached */
#define LOBBY_REJECT_COOLDOWN          7   /* per-client request cooldown active */

/* Alliance update event types */
#define ALLIANCE_EVENT_REQUEST  0
#define ALLIANCE_EVENT_ACCEPT   1
#define ALLIANCE_EVENT_LEAVE    2
#define ALLIANCE_EVENT_RESET    3   /* full per-player ally bitmap; CTRL_ALLIANCE_RESET */

/* Magic bytes for new protocol packets */
#define BOLO_NEW_MAGIC_0  'W'
#define BOLO_NEW_MAGIC_1  'B'

/* Wire packet header layout (8 bytes):
 *   [magic0 1][magic1 1][packetType 1][reserved 1][sequence 4]
 * Used by every packet type below; client_net.c builds packets
 * directly and transport_local.c decodes them, so the constant
 * is exposed here rather than buried in transport_udp_internal.h. */
#define PACKET_HEADER_SIZE 8

/* PACKET_MAX_PLAYER_NAME lives in public/wire_limits.h (included above
 * via the file-top include list) alongside PACKET_MAX_CHAT_MESSAGE. */

/* Maximum compressed map size (256x256 LZW + bases + pills + starts). The map
 * streams on CHANNEL_BULK (no per-chunk packet), but this still bounds the blob
 * the sender stages and the receiver allocates. */
#define MAP_DOWNLOAD_MAX_SIZE 65536

/* Number of redundant inputs per packet (for packet loss) — each input
 * rides in 4 consecutive 50/s packets (2 new inputs per packet), so a
 * 3-packet loss burst no longer starves the server's input queue */
#define INPUT_REDUNDANCY_COUNT 8

/* Client timeout in ticks (20 seconds at 50 ticks/sec) */
#define CLIENT_TIMEOUT_TICKS 1000

/* Ping interval in ticks (~0.4 seconds at the 50 Hz game-tick clock) */
#define PING_INTERVAL_TICKS 20

#define INFOREQUESTHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOPACKET_INFOREQUEST }
#define TOKENHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOPACKET_TOKEN }
#define DATAHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOPACKET_DATA }
#define GENERICHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION }
#define MESSAGEHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOPACKET_MESSAGE }
#define MESSAGEHEADER_ALLPLAYERS { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOPACKET_MESSAGE_ALL_PLAYERS }
#define POSHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOPOSITION_DATA }
#define VALIDHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOPACKET_VALID }
#define INVALIDHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOPACKET_INVALID }
#define TIMEHEADER { { {'B','o','l','o'}, BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOPACKET_TIMERESPONSE }, 0, 0 }
#define CLIENTHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOCLIENT_DATA }
#define SEVERMSGHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOSERVERMESSAGE }
#define ALLOWPLAYERSHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOALLOWNEWPLAYERS }
#define DISALLOWPLAYERSHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLONOALLOWNEWPLAYERS }
#define REQUESTHEADER { { {'B','o','l','o'}, BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOREJOINREQUEST }, 0 }
#define LGMRETURNHEADER { { {'B','o','l','o'}, BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOLGMRETURN }, 0, 0, 0 }
#define LGMWORKINGHEADER { { {'B','o','l','o'}, BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOPACKET_LGM_OUTWORKING }, 0, 0, 0, 0, 0 }

#define STARTREQUESTHEADER { 'B','o','l','o', BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLOREQUEST_STARTPOS }
#define STARTRESPONSEHEADER { { {'B','o','l','o'}, BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR, BOLO_VERSION_REVISION, BOLORESPONSE_STARTPOS }, 0, 0, 0, 0, 0 }

/* Size of the request packet */
#define BOLOPACKET_REQUEST_SIZE 8
/* position of the request/response part */
#define BOLOPACKET_REQUEST_TYPEPOS 7

/* Size of the mandatory token packet data */
#define BOLOPACKET_MAND_DATA 17

/* Optional Data in the data packets */
#define BOLO_PACKET_MAPDATA 10
#define BOLO_PACKET_SHELLDATA 11
#define BOLO_PACKET_TKDATA 12
#define BOLO_PACKET_PNBDATA 13
#define BOLO_PACKET_MNTDATA 14
#define BOLO_PACKET_SHELLNHDATA 15
/* Game Time request/Response */
#define BOLOPACKET_TIMEREQUEST 42
#define BOLOPACKET_TIMERESPONSE 43

/* Size of the option data header */
#define BOLO_PACKET_OPTDATAHEADER 2


#define BOLO_PACKET_CRC_SIZE 2

/* Mines */
#define ALL_MINES_VISIBLE 0xC0
#define HIDDEN_MINES 0x80
/* "Bolo" string and size of */
#define BOLO_SIGNITURE "Bolo"
#define BOLO_SIGNITURE_SIZE 4

/* String displayed in single player games rather then address */
#define NET_SINGLE_PLAYER_GAME "Single Player Game\0"



#endif /* _NETPACKS_H */

