/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Process-global preferences API. See prefs.h for the contract. This is
 * a thin layer over the PrefsDoc module: it owns one document and a copy
 * of its on-disk path, and flushes on every set to preserve the
 * per-write durability of the INI API it replaces.
 *
 * Single-threaded by contract (main loop only); no locks are taken.
 */

#include "prefs.h"

#include "prefs_doc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static PrefsDoc *g_doc;
static char g_path[FILENAME_MAX];

/* Sections held only on this device and never uploaded: WINBOLO.NET (auth
 * token/expiry), DEVICE (sync bookkeeping), VOICE.DEVICE (the chosen
 * microphone and speakers, which are named per machine — the rest of VOICE
 * does sync) and SCENARIO PANEL POPOUT (where each popped-out scenario
 * panel's window sat, in this machine's desktop coordinates, as WINDOW is).
 * SCENARIO PANEL SHOWN is a choice, not a place, so it syncs. This
 * array is the single source of truth for "never synced" — both the upload
 * serializer and the sync-dirty trigger consult it. */
static const char *const kDeviceLocalSections[] = { "WINBOLO.NET", "DEVICE",
                                                    "MAPEDITOR", "LOGVIEWER",
                                                    "WINDOW", "HOSTING",
                                                    "VOICE.DEVICE",
                                                    "SCENARIO PANEL POPOUT" };
#define PREFS_DEVICE_LOCAL_COUNT \
    (sizeof(kDeviceLocalSections) / sizeof(kDeviceLocalSections[0]))

/* DEVICE section keys. */
static const char kSecDevice[]             = "DEVICE";
static const char kKeyLastSyncedUpdatedAt[] = "LastSyncedUpdatedAt";
static const char kKeySyncDirty[]          = "SyncDirty";

/* Autosave mode. In immediate mode (default) prefsSetString flushes on
 * every set. In debounced mode it only marks the document dirty; the
 * caller drives prefsPumpAutosave to coalesce bursts into a single
 * trailing write, and prefsFlush/prefsShutdown cover join and exit. */
static bool s_debounce;
static unsigned s_intervalMs;
static uint64_t s_lastChangeMs;
static bool s_pendingChange;

static bool prefsFileExists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fclose(fp);
    return true;
}

/* ---- Merge on save ------------------------------------------------------
 * More than one program keeps this file: the game, the log viewer, the map
 * editor. Each loads the whole document when it starts and, left alone,
 * writes the whole document back, so two of them running at once wipe each
 * other's changes with the copy they loaded.
 *
 * A program that turns merge-on-save on (prefsSetMergeOnSave) instead keeps
 * a list of the keys it has changed since its last write. To write, it reads
 * the file again, puts only those keys onto what it read, writes that, and
 * takes the result as its own document, so what it reads afterwards is what
 * the other programs last wrote. A set then only marks the document dirty;
 * prefsFlush (or prefsShutdown) does the one write.
 *
 * A file that is gone, or that no longer parses, is written from the
 * document in memory, as without the merge. */
typedef struct {
    char *section;
    char *key;
} PrefsChangedKey;

static bool s_merge;
static bool s_mergeWhole;        /* the next write is the whole document */
static PrefsChangedKey *s_changed;
static size_t s_changedCount;
static size_t s_changedCap;

static void prefsChangedClear(void) {
    for (size_t i = 0; i < s_changedCount; i++) {
        free(s_changed[i].section);
        free(s_changed[i].key);
    }
    s_changedCount = 0;
}

static char *prefsDupString(const char *text) {
    size_t len = strlen(text) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, text, len);
    return copy;
}

/* Note that this program changed [section]/key. When the list cannot grow
 * the next write falls back to the whole document: nothing of this
 * program's is lost, which matters more than the other programs' keys. */
static void prefsChangedNote(const char *section, const char *key) {
    if (!s_merge || !section || !key) return;
    for (size_t i = 0; i < s_changedCount; i++) {
        if (strcmp(s_changed[i].section, section) == 0 &&
            strcmp(s_changed[i].key, key) == 0) {
            return;
        }
    }
    if (s_changedCount == s_changedCap) {
        size_t cap = s_changedCap ? s_changedCap * 2 : 32;
        PrefsChangedKey *grown =
            (PrefsChangedKey *)realloc(s_changed, cap * sizeof(*grown));
        if (!grown) { s_mergeWhole = true; return; }
        s_changed = grown;
        s_changedCap = cap;
    }
    char *sec = prefsDupString(section);
    char *k = prefsDupString(key);
    if (!sec || !k) {
        free(sec);
        free(k);
        s_mergeWhole = true;
        return;
    }
    s_changed[s_changedCount].section = sec;
    s_changed[s_changedCount].key = k;
    s_changedCount++;
}

/* Every write of the document goes through here. 1 ok / 0 fail. */
static int prefsWrite(void) {
    if (!g_doc) return 0;
    if (s_merge && !s_mergeWhole && prefsFileExists(g_path)) {
        PrefsDoc *disk = prefsDocLoad(g_path);
        bool grafted = (disk != NULL);
        for (size_t i = 0; grafted && i < s_changedCount; i++) {
            if (!prefsDocCopyKey(disk, g_doc, s_changed[i].section,
                                 s_changed[i].key)) {
                grafted = false;
            }
        }
        if (grafted) {
            if (!prefsDocSave(disk, g_path)) {
                /* Not written: keep the document and the list as they
                 * are, so the next flush tries again. */
                prefsDocFree(disk);
                return 0;
            }
            prefsDocFree(g_doc);
            g_doc = disk;
            prefsChangedClear();
            return 1;
        }
        /* The file does not parse, or a key could not be copied: write
         * the document in memory, as without the merge. */
        prefsDocFree(disk);
    }
    if (!prefsDocSave(g_doc, g_path)) return 0;
    prefsChangedClear();
    s_mergeWhole = false;
    return 1;
}

void prefsSetMergeOnSave(bool on) {
    s_merge = on;
    s_mergeWhole = false;
    prefsChangedClear();
}

bool prefsMergeOnSave(void) {
    return s_merge;
}

void prefsInit(const char *jsonPath) {
    /* Re-init: drop any previous document first. */
    prefsDocFree(g_doc);
    g_doc = NULL;
    g_path[0] = '\0';
    prefsChangedClear();
    s_mergeWhole = false;

    if (!jsonPath) {
        return;
    }

    bool existed = prefsFileExists(jsonPath);

    g_doc = prefsDocLoad(jsonPath);
    if (!g_doc) {
        /* Existing file present but did not parse: preserve it under a
         * .corrupt sibling (best-effort) and start from a fresh empty
         * document, materializing the json path below. */
        char corruptPath[FILENAME_MAX];
        int n = snprintf(corruptPath, sizeof(corruptPath), "%s.corrupt",
                         jsonPath);
        if (n > 0 && n < (int)sizeof(corruptPath)) {
            rename(jsonPath, corruptPath); /* ignore failure */
        }
        g_doc = prefsDocNew();
        existed = false; /* force materialization of the replacement */
    }

    snprintf(g_path, sizeof(g_path), "%s", jsonPath);

    /* Materialize a fresh (or just-recovered) document so the file
     * always exists after init. */
    if (g_doc && !existed) {
        prefsDocSave(g_doc, g_path);
    }
}

void prefsShutdown(void) {
    /* Flush a pending debounced change so a clean exit never loses it. */
    if (g_doc && prefsDocIsDirty(g_doc)) {
        prefsWrite();
    }
    prefsDocFree(g_doc);
    g_doc = NULL;
    prefsChangedClear();
    s_mergeWhole = false;
}

unsigned long prefsGetString(const char *section, const char *key,
                             const char *defaultVal,
                             char *out, size_t outSize) {
    if (!g_doc) {
        const char *src = defaultVal ? defaultVal : "";
        if (!out || outSize == 0) return 0;
        size_t len = strlen(src);
        if (len > outSize - 1) len = outSize - 1;
        memcpy(out, src, len);
        out[len] = '\0';
        return (unsigned long)len;
    }
    return prefsDocGetString(g_doc, section, key, defaultVal, out, outSize);
}

static bool prefsSectionIsDeviceLocal(const char *section) {
    for (size_t i = 0; i < PREFS_DEVICE_LOCAL_COUNT; i++) {
        if (strcmp(section, kDeviceLocalSections[i]) == 0) return true;
    }
    return false;
}

int prefsSetString(const char *section, const char *key,
                   const char *value) {
    if (!g_doc) return 0;
    /* Skip a write that changes nothing: re-setting a value to what it
     * already holds must not dirty the document or flag a cloud re-upload.
     * This lets callers flush a whole batch of settings (e.g. closing the
     * Settings dialog) without uploading when nothing actually changed. */
    if (prefsDocStringEquals(g_doc, section, key, value)) return 1;
    if (!prefsDocSetString(g_doc, section, key, value)) return 0;
    prefsChangedNote(section, key);

    /* A change to an upload-eligible section flags the document for cloud
     * re-upload. Changes to device-local sections (auth, device identity
     * and sync bookkeeping) never sync, so they must not set sync-dirty.
     * Write the flag straight to the document so this set is not itself
     * treated as an upload-eligible change. */
    if (!prefsSectionIsDeviceLocal(section) && !prefsSyncDirty()) {
        prefsDocSetString(g_doc, kSecDevice, kKeySyncDirty, "Yes");
        prefsChangedNote(kSecDevice, kKeySyncDirty);
    }

    if (s_debounce) {
        s_pendingChange = true;
        return 1;
    }
    /* Merge-on-save: the caller's prefsFlush does one write for all the
     * keys it set, not a read and a write of the file for each. */
    if (s_merge) return 1;
    return prefsWrite();
}

int prefsFlush(void) {
    if (!g_doc) return 1;
    if (!prefsDocIsDirty(g_doc)) return 1;
    return prefsWrite();
}

bool prefsIsDirty(void) {
    return g_doc ? prefsDocIsDirty(g_doc) : false;
}

void prefsSetAutosaveDebounce(unsigned intervalMs) {
    s_debounce = true;
    s_intervalMs = intervalMs;
}

bool prefsPumpAutosave(uint64_t nowMs) {
    if (!s_debounce || !g_doc || !prefsDocIsDirty(g_doc)) return false;
    /* Re-arm the timer on each newly-seen change so a burst of sets
     * coalesces into one trailing write. */
    if (s_pendingChange) {
        s_lastChangeMs = nowMs;
        s_pendingChange = false;
    }
    if (nowMs - s_lastChangeMs >= s_intervalMs) {
        prefsWrite();
        return true;
    }
    return false;
}

/* ---- Cloud sync (device-local) ---------------------------------------- */

char *prefsSerializeForUpload(void) {
    if (!g_doc) return NULL;
    return prefsDocSerializeExcluding(g_doc, kDeviceLocalSections,
                                      PREFS_DEVICE_LOCAL_COUNT);
}

bool prefsSyncDirty(void) {
    if (!g_doc) return false;
    char buf[8];
    prefsDocGetString(g_doc, kSecDevice, kKeySyncDirty, "No",
                      buf, sizeof(buf));
    return strcmp(buf, "Yes") == 0;
}

void prefsClearSyncDirty(void) {
    if (!g_doc) return;
    prefsDocSetString(g_doc, kSecDevice, kKeySyncDirty, "No");
    prefsChangedNote(kSecDevice, kKeySyncDirty);
    prefsWrite();
}

int prefsAdoptServerDocument(const char *serverPrefsJson) {
    if (!g_doc) return PREFS_ADOPT_MALFORMED;
    int rc = prefsDocAdoptUploadEligible(g_doc, serverPrefsJson,
                                         kDeviceLocalSections,
                                         PREFS_DEVICE_LOCAL_COUNT);
    if (rc == PREFS_ADOPT_OK) {
        /* Whole sections were replaced, which a list of changed keys
         * cannot say: this one write is the whole document. */
        if (s_merge) s_mergeWhole = true;
        /* Adopting the server doc means the local copy now matches it:
         * drop sync-dirty and persist (the save flushes the grafted
         * sections too). The caller records the server token separately
         * via prefsMarkSynced. */
        prefsClearSyncDirty();
    }
    return rc;
}

void prefsMarkSynced(const char *updatedAt) {
    if (!g_doc) return;
    prefsSetLastSyncedUpdatedAt(updatedAt);
    prefsClearSyncDirty();
}

void prefsGetLastSyncedUpdatedAt(char *out, size_t outSize) {
    if (!out || outSize == 0) return;
    if (!g_doc) { out[0] = '\0'; return; }
    prefsDocGetString(g_doc, kSecDevice, kKeyLastSyncedUpdatedAt, "",
                      out, outSize);
}

void prefsSetLastSyncedUpdatedAt(const char *token) {
    if (!g_doc) return;
    prefsDocSetString(g_doc, kSecDevice, kKeyLastSyncedUpdatedAt,
                      token ? token : "");
    prefsChangedNote(kSecDevice, kKeyLastSyncedUpdatedAt);
    prefsWrite();
}
