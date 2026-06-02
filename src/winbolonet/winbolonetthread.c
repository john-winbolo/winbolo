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
#include <string.h>
#include <stdlib.h>
#include "global.h"
#include "http.h"
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

/*********************************************************
*NAME:          winbolonetThreadCreate
*PURPOSE:
*  Creates the winbolonet update thread. Returns success.
*********************************************************/
bool winbolonetThreadCreate(void) {
  bool returnValue;        /* Value to return */
#ifdef _WIN32
  char name[FILENAME_MAX]; /* Used in Mutex creation */
#endif

  returnValue = TRUE;
  wbnProcessing = NULL;
  wbnWaiting = NULL;
  wbnShouldRun = TRUE;
  wbnFinished = FALSE;

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

  return returnValue;
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
    /* Wait for all events to be sent... */
    while (wbnProcessing != NULL || wbnWaiting != NULL) {
#ifdef _WIN32
      Sleep(WBN_SHUTDOWN_SLEEP_TIME);
#else
      SDL_Delay(WBN_SHUTDOWN_SLEEP_TIME);
#endif
    }

    /* Wait for current to finish */
    wbnShouldRun = FALSE;
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
}


/*********************************************************
*NAME:          enqueueRequest
*PURPOSE:
*  Shared queue-add. needs_bearer chooses between the
*  bare wbn_api_post and the bearer-bearing
*  wbn_api_post_server at thread-fire time.
*********************************************************/
static void enqueueRequest(const char *endpoint, const char *json_body, bool needs_bearer) {
  wbnList add;

  if (wbnShouldRun != TRUE) {
    return;
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
}

/*********************************************************
*NAME:          winbolonetThreadAddRequest
*PURPOSE:
*  Adds a JSON API request to the background queue.
*  The json_body string is copied internally.
*********************************************************/
void winbolonetThreadAddRequest(const char *endpoint, const char *json_body) {
  enqueueRequest(endpoint, json_body, FALSE);
}

/*********************************************************
*NAME:          winbolonetThreadAddServerRequest
*PURPOSE:
*  Adds a JSON API request to the background queue, flagged
*  to send through wbn_api_post_server so the thread attaches
*  the Authorization: Bearer header at fire time.
*********************************************************/
void winbolonetThreadAddServerRequest(const char *endpoint, const char *json_body) {
  enqueueRequest(endpoint, json_body, TRUE);
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
  char *resp = NULL;

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
        if (wbnProcessing->needs_bearer) {
          wbn_api_post_server(wbnProcessing->endpoint, wbnProcessing->json_body, &resp);
        } else {
          wbn_api_post(wbnProcessing->endpoint, wbnProcessing->json_body, &resp);
        }
        free(resp);
        resp = NULL;
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
        if (q->needs_bearer) {
          wbn_api_post_server(q->endpoint, q->json_body, &resp);
        } else {
          wbn_api_post(q->endpoint, q->json_body, &resp);
        }
        free(resp);
        resp = NULL;
        free(q->json_body);
        Dispose(q);
      }
    }
#ifdef _WIN32
    Sleep(WBN_THREAD_SLEEP_TIME);
#else
    SDL_Delay(WBN_THREAD_SLEEP_TIME);
#endif
  }
  wbnFinished = TRUE;
  return 0;
}
