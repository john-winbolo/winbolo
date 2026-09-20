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
*Name:          WinBolo.net server bearer-token state
*Filename:      wbn_bearer.c
*Author:        John Morrison
*Creation Date: 30/03/26
*Last Modified: 30/03/26
*Purpose:
*  Storage + accessors for the server bearer token. Lives
*  in its own TU (no curl / cJSON) so the bearer-state tests
*  can link against just this file without pulling in the
*  rest of winbolonet_core.
*
*  Two threads reach the token. The tick writes it when a
*  register's result is applied, and the WinBolo.net worker
*  reads it byte by byte to build the Authorization header
*  of a post it is about to send (http.c, wbn_api_post_server).
*  Both go through the spinlock below, so a read cannot see
*  half of one token and half of another. It is the same
*  shape as wbnKeyLock in winbolonetthread.c, and for the
*  same pair of threads.
*********************************************************/

#include <string.h>
#include <SDL3/SDL_atomic.h>   /* SDL_SpinLock */
#include "wbn_bearer.h"

static char bearerToken[WBN_SERVER_TOKEN_LEN];
static SDL_SpinLock bearerLock = 0;

void httpSetServerBearerToken(const char *token) {
  SDL_LockSpinlock(&bearerLock);
  if (token == NULL || token[0] == '\0') {
    bearerToken[0] = '\0';
  } else {
    strncpy(bearerToken, token, WBN_SERVER_TOKEN_LEN - 1);
    bearerToken[WBN_SERVER_TOKEN_LEN - 1] = '\0';
  }
  SDL_UnlockSpinlock(&bearerLock);
}

void httpClearServerBearerToken(void) {
  SDL_LockSpinlock(&bearerLock);
  bearerToken[0] = '\0';
  SDL_UnlockSpinlock(&bearerLock);
}

void winboloNetGetServerToken(char *out, size_t outSize) {
  size_t i;
  size_t limit;

  if (out == NULL || outSize == 0) {
    return;
  }
  limit = outSize - 1;
  SDL_LockSpinlock(&bearerLock);
  for (i = 0; i < limit && bearerToken[i] != '\0'; i++) {
    out[i] = bearerToken[i];
  }
  SDL_UnlockSpinlock(&bearerLock);
  out[i] = '\0';
}
