/*
 * pool_render_host.h -- builds the pool panel JSON from a raw pool block.
 *
 * A -brain-debug recording stores each bot's pool panel either as the
 * finished JSON (a block that starts with '{', the old form) or as the
 * brain's raw capture (a block that starts with "WBPR1:", see
 * brains/GoalHunter/pool_render.lua). This helper turns a raw block into the
 * same JSON the brain would have built, in a plain lua_State that loads only
 * pool_render.lua and the few files it requires from the brain directory.
 *
 * Not thread safe: call it from one thread (BrainTest's main thread, a tool's
 * main loop, a unit test).
 */
#ifndef POOL_RENDER_HOST_H
#define POOL_RENDER_HOST_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POOL_RENDER_RAW_TAG "WBPR1:"

/* True when blob is a raw pool block (starts with POOL_RENDER_RAW_TAG). */
bool poolRenderIsRaw(const char *blob);

/* A malloc'd, NUL-terminated copy of a string a brain returned (s, len from
 * lua_tolstring). The brain sends a raw pool block as its tag plus the
 * string.buffer bytes, which hold NUL bytes; the recording pipeline carries
 * C strings, so in a raw block each 0x00 byte becomes 0x01 0x02 and each
 * 0x01 byte becomes 0x01 0x03 (pool_render.unpack undoes it). Any other
 * string is copied up to its first NUL, as before. NULL when out of memory. */
char *poolRenderDupBrainString(const char *s, size_t len);

/* The panel JSON for blob, in a new malloc'd string the caller frees.
 * brainDir is the brain directory that holds pool_render.lua (for example
 * "brains/GoalHunter"). A JSON block comes back as an unchanged copy. When
 * the render fails, the result is a small panel JSON that shows the error
 * text, so a viewer still has something to show; NULL only when out of
 * memory. err (optional) gets the error text, or "" on success. */
char *poolRenderToJson(const char *brainDir, const char *blob,
                       char *err, size_t errLen);

/* Closes the Lua state (the next call opens a new one). */
void poolRenderHostShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* POOL_RENDER_HOST_H */
