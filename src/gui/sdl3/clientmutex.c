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
*  WinBolo Client Thread manager (SDL3 implementation)
*********************************************************/

#include <SDL3/SDL.h>
#include <string.h>
#include "global.h"
#include "../clientmutex.h"
#include "../../server/threads.h"

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
  return hClientMutexHandle != NULL;
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
*  Acquires both the client and server mutexes (blocking).
*
*ARGUMENTS:
*
*********************************************************/
/* Lock order: threads mutex BEFORE client mutex.
 *
 * The host server timer thread holds the threads mutex throughout
 * serverInstanceTick, and inside that tick it dispatches CTRL_PLAYER_JOIN
 * etc. to every in-process subscriber (the bots' ClientSims). Each
 * subscriber's apply runs playersSetPlayer -> frontEndRedrawAll ->
 * windowRedrawAll -> clientMutexWaitFor. If that path took client before
 * threads, the timer thread would invert the order vs the main thread
 * (main thread enters via UDP recv and would take client first), and the
 * two would deadlock as soon as both happened to dispatch a player-join
 * simultaneously. Acquiring threads first folds the timer-thread case into
 * a depth-bump and forces both paths to serialise on the same outer lock. */
void clientMutexWaitFor(void) {
  threadsWaitForMutex();
  SDL_LockMutex(hClientMutexHandle);
}

bool clientMutexTryWaitFor(void) {
  if (!threadsTryWaitForMutex()) {
    return FALSE;
  }
  if (!SDL_TryLockMutex(hClientMutexHandle)) {
    threadsReleaseMutex();
    return FALSE;
  }
  return TRUE;
}

/*********************************************************
*NAME:          clientMutexRelease
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
*  Frees the lock on the client Mutex so other waiting
*  subsystems can acquire it.
*
*ARGUMENTS:
*
*********************************************************/
void clientMutexRelease(void) {
  SDL_UnlockMutex(hClientMutexHandle);
  threadsReleaseMutex();
}
