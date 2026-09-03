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
 * Name:          skin_source.h
 * Purpose:
 *   Reads skin assets out of a directory or a .wsf/.zip
 *   archive through one name index, tracks the active
 *   skin, and enumerates the skins installed on disk.
 *********************************************************/

#ifndef SKIN_SOURCE_H
#define SKIN_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SKIN_ID_MAX     160
#define SKIN_NAME_MAX   128
#define SKIN_NOTES_MAX  512
#define SKIN_PATH_MAX   512

typedef enum SkinKind {
    SKIN_KIND_BUILTIN = 0,   /* shipped under <base>/data/skins/ */
    SKIN_KIND_USER,          /* <prefpath>/skins/ or <base>/skins/ */
    SKIN_KIND_WORKSHOP       /* Steam Workshop install folder (populated later) */
} SkinKind;

/* [Skin] section of skin.ini. Absent keys leave empty strings / zeros. */
typedef struct SkinInfo {
    char     name[SKIN_NAME_MAX];
    char     author[SKIN_NAME_MAX];
    char     notes[SKIN_NOTES_MAX];
    uint64_t workshopId;      /* 0 = none */
    int      maxPixelDensity; /* 0 = unlimited */
    int      inGameRotate;    /* 0 or 1 */
} SkinInfo;

/* One skin found on disk. Fixed size: nothing to free. */
typedef struct SkinEntry {
    char     id[SKIN_ID_MAX];          /* "builtin:x" / "user:x" / "workshop:x" */
    char     displayName[SKIN_NAME_MAX];
    char     path[SKIN_PATH_MAX];      /* absolute path to the dir or archive */
    SkinKind kind;
} SkinEntry;

typedef struct SkinSource SkinSource;   /* opaque */

/*********************************************************
 * NAME:          skinSourceOpen
 * PURPOSE:
 *   Opens a skin directory, a .wsf/.zip archive, or a
 *   directory holding exactly one .wsf/.zip and nothing
 *   else skin-like (that archive is opened instead).
 *   Builds the whole name index up front so every later
 *   lookup is a hash hit. Returns NULL on failure.
 *********************************************************/
SkinSource *skinSourceOpen(const char *path);

/*********************************************************
 * NAME:          skinSourceClose
 * PURPOSE:
 *   Releases a source opened by skinSourceOpen.
 *********************************************************/
void        skinSourceClose(SkinSource *src);

/*********************************************************
 * NAME:          skinSourceExists
 * PURPOSE:
 *   True when the skin holds relName. Pure hash lookup:
 *   no file I/O for either source kind.
 *********************************************************/
bool        skinSourceExists(SkinSource *src, const char *relName);

/*********************************************************
 * NAME:          skinSourceRead
 * PURPOSE:
 *   Reads relName into an SDL_malloc'ed buffer of len + 1
 *   bytes, NUL-terminated at [len] like SDL_LoadFile's, so
 *   the bytes can go straight to nsvgParse. The caller
 *   SDL_frees it. Leaves *buf / *len untouched on a miss.
 *********************************************************/
bool        skinSourceRead(SkinSource *src, const char *relName,
                           void **buf, size_t *len);

/*********************************************************
 * NAME:          skinSourceReadIni
 * PURPOSE:
 *   Fills out from the skin's skin.ini [Skin] section.
 *   Zeroes out first, so a missing ini leaves it empty.
 *********************************************************/
void        skinSourceReadIni(SkinSource *src, SkinInfo *out);

/*********************************************************
 * NAME:          skinSourceZipDirectory
 * PURPOSE:
 *   Writes the top level of dir plus its sounds/ subfolder
 *   into outZip. wrapFolder, when given, prefixes every
 *   entry name with "<wrapFolder>/". False on any failure.
 *********************************************************/
bool        skinSourceZipDirectory(const char *dir, const char *outZip,
                                   const char *wrapFolder /* NULL = none */);

/*********************************************************
 * NAME:          skinSetActive
 * PURPOSE:
 *   Makes the skin with this id current. NULL, "" or
 *   "default" clears to the built-in assets and returns
 *   true. An id that does not resolve also clears, and
 *   returns false.
 *********************************************************/
bool         skinSetActive(const char *id);   /* NULL or "" or "default" = none */

/*********************************************************
 * NAME:          skinGetActive
 * PURPOSE:
 *   Id of the active skin, "" when none is active.
 *********************************************************/
const char  *skinGetActive(void);             /* "" when none */

/*********************************************************
 * NAME:          skinGetActiveSource
 * PURPOSE:
 *   The active skin's source, NULL when none is active.
 *   Owned by the registry; do not close it.
 *********************************************************/
SkinSource  *skinGetActiveSource(void);       /* NULL when none */

/*********************************************************
 * NAME:          skinScanCount
 * PURPOSE:
 *   How many skins the scan locations hold, so a caller
 *   can size the buffer skinScan fills.
 *********************************************************/
int skinScanCount(void);

/*********************************************************
 * NAME:          skinScan
 * PURPOSE:
 *   Writes up to max skins into out and returns how many
 *   it wrote. Locations are walked user-first and the
 *   first hit for a given id wins.
 *********************************************************/
int skinScan(SkinEntry *out, int max);        /* returns entries written */

#ifdef __cplusplus
}
#endif

#endif /* SKIN_SOURCE_H */
