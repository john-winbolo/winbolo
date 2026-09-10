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

/* Buffer sizes for the optional human-readable metadata loaded by
 * brainListLoadMeta. These are display-only (read locally, never sent over
 * the wire), so they don't affect the BrainList struct or any packet. */
#define BRAIN_LIST_TAG_LEN   192   /* short one-line tagline (~20 words)     */
#define BRAIN_LIST_DESC_LEN  640   /* longer hover description (multi-line)  */

/* Load optional metadata for a brain by its catalogue name. Reads
 * "<brains-parent>/<name>/about.txt" from the first parent that has it
 * (working-dir brains/ and Brains/, then SDL_GetBasePath()). The file's first
 * non-empty line is the short tagline; the remainder is the long description.
 * Either out buffer may be NULL; both are NUL-terminated (and cleared) on
 * return. Returns true iff an about.txt was found. */
bool brainListLoadMeta(const char *name,
                       char *tagline, size_t taglineSz,
                       char *desc, size_t descSz);

/* A brain's own tag colour, from a "color: #RRGGBB" line anywhere in its
 * about.txt (the line is kept out of the tagline/description). The lobby
 * paints the bot's name tag with it; a brain that declares none gets a
 * colour derived from its name instead. Returns true iff a valid colour was
 * found; *rgb is 0xRRGGBB. */
bool brainListLoadColor(const char *name, uint32_t *rgb);

/* Split "Name_<ver>" into base ("Name") + numeric version (1.7). No trailing
 * _<digit> suffix → version 0 and the whole name as base. Used to sort the
 * catalogue newest-first, and by the lobby to label a bot row "GoalHunter"
 * rather than "GoalHunter_1.7". `base` is always NUL-terminated. */
double brainListSplitVersion(const char *name, char *base, size_t baseSz);

#endif /* BRAIN_LIST_H */
