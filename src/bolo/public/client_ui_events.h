/*
 * $Id$
 *
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
*Name:          ClientUiEvents
*Filename:      client_ui_events.h
*Author:        John Morrison
*Purpose:
*  Post-tick UI fan-out for the client. Pushes synced
*  ClientSim state to the front end each tick: scrolling,
*  status bars, kill/death counters, LGM indicator,
*  pillbox view, messages, and game-over.
*********************************************************/

#ifndef CLIENT_UI_EVENTS_H
#define CLIENT_UI_EVENTS_H

#include "global.h"

struct ClientSim;

void clientUiOnTick(struct ClientSim *cs, bool isBrain);

#endif /* CLIENT_UI_EVENTS_H */
