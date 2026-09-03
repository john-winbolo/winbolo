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

#include "global.h"
#include "overview_types.h"
#include "types.h"

struct GameSim;

/* Ticks the block round a dead tank takes to close over the wreck once it
 * starts closing — half a second at the game's tick rate. */
#define OVERVIEW_DEATH_CLOSE_TICKS (GAME_NUMGAMETICKS_SEC / 2)

/* Ticks of static at the end of a death, running out as the tank comes back —
 * two seconds at the game's tick rate. The order is the point: the block
 * closes first and the player sits in the fog for a moment, and the static is
 * then the last thing between them and the respawn. It has to be shorter than
 * what is left of the wait once the block has closed, or the map would never
 * go dark at all; overviewMapDeathStatic's own case in the unit tests pins
 * that. */
#define OVERVIEW_DEATH_STATIC_TICKS (2 * GAME_NUMGAMETICKS_SEC)

/* Clears the memory: every square unseen, no regions, generation 0.
 * A calloc'd owner is not already reset — OVERVIEW_UNSEEN is 0xFF. */
void overviewMapReset(OverviewMap *om);

/* Stamps the whole map once in the remembered (not-live) style, so the
 * overview opens with every square readable but dimmed instead of black.
 * Run after a map lands, with the local slot settled — the per-square
 * calculator draws pills, bases and mines from that player's point of
 * view. A seeded square then freezes like any other remembered one until
 * a live region reaches it. */
void overviewMapSeedAll(OverviewMap *om, struct GameSim *sim,
                        BYTE myPlayerNum);

/* Pure geometry. Writes the tank rect first when haveTank is true, then one
 * rect per pillbox passing pillsCanView in ascending pill index. Returns the
 * number written, never more than maxOut. The ordering is contractual — the
 * farewell stamp in overviewMapUpdate replays it to pair a stale rect with
 * the region that produced it.
 *
 * tankHalf is the half-width of the tank's block: OVERVIEW_TANK_HALF for a
 * living tank, and the shrinking figure from overviewMapDeathTankHalf while a
 * dead one's sight closes. The pill blocks are always OVERVIEW_PILL_HALF —
 * dying takes the tank's own sight, not what its pillboxes report. */
int  overviewMapBuildRegions(struct GameSim *sim, BYTE myPlayerNum,
                             bool haveTank, BYTE tankMX, BYTE tankMY,
                             int tankHalf, OverviewRect *out, int maxOut);

/* Half-width of a dead tank's block, from the ticks left on its death wait and
 * what killed it: OVERVIEW_TANK_HALF while the player is still watching the
 * explosion, shrinking square by square over OVERVIEW_DEATH_CLOSE_TICKS once
 * the classic main view would have cut to static, and -1 once the block has
 * gone entirely. Both views therefore take the picture away at the same point
 * in the wait; what they put there differs, the classic view's static running
 * from here to the respawn and the overview going dark and finding its static
 * at the end. */
int  overviewMapDeathTankHalf(int deathWait, int lastDeath);

/* Whether the overview should be showing static this tick: the last
 * OVERVIEW_DEATH_STATIC_TICKS of the death wait, and nothing outside a death.
 * Cause does not enter into it — how the tank died changes when the picture
 * goes, not when it comes back. */
bool overviewMapDeathStatic(int deathWait);

/* Rebuilds the live regions from the current sim state, stamps them into the
 * memory, and gives any region that has just stopped being live one last
 * stamp so it freezes as it is now rather than as it was a tick ago.
 *
 * Two single squares are also written outside any live region, so the map
 * cannot contradict the status panels, which report both of these live: the
 * square a pillbox has just been lifted from, on the tick it goes in-tank,
 * and every base's own square, on any tick its tile has moved.
 *
 * tankDeathWait is the ticks left on the death wait of a tank that is still in
 * its slot but dead, and 0 for a tank that is alive or gone. Such a tank keeps
 * its block live on the square it last held - the caller has no position to
 * give for a dead tank - so the player watches the explosion where it happened
 * rather than the ground round it greying out the moment they die. The block
 * then closes over the wreck on the schedule overviewMapDeathTankHalf sets,
 * tankLastDeath being the LAST_DEATH_BY_* that schedule reads. A tank that has
 * really gone drops its block outright. */
void overviewMapUpdate(OverviewMap *om, struct GameSim *sim, BYTE myPlayerNum,
                       bool haveTank, int tankDeathWait, int tankLastDeath,
                       BYTE tankMX, BYTE tankMY);

/* sim is non-const in both calls only because the state they read is reached
 * through non-const APIs: pillsCanView calls playersIsAllie, and
 * viewportCalcSquarePure calls pillsGetScreenHealth / basesGetAlliancePos.
 * Neither function writes through sim. */

#endif /* OVERVIEW_MAP_H */
