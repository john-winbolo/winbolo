/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Process-global preferences over the PrefsDoc module: one in-memory
 * document loaded from WinBolo.json, exposed through string get/set
 * that mirror the classic Get/WritePrivateProfileString signatures
 * minus the file-path argument. There is no INI migration — a fresh
 * install, or a missing/corrupt file, starts from an empty document.
 *
 * Single-threaded by contract (driven from the main loop only); the
 * module holds no locks.
 */

#ifndef WINBOLO_PREFS_H
#define WINBOLO_PREFS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load the process-global preferences document from jsonPath. A
 * missing file starts an empty document, materialized to jsonPath on
 * init. A file that does not parse is renamed to "<jsonPath>.corrupt"
 * and replaced with a fresh empty document (no silent loss; the app
 * always starts). There is no INI migration. Re-callable (frees any
 * previous global document first), which the tests rely on. */
void prefsInit(const char *jsonPath);

/* Free the global document. */
void prefsShutdown(void);

/* Read [section]/key into out (NUL-terminated, truncated to outSize),
 * or defaultVal if absent. Returns length copied excluding NUL.
 * Mirrors GetPrivateProfileString minus the file-path argument. */
unsigned long prefsGetString(const char *section, const char *key,
                             const char *defaultVal,
                             char *out, size_t outSize);

/* Set [section]/key and flush the document to disk atomically.
 * NULL value stored as "". Returns 1 on success, 0 on failure.
 * Mirrors WritePrivateProfileString minus the file-path argument. */
int prefsSetString(const char *section, const char *key,
                   const char *value);

/* Atomically write the document if it has unsaved changes. 1 ok / 0 fail
 * (also 1 when there was nothing to flush). */
int prefsFlush(void);

bool prefsIsDirty(void);

/* Switch this process to debounced autosave: prefsSetString then only
 * marks the document dirty, and the caller must drive prefsPumpAutosave
 * (and prefsFlush at shutdown / before join). Default mode is immediate
 * flush-on-set (unchanged). intervalMs is the trailing-write delay. */
void prefsSetAutosaveDebounce(unsigned intervalMs);

/* Debounced mode only: call frequently (e.g. once per frame) with a
 * monotonic millisecond clock. Flushes the document once intervalMs has
 * elapsed since the most recent change. No-op in immediate mode or when
 * not dirty. Returns true exactly when it performed a flush this call
 * (false otherwise, including immediate mode, not-dirty, or when the
 * interval has not yet elapsed). */
bool prefsPumpAutosave(uint64_t nowMs);

/* ---- Cloud sync (device-local) ---------------------------------------- */

/* Serialize the upload body: the document minus the device-local sections
 * (WINBOLO.NET auth and the DEVICE sync state), which never leave
 * the device. _version and every upload-eligible section are kept. This is
 * the only path that produces an upload body. Newly malloc'd, caller frees;
 * NULL on OOM or before init. Does not mutate the live document. */
char *prefsSerializeForUpload(void);

/* Sync-dirty flag: set automatically when any upload-eligible value
 * changes, cleared by the caller after a successful upload. Persisted in
 * the document (DEVICE/SyncDirty) so it survives a restart. */
bool prefsSyncDirty(void);
void prefsClearSyncDirty(void);

/* Adopt a downloaded server prefs document into the live document,
 * keeping every device-local section untouched. Schema-version gated: a
 * server _version newer than this build's PREFS_SCHEMA_VERSION is
 * rejected and the local document is left unchanged. On apply, clears
 * sync-dirty and flushes. Returns the prefsDocAdoptUploadEligible code
 * (PREFS_ADOPT_OK on apply, PREFS_ADOPT_VERSION_TOO_NEW when the server
 * is newer, PREFS_ADOPT_MALFORMED on bad input) so the caller can tell
 * apply from version-rejected from malformed. Does not set
 * lastSyncedUpdatedAt — the caller does that with the server token via
 * prefsMarkSynced. */
int prefsAdoptServerDocument(const char *serverPrefsJson);

/* Record a successful sync: set lastSyncedUpdatedAt to the server token,
 * clear sync-dirty, persist. Used after a successful PUT and after an
 * adopt. */
void prefsMarkSynced(const char *updatedAt);

/* The opaque server version token the local document was last in sync with
 * (DEVICE/LastSyncedUpdatedAt). Empty string means never synced. */
void prefsGetLastSyncedUpdatedAt(char *out, size_t outSize);
void prefsSetLastSyncedUpdatedAt(const char *token);

#ifdef __cplusplus
}
#endif

#endif
