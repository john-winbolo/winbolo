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
 *   carries a display name and a coarse version string
 *   sourced from the most-recent modification time of the
 *   brain's .lua files. Disk paths are server-private and
 *   live alongside the catalogue under internal/.
 *
 *   Server-side discovery — the resulting list is shipped
 *   to clients via PACKET_LOBBY_BRAIN_LIST so the lobby
 *   AiConfig combo can list the available brains by name.
 *********************************************************/

#ifndef BRAIN_LIST_H
#define BRAIN_LIST_H

#include "global.h"

#define BRAIN_LIST_MAX        16
#define BRAIN_LIST_NAME_LEN   32
#define BRAIN_LIST_VER_LEN    24

typedef struct {
    char name[BRAIN_LIST_NAME_LEN];     /* "GoalHunter" — dir name */
    char version[BRAIN_LIST_VER_LEN];   /* "2026-05-11 12:30" or "" */
} BrainListEntry;

typedef struct {
    BrainListEntry entries[BRAIN_LIST_MAX];
    int            count;
} BrainList;

#endif /* BRAIN_LIST_H */
