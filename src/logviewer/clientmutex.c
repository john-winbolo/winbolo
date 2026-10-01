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
*Name:          clientMutex
*Filename:      clientMutex.c
*Author:        John Morrison
*Creation Date: 27/5/00
*Last Modified: 27/5/00
*Purpose:
*  WinBolo Server Thread manager
*********************************************************/

#include <SDL3/SDL.h>
#include "../common/wb_log.h"
#include "lv_global.h"
#include "clientmutex.h"

static SDL_Mutex *hClientMutexHandle = NULL;


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
bool lv_clientMutexCreate(void) {
  hClientMutexHandle = SDL_CreateMutex();
  return (hClientMutexHandle != NULL) ? TRUE : FALSE;
}

/*********************************************************
*NAME:          lv_clientMutexDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
*  Destroys the client Mutex.
*
*ARGUMENTS:
*
*********************************************************/
void lv_clientMutexDestroy(void) {
  SDL_DestroyMutex(hClientMutexHandle);
  hClientMutexHandle = NULL;
}


/*********************************************************
*NAME:          lv_clientMutexWaitFor
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
*  Acquires the client Mutex.
*
*ARGUMENTS:
*
*********************************************************/
void lv_clientMutexWaitFor(void) {
  if (hClientMutexHandle == NULL) {
    WB_LOG_WARN(WB_LOG_CAT_LOGVIEWER, "lv_clientMutexWaitFor: mutex is NULL, cannot lock");
    return;
  }
  SDL_LockMutex(hClientMutexHandle);
}

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
void lv_clientMutexRelease(void) {
  if (hClientMutexHandle == NULL) {
    WB_LOG_WARN(WB_LOG_CAT_LOGVIEWER, "lv_clientMutexRelease: mutex is NULL, cannot unlock");
    return;
  }
  SDL_UnlockMutex(hClientMutexHandle);
}
