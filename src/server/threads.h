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
*Name:          Threads
*Filename:      threads.h
*Author:        John Morrison
*Creation Date: 12/08/99
*Last Modified: 21/09/03
*Purpose:
*  WinBolo Server Thread manager
*********************************************************/

#ifndef _THREADS_H
#define _THREADS_H

#include "global.h"

#ifdef __cplusplus
extern "C" {
#endif

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
bool threadsCreate(bool context);

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
void threadsDestroy(void);

/*********************************************************
 * *NAME:          threadsWaitFor
 * *AUTHOR:        John Morrison
 * *CREATION DATE: 12/8/99
 * *LAST MODIFIED: 24/8/99
 * *PURPOSE:
 * *  Waits till the mutex is acquired.
 * *
 * *ARGUMENTS:
 * *
 * *********************************************************/
void threadsWaitForMutex(void);

/*********************************************************
*NAME:          threadsTryWaitForMutex
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
*  Tries to acquire the mutex without blocking.
*  Returns TRUE if acquired, FALSE if busy.
*
*ARGUMENTS:
*
*********************************************************/
bool threadsTryWaitForMutex(void);

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
void threadsReleaseMutex(void);

/*********************************************************
*NAME:          threadsCurrentlyHoldsMutex
*PURPOSE:
*  Debug-only accessor for asserting the mutex-ownership
*  contract on functions whose callers are required to hold
*  the lock (e.g. serverSimApplyCommand). Returns true iff
*  the calling thread is the current owner. Safe to call
*  without holding the mutex — only the owning thread writes
*  its own ID.
*
*ARGUMENTS:
*
*********************************************************/
bool threadsCurrentlyHoldsMutex(void);

/*********************************************************
*NAME:          threadsContextActive
*PURPOSE:
*  True once threadsCreate has brought up the mutex (any real
*  server / loopback / fuzz context). False in bare logic unit
*  tests that never start the threading system. Lets a
*  mutex-ownership assertion no-op where there is no concurrent
*  thread to serialize against.
*
*ARGUMENTS:
*
*********************************************************/
bool threadsContextActive(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _THREADS_H */

