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
*Name:          Labels
*Filename:      label.c
*Author:        John Morrison
*Creation Date:  2/2/99
*Last Modified:  2/2/99
*Purpose:
*  Responsable for message labels (short/long etc).
*********************************************************/

#include <string.h>
#include "global.h"
#include "client_sim.h"
#include "labels.h"
#include "players.h"   /* playersLocationShown */

/* Both builders below put a location after a name in long-label mode, and
 * both ask playersLocationShown first. Without that test they append the
 * geolocator's "XX" unknown sentinel like a real country code, so a player
 * the lookup could not place — and every bot, which is given "XX" outright —
 * carries "@XX" on their tank label and in every message they send. The name
 * builders in players.c make the same decision with the same call. */

void labelMakeMessage(ClientSim *cs, char *res, char *name, char *loc) {
  labelLen lm = cs ? clientSimGetLabelMessage(cs) : lblShort;
  res[0] = '\0';
  if (lm != lblNone) {
    strcat(res, name);
    if (lm == lblLong && playersLocationShown(loc)) {
      strcat(res, LABEL_AT_SYMBOL);
      strcat(res, loc);
    }
  }
}

void labelMakeTankLabel(ClientSim *cs, char *res, char *name, char *loc, bool isOwn) {
  labelLen lt = cs ? clientSimGetLabelTankLabel(cs) : lblShort;
  bool ownTank = cs ? clientSimIsLabelOwnTank(cs) : TRUE;
  res[0] = '\0';
  if (lt != lblNone && (isOwn == FALSE || (isOwn == TRUE && ownTank == TRUE))) {
    strcat(res, name);
    if (lt == lblLong && playersLocationShown(loc)) {
      strcat(res, LABEL_AT_SYMBOL);
      strcat(res, loc);
    }
  }
}
