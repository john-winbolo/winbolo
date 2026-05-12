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
 *Name:          ViewPort
 *Filename:      viewport.h
 *Purpose:
 *  Declares struct ViewPort (forward only) and the viewport
 *  math operations (init, recalc, pan, pill-view toggle,
 *  tank-follow, square calculation) that operate on a
 *  ViewPort substructure of ClientSim.
 *
 *  The struct definition lives in client_sim_internal.h
 *  because ViewPort is embedded by value in struct ClientSim;
 *  external callers reach it through the clientSimViewport /
 *  clientSimViewportMut accessors in client_sim.h.
 *********************************************************/

#ifndef VIEWPORT_H
#define VIEWPORT_H

#include "viewport_types.h"
#include "types.h"

typedef struct ViewPort ViewPort;

void viewportInit(ViewPort *vp);
void viewportRecalc(ViewPort *vp, map mp, tank myTank);
void viewportPanX(ViewPort *vp, int dxTiles);
void viewportPanY(ViewPort *vp, int dyTiles);
void viewportEnterPillView(ViewPort *vp, BYTE pillX, BYTE pillY);
void viewportExitPillView(ViewPort *vp);
void viewportFollowTank(ViewPort *vp, tank myTank);
BYTE viewportCalcSquare(const ViewPort *vp, BYTE xValue, BYTE yValue, BYTE scrX, BYTE scrY);

#endif /* VIEWPORT_H */
