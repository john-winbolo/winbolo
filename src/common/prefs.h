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

#ifdef __cplusplus
}
#endif

#endif
