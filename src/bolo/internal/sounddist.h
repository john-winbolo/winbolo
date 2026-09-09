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
*Name:          Sound Distancing
*Filename:      sounddist.h
*Author:        John Morrison
*Creation Date: 19/1/99
*Last Modified: 05/05/01
*Purpose:
*  Responsible for differentiating between playing soft
*  sound effects and loud sound effects.
*********************************************************/

#ifndef _SOUNDDIST_H
#define _SOUNDDIST_H

#include "global.h"
#include "client_enums.h"  /* sndEffects */
#include "game_sim.h"

/* If the distance from the tank to the event is greater then */
/* 15 map squares play the soft sound */
#define SDIST_SOFT 15

/* If the distance from the tank to the event is greater then */
/* 40 map squares then play no sound */
#define SDIST_NONE 40


/* Prototypes */

/* Play the variant `tier` names for a sound the server delivered. `dir` is
   the map-absolute bearing from input_packet.h, carried for a future stereo
   panner and not read yet. */
void clientSoundDist(struct GameSim *sim, sndEffects value, BYTE tier,
                     BYTE dir);

/* A sound this client's own sim raised, which still has its real square:
   work the tier out from the listener's tank and play it. */
void clientSoundDistLocal(struct GameSim *sim, sndEffects value, BYTE mx,
                          BYTE my);

#endif /* _SOUNDDIST_H */


