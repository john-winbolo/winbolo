/*
 * Copyright (c) 1998-2026 John Morrison.
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
 * Name:          local_map_index.h
 * Purpose:       Finds a .map file on this machine by the
 *                md5 of its bytes. A host advertises the
 *                md5 of the map file it loaded; the game
 *                finder uses this to draw that map from
 *                the player's own copy, so a map WinBolo.net
 *                does not store under that md5 (32 of the
 *                94 maps the game ships with, when this was
 *                written) still gets a preview, and so does
 *                a LAN game with no internet connection.
 *
 *                Plain C with no ImGui and no network, so
 *                the unit tests link it. Not thread safe:
 *                the caller serialises every call on one
 *                index.
 *********************************************************/

#ifndef LOCAL_MAP_INDEX_H
#define LOCAL_MAP_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A file larger than this is not hashed. A map is at most 64 KiB; the room
 * above that is for a packed map, which carries its scenario after the map
 * and is hashed whole by the host. */
#define LOCAL_MAP_INDEX_MAX_FILE (4u * 1024u * 1024u)

typedef struct LocalMapIndex LocalMapIndex;

/* An empty index. NULL when out of memory. */
LocalMapIndex *localMapIndexCreate(void);

/* Frees the index. NULL is allowed. */
void localMapIndexDestroy(LocalMapIndex *idx);

/* Brings the index up to date with the .map files under each of `roots`,
 * subfolders included. A root that does not exist is skipped. A file whose
 * size and modification time are what the last scan saw keeps the md5 it
 * already has and is not read again; a new or changed file is read and
 * hashed; a file that is gone is dropped. Only a file that starts with the
 * BMAPBOLO header is kept, which is the same test the host applies before it
 * advertises an md5.
 *
 * Returns how many files this call read and hashed. */
int localMapIndexScan(LocalMapIndex *idx, const char *const *roots,
                      int rootCount);

/* How many files the index holds. */
int localMapIndexCount(const LocalMapIndex *idx);

/* The path of a file whose md5 is md5Hex (32 hex characters, either case),
 * or NULL. The pointer is good until the next scan or destroy. When two
 * files share an md5 the one from the earlier root is returned. */
const char *localMapIndexFind(const LocalMapIndex *idx, const char *md5Hex);

/* Finds md5Hex and reads that file. The bytes are hashed again after the
 * read, and the call fails when they no longer match: a file replaced since
 * the scan must not be drawn as the host's map. On success *bytesOut is the
 * whole file, to be released with SDL_free, and *lenOut its length. */
bool localMapIndexLoad(const LocalMapIndex *idx, const char *md5Hex,
                       uint8_t **bytesOut, size_t *lenOut);

#ifdef __cplusplus
}
#endif

#endif /* LOCAL_MAP_INDEX_H */
