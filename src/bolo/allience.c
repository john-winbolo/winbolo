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
*Name:          Allience
*Filename:      Allience.c
*Author:        John Morrison
*Creation Date: 18/02/99
*Last Modified: 25/07/04
*Purpose:
*  Handles allience. Who is allied to who etc. Backed by a
*  bitmask: bit N set => allied to player N.
*********************************************************/

#include "global.h"
#include "allience.h"

allience allienceCreate(void) {
  return 0;
}

void allienceDestroy(allience *value) {
  *value = 0;
}

void allienceAdd(allience *value, BYTE playerNum) {
  *value |= (allience)1u << playerNum;
}

void allienceRemove(allience *value, BYTE playerNum) {
  *value &= ~((allience)1u << playerNum);
}

bool allienceExist(allience *value, BYTE playerNum) {
  return ((*value >> playerNum) & 1u) ? TRUE : FALSE;
}

BYTE allienceNumAllies(allience *value) {
  allience bits = *value;
  BYTE n = 0;
  /* Kernighan: each iteration clears the lowest set bit. */
  while (bits) {
    bits &= bits - 1;
    n++;
  }
  return n;
}

BYTE allienceReturnNum(allience *value, BYTE num) {
  allience bits = *value;
  BYTE i;
  BYTE count = 0;

  for (i = 0; i < MAX_TANKS; i++) {
    if (bits & ((allience)1u << i)) {
      if (count == num) {
        return i;
      }
      count++;
    }
  }
  return NEUTRAL;
}

BYTE allianceMakeLogAlliance(allience *value, BYTE *buff) {
  allience bits = *value;
  BYTE i;
  BYTE n = 0;

  for (i = 0; i < MAX_TANKS; i++) {
    if (bits & ((allience)1u << i)) {
      buff[1 + n] = i;
      n++;
    }
  }
  buff[0] = n;
  return (BYTE)(n + 1);
}
