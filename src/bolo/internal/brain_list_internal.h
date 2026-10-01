/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          Brain list (internal)
 * Filename:      brain_list_internal.h
 * Purpose:
 *   Server-private bits that go alongside the public
 *   BrainList catalogue: the on-disk path width used for the
 *   paths-mirror, and the brainListScan entry point itself.
 *   Disk paths are not part of the public catalogue or the
 *   wire payload — only the server (and brain_list.c) need
 *   to know about them.
 *********************************************************/

#ifndef BRAIN_LIST_INTERNAL_H
#define BRAIN_LIST_INTERNAL_H

#include "brain_list.h"

#define BRAIN_LIST_PATH_LEN   256

/* Scan brains/ (and brains/ at SDL_GetBasePath when present) for any
 * sub-directory containing an init.lua. Fills `out` with up to
 * BRAIN_LIST_MAX entries sorted alphabetically by name. The version
 * field is set from the recursive max-mtime of .lua files inside the
 * brain directory; empty when no .lua files are found.
 *
 * Also populates `paths` in lockstep with `out`: paths[i] carries the
 * disk path for out->entries[i] after the function returns. Caller
 * provides storage for BRAIN_LIST_MAX rows. Pass NULL to skip the
 * paths-mirror fill. */
void brainListScan(BrainList *out, char (*paths)[BRAIN_LIST_PATH_LEN]);

/* Scan a single parent directory for child dirs that hold a brain (a
 * sub-dir with an init.lua) and append them to `out`/`paths`.
 * brainListScan calls this once per candidate root. Exposed so the unit
 * test can pin the stored path to the directory actually scanned. */
void brainListScanParent(BrainList *out,
                         char (*paths)[BRAIN_LIST_PATH_LEN],
                         const char *parent);

#endif /* BRAIN_LIST_INTERNAL_H */
