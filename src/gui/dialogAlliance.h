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
*Name:          Dialog - Alliance
*Filename:      dialogAlliance.h
*Author:        John Morrison
*Creation Date: 11/6/00
*Last Modified: 11/6/00
*Purpose:
*  Looks after the Alliance dialog
*********************************************************/

#ifndef _DIALOG_ALLIANCE_H
#define _DIALOG_ALLIANCE_H

#include "global.h"

/*********************************************************
*NAME:          dialogAllianceCreate
*PURPOSE:
*  Creates the alliance dialog. Returns an opaque handle.
*ARGUMENTS:
*
*********************************************************/
void *dialogAllianceCreate(void);

/*********************************************************
*NAME:          dialogAllianceDestroy
*PURPOSE:
*  Destroys/closes the alliance dialog.
*ARGUMENTS:
*  dlg - The dialog handle from dialogAllianceCreate
*********************************************************/
void dialogAllianceDestroy(void *dlg);

/*********************************************************
*NAME:          dialogAllianceSetName
*PURPOSE:
*  Shows an alliance request from a player.
*ARGUMENTS:
*  playerName - The player name to display
*  playerNum  - The player number requesting alliance
*********************************************************/
void dialogAllianceSetName(char *playerName, BYTE playerNum);

#endif /* _DIALOG_ALLIANCE_H */
