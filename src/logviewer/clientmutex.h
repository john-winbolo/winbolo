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
*Name:          clientMutex
*Filename:      clientMutex.h
*Author:        John Morrison
*Creation Date: 27/5/00
*Last Modified: 27/5/00
*Purpose:
*  WinBolo Server Thread manager
*********************************************************/

#ifndef _CLIENT_MUTEX_H
#define _CLIENT_MUTEX_H

#include "lv_global.h"


/*********************************************************
*NAME:          lv_clientMutexCreate
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
*  Creates the client Mutex. Returns success
*
*ARGUMENTS:
*
*********************************************************/
bool lv_clientMutexCreate(void);

/*********************************************************
*NAME:          lv_clientMutexCreate
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
*  Destroys the client Mutex.
*
*ARGUMENTS:
*
*********************************************************/
void lv_clientMutexDestroy(void);

/*********************************************************
*NAME:          lv_clientMutexWaitFor
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
*  Destroys the client Mutex.
*
*ARGUMENTS:
*
*********************************************************/
void lv_clientMutexWaitFor(void);

/*********************************************************
*NAME:          lv_clientMutexRelease
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
*  Frees the lock on the client Mutex so other waiting
*  subsystems can aquire it.
*
*ARGUMENTS:
*
*********************************************************/
void lv_clientMutexRelease(void);

#endif /* _CLIENT_MUTEX_H */
