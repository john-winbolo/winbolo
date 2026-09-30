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
*Name:          gametype
*Filename:      gametype.c
*Author:        John Morrison
*Creation Date: 29/1/99
*Last Modified:  6/1/00
*Purpose:
*  Responsable for tracking amounts of armour, health etc.
*  that should be given to a tank and gametype
*********************************************************/

#include <stdlib.h>
#include "global.h"
#include "gametype.h"
#include "game_sim.h"

/* 2 as a define to eliminate magic number warnings */
#define TWO 2.0
/* Max bases = 16 as a double */
#define BASE_MAX_DOUBLE 16.0

gameType gmeType = gameOpen; /* Type of game being played */

/*********************************************************
*NAME:          gameTypeSet
*AUTHOR:        John Morrison
*CREATION DATE: 29/1/99
*LAST MODIFIED: 17/12/03
*PURPOSE:
* Sets the game type. Should be set before any other
* calls are made to the module
*
*ARGUMENTS:
*  gmeType      - Pointer to the game type
* value - Value to set the game type to
*********************************************************/
void gameTypeSet(gameType *gmeType, gameType value) {
  *gmeType = value;
}

/*********************************************************
*NAME:          gameTypeGet
*AUTHOR:        John Morrison
*CREATION DATE: 29/01/99
*LAST MODIFIED: 17/12/03
*PURPOSE:
* Returns the game type.
*
*ARGUMENTS:
*  gmeType      - Pointer to the game type
*********************************************************/
gameType gameTypeGet(gameType *gmeType) {
  return *gmeType;
}

/*********************************************************
*NAME:          gameTypeResolve
*AUTHOR:        John Morrison
*PURPOSE:
* The game type a site should pick its behaviour from.
* Everything but gameScripted answers itself; gameScripted
* answers the base game the scenario declared.
*
*ARGUMENTS:
*  sim   - The game being played
*  value - The game type to resolve
*********************************************************/
gameType gameTypeResolve(GameSim *sim, gameType value) {
  gameType base;

  if (value != gameScripted) {
    return value;
  }
  base = (sim != NULL) ? sim->scenarioBaseGame : (gameType)0;
  if (base == gameOpen || base == gameTournament ||
      base == gameStrictTournament) {
    return base;
  }
  /* Nothing declared, or a word the engine has no behaviour for: the round
     plays strict tournament. An author who wants an open round writes
     game = "open" in the manifest, so an empty or misspelt game is a
     scenario that said nothing about how the round is played rather than
     one that asked for the loosest rules. Nothing is assumed on the
     author's behalf. */
  return gameStrictTournament;
}

/*********************************************************
*NAME:          gameTypeGetItems
*AUTHOR:        John Morrison
*CREATION DATE: 29/01/99
*LAST MODIFIED: 17/12/03
*PURPOSE:
* Called when a tank needs to restart. Fills the 
* parameters with how much stuff it should hold.
*
*ARGUMENTS:
*  gmeType      - Pointer to the game type
*  shellsAmount - Pointer to hold the number of shells
*  mines  - Pointer to hold the number of mines
*  armour - Pointer to hold the amount of armour
*  trees  - Pointer to hold the number of trees
*********************************************************/
void gameTypeGetItems(GameSim *sim, gameType *gmeType, BYTE *shellsAmount, BYTE *mines, BYTE *armour, BYTE *trees) {
  double percent; /* Percent of free bases */
  BYTE numBases;  /* Number of bases on the map */

  *armour = (BYTE) sim->rules.tank_full_armour;
  switch (*gmeType) {
  case gameOpen:
    *shellsAmount = (BYTE) sim->rules.tank_full_shells;
    *mines = (BYTE) sim->rules.tank_full_mines;
    *trees = (BYTE) sim->rules.tank_full_trees;
    break;
  case gameTournament:
    /* Live bases only: a removed base is not on the map, so counting its slot
       would dilute the neutral share the shell allowance is drawn from. */
    numBases = 0;
    {
      BYTE bi;
      for (bi = 0; bi < basesGetNumBases(&sim->bs); bi++) {
        if (basesIsActive(&sim->bs, (BYTE)(bi + 1))) {
          numBases++;
        }
      }
    }
    if (numBases == 0) {
      numBases = 1;
    }
    percent = (double) basesGetNumNeutral(&sim->bs);
    percent /= numBases;
    percent *= BASE_MAX_DOUBLE;
    *shellsAmount = (BYTE) (TWO * percent);
    *mines = 0;
    *trees = 0;
    break;
  case gameScripted: {
    /* The scenario's declared base game decides, so a scripted round hands
       a tank what that game hands one. */
    gameType base = gameTypeResolve(sim, gameScripted);
    gameTypeGetItems(sim, &base, shellsAmount, mines, armour, trees);
    break;
  }
  case gameStrictTournament:
  default:
    /* gameStrictTournament */
    *shellsAmount = 0;
    *mines = 0;
    *trees = 0;
    break;
  }
}
