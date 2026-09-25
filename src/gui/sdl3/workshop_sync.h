/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          workshop_sync.h
 * Purpose:       Copies the content of every installed,
 *                subscribed Workshop item into one
 *                directory, <prefpath>Workshop, and keeps
 *                a workshop.json index there saying which
 *                file came from which item.
 *
 *                The mod and map listers read that
 *                directory like any other; nothing reads a
 *                Steam install folder in place except the
 *                skin picker, which keeps doing so. A pass
 *                runs once at startup and again each time
 *                Steam says an item finished installing.
 *
 *                Desktop only: the mobile and wasm builds
 *                have no Workshop and do not compile this.
 *********************************************************/

#ifndef WORKSHOP_SYNC_H
#define WORKSHOP_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the sync reads items from. The default reads the Steam wrapper;
   a test hands in its own. */
typedef struct {
    bool (*available)(void);
    int  (*count)(void);
    bool (*item)(int idx, uint64_t *id, char *folder, size_t folderSize);
    void (*requestDownload)(uint64_t id);
} WorkshopSyncSource;

typedef struct {
    int copied;     /* files copied in (new or changed) */
    int removed;    /* files removed with their index rows */
    int pending;    /* subscribed, not installed; a download was asked for */
    int skipped;    /* installed but not content, or a name clash */
} WorkshopSyncReport;

/* What an item's install folder holds. */
typedef enum {
    WORKSHOP_ITEM_CONTENT,  /* exactly one .map, .scenario or .lua */
    WORKSHOP_ITEM_SKIN,     /* a skin.ini, .wsf or .zip: read in place */
    WORKSHOP_ITEM_OTHER     /* anything else; *why says what */
} WorkshopItemKind;

/* The longest content file name the sync copies, terminator included. The
   listings keep a file name in a buffer of this size, so a longer one could
   not be offered anyway. */
#define WORKSHOP_SYNC_FILE_MAX 128

/* The index the sync keeps beside the files it copied. */
#define WORKSHOP_SYNC_INDEX "workshop.json"

/* Says what folder holds. For WORKSHOP_ITEM_CONTENT the content's file
   name is written to file; for WORKSHOP_ITEM_OTHER, why (when not NULL)
   is pointed at a short reason for the log. Only the folder's own entries
   are read, never its subdirectories. */
WorkshopItemKind workshopSyncClassify(const char *folder, char *file,
                                      size_t fileSize, const char **why);

/* One pass over src into dir. False, having touched nothing, when src says
   the Workshop is unavailable or dir cannot be made; true once the pass has
   run, whatever it found. rep may be NULL. */
bool workshopSyncRunWith(const WorkshopSyncSource *src, const char *dir,
                         WorkshopSyncReport *rep);

/* One pass with the Steam wrapper into scenarioHostWorkshopDir(). */
void workshopSyncRun(void);

/* Once per frame, after steam_run_callbacks(): consumes the wrapper's
   "item installed" edge and, when it fired, runs the sync and moves the
   generation on. The only caller of steam_workshop_consume_installed_event. */
void workshopSyncPoll(void);

/* Moves on each time an installed-item edge was consumed. A reader that
   used to consume the edge keeps the last value it saw and compares. */
uint32_t workshopSyncGeneration(void);

/* One row of the index: an item and the file the sync copied out of it. */
typedef struct {
    uint64_t id;
    char     file[WORKSHOP_SYNC_FILE_MAX];
} WorkshopSyncRow;

/* The rows workshop.json holds in scenarioHostWorkshopDir(), in id order,
   at most max of them. An item still downloading has no row: the sync
   copies only what is installed. A file read, so a caller builds its list
   with it when the list is asked for, never per frame. */
int workshopSyncIndexRows(WorkshopSyncRow *out, int max);

#endif /* WORKSHOP_SYNC_H */
