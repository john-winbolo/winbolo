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
*Filename:      winbolonetThread.h
*Author:        John Morrison
*Creation Date: 16/02/03
*Last Modified: 30/03/26
*Purpose:
*  WinBolo.net Thread manager - Queues JSON API requests
*  to avoid blocking the game loop
*********************************************************/

#ifndef __WINBOLONET_THREAD_H
#define __WINBOLONET_THREAD_H

#include <string.h>
#include "global.h"

#define IsEmpty(list) ((list) ==NULL)
#define NonEmpty(list) (!IsEmpty(list))

/* Time to sleep between checks (MS) */
#define WBN_THREAD_SLEEP_TIME 500
#define WBN_THREAD_SLEEP_TIME_LINUX 1

/* Time to sleep between shutdown checks (MS) */
#define WBN_SHUTDOWN_SLEEP_TIME 200
#define WBN_SHUTDOWN_SLEEP_TIME_LINUX 1

/* Amount of time to wait for the thread to shutdown (MS) */
#define WBN_WAIT_THREAD_EXIT 1000


/* A job's kind says what its response is for. The worker keeps the response
   for every kind but this one and hands it back through
   winbolonetThreadDrainResults; what the kinds mean is the caller's, so
   nothing in the thread knows any of them.

   WBN_JOB_NONE is fire-and-forget: the response is discarded as it is sent
   and no result is kept. It is what winbolonetThreadAddRequest and
   winbolonetThreadAddServerRequest queue, and what every caller that does
   not read a reply wants. */
#define WBN_JOB_NONE 0

/* Results held for a caller that has not drained them yet. A drain runs on
   the thread that queued the work, so the normal depth is one; the cap is
   what stops the list growing for the life of the server if a kind is ever
   added with no drain wired up. Past it the oldest is dropped and the drop
   is logged. */
#define WBN_MAX_RESULTS 64

typedef struct wbnListObj *wbnList;
struct wbnListObj {
  wbnList next;           /* Next item */
  uint32_t id;            /* Job id: unique for the process, never 0 */
  uint8_t kind;           /* WBN_JOB_NONE, or what the response is for */
  char endpoint[128];     /* API endpoint path */
  char *json_body;        /* Heap-allocated JSON body string */
  bool needs_bearer;      /* Send via wbn_api_post_server (Authorization: Bearer) */
};

/*********************************************************
*NAME:          WbnResultHandler
*PURPOSE:
*  Called once per completed job by
*  winbolonetThreadDrainResults, on the draining thread.
*  response is the reply body, or NULL when the post never
*  sent; it belongs to the drain and is freed as soon as the
*  handler returns, so anything kept must be copied.
*
*ARGUMENTS:
* id       - The id winbolonetThreadAddJob returned
* kind     - The kind the job was queued with
* status   - HTTP status, or -1 when the post never sent
* response - Reply body, or NULL
* ctx      - The pointer handed to winbolonetThreadDrainResults
*********************************************************/
typedef void (*WbnResultHandler)(uint32_t id, uint8_t kind, int status,
                                 const char *response, void *ctx);


/*********************************************************
*NAME:          winbolonetThreadCreate
*PURPOSE:
*  Creates the winbolonet update thread. Returns success.
*  Reports success and changes nothing when the thread is
*  already running, so a second caller cannot orphan it.
*********************************************************/
bool winbolonetThreadCreate(void);

/*********************************************************
*NAME:          winbolonetThreadDrain
*PURPOSE:
*  Waits until the queued requests have all been sent and
*  returns with the thread still running. Use at a session
*  boundary, where the queue has to be empty before the
*  caller's own post goes out but the worker is wanted for
*  the next session. Returns at once when the queue is
*  empty, and when there is no thread.
*********************************************************/
void winbolonetThreadDrain(void);

/*********************************************************
*NAME:          winbolonetThreadDestroy
*PURPOSE:
*  Destroys the WBN update thread. Drains the queue first,
*  as winbolonetThreadDrain does, then stops and joins the
*  thread.
*********************************************************/
void winbolonetThreadDestroy(void);

/*********************************************************
*NAME:          winbolonetThreadAddRequest
*PURPOSE:
*  Adds a JSON API request to the background queue.
*  The json_body string is copied internally. Sent via
*  wbn_api_post (no Authorization header).
*
*  Returns TRUE when the request was queued, FALSE when the
*  thread is not running and nothing was taken. A caller
*  that must not lose the post acts on the FALSE by sending
*  it itself.
*
*ARGUMENTS:
* endpoint  - API endpoint path (e.g. "server/update")
* json_body - JSON request body string (copied, caller may free)
*********************************************************/
bool winbolonetThreadAddRequest(const char *endpoint, const char *json_body);

/*********************************************************
*NAME:          winbolonetThreadAddServerRequest
*PURPOSE:
*  Adds a JSON API request to the background queue, to be
*  sent with the Authorization: Bearer header (via
*  wbn_api_post_server). Use for queued server/ endpoints
*  that need the bearer attached when the thread fires.
*
*  Returns TRUE when the request was queued, FALSE when the
*  thread is not running and nothing was taken.
*
*ARGUMENTS:
* endpoint  - API endpoint path (e.g. "server/lobby")
* json_body - JSON request body string (copied, caller may free)
*********************************************************/
bool winbolonetThreadAddServerRequest(const char *endpoint, const char *json_body);

/*********************************************************
*NAME:          winbolonetThreadAddJob
*PURPOSE:
*  Adds a request whose response is kept for the caller.
*  The json_body string is copied internally. The worker
*  posts it as the two calls above do, then holds the status
*  and the reply until winbolonetThreadDrainResults hands
*  them over.
*
*  Returns the job id, which is never 0, or 0 when the
*  thread is not running and nothing was taken.
*
*ARGUMENTS:
* endpoint     - API endpoint path (e.g. "client/verify")
* json_body    - JSON request body string (copied, caller may free)
* needs_bearer - Send via wbn_api_post_server
* kind         - What the response is for. WBN_JOB_NONE keeps
*                nothing and is the same as the calls above.
*********************************************************/
uint32_t winbolonetThreadAddJob(const char *endpoint, const char *json_body,
                                bool needs_bearer, uint8_t kind);

/*********************************************************
*NAME:          winbolonetThreadDrainResults
*PURPOSE:
*  Hands every completed job's result to handler on the
*  calling thread, oldest first, and frees it. The worker's
*  mutex is not held while handler runs, so a handler is
*  free to take whatever locks the server code it calls
*  needs. Does nothing when there is nothing to hand over,
*  and when there is no thread.
*
*ARGUMENTS:
* handler - Called once per result
* ctx     - Passed through to handler
*********************************************************/
void winbolonetThreadDrainResults(WbnResultHandler handler, void *ctx);

/*********************************************************
*NAME:          winbolonetThreadRun
*PURPOSE:
*  The background thread run method.
*********************************************************/
int winbolonetThreadRun(void *data);

#endif /* __WINBOLONET_THREAD_H */
