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
#include "client_command.h" /* VIEW_KIND_* — which item the player is watching */
#include "overview_types.h"
#include "types.h"
#include "view_policy.h"    /* ViewPolicy / ViewCategory / VIEW_DECAY_* */

struct GameSim;

/* What the region build needs beyond the sim itself: the server's visibility
 * rules, the proximity clocks the client keeps for the local player under
 * viewPolicyDecay, the tick those clocks are read against, and what the camera
 * is parked on. The client fills this from its own state and passes it in, so
 * the builder stays geometry a test can drive on its own.
 *
 * The three clock arrays are the client's mirror of the server's, on the
 * client's own clock: MAX_PILLS, MAX_BASES and MAX_TANKS entries, 0 meaning
 * the player has never been near that item. A NULL array reads as all-zero, so
 * a caller with no clocks can leave them out. ticksPerSec is what ties nowTick
 * to the seconds in decaySecs: the client's clock is the display tick, so it
 * passes GAME_NUMGAMETICKS_SEC. A ticksPerSec of 0 leaves every window shut.
 *
 * allyViewable is the alive-tank bit per slot the ally-view helpers take
 * (clientSimAllyViewMask); with no bits set no allied tank ever earns a
 * region, whatever the policy says. */
typedef struct OverviewViewInputs {
    ViewPolicy      policy[VIEW_CATEGORY_COUNT];
    uint16_t        decaySecs[VIEW_CATEGORY_COUNT];
    const uint32_t *pillNearTick;
    const uint32_t *baseNearTick;
    const uint32_t *allyNearTick;
    uint32_t        nowTick;
    unsigned        ticksPerSec;   /* how nowTick converts to real seconds */
    PlayerBitMap    allyViewable;
    uint8_t         viewKind;      /* VIEW_KIND_* the player is watching */
    BYTE            viewTarget;    /* the pill/base index or ally player number */
} OverviewViewInputs;

/* The rules a server ships with: pillboxes always, bases off, allied tanks
 * always. No clocks, no item view, no viewable allies — so what comes out is
 * the tank block and the pillboxes the player can view through, which is the
 * region set the overview has always had. */
void overviewViewInputsDefaults(OverviewViewInputs *in);

/* Whether one proximity clock is still inside its category's window, and how
 * bright the region it earns is: 255 down to the tick VIEW_DECAY_FADE_SECS of
 * the window is left, then a step a tick to 0 on the last tick of it. A clock
 * of 0 is a player who has never been near the item, so there is nothing to
 * fade. outAlpha may be NULL for a caller that
 * only wants the yes or no — which is what the client's own view exit asks,
 * so the exit and the fade cannot disagree about when a window ends. */
bool overviewViewDecayLive(const OverviewViewInputs *in, ViewCategory cat,
                           uint32_t nearTick, BYTE *outAlpha);

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

/* Pure geometry. Writes the tank rect first when haveTank is true, then the
 * pillboxes in ascending pill index, then the bases in ascending base index,
 * then the allied tanks in ascending player number. Returns the number
 * written, never more than maxOut. The ordering is contractual — the farewell
 * stamp in overviewMapUpdate replays it to pair a stale rect with the region
 * that produced it.
 *
 * Which items of a category earn a rect is the policy in `in`: every one that
 * qualifies under viewPolicyAlways, only the one the player is watching under
 * viewPolicyKey, the ones whose proximity clock has not run out under
 * viewPolicyDecay, and none at all under viewPolicyOff. Qualifying is the same
 * test the item views make — pillsCanView, basesCanView, playersCanAllyView —
 * so an enemy, dead, carried, neutral or un-allied item never appears whatever
 * the policy says. Each rect carries the alpha the fade wants: 255 outright,
 * ramping to 0 over the last VIEW_DECAY_FADE_SECS of a decay window.
 *
 * tankHalf is the half-width of the tank's block: OVERVIEW_TANK_HALF for a
 * living tank, and the shrinking figure from overviewMapDeathTankHalf while a
 * dead one's sight closes. A pill and a base block are always
 * OVERVIEW_PILL_HALF and an allied tank's always OVERVIEW_TANK_HALF — dying
 * takes the tank's own sight, not what it is watching through. */
int  overviewMapBuildRegions(struct GameSim *sim, BYTE myPlayerNum,
                             const OverviewViewInputs *in,
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
                       const OverviewViewInputs *in, bool haveTank,
                       int tankDeathWait, int tankLastDeath,
                       BYTE tankMX, BYTE tankMY);

/* sim is non-const in both calls only because the state they read is reached
 * through non-const APIs: pillsCanView, basesCanView and playersCanAllyView
 * call playersIsAllie, and viewportCalcSquarePure calls pillsGetScreenHealth /
 * basesGetAlliancePos. None of them writes through sim. */

#endif /* OVERVIEW_MAP_H */
