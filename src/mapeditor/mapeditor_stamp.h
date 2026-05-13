/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_stamp.h
 * Purpose:
 *   Stamp/prefab library for the map editor. Allows saving
 *   clipboard contents as named reusable .bstamp files and
 *   loading them back into paste mode.
 *********************************************************/

#ifndef MAPEDITOR_STAMP_H
#define MAPEDITOR_STAMP_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include "global.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* .bstamp file format magic */
#define STAMP_MAGIC     "BSTAMP01"
#define STAMP_MAGIC_LEN 8

/* On-disk header (packed to ensure portable layout) */
#pragma pack(push, 1)
typedef struct {
    char     magic[8];       /* "BSTAMP01" */
    uint16_t width;          /* 1-256 */
    uint16_t height;         /* 1-256 */
    uint16_t numPills;       /* 0-16 */
    uint16_t numBases;       /* 0-16 */
    uint16_t numStarts;      /* 0-16 */
    uint8_t  reserved[6];    /* padding for future use */
} StampHeader;
#pragma pack(pop)

/* Preview thumbnail dimensions */
#define STAMP_PREVIEW_SIZE 64

/* A single stamp entry in the library */
typedef struct {
    char     name[64];          /* display name (derived from filename) */
    char     filePath[512];     /* full path to .bstamp file */
    int      width, height;
    int      numPills, numBases, numStarts;
    uint32_t previewPixels[STAMP_PREVIEW_SIZE][STAMP_PREVIEW_SIZE]; /* 64x64 RGBA */
    bool     previewReady;
    bool     isBundled;         /* true = from data/stamps/, false = user stamp */
    SDL_Texture *tex;           /* lazily created preview texture */
} StampEntry;

/* Library container */
typedef struct {
    StampEntry *entries;
    int count;
    int capacity;
} StampLibrary;

/* Initialize the stamp library by scanning stamp directories.
 * dataDir: path to the bundled data directory (for data/stamps/). */
void stampLibraryInit(StampLibrary *lib, const char *dataDir);

/* Free all entries and textures. */
void stampLibraryFree(StampLibrary *lib);

/* Re-scan directories (free + re-init). */
void stampLibraryRefresh(StampLibrary *lib, const char *dataDir);

/* Save current clipboard contents to a .bstamp file.
 * Returns true on success. */
bool stampSave(const char *filePath,
               int clipW, int clipH,
               const BYTE clipTerrain[256][256],
               int numPills, const pillbox *pills,
               int numBases, const base *bases,
               int numStarts, const start *starts);

/* Load a .bstamp file into clipboard buffers.
 * Returns true on success. */
bool stampLoad(const char *filePath,
               BYTE clipTerrain[256][256], int *clipW, int *clipH,
               pillbox *pills, int *numPills,
               base *bases, int *numBases,
               start *starts, int *numStarts);

/* Get the user stamps directory path. Creates it if it doesn't exist.
 * Returns the path in dest (caller provides buffer of at least 512 bytes). */
void stampGetUserDir(char *dest, int destLen);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_STAMP_H */
