/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_fonts.h
 * Purpose:
 *   Font enumeration for the map editor. Scans bundled
 *   and system font directories for TTF/OTF files.
 *********************************************************/

#ifndef MAPEDITOR_FONTS_H
#define MAPEDITOR_FONTS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_NAME_MAX 128
#define FONT_STYLE_MAX 64
#define FONT_PATH_MAX 512
#define FONT_MAX_ENTRIES 500

typedef struct {
    char familyName[FONT_NAME_MAX];
    char styleName[FONT_STYLE_MAX];   /* "Regular", "Bold", "Italic", "Bold Italic" */
    char filePath[FONT_PATH_MAX];
    bool isBundled;                    /* true for fonts in data/fonts/ */
} FontEntry;

typedef struct {
    FontEntry entries[FONT_MAX_ENTRIES];
    int count;
} FontList;

/* Scan bundled + system fonts. Call once at editor startup.
 * Bundled fonts are loaded from data/fonts/.
 * System fonts are scanned from platform-specific directories. */
void fontListInit(FontList *list);

/* Find all styles for a given family name. Returns count of matching entries.
 * outIndices: array of indices into list->entries, caller provides, max outMax. */
int fontListFindStyles(const FontList *list, const char *familyName,
                       int *outIndices, int outMax);

/* Get a deduplicated list of unique family names.
 * outNames: array of string pointers into list->entries[].familyName.
 * Returns count. Bundled fonts come first. */
int fontListGetFamilies(const FontList *list, const char **outNames, int outMax);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_FONTS_H */
