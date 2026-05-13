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
 *Name:          Client Sim Control
 *Filename:      client_sim_control.h
 *Author:        John Morrison
 *Purpose:
 *  Single client-side mutation path. Applies a ControlEvent
 *  to a ClientSim so that every transport (local + UDP)
 *  routes out-of-band state changes through one function.
 *********************************************************/

#ifndef CLIENT_SIM_CONTROL_H
#define CLIENT_SIM_CONTROL_H

#include "client_sim.h"
#include "control_event.h"

/* Apply a ControlEvent to a ClientSim. Mutates ClientSim game state
 * only. Identity-shaped events (CTRL_PLAYER_JOIN, CTRL_PLAYER_NAME,
 * CTRL_LOBBY_SLOT) are skipped if their playerNum matches the
 * recipient's myPlayerNum — the recipient's self-record is set up
 * by the code that creates the ClientSim. */
void clientSimApplyControl(ClientSim *cs, const ControlEvent *evt);

#endif /* CLIENT_SIM_CONTROL_H */
