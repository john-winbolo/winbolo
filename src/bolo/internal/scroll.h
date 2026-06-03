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
*Name:          scroll
*Filename:      scroll.h
*Author:        John Morrison
*Creation Date:  16/1/99
*Last Modified: 19/11/99
*Purpose:
*  Handles scrolling on the screen. Auto scrolling and
*  keeping the object in the centre of the screen
*********************************************************/

#ifndef SCROLL_H
#define SCROLL_H

#include <stdint.h>
#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "viewport_types.h"  /* MAIN_SCREEN_SIZE_X/Y */

#ifndef SCROLLSTATE_TYPEDEF
#define SCROLLSTATE_TYPEDEF
typedef struct ScrollState ScrollState;
#endif

/* Defines */

/* Center Offset from left edge of the screen is 4 map units */
#define SCROLL_CENTER 8
/* Amount to do division by */
#define SCROLL_DIVIDE 2.5

/* The distance from the edge the tank has to beed to scroll when autoscroll is off */
#define NO_SCROLL_EDGE 2

/* Per-instance scroll state (moved from module-level globals).
 *
 * Autoscroll model: the view targets tank_center + currentOffset. The
 * offset is only recomputed on discrete events (gunsight edge cross,
 * new threat entering concern radius, parked with rear threat) — not
 * every tick. Between events the offset is fixed; when it changes,
 * currentOffset slides toward targetOffset one tile at a time. */
struct ScrollState {
  bool autoScroll;
  BYTE scrollX, scrollY;
  BYTE xPositive, yPositive;
  bool autoScrollOverRide;
  bool mods;
  bool stickyX, stickyXDir, stickyY, stickyYDir;

  /* Autoscroll offset relative to tank center.
   * target = whole tiles (computed from threat geometry).
   * currentSub = sub-tile units (1/256 tile) for smooth slide. */
  int8_t  targetOffsetX, targetOffsetY;
  int16_t currentOffsetSubX, currentOffsetSubY;

  /* Sub-tile pixel position within the current view tile (0..255 in
   * 1/256-tile units). Read by the GUI each frame and applied as a
   * drag offset, so the view scrolls continuously across tile
   * boundaries instead of snapping. */
  int16_t subPosX, subPosY;

  /* Event detection state. */
  bool  initialized;        /* false until the first scrollAutoScroll tick has
                             * captured the current threat/gunsight baseline;
                             * suppresses spurious "newly entered" events on
                             * game start, respawn, and mode swap. */
  DWORD lastRecalcTick;     /* last tick we updated targetOffset */
  DWORD parkedSinceTick;    /* tick when speed last became 0 (0 = not parked) */
  bool  gunsightWasInside;  /* gunsight inside view last tick — edge-cross detector */
  bool  prevThreatTank[MAX_TANKS];
  bool  prevThreatPill[MAX_PILLS];
};

/* Prototypes */

void scrollCreate(ScrollState *ss);
void scrollSetScrollType(ScrollState *ss, bool isAuto);
void scrollCenterObject(ScrollState *ss, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY);
bool scrollUpdate(ScrollState *ss, GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, bool isTank, BYTE gunsightX, BYTE gunsightY, BYTE speed, BYTE armour, TURNTYPE angle, bool manual, bool tankIsDead);
bool scrollCheck(BYTE xValue, BYTE yValue, BYTE objectX, BYTE objectY);
bool scrollManual(ScrollState *ss, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, TURNTYPE angle);
bool scrollNoAutoScroll(ScrollState *ss, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, TURNTYPE angle);
bool scrollAutoScroll(ScrollState *ss, GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, BYTE gunsightX, BYTE gunsightY, BYTE speed, TURNTYPE angle);

#endif /* SCROLL_H */

