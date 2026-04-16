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
*Name:          Sound Distancing
*Filename:      sounddist.c
*Author:        John Morrison
*Creation Date: 19/01/99
*Last Modified: 05/05/01
*Purpose:
*  Responsible for differentiating between playing soft
*  sound effects and loud sound effects.
*********************************************************/

#include "global.h"
#include "backend.h"
#include "sounddist.h"
#include "logviewer.h"

void lv_frontEndPlaySound(sndEffects value);

/*********************************************************
*NAME:          lv_soundDist
*AUTHOR:        John Morrison
*CREATION DATE: 19/01/99
*LAST MODIFIED: 05/05/01
*PURPOSE:
*  Calculates whether a soft sound of a loud sound should 
*  be played and passes paremeters to frontend
*
*ARGUMENTS:
*  value - Sound effect to be played
*  mx    - Map X co-ordinatate for the sound origin
*  my    - Map Y co-ordinatate for the sound origin
*********************************************************/
void lv_soundDist(sndEffects value, BYTE mx, BYTE my) {
  BYTE tankX = lv_screenGetXOffset(); /* Tank X Map Co-ordinate */
  BYTE tankY = lv_screenGetYOffset(); /* Tank Y Map Co-ordinate */
  BYTE gapX;  /* Distance from tank to sound */
  BYTE gapY;

  if (lv_screenGetFastForwarding() == TRUE) {
    return;
  }

  if (mineExplosionNear == value) {
    gapY = 0;
  }

  /* Get gap */
  if ((tankX - mx) < 0) {
    gapX = mx - tankX;
  } else {
    gapX = tankX - mx;
  }

  if ((tankY - my) < 0) {
    gapY = my - tankY;
  } else {
    gapY = tankY - my;
  }

  if (gapY < SDIST_NONE && gapX < SDIST_NONE) {
  /* Determine whether loud/soft sound should be played */
    switch (value) {
    case shootSelf:

    case shootNear:
      if (gapX > SDIST_SOFT || gapY > SDIST_SOFT) {
        lv_frontEndPlaySound(shootFar);
      } else { 
        lv_frontEndPlaySound(shootNear);
      }
      break;
    case shotTreeNear:
      if (gapX > SDIST_SOFT || gapY > SDIST_SOFT) {
        lv_frontEndPlaySound(shotTreeFar);
      } else { 
        lv_frontEndPlaySound(shotTreeNear);
      }
      break;
    case shotBuildingNear:
      if (gapX > SDIST_SOFT || gapY > SDIST_SOFT) {
        lv_frontEndPlaySound(shotBuildingFar);
      } else { 
        lv_frontEndPlaySound(shotBuildingNear);
      }
      break;
    case hitTankSelf:
    case hitTankNear:
      if (gapX > SDIST_SOFT || gapY > SDIST_SOFT) {
        lv_frontEndPlaySound(hitTankFar);
      } else { 
        lv_frontEndPlaySound(hitTankNear);
      }
      break;
    case tankSinkNear:
      if (gapX > SDIST_SOFT || gapY > SDIST_SOFT) {
        lv_frontEndPlaySound(tankSinkFar);
      } else { 
        lv_frontEndPlaySound(tankSinkNear);
      }
      break;
    case bigExplosionNear:
      if (gapX > SDIST_SOFT || gapY > SDIST_SOFT) {
        lv_frontEndPlaySound(bigExplosionFar);
      } else { 
        lv_frontEndPlaySound(bigExplosionNear);
      }
      break;
    case farmingTreeNear:
      if (gapX > SDIST_SOFT || gapY > SDIST_SOFT) {
        lv_frontEndPlaySound(farmingTreeFar);
      } else { 
        lv_frontEndPlaySound(farmingTreeNear);
      }
      break;
    case manBuildingNear:
      if (gapX > SDIST_SOFT || gapY > SDIST_SOFT) {
        lv_frontEndPlaySound(manBuildingFar);
      } else { 
        lv_frontEndPlaySound(manBuildingNear);
      }
      break;
    case manDyingNear:
      if (gapX > SDIST_SOFT || gapY > SDIST_SOFT) {
        lv_frontEndPlaySound(manDyingFar);
      } else { 
        lv_frontEndPlaySound(manDyingNear);
      }
      break;
    case mineExplosionNear:
      if (gapX > SDIST_SOFT || gapY > SDIST_SOFT) {
        lv_frontEndPlaySound(mineExplosionFar);
      } else { 
        lv_frontEndPlaySound(mineExplosionNear);
      }
      break;
    case manLayingMineNear:
      if (gapX <= SDIST_SOFT || gapY <= SDIST_SOFT) {
        lv_frontEndPlaySound(manLayingMineNear);
      }
      break;
    case shotTreeFar:
    case shotBuildingFar:
    case hitTankFar:
    case manDyingFar:
    case mineExplosionFar:
    case bubbles:
    case tankSinkFar:
    case bigExplosionFar:
    case farmingTreeFar:
    case manBuildingFar:
    case shootFar:
    default:
      break;
    }
  }
}
