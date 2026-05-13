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
*Name:          brainsHandler
*Filename:      brainsHandler.h
*Author:        John Morrison
*Creation Date: 23/11/99
*Last Modified: 26/11/99
*Purpose:
*  Handles front end brain operations. Menu setting etc.
*********************************************************/

#ifndef BRAINSHANDLER_H
#define BRAINSHANDLER_H

#include "global.h"

struct ClientSim;

/* Strings */
#ifdef _WIN32
#define SLASH_STRING "\\"
#define SLASH_CHAR '\\'
#else
#define SLASH_STRING "/"
#define SLASH_CHAR '/'
#endif
#define BRAINS_DIR_STRING "Brains"
#define BRAINS_FILTER_STRING "*.brn"
#define BRAINS_EXTENSION ".brn"

/* Resource ID Offset */
#define BRAINS_RESOURCE_OFFSET 41000
#define IDR_BRAIN    2000
#define BRAIN_MAINMENU_OFFSET 5

/*********************************************************
*NAME:          brainsHandlerLoadBrains
*PURPOSE:
*  Loads the brain entries into the brains menu.
*  Returns whether the operation was successful or not
*********************************************************/
bool brainsHandlerLoadBrains(void);

/*********************************************************
*NAME:          brainsHandlerSetDisabled
*PURPOSE:
*  Sets each item in the menu to be disabled.
*
*ARGUMENTS:
*  enabled - TRUE if we should enable the items else
*            disable them
*********************************************************/
void brainsHandlerSet(bool enabled);

/*********************************************************
*NAME:          brainsHandlerGetNum
*PURPOSE:
*  Returns the number of brains loaded.
*********************************************************/
int brainsHandlerGetNum(void);

/*********************************************************
*NAME:          brainsHandlerManual
*PURPOSE:
*  The manual brain button has been pressed
*********************************************************/
void brainsHandlerManual(void);

/*********************************************************
*NAME:          brainsHandlerStart
*PURPOSE:
*  Try to launch a new brain. Returns success.
*
*ARGUMENTS:
*  str  - Filename and path of brain to load
*  name - Name of the brain
*********************************************************/
bool brainsHandlerStart(char *str, char *name, struct ClientSim *cs);

/*********************************************************
*NAME:          brainsHandlerItem
*PURPOSE:
*  The brain selection has been made.
*
*ARGUMENTS:
*  id   - Brain index offset pressed.
*********************************************************/
void brainsHandlerItem(unsigned int id, struct ClientSim *cs);

/*********************************************************
*NAME:          brainsHandlerShutdown
*PURPOSE:
*  Shuts down the brains subsystem.
*********************************************************/
void brainsHandlerShutdown(void);

/*********************************************************
*NAME:          brainHandlerIsBrainRunning
*PURPOSE:
*  Returns whether a brain is running or not
*********************************************************/
bool brainHandlerIsBrainRunning(void);

/*********************************************************
*NAME:          brainHandlerRun
*PURPOSE:
*  Called to execute the brains "THINK" call if it is
*  running
*********************************************************/
void brainHandlerRun(void);

#endif /* BRAINSHANDLER_H */
