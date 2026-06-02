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

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "scroll_item_list.h"

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

/* Per-instance scroll state (moved from module-level globals) */
struct ScrollState {
  bool autoScroll;
  BYTE scrollX, scrollY;
  BYTE xPositive, yPositive;
  bool autoScrollOverRide;
  bool mods;
  bool stickyX, stickyXDir, stickyY, stickyYDir;
  ScrollItemList itemList;
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

