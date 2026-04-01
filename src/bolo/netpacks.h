/*
 * $Id$
 *
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

#define MAX_UDPPACKET_SIZE 1024
#define MAX_TCPPACKET_SIZE 1024

#define BOLO_VERSION_MAJOR       0x01
#define BOLO_VERSION_MAJORPOS    4
#define BOLO_VERSION_MINOR       0x01
#define BOLO_VERSION_MINORPOS    5
#define BOLO_VERSION_REVISION    0x08
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
typedef struct BOLO_PACK_ATTR {
  struct in_addr serveraddress;
  unsigned short serverport;
  uint16_t _padding;
  uint32_t start_time;
} GAMEID;
#endif

/* Info packet response — 76-byte wire format for game browser */
#ifndef _INFO_PACKET_DEFINED
#define _INFO_PACKET_DEFINED
typedef struct BOLO_PACK_ATTR {
  BOLOHEADER h;
  char mapname[MAP_STR_SIZE];
  GAMEID gameid;
  BYTE gametype;
  BYTE allow_mines;
  BYTE allow_AI;
  BYTE spare1;
  int32_t start_delay;
  int32_t time_limit;
  WORD num_players;
  WORD free_pills;
  WORD free_bases;
  BYTE has_password;
  BYTE spare2;
} INFO_PACKET;
BOLO_STATIC_ASSERT(sizeof(INFO_PACKET) == 76, INFO_PACKET_must_be_76_bytes);
#endif

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
#define PACKET_JOIN_REJECT     112   /* Reason string */
#define PACKET_PLAYER_JOINED   113   /* New player info */
#define PACKET_PLAYER_LEFT     114   /* Player disconnected */
#define PACKET_CHAT_BROADCAST  115   /* Chat to all */
#define PACKET_FULL_STATE      116   /* Periodic full reconciliation */
#define PACKET_MAP_DELTA       117   /* Terrain changes */
#define PACKET_BASE_STATE      118   /* Base ownership/stock change */
#define PACKET_PILL_STATE      119   /* Pill health/ownership change */
#define PACKET_PONG            120   /* Ping response */
#define PACKET_GAME_EVENT      121   /* Explosion, mine hit, etc. */
#define PACKET_MAP_DOWNLOAD    122   /* Compressed map data chunk (server -> client) */
#define PACKET_MAP_ACK         105   /* Map chunk acknowledgment (client -> server) */
#define PACKET_QUIT            106   /* Graceful disconnect (client -> server) */
#define PACKET_PLAYER_LIST     107   /* All connected players (server -> new client) */
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

/* Lobby packets — Server -> Client */
#define PACKET_LOBBY_STATE       140  /* Full lobby snapshot: all 16 slots + server state */
#define PACKET_LOBBY_UPDATE      141  /* Single-player delta: { playerNum, teamNumber, ready, isBot } */
#define PACKET_COUNTDOWN         142  /* { secondsRemaining } */
#define PACKET_GAME_START        143  /* Signal to transition from lobby to game */
#define PACKET_GAME_OVER         144  /* Signal game ended, return to lobby */
#define PACKET_LOBBY_MAP_CHANGE  145  /* Server changed map, clients must re-download */
#define PACKET_WBN_REAUTH       146  /* Client -> Server: re-authenticate WBN token after lobby reset */

/* Alliance update event types */
#define ALLIANCE_EVENT_REQUEST  0
#define ALLIANCE_EVENT_ACCEPT   1
#define ALLIANCE_EVENT_LEAVE    2

/* Magic bytes for new protocol packets */
#define BOLO_NEW_MAGIC_0  'W'
#define BOLO_NEW_MAGIC_1  'B'

/* Max size of player name in join request */
#define PACKET_MAX_PLAYER_NAME 32

/* Map download chunk size — fits comfortably in a UDP datagram */
#define MAP_DOWNLOAD_CHUNK_SIZE 900

/* Maximum compressed map size (256x256 LZW + bases + pills + starts) */
#define MAP_DOWNLOAD_MAX_SIZE 65536

/* Number of redundant inputs per packet (for packet loss) */
#define INPUT_REDUNDANCY_COUNT 3

/* Client timeout in ticks (20 seconds at 50 ticks/sec) */
#define CLIENT_TIMEOUT_TICKS 1000

/* Ping interval in ticks (2 seconds) */
#define PING_INTERVAL_TICKS 100

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

