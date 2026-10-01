/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Owned, in-memory preferences document backed by a cJSON tree and
 * serialized as JSON. One top-level object whose integer "_version"
 * member carries the schema version and whose other members are
 * sections — each a child object of string members, mirroring the
 * string→string semantics of the classic Get/WritePrivateProfileString
 * INI API.
 *
 * The module is self-contained: load/parse, get/set string values,
 * serialize, atomic save, and a dirty flag set on mutation. It is
 * single-threaded (cJSON is not
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

/* Schema version stamped into a fresh document's "_version" member and
 * the highest version this build knows how to adopt from the server. */
#define PREFS_SCHEMA_VERSION 1

/* prefsDocAdoptUploadEligible return codes. */
#define PREFS_ADOPT_OK              0  /* server doc grafted in */
#define PREFS_ADOPT_MALFORMED       1  /* serverPrefsJson missing / not JSON object */
#define PREFS_ADOPT_VERSION_TOO_NEW 2  /* server _version > PREFS_SCHEMA_VERSION */

typedef struct PrefsDoc PrefsDoc;

/* Empty document: {"_version":1}, not dirty. NULL on OOM. */
PrefsDoc *prefsDocNew(void);

/* Parse a JSON document. A doc missing "_version" is treated as
 * version 1. Not dirty. NULL on parse error / OOM. */
PrefsDoc *prefsDocParseJson(const char *jsonText);

/* Load the JSON document at path. If path does not exist, return a
 * fresh empty doc. Returns NULL on OOM, on a read error of an existing
 * file, or if an existing JSON file does not parse. */
PrefsDoc *prefsDocLoad(const char *path);

/* Serialize to a newly malloc'd JSON string (caller frees). NULL on OOM. */
char *prefsDocSerialize(const PrefsDoc *doc);

/* Serialize the document to JSON omitting the named top-level sections.
 * _version and all other sections are kept. Returns a newly malloc'd
 * string (caller frees), NULL on OOM. Does not mutate the document. */
char *prefsDocSerializeExcluding(const PrefsDoc *doc,
                                 const char *const *sections, size_t count);

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

/* True when [section]/key already exists as a string holding exactly
 * value (NULL compared as ""). Lets callers skip a no-op SetString so an
 * unchanged write does not mark the document dirty. */
bool prefsDocStringEquals(const PrefsDoc *doc, const char *section,
                          const char *key, const char *value);

/* Adopt the upload-eligible content of a downloaded server prefs
 * document into doc, keeping every device-local section untouched.
 * Schema-version gated: if serverPrefsJson's _version exceeds
 * PREFS_SCHEMA_VERSION the doc is left unchanged and
 * PREFS_ADOPT_VERSION_TOO_NEW is returned. Otherwise every
 * non-device-local top-level member of doc (its upload-eligible
 * sections and _version) is removed and replaced with the
 * non-device-local members of serverPrefsJson (defensively skipping
 * any device-local section the server may have echoed back) plus its
 * _version; doc is marked dirty. Device-local sections named in
 * deviceLocalSections are never touched. Returns PREFS_ADOPT_OK,
 * PREFS_ADOPT_VERSION_TOO_NEW, or PREFS_ADOPT_MALFORMED. */
int prefsDocAdoptUploadEligible(PrefsDoc *doc, const char *serverPrefsJson,
                                const char *const *deviceLocalSections,
                                size_t count);

int  prefsDocVersion(const PrefsDoc *doc);   /* "_version" */
bool prefsDocIsDirty(const PrefsDoc *doc);   /* set by SetString */
void prefsDocClearDirty(PrefsDoc *doc);      /* cleared by Save too */
void prefsDocFree(PrefsDoc *doc);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PREFS_DOC_H */
