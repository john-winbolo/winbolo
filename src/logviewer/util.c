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
*Name:          Util
*Filename:      util.c
*Author:        John Morrison
*Creation Date: 25/12/98
*Last Modified: 22/04/01
*Purpose:
*  Provides misc functions
*********************************************************/

/* Inludes */
#include <math.h>
#include <string.h>
#include "lv_global.h"
#include "lv_util.h"


/*********************************************************
*NAME:          lv_utilPtoCString
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Convert Bolo's network pascal string to C strings
*
*ARGUMENTS:
*  src  - Source string
*  dest - Destination string 
*********************************************************/
void lv_utilPtoCString(char *src, char *dest) {
	int count; /* Looping variable */
  int len;   /* Length of the string */
  /* Unsigned: see the note on the game's copy in src/bolo/util.c. This is
   * the one that reads server messages back out of a replay log. */
  len = (unsigned char) src[0];
  for (count=0; count<len; count++) {
    dest[count] = src[count+1];
	}
  dest[count] = '\0';
}

/*********************************************************
*NAME:          lv_utilPtoCString
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Convert a C string to a Bolo's network pascal string 
*
*ARGUMENTS:
*  src  - Source string
*  dest - Destination string 
*********************************************************/
void lv_utilCtoPString(char *src, char *dest) {
	int count; /* Looping variable */
  int len;   /* Length of the string */
  
  len = (int) strlen(src);
  for (count=1; count<=len; count++) {
    dest[count] = src[count-1];
	}
  dest[0] = (char) len;
}

/*********************************************************
*NAME:          lv_utilGetNibbles
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 27/2/99
*PURPOSE:
* Extacts the high and low nibbles out of a byte
*
*ARGUMENTS:
*  value - The byte the nibbles come from
*  high - Pointer to hold high nibble
*  low  - Pointer to hold low nibble
*********************************************************/
void lv_utilGetNibbles(BYTE value, BYTE *high, BYTE *low) {
  *high = *low = value;
  *high >>= NIBBLE_SHIFT_SIZE;
  *low <<= NIBBLE_SHIFT_SIZE;
  *low >>= NIBBLE_SHIFT_SIZE;
}

/*********************************************************
*NAME:          lv_utilPutNibble
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 27/2/99
*PURPOSE:
* Returns the high and low nibbles as a combined byte
*
*ARGUMENTS:
*  high - High nibble
*  low  - Low nibble
*********************************************************/
BYTE lv_utilPutNibble(BYTE high, BYTE low) {
  BYTE returnValue; /* Value to return */

  returnValue = high;
  returnValue <<= NIBBLE_SHIFT_SIZE;
  returnValue += low;
  return returnValue;
}
