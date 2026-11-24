/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          status_draw_hold.h
 * Purpose:       Which status panel draws wait for the frame.
 *
 *   Each pass round the game loop runs the game tick and then
 *   draws a frame. The frame repaints the whole window from the
 *   sim, status panels included, so a panel drawn during the
 *   tick is painted over before anyone sees it. A draw asked
 *   for between the start of the pass and the frame is held
 *   here instead. At the end of the pass the held draws are
 *   dropped if the frame repainted the panels, and one redraw
 *   of every panel is owed if it did not (no tank yet, the
 *   round over).
 *
 *   While the map overview owns the window it draws its own
 *   way, so a draw asked for then is not held.
 *
 *   Logic only, no SDL, so it compiles into the unit test
 *   binary.
 *********************************************************/

#ifndef STATUS_DRAW_HOLD_H
#define STATUS_DRAW_HOLD_H

#include <stdbool.h>

/* Zero-initialise: nothing held, nothing owed. */
typedef struct StatusDrawHold {
  bool held;      /* between the start of the pass and the frame */
  bool pending;   /* a draw was asked for meanwhile */
  bool repainted; /* the frame repainted the panels */
} StatusDrawHold;

/* The start of a pass, before the game tick: draws wait from here on. */
static inline void statusDrawHoldBegin(StatusDrawHold *h) {
  h->held      = true;
  h->repainted = false;
}

/* A status panel draw is asked for. True when it waits for the frame, and
   one is then owed; false when the caller draws it now. */
static inline bool statusDrawHoldAsk(StatusDrawHold *h, bool overviewOwnsWindow) {
  if (!h->held || overviewOwnsWindow) return false;
  h->pending = true;
  return true;
}

/* The frame repainted the status panels. */
static inline void statusDrawHoldRepainted(StatusDrawHold *h) {
  h->repainted = true;
}

/* The end of a pass, after the frame: draws go out as asked again. True
   when a draw was held and the frame did not repaint the panels, so the
   caller redraws every panel once. */
static inline bool statusDrawHoldEnd(StatusDrawHold *h) {
  bool owed = h->pending && !h->repainted;
  h->held    = false;
  h->pending = false;
  return owed;
}

/* The loop was left part way through a pass: nothing is held or owed. */
static inline void statusDrawHoldReset(StatusDrawHold *h) {
  h->held    = false;
  h->pending = false;
}

#endif /* STATUS_DRAW_HOLD_H */
