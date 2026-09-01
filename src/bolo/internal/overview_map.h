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
 *Name:          Overview Map
 *Filename:      overview_map.h
 *Purpose:
 *  Maintains an OverviewMap: which squares the local player
 *  can see right now (the live regions), and the tile every
 *  square carried the last time it was live.
 *
 *  The struct itself is declared in public/overview_types.h
 *  so frontends can read it; only this module writes it.
 *********************************************************/

#ifndef OVERVIEW_MAP_H
#define OVERVIEW_MAP_H

#include "overview_types.h"
#include "types.h"

struct GameSim;

/* Clears the memory: every square unseen, no regions, generation 0.
 * A calloc'd owner is not already reset — OVERVIEW_UNSEEN is 0xFF. */
void overviewMapReset(OverviewMap *om);

/* Pure geometry. Writes the tank rect first when haveTank is true, then one
 * rect per pillbox passing pillsCanView in ascending pill index. Returns the
 * number written, never more than maxOut. The ordering is contractual — the
 * farewell stamp in overviewMapUpdate replays it to pair a stale rect with
 * the region that produced it. */
int  overviewMapBuildRegions(struct GameSim *sim, BYTE myPlayerNum,
                             bool haveTank, BYTE tankMX, BYTE tankMY,
                             OverviewRect *out, int maxOut);

/* Rebuilds the live regions from the current sim state, stamps them into the
 * memory, and gives any region that has just stopped being live one last
 * stamp so it freezes as it is now rather than as it was a tick ago. */
void overviewMapUpdate(OverviewMap *om, struct GameSim *sim, BYTE myPlayerNum,
                       bool haveTank, BYTE tankMX, BYTE tankMY);

/* sim is non-const in both calls only because the state they read is reached
 * through non-const APIs: pillsCanView calls playersIsAllie, and
 * viewportCalcSquarePure calls pillsGetScreenHealth / basesGetAlliancePos.
 * Neither function writes through sim. */

#endif /* OVERVIEW_MAP_H */
