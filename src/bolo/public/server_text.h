/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Server Text
 *Filename:      server_text.h
 *Purpose:
 *  The short name and the longer description a host gives
 *  a server, which the game finder shows in place of the
 *  server's address. The caps here are the wire caps of
 *  the info reply (netpacks.h INFO_SERVER_NAME_MAX /
 *  INFO_SERVER_DESC_MAX), in bytes of UTF-8, and the
 *  sanitiser is the one rule every writer and reader
 *  applies, so a server and a game finder agree on what
 *  a name may hold.
 *********************************************************/

#ifndef SERVER_TEXT_H
#define SERVER_TEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest name and description, in bytes, with no NUL. Plain English
 * text gets one character per byte; other UTF-8 gets fewer characters. */
#define SERVER_NAME_MAX 32
#define SERVER_DESC_MAX 200

/* Buffer sizes: the cap plus a NUL. */
#define SERVER_NAME_LEN (SERVER_NAME_MAX + 1)
#define SERVER_DESC_LEN (SERVER_DESC_MAX + 1)

/*
 * Clean a host-given name or description into out:
 *   - bytes that are not well-formed UTF-8 are dropped;
 *   - control characters (C0, DEL, C1) and the zero-width and
 *     bidirectional-override marks are dropped, except that whitespace
 *     (tab, newline and the other C0 spaces, NEL, no-break space, and the
 *     line and paragraph separators) becomes a plain space;
 *   - runs of spaces become one space, and leading and trailing spaces go;
 *   - the result is cut to maxBytes, never inside a character.
 * The output is one line of printable text. in may be NULL, which gives "".
 * outSize is the size of out including the NUL; out is always
 * NUL-terminated when outSize > 0. Returns the length of out in bytes.
 */
size_t serverTextSanitize(const char *in, char *out, size_t outSize,
                          size_t maxBytes);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_TEXT_H */
