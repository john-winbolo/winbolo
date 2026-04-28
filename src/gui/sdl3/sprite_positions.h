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
 * Name:          sprite_positions.h
 * Purpose:
 *   Tile-number -> atlas-coordinate lookup tables. Split
 *   out of mapview.h so modules that only need the table
 *   (e.g. the log viewer) can include it without pulling
 *   in bolo screen / sprite-list typedefs that conflict
 *   with their own equivalents.
 *
 *   The tables are populated by mapViewInit(), defined in
 *   mapview.c. Values are atlas pixel offsets at scale 1;
 *   callers multiply by their atlas sheet scale at blit
 *   time.
 *********************************************************/

#ifndef SPRITE_POSITIONS_H
#define SPRITE_POSITIONS_H

#ifdef __cplusplus
extern "C" {
#endif

extern int mapViewPosX[256];
extern int mapViewPosY[256];

/* Populate mapViewPosX/Y. Idempotent — safe to call from multiple
 * subsystems' setup paths. */
void mapViewInit(void);

#ifdef __cplusplus
}
#endif

#endif /* SPRITE_POSITIONS_H */
