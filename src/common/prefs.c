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

#include <stdio.h>
#include <string.h>

static PrefsDoc *g_doc;
static char g_path[FILENAME_MAX];

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

    g_doc = prefsDocLoad(jsonPath, NULL); /* NULL legacy: no migration */
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

int prefsSetString(const char *section, const char *key,
                   const char *value) {
    if (!g_doc) return 0;
    if (!prefsDocSetString(g_doc, section, key, value)) return 0;
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
