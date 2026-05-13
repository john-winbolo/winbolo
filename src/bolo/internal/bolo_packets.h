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
 *Name:          Bolo Packets
 *Filename:      bolo_packets.h
 *Purpose:
 *  Shared packet struct definitions used by the game finder
 *  (netclient.c/netclient.h) and wire protocol code.
 *  Extracted from network.h during legacy network removal.
 *********************************************************/

#ifndef BOLO_PACKETS_H
#define BOLO_PACKETS_H

#include "global.h"
#include "platform_net.h"
#include "client_enums.h"  /* netType, netStatus enums live here now */

#pragma pack(push, 1)

#ifndef _PACKETS_DEFINED
#define _PACKETS_DEFINED

/* Bolo header packets */
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

/* Bolo info packet Response */
#ifndef _GAMEID_DEFINED
#define _GAMEID_DEFINED
typedef struct BOLO_PACK_ATTR {
  struct in_addr serveraddress;
  unsigned short serverport;
  uint16_t _padding;        /* explicit padding - matches original 76-byte wire format */
  uint32_t start_time;  /* was u_long: 8 bytes on 64-bit POSIX — fixed to 32-bit */
} GAMEID;
#endif

#ifndef _INFO_PACKET_DEFINED
#define _INFO_PACKET_DEFINED
typedef struct BOLO_PACK_ATTR {
  BOLOHEADER h;

  char mapname[MAP_STR_SIZE]; /* Pascal string (first byte is length)         */
  GAMEID gameid;       /* 8 byte unique ID for game (combination       */
                       /* of starting machine address & timestamp)     */
  BYTE gametype;       /* Game type (1, 2 or 3: open, tourn. & strict) */
  BYTE allow_mines;    /* 0x80 for normal hidden mines                 */
                       /* 0xC0 for all mines visible                   */
  BYTE allow_AI;       /* 0 for no AI tanks, 1 for AI tanks allowed    */
  BYTE spare1;         /* 0                                            */
  int32_t start_delay; /* if non zero, time until game starts, (50ths) — was long */
  int32_t time_limit;  /* if non zero, time until game ends, (50ths)   — was long */

  WORD num_players;    /* number of players                            */
  WORD free_pills;     /* number of free (neutral) pillboxes           */
  WORD free_bases;     /* number of free (neutral) refuelling bases    */
  BYTE has_password;   /* non-zero if game has password set            */
  BYTE spare2;         /* 0                                            */
} INFO_PACKET;
BOLO_STATIC_ASSERT(sizeof(INFO_PACKET) == 76, INFO_PACKET_must_be_76_bytes);
#endif

/* Time response packet */
typedef struct {
  BOLOHEADER h;
  int32_t start_delay; /* if non zero, time until game starts, (50ths) — was long */
  int32_t time_limit;  /* if non zero, time until game ends, (50ths)   — was long */
} TIME_PACKET;

/* Ping packet repsonse */
typedef struct {
  BOLOHEADER h;
  BYTE from;        /* Player that sent this */
  int32_t sendTime; /* Time sent — was long, which is 8 bytes on 64-bit POSIX */
  BYTE inPacket;    /* In packet Number */
  BYTE outPacket;   /* Output packet Number */
} PING_PACKET;

/* Password test packet */
typedef struct {
  BOLOHEADER h;
  char password[MAP_STR_SIZE]; /* Test password - Pascal string (first byte is length)  */
} PASSWORD_PACKET;

#endif /* _PACKETS_DEFINED */

#pragma pack(pop)

#endif /* BOLO_PACKETS_H */
