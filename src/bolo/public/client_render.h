/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


/*********************************************************
*Name:          ClientRender
*Filename:      client_render.h
*Author:        John Morrison
*Purpose:
*  Per-frame render entry point and small screen/mine-view
*  buffer accessors. Extracted from screen.c so the render
*  pipeline lives in its own translation unit.
*********************************************************/

#ifndef CLIENT_RENDER_H
#define CLIENT_RENDER_H

#include "global.h"
#include "viewport_types.h"
#include "client_enums.h"

struct ClientSim;

void clientRenderFrame(struct ClientSim *cs, updateType value);
/* Non-mutating: would a one-tile scroll in `value` (left/right/up/down)
 * keep the tank on screen? Lets the smooth-scroll input layer avoid
 * ramping a sub-tile drag against an edge it can't actually cross. */
bool clientRenderCanScroll(struct ClientSim *cs, updateType value);
BYTE screenGetPos(const screen *value, BYTE xValue, BYTE yValue);
bool screenIsMine(const screenMines *value, BYTE xValue, BYTE yValue);

#endif /* CLIENT_RENDER_H */
