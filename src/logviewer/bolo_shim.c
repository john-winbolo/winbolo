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
*Name:          Bolo Shim
*Filename:      bolo_shim.c
*Purpose:
*  Standalone-LogViewer-only adapter that satisfies the
*  bolo symbols referenced by gui/sdl3/mapview.c when
*  mapview.c is linked into the LogViewer target.
*
*  Two parts:
*
*  Part 1 — accessor wrappers for the eight `screen*Get*`
*  functions invoked from mapView{Tiles,Shells,Tanks,LGMs}
*  (mapview.c lines 51-349). Each wrapper delegates to the
*  logviewer's lv_* equivalent.
*
*  Part 2 — abort()-bodied stubs for the bolo helpers called
*  from the dead-code path in mapview.c lines 350-end
*  (mapViewCalcSquare / mapViewBuildTileBuffer /
*  mapViewRenderCentered). These entry points are NEVER
*  invoked from the logviewer at runtime; the stubs exist
*  only to satisfy the linker.
*
*  This file MUST NOT enter the WinBolo target — it would
*  collide with bolo/ *.c definitions of the same symbols.
*  Lives only in LOGVIEWER_IMGUI_SOURCES per
*  plans/ctrailer.md Phase B.
*
*  Type-layout caveat:
*  mapview.c includes bolo headers directly and sees the
*  bolo struct layouts (e.g. struct screenObj's inline 2D
*  array). The logviewer's parallel structs use a different
*  layout (e.g. screenObj.screenItem is a pointer to an
*  allocated buffer). At the C ABI level a pointer is just
*  a pointer, so the chain
*
*    logviewer caller (logviewer struct *)
*       -> mapview.c forwards as opaque (bolo struct *)
*       -> shim (logviewer struct *)
*       -> lv_* (logviewer struct *, derefs)
*
*  is safe as long as nothing between the caller and the
*  lv_* implementation actually dereferences. mapview.c only
*  goes through these accessors — verified for Phase B's
*  Draw* path.
*********************************************************/

#include "lv_global.h"
#include "backend.h"
#include "lv_screenbullet.h"
#include "lv_screenlgm.h"
#include "lv_screentank.h"
#include "lv_pillbox.h"
#include "lv_bases.h"
#include "lv_bolo_map.h"
#include "lv_players.h"

#include <stdlib.h>  /* abort */


/* ====================================================================
 * Part 1 — accessor wrappers (called from mapview.c Draw* path).
 *
 * Seven thin delegates plus one adapter (screenTanksGetItem) that
 * bridges a 9-arg bolo signature to the logviewer's 11-arg variant.
 * ==================================================================== */

BYTE screenGetPos(screen *value, BYTE xValue, BYTE yValue) {
  return lv_screenGetPos(value, xValue, yValue);
}

bool screenIsMine(screenMines *value, BYTE xValue, BYTE yValue) {
  return lv_screenIsMine(value, xValue, yValue);
}

int screenBulletsGetNumEntries(screenBullets *value) {
  return lv_screenBulletsGetNumEntries(value);
}

void screenBulletsGetItem(screenBullets *value, int itemNum,
                          BYTE *mx, BYTE *my, BYTE *px, BYTE *py,
                          BYTE *frame) {
  lv_screenBulletsGetItem(value, itemNum, mx, my, px, py, frame);
}

BYTE screenTanksGetNumEntries(screenTanks *value) {
  return lv_screenTanksGetNumEntries(value);
}

/* Adapter — bolo's screenTanksGetItem returns playerNum (a single
 * BYTE), but logviewer's lv_screenTanksGetItem returns
 * (team, dir, onBoat) instead. The logviewer's screenTanks slot
 * does not carry playerNum at all, so we cannot recover it.
 *
 * mapViewDrawTanks (mapview.c:172) — the only Draw* caller — captures
 * playerNum into a local but never reads it (sdl3DrawTankLabels does
 * the label work separately). So hardcoding *playerNum = 0 here is
 * safe for the current call site.
 *
 * HAZARD: Phase D's tank-label step (plans/ctrailer.md Phase D
 * step 14, "slot != cameraSlot" check) will need real per-tank slot
 * information. When Phase D arrives, decide between (a) walking
 * lv_playersIsInUse + map-coord matching, or (b) extending
 * logviewer's screenTanks slot to carry playerNum and updating
 * lv_screenTanksAddItem/GetItem. Do NOT silently rely on this
 * adapter's playerNum=0 to stand up label rendering. */
void screenTanksGetItem(screenTanks *value, BYTE itemNum,
                        BYTE *mx, BYTE *my, BYTE *px, BYTE *py,
                        BYTE *frame, BYTE *playerNum, char *playerName) {
  BYTE team = 0, dir = 0;
  bool onBoat = FALSE;
  lv_screenTanksGetItem(value, itemNum, mx, my, px, py, frame,
                        &team, &dir, &onBoat, playerName);
  if (playerNum) *playerNum = 0;
}

BYTE screenLgmGetNumEntries(screenLgm *value) {
  return lv_screenLgmGetNumEntries(value);
}

void screenLgmGetItem(screenLgm *value, BYTE itemNum,
                      BYTE *mx, BYTE *my, BYTE *px, BYTE *py,
                      BYTE *frame) {
  lv_screenLgmGetItem(value, itemNum, mx, my, px, py, frame);
}

/* mapview.c (shared with main game) calls the main-game
 * playersScreenAllience(); forward to the LogViewer's lv_
 * equivalent which uses module-globals for plrs/selfPlayer. */
tankAlliance playersScreenAllience(players *plrs, BYTE selfPlayer,
                                   BYTE playerNum) {
  (void)plrs;
  (void)selfPlayer;
  return lv_playersScreenAllience(playerNum);
}


/* ====================================================================
 * Part 2 — abort stubs (linker-only).
 *
 * Never called at runtime — see plans/ctrailer.md Phase B.
 * mapView{Centered,BuildTileBuffer,CalcSquare} are not invoked from
 * the logviewer; abort() makes any future accidental call obvious.
 *
 * Forward-declared opaque types (bolo-only — not in logviewer):
 *   struct GameSim, struct tankObj (bolo's `tank`), struct minesObj
 *   (bolo's `mines`).
 * The logviewer does have parallel `bases`, `pillboxes`, `map`, and
 * `baseAlliance` types — these come in via the includes above and are
 * used here only as pointers/enum-values; bodies never deref or
 * compare. ABI-compatible with the bolo definitions at the call site
 * because pointer/enum widths match.
 * ==================================================================== */

struct GameSim;
struct tankObj;
struct minesObj;

bool basesExistPos(bases *value, BYTE xValue, BYTE yValue) {
  (void)value; (void)xValue; (void)yValue;
  abort();
}

baseAlliance basesGetAlliancePos(struct GameSim *sim, BYTE xValue, BYTE yValue) {
  (void)sim; (void)xValue; (void)yValue;
  abort();
}

bool basesAmOwner(struct GameSim *sim, BYTE owner, BYTE xValue, BYTE yValue) {
  (void)sim; (void)owner; (void)xValue; (void)yValue;
  abort();
}

bool pillsExistPos(pillboxes *value, BYTE xValue, BYTE yValue) {
  (void)value; (void)xValue; (void)yValue;
  abort();
}

BYTE pillsGetScreenHealth(struct GameSim *sim, pillboxes *value,
                          BYTE xValue, BYTE yValue) {
  (void)sim; (void)value; (void)xValue; (void)yValue;
  abort();
}

BYTE mapGetPos(map *value, BYTE xValue, BYTE yValue) {
  (void)value; (void)xValue; (void)yValue;
  abort();
}

bool mapIsMine(map *value, BYTE xValue, BYTE yValue) {
  (void)value; (void)xValue; (void)yValue;
  abort();
}

bool minesExistPos(struct minesObj **visMines, map *mp,
                   BYTE xValue, BYTE yValue) {
  (void)visMines; (void)mp; (void)xValue; (void)yValue;
  abort();
}

BYTE screenCalcRoad(BYTE aboveLeft, BYTE above, BYTE aboveRight,
                    BYTE left, BYTE right,
                    BYTE belowLeft, BYTE below, BYTE belowRight) {
  (void)aboveLeft; (void)above; (void)aboveRight;
  (void)left; (void)right;
  (void)belowLeft; (void)below; (void)belowRight;
  abort();
}

BYTE screenCalcBuilding(BYTE aboveLeft, BYTE above, BYTE aboveRight,
                        BYTE left, BYTE right,
                        BYTE belowLeft, BYTE below, BYTE belowRight) {
  (void)aboveLeft; (void)above; (void)aboveRight;
  (void)left; (void)right;
  (void)belowLeft; (void)below; (void)belowRight;
  abort();
}

BYTE screenCalcForest(BYTE aboveLeft, BYTE above, BYTE aboveRight,
                      BYTE left, BYTE right,
                      BYTE belowLeft, BYTE below, BYTE belowRight) {
  (void)aboveLeft; (void)above; (void)aboveRight;
  (void)left; (void)right;
  (void)belowLeft; (void)below; (void)belowRight;
  abort();
}

BYTE screenCalcRiver(BYTE aboveLeft, BYTE above, BYTE aboveRight,
                     BYTE left, BYTE right,
                     BYTE belowLeft, BYTE below, BYTE belowRight) {
  (void)aboveLeft; (void)above; (void)aboveRight;
  (void)left; (void)right;
  (void)belowLeft; (void)below; (void)belowRight;
  abort();
}

BYTE screenCalcDeepSea(BYTE aboveLeft, BYTE above, BYTE aboveRight,
                       BYTE left, BYTE right,
                       BYTE belowLeft, BYTE below, BYTE belowRight) {
  (void)aboveLeft; (void)above; (void)aboveRight;
  (void)left; (void)right;
  (void)belowLeft; (void)below; (void)belowRight;
  abort();
}

BYTE screenCalcBoat(BYTE aboveLeft, BYTE above, BYTE aboveRight,
                    BYTE left, BYTE right,
                    BYTE belowLeft, BYTE below, BYTE belowRight) {
  (void)aboveLeft; (void)above; (void)aboveRight;
  (void)left; (void)right;
  (void)belowLeft; (void)below; (void)belowRight;
  abort();
}

BYTE screenCalcCrater(BYTE aboveLeft, BYTE above, BYTE aboveRight,
                      BYTE left, BYTE right,
                      BYTE belowLeft, BYTE below, BYTE belowRight) {
  (void)aboveLeft; (void)above; (void)aboveRight;
  (void)left; (void)right;
  (void)belowLeft; (void)below; (void)belowRight;
  abort();
}

int tankGetDeathWait(struct tankObj **value) {
  (void)value;
  abort();
}

BYTE tankGetDir(struct tankObj **value) {
  (void)value;
  abort();
}

void tankGetWorld(struct tankObj **value, WORLD *x, WORLD *y) {
  (void)value; (void)x; (void)y;
  abort();
}

bool tankIsOnBoat(struct tankObj **value) {
  (void)value;
  abort();
}

BYTE utilGetDir(TURNTYPE value) {
  (void)value;
  abort();
}
