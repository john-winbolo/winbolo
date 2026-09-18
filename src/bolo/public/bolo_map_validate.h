/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BOLO_MAP_VALIDATE_H
#define BOLO_MAP_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>

/* Read `path` and report whether it is a readable, well-formed Bolo map.
 * On success, copies the map's stored name into outMapName (NUL-terminated,
 * truncated to outMapNameSize-1 if too long). Pass NULL/0 to skip. Caller
 * never sees the decoded sim state — the function allocates scratch
 * buffers, runs mapRead against them, and frees them before returning. */
bool boloMapValidate(const char *path, char *outMapName, size_t outMapNameSize);

/* How many bytes of `data` are the map itself: the preamble, the pills,
 * bases and starts, and the runs through the terminator. Anything after
 * that is not map data. False when the buffer does not hold a whole,
 * well-formed BMAP.
 *
 * A measure rather than a parse: it decodes no terrain, builds nothing and
 * allocates nothing, and every step checks the bytes are there before it
 * walks over them. The parser cannot answer this — mapReadRuns reads one
 * more run header after the terminator, so its position is up to four bytes
 * past the end. */
bool boloMapBodyLength(const unsigned char *data, size_t len, size_t *outLen);

#endif
