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
    SKIN_KIND_WORKSHOP       /* Steam Workshop install folder */
} SkinKind;

/* The filter a skin.ini recommends. Mirrors GfxTextureFilter so
   skin_source needs no dependency on the settings module;
   gfx_settings.c static-asserts that the two agree. */
#define SKIN_FILTER_NONE     (-1)
#define SKIN_FILTER_NEAREST    0
#define SKIN_FILTER_LINEAR     1
#define SKIN_FILTER_PIXELART   2

/* An absent colour in [MapPalette]. Each entry stands on its own: a skin that
   names three colours gets those three and the built-in rest. */
#define SKIN_COLOUR_NONE (-1)

/* Colours a player can be assigned. Matches the log viewer's Team Colours
   dialog, which offers exactly these. */
#define SKIN_TEAM_COLOUR_COUNT 17

/* [MapPalette] section of skin.ini: what a map is drawn in once it is too
   small for its sprites - the map overview and the full screen map below 1x,
   and the map choosers at every zoomed-out rung. Each entry is 0xRRGGBB, or
   SKIN_COLOUR_NONE.

   "Palette" rather than "colours" because this name is written by skin
   authors, and palette is spelled the same either side of the Atlantic. The
   code behind it is map_colours, which follows the rest of this tree.

   Parsed here because reading skin.ini is this module's job; what the entries
   mean, and the built-in colours they replace, belong to map_colours.h. */
typedef struct SkinMapPalette {
    /* Ground, one per terrain family. */
    int32_t grass;
    int32_t swamp;
    int32_t rubble;
    int32_t crater;
    int32_t forest;
    int32_t road;
    int32_t river;
    int32_t deepSea;
    int32_t boat;
    int32_t building;
    int32_t halfBuilding;
    /* The shapes standing on it: the tank triangle, the pill disc and the
       base square, which share one colour per allegiance. markerSelf is the
       viewer's own tank where a view tells it apart from its allies; the game
       does not, and draws it markerGood. */
    int32_t markerSelf;
    int32_t markerGood;
    int32_t markerEvil;
    int32_t markerNeutral;
    /* The seventeen colours a player can be assigned, in the order the log
       viewer's Team Colours dialog lists them - Grey, Khaki, Green, Pink,
       Yellow, LightBlue, Orange, LightPurple, Aqua, LightGreen, LightGrey,
       Red, Blue, Brown, LightPink, PaleGreen, Purple. An array rather than
       seventeen named fields: they are one list, and every consumer indexes
       it by the player's assigned slot. */
    int32_t team[SKIN_TEAM_COLOUR_COUNT];
} SkinMapPalette;

/* [Skin] section of skin.ini. Absent keys leave empty strings / zeros,
   except recommendedFilter, whose "absent" is SKIN_FILTER_NONE, and
   mapPalette, whose every entry is SKIN_COLOUR_NONE. */
typedef struct SkinInfo {
    char     name[SKIN_NAME_MAX];
    char     author[SKIN_NAME_MAX];
    char     notes[SKIN_NOTES_MAX];
    uint64_t workshopId;      /* 0 = none */
    uint64_t workshopAuthor;  /* SteamID64 that published it, 0 = unknown */
    int      maxPixelDensity; /* 0 = unlimited */
    int      inGameRotate;    /* 0 or 1 */
    int      recommendedFilter; /* SKIN_FILTER_*, SKIN_FILTER_NONE = not set */
    SkinMapPalette mapPalette;  /* [MapPalette], every entry SKIN_COLOUR_NONE
                                   when the section is absent */
} SkinInfo;

/* One skin found on disk. Fixed size: nothing to free. */
typedef struct SkinEntry {
    char     id[SKIN_ID_MAX];          /* "builtin:x" / "user:x" / "workshop:x" */
    char     displayName[SKIN_NAME_MAX];
    char     path[SKIN_PATH_MAX];      /* absolute path to the dir or archive */
    SkinKind kind;
    bool     pending;                  /* Workshop item subscribed but not
                                          installed yet: path is empty and it
                                          cannot be made active */
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
 *
 *   An archive's names and sizes are its author's, so the
 *   index leaves out what it will not trust: an entry whose
 *   name would climb out of the skin, one declaring more
 *   than 64 MB, everything past the 4096th, and the
 *   __MACOSX/ and .DS_Store housekeeping Finder's Compress
 *   adds (which would otherwise stop the single top-level
 *   folder being stripped).
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
 * NAME:          skinSourceReadHead
 * PURPOSE:
 *   Reads at most max bytes from the start of relName into
 *   buf and writes how many it got, which is fewer than max
 *   only when the file is shorter. A zip entry is inflated
 *   only that far, so a header check on a large sheet does
 *   not cost the whole sheet. False on a miss or a read
 *   failure.
 *********************************************************/
bool        skinSourceReadHead(SkinSource *src, const char *relName,
                               void *buf, size_t max, size_t *got);

/*********************************************************
 * NAME:          skinSourceReadIni
 * PURPOSE:
 *   Fills out from the skin's skin.ini [Skin] section.
 *   Clears out first, so a missing ini leaves it empty and
 *   recommendedFilter SKIN_FILTER_NONE. Parsed once per
 *   source and cached, so calling it every frame is a
 *   struct copy.
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
 * NAME:          skinSourceExtractTo
 * PURPOSE:
 *   Writes every file the skin holds at its top level and
 *   under sounds/ into dir, creating dir and dir/sounds.
 *   Files anywhere deeper are left out, since no reader
 *   finds them. Names come from the index, so they arrive
 *   lowercased and forward-slashed — harmless, because
 *   every lookup is case-insensitive — and a name that
 *   would land outside dir fails the call. False on any
 *   failure.
 *********************************************************/
bool        skinSourceExtractTo(SkinSource *src, const char *dir);

/*********************************************************
 * NAME:          skinSourceResolveArchive
 * PURPOSE:
 *   Writes into out the path of the one .wsf/.zip dir
 *   holds, when dir holds exactly one archive and nothing
 *   else skin-like — the shape skinSourceOpen descends
 *   into, reading the archive rather than the folder.
 *   False for any other directory, leaving out untouched.
 *********************************************************/
bool        skinSourceResolveArchive(const char *dir, char *out,
                                     size_t outLen);

/*********************************************************
 * NAME:          skinSetWorkshopId
 * PURPOSE:
 *   Records the Workshop id, and the SteamID64 that
 *   published it, in the skin's skin.ini so a later publish
 *   updates that item instead of making a duplicate, and
 *   can tell the publisher's own item from one that came
 *   with someone else's skin. skinPath is a directory or a
 *   .wsf/.zip; an archive is unpacked, edited and rebuilt in
 *   place, and if it is the active source it is closed for
 *   the swap and reopened after, so the rename over it can
 *   go through on Windows. Every other key survives, and so
 *   does every file a reader could find (see
 *   skinSourceExtractTo for what is left out).
 *   authorSteamId 0 means unknown: any WorkshopAuthor line
 *   already there is left exactly as it was, neither
 *   replaced nor removed, because overwriting a known author
 *   with nothing loses information. False on any failure,
 *   leaving the original untouched.
 *********************************************************/
bool        skinSetWorkshopId(const char *skinPath, uint64_t id,
                              uint64_t authorSteamId);

/*********************************************************
 * NAME:          skinSetActive
 * PURPOSE:
 *   Records this id as the player's choice and loads its
 *   assets. NULL, "" or "default" clears to the built-in
 *   assets and returns true. An id that does not resolve
 *   leaves the built-in assets loaded and returns false,
 *   but is still recorded as the player's choice.
 *********************************************************/
bool         skinSetActive(const char *id);   /* NULL or "" or "default" = none */

/*********************************************************
 * NAME:          skinGetActive
 * PURPOSE:
 *   Id of the skin whose assets are loaded, "" for the
 *   built-in assets. Non-empty exactly when
 *   skinGetActiveSource() is non-NULL.
 *********************************************************/
const char  *skinGetActive(void);             /* "" when none */

/*********************************************************
 * NAME:          skinGetRequested
 * PURPOSE:
 *   The id the player last chose, "" for the built-in
 *   assets. Unlike skinGetActive it survives a load
 *   failure, so a Workshop skin that has not finished
 *   downloading is still the saved preference and comes
 *   back when the files do. This is what gets written to
 *   WinBolo.json.
 *********************************************************/
const char  *skinGetRequested(void);          /* "" = built-in assets */

/*********************************************************
 * NAME:          skinGetActiveSource
 * PURPOSE:
 *   The active skin's source, NULL when none is active.
 *   Owned by the registry; do not close it.
 *********************************************************/
SkinSource  *skinGetActiveSource(void);       /* NULL when none */

/*********************************************************
 * NAME:          skinSourceLock / skinSourceUnlock
 * PURPOSE:
 *   Hold the module's lock across more than one call.
 *
 *   The individual calls take it for themselves, so most
 *   callers need neither. This is for a caller that asks
 *   skinGetActiveSource for a pointer and then uses it:
 *   skinSetActive closes and frees the active source, so
 *   the pointer is only good for as long as the lock is
 *   held. map_colours does this to read [MapPalette] from
 *   the map chooser's preview worker thread.
 *
 *   Reentrant, so the calls made while it is held may take
 *   it again. Do not hold it across anything slow.
 *********************************************************/
void         skinSourceLock(void);
void         skinSourceUnlock(void);

/*********************************************************
 * NAME:          skinSourceSerial
 * PURPOSE:
 *   A number unique to this source for the life of the
 *   process, 0 for NULL. Key caches on it rather than on
 *   the pointer: skinSetActive closes one source and opens
 *   the next, and the allocator can hand the new one the
 *   old one's address.
 *********************************************************/
uint64_t     skinSourceSerial(const SkinSource *src);

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
