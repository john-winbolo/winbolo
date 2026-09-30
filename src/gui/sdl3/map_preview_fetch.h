/*
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          map_preview_fetch.h
 * Purpose:       Async, session-cached fetch of a stored
 *                map's raw .map bytes by md5, off the frame
 *                loop. Requests dedupe per md5 and results
 *                are cached for the process lifetime.
 *********************************************************/

#ifndef MAP_PREVIEW_FETCH_H
#define MAP_PREVIEW_FETCH_H

#include <cstdint>
#include <cstddef>

enum class MapPreviewFetchState { Pending, Ready, Unavailable };

/* Request an async fetch of the map with this 32-char lowercase-hex md5.
 * No-op if md5Hex is null/empty/not 32 chars, or if a request for it is
 * already pending/cached (dedupe). Safe to call every frame. */
void mapPreviewFetchRequest(const char *md5Hex);

/* Current state for md5Hex. On Ready, *bytesOut / *lenOut point at the cached
 * raw .map bytes (stable for the session; do not free). On Pending/Unavailable
 * they are set to nullptr/0. Returns Unavailable for an unknown/empty md5. */
MapPreviewFetchState mapPreviewFetchTryGet(const char *md5Hex,
                                           const uint8_t **bytesOut,
                                           size_t *lenOut);

#endif /* MAP_PREVIEW_FETCH_H */
