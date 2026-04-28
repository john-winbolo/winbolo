/*
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*********************************************************
 * Name:          playername_validate.c
 * Purpose:       Player-name validation, normalization, and
 *                comparison primitives.  Implements Phase 2 of
 *                plans/playername.md.
 *********************************************************/

#include "playername_validate.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <ctype.h>
#else
#include <strings.h>
#endif

#include "netpacks.h"
#include "utf8proc.h"

#ifdef _WIN32
/* MSVC / MinGW exposes _stricmp instead of strcasecmp. */
#define winbolo_strcasecmp _stricmp
#else
#define winbolo_strcasecmp strcasecmp
#endif

/*
 * Wire-compat name byte cap (Phase 1.5).  PACKET_MAX_PLAYER_NAME is the
 * fixed-size on-wire field including the NUL terminator, so the usable
 * payload is one byte less.
 */
#define WIRE_NAME_BYTES_MAX (PACKET_MAX_PLAYER_NAME - 1)

#define UNVERIFIED_SUFFIX "-unverified"
#define UNVERIFIED_SUFFIX_LEN 11

/* -----------------------------------------------------------------
 * Codepoint predicates
 * ----------------------------------------------------------------- */

static bool isWhitespaceCp(int32_t cp) {
    /* ASCII whitespace */
    if (cp == 0x09 || (cp >= 0x0A && cp <= 0x0D)) return true;
    if (cp == 0x20) return true;
    /* Unicode whitespace classes commonly abused in user-supplied names. */
    if (cp == 0x00A0) return true;                 /* NBSP */
    if (cp == 0x1680) return true;                 /* OGHAM SPACE MARK */
    if (cp >= 0x2000 && cp <= 0x200A) return true; /* en/em spaces */
    if (cp == 0x2028 || cp == 0x2029) return true; /* line/paragraph sep */
    if (cp == 0x202F) return true;                 /* NARROW NBSP */
    if (cp == 0x205F) return true;                 /* MEDIUM MATHEMATICAL SPACE */
    if (cp == 0x3000) return true;                 /* IDEOGRAPHIC SPACE */
    if (cp == 0xFEFF) return true;                 /* BOM / ZWNBSP */
    return false;
}

static bool isDisallowedCp(int32_t cp) {
    /* C0/C1 controls + DEL */
    if (cp <= 0x001F) return true;
    if (cp == 0x007F) return true;
    if (cp >= 0x0080 && cp <= 0x009F) return true;
    /* Zero-width formatting */
    if (cp >= 0x200B && cp <= 0x200F) return true;
    /* Bidi formatting (legacy) */
    if (cp >= 0x202A && cp <= 0x202E) return true;
    /* Word joiner / invisible operators */
    if (cp >= 0x2060 && cp <= 0x2064) return true;
    /* Bidi isolates (Unicode 6.3+) */
    if (cp >= 0x2066 && cp <= 0x2069) return true;
    /* BOM */
    if (cp == 0xFEFF) return true;
    return false;
}

/* -----------------------------------------------------------------
 * Single-script rule
 *
 * Per plans/playername.md: a name is allowed if its codepoints are all
 * in one core script, OR all in a permitted multi-script bundle.  Common
 * (digits, generic punctuation) and Inherited (combining marks) attach
 * to whatever script is present.  Scripts not enumerated here default to
 * PNS_OTHER and the name is rejected.
 * ----------------------------------------------------------------- */

#define PNS_LATIN     (1u << 0)
#define PNS_CYRILLIC  (1u << 1)
#define PNS_HAN       (1u << 2)
#define PNS_HIRAGANA  (1u << 3)
#define PNS_KATAKANA  (1u << 4)
#define PNS_COMMON    (1u << 5)
#define PNS_INHERITED (1u << 6)
#define PNS_OTHER     (1u << 7)

static unsigned classifyCp(int32_t cp) {
    /* ASCII fast path: letters → Latin, everything else → Common. */
    if (cp < 0x80) {
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))
            return PNS_LATIN;
        return PNS_COMMON;
    }
    /* Latin-1 punctuation and symbols */
    if (cp >= 0x00A0 && cp <= 0x00BF) {
        if (cp == 0x00AA || cp == 0x00BA) return PNS_LATIN; /* ª º */
        return PNS_COMMON;
    }
    /* Latin-1 letters */
    if (cp >= 0x00C0 && cp <= 0x00FF) {
        if (cp == 0x00D7 || cp == 0x00F7) return PNS_COMMON; /* × ÷ */
        return PNS_LATIN;
    }
    /* Latin Extended-A/B, IPA, spacing modifiers */
    if (cp >= 0x0100 && cp <= 0x02FF) return PNS_LATIN;
    /* Combining diacritical marks */
    if (cp >= 0x0300 && cp <= 0x036F) return PNS_INHERITED;
    /* Cyrillic + Cyrillic Supplement */
    if (cp >= 0x0400 && cp <= 0x052F) return PNS_CYRILLIC;
    /* Latin Extended Additional */
    if (cp >= 0x1E00 && cp <= 0x1EFF) return PNS_LATIN;
    /* General Punctuation */
    if (cp >= 0x2000 && cp <= 0x206F) return PNS_COMMON;
    /* Combining Diacritical Marks for Symbols */
    if (cp >= 0x20D0 && cp <= 0x20FF) return PNS_INHERITED;
    /* Latin Extended-C */
    if (cp >= 0x2C60 && cp <= 0x2C7F) return PNS_LATIN;
    /* Cyrillic Extended-A */
    if (cp >= 0x2DE0 && cp <= 0x2DFF) return PNS_CYRILLIC;
    /* CJK Symbols and Punctuation */
    if (cp >= 0x3000 && cp <= 0x303F) return PNS_COMMON;
    /* Hiragana */
    if (cp >= 0x3041 && cp <= 0x309F) return PNS_HIRAGANA;
    /* Katakana (with U+30FB middle dot belonging to Common) */
    if (cp >= 0x30A0 && cp <= 0x30FF) {
        if (cp == 0x30FB) return PNS_COMMON;
        return PNS_KATAKANA;
    }
    /* Katakana Phonetic Extensions */
    if (cp >= 0x31F0 && cp <= 0x31FF) return PNS_KATAKANA;
    /* CJK Unified Ideographs Extension A */
    if (cp >= 0x3400 && cp <= 0x4DBF) return PNS_HAN;
    /* CJK Unified Ideographs */
    if (cp >= 0x4E00 && cp <= 0x9FFF) return PNS_HAN;
    /* Cyrillic Extended-B */
    if (cp >= 0xA640 && cp <= 0xA69F) return PNS_CYRILLIC;
    /* Latin Extended-D */
    if (cp >= 0xA720 && cp <= 0xA7FF) return PNS_LATIN;
    /* Latin Extended-E */
    if (cp >= 0xAB30 && cp <= 0xAB6F) return PNS_LATIN;
    /* CJK Compatibility Ideographs */
    if (cp >= 0xF900 && cp <= 0xFAFF) return PNS_HAN;
    /* Latin Alphabetic Presentation Forms (ligatures) */
    if (cp >= 0xFB00 && cp <= 0xFB06) return PNS_LATIN;
    /* Variation selectors */
    if (cp >= 0xFE00 && cp <= 0xFE0F) return PNS_INHERITED;
    /* Halfwidth & Fullwidth Forms */
    if (cp >= 0xFF00 && cp <= 0xFFEF) {
        if ((cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A))
            return PNS_LATIN;
        if (cp >= 0xFF66 && cp <= 0xFF9F) return PNS_KATAKANA;
        return PNS_COMMON;
    }
    /* CJK Unified Ideographs Extension B–H */
    if (cp >= 0x20000 && cp <= 0x2FFFF) return PNS_HAN;
    if (cp >= 0x30000 && cp <= 0x3134F) return PNS_HAN;
    /* Variation selectors supplement */
    if (cp >= 0xE0100 && cp <= 0xE01EF) return PNS_INHERITED;
    return PNS_OTHER;
}

static bool isAllowedScriptMask(unsigned mask) {
    if (mask & PNS_OTHER) return false;
    unsigned core = mask & ~(PNS_COMMON | PNS_INHERITED);
    if (core == 0) return true;
    if (core == PNS_LATIN) return true;
    if (core == PNS_CYRILLIC) return true;
    /* Han + Hiragana + Katakana mix is permitted (Japanese / Chinese names). */
    if ((core & ~(PNS_HAN | PNS_HIRAGANA | PNS_KATAKANA)) == 0) return true;
    return false;
}

/* -----------------------------------------------------------------
 * UTF-8 helpers
 * ----------------------------------------------------------------- */

static bool isPureAscii(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p & 0x80) return false;
        p++;
    }
    return true;
}

static bool isValidUtf8(const char *s) {
    const utf8proc_uint8_t *p = (const utf8proc_uint8_t *)s;
    size_t total = strlen(s);
    size_t pos = 0;
    while (pos < total) {
        utf8proc_int32_t cp = -1;
        utf8proc_ssize_t n = utf8proc_iterate(p + pos, (utf8proc_ssize_t)(total - pos), &cp);
        if (n <= 0 || cp < 0) return false;
        pos += (size_t)n;
    }
    return true;
}

/* -----------------------------------------------------------------
 * Whitespace strip (Unicode-aware, in place)
 * ----------------------------------------------------------------- */

void playerNameStripWhitespace(char *name) {
    if (!name) return;
    size_t total = strlen(name);
    if (total == 0) return;

    /* ASCII fast path: leading/trailing space + tab only. */
    if (isPureAscii(name)) {
        size_t first = 0;
        while (first < total &&
               (name[first] == ' ' || name[first] == '\t' ||
                name[first] == '\n' || name[first] == '\r' ||
                name[first] == '\v' || name[first] == '\f')) {
            first++;
        }
        size_t end = total;
        while (end > first &&
               (name[end - 1] == ' ' || name[end - 1] == '\t' ||
                name[end - 1] == '\n' || name[end - 1] == '\r' ||
                name[end - 1] == '\v' || name[end - 1] == '\f')) {
            end--;
        }
        if (first == 0) {
            name[end] = '\0';
        } else {
            size_t kept = end - first;
            if (kept > 0) memmove(name, name + first, kept);
            name[kept] = '\0';
        }
        return;
    }

    /* Unicode-aware path */
    size_t firstOffset = 0;
    size_t lastEnd = 0;
    bool sawNonWs = false;
    size_t pos = 0;
    while (pos < total) {
        utf8proc_int32_t cp = -1;
        utf8proc_ssize_t n = utf8proc_iterate(
            (const utf8proc_uint8_t *)(name + pos),
            (utf8proc_ssize_t)(total - pos), &cp);
        if (n <= 0) {
            /* Defensive: shouldn't happen because validate runs first. */
            pos++;
            continue;
        }
        if (!isWhitespaceCp(cp)) {
            if (!sawNonWs) {
                firstOffset = pos;
                sawNonWs = true;
            }
            lastEnd = pos + (size_t)n;
        }
        pos += (size_t)n;
    }

    if (!sawNonWs) {
        name[0] = '\0';
        return;
    }
    size_t kept = lastEnd - firstOffset;
    if (firstOffset > 0) memmove(name, name + firstOffset, kept);
    name[kept] = '\0';
}

/* -----------------------------------------------------------------
 * Codepoint-safe truncation
 * ----------------------------------------------------------------- */

size_t playerNameTruncateUtf8(char *buf, size_t maxBytes) {
    if (!buf) return 0;
    size_t total = strlen(buf);
    if (total <= maxBytes) return total;

    size_t pos = 0;
    size_t lastBoundary = 0;
    while (pos < total) {
        utf8proc_int32_t cp = -1;
        utf8proc_ssize_t n = utf8proc_iterate(
            (const utf8proc_uint8_t *)(buf + pos),
            (utf8proc_ssize_t)(total - pos), &cp);
        if (n <= 0) {
            buf[pos] = '\0';
            return pos;
        }
        if (pos + (size_t)n > maxBytes) break;
        lastBoundary = pos + (size_t)n;
        pos += (size_t)n;
    }
    buf[lastBoundary] = '\0';
    return lastBoundary;
}

/* -----------------------------------------------------------------
 * Validation core
 * ----------------------------------------------------------------- */

static bool hasUnverifiedSuffix(const char *s) {
    size_t n = strlen(s);
    /* Phase 5 also reserves -unverified-<digits>: snip a trailing run of
     * one or more ASCII digits (and its leading '-'), then re-test the
     * shorter prefix for the literal -unverified suffix.  Empty digit
     * runs ("-unverified-") fall through unchanged. */
    const char *lastDash = strrchr(s, '-');
    if (lastDash && lastDash[1] != '\0') {
        bool allDigits = true;
        for (const char *p = lastDash + 1; *p; p++) {
            if (*p < '0' || *p > '9') { allDigits = false; break; }
        }
        size_t shorter = (size_t)(lastDash - s);
        if (allDigits && shorter >= UNVERIFIED_SUFFIX_LEN) {
            const char *cand = s + shorter - UNVERIFIED_SUFFIX_LEN;
            size_t i;
            for (i = 0; i < UNVERIFIED_SUFFIX_LEN; i++) {
                char a = cand[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (a != UNVERIFIED_SUFFIX[i]) break;
            }
            if (i == UNVERIFIED_SUFFIX_LEN) return true;
        }
    }
    if (n < UNVERIFIED_SUFFIX_LEN) return false;
    return winbolo_strcasecmp(s + (n - UNVERIFIED_SUFFIX_LEN), UNVERIFIED_SUFFIX) == 0;
}

static bool validateCommon(const char *in, char *out, size_t outSize,
                           bool allowUnverifiedSuffix,
                           PlayerNameValidationError *err) {
    PlayerNameValidationError dummy;
    if (!err) err = &dummy;
    *err = PLAYER_NAME_OK;

    if (!out || outSize == 0) {
        *err = PLAYER_NAME_ERR_TOO_LONG;
        return false;
    }
    out[0] = '\0';

    if (!in) {
        *err = PLAYER_NAME_ERR_EMPTY;
        return false;
    }

    /* 1. UTF-8 well-formedness. */
    if (!isValidUtf8(in)) {
        *err = PLAYER_NAME_ERR_INVALID_UTF8;
        return false;
    }

    /* 2. NFC-normalize.  utf8proc returns a malloc'd NUL-terminated
     *    UTF-8 buffer; we strip and inspect that. */
    utf8proc_uint8_t *nfc = utf8proc_NFC((const utf8proc_uint8_t *)in);
    if (!nfc) {
        *err = PLAYER_NAME_ERR_INVALID_UTF8;
        return false;
    }

    /* 3. Whitespace strip (Unicode-aware, in place on the NFC buffer). */
    playerNameStripWhitespace((char *)nfc);

    /* 4. Empty after strip. */
    if (nfc[0] == '\0') {
        free(nfc);
        *err = PLAYER_NAME_ERR_EMPTY;
        return false;
    }

    /* 5. + 6. Walk codepoints, checking for disallowed chars and
     *         building the script mask in one pass. */
    {
        const utf8proc_uint8_t *p = nfc;
        size_t total = strlen((const char *)nfc);
        size_t pos = 0;
        unsigned scriptMask = 0;
        bool asciiOnly = isPureAscii((const char *)nfc);

        while (pos < total) {
            utf8proc_int32_t cp = -1;
            utf8proc_ssize_t n = utf8proc_iterate(
                p + pos, (utf8proc_ssize_t)(total - pos), &cp);
            if (n <= 0 || cp < 0) {
                free(nfc);
                *err = PLAYER_NAME_ERR_INVALID_UTF8;
                return false;
            }
            if (isDisallowedCp(cp)) {
                free(nfc);
                *err = PLAYER_NAME_ERR_DISALLOWED_CHAR;
                return false;
            }
            if (!asciiOnly) {
                scriptMask |= classifyCp(cp);
            }
            pos += (size_t)n;
        }

        if (!asciiOnly && !isAllowedScriptMask(scriptMask)) {
            free(nfc);
            *err = PLAYER_NAME_ERR_MIXED_SCRIPTS;
            return false;
        }
    }

    /* 7. Reserved leading-'*' check (was duplicated at imgui_setname.cpp:171). */
    if (nfc[0] == '*') {
        free(nfc);
        *err = PLAYER_NAME_ERR_RESERVED_PREFIX;
        return false;
    }

    /* 8. Reserved -unverified suffix.  Verified path skips this rule;
     *    Phase 5 is responsible for wiring that path through. */
    if (!allowUnverifiedSuffix && hasUnverifiedSuffix((const char *)nfc)) {
        free(nfc);
        *err = PLAYER_NAME_ERR_RESERVED_SUFFIX;
        return false;
    }

    /* 9. Copy to caller's buffer, then truncate at the wire cap. */
    {
        size_t len = strlen((const char *)nfc);
        if (len + 1 > outSize) {
            /* Caller buffer is too small to hold even the post-NFC form
             * before we truncate.  Truncate into the caller buffer
             * instead and continue. */
            size_t copy = outSize - 1;
            memcpy(out, nfc, copy);
            out[copy] = '\0';
            playerNameTruncateUtf8(out, outSize - 1);
        } else {
            memcpy(out, nfc, len + 1);
        }
        free(nfc);
    }

    /* Wire-side cap: at most 63 bytes of usable name (Phase 1.5). */
    size_t cap = WIRE_NAME_BYTES_MAX;
    if (cap > outSize - 1) cap = outSize - 1;
    playerNameTruncateUtf8(out, cap);

    /* Truncation could in principle have removed a trailing combining
     * mark, leaving a base character.  That's still a valid name. */
    if (out[0] == '\0') {
        *err = PLAYER_NAME_ERR_EMPTY;
        return false;
    }

    return true;
}

bool playerNameValidate(const char *in, char *out, size_t outSize,
                        PlayerNameValidationError *err) {
    return validateCommon(in, out, outSize, /*allowUnverifiedSuffix=*/false, err);
}

bool playerNameValidateForVerified(const char *in, char *out, size_t outSize,
                                   PlayerNameValidationError *err) {
    return validateCommon(in, out, outSize, /*allowUnverifiedSuffix=*/true, err);
}

/* -----------------------------------------------------------------
 * Comparison: NFC + ASCII casefold
 * ----------------------------------------------------------------- */

static int asciiToLower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

/* -----------------------------------------------------------------
 * Suffix helper for the verified-priority preempt (Phase 5)
 * ----------------------------------------------------------------- */

bool playerNameMakeUnverifiedSuffix(const char *base, int index,
                                    char *out, size_t outSize) {
    if (!base || !out || outSize == 0) return false;
    if (index < 0 || index > 99) return false;
    if (index == 1) return false;                /* reserved (the bare suffix is the "1") */

    /* Suffix forms (ASCII, exact byte counts):
     *   index == 0    -> "-unverified"        (11 bytes)
     *   index 2..9    -> "-unverified-<N>"    (13 bytes)
     *   index 10..99  -> "-unverified-<NN>"   (14 bytes)
     * Wire cap is 63 bytes; out must hold cap + NUL. */
    size_t suffixLen;
    char suffix[16];
    if (index == 0) {
        memcpy(suffix, "-unverified", 11);
        suffix[11] = '\0';
        suffixLen = 11;
    } else {
        int n = snprintf(suffix, sizeof(suffix), "-unverified-%d", index);
        if (n < 0 || (size_t)n >= sizeof(suffix)) return false;
        suffixLen = (size_t)n;
    }

    if (outSize < suffixLen + 1) return false;

    size_t baseCap;
    if (suffixLen >= WIRE_NAME_BYTES_MAX) {
        baseCap = 0;
    } else {
        baseCap = WIRE_NAME_BYTES_MAX - suffixLen;
    }
    if (baseCap > outSize - 1 - suffixLen) {
        baseCap = outSize - 1 - suffixLen;
    }

    /* Stage the base in `out`, truncate codepoint-safely, then append. */
    size_t baseLen = strlen(base);
    if (baseLen > outSize - 1) baseLen = outSize - 1;
    memcpy(out, base, baseLen);
    out[baseLen] = '\0';
    if (baseLen > baseCap) {
        playerNameTruncateUtf8(out, baseCap);
        baseLen = strlen(out);
    }

    memcpy(out + baseLen, suffix, suffixLen);
    out[baseLen + suffixLen] = '\0';
    return true;
}

int playerNameCompare(const char *a, const char *b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;

    utf8proc_uint8_t *na = utf8proc_NFC((const utf8proc_uint8_t *)a);
    utf8proc_uint8_t *nb = utf8proc_NFC((const utf8proc_uint8_t *)b);
    if (!na || !nb) {
        /* Fall back to byte compare when NFC fails — at least gives a
         * deterministic ordering rather than crashing. */
        int rv = strcmp(a, b);
        free(na);
        free(nb);
        return rv;
    }

    const unsigned char *pa = na;
    const unsigned char *pb = nb;
    int rv = 0;
    while (*pa && *pb) {
        int ca = asciiToLower(*pa);
        int cb = asciiToLower(*pb);
        if (ca != cb) { rv = ca - cb; break; }
        pa++;
        pb++;
    }
    if (rv == 0) {
        if (*pa) rv = 1;
        else if (*pb) rv = -1;
    }
    free(na);
    free(nb);
    return rv;
}
