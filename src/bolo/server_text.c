/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Server Text
 *Filename:      server_text.c
 *Purpose:
 *  The sanitiser for the name and description a host
 *  gives a server. See server_text.h for the rules.
 *********************************************************/

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "server_text.h"
#include "utf8proc.h"

/* Decode one UTF-8 character at s (at most avail bytes). Returns its length
 * in bytes and stores the code point, or returns 0 when the bytes there are
 * not a well-formed character: a stray continuation byte, a lead byte
 * without its continuations, an overlong form, a surrogate, or a code point
 * past U+10FFFF. */
static size_t serverTextDecode(const unsigned char *s, size_t avail,
                               uint32_t *cp) {
    uint32_t c;
    size_t   len;
    size_t   i;

    if (avail == 0) {
        return 0;
    }
    c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        c &= 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        c &= 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        c &= 0x07;
    } else {
        return 0;
    }
    if (len > avail) {
        return 0;
    }
    for (i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        c = (c << 6) | (uint32_t)(s[i] & 0x3F);
    }
    if ((len == 3 && c < 0x800) || (len == 4 && c < 0x10000) ||
        (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) {
        return 0;
    }
    *cp = c;
    return len;
}

/* Characters that separate words: they become one plain space. */
static bool serverTextIsSpace(uint32_t cp) {
    return cp == ' ' || (cp >= 0x09 && cp <= 0x0D) || cp == 0x85 ||
           cp == 0xA0 || cp == 0x2028 || cp == 0x2029;
}

/* Characters that never print, that hide text, or that reorder the text
 * around them: dropped. */
static bool serverTextIsDropped(uint32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) ||
           cp == 0xAD ||                       /* soft hyphen */
           cp == 0x061C ||                     /* Arabic letter mark */
           cp == 0x180E ||                     /* Mongolian vowel separator */
           (cp >= 0x200B && cp <= 0x200F) ||   /* zero-width, LRM, RLM */
           (cp >= 0x202A && cp <= 0x202E) ||   /* bidi embeddings, overrides */
           (cp >= 0x2060 && cp <= 0x2064) ||   /* word joiner, invisibles */
           (cp >= 0x2066 && cp <= 0x2069) ||   /* bidi isolates */
           (cp >= 0xFDD0 && cp <= 0xFDEF) ||   /* noncharacters */
           (cp & 0xFFFE) == 0xFFFE ||          /* U+xFFFE / U+xFFFF */
           cp == 0xFEFF ||                     /* byte order mark */
           (cp >= 0xFFF9 && cp <= 0xFFFB) ||   /* interlinear annotation */
           (cp >= 0xE0000 && cp <= 0xE007F);   /* tag characters */
}

/* A combining mark: it draws on the character before it. */
static bool serverTextIsMark(uint32_t cp) {
    utf8proc_category_t cat = utf8proc_category((utf8proc_int32_t)cp);
    return cat == UTF8PROC_CATEGORY_MN || cat == UTF8PROC_CATEGORY_MC ||
           cat == UTF8PROC_CATEGORY_ME;
}

/* One pass over in into out, which has room for limit bytes and a NUL.
 * Drops what is not UTF-8 and what serverTextIsDropped names, folds spaces
 * and trims the ends. With marks set it also applies the combining-mark
 * rules and keeps each character whole with its marks at the cut: a mark
 * with no character before it (at the start, or after a space) is dropped,
 * a character keeps at most SERVER_TEXT_MARKS_MAX marks, and when a mark
 * does not fit, the character it belongs to goes too. Returns the length
 * of out. */
static size_t serverTextPass(const char *in, char *out, size_t limit,
                             bool marks) {
    const unsigned char *s = (const unsigned char *)in;
    size_t inLen = strlen(in);
    size_t pos = 0;
    size_t i = 0;
    size_t clusterStart = 0;    /* where the last base character's bytes
                                 * (and the space before it) began */
    int    markCount = 0;
    bool   haveBase = false;    /* the last thing written can take a mark */
    bool   pendingSpace = false;

    while (i < inLen) {
        uint32_t cp = 0;
        size_t   len = serverTextDecode(s + i, inLen - i, &cp);
        bool     isMark;
        size_t   need;

        if (len == 0) {
            i++;            /* not UTF-8: drop the byte */
            continue;
        }
        if (serverTextIsSpace(cp)) {
            /* A space waits until a character follows it, so none leads
             * and none trails. */
            pendingSpace = (pos > 0);
            haveBase = false;
            i += len;
            continue;
        }
        if (serverTextIsDropped(cp)) {
            i += len;
            continue;
        }
        isMark = marks && serverTextIsMark(cp);
        if (isMark) {
            if (!haveBase || pendingSpace ||
                markCount >= SERVER_TEXT_MARKS_MAX) {
                i += len;   /* nothing to draw on, or too many */
                continue;
            }
            if (pos + len > limit) {
                pos = clusterStart;  /* the base goes with its mark */
                break;
            }
            memcpy(out + pos, s + i, len);
            pos += len;
            markCount++;
            i += len;
            continue;
        }
        need = len + (pendingSpace ? 1 : 0);
        if (pos + need > limit) {
            break;          /* the cut: never part of a character */
        }
        clusterStart = pos;
        if (pendingSpace) {
            out[pos++] = ' ';
            pendingSpace = false;
        }
        memcpy(out + pos, s + i, len);
        pos += len;
        i += len;
        haveBase = true;
        markCount = 0;
    }
    out[pos] = '\0';
    return pos;
}

size_t serverTextSanitize(const char *in, char *out, size_t outSize,
                          size_t maxBytes) {
    size_t            limit;
    size_t            inLen;
    char             *clean;
    utf8proc_uint8_t *nfc = NULL;
    const char       *src;
    size_t            n;

    if (out == NULL || outSize == 0) {
        return 0;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0;
    }
    limit = maxBytes;
    if (limit > outSize - 1) {
        limit = outSize - 1;
    }

    /* First the bytes utf8proc cannot take go (bad UTF-8, controls), then
     * NFC folds a letter and its marks into one character where Unicode
     * has one, so "e" + U+0301 counts as the single "é" it shows as. The
     * second pass applies the mark rules and the cut. If memory runs out,
     * the text is cut from the raw input without NFC. */
    inLen = strlen(in);
    clean = (char *)malloc(inLen + 1);
    src = in;
    if (clean != NULL) {
        serverTextPass(in, clean, inLen, false);
        nfc = utf8proc_NFC((const utf8proc_uint8_t *)clean);
        src = (nfc != NULL) ? (const char *)nfc : clean;
    }
    n = serverTextPass(src, out, limit, true);
    free(nfc);
    free(clean);
    return n;
}
