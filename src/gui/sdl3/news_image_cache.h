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

#ifndef NEWS_IMAGE_CACHE_H
#define NEWS_IMAGE_CACHE_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns NULL while the image is still downloading or has failed
 * (rejected by validation, transport error, decode error, size-cap
 * miss). On the first call for a URL, validates and — if valid —
 * kicks a background download. Validation: scheme http or https,
 * host in the WBN allowlist (winbolo.com, www.winbolo.com,
 * winbolo.net, www.winbolo.net — exact match, case-insensitive).
 * Rejected URLs are recorded as permanently failed (no retry).
 * Drains any pending worker results into SDL_Textures before
 * returning, so a fetch that completed since last call surfaces
 * immediately. Must be called only from the UI thread. */
SDL_Texture *newsImageGet(const char *url);

/* Free all cache entries, textures, decoded-pixel buffers, and join
 * any in-flight worker threads. Idempotent. UI-thread only. */
void newsImageCacheShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NEWS_IMAGE_CACHE_H */
