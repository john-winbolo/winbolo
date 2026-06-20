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
#include "lv_global.h"
#include "lv_labels.h"

static bool labelOwnTank = TRUE; /* Should own tank be labeled? */
static labelLen labelMessage = lblShort; /* Should message labels be short? */
static labelLen labelTankLabel = lblShort; /* Should tank labels be short? */

/*********************************************************
*NAME:          lv_labelSetSenderLength
*AUTHOR:        John Morrison
*CREATION DATE:  2/2/99
*LAST MODIFIED:  2/2/99
*PURPOSE:
* Sets the message sender length item (short/long)
*
*ARGUMENTS:
*  isLengthShort - TRUE if the length is to be short
*********************************************************/
void lv_labelSetSenderLength(labelLen isLengthShort) {
  labelMessage = isLengthShort;
}

/*********************************************************
*NAME:          lv_labelSetTankLength
*AUTHOR:        John Morrison
*CREATION DATE:  2/2/99
*LAST MODIFIED:  2/2/99
*PURPOSE:
* Sets the tank label length (short/long)
*
*ARGUMENTS:
*  isLengthShort - TRUE if the length is to be short
*********************************************************/
void lv_labelSetTankLength(labelLen isLengthShort) {
  labelTankLabel = isLengthShort;
}

/*********************************************************
*NAME:          lv_labelSetLabelOwnTank
*AUTHOR:        John Morrison
*CREATION DATE:  2/2/99
*LAST MODIFIED:  2/2/99
*PURPOSE:
* Sets the tank label length (short/long)
*
*ARGUMENTS:
*  labelOwn - TRUE if you should label your own tank
*********************************************************/
void lv_labelSetLabelOwnTank(bool labelOwn) {
  labelOwnTank = labelOwn;
}

/*********************************************************
*NAME:          lv_labelMakeMessage
*AUTHOR:        John Morrison
*CREATION DATE:  2/2/99
*LAST MODIFIED:  2/2/99
*PURPOSE:
*  Makes a message label from the parameters given
*
*ARGUMENTS:
*  res  - Holds the resultant string
*  name - The tank name
*  loc  - The location of the tank
*********************************************************/
void lv_labelMakeMessage(char *res, char *name, char *loc) {
  res[0] = '\0';
  if (labelMessage != lblNone) {
    strncat(res, name, FILENAME_MAX - 1);
    if (labelMessage == lblLong) {
      strncat(res, LABEL_AT_SYMBOL, FILENAME_MAX - strlen(res) - 1);
      strncat(res, loc, FILENAME_MAX - strlen(res) - 1);
    }
  }
}

/*********************************************************
*NAME:          lv_labelMakeTankLabel
*AUTHOR:        John Morrison
*CREATION DATE:  2/2/99
*LAST MODIFIED:  2/2/99
*PURPOSE:
*  Makes a tank label from the parameters given
*
*ARGUMENTS:
*  res   - Holds the resultant string
*  name  - The tank name
*  loc   - The location of the tank
*  isOwn - Is this tank your own
*********************************************************/
void lv_labelMakeTankLabel(char *res, char *name, char *loc, bool isOwn) {
  res[0] = '\0';
  if (labelTankLabel != lblNone && (isOwn == FALSE || (isOwn == TRUE && labelOwnTank == TRUE))) {
    strncat(res, name, FILENAME_MAX - 1);
    if (labelTankLabel == lblLong) {
      strncat(res, LABEL_AT_SYMBOL, FILENAME_MAX - strlen(res) - 1);
      strncat(res, loc, FILENAME_MAX - strlen(res) - 1);
    }
  }
}

