/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Preferences document backed by cJSON. See prefs_doc.h for the API
 * contract and document shape. The module is standalone; the
 * Get/WritePrivateProfileString Profile API is not backed by it.
 */

#include "prefs_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "cJSON.h"

struct PrefsDoc {
    cJSON *root;   /* top-level object: "_version" + section objects */
    bool dirty;
};

/* ------------------------------------------------------------------ */
/* Construction / parsing                                              */
/* ------------------------------------------------------------------ */

static PrefsDoc *prefsDocWrap(cJSON *root) {
    PrefsDoc *doc = (PrefsDoc *)malloc(sizeof(*doc));
    if (!doc) {
        cJSON_Delete(root);
        return NULL;
    }
    doc->root = root;
    doc->dirty = false;
    return doc;
}

PrefsDoc *prefsDocNew(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    if (!cJSON_AddNumberToObject(root, "_version", 1)) {
        cJSON_Delete(root);
        return NULL;
    }
    return prefsDocWrap(root);
}

PrefsDoc *prefsDocParseJson(const char *jsonText) {
    if (!jsonText) return NULL;
    cJSON *root = cJSON_Parse(jsonText);
    if (!root) return NULL;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return prefsDocWrap(root);
}

/* Get an existing section object, or create and attach one. Returns
 * NULL on OOM. If a same-named member exists but is not an object
 * (malformed input) it is replaced with a fresh object. */
static cJSON *prefsDocSection(cJSON *root, const char *section) {
    cJSON *sec = cJSON_GetObjectItemCaseSensitive(root, section);
    if (sec && cJSON_IsObject(sec)) return sec;
    if (sec) cJSON_DeleteItemFromObjectCaseSensitive(root, section);
    sec = cJSON_CreateObject();
    if (!sec) return NULL;
    cJSON_AddItemToObject(root, section, sec);
    return sec;
}

/* Store key=value as a string member of sectionObj, replacing any
 * existing member of the same name. Returns 0 on OOM. */
static int prefsDocPutMember(cJSON *sectionObj, const char *key,
                             const char *value) {
    cJSON *s = cJSON_CreateString(value);
    if (!s) return 0;
    cJSON_DeleteItemFromObjectCaseSensitive(sectionObj, key);
    cJSON_AddItemToObject(sectionObj, key, s);
    return 1;
}

/* ------------------------------------------------------------------ */
/* File I/O                                                            */
/* ------------------------------------------------------------------ */

/* Read an entire file into a NUL-terminated heap buffer. *existed is
 * set to 1 if the file could be opened. Returns NULL if the file does
 * not exist (*existed == 0) or on a read/OOM error of an existing file
 * (*existed == 1). */
static char *prefsDocReadFile(const char *path, int *existed) {
    *existed = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    *existed = 1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    if (ferror(fp)) { fclose(fp); free(buf); return NULL; }
    fclose(fp);
    buf[rd] = '\0';
    return buf;
}

PrefsDoc *prefsDocLoad(const char *path) {
    if (path) {
        int existed = 0;
        char *content = prefsDocReadFile(path, &existed);
        if (content) {
            PrefsDoc *doc = prefsDocParseJson(content);
            free(content);
            return doc;
        }
        if (existed) return NULL; /* present but unreadable / OOM */
    }
    return prefsDocNew();
}

char *prefsDocSerialize(const PrefsDoc *doc) {
    if (!doc) return NULL;
    return cJSON_PrintUnformatted(doc->root);
}

char *prefsDocSerializeExcluding(const PrefsDoc *doc,
                                 const char *const *sections, size_t count) {
    if (!doc) return NULL;
    cJSON *copy = cJSON_Duplicate(doc->root, 1 /* recurse */);
    if (!copy) return NULL;
    if (sections) {
        for (size_t i = 0; i < count; i++) {
            if (sections[i]) {
                cJSON_DeleteItemFromObjectCaseSensitive(copy, sections[i]);
            }
        }
    }
    char *json = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    return json;
}

#ifndef _WIN32
/* Atomic, mode-0600, fsync'd write — secure temp-file + rename pattern
 * with a JSON payload. */
static int prefsDocWriteAtomic(const char *path, const char *data) {
    char tmpPath[FILENAME_MAX];
    int n = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    if (n < 0 || n >= (int)sizeof(tmpPath)) return 0;

    int fd = open(tmpPath, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return 0;
    FILE *fp = fdopen(fd, "w");
    if (!fp) { close(fd); unlink(tmpPath); return 0; }

    size_t len = strlen(data);
    if (fwrite(data, 1, len, fp) != len) {
        fclose(fp); unlink(tmpPath); return 0;
    }
    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        fclose(fp); unlink(tmpPath); return 0;
    }
    if (fclose(fp) != 0) { unlink(tmpPath); return 0; }

    /* Defensive: re-assert 0600 in case a stale temp inode survived
     * with a wider mode (umask races on first creation). */
    chmod(tmpPath, 0600);
    if (rename(tmpPath, path) != 0) { unlink(tmpPath); return 0; }
    return 1;
}
#else
/* _WIN32 fallback: plain temp-file + rename, no fsync and no mode bits.
 * Minimal on purpose — proper Win32 atomic and mode-restricted writes
 * are added when the Profile API is backed by this module. */
static int prefsDocWriteAtomic(const char *path, const char *data) {
    char tmpPath[FILENAME_MAX];
    int n = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    if (n < 0 || n >= (int)sizeof(tmpPath)) return 0;

    FILE *fp = fopen(tmpPath, "wb");
    if (!fp) return 0;
    size_t len = strlen(data);
    if (fwrite(data, 1, len, fp) != len) { fclose(fp); remove(tmpPath); return 0; }
    if (fclose(fp) != 0) { remove(tmpPath); return 0; }

    remove(path); /* Win32 rename won't overwrite an existing target */
    if (rename(tmpPath, path) != 0) { remove(tmpPath); return 0; }
    return 1;
}
#endif

int prefsDocSave(PrefsDoc *doc, const char *path) {
    if (!doc || !path) return 0;
    char *json = prefsDocSerialize(doc);
    if (!json) return 0;
    int ok = prefsDocWriteAtomic(path, json);
    free(json);
    if (ok) doc->dirty = false;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                          */
/* ------------------------------------------------------------------ */

unsigned long prefsDocGetString(const PrefsDoc *doc, const char *section,
                                const char *key, const char *defaultVal,
                                char *out, size_t outSize) {
    const char *src = defaultVal ? defaultVal : "";
    if (doc && section && key) {
        cJSON *sec = cJSON_GetObjectItemCaseSensitive(doc->root, section);
        if (sec && cJSON_IsObject(sec)) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(sec, key);
            if (item && cJSON_IsString(item) && item->valuestring) {
                src = item->valuestring;
            }
        }
    }

    if (!out || outSize == 0) return 0;
    size_t len = strlen(src);
    if (len > outSize - 1) len = outSize - 1;
    memcpy(out, src, len);
    out[len] = '\0';
    return (unsigned long)len;
}

int prefsDocSetString(PrefsDoc *doc, const char *section,
                      const char *key, const char *value) {
    if (!doc || !section || !key) return 0;
    cJSON *sec = prefsDocSection(doc->root, section);
    if (!sec) return 0;
    if (!prefsDocPutMember(sec, key, value ? value : "")) return 0;
    doc->dirty = true;
    return 1;
}

bool prefsDocStringEquals(const PrefsDoc *doc, const char *section,
                          const char *key, const char *value) {
    if (!doc || !section || !key) return false;
    cJSON *sec = cJSON_GetObjectItemCaseSensitive(doc->root, section);
    if (!cJSON_IsObject(sec)) return false;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(sec, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    return strcmp(item->valuestring, value ? value : "") == 0;
}

static bool prefsDocNameIsDeviceLocal(const char *name,
                                      const char *const *sections,
                                      size_t count) {
    if (!name || !sections) return false;
    for (size_t i = 0; i < count; i++) {
        if (sections[i] && strcmp(name, sections[i]) == 0) return true;
    }
    return false;
}

int prefsDocAdoptUploadEligible(PrefsDoc *doc, const char *serverPrefsJson,
                                const char *const *deviceLocalSections,
                                size_t count) {
    if (!doc || !serverPrefsJson) return PREFS_ADOPT_MALFORMED;

    cJSON *server = cJSON_Parse(serverPrefsJson);
    if (!server) return PREFS_ADOPT_MALFORMED;
    if (!cJSON_IsObject(server)) {
        cJSON_Delete(server);
        return PREFS_ADOPT_MALFORMED;
    }

    /* Version gate. A missing _version is treated as 1, matching the
     * empty-document default. Reject (without touching doc) anything a
     * newer client wrote. */
    int serverVersion = 1;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(server, "_version");
    if (v && cJSON_IsNumber(v)) serverVersion = v->valueint;
    if (serverVersion > PREFS_SCHEMA_VERSION) {
        cJSON_Delete(server);
        return PREFS_ADOPT_VERSION_TOO_NEW;
    }

    /* Drop every non-device-local top-level member (upload-eligible
     * sections and _version), leaving the device-local sections in place.
     * Rescan from the head after each delete so the iteration stays valid. */
    bool removedAny = true;
    while (removedAny) {
        removedAny = false;
        for (cJSON *child = doc->root->child; child; child = child->next) {
            if (child->string &&
                !prefsDocNameIsDeviceLocal(child->string, deviceLocalSections,
                                           count)) {
                cJSON_DeleteItemFromObjectCaseSensitive(doc->root, child->string);
                removedAny = true;
                break;
            }
        }
    }

    /* Graft in every non-device-local member of the server document (its
     * upload-eligible sections and _version). Device-local sections the
     * server may have echoed back are skipped defensively. */
    for (cJSON *child = server->child; child; child = child->next) {
        if (!child->string) continue;
        if (prefsDocNameIsDeviceLocal(child->string, deviceLocalSections, count)) {
            continue;
        }
        cJSON *dup = cJSON_Duplicate(child, 1 /* recurse */);
        if (!dup) {
            cJSON_Delete(server);
            return PREFS_ADOPT_MALFORMED;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(doc->root, child->string);
        cJSON_AddItemToObject(doc->root, child->string, dup);
    }

    doc->dirty = true;
    cJSON_Delete(server);
    return PREFS_ADOPT_OK;
}

int prefsDocVersion(const PrefsDoc *doc) {
    if (!doc) return 1;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(doc->root, "_version");
    if (v && cJSON_IsNumber(v)) return v->valueint;
    return 1;
}

bool prefsDocIsDirty(const PrefsDoc *doc) {
    return doc ? doc->dirty : false;
}

void prefsDocClearDirty(PrefsDoc *doc) {
    if (doc) doc->dirty = false;
}

void prefsDocFree(PrefsDoc *doc) {
    if (!doc) return;
    cJSON_Delete(doc->root);
    free(doc);
}
