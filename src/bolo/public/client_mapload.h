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
 *Name:          Client Map Load
 *Filename:      client_mapload.h
 *Purpose:
 *  Map-load entry points that create the ClientSim,
 *  populate it from a map file (or an in-memory compressed
 *  buffer), wire up the local tank, and prime the viewport.
 *  Also save and preview helpers.
 *********************************************************/

#ifndef CLIENT_MAPLOAD_H
#define CLIENT_MAPLOAD_H

#include "global.h"
#include "gametype.h"

struct ClientSim;

bool clientLoadMap(struct ClientSim *cs, char *fileName, gameType game,
                   bool hiddenMines, int32_t srtDelay, int32_t gmeLen,
                   char *playerName, bool wantFree);
bool clientLoadCompressedMap(struct ClientSim *cs, BYTE *buff, int buffLen,
                             const char *mapn, gameType game, bool hiddenMines,
                             int32_t srtDelay, int32_t gmeLen, char *playerName,
                             BYTE playerNum, bool wantFree);
bool clientSaveMap(struct ClientSim *cs, char *fileName);

#endif /* CLIENT_MAPLOAD_H */
