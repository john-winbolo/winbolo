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
 *Name:          Discovery
 *Filename:      discovery.h
 *Purpose:
 *  Game discovery: tracker TCP queries and LAN UDP
 *  broadcast scanning. Extracted from netclient.c.
 *********************************************************/

#ifndef DISCOVERY_H
#define DISCOVERY_H

#include "global.h"
#include "platform_net.h"
#include "bolo_packets.h"
#include "../gui/currentgames.h"

/*********************************************************
 *NAME:          discoveryFindTrackedGames
 *PURPOSE:
 * Connects to a tracker server via TCP and downloads the
 * list of current games. Returns success.
 *
 *ARGUMENTS:
 *  cg             - Pointer to the currentGames structure
 *  trackerAddress - Address of the tracker to use
 *  port           - Port of the tracker
 *  motd           - Buffer to hold the message of the day
 *********************************************************/
bool discoveryFindTrackedGames(currentGames *cg, char *trackerAddress, unsigned short port, char *motd);

/* Callback-based broadcast search. The callback is invoked each time a
 * server is discovered, allowing the caller to update UI incrementally.
 * userData is passed through to the callback unchanged. */
typedef void (*BroadcastServerCallback)(INFO_PACKET *info, struct in_addr *addr, void *userData);
bool discoveryFindBroadcastGamesAsync(BroadcastServerCallback callback, void *userData);

#endif /* DISCOVERY_H */
