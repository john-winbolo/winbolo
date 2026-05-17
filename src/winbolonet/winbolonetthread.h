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
*********************************************************/
bool winbolonetThreadCreate(void);

/*********************************************************
*NAME:          winbolonetThreadDestroy
*PURPOSE:
*  Destroys the WBN update thread.
*********************************************************/
void winbolonetThreadDestroy(void);

/*********************************************************
*NAME:          winbolonetThreadAddRequest
*PURPOSE:
*  Adds a JSON API request to the background queue.
*  The json_body string is copied internally. Sent via
*  wbn_api_post (no Authorization header).
*
*ARGUMENTS:
* endpoint  - API endpoint path (e.g. "server/update")
* json_body - JSON request body string (copied, caller may free)
*********************************************************/
void winbolonetThreadAddRequest(const char *endpoint, const char *json_body);

/*********************************************************
*NAME:          winbolonetThreadAddServerRequest
*PURPOSE:
*  Adds a JSON API request to the background queue, to be
*  sent with the Authorization: Bearer header (via
*  wbn_api_post_server). Use for queued server/* endpoints
*  that need the bearer attached when the thread fires.
*
*ARGUMENTS:
* endpoint  - API endpoint path (e.g. "server/lobby")
* json_body - JSON request body string (copied, caller may free)
*********************************************************/
void winbolonetThreadAddServerRequest(const char *endpoint, const char *json_body);

/*********************************************************
*NAME:          winbolonetThreadRun
*PURPOSE:
*  The background thread run method.
*********************************************************/
int winbolonetThreadRun(void *data);

#endif /* __WINBOLONET_THREAD_H */
