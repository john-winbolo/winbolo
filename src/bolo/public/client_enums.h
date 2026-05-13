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

#ifndef _AITYPE_ENUM
#define _AITYPE_ENUM

typedef enum {
  aiNone,
  aiYes,
  aiYesAdvantage,
  aiFull
} aiType;

#endif

#ifndef _BUILDSELECT_ENUM
#define _BUILDSELECT_ENUM

/* The type of building operation currently being selected */
typedef enum {
  BsTrees,
  BsRoad,
  BsBuilding,
  BsPillbox,
  BsMine
} buildSelect;
#endif

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

#ifndef _PLAYERNUMBERS_ENUM
#define _PLAYERNUMBERS_ENUM
/* Player Numbers */
typedef enum {
  player01,
  player02,
  player03,
  player04,
  player05,
  player06,
  player07,
  player08,
  player09,
  player10,
  player11,
  player12,
  player13,
  player14,
  player15,
  player16
} playerNumbers;
#endif

#ifndef _NETTYPE_ENUM
#define _NETTYPE_ENUM
/* The network type of game being played */
typedef enum {
  netNone,   /* Game hasn't Started */
  netSingle, /* Single-player (non-networked) game */
  netUdp     /* Networked game */
} netType;
#endif

#ifndef _NETSTATUS_ENUM
#define _NETSTATUS_ENUM
/* Network status */
typedef enum {
  netLobby,          /* In lobby, waiting for ready */
  netLobbyCountdown, /* Countdown active, game starting soon */
  netJoining,
  netRunning,
  netStartDownload, /* First thing we do */
  netBaseDownload,  /* 2nd thing */
  netPillDownload,  /* 3rd thing */
  netMapDownload,   /* 4th - Map download */
  netTimeDownload,  /* Final. Get game times */
  netFailed
} netStatus;
#endif

#ifndef _TANKBUTTON_ENUM
#define _TANKBUTTON_ENUM
typedef enum {
  TNONE,       /* No Buttons being pressed */
  TLEFT,       /* Left button is being pressed */
  TRIGHT,      /* Right button is being pressed */
  TACCEL,      /* Acellerate Button */
  TDECEL,      /* Decellerate Button is being pressed */
  TLEFTACCEL,  /* Left + Accelerate */
  TRIGHTACCEL, /* Right + Acellerate */
  TLEFTDECEL,  /* Left + Decellerate */
  TRIGHTDECEL  /* Right + Decellearate */
} tankButton;
#endif

#endif /* CLIENT_ENUMS_H */
