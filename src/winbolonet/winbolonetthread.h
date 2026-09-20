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


typedef struct wbnListObj *wbnList;
struct wbnListObj {
  wbnList next;           /* Next item */
  char endpoint[128];     /* API endpoint path */
  char *json_body;        /* Heap-allocated JSON body string */
  bool needs_bearer;      /* Send via wbn_api_post_server (Authorization: Bearer) */
};


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
*NAME:          winbolonetThreadRun
*PURPOSE:
*  The background thread run method.
*********************************************************/
int winbolonetThreadRun(void *data);

#endif /* __WINBOLONET_THREAD_H */
