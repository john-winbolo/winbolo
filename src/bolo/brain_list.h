/*
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
 * Name:          Brain list
 * Filename:      brain_list.h
 * Purpose:
 *   Discover the set of bot codebases available on the
 *   server by scanning the brains/ directory for any
 *   sub-directory that contains an init.lua. Each entry
 *   carries a display name, the wire path used as the bot
 *   brain (e.g. "Brains/NewAutopilot/init.lua"), and a
 *   coarse version string sourced from the most-recent
 *   modification time of the brain's .lua files.
 *
 *   Server-side only — the resulting list is shipped to
 *   clients via PACKET_LOBBY_BRAIN_LIST.
 *********************************************************/

#ifndef BRAIN_LIST_H
#define BRAIN_LIST_H

#include "global.h"

#define BRAIN_LIST_MAX        16
#define BRAIN_LIST_NAME_LEN   32
#define BRAIN_LIST_VER_LEN    24
#define BRAIN_LIST_PATH_LEN   256

typedef struct {
    char name[BRAIN_LIST_NAME_LEN];     /* "NewAutopilot" — dir name */
    char version[BRAIN_LIST_VER_LEN];   /* "2026-05-11 12:30" or "" */
    char path[BRAIN_LIST_PATH_LEN];     /* "Brains/NewAutopilot/init.lua" */
} BrainListEntry;

typedef struct {
    BrainListEntry entries[BRAIN_LIST_MAX];
    int            count;
} BrainList;

/* Scan brains/ (and brains/ at SDL_GetBasePath when present) for any
 * sub-directory containing an init.lua. Fills `out` with up to
 * BRAIN_LIST_MAX entries sorted alphabetically by name. The version
 * field is set from the recursive max-mtime of .lua files inside the
 * brain directory; empty when no .lua files are found. */
void brainListScan(BrainList *out);

/* Find an entry by wire path (matches `entry.path`). Returns NULL when
 * not present. */
const BrainListEntry *brainListFindByPath(const BrainList *list,
                                          const char *path);

#endif /* BRAIN_LIST_H */
