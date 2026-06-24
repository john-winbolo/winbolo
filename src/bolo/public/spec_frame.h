/*
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

/*
 * SpecFramePOD — a plain-old-data snapshot of the log viewer's reconstructed
 * world for one tick, used to bridge the decoder into the spectator render
 * path.
 *
 * This header is deliberately dependency-free: it pulls in only <stdint.h> and
 * defines its own size constants. The bolo and logviewer trees cannot share
 * their own headers in a single translation unit (their `player`/`players`
 * types and the global.h New/Dispose allocator macros collide), so this POD is
 * the neutral seam. The logviewer side fills it from the decoder; the bolo /
 * client side reads it to draw. Neither end needs the other's headers.
 *
 * SPEC_MAX_TANKS mirrors bolo's MAX_TANKS / MAX_PILLS / MAX_BASES (all 16) and
 * SPEC_NAME_LEN mirrors PLAYER_NAME_LEN (65); they are redefined here rather
 * than included so this header stays free of bolo/lv dependencies.
 */

#ifndef SPEC_FRAME_H
#define SPEC_FRAME_H

#include <stdint.h>

#define SPEC_MAX_TANKS 16
#define SPEC_NAME_LEN  65

/* One human/bot tank. present==0 means the slot is empty (every other field
 * is then zero). mx/my are map cells, px/py pixel offsets within the cell. */
typedef struct {
  uint8_t present;
  uint8_t slot;
  uint8_t mx;
  uint8_t my;
  uint8_t px;
  uint8_t py;
  uint8_t frame;
  uint8_t team;
  uint8_t onBoat;
  char    name[SPEC_NAME_LEN];
} SpecTankPOD;

/* One little green man. present mirrors the owning tank's slot being in use. */
typedef struct {
  uint8_t present;
  uint8_t slot;
  uint8_t mx;
  uint8_t my;
  uint8_t px;
  uint8_t py;
  uint8_t frame;
} SpecLgmPOD;

/* One pillbox. mx/my from pillbox.x/.y, owner from pillbox.owner. */
typedef struct {
  uint8_t present;
  uint8_t mx;
  uint8_t my;
  uint8_t owner;
} SpecPillPOD;

/* One base. mx/my from base.x/.y, owner from base.owner. */
typedef struct {
  uint8_t present;
  uint8_t mx;
  uint8_t my;
  uint8_t owner;
} SpecBasePOD;

typedef struct {
  SpecTankPOD tanks[SPEC_MAX_TANKS];
  SpecLgmPOD  lgms[SPEC_MAX_TANKS];
  SpecPillPOD pills[SPEC_MAX_TANKS];
  SpecBasePOD bases[SPEC_MAX_TANKS];
  uint8_t     followedSlot;   /* the slot the camera is currently following */
} SpecFramePOD;

#endif /* SPEC_FRAME_H */
