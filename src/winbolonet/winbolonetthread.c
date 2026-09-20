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
*Name:          winbolonetThread
*Filename:      winbolonetThread.c
*Author:        John Morrison
*Creation Date: 16/02/03
*Last Modified: 30/03/26
*Purpose:
*  WinBolo.net Thread manager - Queues JSON API requests
*  to avoid blocking the game loop
*********************************************************/

#ifdef _WIN32
/* Windows path: HANDLE and DWORD come via WinSock2.h → windows.h in global.h */
#else
#include <SDL3/SDL.h>
typedef SDL_Mutex *HANDLE;
/* DWORD is now uint32_t from platform_types.h — no local typedef needed */
#endif
/* The wake is an SDL semaphore on every platform, including the Windows
 * path above, which otherwise uses Win32 primitives. */
#include <SDL3/SDL_mutex.h>
#include <string.h>
#include <stdlib.h>
#include "global.h"
#include "http.h"
#include "../common/wb_log.h"
#include "winbolonetthread.h"

HANDLE hWbnMutexHandle = NULL;
#ifdef _WIN32
  HANDLE hWbnThread;
  DWORD wbnThreadID;
#else
  SDL_Thread *hWbnThread;
#endif

wbnList wbnProcessing;
wbnList wbnWaiting;
bool wbnShouldRun;
bool wbnFinished;

/* Signalled when a request is queued and when the stop flag is set, so the
 * loop below picks a post up as it arrives rather than at the end of its
 * next wait, and shutdown does not have to wait one out. */
static SDL_Semaphore *wbnWake = NULL;

/*********************************************************
*NAME:          winbolonetThreadCreate
*PURPOSE:
*  Creates the winbolonet update thread. Returns success.
*  Reports success and changes nothing when the thread is
*  already running.
*********************************************************/
bool winbolonetThreadCreate(void) {
  bool returnValue;        /* Value to return */
#ifdef _WIN32
  char name[FILENAME_MAX]; /* Used in Mutex creation */
#endif

  /* Already up. The session boundary drains the worker instead of
     destroying it, so the create after the first server/register is the
     only one; a second create would orphan the running thread, leak its
     mutex and semaphore, and drop whatever was queued. */
  if (hWbnMutexHandle != NULL) {
    return TRUE;
  }

  returnValue = TRUE;
  wbnProcessing = NULL;
  wbnWaiting = NULL;
  wbnShouldRun = TRUE;
  wbnFinished = FALSE;

  wbnWake = SDL_CreateSemaphore(0);
  if (wbnWake == NULL) {
    /* Clear the flag before returning: enqueueRequest reads it as "there is
       a queue to link into", and there is not. */
    wbnShouldRun = FALSE;
    return FALSE;
  }

#ifdef _WIN32
  sprintf(name, "%s%d", "WBNUPDATE", GetTickCount());
  hWbnMutexHandle = CreateMutex(NULL, FALSE, (LPCTSTR ) name);
#else
  hWbnMutexHandle = SDL_CreateMutex();
#endif
  if (hWbnMutexHandle == NULL) {
    returnValue = FALSE;
  }

  /* create thread and run */
#ifdef _WIN32
  if (returnValue == TRUE) {
    hWbnThread = CreateThread((LPSECURITY_ATTRIBUTES) NULL, 0, (LPTHREAD_START_ROUTINE) winbolonetThreadRun, 0, 0, &wbnThreadID);
    if (hWbnThread == NULL) {
      CloseHandle(hWbnMutexHandle);
      hWbnMutexHandle = NULL;
      returnValue = FALSE;
    }
  }
#else
  if (returnValue == TRUE) {
    hWbnThread = SDL_CreateThread(winbolonetThreadRun, "WbnUpdate", NULL);
    if (hWbnThread  == NULL) {
      SDL_DestroyMutex(hWbnMutexHandle);
      hWbnMutexHandle = NULL;
      returnValue = FALSE;
    }
  }
#endif

  if (returnValue == FALSE) {
    SDL_DestroySemaphore(wbnWake);
    wbnWake = NULL;
    wbnShouldRun = FALSE;
  }

  return returnValue;
}

/*********************************************************
*NAME:          winbolonetThreadDrain
*PURPOSE:
*  Waits until every queued request has been sent, leaving
*  the thread running and taking nothing on. A session
*  boundary uses this where it used to destroy the thread,
*  so the worker keeps its pooled connection across a round.
*  Returns at once when nothing is queued, and when no
*  thread was ever created.
*********************************************************/
void winbolonetThreadDrain(void) {
  if (hWbnMutexHandle == NULL) {
    return;
  }

  /* The wake goes out before the wait: the loop parks on the semaphore
     between passes, so a queue handed to it a moment ago would otherwise
     sit there until the end of that wait. */
  if (wbnWake != NULL) {
    SDL_SignalSemaphore(wbnWake);
  }

  /* Wait for all events to be sent... */
  while (wbnProcessing != NULL || wbnWaiting != NULL) {
#ifdef _WIN32
    Sleep(WBN_SHUTDOWN_SLEEP_TIME);
#else
    SDL_Delay(WBN_SHUTDOWN_SLEEP_TIME);
#endif
  }
}

/*********************************************************
*NAME:          winbolonetThreadDestroy
*PURPOSE:
*  Destroys the WBN update thread.
*********************************************************/
void winbolonetThreadDestroy(void) {
  wbnList del;   /* Use to delete our queues */
#ifdef _WIN32
  DWORD val = 0; /* Thread Exit Value for WIN32 */
#endif

  if (hWbnMutexHandle != NULL) {
    winbolonetThreadDrain();

    /* Wait for current to finish. The wake goes out after the flag so a
       loop parked on the semaphore returns now rather than at the end of
       its timeout. */
    wbnShouldRun = FALSE;
    if (wbnWake != NULL) {
      SDL_SignalSemaphore(wbnWake);
    }
    while (wbnFinished == FALSE) {
#ifdef _WIN32
      Sleep(WBN_SHUTDOWN_SLEEP_TIME);
#else
      SDL_Delay(WBN_SHUTDOWN_SLEEP_TIME);
#endif
    }

    /* End Thread */
#ifdef _WIN32
    WaitForSingleObject(hWbnThread, WBN_WAIT_THREAD_EXIT);
    GetExitCodeThread(hWbnThread, &val);
    if (val == STILL_ACTIVE) {
      TerminateThread(hWbnThread, 0);
    }

    CloseHandle(hWbnThread);
#else
    SDL_WaitThread(hWbnThread, NULL);
#endif

    /* Free our list queues */
#ifdef _WIN32
    WaitForSingleObject(hWbnMutexHandle, INFINITE);
#else
    SDL_LockMutex(hWbnMutexHandle);
#endif

    while (NonEmpty(wbnProcessing)) {
      del = wbnProcessing;
      wbnProcessing = wbnProcessing->next;
      free(del->json_body);
      Dispose(del);
    }
    while (NonEmpty(wbnWaiting)) {
      del = wbnWaiting;
      wbnWaiting = wbnWaiting->next;
      free(del->json_body);
      Dispose(del);
    }

#ifdef _WIN32
    ReleaseMutex(hWbnMutexHandle);
    CloseHandle(hWbnMutexHandle);
#else
    SDL_UnlockMutex(hWbnMutexHandle);
    SDL_DestroyMutex(hWbnMutexHandle);
#endif

    hWbnMutexHandle = NULL;
  }

  /* After the join above: nothing is left to wait on it. */
  if (wbnWake != NULL) {
    SDL_DestroySemaphore(wbnWake);
    wbnWake = NULL;
  }
}


/*********************************************************
*NAME:          enqueueRequest
*PURPOSE:
*  Shared queue-add. needs_bearer chooses between the
*  bare wbn_api_post and the bearer-bearing
*  wbn_api_post_server at thread-fire time.
*  Returns TRUE when the item was linked in, FALSE when the
*  worker is not running and nothing was taken. The answer
*  is the one the caller acts on, so it is given by the
*  call that would have queued the work rather than by a
*  separate question asked beforehand.
*********************************************************/
static bool enqueueRequest(const char *endpoint, const char *json_body, bool needs_bearer) {
  wbnList add;

  if (wbnShouldRun != TRUE) {
    return FALSE;
  }
  New(add);
  strncpy(add->endpoint, endpoint, sizeof(add->endpoint) - 1);
  add->endpoint[sizeof(add->endpoint) - 1] = '\0';
  add->json_body = strdup(json_body);
  add->needs_bearer = needs_bearer;
#ifdef _WIN32
  WaitForSingleObject(hWbnMutexHandle, INFINITE);
  add->next = wbnWaiting;
  wbnWaiting = add;
  ReleaseMutex(hWbnMutexHandle);
#else
  SDL_LockMutex(hWbnMutexHandle);
  add->next = wbnWaiting;
  wbnWaiting = add;
  SDL_UnlockMutex(hWbnMutexHandle);
#endif
  /* After the mutex is released: the loop wakes, takes the mutex and swaps
     the queue, so signalling under it would only make it wait. */
  if (wbnWake != NULL) {
    SDL_SignalSemaphore(wbnWake);
  }
  return TRUE;
}

/*********************************************************
*NAME:          winbolonetThreadAddRequest
*PURPOSE:
*  Adds a JSON API request to the background queue.
*  The json_body string is copied internally.
*********************************************************/
bool winbolonetThreadAddRequest(const char *endpoint, const char *json_body) {
  return enqueueRequest(endpoint, json_body, FALSE);
}

/*********************************************************
*NAME:          winbolonetThreadAddServerRequest
*PURPOSE:
*  Adds a JSON API request to the background queue, flagged
*  to send through wbn_api_post_server so the thread attaches
*  the Authorization: Bearer header at fire time.
*********************************************************/
bool winbolonetThreadAddServerRequest(const char *endpoint, const char *json_body) {
  return enqueueRequest(endpoint, json_body, TRUE);
}


/*********************************************************
*NAME:          fireRequest
*PURPOSE:
*  Posts one queued request. The response is discarded —
*  no caller of a queued endpoint reads one — so a failure
*  is reported here instead.
*********************************************************/
static void fireRequest(const char *endpoint, const char *json_body, bool needs_bearer) {
  char *resp = NULL;
  int status;

  if (needs_bearer) {
    status = wbn_api_post_server(endpoint, json_body, &resp);
  } else {
    status = wbn_api_post(endpoint, json_body, &resp);
  }
  if (status < 200 || status > 299) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "WinBolo.net %s failed: HTTP %d", endpoint, status);
  }
  free(resp);
}

/*********************************************************
*NAME:          winbolonetThreadRun
*PURPOSE:
*  The background thread run method. Processes queued
*  JSON API requests via wbn_api_post.
*********************************************************/
int winbolonetThreadRun(void *data) {
  (void)data;
  wbnList q;
  wbnList prev;

  /* One libcurl handle for the life of this thread, so the queued posts
   * below reuse a connection instead of opening one each time. */
  httpWorkerPoolBegin();

  while (wbnShouldRun == TRUE) {

#ifdef _WIN32
    WaitForSingleObject(hWbnMutexHandle, INFINITE);
    wbnProcessing = wbnWaiting;
    wbnWaiting = NULL;
    ReleaseMutex(hWbnMutexHandle);
#else
    SDL_LockMutex(hWbnMutexHandle);
    wbnProcessing = wbnWaiting;
    wbnWaiting = NULL;
    SDL_UnlockMutex(hWbnMutexHandle);
#endif

    while (NonEmpty(wbnProcessing) && wbnShouldRun == TRUE) {
      /* Get last entry (oldest) */
      if (wbnProcessing->next == NULL) {
        /* Only one entry */
        fireRequest(wbnProcessing->endpoint, wbnProcessing->json_body,
                    wbnProcessing->needs_bearer);
        free(wbnProcessing->json_body);
        Dispose(wbnProcessing);
        wbnProcessing = NULL;
      } else {
        prev = wbnProcessing;
        q = wbnProcessing;
        while (q->next != NULL) {
          prev = q;
          q = q->next;
        }
        /* Got last entry */
        prev->next = NULL;
        fireRequest(q->endpoint, q->json_body, q->needs_bearer);
        free(q->json_body);
        Dispose(q);
      }
    }
    /* The old sleep value, now the longest this waits with nothing queued:
       an enqueue or the stop flag returns it at once. */
    SDL_WaitSemaphoreTimeout(wbnWake, WBN_THREAD_SLEEP_TIME);
  }
  httpWorkerPoolEnd();
  wbnFinished = TRUE;
  return 0;
}
