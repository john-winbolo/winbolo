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
 * Snapshots the decoder's current reconstructed world into a SpecFramePOD so
 * the bolo / client render path can draw it without sharing logviewer headers.
 * This translation unit is logviewer-world: it reads decoder state through the
 * lv accessors and copies the scalar fields the POD exposes. The POD header
 * (spec_frame.h) carries no bolo/lv dependencies, so the consuming side never
 * sees these headers.
 */

#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_players.h"
#include "lv_pillbox.h"
#include "lv_bases.h"

#include "spec_frame.h"

/* Public camera-slot getter: the followed slot lives in the per-log
 * LogViewerState (cameraSlot), reachable here via lv_screenGetState(). Returns
 * 0 when no log is loaded. */
BYTE lv_screenGetCameraSlot(void) {
  LogViewerState *lv = lv_screenGetState();
  return (lv != NULL) ? lv->cameraSlot : 0;
}

void specFrameFill(SpecFramePOD *out) {
  LogViewerState *lv;
  BYTE i;
  BYTE numPills;
  BYTE numBases;

  if (out == NULL) {
    return;
  }

  /* Zero first so absent slots and empty pill/base indices read present==0. */
  memset(out, 0, sizeof(*out));

  /* Tanks + LGMs are slot-indexed; the players accessors operate on the
   * decoder's current player table. An in-use slot yields a present tank and,
   * since the server emits an LGM for every live tank, a present LGM. */
  for (i = 0; i < SPEC_MAX_TANKS; i++) {
    BYTE mx;
    BYTE my;
    BYTE px;
    BYTE py;
    BYTE frame;
    bool onBoat;

    if (!lv_playersIsInUse(i)) {
      continue;
    }

    lv_playersGetTankDetails(i, &mx, &my, &px, &py, &frame, &onBoat);
    out->tanks[i].present = 1;
    out->tanks[i].slot = i;
    out->tanks[i].mx = mx;
    out->tanks[i].my = my;
    out->tanks[i].px = px;
    out->tanks[i].py = py;
    out->tanks[i].frame = frame;
    out->tanks[i].team = lv_playersGetTeamId(i);
    out->tanks[i].onBoat = onBoat ? 1 : 0;
    lv_playersGetPlayerName(i, out->tanks[i].name);

    lv_playersGetLgmDetails(i, &mx, &my, &px, &py, &frame);
    out->lgms[i].present = 1;
    out->lgms[i].slot = i;
    out->lgms[i].mx = mx;
    out->lgms[i].my = my;
    out->lgms[i].px = px;
    out->lgms[i].py = py;
    out->lgms[i].frame = frame;
  }

  /* Pills and bases are 1-based in the decoder's accessors; copy map position
   * and owner into the 0-based POD arrays. */
  lv = lv_screenGetState();
  if (lv != NULL) {
    numPills = lv_pillsGetNumPills(&lv->pb);
    if (numPills > SPEC_MAX_TANKS) {
      numPills = SPEC_MAX_TANKS;
    }
    for (i = 0; i < numPills; i++) {
      pillbox item;
      lv_pillsGetPill(&lv->pb, &item, (BYTE)(i + 1));
      out->pills[i].present = 1;
      out->pills[i].mx = item.x;
      out->pills[i].my = item.y;
      out->pills[i].owner = item.owner;
    }

    numBases = lv_basesGetNumBases(&lv->bs);
    if (numBases > SPEC_MAX_TANKS) {
      numBases = SPEC_MAX_TANKS;
    }
    for (i = 0; i < numBases; i++) {
      base item;
      lv_basesGetBase(&lv->bs, &item, (BYTE)(i + 1));
      out->bases[i].present = 1;
      out->bases[i].mx = item.x;
      out->bases[i].my = item.y;
      out->bases[i].owner = item.owner;
    }
  }

  out->followedSlot = lv_screenGetCameraSlot();
}
