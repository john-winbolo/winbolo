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

/* Q8 gains for a sound `pan` squares east (positive) or west (negative) of
   the listener, `dist` squares away on the larger axis. The channel towards
   the sound is unity and the other is ducked, so neither is ever above
   SOUND_GAIN_UNITY; past SDIST_SOFT both fall towards the far gain at
   SDIST_NONE. Pure integer arithmetic. */
void soundDistGains(int8_t pan, BYTE dist, uint16_t *gainL, uint16_t *gainR);

/* Play a sound the server delivered, with the pan and dist from its payload
   (see Sound event payloads in input_packet.h): the far variant when dist is
   past SDIST_SOFT, the near one otherwise, panned and attenuated by
   soundDistGains. */
void clientSoundDist(struct GameSim *sim, sndEffects value, int8_t pan,
                     BYTE dist);

/* A sound this client's own sim raised, which still has its real square:
   work the near/far variant, the pan and the distance out from the listener's
   tank and play it. */
void clientSoundDistLocal(struct GameSim *sim, sndEffects value, BYTE mx,
                          BYTE my);

/* A smart-ping sound, panned by the pinged square's east-west offset from
   `listener`'s tank. Distance never quietens it. Plays centred when the
   listener has no tank. */
void clientSoundPing(struct GameSim *sim, BYTE listener, sndEffects value,
                     BYTE mx);

#endif /* _SOUNDDIST_H */


