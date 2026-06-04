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
*Name:          Threads
*Filename:      threads.c
*Author:        John Morrison
*Creation Date: 12/08/99
*Last Modified: 21/09/03
*Purpose:
*  WinBolo Server Thread manager
*********************************************************/

#include <stdio.h>
#include <SDL3/SDL.h>
#include "global.h"
#include "server_sim.h"
#include "threads.h"

SDL_Mutex *hMutexHandle = NULL;
bool threadStarted = FALSE;

/* SDL3's SDL_CreateMutex on Windows returns an SRW-based mutex that is not
 * recursive. Several call paths in the server nest acquires on the same
 * thread (the host/SP timer callback wraps serverInstanceTick which has its
 * own acquire/release pair; the passive local transport self-locks on the
 * thread that already holds the server lock; sim setters lock while callers
 * already hold). Track the owning thread plus a depth count so the
 * underlying SDL_Mutex is acquired only on the outermost lock and released
 * only on the outermost unlock. Reads of mutexOwner outside the lock are
 * safe because only the owning thread writes its own ID, and it writes
 * while holding the lock. */
static SDL_ThreadID mutexOwner = 0;
static unsigned     mutexDepth = 0;

/*********************************************************
*NAME:          threadsCreate
*AUTHOR:        John Morrison
*CREATION DATE: 12/08/99
*LAST MODIFIED: 21/09/03
*PURPOSE:
*  Creates the thread manager for the server and sets up
*  TCP connection request and UDP listen thread. Returns
*  success
*
*ARGUMENTS:
*  context - TRUE if to start in server context
*********************************************************/
bool threadsCreate(bool context) {
  bool returnValue = TRUE; /* Value to return */

  if (threadStarted == TRUE) {
    return TRUE;
  }
  serverSimConsoleMessage("Thread Manager Startup");
  hMutexHandle = SDL_CreateMutex();
  if (hMutexHandle == NULL) {
    returnValue = FALSE;
    fprintf(stderr, "Error Creating Mutex\n");
  }
 
  threadStarted = returnValue;
  return returnValue;
}

/*********************************************************
*NAME:          threadsDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 12/8/99
*LAST MODIFIED: 24/8/99
*PURPOSE:
*  Terminate and shuts down the threads.
*
*ARGUMENTS:
*
*********************************************************/
void threadsDestroy(void) {
  if (threadStarted == FALSE) {
    return;
  }
  threadStarted = FALSE;
  serverSimConsoleMessage("Thread Manager Shutdown");
  mutexOwner = 0;
  mutexDepth = 0;
  SDL_DestroyMutex(hMutexHandle);
  hMutexHandle = NULL;
   
}

/*********************************************************
*NAME:          threadsWaitFor
*AUTHOR:        John Morrison
*CREATION DATE: 12/8/99
*LAST MODIFIED: 24/8/99
*PURPOSE:
*  Waits till the mutex is acquired.
*
*ARGUMENTS:
*
*********************************************************/
void threadsWaitForMutex(void) {
  SDL_ThreadID me = SDL_GetCurrentThreadID();
  if (mutexOwner == me) {
    mutexDepth++;
    return;
  }
  SDL_LockMutex(hMutexHandle);
  mutexOwner = me;
  mutexDepth = 1;
}

bool threadsTryWaitForMutex(void) {
  SDL_ThreadID me = SDL_GetCurrentThreadID();
  if (mutexOwner == me) {
    mutexDepth++;
    return true;
  }
  if (SDL_TryLockMutex(hMutexHandle)) {
    mutexOwner = me;
    mutexDepth = 1;
    return true;
  }
  return false;
}

/*********************************************************
*NAME:          threadsRelease
*AUTHOR:        John Morrison
*CREATION DATE: 12/8/99
*LAST MODIFIED: 24/8/99
*PURPOSE:
*  Releases the mutex when we are done with it.
*
*ARGUMENTS:
*
*********************************************************/
void threadsReleaseMutex(void) {
  if (mutexDepth > 1) {
    mutexDepth--;
    return;
  }
  mutexOwner = 0;
  mutexDepth = 0;
  SDL_UnlockMutex(hMutexHandle);
}

bool threadsCurrentlyHoldsMutex(void) {
  return mutexOwner == SDL_GetCurrentThreadID();
}

