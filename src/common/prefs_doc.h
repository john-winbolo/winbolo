/*
 * Owned, in-memory preferences document backed by a cJSON tree and
 * serialized as JSON. One top-level object whose integer "_version"
 * member carries the schema version and whose other members are
 * sections — each a child object of string members, mirroring the
 * string→string semantics of the classic Get/WritePrivateProfileString
 * INI API.
 *
 * The module is self-contained: load/parse, get/set string values,
 * serialize, atomic save, a one-time INI→JSON migration on load, and a
 * dirty flag set on mutation. It is single-threaded (cJSON is not
 * thread-safe); callers serialize a string snapshot before handing
 * work to other threads.
 */

#ifndef WINBOLO_PREFS_DOC_H
#define WINBOLO_PREFS_DOC_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PrefsDoc PrefsDoc;

/* Empty document: {"_version":1}, not dirty. NULL on OOM. */
PrefsDoc *prefsDocNew(void);

/* Parse a JSON document. A doc missing "_version" is treated as
 * version 1. Not dirty. NULL on parse error / OOM. */
PrefsDoc *prefsDocParseJson(const char *jsonText);

/* Parse classic INI text (migration source): [section] -> object,
 * key=value -> string member. Stamps _version:1. Blank lines and
 * lines without '=' outside a section header are skipped. Not dirty.
 * NULL on OOM. */
PrefsDoc *prefsDocParseIni(const char *iniText);

/* Load the JSON document at path. If path does not exist and
 * legacyIniPath is non-NULL and exists, migrate from that INI and
 * return the migrated (unsaved, not-dirty) doc. If neither exists,
 * return a fresh empty doc. Returns NULL on OOM, on a read error of an
 * existing file, or if an existing JSON file does not parse. */
PrefsDoc *prefsDocLoad(const char *path, const char *legacyIniPath);

/* Serialize to a newly malloc'd JSON string (caller frees). NULL on OOM. */
char *prefsDocSerialize(const PrefsDoc *doc);

/* Atomic write as JSON to path: sibling temp file created mode 0600,
 * fsync, rename over target. Clears dirty on success. 1 ok / 0 fail. */
int prefsDocSave(PrefsDoc *doc, const char *path);

/* Copy [section]/key into out (NUL-terminated, truncated to outSize),
 * or defaultVal if absent. Returns length copied excluding NUL,
 * mirroring GetPrivateProfileString. */
unsigned long prefsDocGetString(const PrefsDoc *doc, const char *section,
                                const char *key, const char *defaultVal,
                                char *out, size_t outSize);

/* Set [section]/key (creating the section if needed); marks dirty.
 * NULL value stored as "". 1 ok / 0 on invalid args or OOM. Values
 * may contain any bytes — JSON escaping removes the INI injection
 * surface, so control characters are NOT rejected. */
int prefsDocSetString(PrefsDoc *doc, const char *section,
                      const char *key, const char *value);

int  prefsDocVersion(const PrefsDoc *doc);   /* "_version" */
bool prefsDocIsDirty(const PrefsDoc *doc);   /* set by SetString */
void prefsDocClearDirty(PrefsDoc *doc);      /* cleared by Save too */
void prefsDocFree(PrefsDoc *doc);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PREFS_DOC_H */
