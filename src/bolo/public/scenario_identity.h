/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Script Identity
 *Filename:      scenario_identity.h
 *Purpose:
 *  Who wrote a script and when it last changed, as its
 *  manifest states them, and the small blob the two travel
 *  to a lobby in. Together they tell one edition of a mod
 *  from another that carries the same name:
 *
 *    scenario = {
 *      name    = "Mac Bolo Rules",
 *      author  = "WinBolo",
 *      updated = "2026-10-03T20:47Z",
 *      ...
 *    }
 *
 *  updated is the time the content last changed, written
 *  into the file by its author. It is not the file's
 *  modified time on disk, which a copy or a download moves
 *  and which therefore says nothing about the edition. The
 *  one form taken is an ISO 8601 date and time in UTC to the
 *  minute: YYYY-MM-DDTHH:MMZ. Anything else reads as not
 *  stated.
 *
 *  Both are optional. A script without them loads as it
 *  always did and shows as "unknown"; the validator warns
 *  about each one missing and refuses nothing.
 *
 *  The author is the one text here a stranger writes and
 *  every lobby shows, so every reader cleans it the same
 *  way (scnIdentityCleanAuthor): control characters out,
 *  broken UTF-8 out, spaces trimmed, cut to the length at a
 *  character boundary. Shown with "%s" or TextUnformatted
 *  and never as a format.
 *
 *  The blob is one script's pair:
 *
 *    [authorLen 1][author][updatedLen 1][updated]
 *
 *  It travels behind a script's details in the details
 *  reply (BULK_SCN_DETAILS_FOUND_V3) and once per row in the
 *  trailer of a scenario-list chunk. A reader takes what it
 *  knows and ignores bytes after the pair, so a later field
 *  can follow without an older reader refusing the blob.
 *
 *  Here in public/ for the reason scenario_callbacks.h is:
 *  the manifest readers, the server's UDP layer, the client
 *  transport and the lobby's dialogs all read these bytes
 *  and share no other header. Inline, so a reader links
 *  nothing for it.
 *********************************************************/

#ifndef SCENARIO_IDENTITY_H
#define SCENARIO_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The author and its terminator: a name or a handle, not a sentence. */
#define SCN_AUTHOR_LEN 48

/* "YYYY-MM-DDTHH:MMZ" and its terminator. */
#define SCN_UPDATED_CHARS 17
#define SCN_UPDATED_LEN   (SCN_UPDATED_CHARS + 1)

/* The blob at its largest: both length bytes and both strings full. */
#define SCN_IDENTITY_BLOB_MAX (2 + (SCN_AUTHOR_LEN - 1) + SCN_UPDATED_CHARS)

/* The byte ahead of the identity trailer in a scenario-list chunk: after the
 * last row comes this tag and then one blob per row, in row order. A chunk
 * from an older server ends at its last row, so a reader that finds no tag
 * there knows the rows carry no identity. */
#define SCN_LIST_IDENTITY_TAG 0x49 /* 'I' */

/* How many bytes the UTF-8 sequence at s[0..n) takes when it is whole and
 * well formed, or 0 when it is not: a stray continuation byte, a lead byte no
 * sequence starts with, a sequence cut short, an overlong form, a surrogate
 * or a code point past U+10FFFF. */
static inline size_t scnIdentityUtf8Len(const unsigned char *s, size_t n) {
    unsigned char c = s[0];
    size_t        need;
    size_t        i;
    uint32_t      cp;

    if (c < 0x80u) return 1;
    if (c >= 0xC2u && c <= 0xDFu) {
        need = 2;
        cp   = c & 0x1Fu;
    } else if (c >= 0xE0u && c <= 0xEFu) {
        need = 3;
        cp   = c & 0x0Fu;
    } else if (c >= 0xF0u && c <= 0xF4u) {
        need = 4;
        cp   = c & 0x07u;
    } else {
        return 0;
    }
    if (need > n) return 0;
    for (i = 1; i < need; i++) {
        if ((s[i] & 0xC0u) != 0x80u) return 0;
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    if ((need == 3 && cp < 0x800u) || (need == 4 && cp < 0x10000u) ||
        (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
        return 0;
    }
    return need;
}

/* The author as it may be shown: the len bytes at src with every control
 * character (below space, DEL, and U+0080..U+009F) and every byte of broken
 * UTF-8 dropped, spaces trimmed off both ends, and cut to fit cap with its
 * terminator at a character boundary. out is always terminated when cap is
 * above 0. src may hold NULs, which are control characters like the rest.
 *
 * Answers true when out holds exactly the bytes src did, and false when
 * anything was dropped, trimmed or cut, which is what a validator warns
 * about. */
static inline bool scnIdentityCleanAuthor(char *out, size_t cap,
                                          const char *src, size_t len) {
    const unsigned char *s = (const unsigned char *)src;
    size_t               o = 0;
    size_t               i = 0;
    size_t               start;
    bool                 same = true;

    if (out == NULL || cap == 0) return false;
    out[0] = '\0';
    if (src == NULL) return len == 0;
    while (i < len) {
        size_t step = scnIdentityUtf8Len(s + i, len - i);

        if (step == 0) {
            same = false;
            i++;
            continue;
        }
        if ((step == 1 && (s[i] < 0x20u || s[i] == 0x7Fu)) ||
            (step == 2 && s[i] == 0xC2u && s[i + 1] < 0xA0u)) {
            same = false;
            i += step;
            continue;
        }
        if (o + step > cap - 1) {
            same = false;
            break;
        }
        memcpy(out + o, s + i, step);
        o += step;
        i += step;
    }
    out[o] = '\0';

    while (o > 0 && out[o - 1] == ' ') {
        out[--o] = '\0';
        same = false;
    }
    start = 0;
    while (out[start] == ' ') start++;
    if (start > 0) {
        memmove(out, out + start, o - start + 1);
        same = false;
    }
    return same;
}

/* Whether s is a last-changed time in the one form taken,
 * "YYYY-MM-DDTHH:MMZ", naming a day the calendar has: month 1-12, a day the
 * month holds (29 February in a leap year only), hour 0-23, minute 0-59. */
static inline bool scnIdentityUpdatedValid(const char *s) {
    static const char shape[] = "dddd-dd-ddTdd:ddZ";
    static const int  days[]  = { 31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31 };
    int year;
    int month;
    int day;
    int most;
    int i;

    if (s == NULL || strlen(s) != SCN_UPDATED_CHARS) return false;
    for (i = 0; i < SCN_UPDATED_CHARS; i++) {
        if (shape[i] == 'd') {
            if (s[i] < '0' || s[i] > '9') return false;
        } else if (s[i] != shape[i]) {
            return false;
        }
    }
#define SCN_ID_NUM2(p) (((p)[0] - '0') * 10 + ((p)[1] - '0'))
    year  = SCN_ID_NUM2(s) * 100 + SCN_ID_NUM2(s + 2);
    month = SCN_ID_NUM2(s + 5);
    day   = SCN_ID_NUM2(s + 8);
    if (month < 1 || month > 12 || SCN_ID_NUM2(s + 11) > 23 ||
        SCN_ID_NUM2(s + 14) > 59) {
        return false;
    }
#undef SCN_ID_NUM2
    most = days[month - 1];
    if (month == 2 &&
        ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        most = 29;
    }
    return day >= 1 && day <= most;
}

/* updated as a person reads it, "2026-10-03 20:47 UTC", into out. "" for a
 * value that is not one, which a caller shows as unknown. */
static inline void scnIdentityUpdatedText(const char *updated, char *out,
                                          size_t cap) {
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    if (!scnIdentityUpdatedValid(updated)) return;
    snprintf(out, cap, "%.10s %.5s UTC", updated, updated + 11);
}

/* The pair as a blob, into out, which holds cap bytes. Answers the length,
 * or 0 when it does not fit. Either string may be "" or NULL for not stated;
 * an author is written as far as SCN_AUTHOR_LEN allows and an updated that
 * is not in the one form is written as not stated, so what is written is
 * always what a reader would keep. */
static inline size_t scnIdentityBlobWrite(uint8_t *out, size_t cap,
                                          const char *author,
                                          const char *updated) {
    char   clean[SCN_AUTHOR_LEN];
    size_t aLen;
    size_t uLen;

    if (out == NULL) return 0;
    (void)scnIdentityCleanAuthor(clean, sizeof(clean), author,
                                 author != NULL ? strlen(author) : 0);
    aLen = strlen(clean);
    uLen = scnIdentityUpdatedValid(updated) ? SCN_UPDATED_CHARS : 0;
    if (2 + aLen + uLen > cap) return 0;
    out[0] = (uint8_t)aLen;
    memcpy(out + 1, clean, aLen);
    out[1 + aLen] = (uint8_t)uLen;
    if (uLen > 0) memcpy(out + 2 + aLen, updated, uLen);
    return 2 + aLen + uLen;
}

/* One pair off the front of the len bytes at blob: the author, cleaned, into
 * author, and the updated time into updated when it is one, "" when not.
 * Answers how many bytes the pair took, or -1 for bytes that are not one: a
 * length past what is there, or past the field it names. Bytes after the pair
 * are left for the caller. The buffers are SCN_AUTHOR_LEN and
 * SCN_UPDATED_LEN wide in every caller; a smaller one is cut, never overrun. */
static inline int scnIdentityBlobRead(const uint8_t *blob, size_t len,
                                      char *author, size_t authorCap,
                                      char *updated, size_t updatedCap) {
    char   when[SCN_UPDATED_LEN];
    size_t aLen;
    size_t uLen;

    if (author != NULL && authorCap > 0) author[0] = '\0';
    if (updated != NULL && updatedCap > 0) updated[0] = '\0';
    if (blob == NULL || len < 2) return -1;
    aLen = blob[0];
    if (aLen >= SCN_AUTHOR_LEN || 1 + aLen + 1 > len) return -1;
    uLen = blob[1 + aLen];
    if (uLen > SCN_UPDATED_CHARS || 2 + aLen + uLen > len) return -1;
    if (author != NULL && authorCap > 0) {
        (void)scnIdentityCleanAuthor(author, authorCap,
                                     (const char *)blob + 1, aLen);
    }
    if (uLen == SCN_UPDATED_CHARS) {
        memcpy(when, blob + 2 + aLen, uLen);
        when[uLen] = '\0';
        if (scnIdentityUpdatedValid(when) && updated != NULL &&
            updatedCap > 0) {
            snprintf(updated, updatedCap, "%s", when);
        }
    }
    return (int)(2 + aLen + uLen);
}

#endif /* SCENARIO_IDENTITY_H */
