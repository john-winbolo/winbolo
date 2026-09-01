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
void viewportPanInPillView(ViewPort *vp, struct GameSim *sim, ScrollState *scroll,
                           tank myTank, int horz, int vert);
void viewportSetCursor(ViewPort *vp, BYTE posX, BYTE posY);
bool viewportGetCursor(const ViewPort *vp, BYTE *posX, BYTE *posY);
void viewportCenterOnTank(ViewPort *vp, ScrollState *scroll, tank myTank);

#endif /* VIEWPORT_H */
