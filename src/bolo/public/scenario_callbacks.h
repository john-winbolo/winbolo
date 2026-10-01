/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Callback Descriptions
 *Filename:      scenario_callbacks.h
 *Author:        John Morrison
 *Purpose:
 *  What a script says each of its engine callbacks does,
 *  as the manifest's callbacks block states it, packed into
 *  one small blob a lobby can be sent and read.
 *
 *  The blob is one script's whole block:
 *
 *    [count 1] then count rows of
 *    [type 1][nameLen 1][name][textLen 1][text]
 *
 *  and no bytes at all for a script that describes nothing,
 *  so a script without the block costs nothing anywhere it
 *  travels. The names are the catalogue's own (on_start,
 *  can_die, ...), the type is one of SCN_CB_TYPE_* below
 *  and the text is the author's sentence.
 *
 *  It travels as the tail of a script's details blob
 *  (scenario_details.h), after the file's own rules.
 *
 *  Here in public/ because the places that hold the same
 *  bytes share no other header: the manifest reader, the
 *  scenario directory's entry, the UDP details packets and
 *  the lobby's details dialog, which is a gui translation
 *  unit and sees public/ alone. Inline, so a target that
 *  reads a blob links nothing for it.
 *********************************************************/

#ifndef SCENARIO_CALLBACKS_H
#define SCENARIO_CALLBACKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A callback's name and its terminator. The longest the catalogue holds is
 * on_base_neutralized, nineteen letters, so 32 is room over; the same width
 * the trigger rows give a hook's name. */
#define SCN_CALLBACK_NAME_LEN 32

/* One description and its terminator: a sentence, not prose. Shorter than
 * the manifest's description (256), which says what the whole file is for;
 * this says what one function in it does, and a script may have a dozen. */
#define SCN_CALLBACK_TEXT_LEN 160

/* What kind of callback a row describes, as the details dialog's Type
 * column names it. The server works it out, because only the server sees the
 * catalogue (scenario_lua.h) and the script:
 *
 *   EVENT   a hook the engine calls to say something happened and whose
 *           return it throws away: every row of SCN_HOOK_LIST;
 *   QUERY   a policy the engine asks and whose answer it uses: every row of
 *           SCN_POLICY_LIST;
 *   TRIGGER a hook no function defines, only a declared trigger's `when`.
 *
 * A byte on the wire, so the numbers are fixed. */
#define SCN_CB_TYPE_EVENT   0
#define SCN_CB_TYPE_QUERY   1
#define SCN_CB_TYPE_TRIGGER 2
#define SCN_CB_TYPE_COUNT   3

/* The most one script's packed block may run to, and the cap the manifest
 * read holds a block to. The block travels in pieces as part of the details
 * blob, so no datagram binds it; the cap keeps a directory entry, which
 * carries the blob inline, and the lobby's per-row copy of it small. A row
 * costs its type and two length bytes plus its name and text, so 2048 is
 * about twenty callbacks at a line of eighty letters each. The details reply
 * gives the whole details blob a two-byte length, which is far above this. */
#define SCN_CALLBACKS_BLOB_MAX 2048

/* What one row adds to a blob. The count byte is paid once, by the first. */
static inline size_t scnCallbacksRowCost(size_t nameLen, size_t textLen) {
    return 3u + nameLen + textLen;
}

/* How many of the n bytes of text fit a description row with its
 * terminator, cut back to the start of a UTF-8 sequence rather than through
 * one: a sentence cut short still reads, half a character is a box. Both
 * readers of a manifest cut with this, so a package's two forms cut a long
 * sentence at the same byte. */
static inline size_t scnCallbacksTextFit(const char *text, size_t n) {
    if (n < SCN_CALLBACK_TEXT_LEN) {
        return n;
    }
    n = SCN_CALLBACK_TEXT_LEN - 1;
    while (n > 0 && ((unsigned char)text[n] & 0xC0u) == 0x80u) {
        n--;
    }
    return n;
}

/* One row onto the end of a blob of *len bytes. False, and the blob left as
 * it was, when the row would take it past cap, either string is empty or too
 * long for its length, or the type is none of SCN_CB_TYPE_*. A blob starts
 * at *len == 0. */
static inline bool scnCallbacksBlobAppend(uint8_t *blob, size_t cap,
                                          size_t *len, uint8_t type,
                                          const char *name,
                                          const char *text) {
    size_t nameLen;
    size_t textLen;
    size_t at;
    size_t need;

    if (blob == NULL || len == NULL || name == NULL || text == NULL) {
        return false;
    }
    nameLen = strlen(name);
    textLen = strlen(text);
    if (nameLen == 0 || nameLen >= SCN_CALLBACK_NAME_LEN || textLen == 0 ||
        textLen >= SCN_CALLBACK_TEXT_LEN || type >= SCN_CB_TYPE_COUNT) {
        return false;
    }
    at   = (*len == 0) ? 1u : *len;
    need = at + scnCallbacksRowCost(nameLen, textLen);
    if (need > cap || (*len > 0 && blob[0] == 255u)) {
        return false;
    }
    if (*len == 0) {
        blob[0] = 0;
    }
    blob[at++] = type;
    blob[at++] = (uint8_t)nameLen;
    memcpy(blob + at, name, nameLen);
    at += nameLen;
    blob[at++] = (uint8_t)textLen;
    memcpy(blob + at, text, textLen);
    at += textLen;
    blob[0]++;
    *len = at;
    return true;
}

/* How many rows a blob holds, or -1 for bytes that are not one: a type
 * past SCN_CB_TYPE_COUNT, a length past what is there, an empty or over-long string, a NUL inside one, bytes
 * left over after the last row, or more than SCN_CALLBACKS_BLOB_MAX of it.
 * Zero bytes is a blob of no rows. Every reader of a blob off the wire asks
 * this first, so the row reader below never has to. */
static inline int scnCallbacksBlobCount(const uint8_t *blob, size_t len) {
    size_t pos = 1;
    int    count;
    int    i;

    if (len == 0) return 0;
    if (blob == NULL || len > SCN_CALLBACKS_BLOB_MAX) return -1;
    count = (int)blob[0];
    if (count == 0) return -1;   /* a blob of nothing is no bytes at all */
    for (i = 0; i < count; i++) {
        size_t nameLen;
        size_t textLen;

        if (pos + 1 >= len || blob[pos] >= SCN_CB_TYPE_COUNT) return -1;
        pos++;
        nameLen = blob[pos++];
        if (nameLen == 0 || nameLen >= SCN_CALLBACK_NAME_LEN ||
            pos + nameLen >= len ||
            memchr(blob + pos, 0, nameLen) != NULL) {
            return -1;
        }
        pos += nameLen;
        textLen = blob[pos++];
        if (textLen == 0 || textLen >= SCN_CALLBACK_TEXT_LEN ||
            pos + textLen > len ||
            memchr(blob + pos, 0, textLen) != NULL) {
            return -1;
        }
        pos += textLen;
    }
    return (pos == len) ? count : -1;
}

/* Row idx of a blob scnCallbacksBlobCount has passed: its type, and its name
 * and text copied out as two terminated strings. False for an idx past the
 * rows, and then *type is SCN_CB_TYPE_EVENT and both strings "". type may be
 * NULL. The buffers are SCN_CALLBACK_NAME_LEN and SCN_CALLBACK_TEXT_LEN
 * wide in every caller; a smaller one is cut, never overrun. */
static inline bool scnCallbacksBlobRow(const uint8_t *blob, size_t len,
                                       int idx, uint8_t *type, char *name,
                                       size_t nameCap, char *text,
                                       size_t textCap) {
    size_t pos = 1;
    int    i;

    if (type != NULL) *type = SCN_CB_TYPE_EVENT;
    if (name != NULL && nameCap > 0) name[0] = '\0';
    if (text != NULL && textCap > 0) text[0] = '\0';
    if (blob == NULL || len == 0 || idx < 0 || idx >= (int)blob[0]) {
        return false;
    }
    for (i = 0; i <= idx; i++) {
        uint8_t rowType = blob[pos++];
        size_t  nameLen = blob[pos++];
        size_t nameAt  = pos;
        size_t textLen;
        size_t textAt;

        pos += nameLen;
        textLen = blob[pos++];
        textAt  = pos;
        pos += textLen;
        if (i == idx) {
            if (type != NULL) *type = rowType;
            if (name != NULL && nameCap > 0) {
                size_t n = (nameLen < nameCap - 1) ? nameLen : nameCap - 1;
                memcpy(name, blob + nameAt, n);
                name[n] = '\0';
            }
            if (text != NULL && textCap > 0) {
                size_t n = (textLen < textCap - 1) ? textLen : textCap - 1;
                memcpy(text, blob + textAt, n);
                text[n] = '\0';
            }
        }
    }
    return true;
}

#endif /* SCENARIO_CALLBACKS_H */
