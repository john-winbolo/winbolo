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

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "server_text.h"

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

/* Characters that never print or that reorder the text around them:
 * dropped. */
static bool serverTextIsDropped(uint32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) ||
           (cp >= 0x200B && cp <= 0x200F) ||   /* zero-width, LRM, RLM */
           (cp >= 0x202A && cp <= 0x202E) ||   /* bidi embeddings, overrides */
           (cp >= 0x2060 && cp <= 0x2064) ||   /* word joiner, invisibles */
           (cp >= 0x2066 && cp <= 0x2069) ||   /* bidi isolates */
           cp == 0xFEFF;                       /* byte order mark */
}

size_t serverTextSanitize(const char *in, char *out, size_t outSize,
                          size_t maxBytes) {
    const unsigned char *s = (const unsigned char *)in;
    size_t inLen;
    size_t pos = 0;
    size_t limit;
    size_t i = 0;
    bool   pendingSpace = false;

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
    inLen = strlen(in);
    while (i < inLen) {
        uint32_t cp = 0;
        size_t   len = serverTextDecode(s + i, inLen - i, &cp);
        size_t   need;

        if (len == 0) {
            i++;            /* not UTF-8: drop the byte */
            continue;
        }
        if (serverTextIsSpace(cp)) {
            /* A space waits until a character follows it, so none leads
             * and none trails. */
            pendingSpace = (pos > 0);
            i += len;
            continue;
        }
        if (serverTextIsDropped(cp)) {
            i += len;
            continue;
        }
        need = len + (pendingSpace ? 1 : 0);
        if (pos + need > limit) {
            break;          /* the cut: never part of a character */
        }
        if (pendingSpace) {
            out[pos++] = ' ';
            pendingSpace = false;
        }
        memcpy(out + pos, s + i, len);
        pos += len;
        i += len;
    }
    out[pos] = '\0';
    return pos;
}
