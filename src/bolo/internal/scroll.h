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

  /* Canuck's threat alert-jump overlay (on top of the crosshair-edge push).
   * peekPhase: 0 none, 1 sliding to threat, 2 holding, 3 sliding back.
   * seenThreat* latches a threat we've already alerted on so we don't jump to
   * it again until it leaves the concern radius. */
  BYTE  peekPhase;
  BYTE  peekTargetX, peekTargetY;
  DWORD peekUntilTick;
  BYTE  peekReturnX, peekReturnY;
  bool  seenThreatTank[MAX_TANKS];
  bool  seenThreatPill[MAX_PILLS];

  /* v1WithThreats threat bias: a world-relative view offset toward the threats
   * around the tank, layered on top of unmodified v1 autoscroll and active
   * only while moving. It is NOT perpendicular to facing, so steering doesn't
   * swing the camera. threatLat* is the offset actually applied last tick (in
   * tiles), removed before re-running v1 so the underlying classic trajectory
   * is unaffected; threatLatDesired* is the debounced target it eases toward.
   * threatLatSub* is the same applied offset tracked at sub-tile (1/256)
   * resolution: it eases smoothly and its fractional part is handed to the
   * renderer via subPos so the camera glides instead of stepping a whole tile;
   * threatLat* is just its whole-tile part, kept for exact base recovery. */
  int8_t  threatLatX, threatLatY;
  int8_t  threatLatDesiredX, threatLatDesiredY;
  int16_t threatLatSubX, threatLatSubY;
};

/* ------------------------------------------------------------------
 * Scrolling mechanism selector (experiment scaffold).
 *
 * Switches the whole view-scroll algorithm. ENHANCED is John's current
 * sub-tile / threat-aware autoscroll. The CLASSIC_* modes reproduce the
 * original integer-tile WinBolo behaviour (no sub-tile smoothing) as a
 * starting point to build new mechanisms on. Two orthogonal knobs let a
 * mechanism opt into smoothness and sub-tile pixel precision; the
 * CLASSIC modes ignore them (always integer / no smoothing).
 * ------------------------------------------------------------------ */
typedef enum {
  SCROLL_MECH_CLASSIC_NO_AUTOSCROLL = 0, /* original, autoscroll off */
  SCROLL_MECH_CLASSIC_AUTOSCROLL    = 1, /* original gunsight-edge autoscroll */
  SCROLL_MECH_ENHANCED              = 2, /* current sub-tile / threat-aware */
  SCROLL_MECH_ANDREW_ENHANCED       = 3, /* Andrew's variant (WIP; = enhanced for now) */
  SCROLL_MECH_V1_WITH_THREATS       = 4  /* v1 autoscroll + moving-only lateral
                                          * bias toward the side with threats */
} ScrollMechanism;

/* Compile-time default. Change this (or call scrollSetMechanism at
 * runtime) to switch mechanisms. Hardcoded to V1_WITH_THREATS so the new
 * mechanism is active without touching the GUI selector. */
#ifndef SCROLL_MECHANISM_DEFAULT
#define SCROLL_MECHANISM_DEFAULT SCROLL_MECH_V1_WITH_THREATS
#endif

/* Mechanism selector + tuning knobs (process-global, runtime-switchable). */
ScrollMechanism scrollGetMechanism(void);
void            scrollSetMechanism(ScrollMechanism mech);
/* Smoothness level for mechanisms that support it (0 = none). The CLASSIC
 * modes ignore it; a new mechanism reads it to scale its easing. */
int             scrollGetSmoothness(void);
void            scrollSetSmoothness(int level);
/* Whether the view is tracked at sub-tile (1/256) precision. The CLASSIC
 * modes are always integer-tile and ignore this. */
bool            scrollGetSubTilePrecision(void);
void            scrollSetSubTilePrecision(bool on);

/* Prototypes */

void scrollCreate(ScrollState *ss);
void scrollSetScrollType(ScrollState *ss, bool isAuto);
void scrollCenterObject(ScrollState *ss, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY);
/* ownRenderX/ownRenderY: the local tank's rendered (render-error-smoothed)
 * world position. The ENHANCED sub-tile autoscroll centres on it so the
 * camera tracks the drawn hull instead of shearing against it; the classic
 * whole-tile paths use objectX/objectY. With the offset at zero these equal
 * the raw tank position. */
bool scrollUpdate(ScrollState *ss, GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, bool isTank, BYTE gunsightX, BYTE gunsightY, BYTE speed, BYTE armour, TURNTYPE angle, bool manual, bool tankIsDead, WORLD ownRenderX, WORLD ownRenderY);
bool scrollCheck(BYTE xValue, BYTE yValue, BYTE objectX, BYTE objectY);
bool scrollManual(ScrollState *ss, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, TURNTYPE angle);
bool scrollNoAutoScroll(ScrollState *ss, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, TURNTYPE angle);
bool scrollAutoScroll(ScrollState *ss, GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, BYTE gunsightX, BYTE gunsightY, BYTE speed, TURNTYPE angle, WORLD ownRenderX, WORLD ownRenderY);

#endif /* SCROLL_H */

