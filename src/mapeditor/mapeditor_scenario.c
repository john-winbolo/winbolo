/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario.c
 * Purpose:
 *   The scenario script beside a map: finding it, reading
 *   it, writing it back, and holding the text the pane
 *   shows. File handling follows mapeditor_stamp.c, which
 *   reads and writes through stdio.
 *********************************************************/

#include "mapeditor_scenario.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gui/lang.h" /* langGetText — the status line the pane shows */

#include "scenario_host.h"     /* SCN_SCRIPT_MAX_BYTES — the largest script the
                                * server will read, and so the largest the
                                * editor opens */
#include "scenario_validate.h" /* scnScriptPath — the path a script takes
                                * beside a map */

/* ── Status ───────────────────────────────────────────────────────── */

static void meScenarioStatus(MEScenarioState *st, langid id) {
    char *text = langGetText(id);
    snprintf(st->status, sizeof(st->status), "%s", text != NULL ? text : "");
}

/* Hands the buffer over to the state, releasing whatever it held. */
static void meScenarioTake(MEScenarioState *st, char *text, size_t len) {
    free(st->script);
    st->script = text;
    st->scriptLen = len;
}

/* ── Finding the script ───────────────────────────────────────────── */

/* Where a script sits beside a map is the server's rule, and scnScriptPath in
 * src/scenario/scenario_host.c is the one place it is written: a script the
 * editor writes is one the server has to find.
 *
 * The one difference is deliberate: an empty path is refused here rather
 * than resolving to a bare ".scenario.lua", because a map with no file yet
 * has nowhere to put a script. A NULL path and a missing buffer are refused
 * here too, since scnScriptPath reads both without checking them. */
bool meScenarioScriptPathForMap(const char *mapPath, char *out, size_t outLen) {
    if (mapPath == NULL || mapPath[0] == '\0' || out == NULL || outLen == 0) {
        return false;
    }
    return scnScriptPath(mapPath, out, outLen);
}

/* ── Reading ──────────────────────────────────────────────────────── */

/* The whole file, NUL-terminated, into a buffer the caller owns. found says
 * whether a file was there at all, which is the difference between a map
 * that has no script yet and one whose script will not read. tooBig says the
 * file is past the cap, which is refused rather than truncated. A read that
 * stops short of the length the file measured is refused the same way rather
 * than handed back as the script: the bytes that did arrive are not what the
 * file says, and a later Save would put them over it. */
static bool meScenarioReadFile(const char *path, char **out, size_t *outLen,
                               bool *found, bool *tooBig) {
    FILE  *f;
    long   size;
    char  *buf;
    size_t got;

    *out = NULL;
    *outLen = 0;
    *found = false;
    *tooBig = false;

    f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    *found = true;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return false;
    }
    if ((size_t)size > (size_t)SCN_SCRIPT_MAX_BYTES) {
        *tooBig = true;
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return false;
    }
    buf[got] = '\0';

    *out = buf;
    *outLen = got;
    return true;
}

/* ── The state ────────────────────────────────────────────────────── */

void meScenarioInit(MEScenarioState *st) {
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
}

void meScenarioFree(MEScenarioState *st) {
    if (st == NULL) {
        return;
    }
    free(st->script);
    memset(st, 0, sizeof(*st));
}

void meScenarioSetMap(MEScenarioState *st, const char *mapPath) {
    char   path[ME_PATH_MAX];
    char  *text;
    size_t len;
    bool   found;
    bool   tooBig;

    if (st == NULL) {
        return;
    }

    /* No map, or a name a script path cannot be built from: nothing to edit.
     * A map downloaded from WinBolo.net comes through here, since it has no
     * file behind it to put a script beside. */
    if (mapPath == NULL || mapPath[0] == '\0' ||
        !meScenarioScriptPathForMap(mapPath, path, sizeof(path))) {
        meScenarioTake(st, NULL, 0);
        st->scriptPath[0] = '\0';
        st->status[0] = '\0';
        st->dirty = false;
        st->fileOnDisk = false;
        st->readRefused = false;
        st->pushToWidget = true;
        return;
    }

    snprintf(st->scriptPath, sizeof(st->scriptPath), "%s", path);
    st->dirty = false;
    st->pushToWidget = true;

    if (meScenarioReadFile(path, &text, &len, &found, &tooBig)) {
        meScenarioTake(st, text, len);
        st->fileOnDisk = true;
        st->readRefused = false;
        meScenarioStatus(st, STR_MAPEDIT_SCENARIO_LOADED);
        return;
    }

    /* Nothing to show. A file that is there but would not read still counts
     * as being on disk, and is marked as not read, because the empty buffer
     * standing in for it must not be written back over it. */
    meScenarioTake(st, NULL, 0);
    st->fileOnDisk = found;
    st->readRefused = found;
    if (tooBig) {
        meScenarioStatus(st, STR_MAPEDIT_SCENARIO_TOO_BIG);
    } else if (found) {
        meScenarioStatus(st, STR_MAPEDIT_SCENARIO_READ_FAILED);
    } else {
        meScenarioStatus(st, STR_MAPEDIT_SCENARIO_NO_SCRIPT);
    }
}

bool meScenarioSaveForMap(MEScenarioState *st, const char *mapPath) {
    char   path[ME_PATH_MAX];
    FILE  *f;
    size_t len;

    if (st == NULL) {
        return false;
    }
    if (!meScenarioScriptPathForMap(mapPath, path, sizeof(path))) {
        meScenarioStatus(st, STR_MAPEDIT_SCENARIO_WRITE_FAILED);
        return false;
    }

    /* The script this state stands for is on disk and was not read, so the
     * buffer is empty for want of its contents rather than because that is
     * what the file says. Writing it back would destroy the file. Saving
     * under another name writes a new file and is allowed. */
    if (st->readRefused && strcmp(path, st->scriptPath) == 0) {
        meScenarioStatus(st, STR_MAPEDIT_SCENARIO_SAVE_REFUSED);
        return false;
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        meScenarioStatus(st, STR_MAPEDIT_SCENARIO_WRITE_FAILED);
        return false;
    }
    len = (st->script != NULL) ? st->scriptLen : 0;
    if (len > 0 && fwrite(st->script, 1, len, f) != len) {
        fclose(f);
        meScenarioStatus(st, STR_MAPEDIT_SCENARIO_WRITE_FAILED);
        return false;
    }
    if (fclose(f) != 0) {
        meScenarioStatus(st, STR_MAPEDIT_SCENARIO_WRITE_FAILED);
        return false;
    }

    snprintf(st->scriptPath, sizeof(st->scriptPath), "%s", path);
    st->dirty = false;
    st->fileOnDisk = true;
    /* What is on disk at this path is now what the buffer holds. */
    st->readRefused = false;
    meScenarioStatus(st, STR_MAPEDIT_SCENARIO_SAVED);
    return true;
}

void meScenarioAdoptPath(MEScenarioState *st, const char *mapPath) {
    char  path[ME_PATH_MAX];
    FILE *f;

    if (st == NULL) {
        return;
    }
    if (!meScenarioScriptPathForMap(mapPath, path, sizeof(path))) {
        return;
    }

    /* The same name: the state already stands for this file, so nothing moves.
     * Only the answer to "is there a file at this name" is taken again, and
     * that is what Reload asks. A refusal at this name is kept, because it is
     * still the same file that would not open. */
    if (strcmp(path, st->scriptPath) == 0) {
        f = fopen(path, "rb");
        st->fileOnDisk = (f != NULL);
        if (f != NULL) {
            fclose(f);
        }
        return;
    }

    /* A different name, and the buffer holds what a loose script at the old
     * name says. The map is being kept under the new name, so the script has
     * to be there too: a server runs the loose script in preference to the
     * packed one, and a copy with no loose script beside it plays differently
     * from the map it was copied from. The buffer is the widget's text, so a
     * script read with CRLF line endings is written back with LF — that is
     * true of every save the pane makes and needs nothing extra here. */
    if (st->fileOnDisk && !st->readRefused) {
        if (meScenarioSaveForMap(st, mapPath)) {
            return;
        }
        /* The write did not happen and its status line says why. The state
         * still follows the map, with nothing at the new name to reload. */
        snprintf(st->scriptPath, sizeof(st->scriptPath), "%s", path);
        st->fileOnDisk = false;
        st->readRefused = false;
        return;
    }

    /* Nothing was read from a loose file at the old name: either the file is
     * there and would not open, so the empty buffer standing in for it is not
     * a script to copy anywhere, or the text came out of the map's own package
     * and travels inside the map. Re-point without reading or writing. A
     * different name is a different file, so whatever would not open at the
     * old one says nothing about this one. */
    st->readRefused = false;
    snprintf(st->scriptPath, sizeof(st->scriptPath), "%s", path);

    /* The buffer and its dirty flag are left alone; only the answer to "is
     * there a file at the new name" changes, and that is what Reload asks. */
    f = fopen(path, "rb");
    st->fileOnDisk = (f != NULL);
    if (f != NULL) {
        fclose(f);
    }
}

void meScenarioSetPackedScript(MEScenarioState *st, const char *text,
                               size_t len) {
    char *copy;

    if (st == NULL) {
        return;
    }
    if (text == NULL) {
        len = 0;
    }

    copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        return; /* keep what is already there rather than losing it */
    }
    if (len > 0) {
        memcpy(copy, text, len);
    }
    copy[len] = '\0';

    meScenarioTake(st, copy, len);
    /* Not an edit and not a file: the text came out of the map itself. Saving
     * it writes the loose script beside the map, and a loose script is what a
     * server runs in preference to the packed one, so the Save button stays
     * off until the author actually changes something. */
    st->dirty        = false;
    st->fileOnDisk   = false;
    st->readRefused  = false;
    st->pushToWidget = true;
    meScenarioStatus(st, STR_MAPEDIT_SCENARIO_FROM_PACKAGE);
}

bool meScenarioReload(MEScenarioState *st) {
    char  *text;
    size_t len;
    bool   found;
    bool   tooBig;

    if (st == NULL || st->scriptPath[0] == '\0') {
        return false;
    }

    if (!meScenarioReadFile(st->scriptPath, &text, &len, &found, &tooBig)) {
        /* The edits stay in the buffer rather than being thrown away for a
         * read that did not produce anything to replace them with. */
        if (tooBig) {
            meScenarioStatus(st, STR_MAPEDIT_SCENARIO_TOO_BIG);
        } else if (found) {
            meScenarioStatus(st, STR_MAPEDIT_SCENARIO_READ_FAILED);
        } else {
            meScenarioStatus(st, STR_MAPEDIT_SCENARIO_NO_SCRIPT);
        }
        st->fileOnDisk = found;
        st->readRefused = found;
        return false;
    }

    meScenarioTake(st, text, len);
    st->dirty = false;
    st->fileOnDisk = true;
    st->readRefused = false;
    st->pushToWidget = true;
    meScenarioStatus(st, STR_MAPEDIT_SCENARIO_LOADED);
    return true;
}

void meScenarioSetText(MEScenarioState *st, const char *text, size_t len) {
    char *copy;

    if (st == NULL) {
        return;
    }
    if (text == NULL) {
        len = 0;
    }

    /* The same text again is not an edit. The widget reports a change on
     * anything that touches its undo history, so this keeps a keystroke that
     * left the text as it was from marking the script unsaved. */
    if (len == st->scriptLen &&
        (len == 0 || (st->script != NULL && memcmp(st->script, text, len) == 0))) {
        return;
    }

    copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        return; /* keep what is already there rather than losing it */
    }
    if (len > 0) {
        memcpy(copy, text, len);
    }
    copy[len] = '\0';

    meScenarioTake(st, copy, len);
    st->dirty = true;
}

void meScenarioSetStatus(MEScenarioState *st, const char *text) {
    if (st == NULL) {
        return;
    }
    snprintf(st->status, sizeof(st->status), "%s", text != NULL ? text : "");
}

bool meScenarioDirty(const MEScenarioState *st) {
    return st != NULL && st->dirty;
}
