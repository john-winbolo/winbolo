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
*Name:          WinBolo.net Core
*Filename:      winbolonet_core.h
*Author:        John Morrison
*Creation Date: 23/09/01
*Last Modified: 30/03/26
*Purpose:
*  Core WinBolo.net surface: lifecycle, the shared event
*  queue, and storage accessors. Linked into every binary
*  that talks to WinBolo.net.
*********************************************************/

#ifndef __WINBOLO_NET_CORE_H
#define __WINBOLO_NET_CORE_H

#include <stdio.h>
#include <string.h>
#include "global.h"

/* Size of key buffers (32-char hex string + null terminator) */
#define WINBOLONET_KEY_LEN 33

/* Event Types — values match the JSON API event type field */
#define WINBOLO_NET_EVENT_ALLY_JOIN 0     /* Player B has joined Alliance A */
#define WINBOLO_NET_EVENT_ALLY_LEAVE 1    /* Player B has left Alliance A */
#define WINBOLO_NET_EVENT_BASE_CAPTURE 2  /* Player A has captured a base */
#define WINBOLO_NET_EVENT_PILL_CAPTURE 3  /* Player A has captured a pill */
#define WINBOLO_NET_EVENT_TANK_KILL 4     /* Player A has killed a tank */
#define WINBOLO_NET_EVENT_LGM_KILL 5      /* Player A has killed a lgm */
#define WINBOLO_NET_EVENT_LGM_LOST 6      /* Player A has lost their lgm */
#define WINBOLO_NET_EVENT_BASE_STEAL 7    /* Player A has stolen a base */
#define WINBOLO_NET_EVENT_PILL_STEAL 8    /* Player A has stolen a pill */
#define WINBOLO_NET_EVENT_PLAYER_JOIN 9   /* Player A has joined the game */
#define WINBOLO_NET_EVENT_PLAYER_LEAVE 10 /* Player A has left the game */
#define WINBOLO_NET_EVENT_WIN 11          /* Player A has won the game */
#define WINBOLO_NET_EVENT_REJOIN 12       /* Player A is rejoining */
#define WINBOLO_NET_EVENT_QUITTING 13     /* Player A is quitting */

#define WINBOLO_NET_NO_PLAYER 100 /* If no player use this holder */

/*********************************************************
*NAME:          winbolonetDestroy
*PURPOSE:
* Destroys the winbolonet module.
* Cleans up any open libraries.
*
*ARGUMENTS:
* isServer - TRUE if we are the server
*********************************************************/
void winbolonetDestroy(bool isServer);

/*********************************************************
*NAME:          winbolonetGoodbye
*PURPOSE:
* Sends final update and server quit message to WinBolo.net.
*
*ARGUMENTS:
*
*********************************************************/
void winbolonetGoodbye(void);

/*********************************************************
*NAME:          winbolonetAddEvent
*PURPOSE:
* Adds a WinBolo.net Event for sending to the server.
*
*ARGUMENTS:
* eventType - Type of event this is
* isServer  - Are we the server for this and not a client
* playerA   - Player A player Number
* playerB   - Player B player Number
* aIsBot    - TRUE if player A is a bot (no WBN key)
* bIsBot    - TRUE if player B is a bot (no WBN key)
*********************************************************/
void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB, bool aIsBot, bool bIsBot);

/*********************************************************
*NAME:          winbolonetIsRunning
*PURPOSE:
* Returns if the winbolonet module is running or not.
*
*ARGUMENTS:
*
*********************************************************/
bool winbolonetIsRunning(void);

/*********************************************************
*NAME:          winboloNetGetServerKey
*PURPOSE:
* Copies the server key into keyBuff. Will be empty string
* if not participating in WinBolo.net.
*
*ARGUMENTS:
* keyBuff - Buffer to hold key (must be WINBOLONET_KEY_LEN)
*********************************************************/
void winboloNetGetServerKey(char *keyBuff);

/*********************************************************
*NAME:          winbolonetGetCountryCode
*PURPOSE:
* Returns the cached country code: uppercase 2-char ISO
* 3166-1 alpha-2. Always a valid 2-char string; returns
* "XX" if no fetch has ever written one. Reads from
* [WINBOLO.NET] CountryCode= on first call and from the
* in-memory copy thereafter.
*********************************************************/
const char *winbolonetGetCountryCode(void);

/*********************************************************
*NAME:          winbolonetSetCountryCode
*PURPOSE:
* Called by the news fetcher on a successful response.
* Validates the input is a 2-char string; unsuitable
* values (NULL, wrong length, non-alpha) are ignored —
* the previous value stays. Writes through to
* [WINBOLO.NET] CountryCode=.
*
*ARGUMENTS:
* cc - 2-char ISO 3166-1 alpha-2 country code (any case)
*********************************************************/
void winbolonetSetCountryCode(const char *cc);

#endif /* __WINBOLO_NET_CORE_H */
