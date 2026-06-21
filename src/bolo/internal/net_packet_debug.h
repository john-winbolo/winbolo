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
*Name:          Net Packet Debug
*Filename:      net_packet_debug.h
*Purpose:
*  Network packet debug logger. Logs packet type, direction,
*  size, and hex dump to stderr or a log file.
*  Controlled at runtime via netPacketDebugEnable() or at
*  compile time by defining BOLO_NET_PACKET_DEBUG=0 to
*  completely compile out all logging.
*********************************************************/

#ifndef NET_PACKET_DEBUG_H
#define NET_PACKET_DEBUG_H

#include "platform_types.h"
#include <stdio.h>

/* Compile-time kill switch: define BOLO_NET_PACKET_DEBUG=0 to remove all logging code */
#ifndef BOLO_NET_PACKET_DEBUG
#define BOLO_NET_PACKET_DEBUG 0
#endif

typedef enum {
  NET_PKT_DIR_RECV,
  NET_PKT_DIR_SEND
} netPacketDir;

typedef enum {
  NET_PKT_SIDE_CLIENT,
  NET_PKT_SIDE_SERVER
} netPacketSide;

#if BOLO_NET_PACKET_DEBUG

/*********************************************************
*NAME:          netPacketDebugEnable
*PURPOSE:       Enable or disable packet debug logging at runtime
*ARGUMENTS:     enable - true to enable, false to disable
*********************************************************/
void netPacketDebugEnable(int enable);

/*********************************************************
*NAME:          netPacketDebugIsEnabled
*PURPOSE:       Query whether packet debug logging is enabled
*RETURNS:       non-zero if enabled
*********************************************************/
int netPacketDebugIsEnabled(void);

/*********************************************************
*NAME:          netPacketDebugSetFile
*PURPOSE:       Set output to a file instead of stderr.
*               Pass NULL to revert to stderr.
*ARGUMENTS:     f - FILE pointer (caller owns lifetime)
*********************************************************/
void netPacketDebugSetFile(FILE *f);

/*********************************************************
*NAME:          netPacketDebugLog
*PURPOSE:       Log a network packet
*ARGUMENTS:
*  side      - CLIENT or SERVER
*  dir       - RECV or SEND
*  buff      - raw packet bytes
*  len       - packet length
*  extraInfo - optional text (e.g. "rejected: bad version"), or NULL
*********************************************************/
void netPacketDebugLog(netPacketSide side, netPacketDir dir,
                       const BYTE *buff, int len, const char *extraInfo);

#else /* BOLO_NET_PACKET_DEBUG == 0 */

#define netPacketDebugEnable(e)       ((void)0)
#define netPacketDebugIsEnabled()     (0)
#define netPacketDebugSetFile(f)      ((void)0)
#define netPacketDebugLog(s,d,b,l,i)  ((void)0)

#endif /* BOLO_NET_PACKET_DEBUG */

#endif /* NET_PACKET_DEBUG_H */
