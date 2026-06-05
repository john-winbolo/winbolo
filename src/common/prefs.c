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
#include <string.h>
#include <time.h>

static PrefsDoc *g_doc;
static char g_path[FILENAME_MAX];

/* Sections held only on this device and never uploaded: WINBOLO.NET (auth
 * token/expiry) and DEVICE (install id, label, and sync bookkeeping). This
 * array is the single source of truth for "never synced" — both the upload
 * serializer and the sync-dirty trigger consult it. */
static const char *const kDeviceLocalSections[] = { "WINBOLO.NET", "DEVICE",
                                                    "MAPEDITOR", "LOGVIEWER",
                                                    "WINDOW" };
#define PREFS_DEVICE_LOCAL_COUNT \
    (sizeof(kDeviceLocalSections) / sizeof(kDeviceLocalSections[0]))

/* DEVICE section keys. */
static const char kSecDevice[]             = "DEVICE";
static const char kKeyDeviceId[]           = "DeviceId";
static const char kKeyDeviceLabel[]        = "DeviceLabel";
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

void prefsInit(const char *jsonPath) {
    /* Re-init: drop any previous document first. */
    prefsDocFree(g_doc);
    g_doc = NULL;
    g_path[0] = '\0';

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
        prefsDocSave(g_doc, g_path);
    }
    prefsDocFree(g_doc);
    g_doc = NULL;
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
    if (!prefsDocSetString(g_doc, section, key, value)) return 0;

    /* A change to an upload-eligible section flags the document for cloud
     * re-upload. Changes to device-local sections (auth, device identity
     * and sync bookkeeping) never sync, so they must not set sync-dirty.
     * Write the flag straight to the document so this set is not itself
     * treated as an upload-eligible change. */
    if (!prefsSectionIsDeviceLocal(section) && !prefsSyncDirty()) {
        prefsDocSetString(g_doc, kSecDevice, kKeySyncDirty, "Yes");
    }

    if (s_debounce) {
        s_pendingChange = true;
        return 1;
    }
    return prefsDocSave(g_doc, g_path);
}

int prefsFlush(void) {
    if (!g_doc) return 1;
    if (!prefsDocIsDirty(g_doc)) return 1;
    return prefsDocSave(g_doc, g_path);
}

bool prefsIsDirty(void) {
    return g_doc ? prefsDocIsDirty(g_doc) : false;
}

void prefsSetAutosaveDebounce(unsigned intervalMs) {
    s_debounce = true;
    s_intervalMs = intervalMs;
}

void prefsPumpAutosave(uint64_t nowMs) {
    if (!s_debounce || !g_doc || !prefsDocIsDirty(g_doc)) return;
    /* Re-arm the timer on each newly-seen change so a burst of sets
     * coalesces into one trailing write. */
    if (s_pendingChange) {
        s_lastChangeMs = nowMs;
        s_pendingChange = false;
    }
    if (nowMs - s_lastChangeMs >= s_intervalMs) {
        prefsDocSave(g_doc, g_path);
    }
}

/* ---- Cloud sync (device-local) ---------------------------------------- */

/* Copy src into out, NUL-terminated and truncated to outSize. */
static void prefsCopyOut(const char *src, char *out, size_t outSize) {
    if (!out || outSize == 0) return;
    size_t len = strlen(src);
    if (len > outSize - 1) len = outSize - 1;
    memcpy(out, src, len);
    out[len] = '\0';
}

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
    prefsDocSave(g_doc, g_path);
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
    prefsDocSave(g_doc, g_path);
}

/* Generate a 32-hex-char id from non-cryptographic but process-unique
 * entropy — wall clock, a coarse CPU clock, and a stack address — expanded
 * through a splitmix64 step. Deliberately independent of the game's
 * deterministic bolo_rand, which must stay reproducible; this never feeds
 * the global rand() state either. Uniqueness, not unpredictability. */
static void prefsGenerateDeviceId(char *out /* >= 33 bytes */) {
    static const char hex[] = "0123456789abcdef";
    uint64_t s = (uint64_t)time(NULL);
    s ^= (uint64_t)clock() * 0x9E3779B97F4A7C15ULL;
    s ^= (uint64_t)(uintptr_t)&out;
    for (int i = 0; i < 32; i++) {
        s += 0x9E3779B97F4A7C15ULL;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        out[i] = hex[z & 0xF];
    }
    out[32] = '\0';
}

void prefsGetDeviceId(char *out, size_t outSize) {
    if (!out || outSize == 0) return;
    if (!g_doc) { out[0] = '\0'; return; }
    char id[33];
    prefsDocGetString(g_doc, kSecDevice, kKeyDeviceId, "", id, sizeof(id));
    if (id[0] == '\0') {
        prefsGenerateDeviceId(id);
        prefsDocSetString(g_doc, kSecDevice, kKeyDeviceId, id);
        prefsDocSave(g_doc, g_path); /* durable from first use */
    }
    prefsCopyOut(id, out, outSize);
}

void prefsGetDeviceLabel(char *out, size_t outSize) {
    if (!out || outSize == 0) return;
    if (!g_doc) { out[0] = '\0'; return; }
    prefsDocGetString(g_doc, kSecDevice, kKeyDeviceLabel, "", out, outSize);
}

void prefsSetDeviceLabel(const char *label) {
    if (!g_doc) return;
    prefsDocSetString(g_doc, kSecDevice, kKeyDeviceLabel, label ? label : "");
    prefsDocSave(g_doc, g_path);
}
