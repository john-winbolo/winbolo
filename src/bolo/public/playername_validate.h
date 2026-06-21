/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*********************************************************
 * Name:          playername_validate.h
 * Purpose:       Player-name validation, normalization, and
 *                comparison primitives.  See plans/playername.md
 *                Phase 2 for the rule set.
 *********************************************************/

#ifndef WINBOLO_PLAYERNAME_VALIDATE_H
#define WINBOLO_PLAYERNAME_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_NAME_OK = 0,
    PLAYER_NAME_ERR_INVALID_UTF8,
    PLAYER_NAME_ERR_EMPTY,
    PLAYER_NAME_ERR_DISALLOWED_CHAR,    /* zero-width / bidi / control */
    PLAYER_NAME_ERR_MIXED_SCRIPTS,
    PLAYER_NAME_ERR_RESERVED_PREFIX,    /* leading '*' */
    PLAYER_NAME_ERR_RESERVED_SUFFIX,    /* -unverified */
    PLAYER_NAME_ERR_TOO_LONG            /* shouldn't happen post-truncate; defensive */
} PlayerNameValidationError;

/*
 * Validate, NFC-normalize, and (codepoint-safe) length-cap a player name.
 *
 * `in`     — caller-supplied UTF-8 string (may be unsanitised).
 * `out`    — destination buffer; receives the normalized, stripped, and
 *            truncated form on success.  Always NUL-terminated.
 * `outSize`— size of `out` in bytes, including NUL.  Must be >= 64 to fit
 *            the 63-byte wire cap plus terminator.
 * `err`    — optional; receives the failure reason on a `false` return.
 *
 * Returns true on success, false on rejection.  Rules and order are
 * documented in plans/playername.md (Phase 2).  This is the strict
 * (non-verified) variant: the `-unverified` suffix is rejected.
 */
bool playerNameValidate(const char *in, char *out, size_t outSize,
                        PlayerNameValidationError *err);

/*
 * Same as playerNameValidate, but allows the `-unverified` suffix.  Used
 * for the verified-WBN exception in Phase 5.  Wired by callers as needed.
 */
bool playerNameValidateForVerified(const char *in, char *out, size_t outSize,
                                   PlayerNameValidationError *err);

/*
 * Compare two player names for equality after NFC + ASCII casefold.
 * Returns 0 if equal, <0 / >0 otherwise (same convention as strcmp).
 * Used by collision-detection sites.
 */
int playerNameCompare(const char *a, const char *b);

/*
 * Truncate `buf` (NUL-terminated UTF-8) to at most `maxBytes` bytes,
 * cutting on a codepoint boundary.  Always NUL-terminates.  Returns
 * the resulting byte length (excluding NUL).
 */
size_t playerNameTruncateUtf8(char *buf, size_t maxBytes);

/*
 * Internal: Unicode-aware whitespace strip.  Called by playerNameValidate
 * and exposed for legacy utilStripName shimming.  Modifies `name` in
 * place (NUL-terminated).
 */
void playerNameStripWhitespace(char *name);

/*
 * Build a "<base>-unverified" or "<base>-unverified-<N>" name into `out`.
 *   index == 0       -> "<base>-unverified".
 *   index in [2,99]  -> "<base>-unverified-<N>".
 *   index 1 is reserved/unused; the bare suffix is the "1".
 * If the resulting name would exceed the 63-byte wire cap, the base is
 * truncated codepoint-safely via playerNameTruncateUtf8 first, then the
 * (ASCII) suffix is appended.
 *
 * Returns true on success (out is NUL-terminated), false if outSize is
 * insufficient or index is out of range.
 */
bool playerNameMakeUnverifiedSuffix(const char *base, int index,
                                    char *out, size_t outSize);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PLAYERNAME_VALIDATE_H */
