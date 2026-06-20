/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Starred (favourited) map-chooser entries.
 *
 * Persisted across sessions as a flat text file in SDL_GetPrefPath/
 * map_stars.txt — one entry per line, TAB-separated:
 *
 *   <scope>\t<path>\t<isFolder 0|1>\t<displayName>
 *
 * `scope` matches MapFsProvider::cacheScope ("server", "upload",
 * "wbn", or "" for the default). Same key the thumbnail cache uses,
 * so identically-named entries on different sources stay distinct.
 *
 * The chooser UI consults this layer to:
 *   - render a star icon column at the right edge of every row, and
 *     toggle the star on click;
 *   - render a "Starred" bar above the regular list (entries that
 *     belong to the current scope, regardless of which folder
 *     they live in).
 */

#ifndef WINBOLO_MAP_STARS_H
#define WINBOLO_MAP_STARS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load the persisted star set from disk. Idempotent — safe to call
 * more than once; subsequent calls are no-ops. Call once at app
 * startup before the first lobby render. */
void mapStarsInit(void);

/* True if (scope, path) is currently starred. */
bool mapStarsIsStarred(const char *scope, const char *path);

/* Toggle the star state for (scope, path). `name` and `isFolder` are
 * recorded so the "Starred" bar at the top of the list can render
 * the entry without re-walking the provider's tree. Persists to disk
 * synchronously (the file is tiny — typically dozens of lines). */
void mapStarsToggle(const char *scope, const char *path,
                    const char *name, bool isFolder);

/* Iterate the starred entries for one scope, in the order they were
 * starred. Returns the count visited. `cb` receives the entry path,
 * name and isFolder flag. Stops early if cb returns false. */
typedef bool (*MapStarsVisitor)(const char *path, const char *name,
                                 bool isFolder, void *ctx);
size_t mapStarsVisitScope(const char *scope, MapStarsVisitor cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_MAP_STARS_H */
