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

/* Asks for an image that is about to be drawn. Returns NULL while the
 * image is still downloading or has failed (rejected by validation,
 * transport error, decode error, size-cap miss). On the first call
 * for a URL, validates and — if valid — kicks a background download.
 * Validation: scheme http or https, host in the WBN allowlist
 * (winbolo.com, www.winbolo.com, winbolo.net, www.winbolo.net — exact
 * match, case-insensitive). Rejected URLs are recorded as permanently
 * failed (no retry). A returned texture counts as drawn this frame and
 * is not evicted for two frames. Drains any pending worker results
 * into SDL_Textures before returning, so a fetch that completed since
 * last call surfaces immediately. An image that was refused for room
 * earlier is downloaded again once the budget could take it. Call
 * this only for an image in view; use newsImagePeek for one that is
 * not. Must be called only from the UI thread. */
SDL_Texture *newsImageGet(const char *url);

/* Looks at an image without asking for it: nothing is downloaded,
 * drained or counted as drawn, so the image may be evicted later. For
 * a loaded image returns true and writes its texture and size, so an
 * image scrolled out of view keeps its place in the layout. Returns
 * false otherwise. The texture must not be drawn: it may be evicted
 * before the frame is rendered. UI-thread only. */
bool newsImagePeek(const char *url, SDL_Texture **tex, float *w, float *h);

/* True once url's image has failed for good: refused by the host
 * allowlist, or a download, decode or size failure. Such a URL is
 * never downloaded again in this process. An image refused for room
 * in the cache is not failed; it is fetched again when it fits.
 * UI-thread only. */
bool newsImageIsFailed(const char *url);

/* How many images have failed since startup. It only goes up, so a
 * caller that saw a different value last time knows to look again
 * with newsImageIsFailed. UI-thread only. */
unsigned newsImageFailedCount(void);

/* Starts a new frame for the cache's eviction rule: an image returned
 * by newsImageGet in this frame or the previous one is never evicted.
 * Call once per frame, before the frame's newsImageGet calls, while
 * images are being drawn. UI-thread only. */
void newsImageCacheNewFrame(void);

/* Free all cache entries, textures, decoded-pixel buffers, and join
 * any in-flight worker threads. Idempotent. UI-thread only. */
void newsImageCacheShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NEWS_IMAGE_CACHE_H */
