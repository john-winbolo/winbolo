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
*Name:          WinBolo.net server bearer-token state
*Filename:      wbn_bearer.h
*Author:        John Morrison
*Creation Date: 30/03/26
*Last Modified: 30/03/26
*Purpose:
*  In-memory storage for the server bearer token returned
*  by POST /api/v1/server/register. Used by http.c when
*  attaching `Authorization: Bearer <token>` to server-side
*  WinBolo.net calls (wbn_api_call_server, httpSendLogFile).
*
*  Hoisted into its own translation unit so the storage +
*  accessors can compile into WinBoloUnitTests without
*  dragging in libcurl / cJSON / SDL3.
*
*  Memory-only, never persisted, never logged with its value.
*********************************************************/

#ifndef __WBN_BEARER_H
#define __WBN_BEARER_H

#include <stddef.h>

/* 64 hex chars + NUL */
#define WBN_SERVER_TOKEN_LEN 65

/*********************************************************
*NAME:          httpSetServerBearerToken
*PURPOSE:
* Stores the bearer token for subsequent server-side calls.
* NULL or "" argument behaves as a clear.
*
*ARGUMENTS:
* token - 64-char hex token, or NULL/"" to clear
*********************************************************/
void httpSetServerBearerToken(const char *token);

/*********************************************************
*NAME:          httpClearServerBearerToken
*PURPOSE:
* Clears the stored bearer token. Idempotent.
*********************************************************/
void httpClearServerBearerToken(void);

/*********************************************************
*NAME:          winboloNetGetServerToken
*PURPOSE:
* Copies the stored bearer token into out (NUL-terminated).
* If no token is set, out becomes the empty string.
* Caller list is intentionally narrow: http.c only, for
* wbn_api_call_server and httpSendLogFile. Not exposed on
* winbolonet_core.h.
*
*ARGUMENTS:
* out     - Destination buffer
* outSize - Capacity of out (no-op when 0)
*********************************************************/
void winboloNetGetServerToken(char *out, size_t outSize);

#endif /* __WBN_BEARER_H */
