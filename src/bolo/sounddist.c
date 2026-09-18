/*
 * $Id$
 *
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
#include "tank.h"
#include "game_sim.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "players.h"
#include "frontend.h"
#include "sounddist.h"
#include "input_packet.h"

/*********************************************************
*NAME:          clientSoundDist
*AUTHOR:        John Morrison
*CREATION DATE: 19/01/99
*LAST MODIFIED: 09/09/26
*PURPOSE:
*  Plays the near or the far variant of a sound, whichever
*  the tier the server sent names. The server has already
*  dropped anything out of earshot, so there is no range
*  test here.
*
*ARGUMENTS:
*  sim   - Sim the sound is played against
*  value - Sound effect to be played
*  tier  - SOUND_TIER_NEAR or SOUND_TIER_FAR
*  dir   - Map-absolute bearing to the sound
*********************************************************/
void clientSoundDist(GameSim *sim, sndEffects value, BYTE tier, BYTE dir) {
  struct ClientSim *cs = clientSimFromSim(sim);

  /* The bearing a stereo panner would pan on. Nothing reads it yet. */
  (void)dir;

  /* Determine whether loud/soft sound should be played */
  switch (value) {
    case shootNear:
      if (tier == SOUND_TIER_FAR) {
        frontEndPlaySound(cs, shootFar);
      } else {
        frontEndPlaySound(cs, shootNear);
      }
      break;
    case shotTreeNear:
      if (tier == SOUND_TIER_FAR) {
        frontEndPlaySound(cs, shotTreeFar);
      } else {
        frontEndPlaySound(cs, shotTreeNear);
      }
      break;
    case shotBuildingNear:
      if (tier == SOUND_TIER_FAR) {
        frontEndPlaySound(cs, shotBuildingFar);
      } else {
        frontEndPlaySound(cs, shotBuildingNear);
      }
      break;
    case hitTankNear:
      if (tier == SOUND_TIER_FAR) {
        frontEndPlaySound(cs, hitTankFar);
      } else {
        frontEndPlaySound(cs, hitTankNear);
      }
      break;
    case bubbles:
      frontEndPlaySound(cs, bubbles);
      break;
    case tankSinkNear:
      if (tier == SOUND_TIER_FAR) {
        frontEndPlaySound(cs, tankSinkFar);
      } else {
        frontEndPlaySound(cs, tankSinkNear);
      }
      break;
    case bigExplosionNear:
      if (tier == SOUND_TIER_FAR) {
        frontEndPlaySound(cs, bigExplosionFar);
      } else {
        frontEndPlaySound(cs, bigExplosionNear);
      }
      break;
    case farmingTreeNear:
      if (tier == SOUND_TIER_FAR) {
        frontEndPlaySound(cs, farmingTreeFar);
      } else {
        frontEndPlaySound(cs, farmingTreeNear);
      }
      break;
    case manBuildingNear:
      if (tier == SOUND_TIER_FAR) {
        frontEndPlaySound(cs, manBuildingFar);
      } else {
        frontEndPlaySound(cs, manBuildingNear);
      }
      break;
    case manDyingNear:
      if (tier == SOUND_TIER_FAR) {
        frontEndPlaySound(cs, manDyingFar);
      } else {
        frontEndPlaySound(cs, manDyingNear);
      }
      break;
    case mineExplosionNear:
      if (tier == SOUND_TIER_FAR) {
        frontEndPlaySound(cs, mineExplosionFar);
      } else {
        frontEndPlaySound(cs, mineExplosionNear);
      }
      break;
    case manLayingMineNear:
      frontEndPlaySound(cs, manLayingMineNear);
      break;
    case shootSelf:
    case shotTreeFar:
    case shotBuildingFar:
    case hitTankFar:
    case hitTankSelf:
    case manDyingFar:
    case mineExplosionFar:
    case tankSinkFar:
    case bigExplosionFar:
    case farmingTreeFar:
    case manBuildingFar:
    case shootFar:
    default:
      break;
  }
}

/*********************************************************
*NAME:          clientSoundDistLocal
*AUTHOR:        John Morrison
*CREATION DATE: 19/01/99
*LAST MODIFIED: 09/09/26
*PURPOSE:
*  Measures a sound this client's own sim raised against the
*  listener's tank, turns the distance into a tier and plays
*  it. Sounds past SDIST_NONE are dropped.
*
*ARGUMENTS:
*  sim   - Sim the sound is played against
*  value - Sound effect to be played
*  mx    - Map X co-ordinatate for the sound origin
*  my    - Map Y co-ordinatate for the sound origin
*********************************************************/
void clientSoundDistLocal(GameSim *sim, sndEffects value, BYTE mx, BYTE my) {
  BYTE tankX; /* Tank X Map Co-ordinate */
  BYTE tankY; /* Tank Y Map Co-ordinate */
  BYTE gapX;  /* Distance from tank to sound */
  BYTE gapY;
  BYTE tier;  /* Band the gap puts the sound in */
  BYTE self = sim->viewPlayer;

  if (self >= MAX_TANKS || sim->tanks[self] == NULL) return;
  tankX = tankGetScreenMX(&sim->tanks[self]);
  tankY = tankGetScreenMY(&sim->tanks[self]);

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

  if (gapY >= sim->rules.sound_none_range ||
      gapX >= sim->rules.sound_none_range) {
    return;
  }

  if (gapX <= sim->rules.sound_soft_range &&
      gapY <= sim->rules.sound_soft_range) {
    tier = SOUND_TIER_NEAR;
  } else {
    tier = SOUND_TIER_FAR;
  }

  /* Neither bubbles nor manLayingMineNear has a far variant, so a far one is
     silence. Both server delivery paths drop those before sending; this one
     drops them before playing. */
  if (tier == SOUND_TIER_FAR &&
      (value == bubbles || value == manLayingMineNear)) {
    return;
  }

  /* No bearing worked out here: nothing reads one. */
  clientSoundDist(sim, value, tier, SOUND_DIR_CENTRE);
}
