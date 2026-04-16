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
*Name:          clientMutex
*Filename:      clientMutex.c
*Author:        John Morrison
*Creation Date: 27/5/00
*Last Modified: 27/5/00
*Purpose:
*  WinBolo Server Thread manager
*********************************************************/

#include <SDL3/SDL.h>
#include "global.h"
#include "clientmutex.h"

/* TODO: move to LogViewerState for embedded multi-instance support */
static SDL_Mutex *hClientMutexHandle = NULL;


/*********************************************************
*NAME:          clientMutexCreate
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
*  Creates the client Mutex. Returns success
*
*ARGUMENTS:
*
*********************************************************/
bool clientMutexCreate(void) {
  hClientMutexHandle = SDL_CreateMutex();
  return (hClientMutexHandle != NULL) ? TRUE : FALSE;
}

/*********************************************************
*NAME:          clientMutexDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
*  Destroys the client Mutex.
*
*ARGUMENTS:
*
*********************************************************/
void clientMutexDestroy(void) {
  SDL_DestroyMutex(hClientMutexHandle);
  hClientMutexHandle = NULL;
}


/*********************************************************
*NAME:          clientMutexWaitFor
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
*  Acquires the client Mutex.
*
*ARGUMENTS:
*
*********************************************************/
void clientMutexWaitFor(void) {
  if (hClientMutexHandle == NULL) {
    SDL_Log("clientMutexWaitFor: mutex is NULL, cannot lock");
    return;
  }
  SDL_LockMutex(hClientMutexHandle);
}

/*********************************************************
*NAME:          clientMutexRelease
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
void clientMutexRelease(void) {
  if (hClientMutexHandle == NULL) {
    SDL_Log("clientMutexRelease: mutex is NULL, cannot unlock");
    return;
  }
  SDL_UnlockMutex(hClientMutexHandle);
}
