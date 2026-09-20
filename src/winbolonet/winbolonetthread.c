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

/* One finished job's reply, waiting for the thread that asked for it. Held
 * oldest first so a drain hands them over in the order they completed. */
typedef struct wbnResultObj *wbnResult;
struct wbnResultObj {
  wbnResult next;
  uint32_t  id;
  uint8_t   kind;
  int       status;
  char     *response;  /* Heap, and NULL when the post never sent */
};

static wbnResult wbnResults = NULL;      /* Oldest */
static wbnResult wbnResultsTail = NULL;  /* Newest */
static int wbnResultCount = 0;

/* Set while a job is out of the queue and being posted, so a drain does not
 * read an empty queue with a post still in flight. */
static bool wbnJobInFlight = FALSE;

/* Job ids, handed out under the mutex below. Never 0, which is what a
 * caller reads as "not queued", and not reset by a create, so an id names
 * the same job for the life of the process. */
static uint32_t wbnNextJobId = 1;

/*********************************************************
*NAME:          wbnQueueLock
*PURPOSE:
*  Takes the mutex that covers the request queues, the
*  results list, the in-flight flag and the id counter.
*********************************************************/
static void wbnQueueLock(void) {
#ifdef _WIN32
  WaitForSingleObject(hWbnMutexHandle, INFINITE);
#else
  SDL_LockMutex(hWbnMutexHandle);
#endif
}

/*********************************************************
*NAME:          wbnQueueUnlock
*PURPOSE:
*  Releases the mutex wbnQueueLock took.
*********************************************************/
static void wbnQueueUnlock(void) {
#ifdef _WIN32
  ReleaseMutex(hWbnMutexHandle);
#else
  SDL_UnlockMutex(hWbnMutexHandle);
#endif
}

/*********************************************************
*NAME:          wbnSleep
*PURPOSE:
*  The wait between checks in the spins below.
*********************************************************/
static void wbnSleep(void) {
#ifdef _WIN32
  Sleep(WBN_SHUTDOWN_SLEEP_TIME);
#else
  SDL_Delay(WBN_SHUTDOWN_SLEEP_TIME);
#endif
}

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
  wbnResults = NULL;
  wbnResultsTail = NULL;
  wbnResultCount = 0;
  wbnJobInFlight = FALSE;
  wbnShouldRun = TRUE;
  wbnFinished = FALSE;

  wbnWake = SDL_CreateSemaphore(0);
  if (wbnWake == NULL) {
    /* Clear the flag before returning: enqueueJob reads it as "there is
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
  bool busy;

  if (hWbnMutexHandle == NULL) {
    return;
  }

  /* The wake goes out before the wait: the loop parks on the semaphore
     between passes, so a queue handed to it a moment ago would otherwise
     sit there until the end of that wait. */
  if (wbnWake != NULL) {
    SDL_SignalSemaphore(wbnWake);
  }

  /* Wait for all events to be sent... The check is under the mutex because
     that is where the worker moves the queues and raises the in-flight
     flag, not because the unlocked read this replaces was misreading:
     the sleep in the loop stopped the compiler holding the values in a
     register, and this path has never been seen to return early. */
  for (;;) {
    wbnQueueLock();
    busy = (wbnProcessing != NULL || wbnWaiting != NULL ||
            wbnJobInFlight == TRUE);
    wbnQueueUnlock();
    if (busy == FALSE) {
      return;
    }
    wbnSleep();
  }
}

/*********************************************************
*NAME:          winbolonetThreadDestroy
*PURPOSE:
*  Destroys the WBN update thread.
*********************************************************/
void winbolonetThreadDestroy(void) {
  wbnList del;      /* Use to delete our queues */
  wbnResult delRes; /* Use to delete any undrained results */
#ifdef _WIN32
  DWORD val = 0;    /* Thread Exit Value for WIN32 */
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
      wbnSleep();
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
    wbnQueueLock();

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

    /* And any result nobody came back for, so the thread leaves nothing
       behind for a later session to find. */
    while (wbnResults != NULL) {
      delRes = wbnResults;
      wbnResults = wbnResults->next;
      free(delRes->response);
      Dispose(delRes);
    }
    wbnResultsTail = NULL;
    wbnResultCount = 0;
    wbnJobInFlight = FALSE;

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
*NAME:          enqueueJob
*PURPOSE:
*  Shared queue-add. needs_bearer chooses between the
*  bare wbn_api_post and the bearer-bearing
*  wbn_api_post_server at thread-fire time; kind decides
*  whether the reply is kept for the caller.
*  Returns the job's id, or 0 when the worker is not running
*  and nothing was taken. The answer is the one the caller
*  acts on, so it is given by the call that would have
*  queued the work rather than by a separate question asked
*  beforehand.
*********************************************************/
static uint32_t enqueueJob(const char *endpoint, const char *json_body,
                           bool needs_bearer, uint8_t kind) {
  wbnList add;
  uint32_t id;

  if (wbnShouldRun != TRUE) {
    return 0;
  }
  New(add);
  strncpy(add->endpoint, endpoint, sizeof(add->endpoint) - 1);
  add->endpoint[sizeof(add->endpoint) - 1] = '\0';
  add->json_body = strdup(json_body);
  add->needs_bearer = needs_bearer;
  add->kind = kind;

  wbnQueueLock();
  id = wbnNextJobId;
  wbnNextJobId++;
  if (wbnNextJobId == 0) {
    /* Wrapped. 0 is the refusal answer, so it is not an id. */
    wbnNextJobId = 1;
  }
  add->id = id;
  add->next = wbnWaiting;
  wbnWaiting = add;
  wbnQueueUnlock();

  /* After the mutex is released: the loop wakes, takes the mutex and swaps
     the queue, so signalling under it would only make it wait. */
  if (wbnWake != NULL) {
    SDL_SignalSemaphore(wbnWake);
  }
  return id;
}

/*********************************************************
*NAME:          winbolonetThreadAddRequest
*PURPOSE:
*  Adds a JSON API request to the background queue.
*  The json_body string is copied internally.
*********************************************************/
bool winbolonetThreadAddRequest(const char *endpoint, const char *json_body) {
  return enqueueJob(endpoint, json_body, FALSE, WBN_JOB_NONE) != 0;
}

/*********************************************************
*NAME:          winbolonetThreadAddServerRequest
*PURPOSE:
*  Adds a JSON API request to the background queue, flagged
*  to send through wbn_api_post_server so the thread attaches
*  the Authorization: Bearer header at fire time.
*********************************************************/
bool winbolonetThreadAddServerRequest(const char *endpoint, const char *json_body) {
  return enqueueJob(endpoint, json_body, TRUE, WBN_JOB_NONE) != 0;
}

/*********************************************************
*NAME:          winbolonetThreadAddJob
*PURPOSE:
*  Adds a request whose reply is kept for the caller.
*  Returns the job id, or 0 when nothing was taken.
*********************************************************/
uint32_t winbolonetThreadAddJob(const char *endpoint, const char *json_body,
                                bool needs_bearer, uint8_t kind) {
  return enqueueJob(endpoint, json_body, needs_bearer, kind);
}

/*********************************************************
*NAME:          pushResult
*PURPOSE:
*  Links one finished job's reply onto the results list,
*  newest last. Takes over the response buffer. Called by
*  the worker; the list is handed out by
*  winbolonetThreadDrainResults on the caller's thread.
*********************************************************/
static void pushResult(uint32_t id, uint8_t kind, int status, char *response) {
  wbnResult add;
  wbnResult drop = NULL;

  New(add);
  if (add == NULL) {
    free(response);
    return;
  }
  add->next = NULL;
  add->id = id;
  add->kind = kind;
  add->status = status;
  add->response = response;

  wbnQueueLock();
  if (wbnResultCount >= WBN_MAX_RESULTS) {
    /* Nobody is draining. Drop the oldest rather than grow without bound;
       the freeing and the log line are outside the mutex below. */
    drop = wbnResults;
    wbnResults = drop->next;
    if (wbnResults == NULL) {
      wbnResultsTail = NULL;
    }
    wbnResultCount--;
  }
  if (wbnResultsTail != NULL) {
    wbnResultsTail->next = add;
  } else {
    wbnResults = add;
  }
  wbnResultsTail = add;
  wbnResultCount++;
  wbnQueueUnlock();

  if (drop != NULL) {
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "WinBolo.net results full at %d, dropped job %u kind %u",
                WBN_MAX_RESULTS, (unsigned int)drop->id,
                (unsigned int)drop->kind);
    free(drop->response);
    Dispose(drop);
  }
}

/*********************************************************
*NAME:          winbolonetThreadDrainResults
*PURPOSE:
*  Hands every completed job's result to handler on the
*  calling thread, oldest first, and frees it.
*********************************************************/
void winbolonetThreadDrainResults(WbnResultHandler handler, void *ctx) {
  wbnResult list;
  wbnResult item;

  if (hWbnMutexHandle == NULL) {
    return;
  }

  /* Take the whole list and let go of the mutex before anything is
     dispatched: a handler runs server code that takes locks of its own,
     and the worker must not be held off behind it. */
  wbnQueueLock();
  list = wbnResults;
  wbnResults = NULL;
  wbnResultsTail = NULL;
  wbnResultCount = 0;
  wbnQueueUnlock();

  while (list != NULL) {
    item = list;
    list = list->next;
    if (handler != NULL) {
      handler(item->id, item->kind, item->status, item->response, ctx);
    }
    free(item->response);
    Dispose(item);
  }
}

/*********************************************************
*NAME:          fireRequest
*PURPOSE:
*  Posts one queued request and returns its status. The
*  reply goes to response_out when the caller wants it, and
*  is freed here when it does not — no caller of a
*  WBN_JOB_NONE endpoint reads one — so a failure is
*  reported here instead.
*********************************************************/
static int fireRequest(const char *endpoint, const char *json_body,
                       bool needs_bearer, char **response_out) {
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
  if (response_out != NULL) {
    *response_out = resp;
  } else {
    free(resp);
  }
  return status;
}

/*********************************************************
*NAME:          runJob
*PURPOSE:
*  Posts one job, keeping the reply when its kind asks for
*  one. The job itself is the caller's to free.
*********************************************************/
static void runJob(wbnList job) {
  char *resp = NULL;
  int status;

  if (job->kind == WBN_JOB_NONE) {
    fireRequest(job->endpoint, job->json_body, job->needs_bearer, NULL);
    return;
  }
  status = fireRequest(job->endpoint, job->json_body, job->needs_bearer,
                       &resp);
  pushResult(job->id, job->kind, status, resp);
}

/*********************************************************
*NAME:          takeOldestJob
*PURPOSE:
*  Unlinks the oldest queued job and returns it, or NULL
*  when there is nothing to send. Takes the waiting queue
*  over when the processing one runs out. Raises the
*  in-flight flag with the job still to post, so a drain
*  waiting on the queue does not read it as empty.
*********************************************************/
static wbnList takeOldestJob(void) {
  wbnList job = NULL;
  wbnList prev;
  wbnList q;

  wbnQueueLock();
  if (wbnProcessing == NULL) {
    wbnProcessing = wbnWaiting;
    wbnWaiting = NULL;
  }
  if (wbnProcessing != NULL) {
    if (wbnProcessing->next == NULL) {
      /* Only one entry */
      job = wbnProcessing;
      wbnProcessing = NULL;
    } else {
      /* An enqueue links at the head, so the last entry is the oldest. */
      prev = wbnProcessing;
      q = wbnProcessing;
      while (q->next != NULL) {
        prev = q;
        q = q->next;
      }
      prev->next = NULL;
      job = q;
    }
    job->next = NULL;
    wbnJobInFlight = TRUE;
  }
  wbnQueueUnlock();
  return job;
}

/*********************************************************
*NAME:          finishedJob
*PURPOSE:
*  Frees a job that has been posted and lowers the
*  in-flight flag.
*********************************************************/
static void finishedJob(wbnList job) {
  free(job->json_body);
  Dispose(job);

  wbnQueueLock();
  wbnJobInFlight = FALSE;
  wbnQueueUnlock();
}

/*********************************************************
*NAME:          winbolonetThreadRun
*PURPOSE:
*  The background thread run method. Processes queued
*  JSON API requests via wbn_api_post.
*********************************************************/
int winbolonetThreadRun(void *data) {
  (void)data;
  wbnList job;

  /* One libcurl handle for the life of this thread, so the queued posts
   * below reuse a connection instead of opening one each time. */
  httpWorkerPoolBegin();

  while (wbnShouldRun == TRUE) {

    /* Oldest first, one at a time. The stop flag is read before each job is
       unlinked, so a stop leaves the rest of the queue where
       winbolonetThreadDestroy frees it. */
    while (wbnShouldRun == TRUE) {
      job = takeOldestJob();
      if (job == NULL) {
        break;
      }
      runJob(job);
      finishedJob(job);
    }

    /* The old sleep value, now the longest this waits with nothing queued:
       an enqueue or the stop flag returns it at once. */
    SDL_WaitSemaphoreTimeout(wbnWake, WBN_THREAD_SLEEP_TIME);
  }
  httpWorkerPoolEnd();
  wbnFinished = TRUE;
  return 0;
}
