/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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

#define PAN_WIDTH     SOUND_PAN_MAX /* 8 squares to full pan: just past the
                                       main screen half (7) */
#define PAN_DEPTH     179   /* Q8; 256-179 = 77, so 0.3 on the far channel */
#define DIST_GAIN_FAR 256   /* Q8 gain at SDIST_NONE; unity until tuned by ear */

/*********************************************************
*NAME:          soundDistGains
*AUTHOR:        John Morrison
*CREATION DATE: 26/09/26
*LAST MODIFIED: 26/09/26
*PURPOSE:
*  Works out the Q8 left and right gains for a sound pan
*  squares east (positive) or west (negative) of the
*  listener and dist squares away. The channel towards the
*  sound stays at unity and the other is ducked, reaching
*  PAN_DEPTH below unity at PAN_WIDTH, so no gain is ever
*  above unity. Past SDIST_SOFT both channels fall linearly
*  to DIST_GAIN_FAR at SDIST_NONE.
*
*ARGUMENTS:
*  pan   - East-west offset in squares, east positive
*  dist  - Larger-axis distance in squares
*  gainL - Destination for the left gain
*  gainR - Destination for the right gain
*********************************************************/
void soundDistGains(int8_t pan, BYTE dist, uint16_t *gainL, uint16_t *gainR) {
  int p;         /* Signed Q8 pan position, -256 west to 256 east */
  int duck;      /* Gain of the channel away from the sound */
  int panL;
  int panR;
  int d;
  int distGain;

  p = ((int)pan * 256) / PAN_WIDTH;
  if (p > 256) {
    p = 256;
  } else if (p < -256) {
    p = -256;
  }
  duck = 256 - ((PAN_DEPTH * abs(p)) >> 8);

  if (p > 0) {
    panL = duck;
    panR = 256;
  } else if (p < 0) {
    panL = 256;
    panR = duck;
  } else {
    panL = 256;
    panR = 256;
  }

  d = dist;
  if (d > SDIST_NONE) {
    d = SDIST_NONE;
  }
  if (d <= SDIST_SOFT) {
    distGain = 256;
  } else {
    distGain = 256 - ((256 - DIST_GAIN_FAR) * (d - SDIST_SOFT)) /
                     (SDIST_NONE - SDIST_SOFT);
  }

  *gainL = (uint16_t)((panL * distGain) >> 8);
  *gainR = (uint16_t)((panR * distGain) >> 8);
}

/*********************************************************
*NAME:          soundDistPositional
*AUTHOR:        John Morrison
*CREATION DATE: 27/09/26
*LAST MODIFIED: 27/09/26
*PURPOSE:
*  Returns whether sounds are played panned and attenuated
*  by their position. False while the server has positional
*  sound off, and every sound then plays centred at unity.
*
*ARGUMENTS:
*  sim - Sim the sound is played against
*********************************************************/
static bool soundDistPositional(GameSim *sim) {
  return clientSimGetPositionalSound(clientSimFromSim(sim));
}

/*********************************************************
*NAME:          soundDistPlay
*AUTHOR:        John Morrison
*CREATION DATE: 19/01/99
*LAST MODIFIED: 27/09/26
*PURPOSE:
*  Plays the near or the far variant of a sound, panned
*  and attenuated by soundDistGains, or at unity on both
*  channels while positional sound is off.
*
*ARGUMENTS:
*  sim   - Sim the sound is played against
*  value - Sound effect to be played
*  isFar - TRUE to play the far variant
*  pan   - East-west offset in squares, east positive
*  dist  - Larger-axis distance in squares
*********************************************************/
static void soundDistPlay(GameSim *sim, sndEffects value, bool isFar,
                          int8_t pan, BYTE dist) {
  struct ClientSim *cs = clientSimFromSim(sim);
  uint16_t gL;
  uint16_t gR;

  if (soundDistPositional(sim)) {
    soundDistGains(pan, dist, &gL, &gR);
  } else {
    gL = SOUND_GAIN_UNITY;
    gR = SOUND_GAIN_UNITY;
  }

  /* Determine whether loud/soft sound should be played */
  switch (value) {
    case shootNear:
      if (isFar) {
        frontEndPlaySoundPan(cs, shootFar, gL, gR);
      } else {
        frontEndPlaySoundPan(cs, shootNear, gL, gR);
      }
      break;
    case shotTreeNear:
      if (isFar) {
        frontEndPlaySoundPan(cs, shotTreeFar, gL, gR);
      } else {
        frontEndPlaySoundPan(cs, shotTreeNear, gL, gR);
      }
      break;
    case shotBuildingNear:
      if (isFar) {
        frontEndPlaySoundPan(cs, shotBuildingFar, gL, gR);
      } else {
        frontEndPlaySoundPan(cs, shotBuildingNear, gL, gR);
      }
      break;
    case hitTankNear:
      if (isFar) {
        frontEndPlaySoundPan(cs, hitTankFar, gL, gR);
      } else {
        frontEndPlaySoundPan(cs, hitTankNear, gL, gR);
      }
      break;
    case bubbles:
      frontEndPlaySoundPan(cs, bubbles, gL, gR);
      break;
    case tankSinkNear:
      if (isFar) {
        frontEndPlaySoundPan(cs, tankSinkFar, gL, gR);
      } else {
        frontEndPlaySoundPan(cs, tankSinkNear, gL, gR);
      }
      break;
    case bigExplosionNear:
      if (isFar) {
        frontEndPlaySoundPan(cs, bigExplosionFar, gL, gR);
      } else {
        frontEndPlaySoundPan(cs, bigExplosionNear, gL, gR);
      }
      break;
    case farmingTreeNear:
      if (isFar) {
        frontEndPlaySoundPan(cs, farmingTreeFar, gL, gR);
      } else {
        frontEndPlaySoundPan(cs, farmingTreeNear, gL, gR);
      }
      break;
    case manBuildingNear:
      if (isFar) {
        frontEndPlaySoundPan(cs, manBuildingFar, gL, gR);
      } else {
        frontEndPlaySoundPan(cs, manBuildingNear, gL, gR);
      }
      break;
    case manDyingNear:
      if (isFar) {
        frontEndPlaySoundPan(cs, manDyingFar, gL, gR);
      } else {
        frontEndPlaySoundPan(cs, manDyingNear, gL, gR);
      }
      break;
    case mineExplosionNear:
      if (isFar) {
        frontEndPlaySoundPan(cs, mineExplosionFar, gL, gR);
      } else {
        frontEndPlaySoundPan(cs, mineExplosionNear, gL, gR);
      }
      break;
    case manLayingMineNear:
      frontEndPlaySoundPan(cs, manLayingMineNear, gL, gR);
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
*NAME:          clientSoundDist
*AUTHOR:        John Morrison
*CREATION DATE: 19/01/99
*LAST MODIFIED: 26/09/26
*PURPOSE:
*  Plays a sound the server delivered: the far variant when
*  dist is past SDIST_SOFT, the near one otherwise, panned
*  by pan. The server has already dropped anything out of
*  earshot, so there is no range test here.
*
*ARGUMENTS:
*  sim   - Sim the sound is played against
*  value - Sound effect to be played
*  pan   - East-west offset in squares, east positive
*  dist  - Band top of the larger-axis distance
*********************************************************/
void clientSoundDist(GameSim *sim, sndEffects value, int8_t pan, BYTE dist) {
  soundDistPlay(sim, value, dist > SDIST_SOFT, pan, dist);
}

/*********************************************************
*NAME:          clientSoundDistLocal
*AUTHOR:        John Morrison
*CREATION DATE: 19/01/99
*LAST MODIFIED: 26/09/26
*PURPOSE:
*  Measures a sound this client's own sim raised against the
*  listener's tank, picks the near or far variant from the
*  distance, and plays it panned by its east-west offset.
*  Sounds past SDIST_NONE are dropped.
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
  bool isFar; /* Gap is past the soft range */
  int pan;    /* East-west offset from tank to sound, east positive */
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
    isFar = FALSE;
  } else {
    isFar = TRUE;
  }

  /* Neither bubbles nor manLayingMineNear has a far variant, so a far one is
     silence. Both server delivery paths drop those before sending; this one
     drops them before playing. */
  if (isFar && (value == bubbles || value == manLayingMineNear)) {
    return;
  }

  /* The real square is known here, so the pan is not stepped the way the
     wire's is. */
  pan = (int)mx - (int)tankX;
  if (pan > SOUND_PAN_MAX) {
    pan = SOUND_PAN_MAX;
  } else if (pan < -SOUND_PAN_MAX) {
    pan = -SOUND_PAN_MAX;
  }

  soundDistPlay(sim, value, isFar, (int8_t)pan,
                (gapX > gapY) ? gapX : gapY);
}

/*********************************************************
*NAME:          clientSoundPing
*AUTHOR:        John Morrison
*CREATION DATE: 26/09/26
*LAST MODIFIED: 27/09/26
*PURPOSE:
*  Plays a smart-ping sound panned by the pinged square's
*  east-west offset from the listener's tank. Distance does
*  not change its volume: a ping is a message and stays
*  audible wherever it is. With no listener tank, or with
*  positional sound off, it plays centred.
*
*ARGUMENTS:
*  sim      - Sim the sound is played against
*  listener - Player whose tank the pan is measured from
*  value    - Ping sound effect to be played
*  mx       - Map X co-ordinate of the pinged square
*********************************************************/
void clientSoundPing(GameSim *sim, BYTE listener, sndEffects value, BYTE mx) {
  struct ClientSim *cs = clientSimFromSim(sim);
  int pan;
  uint16_t gL;
  uint16_t gR;

  if (!soundDistPositional(sim) || listener >= MAX_TANKS ||
      sim->tanks[listener] == NULL) {
    frontEndPlaySound(cs, value);
    return;
  }

  pan = (int)mx - (int)tankGetScreenMX(&sim->tanks[listener]);
  if (pan > SOUND_PAN_MAX) {
    pan = SOUND_PAN_MAX;
  } else if (pan < -SOUND_PAN_MAX) {
    pan = -SOUND_PAN_MAX;
  }

  soundDistGains((int8_t)pan, 0, &gL, &gR);
  frontEndPlaySoundPan(cs, value, gL, gR);
}
