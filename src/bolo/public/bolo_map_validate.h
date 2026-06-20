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

#endif
