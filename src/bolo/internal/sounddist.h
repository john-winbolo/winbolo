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

/*********************************************************
*NAME:          soundDistLog
*AUTHOR:        John Morrison
*CREATION DATE: 05/05/01
*LAST MODIFIED: 05/05/01
*PURPOSE:
*  Calculates the item to be logged
*
*ARGUMENTS:
*  value - Sound effect to be played
*  mx    - Map X co-ordinatate for the sound origin
*  my    - Map Y co-ordinatate for the sound origin
*********************************************************/
void soundDistLog(sndEffects value, BYTE mx, BYTE my);

void clientSoundDist(struct GameSim *sim, sndEffects value, BYTE mx, BYTE my);

#endif /* _SOUNDDIST_H */


