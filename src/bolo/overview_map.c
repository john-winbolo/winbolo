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

/*********************************************************
 *Name:          Overview Map
 *Filename:      overview_map.c
 *Purpose:
 *  Keeps an OverviewMap in step with the sim. Every update
 *  works out which squares the player can see right now -
 *  the block their tank could scroll over plus a block round
 *  each pillbox they can view through - and rewrites those
 *  squares from the current map. Squares that have dropped
 *  out of that set keep the tile they last carried, so what
 *  the player has walked past stays as they left it.
 *
 *  Two kinds of square are held current wherever they are,
 *  because the status panels report both live and a frozen
 *  square would leave the map contradicting them: the one a
 *  pillbox has just been lifted from, and every base's. A
 *  base square is its alliance and nothing else. The lifted
 *  pill's square is the one place a square outside a live
 *  region reads current ground, which is one square, and the
 *  alternative is a pillbox drawn where there is none.
 *
 *  The memory opens seeded: when a map lands, every square
 *  is stamped once from it in the remembered (not-live)
 *  style, so the whole map reads dimmed from the first
 *  frame - the terrain is in the map file every client
 *  holds, so the picture gives nothing away - and only the
 *  live regions brighten over it. From then on a seeded
 *  square behaves exactly like a walked-past one: it holds
 *  what it was stamped with until it goes live, so terrain
 *  changes on ground the player cannot see stay invisible.
 *********************************************************/

#include "global.h"
#include "overview_map.h"
#include "bases.h"
#include "game_sim.h"
#include "pillbox.h"
#include "viewport.h"

/* An inclusive square block centred on (cx,cy), trimmed to the map. The
 * centre and half-width are ints so a block over the top or left edge
 * clamps instead of wrapping through zero. */
static OverviewRect overviewRectAround(int cx, int cy, int half) {
  OverviewRect r; /* Rect to return */

  r.left = cx - half;
  r.top = cy - half;
  r.right = cx + half;
  r.bottom = cy + half;
  if (r.left < 0) {
    r.left = 0;
  }
  if (r.top < 0) {
    r.top = 0;
  }
  if (r.right > MAP_ARRAY_SIZE - 1) {
    r.right = MAP_ARRAY_SIZE - 1;
  }
  if (r.bottom > MAP_ARRAY_SIZE - 1) {
    r.bottom = MAP_ARRAY_SIZE - 1;
  }
  return r;
}

/* Rewrites every square of an inclusive rect from the current sim state.
 * This is the only writer of OverviewMap::tile in the codebase - nothing
 * else may touch it, or the memory stops being a record of what was seen.
 * Returns TRUE if any byte came out different. */
static bool overviewStampRect(OverviewMap *om, struct GameSim *sim, BYTE me,
                              const OverviewRect *r, bool setLive) {
  bool changed;  /* Did any byte move */
  bool isMine;   /* Mine visible on this square */
  BYTE tileNum;  /* Tile the square shows now */
  BYTE flagBits; /* Flags the square carries now */
  int x;         /* Looping variable */
  int y;         /* Looping variable */

  changed = FALSE;
  for (x = r->left; x <= r->right; x++) {
    for (y = r->top; y <= r->bottom; y++) {
      isMine = FALSE;
      tileNum = viewportCalcSquarePure(sim, me, (BYTE)x, (BYTE)y, &isMine);
      flagBits = (BYTE)((setLive == TRUE ? OVERVIEW_F_LIVE : 0) |
                        (isMine == TRUE ? OVERVIEW_F_MINE : 0));

      if (om->tile[x][y] == OVERVIEW_UNSEEN && tileNum != OVERVIEW_UNSEEN) {
        om->seenCount++;
      }
      if (om->tile[x][y] != tileNum) {
        om->tile[x][y] = tileNum;
        changed = TRUE;
      }
      if (om->flags[x][y] != flagBits) {
        om->flags[x][y] = flagBits;
        changed = TRUE;
      }
    }
  }
  return changed;
}

/* TRUE when the square falls inside any of the count rects. There are never
 * more than OVERVIEW_MAX_REGIONS of them, so the walk is cheap. */
static bool overviewPointInRects(const OverviewRect *r, int count, int x,
                                 int y) {
  int i; /* Looping variable */

  for (i = 0; i < count; i++) {
    if (x >= r[i].left && x <= r[i].right && y >= r[i].top &&
        y <= r[i].bottom) {
      return TRUE;
    }
  }
  return FALSE;
}

/* Drops OVERVIEW_F_LIVE over a rect, leaving the tiles alone. Squares that
 * still fall inside one of the keep rects are left untouched: they have not
 * left the live set, and stripping the flag off them only to have it put
 * straight back would report a change on a tick where nothing moved. Returns
 * TRUE if any square that has genuinely left was carrying the flag. */
static bool overviewClearLiveRect(OverviewMap *om, const OverviewRect *r,
                                  const OverviewRect *keep, int keepCount) {
  bool changed; /* Did any byte move */
  int x;        /* Looping variable */
  int y;        /* Looping variable */

  changed = FALSE;
  for (x = r->left; x <= r->right; x++) {
    for (y = r->top; y <= r->bottom; y++) {
      if (overviewPointInRects(keep, keepCount, x, y) == TRUE) {
        continue;
      }
      if ((om->flags[x][y] & OVERVIEW_F_LIVE) != 0) {
        om->flags[x][y] = (BYTE)(om->flags[x][y] & ~OVERVIEW_F_LIVE);
        changed = TRUE;
      }
    }
  }
  return changed;
}

/* TRUE when the two region sets are not the same rects in the same order. */
static bool overviewRegionsDiffer(const OverviewRect *a, int aCount,
                                  const OverviewRect *b, int bCount) {
  int i; /* Looping variable */

  if (aCount != bCount) {
    return TRUE;
  }
  for (i = 0; i < aCount; i++) {
    if (a[i].left != b[i].left || a[i].top != b[i].top ||
        a[i].right != b[i].right || a[i].bottom != b[i].bottom) {
      return TRUE;
    }
  }
  return FALSE;
}

void overviewMapReset(OverviewMap *om) {
  if (om == NULL) {
    return;
  }

  memset(om->tile, OVERVIEW_UNSEEN, sizeof(om->tile));
  memset(om->flags, 0, sizeof(om->flags));
  memset(om->live, 0, sizeof(om->live));
  om->liveCount = 0;
  memset(om->prevLive, 0, sizeof(om->prevLive));
  om->prevLiveCount = 0;
  om->tankWasLive = FALSE;
  om->lastTankMX = 0;
  om->lastTankMY = 0;
  om->haveLastTank = FALSE;
  memset(om->pillWasLive, 0, sizeof(om->pillWasLive));
  memset(om->pillWasInTank, 0, sizeof(om->pillWasInTank));
  om->generation = 0;
  om->seenCount = 0;
}

void overviewMapSeedAll(OverviewMap *om, struct GameSim *sim,
                        BYTE myPlayerNum) {
  OverviewRect all; /* The whole map, one stamp */

  if (om == NULL || sim == NULL) {
    return;
  }

  all.left = 0;
  all.top = 0;
  all.right = MAP_ARRAY_SIZE - 1;
  all.bottom = MAP_ARRAY_SIZE - 1;
  if (overviewStampRect(om, sim, myPlayerNum, &all, FALSE) == TRUE) {
    om->generation++;
  }
}

int overviewMapBuildRegions(struct GameSim *sim, BYTE myPlayerNum,
                            bool haveTank, BYTE tankMX, BYTE tankMY,
                            OverviewRect *out, int maxOut) {
  int count;     /* Rects written so far */
  BYTE numPills; /* Pills on the map */
  BYTE i;        /* Looping variable */

  count = 0;
  if (sim == NULL || out == NULL || maxOut <= 0) {
    return 0;
  }

  if (haveTank == TRUE && count < maxOut) {
    out[count] = overviewRectAround((int)tankMX, (int)tankMY, OVERVIEW_TANK_HALF);
    count++;
  }

  numPills = pillsGetNumPills(&sim->pb);
  for (i = 0; i < numPills && count < maxOut; i++) {
    if (pillsCanView(sim, &sim->pb, i, myPlayerNum) == TRUE) {
      out[count] = overviewRectAround((int)sim->pb->item[i].x,
                                      (int)sim->pb->item[i].y,
                                      OVERVIEW_PILL_HALF);
      count++;
    }
  }

  return count;
}

void overviewMapUpdate(OverviewMap *om, struct GameSim *sim, BYTE myPlayerNum,
                       bool haveTank, bool tankDeathWait, BYTE tankMX,
                       BYTE tankMY) {
  bool tankLive; /* Is there a tank region this update */
  BYTE useMX;    /* Centre of that region */
  BYTE useMY;    /* Centre of that region */
  bool changed;  /* Did anything move this update */
  BYTE numPills; /* Pills on the map */
  BYTE numBases; /* Bases on the map */
  bool nowInTank;      /* Is this pill being carried this update */
  OverviewRect square; /* A single square being held current on its own */
  int idx;       /* Which prevLive rect the replay is up to */
  int i;         /* Looping variable */

  if (om == NULL || sim == NULL) {
    return;
  }

  /* Which square the tank block sits on, if there is one at all. A tank with
   * a position of its own records it here on the way past; one that is dead
   * and still in its slot has none to give - a dead tank reads as the map
   * origin - so it holds the block on the square it last had one, and the
   * player watches their own wreck instead of the ground round it greying
   * out. Anything else means no tank block, which is what releases it and
   * lets the farewell stamp below run. */
  tankLive = haveTank;
  useMX = tankMX;
  useMY = tankMY;
  if (haveTank == TRUE) {
    om->lastTankMX = tankMX;
    om->lastTankMY = tankMY;
    om->haveLastTank = TRUE;
  } else if (tankDeathWait == TRUE && om->haveLastTank == TRUE) {
    tankLive = TRUE;
    useMX = om->lastTankMX;
    useMY = om->lastTankMY;
  }

  memcpy(om->prevLive, om->live, sizeof(om->prevLive));
  om->prevLiveCount = om->liveCount;

  om->liveCount = overviewMapBuildRegions(sim, myPlayerNum, tankLive, useMX,
                                          useMY, om->live,
                                          OVERVIEW_MAX_REGIONS);
  changed = overviewRegionsDiffer(om->live, om->liveCount, om->prevLive,
                                  om->prevLiveCount);

  /* Farewell stamp. A region that has just stopped being live gets one last
   * write from the state as it is now, so an allied pill that has died
   * freezes dead and one that has been captured freezes in the captor's
   * colour rather than a tick stale. overviewMapBuildRegions emits the tank
   * rect first and then pills in ascending index, so walking the same order
   * over last update's owners pairs each stale rect with the region that
   * produced it. */
  idx = 0;
  if (om->tankWasLive == TRUE) {
    if (tankLive == FALSE && idx < om->prevLiveCount) {
      if (overviewStampRect(om, sim, myPlayerNum, &om->prevLive[idx], FALSE) ==
          TRUE) {
        changed = TRUE;
      }
    }
    idx++;
  }
  for (i = 0; i < MAX_PILLS; i++) {
    if (om->pillWasLive[i] == FALSE) {
      continue;
    }
    if (idx < om->prevLiveCount &&
        pillsCanView(sim, &sim->pb, (BYTE)i, myPlayerNum) == FALSE) {
      if (overviewStampRect(om, sim, myPlayerNum, &om->prevLive[idx], FALSE) ==
          TRUE) {
        changed = TRUE;
      }
    }
    idx++;
  }

  /* Pickup stamp. A pillbox that has just been lifted into a tank leaves the
   * square it stood on, and the memory has to be told: the square is frozen,
   * so nothing else will ever rewrite it and the map would keep drawing a
   * pillbox on ground that has not had one since. Only the one square is
   * restamped, and a pill leaves its map position behind when it is carried,
   * so that position is the square the memory is showing it at.
   *
   * This says nothing the player was not already being told - the status
   * panel draws every carried pill as in-tank, whoever is carrying it. Where
   * it is put down is a different matter, and stays hidden: the new square is
   * written only if it is one the player can see. */
  numPills = pillsGetNumPills(&sim->pb);
  for (i = 0; i < MAX_PILLS; i++) {
    nowInTank = (i < (int)numPills) ? sim->pb->item[i].inTank : FALSE;
    if (nowInTank == TRUE && om->pillWasInTank[i] == FALSE) {
      square.left = square.right = (int)sim->pb->item[i].x;
      square.top = square.bottom = (int)sim->pb->item[i].y;
      if (overviewStampRect(om, sim, myPlayerNum, &square, FALSE) == TRUE) {
        changed = TRUE;
      }
    }
    om->pillWasInTank[i] = nowInTank;
  }

  /* Ownership stamp. A base changing hands shows on the status panel the
   * moment it happens, whoever took it, so a map still drawing the old colour
   * on a square the player has walked away from is the map contradicting the
   * panel. Every base square outside the live regions is kept current
   * instead. It gives nothing else away: a base square's tile is its
   * alliance and nothing more - the per-square calculator answers from the
   * base before it looks at terrain or mines - so this says only what the
   * panel is already saying. Dead does not enter into it, because a dead base
   * draws in its owner's colour and the tile does not move.
   *
   * A base inside a live region is skipped and left to the pass below, which
   * is about to write the square anyway. */
  numBases = basesGetNumBases(&sim->bs);
  for (i = 0; i < (int)numBases; i++) {
    square.left = square.right = (int)sim->bs->item[i].x;
    square.top = square.bottom = (int)sim->bs->item[i].y;
    if (overviewPointInRects(om->live, om->liveCount, square.left,
                             square.top) == TRUE) {
      continue;
    }
    if (overviewStampRect(om, sim, myPlayerNum, &square, FALSE) == TRUE) {
      changed = TRUE;
    }
  }

  for (i = 0; i < om->liveCount; i++) {
    if (overviewStampRect(om, sim, myPlayerNum, &om->live[i], TRUE) == TRUE) {
      changed = TRUE;
    }
  }

  /* Stripping the stale flag comes after the stamp, and skips whatever is
   * still live, so a square that was live and stays live is never written
   * twice for no reason. Only a square that has genuinely dropped out of the
   * live set reports a change here, which is what keeps generation still on a
   * tick where nothing moved. */
  for (i = 0; i < om->prevLiveCount; i++) {
    if (overviewClearLiveRect(om, &om->prevLive[i], om->live, om->liveCount) ==
        TRUE) {
      changed = TRUE;
    }
  }

  om->tankWasLive = tankLive;
  numPills = pillsGetNumPills(&sim->pb);
  for (i = 0; i < MAX_PILLS; i++) {
    if (i < (int)numPills) {
      om->pillWasLive[i] = pillsCanView(sim, &sim->pb, (BYTE)i, myPlayerNum);
    } else {
      om->pillWasLive[i] = FALSE;
    }
  }

  if (changed == TRUE) {
    om->generation++;
  }
}
