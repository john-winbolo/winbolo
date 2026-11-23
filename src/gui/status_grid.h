/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * status_grid.h - Where slot n of a status panel sits
 *
 * The bases, pillboxes and tanks status panels share one grid of three
 * rows: six slots, then two and two with a gap in the middle for the centre
 * icon, then six. The game's renderer (sdl3draw_status.c) and the web Log
 * Viewer (wasm/main_wasm.c) both place their icons with this one sum, so a
 * change to the grid reaches both.
 *
 * Include a positions.h and a tiles.h first. The game's (src/gui/) and the
 * Log Viewer's (src/logviewer/) copies of those headers give the STATUS_*
 * grid values used here with the same numbers. This header includes none of
 * them, because each pair shares one include guard.
 */

#ifndef STATUS_GRID_H
#define STATUS_GRID_H

#if !defined(STATUS_BASE_1_X) || !defined(STATUS_BASE_1_Y) || \
    !defined(STATUS_ITEM_GAP_X) || !defined(STATUS_ITEM_GAP_Y) || \
    !defined(STATUS_ITEM_SIZE_X) || !defined(STATUS_ITEM_SIZE_Y)
#error "status_grid.h: include positions.h and tiles.h before this header"
#endif

/* Slot n (1 to 16) of a status panel, in panel pixels at zoom 1. */
static inline void statusGridItemPos(int n, int *outX, int *outY) {
  int col, row;
  if (n <= 6) {
    col = n - 1;
    row = 0;
  } else if (n <= 8) {
    col = n - 7;
    row = 1;
  } else if (n <= 10) {
    col = (n - 9) + 4; /* cols 4,5 in row 1 */
    row = 1;
  } else {
    col = n - 11;
    row = 2;
  }
  if (outX != 0) *outX = STATUS_BASE_1_X + col * (STATUS_ITEM_GAP_X + STATUS_ITEM_SIZE_X);
  if (outY != 0) *outY = STATUS_BASE_1_Y + row * (STATUS_ITEM_GAP_Y + STATUS_ITEM_SIZE_Y);
}

#endif /* STATUS_GRID_H */
