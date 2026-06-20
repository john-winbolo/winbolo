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
*  in its own TU (no curl / cJSON / SDL3) so the bearer-state
*  tests can link against just this file without pulling in
*  the rest of winbolonet_core.
*********************************************************/

#include <string.h>
#include "wbn_bearer.h"

static char bearerToken[WBN_SERVER_TOKEN_LEN];

void httpSetServerBearerToken(const char *token) {
  if (token == NULL || token[0] == '\0') {
    bearerToken[0] = '\0';
    return;
  }
  strncpy(bearerToken, token, WBN_SERVER_TOKEN_LEN - 1);
  bearerToken[WBN_SERVER_TOKEN_LEN - 1] = '\0';
}

void httpClearServerBearerToken(void) {
  bearerToken[0] = '\0';
}

void winboloNetGetServerToken(char *out, size_t outSize) {
  if (out == NULL || outSize == 0) {
    return;
  }
  size_t i;
  size_t limit = outSize - 1;
  for (i = 0; i < limit && bearerToken[i] != '\0'; i++) {
    out[i] = bearerToken[i];
  }
  out[i] = '\0';
}
