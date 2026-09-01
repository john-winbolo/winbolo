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
#include "client_sim_internal.h"
#include "game_sim.h"
#include "bolo_map.h"
#include "bases.h"
#include "pillbox.h"
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
  vp->inPillView = FALSE;
  vp->pillViewX = 0;
  vp->pillViewY = 0;
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
  if ((pillsExistPos(&sim->pb, xValue, yValue)) == TRUE) {
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
  if (pillsExistPos(&sim->pb, xValue, yValue) == FALSE &&
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

void viewportFollowTank(ViewPort *vp, ScrollState *scroll, tank myTank) {
  vp->inPillView = FALSE;
  viewportCenterOnTank(vp, scroll, myTank);
}

void viewportPanInPillView(ViewPort *vp, struct GameSim *sim, ScrollState *scroll,
                           tank myTank, int horz, int vert) {
  bool result;

  if (vp->inPillView == FALSE) {
    if (pillsCheckView(sim, &sim->pb, vp->pillViewX, vp->pillViewY) == TRUE) {
      vp->inPillView = TRUE;
      scrollCenterObject(scroll, &vp->xOffset, &vp->yOffset, vp->pillViewX, vp->pillViewY);
      viewportRecalc(vp);
    } else {
      result = pillsGetNextView(sim, &sim->pb, &vp->pillViewX, &vp->pillViewY, FALSE);
      if (result == TRUE) {
        /* Center on the object */
        vp->inPillView = TRUE;
        scrollCenterObject(scroll, &vp->xOffset, &vp->yOffset, vp->pillViewX, vp->pillViewY);
        viewportRecalc(vp);
      } else {
        vp->inPillView = FALSE;
      }
    }
  } else {
    if (horz == 0 && vert == 0) {
      result = pillsGetNextView(sim, &sim->pb, &vp->pillViewX, &vp->pillViewY, TRUE);
      if (result == FALSE) {
        viewportFollowTank(vp, scroll, myTank);
      } else {
        /* Center on the object */
        scrollCenterObject(scroll, &vp->xOffset, &vp->yOffset, vp->pillViewX, vp->pillViewY);
        viewportRecalc(vp);
      }
    } else {
      if (pillsMoveView(sim, &sim->pb, &vp->pillViewX, &vp->pillViewY, horz, vert) == TRUE) {
        scrollCenterObject(scroll, &vp->xOffset, &vp->yOffset, vp->pillViewX, vp->pillViewY);
        viewportRecalc(vp);
      }
    }
  }
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
