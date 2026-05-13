/*
 * $Id$
 *
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
BYTE screenGetPos(screen *value, BYTE xValue, BYTE yValue);
bool screenIsMine(screenMines *value, BYTE xValue, BYTE yValue);

#endif /* CLIENT_RENDER_H */
