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
 *Name:          ViewPort
 *Filename:      viewport.h
 *Purpose:
 *  Declares struct ViewPort (forward only) and the viewport
 *  math operations (init/destroy, recalc, view-buffer fill,
 *  square calculation, pan, tank-follow, pill-view pan,
 *  cursor get/set) that operate on a ViewPort substructure
 *  of ClientSim.
 *
 *  The struct definition lives in client_sim_internal.h
 *  because ViewPort is embedded by value in struct ClientSim;
 *  external callers reach it through the clientSimViewport /
 *  clientSimViewportMut accessors in client_sim.h.
 *********************************************************/

#ifndef VIEWPORT_H
#define VIEWPORT_H

#include "viewport_types.h"
#include "client_enums.h"
#include "types.h"
#include "view_policy.h"   /* ViewCategory / VIEW_CATEGORY_COUNT */

struct GameSim;
#ifndef SCROLLSTATE_TYPEDEF
#define SCROLLSTATE_TYPEDEF
typedef struct ScrollState ScrollState;
#endif

typedef struct ViewPort ViewPort;

void viewportInit(ViewPort *vp);
void viewportDestroy(ViewPort *vp);
void viewportRecalc(ViewPort *vp);
void viewportUpdateView(ViewPort *vp, struct GameSim *sim, BYTE myPlayerNum,
                        BYTE brainMap[][MAP_ARRAY_SIZE], updateType value);
BYTE viewportCalcSquare(ViewPort *vp, struct GameSim *sim, BYTE myPlayerNum,
                        BYTE xValue, BYTE yValue, BYTE scrX, BYTE scrY);
/* The tile calculation on its own: reports the mine flag through isMine
 * instead of writing a view buffer, and leaves the map and the mine overlay
 * untouched. sim is non-const only because pillsGetScreenHealth and
 * basesGetAlliancePos take a non-const GameSim *. */
BYTE viewportCalcSquarePure(struct GameSim *sim, BYTE myPlayerNum,
                            BYTE xValue, BYTE yValue, bool *isMine);
void viewportPanX(ViewPort *vp, int dxTiles);
void viewportPanY(ViewPort *vp, int dyTiles);
void viewportFollowTank(ViewPort *vp, ScrollState *scroll, tank myTank);
/* Drop back to the tank view without moving the camera — for the exit paths
 * that recentre themselves (or deliberately don't), where viewportFollowTank
 * would centre too early. */
void viewportSetTankView(ViewPort *vp);
/* What the item-view cycling needs from the client beyond the sim itself.
 * The client fills this from its own state and passes it in, so the cycling
 * here stays logic a test can drive on its own.
 *
 * allyViewable is the alive-tank bit per slot playersCanAllyView takes, and
 * is ignored by the pill and base kinds. eligible is a separate bit per item
 * of each category: a clear bit means the server has stopped sending that
 * item's squares under a decay policy, so the cycle steps over it exactly as
 * it steps over an item that does not qualify. The two masks stay apart
 * because the aliveness mask is also what the overview's region build takes,
 * and that applies decay on its own path.
 *
 * allyLastMapX/allyLastMapY are MAX_TANKS entries holding the last map square
 * each remote tank was actually seen on. A tank outside our viewport arrives
 * as a hidden stub, which zeroes its players entry, so this is the square an
 * ally view centres on while its target is out of sight. Either may be NULL
 * for a caller that keeps no such record. */
typedef struct ViewCycleInputs {
    PlayerBitMap allyViewable;
    PlayerBitMap eligible[VIEW_CATEGORY_COUNT];
    const BYTE  *allyLastMapX;
    const BYTE  *allyLastMapY;
} ViewCycleInputs;

/* No viewable allies, every item eligible, no remembered squares — which
 * leaves the cycling doing what it does with no policy in play. */
void viewCycleInputsDefaults(ViewCycleInputs *in);

/* Enter, cycle or step an item view. kind is a ViewStateKind
 * (client_command.h): VIEW_KIND_PILL, _BASE or _ALLY. horz/vert both 0 means
 * "enter this kind of view, or cycle to the next item if already in it";
 * either non-zero steps to the nearest item that way. Cycling wraps around
 * the items, and drops back to the tank only when there is nothing of that
 * kind left to watch. in may be NULL, which reads as the defaults above. */
void viewportPanInView(ViewPort *vp, struct GameSim *sim, ScrollState *scroll,
                       tank myTank, uint8_t kind, const ViewCycleInputs *in,
                       int horz, int vert);
/* Park the camera on an ally the server has chosen, where the pan entry point
 * above chooses for itself. target is the ally's player number and x/y the map
 * square to centre on. */
void viewportEnterAllyView(ViewPort *vp, ScrollState *scroll, BYTE target,
                           BYTE x, BYTE y);
void viewportPanInPillView(ViewPort *vp, struct GameSim *sim, ScrollState *scroll,
                           tank myTank, int horz, int vert);
/* Per-display-tick upkeep for an item view: returns FALSE once the watched
 * item stops qualifying (pill dead or carried, base captured or gone neutral,
 * ally dead, un-allied or gone, or its decay clock run out), leaving the
 * caller to drop to the tank view. An ally view also re-centres on its target
 * as it drives. Always TRUE in the tank view. */
bool viewportUpdateItemView(ViewPort *vp, struct GameSim *sim, ScrollState *scroll,
                            const ViewCycleInputs *in);
void viewportSetCursor(ViewPort *vp, BYTE posX, BYTE posY);
bool viewportGetCursor(const ViewPort *vp, BYTE *posX, BYTE *posY);
void viewportCenterOnTank(ViewPort *vp, ScrollState *scroll, tank myTank);

#endif /* VIEWPORT_H */
