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
 *Name:          Client Enums
 *Filename:      client_enums.h
 *Purpose:
 *  Tier-T1 header: gameType / labelLen / updateType /
 *  sndEffects enums. Split out of screen.h so the ~60
 *  callers that only need these enums can include this
 *  header instead of the whole screen.h facade.
 *********************************************************/

#ifndef CLIENT_ENUMS_H
#define CLIENT_ENUMS_H

#ifndef _GAMETYPE_ENUM
#define _GAMETYPE_ENUM

typedef enum {
  gameOpen = 1,
  gameTournament,
  gameStrictTournament
} gameType;

#endif


#ifndef _LABELLEN_ENUM
#define _LABELLEN_ENUM

typedef enum {
  lblNone,
  lblShort,
  lblLong
} labelLen;

#endif

#ifndef _UPDATETYPE_ENUM
#define _UPDATETYPE_ENUM
typedef enum {
  left,
  right,
  up,
  down,
  redraw
} updateType;

#endif

#ifndef _SNDEFFECTS_ENUM
#define _SNDEFFECTS_ENUM
typedef enum {
  shootSelf,
  shootNear,
  shotTreeNear,
  shotTreeFar,
  shotBuildingNear,
  shotBuildingFar,
  hitTankNear,
  hitTankFar,
  hitTankSelf,
  bubbles,
  tankSinkNear,
  tankSinkFar,
  bigExplosionNear,
  bigExplosionFar,
  farmingTreeNear,
  farmingTreeFar,
  manBuildingNear,
  manBuildingFar,
  manDyingNear,
  manDyingFar,
  manLayingMineNear,
  mineExplosionNear,
  mineExplosionFar,
  shootFar
} sndEffects;

#endif

#endif /* CLIENT_ENUMS_H */
