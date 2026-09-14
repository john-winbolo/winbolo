/*
 * Tests for the process-global preferences API in src/common/prefs.c.
 *
 *   - prefs_api_roundtrip:      missing file is materialized on init,
 *                               sets flush (not dirty), values persist
 *                               across shutdown/re-init
 *   - prefs_api_defaults:       missing section/key returns the default,
 *                               NULL default reads back empty
 *   - prefs_api_corrupt_backup: an unparseable file is renamed to
 *                               "<path>.corrupt" and replaced by a fresh
 *                               empty document; the corrupt content is
 *                               discarded, not used
 *
 * Stubbed out on Windows: the API itself is cross-platform, but these
 * tests lean on POSIX unlink/stat for path hygiene.
 */

#include "test_harness.h"

#ifdef _WIN32

int run_prefs_api_roundtrip(void)      { return 0; }
int run_prefs_api_defaults(void)       { return 0; }
int run_prefs_api_corrupt_backup(void) { return 0; }
int run_prefs_api_debounce(void)       { return 0; }
int run_prefs_api_shutdown_flush(void) { return 0; }
int run_prefs_api_upload_excludes_local(void) { return 0; }
int run_prefs_api_sync_dirty(void)            { return 0; }
int run_prefs_api_device_identity(void)       { return 0; }
int run_prefs_api_adopt_server(void)          { return 0; }
int run_prefs_api_mark_synced(void)           { return 0; }

#else

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/prefs.h"
#include "common/prefs_doc.h"

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void cleanup(const char *jsonPath) {
    /* Over the 1024 the callers give utScratchPath, plus ".corrupt": a
     * scratch path carries the whole build directory, so an equal-sized
     * buffer could truncate and silently miss the file. */
    char corruptPath[1100];
    snprintf(corruptPath, sizeof(corruptPath), "%s.corrupt", jsonPath);
    unlink(jsonPath);
    unlink(corruptPath);
}

/* -------------------------------------------------------------------- */

int run_prefs_api_roundtrip(void) {
    char jsonPath[1024];
    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath), "prefs.json"));
    cleanup(jsonPath);

    prefsInit(jsonPath);
    UT_ASSERT_MSG(file_exists(jsonPath),
                  "init did not materialize a missing file at %s", jsonPath);

    UT_ASSERT(prefsSetString("SETTINGS", "Player Name", "Me"));
    UT_ASSERT(prefsSetString("SETTINGS", "Target Address", "127.0.0.1"));
    UT_ASSERT(prefsSetString("KEYS", "Forward", "8"));

    /* Flush-on-set leaves nothing pending. */
    UT_ASSERT_MSG(!prefsIsDirty(), "document dirty after a flushing set");

    char buf[128];
    prefsGetString("SETTINGS", "Player Name", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "Me") == 0, "round-trip mismatch: '%s'", buf);
    prefsGetString("KEYS", "Forward", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "8") == 0, "round-trip mismatch: '%s'", buf);

    prefsShutdown();

    /* Values survive a fresh load of the same file. */
    prefsInit(jsonPath);
    prefsGetString("SETTINGS", "Target Address", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "127.0.0.1") == 0,
                  "value did not persist across re-init: '%s'", buf);
    prefsGetString("KEYS", "Forward", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "8") == 0,
                  "value did not persist across re-init: '%s'", buf);
    prefsShutdown();

    cleanup(jsonPath);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_api_defaults(void) {
    char jsonPath[1024];
    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath), "prefs.json"));
    cleanup(jsonPath);

    prefsInit(jsonPath);

    char buf[64];
    prefsGetString("NOPE", "Missing", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "<default>") == 0,
                  "missing section/key should return default, got '%s'", buf);

    /* NULL default reads back as an empty string. */
    prefsGetString("NOPE", "Missing", NULL, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "") == 0,
                  "NULL default should read back empty, got '%s'", buf);

    prefsShutdown();
    cleanup(jsonPath);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_api_corrupt_backup(void) {
    char jsonPath[1024];
    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath), "prefs.json"));
    char corruptPath[1100];  /* jsonPath + ".corrupt" — see cleanup() */
    snprintf(corruptPath, sizeof(corruptPath), "%s.corrupt", jsonPath);
    cleanup(jsonPath);

    /* Plant an unparseable file. */
    FILE *fp = fopen(jsonPath, "wb");
    UT_ASSERT(fp != NULL);
    UT_ASSERT(fputs("{ not valid json", fp) >= 0);
    UT_ASSERT(fclose(fp) == 0);

    prefsInit(jsonPath);

    /* Corrupt content preserved under the .corrupt sibling. */
    UT_ASSERT_MSG(file_exists(corruptPath),
                  "corrupt file not backed up to %s", corruptPath);
    /* json path rewritten with a valid, freshly materialized document. */
    UT_ASSERT_MSG(file_exists(jsonPath),
                  "json path not rewritten after corrupt recovery");

    char buf[64];
    prefsGetString("SETTINGS", "Player Name", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "<default>") == 0,
                  "corrupt content was not discarded: got '%s'", buf);

    prefsShutdown();
    cleanup(jsonPath);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_api_debounce(void) {
    char jsonPath[1024];
    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath), "prefs.json"));
    cleanup(jsonPath);

    prefsInit(jsonPath);
    prefsSetAutosaveDebounce(15000);

    /* In debounce mode a set marks the document dirty without writing. */
    UT_ASSERT(prefsSetString("SETTINGS", "Player Name", "Me"));
    UT_ASSERT_MSG(prefsIsDirty(), "debounced set should leave document dirty");

    /* Pump before the interval elapses: still pending, not flushed. */
    UT_ASSERT_MSG(!prefsPumpAutosave(1000),
                  "pump within interval should report no flush");
    UT_ASSERT_MSG(prefsIsDirty(), "pump within interval should not flush");

    /* Pump past the interval: the trailing write clears dirty. */
    UT_ASSERT_MSG(prefsPumpAutosave(20000),
                  "pump past interval should report a flush");
    UT_ASSERT_MSG(!prefsIsDirty(), "pump past interval should flush");

    /* The flushed value is on disk. */
    prefsShutdown();
    prefsInit(jsonPath);
    char buf[128];
    prefsGetString("SETTINGS", "Player Name", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "Me") == 0,
                  "debounced value did not persist to disk: '%s'", buf);
    prefsShutdown();

    cleanup(jsonPath);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_api_shutdown_flush(void) {
    char jsonPath[1024];
    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath), "prefs.json"));
    cleanup(jsonPath);

    prefsInit(jsonPath);
    prefsSetAutosaveDebounce(15000);

    /* Dirty, unflushed pending change. */
    UT_ASSERT(prefsSetString("SETTINGS", "Player Name", "Me"));
    UT_ASSERT_MSG(prefsIsDirty(), "debounced set should leave document dirty");

    /* Shutdown must flush before freeing. */
    prefsShutdown();

    prefsInit(jsonPath);
    char buf[128];
    prefsGetString("SETTINGS", "Player Name", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "Me") == 0,
                  "shutdown did not flush pending change: '%s'", buf);
    prefsShutdown();

    cleanup(jsonPath);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_api_upload_excludes_local(void) {
    char jsonPath[1024];
    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath), "prefs.json"));
    cleanup(jsonPath);

    prefsInit(jsonPath);

    /* Upload-eligible sections. */
    UT_ASSERT(prefsSetString("SETTINGS", "Player Name", "Spectre"));
    UT_ASSERT(prefsSetString("KEYS", "Forward", "273"));
    /* Device-local sections that must never leave the device. */
    UT_ASSERT(prefsSetString("WINBOLO.NET", "Token", "secret-token"));
    UT_ASSERT(prefsSetString("WINDOW", "Window X", "42"));
    UT_ASSERT(prefsSetString("MAPEDITOR", "Last Map", "rocket.map"));
    UT_ASSERT(prefsSetString("DEVICE", "DeviceLabel", "steamdeck"));

    char *body = prefsSerializeForUpload();
    UT_ASSERT_MSG(body != NULL, "upload serialization returned NULL");

    PrefsDoc *up = prefsDocParseJson(body);
    free(body);
    UT_ASSERT_MSG(up != NULL, "upload body did not parse as JSON");

    char buf[128];

    /* Upload-eligible content is present. */
    prefsDocGetString(up, "SETTINGS", "Player Name", "<absent>",
                      buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "Spectre") == 0,
                  "SETTINGS missing from upload body: '%s'", buf);
    prefsDocGetString(up, "KEYS", "Forward", "<absent>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "273") == 0,
                  "KEYS missing from upload body: '%s'", buf);
    UT_ASSERT_MSG(prefsDocVersion(up) == 1,
                  "_version missing from upload body");

    /* Device-local sections are absent. */
    prefsDocGetString(up, "WINBOLO.NET", "Token", "<absent>",
                      buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "<absent>") == 0,
                  "WINBOLO.NET leaked into upload body: '%s'", buf);
    prefsDocGetString(up, "DEVICE", "DeviceLabel", "<absent>",
                      buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "<absent>") == 0,
                  "DEVICE leaked into upload body: '%s'", buf);
    prefsDocGetString(up, "WINDOW", "Window X", "<absent>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "<absent>") == 0,
                  "WINDOW leaked into upload body: '%s'", buf);
    prefsDocGetString(up, "MAPEDITOR", "Last Map", "<absent>",
                      buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "<absent>") == 0,
                  "MAPEDITOR leaked into upload body: '%s'", buf);
    prefsDocGetString(up, "LOGVIEWER", "Last Log", "<absent>",
                      buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "<absent>") == 0,
                  "LOGVIEWER leaked into upload body: '%s'", buf);

    prefsDocFree(up);

    /* Serializing for upload must not mutate the live document. */
    prefsGetString("WINBOLO.NET", "Token", "<absent>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "secret-token") == 0,
                  "upload serialization mutated the live document: '%s'", buf);

    prefsShutdown();
    cleanup(jsonPath);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_api_sync_dirty(void) {
    char jsonPath[1024];
    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath), "prefs.json"));
    cleanup(jsonPath);

    prefsInit(jsonPath);
    UT_ASSERT_MSG(!prefsSyncDirty(), "fresh document should not be sync-dirty");

    /* An upload-eligible change sets sync-dirty. */
    UT_ASSERT(prefsSetString("SETTINGS", "Player Name", "Me"));
    UT_ASSERT_MSG(prefsSyncDirty(),
                  "upload-eligible change did not set sync-dirty");

    prefsClearSyncDirty();
    UT_ASSERT_MSG(!prefsSyncDirty(), "clear did not reset sync-dirty");

    /* A device-local change must not set sync-dirty. */
    UT_ASSERT(prefsSetString("WINBOLO.NET", "Token", "abc"));
    UT_ASSERT_MSG(!prefsSyncDirty(),
                  "device-local change wrongly set sync-dirty");

    /* A device-local WINDOW change must not set sync-dirty either. */
    UT_ASSERT(prefsSetString("WINDOW", "Window X", "7"));
    UT_ASSERT_MSG(!prefsSyncDirty(),
                  "device-local WINDOW change wrongly set sync-dirty");

    /* Dirty again, then confirm it persists across a reload. */
    UT_ASSERT(prefsSetString("KEYS", "Forward", "8"));
    UT_ASSERT_MSG(prefsSyncDirty(), "sync-dirty not set before reload");
    prefsShutdown();

    prefsInit(jsonPath);
    UT_ASSERT_MSG(prefsSyncDirty(), "sync-dirty did not persist across re-init");
    prefsShutdown();

    cleanup(jsonPath);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_api_device_identity(void) {
    char jsonPath[1024];
    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath), "prefs.json"));
    cleanup(jsonPath);

    prefsInit(jsonPath);

    /* The last-synced token round-trips. */
    prefsSetLastSyncedUpdatedAt("1700000000");

    char buf[64];
    prefsGetLastSyncedUpdatedAt(buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "1700000000") == 0,
                  "last-synced round-trip failed: '%s'", buf);

    prefsShutdown();

    /* The last-synced token is persisted across re-init. */
    prefsInit(jsonPath);
    prefsGetLastSyncedUpdatedAt(buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "1700000000") == 0,
                  "last-synced did not persist: '%s'", buf);
    prefsShutdown();

    cleanup(jsonPath);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_api_adopt_server(void) {
    char jsonPath[1024];
    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath), "prefs.json"));
    cleanup(jsonPath);

    prefsInit(jsonPath);

    /* Local upload-eligible content. */
    UT_ASSERT(prefsSetString("SETTINGS", "Player Name", "LocalMe"));
    UT_ASSERT(prefsSetString("KEYS", "Forward", "8"));
    /* Device-local content that must survive an adopt untouched. */
    UT_ASSERT(prefsSetString("WINBOLO.NET", "Token", "secret-token"));
    UT_ASSERT(prefsSetString("DEVICE", "DeviceLabel", "steamdeck"));
    UT_ASSERT_MSG(prefsSyncDirty(), "local edits should be sync-dirty before adopt");

    /* Server document with different upload-eligible content, schema v1. */
    const char *serverV1 =
        "{\"_version\":1,"
        "\"SETTINGS\":{\"Player Name\":\"ServerMe\",\"Target Address\":\"10.0.0.1\"},"
        "\"KEYS\":{\"Forward\":\"99\"}}";
    int rc = prefsAdoptServerDocument(serverV1);
    UT_ASSERT_MSG(rc == PREFS_ADOPT_OK, "adopt should apply, got %d", rc);

    char buf[128];
    /* Upload-eligible sections now match the server. */
    prefsGetString("SETTINGS", "Player Name", "<absent>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "ServerMe") == 0,
                  "SETTINGS not adopted from server: '%s'", buf);
    prefsGetString("SETTINGS", "Target Address", "<absent>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "10.0.0.1") == 0,
                  "new server key not adopted: '%s'", buf);
    prefsGetString("KEYS", "Forward", "<absent>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "99") == 0, "KEYS not adopted from server: '%s'", buf);

    /* Device-local sections preserved. */
    prefsGetString("WINBOLO.NET", "Token", "<absent>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "secret-token") == 0,
                  "WINBOLO.NET clobbered by adopt: '%s'", buf);
    prefsGetString("DEVICE", "DeviceLabel", "<absent>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "steamdeck") == 0,
                  "DEVICE label clobbered by adopt: '%s'", buf);

    /* Sync-dirty cleared on apply. */
    UT_ASSERT_MSG(!prefsSyncDirty(), "adopt should clear sync-dirty");

    /* A newer-schema server document is rejected and leaves the doc as-is. */
    const char *serverV2 =
        "{\"_version\":2,"
        "\"SETTINGS\":{\"Player Name\":\"FromTheFuture\"}}";
    rc = prefsAdoptServerDocument(serverV2);
    UT_ASSERT_MSG(rc == PREFS_ADOPT_VERSION_TOO_NEW,
                  "newer schema should be version-rejected, got %d", rc);
    prefsGetString("SETTINGS", "Player Name", "<absent>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "ServerMe") == 0,
                  "version-rejected adopt mutated the doc: '%s'", buf);

    prefsShutdown();
    cleanup(jsonPath);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_api_mark_synced(void) {
    char jsonPath[1024];
    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath), "prefs.json"));
    cleanup(jsonPath);

    prefsInit(jsonPath);

    /* A local edit makes the doc sync-dirty and leaves no synced token. */
    UT_ASSERT(prefsSetString("SETTINGS", "Player Name", "Me"));
    UT_ASSERT_MSG(prefsSyncDirty(), "edit should set sync-dirty");

    const char *token = "0123456789abcdef0123456789abcdef";
    prefsMarkSynced(token);

    char buf[64];
    prefsGetLastSyncedUpdatedAt(buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, token) == 0,
                  "mark-synced did not set token: '%s'", buf);
    UT_ASSERT_MSG(!prefsSyncDirty(), "mark-synced did not clear sync-dirty");

    /* Both persist across a reload. */
    prefsShutdown();
    prefsInit(jsonPath);
    prefsGetLastSyncedUpdatedAt(buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, token) == 0,
                  "synced token did not persist: '%s'", buf);
    UT_ASSERT_MSG(!prefsSyncDirty(), "cleared sync-dirty did not persist");
    prefsShutdown();

    cleanup(jsonPath);
    return 0;
}

#endif /* !_WIN32 */
