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

/* Which rule decides the block of squares round the player's own tank.
 * Expanded is what the map has always drawn - everything the classic 15x15
 * view could scroll to - and Classic narrows it to the window that view is
 * actually showing. Both are client presentation: the server picks which one
 * and sends the same map data either way. */
typedef enum {
  fogExperimentExpanded = 0,
  fogExperimentClassic,
  FOG_EXPERIMENT_COUNT
} FogExperiment;

/* Whether anything stops the player seeing inside that block: nothing, or
 * buildings and any stand of trees more than SIGHT_TREE_MAX_DEPTH deep. Held
 * as a selector rather than a bool so another rule can join it without a
 * second setting. */
typedef enum {
  fogSightOff = 0,
  fogSightBuildingsAndTrees,
  FOG_SIGHT_COUNT
} FogSightMode;

/* The block in force and what blocks sight inside it. Both are process
 * globals, not saved: every launch starts on Expanded with sight off, and
 * the server's lobby settings write them as they arrive — the client applies
 * what it is told rather than choosing for itself.
 *
 * Frontends reach both through the int-typed clientSim mirrors rather than
 * this header, which they may not include. */
FogExperiment overviewFogExperimentGet(void);
void          overviewFogExperimentSet(FogExperiment e);
FogSightMode  overviewFogSightGet(void);
void          overviewFogSightSet(FogSightMode m);

/* Whether this experiment places the block round the tank from the classic
 * view rather than round the tank itself. The block builder and the camera the
 * frontend follows both ask here, so the two cannot disagree about what the
 * scroll keys are driving. */
bool overviewFogBlockFollowsView(FogExperiment e);

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
 * region, whatever the policy says.
 *
 * The rest is the fog experiment and the state the narrower block is placed
 * from: the classic view is still scrolling under the full screen map, so its
 * first visible square and the sub-square part of its position say where the
 * window the player is driving actually is. viewValid is false when there is
 * no live tank or the player is watching an item, which is when the view
 * readings mean nothing. */
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
    uint8_t         experiment;    /* FogExperiment */
    uint8_t         sightMode;     /* FogSightMode */
    bool            viewValid;     /* the classic-view fields below mean something */
    BYTE            viewLeft, viewTop;  /* first visible square of that view */
    bool            manualHold;    /* the player is holding the view off autoscroll */
    int16_t         viewSubX, viewSubY; /* sub-square part of the view position */
} OverviewViewInputs;

/* The rules a server ships with: pillboxes always, bases off, allied tanks
 * always. No clocks, no item view, no viewable allies — so what comes out is
 * the tank block and the pillboxes the player can view through, which is the
 * region set the overview has always had. The fog fields zero with it, which
 * reads as Expanded with sight off and no classic view to place a block
 * from. */
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

/* Pure geometry. Writes the tank rect first when there is one, then the
 * pillboxes in ascending pill index, then the
 * bases in ascending base index, then the allied tanks in ascending player
 * number. Returns the number written, never more than maxOut. The ordering is
 * contractual — the farewell stamp in overviewMapUpdate replays it to pair a
 * stale rect with the region that produced it.
 *
 * Which items of a category earn a rect is the policy in `in`: every one that
 * qualifies under viewPolicyAlways, only the one the player is watching under
 * viewPolicyKey, the ones whose proximity clock has not run out under
 * viewPolicyDecay, and none at all under viewPolicyOff. While a viewPolicyKey
 * category is granting the watched item its rect there is no tank rect at all,
 * whatever tankRect holds: key is one view at a time, and the
 * item's block replaces the tank's rather than joining it. Qualifying is the same
 * test the item views make — pillsCanView, basesCanView, playersCanAllyView —
 * so an enemy, dead, carried, neutral or un-allied item never appears whatever
 * the policy says. Each rect carries the alpha the fade wants: 255 outright,
 * ramping to 0 over the last VIEW_DECAY_FADE_SECS of a decay window.
 *
 * tankRect is the block round the player's own tank, already placed and sized
 * by the caller — the fog experiment decides where it goes and how wide it is,
 * so the choice is made once, in overviewMapUpdate, and this only copies what
 * it is handed. NULL means no tank rect at all. It is copied as it is handed
 * over, so where it goes and what it grants is settled before the call. Every
 * watched item's block — a pill, a base, an allied tank — is always
 * OVERVIEW_PILL_HALF round the item, the size its own view shows, whatever the
 * tank's block is doing. */
int  overviewMapBuildRegions(struct GameSim *sim, BYTE myPlayerNum,
                             const OverviewViewInputs *in,
                             const OverviewRect *tankRect,
                             OverviewRect *out, int maxOut);

/* Whether the overview should be black this tick: from the tick the classic
 * main view cuts to static through to the respawn, and nothing outside a
 * death. What killed the tank decides where that starts — a drowning sinks
 * slowly and is given longer to watch — so both views take the picture away
 * at the same point in the wait, the classic one with static and this one by
 * fading to black. */
bool overviewMapDeathBlackout(int deathWait, int lastDeath);

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
 * its block live and whole, where it last had one - the caller has no position
 * or classic view to give for a dead tank, so both are taken from what was
 * recorded on the last update it was alive for - and the player watches the
 * explosion where it happened rather than the ground round it greying out the
 * moment they die.
 * The view's blackout is what takes the picture away from there; the memory
 * keeps stamping underneath it. A tank that has really gone drops its block
 * outright.
 *
 * With sightMode past fogSightOff, every live block is masked by what the
 * thing it belongs to can actually see from where it stands: a square with a
 * building, or a deep enough stand of trees, between it and that square keeps
 * the tile it last showed, carries OVERVIEW_F_HIDDEN instead of the live and
 * sight bits, and is left in full fog. Each block is looked out of its own
 * OverviewRect::origin - the player's own from the tank, a pillbox's from the
 * pillbox, a base's from the base, an ally's from the ally - so a watched item
 * sees what it can see rather than what the tank can. The last stamp a block
 * gets as it stops being live is masked the same way, so letting the block go
 * does not show the player what it had been keeping from them. With sight off
 * no mask is built and no square ever carries the flag.
 * OverviewMap::hiddenActive records which of the two the update did. */
void overviewMapUpdate(OverviewMap *om, struct GameSim *sim, BYTE myPlayerNum,
                       const OverviewViewInputs *in, bool haveTank,
                       int tankDeathWait, BYTE tankMX, BYTE tankMY);

/* sim is non-const in both calls only because the state they read is reached
 * through non-const APIs: pillsCanView, basesCanView and playersCanAllyView
 * call playersIsAllie, and viewportCalcSquarePure calls pillsGetScreenHealth /
 * basesGetAlliancePos. None of them writes through sim. */

#endif /* OVERVIEW_MAP_H */
