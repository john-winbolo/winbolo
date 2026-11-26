/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          item_labels.h
 * Purpose:
 *   Which pill and base numbers the views draw. Two plain
 *   C functions, in a header so the unit tests can call
 *   them with no window, renderer or font:
 *
 *   - mapViewItemLabelNumber: the classic view asks the sim
 *     which pill or base is on a square. This turns that
 *     answer into the number to draw, or -1 when the lookup
 *     found nothing there.
 *   - mapViewPickItemLabels: the full screen map and the map
 *     overview pick, from the snapshot's list, the numbers
 *     whose square shows a pill or base tile in the memory.
 *
 *   Needs only the public types (client_sim.h for
 *   OverviewItemLabel and OverviewMap) and map_colours.h for
 *   the tile test. No SDL calls, no ImGui, no ClientSim calls.
 *********************************************************/

#ifndef WINBOLO_ITEM_LABELS_H
#define WINBOLO_ITEM_LABELS_H

#include <stdbool.h>
#include "global.h"
#include "client_sim.h"   /* OverviewItemLabel, OverviewMap, MAX_PILLS, MAX_BASES */
#include "map_colours.h"  /* mapColourItemNumberOnTile */

#ifdef __cplusplus
extern "C" {
#endif

/* The number to draw for a lookup by square: clientSimGetPillNumPos or
   clientSimGetBaseNumPos give 1..MAX_PILLS (or 1..MAX_BASES) when they find
   an item there. When they find none they give a value past that range
   (253 for a pill, 254 for a base), and the square gets no number. A hidden
   square can show a remembered pill tile after its pill moved, so the
   lookup can find nothing on a pill tile. Returns the number less one, as
   the views draw it, or -1 for no number. */
static inline int mapViewItemLabelNumber(BYTE found, bool isBase) {
  int limit = isBase ? MAX_BASES : MAX_PILLS;
  if (found < 1 || (int)found > limit) {
    return -1;
  }
  return (int)found - 1;
}

/* Copies into out (at most cap) each label from all[0..total) that the full
   screen map and the map overview draw. A label is kept when its switch is
   on (showPills for a pill, showBases for a base) and the memory's tile on
   its square is a pill tile (or a base tile). Only the remembered tile is
   asked. Whether the square is live is not asked, so a pill or base in the
   fog keeps its number. Returns the number of labels copied. */
static inline int mapViewPickItemLabels(const OverviewMap *om,
                                        const OverviewItemLabel *all,
                                        int total,
                                        bool showPills, bool showBases,
                                        OverviewItemLabel *out, int cap) {
  int n = 0;
  int i;
  if (om == NULL || all == NULL || out == NULL) {
    return 0;
  }
  for (i = 0; i < total && n < cap; i++) {
    const OverviewItemLabel *l = &all[i];
    bool wanted = l->isBase ? showBases : showPills;
    if (!wanted) {
      continue;
    }
    if (!mapColourItemNumberOnTile(om->tile[l->mapX][l->mapY], l->isBase)) {
      continue;
    }
    out[n++] = *l;
  }
  return n;
}

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_ITEM_LABELS_H */
