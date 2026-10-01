/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          wbn_map
*Filename:      wbn_map.h
*Purpose:
*  Resolves a stored map by its md5 via the WinBolo.net
*  REST API (POST /api/v1/map). A pure response parser
*  (base64-decodes the map bytes) plus a thin synchronous
*  fetch wrapper that signs and posts the request.
*********************************************************/

#ifndef __WBN_MAP_H
#define __WBN_MAP_H

#include "global.h"   /* MAP_STR_SIZE */

typedef struct {
  bool     found;            /* true on a 200 carrying a stored map */
  char     mapMd5[33];       /* echoed 32-hex + NUL */
  char     name[MAP_STR_SIZE];
  uint8_t *mapData;          /* heap; raw .map bytes (base64-decoded). NULL if !found. */
  size_t   mapDataLen;
} WbnMapResult;

/*********************************************************
*NAME:          wbnMapParseResponse
*PURPOSE:
* Parse a /api/v1/map JSON response body. Returns true if the
* JSON parsed; out->found reflects the body's "found". On found,
* base64-decodes "map_data" into out->mapData (caller frees via
* wbnMapResultFree). Returns false on malformed JSON or a
* base64-decode failure.
*
*ARGUMENTS:
* json - Response body string (may be NULL)
* out  - Receives the parsed result (caller frees with
*        wbnMapResultFree)
*********************************************************/
bool wbnMapParseResponse(const char *json, WbnMapResult *out);

/*********************************************************
*NAME:          wbnMapFetchByMd5
*PURPOSE:
* Synchronous (blocking) POST {map_md5} to /api/v1/map and fill
* *out. Returns true on a 200 with a decoded map
* (out->found == true); false on 404/400/transport/parse error
* (out zeroed, mapData NULL). Run off the frame loop.
*
*ARGUMENTS:
* md5Hex - 32-char hex map md5
* out    - Receives the parsed result (caller frees with
*          wbnMapResultFree)
*********************************************************/
bool wbnMapFetchByMd5(const char *md5Hex, WbnMapResult *out);

/*********************************************************
*NAME:          wbnMapResultFree
*PURPOSE:
* Frees out->mapData and zeroes the struct. Safe on a
* zeroed/!found result or a NULL pointer.
*
*ARGUMENTS:
* out - Result to free (may be NULL)
*********************************************************/
void wbnMapResultFree(WbnMapResult *out);

#endif /* __WBN_MAP_H */
