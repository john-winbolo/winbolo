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
 *Name:          ViewPort
 *Filename:      viewport.c
 *Purpose:
 *  Camera / visible-tile math. Operates on the ViewPort
 *  substruct embedded in ClientSim. Sources of non-viewport
 *  state (GameSim, ScrollState, the local tank, brain map)
 *  are taken as explicit parameters so the math here has no
 *  knowledge of ClientSim's full layout.
 *********************************************************/

#include "global.h"
#include "viewport.h"
#include "client_command.h"   /* ViewStateKind — the ViewPort's viewKind values */
#include "client_sim_internal.h"
#include "game_sim.h"
#include "bolo_map.h"
#include "bases.h"
#include "pillbox.h"
#include "players.h"
#include "mines.h"
#include "tank.h"
#include "scroll.h"
#include "screencalc.h"
#include "screenbrainmap.h"
#include "tilenum.h"
#include "gametype.h"

void viewportInit(ViewPort *vp) {
  New(vp->view);
  New(vp->mineView);
  vp->xOffset = 0;
  vp->yOffset = 0;
  vp->cursorPosX = -1;
  vp->cursorPosY = -1;
  vp->needRecalc = FALSE;
  vp->viewKind = VIEW_KIND_TANK;
  vp->viewTarget = 0;
  vp->viewX = 0;
  vp->viewY = 0;
}

void viewportDestroy(ViewPort *vp) {
  if (vp->view != NULL) {
    Dispose(vp->view);
    vp->view = NULL;
  }
  if (vp->mineView != NULL) {
    Dispose(vp->mineView);
    vp->mineView = NULL;
  }
}

void viewportRecalc(ViewPort *vp) {
  vp->needRecalc = TRUE;
}

void viewportUpdateView(ViewPort *vp, struct GameSim *sim, BYTE myPlayerNum,
                        BYTE brainMap[][MAP_ARRAY_SIZE], updateType value) {
  /* NOTE: value UNUSED */
  BYTE count;
  BYTE count2;
  (void)value;

  for (count = 0; count < MAIN_BACK_BUFFER_SIZE_X; count++) {
    for (count2 = 0; count2 < MAIN_BACK_BUFFER_SIZE_Y; count2++) {
      vp->view->screenItem[count][count2] =
          viewportCalcSquare(vp, sim, myPlayerNum,
                             (BYTE)(count + vp->xOffset),
                             (BYTE)(count2 + vp->yOffset), count, count2);
      screenBrainMapSetPos(brainMap,
                           (BYTE)(count + vp->xOffset),
                           (BYTE)(count2 + vp->yOffset),
                           mapGetPos(&sim->mp, (BYTE)(count + vp->xOffset),
                                     (BYTE)(count2 + vp->yOffset)),
                           minesExistPos(&sim->mns, &sim->mp,
                                         (BYTE)(count + vp->xOffset),
                                         (BYTE)(count2 + vp->yOffset)));
    }
  }
}

/* mapIsMine over a caller-supplied terrain byte, so a locally normalised
 * value can be tested without writing it back to the map. */
static bool viewportSquareIsMine(BYTE xValue, BYTE yValue, BYTE terrain) {
  bool returnValue; /* Value to return */

  returnValue = FALSE;
  if (xValue <= MAP_MINE_EDGE_LEFT || xValue >= MAP_MINE_EDGE_RIGHT ||
      yValue <= MAP_MINE_EDGE_TOP || yValue >= MAP_MINE_EDGE_BOTTOM) {
    returnValue = TRUE;
  } else if (terrain >= MINE_START && terrain <= MINE_END) {
    returnValue = TRUE;
  }
  return returnValue;
}

/* minesExistPos over a caller-supplied terrain byte; cleared stands in for a
 * minesRemoveItem the caller only modelled. */
static bool viewportSquareMineVisible(mines *visMines, BYTE xValue, BYTE yValue,
                                      BYTE terrain, bool cleared) {
  bool returnValue; /* Value to return */

  if (xValue <= MAP_MINE_EDGE_LEFT || xValue >= MAP_MINE_EDGE_RIGHT ||
      yValue <= MAP_MINE_EDGE_TOP || yValue >= MAP_MINE_EDGE_BOTTOM) {
    returnValue = TRUE;
  } else if ((*visMines)->minesHiddenMines == TRUE) {
    if (cleared == TRUE) {
      returnValue = FALSE;
    } else {
      returnValue = (*visMines)->pos[xValue][yValue];
    }
  } else {
    returnValue = (bool)(terrain >= MINE_START && terrain <= MINE_END);
  }
  return returnValue;
}

BYTE viewportCalcSquarePure(struct GameSim *sim, BYTE myPlayerNum,
                            BYTE xValue, BYTE yValue, bool *isMine) {
  baseAlliance ba;
  BYTE returnValue;
  BYTE currentPos;
  bool normalised;
  BYTE aboveLeft;
  BYTE above;
  BYTE aboveRight;
  BYTE leftPos;
  BYTE rightPos;
  BYTE belowLeft;
  BYTE below;
  BYTE belowRight;

  *isMine = FALSE;
  /* Set up Items */
  /* The remembered answer: a pill this client has lost sight of keeps its tile
   * at the square it was last seen on rather than showing the ground under it.
   * It is no longer solid — that is pillsExistPos's job, which the movement and
   * collision paths ask. */
  if ((pillsViewExistPos(&sim->pb, xValue, yValue)) == TRUE) {
    returnValue = pillsGetScreenHealth(sim, &sim->pb, xValue, yValue, myPlayerNum);
  } else if ((basesExistPos(&sim->bs, xValue, yValue)) == TRUE) {
    ba = basesGetAlliancePos(sim, xValue, yValue, myPlayerNum);
    switch (ba) {
    case baseOwnGood:
      returnValue = BASE_GOOD;
      break;
    case baseAllieGood:
      returnValue = BASE_GOOD;
      break;
    case baseNeutral:
      returnValue = BASE_NEUTRAL;
      break;
    case baseDead:
      if (basesAmOwner(sim, myPlayerNum, xValue, yValue) == TRUE) {
        returnValue = BASE_GOOD;
      } else {
        returnValue = BASE_EVIL;
      }
      break;
    case baseEvil:
    default:
      /* Base Evil */
      returnValue = BASE_EVIL;
    }
  } else {
    currentPos = mapGetPos(&sim->mp, xValue, yValue);
    normalised = FALSE;
    if (currentPos >= HALFBUILDING + MINE_SUBTRACT && currentPos != DEEP_SEA) {
      currentPos = (BYTE)(currentPos - MINE_SUBTRACT);
      normalised = TRUE;
    }
    if (viewportSquareIsMine(xValue, yValue, currentPos) == TRUE) {
      if (viewportSquareMineVisible(&sim->mns, xValue, yValue, currentPos,
                                    normalised) == TRUE) {
        *isMine = TRUE;
      }
      if (currentPos != DEEP_SEA) {
        currentPos = (BYTE)(currentPos - MINE_SUBTRACT);
      }
    } else {
      *isMine = FALSE;
    }

    if (basesExistPos(&sim->bs, (BYTE)(xValue - 1), (BYTE)(yValue - 1)) == TRUE) {
      aboveLeft = ROAD;
    } else {
      aboveLeft = mapGetPos(&sim->mp, (BYTE)(xValue - 1), (BYTE)(yValue - 1));
      if (aboveLeft >= MINE_START && aboveLeft <= MINE_END) {
        aboveLeft = aboveLeft - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&sim->bs, xValue, (BYTE)(yValue - 1)) == TRUE) {
      above = ROAD;
    } else {
      above = mapGetPos(&sim->mp, xValue, (BYTE)(yValue - 1));
      if (above >= MINE_START && above <= MINE_END) {
        above = above - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&sim->bs, (BYTE)(xValue + 1), (BYTE)(yValue - 1)) == TRUE) {
      aboveRight = ROAD;
    } else {
      aboveRight = mapGetPos(&sim->mp, (BYTE)(xValue + 1), (BYTE)(yValue - 1));
      if (aboveRight >= MINE_START && aboveRight <= MINE_END) {
        aboveRight = aboveRight - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&sim->bs, (BYTE)(xValue - 1), yValue) == TRUE) {
      leftPos = ROAD;
    } else {
      leftPos = mapGetPos(&sim->mp, (BYTE)(xValue - 1), yValue);
      if (leftPos >= MINE_START && leftPos <= MINE_END) {
        leftPos = leftPos - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&sim->bs, (BYTE)(xValue + 1), yValue) == TRUE) {
      rightPos = ROAD;
    } else {
      rightPos = mapGetPos(&sim->mp, (BYTE)(xValue + 1), yValue);
      if (rightPos >= MINE_START && rightPos <= MINE_END) {
        rightPos = rightPos - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&sim->bs, (BYTE)(xValue - 1), (BYTE)(yValue + 1)) == TRUE) {
      belowLeft = ROAD;
    } else {
      belowLeft = mapGetPos(&sim->mp, (BYTE)(xValue - 1), (BYTE)(yValue + 1));
      if (belowLeft >= MINE_START && belowLeft <= MINE_END) {
        belowLeft = belowLeft - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&sim->bs, xValue, (BYTE)(yValue + 1)) == TRUE) {
      below = ROAD;
    } else {
      below = mapGetPos(&sim->mp, xValue, (BYTE)(yValue + 1));
      if (below >= MINE_START && below <= MINE_END) {
        below = below - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&sim->bs, (BYTE)(xValue + 1), (BYTE)(yValue + 1)) == TRUE) {
      belowRight = ROAD;
    } else {
      belowRight = mapGetPos(&sim->mp, (BYTE)(xValue + 1), (BYTE)(yValue + 1));
      if (belowRight >= MINE_START && belowRight <= MINE_END) {
        belowRight = belowRight - MINE_SUBTRACT;
      }
    }

    switch (currentPos) {
    case ROAD:
      returnValue = screenCalcRoad(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case BUILDING:
      returnValue = screenCalcBuilding(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case FOREST:
      returnValue = screenCalcForest(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case RIVER:
      returnValue = screenCalcRiver(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case DEEP_SEA:
      returnValue = screenCalcDeepSea(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case BOAT:
      returnValue = screenCalcBoat(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case CRATER:
      returnValue = screenCalcCrater(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    default:
      returnValue = currentPos;
      break;
    }
  }
  return returnValue;
}

BYTE viewportCalcSquare(ViewPort *vp, struct GameSim *sim, BYTE myPlayerNum,
                        BYTE xValue, BYTE yValue, BYTE scrX, BYTE scrY) {
  bool isMine = FALSE;
  BYTE result;
  BYTE currentPos;

  result = viewportCalcSquarePure(sim, myPlayerNum, xValue, yValue, &isMine);
  vp->mineView->mineItem[scrX][scrY] = isMine;

  /* Repair a malformed terrain byte; the view is the only path that
   * normalises these. The tile above was computed from the normalised
   * value already, so the write-back only fixes the stored map. */
  if (pillsViewExistPos(&sim->pb, xValue, yValue) == FALSE &&
      basesExistPos(&sim->bs, xValue, yValue) == FALSE) {
    currentPos = mapGetPos(&sim->mp, xValue, yValue);
    if (currentPos >= HALFBUILDING + MINE_SUBTRACT && currentPos != DEEP_SEA) {
      minesRemoveItem(&sim->mns, xValue, yValue);
      mapSetPos(sim, &sim->mp, xValue, yValue,
                (BYTE)(currentPos - MINE_SUBTRACT), TRUE, TRUE);
    }
  }

  return result;
}

void viewportPanX(ViewPort *vp, int dxTiles) {
  vp->xOffset = (BYTE)(vp->xOffset + dxTiles);
}

void viewportPanY(ViewPort *vp, int dyTiles) {
  vp->yOffset = (BYTE)(vp->yOffset + dyTiles);
}

void viewportSetTankView(ViewPort *vp) {
  vp->viewKind = VIEW_KIND_TANK;
  vp->viewTarget = 0;
}

void viewportFollowTank(ViewPort *vp, ScrollState *scroll, tank myTank) {
  viewportSetTankView(vp);
  viewportCenterOnTank(vp, scroll, myTank);
}

/* The pill and base index a square holds, as the 0-based number the server's
 * CMD_VIEW_STATE uses. Only ever called for a square a cycling helper has just
 * reported, so the not-found case is unreachable; 0 stands in for it. */
static BYTE viewportPillIndexAt(struct GameSim *sim, BYTE mx, BYTE my) {
  BYTE pillNum = pillsGetViewPillNum(&sim->pb, mx, my, FALSE, FALSE);
  return (BYTE)(pillNum == PILL_NOT_FOUND ? 0 : pillNum - 1);
}

static BYTE viewportBaseIndexAt(struct GameSim *sim, BYTE mx, BYTE my) {
  BYTE baseNum = basesGetBaseNum(&sim->bs, mx, my);
  return (BYTE)(baseNum == BASE_NOT_FOUND ? 0 : baseNum - 1);
}

void viewCycleInputsDefaults(ViewCycleInputs *in) {
  int cat; /* Looping variable */

  if (in == NULL) {
    return;
  }
  memset(in, 0, sizeof(*in));
  for (cat = 0; cat < VIEW_CATEGORY_COUNT; cat++) {
    in->eligible[cat] = ~(PlayerBitMap)0;
  }
}

/* The eligibility mask for the category this kind of view cycles through. */
static PlayerBitMap viewportEligibleMask(const ViewCycleInputs *in,
                                         uint8_t kind) {
  switch (kind) {
  case VIEW_KIND_PILL:
    return in->eligible[viewCategoryPill];
  case VIEW_KIND_BASE:
    return in->eligible[viewCategoryBase];
  case VIEW_KIND_ALLY:
    return in->eligible[viewCategoryAlly];
  default:
    return 0;
  }
}

/* Whether one item of this kind may be selected right now. */
static bool viewportItemEligible(const ViewCycleInputs *in, uint8_t kind,
                                 BYTE index) {
  PlayerBitMap mask = viewportEligibleMask(in, kind);

  return (mask & ((PlayerBitMap)1 << index)) != 0;
}

/* Where an ally is, as the view should centre on it. A tank outside our
 * viewport arrives as a hidden stub, which zeroes its players entry so the
 * renderer stops drawing the last in-view position as a ghost; the square the
 * client last actually saw it on is kept separately, and that is what the
 * view falls back to rather than the map origin. */
static void viewportAllySquare(struct GameSim *sim, const ViewCycleInputs *in,
                               BYTE target, BYTE *x, BYTE *y) {
  BYTE mx = (*sim->plyrs).item[target].mapX;
  BYTE my = (*sim->plyrs).item[target].mapY;

  if (mx == 0 && my == 0 && in->allyLastMapX != NULL &&
      in->allyLastMapY != NULL) {
    mx = in->allyLastMapX[target];
    my = in->allyLastMapY[target];
  }
  *x = mx;
  *y = my;
}

/* Is the item the view is parked on still watchable? A pill or base is
 * identified by its square, so the index is re-derived from it; an ally is
 * identified by its player number, and its x/y are refreshed to where it is
 * now so the view does not centre on a square it has driven away from. An
 * item whose decay clock has run out is no longer watchable either: the
 * server has stopped sending its squares. */
static bool viewportItemCheck(struct GameSim *sim, uint8_t kind,
                              const ViewCycleInputs *in,
                              BYTE *target, BYTE *x, BYTE *y) {
  switch (kind) {
  case VIEW_KIND_PILL:
    if (pillsCheckView(sim, &sim->pb, *x, *y) == FALSE) {
      return FALSE;
    }
    *target = viewportPillIndexAt(sim, *x, *y);
    return viewportItemEligible(in, kind, *target);
  case VIEW_KIND_BASE:
    if (basesCheckView(sim, &sim->bs, *x, *y) == FALSE) {
      return FALSE;
    }
    *target = viewportBaseIndexAt(sim, *x, *y);
    return viewportItemEligible(in, kind, *target);
  case VIEW_KIND_ALLY:
    if (playersCanAllyView(sim, in->allyViewable, *target) == FALSE ||
        viewportItemEligible(in, kind, *target) == FALSE) {
      return FALSE;
    }
    viewportAllySquare(sim, in, *target, x, y);
    return TRUE;
  default:
    return FALSE;
  }
}

/* Step to the next watchable item of this kind, wrapping when prev is set. */
static bool viewportItemNext(struct GameSim *sim, uint8_t kind,
                             const ViewCycleInputs *in,
                             BYTE *target, BYTE *x, BYTE *y, bool prev) {
  bool result;

  switch (kind) {
  case VIEW_KIND_PILL:
    result = pillsGetNextView(sim, &sim->pb, viewportEligibleMask(in, kind),
                              x, y, prev);
    if (result == TRUE) {
      *target = viewportPillIndexAt(sim, *x, *y);
    }
    return result;
  case VIEW_KIND_BASE:
    result = basesGetNextView(sim, &sim->bs, viewportEligibleMask(in, kind),
                              x, y, prev);
    if (result == TRUE) {
      *target = viewportBaseIndexAt(sim, *x, *y);
    }
    return result;
  case VIEW_KIND_ALLY:
    result = playersGetNextAllyView(sim, in->allyViewable,
                                    viewportEligibleMask(in, kind), target,
                                    x, y, prev);
    if (result == TRUE) {
      viewportAllySquare(sim, in, *target, x, y);
    }
    return result;
  default:
    return FALSE;
  }
}

/* Step to the nearest watchable item of this kind in the pressed direction. */
static bool viewportItemMove(struct GameSim *sim, uint8_t kind,
                             const ViewCycleInputs *in,
                             BYTE *target, BYTE *x, BYTE *y, int horz, int vert) {
  bool result;

  switch (kind) {
  case VIEW_KIND_PILL:
    result = pillsMoveView(sim, &sim->pb, viewportEligibleMask(in, kind),
                           x, y, horz, vert);
    if (result == TRUE) {
      *target = viewportPillIndexAt(sim, *x, *y);
    }
    return result;
  case VIEW_KIND_BASE:
    result = basesMoveView(sim, &sim->bs, viewportEligibleMask(in, kind),
                           x, y, horz, vert);
    if (result == TRUE) {
      *target = viewportBaseIndexAt(sim, *x, *y);
    }
    return result;
  case VIEW_KIND_ALLY:
    result = playersMoveAllyView(sim, in->allyViewable,
                                 viewportEligibleMask(in, kind), target, x, y,
                                 horz, vert);
    if (result == TRUE) {
      viewportAllySquare(sim, in, *target, x, y);
    }
    return result;
  default:
    return FALSE;
  }
}

/* Park the camera on an item and remember what it is. */
static void viewportEnterItemView(ViewPort *vp, ScrollState *scroll, uint8_t kind,
                                  BYTE target, BYTE x, BYTE y) {
  vp->viewKind = kind;
  vp->viewTarget = target;
  vp->viewX = x;
  vp->viewY = y;
  scrollCenterObject(scroll, &vp->xOffset, &vp->yOffset, x, y);
  viewportRecalc(vp);
}

void viewportEnterAllyView(ViewPort *vp, ScrollState *scroll, BYTE target,
                           BYTE x, BYTE y) {
  viewportEnterItemView(vp, scroll, VIEW_KIND_ALLY, target, x, y);
}

void viewportPanInView(ViewPort *vp, struct GameSim *sim, ScrollState *scroll,
                       tank myTank, uint8_t kind, const ViewCycleInputs *in,
                       int horz, int vert) {
  ViewCycleInputs fallback; /* Stands in for a caller with no inputs */
  bool result;
  BYTE target;
  BYTE x;
  BYTE y;

  if (in == NULL) {
    viewCycleInputsDefaults(&fallback);
    in = &fallback;
  }

  target = vp->viewTarget;
  x = vp->viewX;
  y = vp->viewY;

  if (vp->viewKind != kind) {
    /* Entering this kind of view from the tank: resume on the item we were
     * last parked on if it is still watchable, otherwise take the first one
     * there is. Switching straight from another kind of item view always
     * starts that kind's cycle at its first item — the remembered square and
     * target belong to the kind being left, not this one. */
    if (vp->viewKind == VIEW_KIND_TANK &&
        viewportItemCheck(sim, kind, in, &target, &x, &y) == TRUE) {
      viewportEnterItemView(vp, scroll, kind, target, x, y);
    } else {
      result = viewportItemNext(sim, kind, in, &target, &x, &y, FALSE);
      if (result == TRUE) {
        /* Center on the object */
        viewportEnterItemView(vp, scroll, kind, target, x, y);
      } else {
        /* Nothing of this kind to watch — back to the tank. The camera is
         * left where it is; the per-tick tank follow brings it back. */
        viewportSetTankView(vp);
      }
    }
  } else {
    if (horz == 0 && vert == 0) {
      result = viewportItemNext(sim, kind, in, &target, &x, &y, TRUE);
      if (result == FALSE) {
        viewportFollowTank(vp, scroll, myTank);
      } else {
        /* Center on the object */
        viewportEnterItemView(vp, scroll, kind, target, x, y);
      }
    } else {
      if (viewportItemMove(sim, kind, in, &target, &x, &y, horz, vert) == TRUE) {
        viewportEnterItemView(vp, scroll, kind, target, x, y);
      }
    }
  }
}

void viewportPanInPillView(ViewPort *vp, struct GameSim *sim, ScrollState *scroll,
                           tank myTank, int horz, int vert) {
  ViewCycleInputs in; /* No policy in play: every pill eligible */

  viewCycleInputsDefaults(&in);
  viewportPanInView(vp, sim, scroll, myTank, VIEW_KIND_PILL, &in, horz, vert);
}

bool viewportUpdateItemView(ViewPort *vp, struct GameSim *sim, ScrollState *scroll,
                            const ViewCycleInputs *in) {
  ViewCycleInputs fallback; /* Stands in for a caller with no inputs */
  BYTE target;
  BYTE x;
  BYTE y;

  if (vp->viewKind == VIEW_KIND_TANK) {
    return TRUE;
  }

  if (in == NULL) {
    viewCycleInputsDefaults(&fallback);
    in = &fallback;
  }

  target = vp->viewTarget;
  x = vp->viewX;
  y = vp->viewY;
  if (viewportItemCheck(sim, vp->viewKind, in, &target, &x, &y) == FALSE) {
    return FALSE;
  }

  /* A pill or base never moves, so only an ally view has anything to do here:
   * re-centre on where the ally has driven to since the last tick. The pill
   * and base kinds still take the index the check re-derived from the square,
   * so what gets reported to the server keeps matching what is on screen. */
  vp->viewTarget = target;
  if (vp->viewKind == VIEW_KIND_ALLY && (x != vp->viewX || y != vp->viewY)) {
    viewportEnterItemView(vp, scroll, vp->viewKind, target, x, y);
  }
  return TRUE;
}

void viewportSetCursor(ViewPort *vp, BYTE posX, BYTE posY) {
  if (posX != 0 && posY != 0) {
    vp->cursorPosX = posX;
    vp->cursorPosY = posY;
  }
}

bool viewportGetCursor(const ViewPort *vp, BYTE *posX, BYTE *posY) {
  bool returnValue;

  returnValue = FALSE;
  if (vp->cursorPosX >= 0 && vp->cursorPosX <= MAIN_SCREEN_SIZE_X &&
      vp->cursorPosY >= 0 && vp->cursorPosY <= MAIN_SCREEN_SIZE_Y) {
    returnValue = TRUE;
    *posX = (BYTE)vp->cursorPosX;
    *posY = (BYTE)vp->cursorPosY;
  } else {
    *posX = 0;
    *posY = 0;
  }

  return returnValue;
}

void viewportCenterOnTank(ViewPort *vp, ScrollState *scroll, tank myTank) {
  BYTE high, low, health, dummy;

  tankGetStats(&myTank, &high, &low, &health, &dummy);
  if (health <= TANK_FULL_ARMOUR) {
    /* Tank isn't dead */
    scrollCenterObject(scroll, &vp->xOffset, &vp->yOffset,
                       tankGetMX(&myTank), tankGetMY(&myTank));
    viewportRecalc(vp);
  }
}
