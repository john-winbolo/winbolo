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

/*
 * The recap clip GIF's length rules and gallery metadata. See clip_gif_core.h.
 */

#include "clip_gif_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cJSON.h"

/* ── Length ────────────────────────────────────────────────────────── */

uint32_t clipGifLenClampMs(uint32_t lenMs, uint32_t curMs, uint32_t totalMs) {
    /* Cap before rounding, so the rounding below cannot overflow. */
    if (lenMs > CLIP_GIF_LEN_MAX_MS) {
        lenMs = CLIP_GIF_LEN_MAX_MS;
    }
    lenMs = ((lenMs + CLIP_GIF_LEN_STEP_MS / 2u) / CLIP_GIF_LEN_STEP_MS) *
            CLIP_GIF_LEN_STEP_MS;
    if (lenMs > CLIP_GIF_LEN_MAX_MS) {
        lenMs = CLIP_GIF_LEN_MAX_MS;
    }
    if (lenMs < CLIP_GIF_LEN_MIN_MS) {
        lenMs = CLIP_GIF_LEN_MIN_MS;
    }
    if (totalMs > 0u) {
        /* Whole frames only, so the end of the clip is never past the end of
         * the round. */
        uint32_t left = (curMs < totalMs) ? (totalMs - curMs) : 0u;
        left = (left / CLIP_GIF_LEN_STEP_MS) * CLIP_GIF_LEN_STEP_MS;
        if (lenMs > left) {
            lenMs = left;
        }
        if (lenMs < CLIP_GIF_LEN_MIN_MS) {
            lenMs = CLIP_GIF_LEN_MIN_MS;
        }
    }
    return lenMs;
}

uint32_t clipGifLenFromSeconds(float secs, uint32_t curMs, uint32_t totalMs) {
    uint32_t lenMs;
    /* !(secs > 0) is also true for NaN. */
    if (!(secs > 0.0f)) {
        lenMs = 0u;
    } else if (secs > (float)(CLIP_GIF_LEN_MAX_MS / 1000u) + 1.0f) {
        lenMs = CLIP_GIF_LEN_MAX_MS;
    } else {
        lenMs = (uint32_t)(secs * 1000.0f + 0.5f);
    }
    return clipGifLenClampMs(lenMs, curMs, totalMs);
}

float clipGifLenToSeconds(uint32_t lenMs) {
    return (float)lenMs / 1000.0f;
}

/* ── Recording mode ────────────────────────────────────────────────── */

bool clipGifToolsShown(bool recordMode, bool captureActive) {
    return recordMode || captureActive;
}

/* ── Timeline position ─────────────────────────────────────────────── */

float clipGifTimelineX(uint32_t timeMs, uint32_t totalMs, float x0, float x1) {
    if (totalMs == 0u || !(x1 > x0)) {
        return x0;
    }
    if (timeMs > totalMs) {
        timeMs = totalMs;
    }
    return x0 + (float)((double)(x1 - x0) * (double)timeMs / (double)totalMs);
}

uint32_t clipGifLenFromTimelineX(float x, uint32_t curMs, uint32_t totalMs,
                                 float x0, float x1) {
    if (totalMs == 0u || !(x1 > x0)) {
        return clipGifLenClampMs(CLIP_GIF_LEN_MIN_MS, curMs, totalMs);
    }
    double ratio = ((double)x - (double)x0) / ((double)x1 - (double)x0);
    if (!(ratio > 0.0)) {
        ratio = 0.0;
    }
    if (ratio > 1.0) {
        ratio = 1.0;
    }
    uint32_t endMs = (uint32_t)(ratio * (double)totalMs + 0.5);
    uint32_t lenMs = (endMs > curMs) ? (endMs - curMs) : 0u;
    return clipGifLenClampMs(lenMs, curMs, totalMs);
}

/* ── Metadata ──────────────────────────────────────────────────────── */

char *clipGifMetaToJson(const ClipGifMeta *m) {
    if (m == NULL) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "version", CLIP_GIF_META_VERSION);
    cJSON_AddNumberToObject(root, "created", (double)m->createdUnix);
    cJSON_AddStringToObject(root, "map", m->map);
    cJSON *players = cJSON_AddArrayToObject(root, "players");
    int count = m->playerCount;
    if (count < 0) count = 0;
    if (count > CLIP_GIF_META_MAX_PLAYERS) count = CLIP_GIF_META_MAX_PLAYERS;
    for (int i = 0; players != NULL && i < count; i++) {
        cJSON_AddItemToArray(players, cJSON_CreateString(m->players[i]));
    }
    cJSON_AddNumberToObject(root, "durationMs", (double)m->durationMs);
    cJSON_AddNumberToObject(root, "width", (double)m->width);
    cJSON_AddNumberToObject(root, "height", (double)m->height);
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    return text;
}

void clipGifMetaFreeJson(char *json) {
    if (json != NULL) {
        cJSON_free(json);
    }
}

/* A whole number in [lo, hi] read from key, or dflt when the key is missing,
 * is not a number or is out of range. */
static double metaNumber(const cJSON *obj, const char *key, double lo,
                         double hi, double dflt) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(it)) {
        return dflt;
    }
    double v = it->valuedouble;
    if (!(v >= lo && v <= hi)) {
        return dflt;
    }
    return floor(v);
}

static void metaString(const cJSON *obj, const char *key, char *out,
                       size_t outLen) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    out[0] = '\0';
    if (cJSON_IsString(it) && it->valuestring != NULL) {
        SDL_utf8strlcpy(out, it->valuestring, outLen);
    }
}

bool clipGifMetaFromJson(const char *text, size_t len, ClipGifMeta *out) {
    if (out == NULL) {
        return false;
    }
    SDL_memset(out, 0, sizeof(*out));
    if (text == NULL || len == 0) {
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (root == NULL) {
        return false;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    /* The time is what the list is sorted on, so metadata without one is no
     * better than none. 2^53 is the largest whole number a double holds. */
    double created = metaNumber(root, "created", 0.0, 9007199254740992.0, -1.0);
    if (created < 0.0) {
        cJSON_Delete(root);
        return false;
    }
    out->createdUnix = (int64_t)created;
    metaString(root, "map", out->map, sizeof(out->map));

    const cJSON *players = cJSON_GetObjectItemCaseSensitive(root, "players");
    if (cJSON_IsArray(players)) {
        const cJSON *p;
        cJSON_ArrayForEach(p, players) {
            if (out->playerCount >= CLIP_GIF_META_MAX_PLAYERS) {
                break;
            }
            if (cJSON_IsString(p) && p->valuestring != NULL &&
                p->valuestring[0] != '\0') {
                SDL_utf8strlcpy(out->players[out->playerCount], p->valuestring,
                                sizeof(out->players[0]));
                out->playerCount++;
            }
        }
    }
    out->durationMs = (uint32_t)metaNumber(root, "durationMs", 0.0,
                                           4294967295.0, 0.0);
    out->width  = (int)metaNumber(root, "width", 0.0, 65535.0, 0.0);
    out->height = (int)metaNumber(root, "height", 0.0, 65535.0, 0.0);
    out->hasMeta = true;
    cJSON_Delete(root);
    return true;
}

bool clipGifMetaWriteFile(const char *path, const ClipGifMeta *m) {
    if (path == NULL || m == NULL) {
        return false;
    }
    char *text = clipGifMetaToJson(m);
    if (text == NULL) {
        return false;
    }
    bool ok = false;
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (io != NULL) {
        size_t n = SDL_strlen(text);
        ok = (SDL_WriteIO(io, text, n) == n);
        if (!SDL_CloseIO(io)) {
            ok = false;
        }
    }
    clipGifMetaFreeJson(text);
    return ok;
}

bool clipGifMetaReadFile(const char *path, ClipGifMeta *out) {
    if (out == NULL) {
        return false;
    }
    SDL_memset(out, 0, sizeof(*out));
    if (path == NULL) {
        return false;
    }
    size_t len = 0;
    void *data = SDL_LoadFile(path, &len);
    if (data == NULL) {
        return false;
    }
    bool ok = clipGifMetaFromJson((const char *)data, len, out);
    SDL_free(data);
    return ok;
}

static int metaNewestFirst(const void *pa, const void *pb) {
    const ClipGifMeta *a = (const ClipGifMeta *)pa;
    const ClipGifMeta *b = (const ClipGifMeta *)pb;
    if (a->createdUnix != b->createdUnix) {
        return (a->createdUnix > b->createdUnix) ? -1 : 1;
    }
    return -strcmp(a->gifFile, b->gifFile);
}

void clipGifMetaSortNewestFirst(ClipGifMeta *items, int count) {
    if (items != NULL && count > 1) {
        qsort(items, (size_t)count, sizeof(items[0]), metaNewestFirst);
    }
}

bool clipGifMetaPathFor(const char *gifPath, char *out, size_t outLen) {
    if (gifPath == NULL || out == NULL || outLen == 0) {
        return false;
    }
    size_t n = SDL_strlen(gifPath);
    size_t keep = n;
    if (n >= 4 && SDL_strcasecmp(gifPath + n - 4, ".gif") == 0) {
        keep = n - 4;
    }
    if (keep + 5 + 1 > outLen) {
        out[0] = '\0';
        return false;
    }
    SDL_memcpy(out, gifPath, keep);
    SDL_memcpy(out + keep, ".json", 6);
    return true;
}

/* dir and name joined with one separator between them. */
static bool joinPath(char *out, size_t outLen, const char *dir,
                     const char *name) {
    size_t n = SDL_strlen(dir);
    bool hasSep = (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\'));
    int w = SDL_snprintf(out, outLen, "%s%s%s", dir, hasSep ? "" : "/", name);
    return w > 0 && (size_t)w < outLen;
}

int clipGifGalleryScan(const char *dir, ClipGifMeta **out) {
    if (out == NULL) {
        return 0;
    }
    *out = NULL;
    if (dir == NULL || dir[0] == '\0') {
        return 0;
    }
    int found = 0;
    char **names = SDL_GlobDirectory(dir, "*.gif", SDL_GLOB_CASEINSENSITIVE,
                                     &found);
    if (names == NULL || found <= 0) {
        SDL_free(names);
        return 0;
    }
    ClipGifMeta *items =
        (ClipGifMeta *)SDL_calloc((size_t)found, sizeof(ClipGifMeta));
    if (items == NULL) {
        SDL_free(names);
        return 0;
    }
    int count = 0;
    for (int i = 0; i < found; i++) {
        const char *name = names[i];
        char gifPath[1024];
        char metaPath[1024];
        /* A name too long for the fields is skipped rather than listed under
         * a cut name that would not open. */
        if (name == NULL || SDL_strlen(name) >= CLIP_GIF_META_FILE_LEN ||
            SDL_strchr(name, '/') != NULL || SDL_strchr(name, '\\') != NULL ||
            !joinPath(gifPath, sizeof(gifPath), dir, name)) {
            continue;
        }
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(gifPath, &info) ||
            info.type != SDL_PATHTYPE_FILE) {
            continue;
        }
        ClipGifMeta *m = &items[count];
        if (!clipGifMetaPathFor(gifPath, metaPath, sizeof(metaPath)) ||
            !clipGifMetaReadFile(metaPath, m)) {
            /* No metadata, or none that reads: list the GIF under the time
             * the file was last written. */
            SDL_memset(m, 0, sizeof(*m));
            m->createdUnix = (int64_t)(info.modify_time / SDL_NS_PER_SECOND);
            m->hasMeta = false;
        }
        SDL_strlcpy(m->gifFile, name, sizeof(m->gifFile));
        count++;
    }
    SDL_free(names);
    if (count == 0) {
        SDL_free(items);
        return 0;
    }
    clipGifMetaSortNewestFirst(items, count);
    *out = items;
    return count;
}

static bool localDateTime(int64_t createdUnix, SDL_DateTime *dt) {
    SDL_Time t = (SDL_Time)createdUnix * (SDL_Time)SDL_NS_PER_SECOND;
    return SDL_TimeToDateTime(t, dt, true);
}

void clipGifFormatTime(int64_t createdUnix, char *out, size_t outLen) {
    SDL_DateTime dt;
    if (out == NULL || outLen == 0) {
        return;
    }
    if (!localDateTime(createdUnix, &dt)) {
        SDL_strlcpy(out, "----------", outLen);
        return;
    }
    SDL_snprintf(out, outLen, "%04d-%02d-%02d %02d:%02d", dt.year, dt.month,
                 dt.day, dt.hour, dt.minute);
}

void clipGifFormatFileStamp(int64_t createdUnix, char *out, size_t outLen) {
    SDL_DateTime dt;
    if (out == NULL || outLen == 0) {
        return;
    }
    if (!localDateTime(createdUnix, &dt)) {
        SDL_snprintf(out, outLen, "%lld", (long long)createdUnix);
        return;
    }
    SDL_snprintf(out, outLen, "%04d%02d%02d-%02d%02d%02d", dt.year, dt.month,
                 dt.day, dt.hour, dt.minute, dt.second);
}

/* ── Text ──────────────────────────────────────────────────────────── */

/* The length of the UTF-8 sequence that starts with byte c: 1 for ASCII and
 * for a byte that cannot start a sequence (it is kept as it is), else 2-4. */
static size_t utf8SeqLen(unsigned char c) {
    if (c >= 0xF0u && c <= 0xF7u) return 4;
    if (c >= 0xE0u) return (c <= 0xEFu) ? 3 : 1;
    if (c >= 0xC0u) return 2;
    return 1;
}

void clipGifUtf8CutEnd(char *s, size_t len) {
    if (s == NULL) {
        return;
    }
    /* Find where the last sequence starts: at most 3 continuation bytes
     * back. */
    size_t start = len;
    size_t back = 0;
    while (start > 0 && back < 4) {
        start--;
        back++;
        if (((unsigned char)s[start] & 0xC0u) != 0x80u) {
            break;
        }
    }
    if (start >= len) {
        s[len] = '\0';
        return;
    }
    unsigned char lead = (unsigned char)s[start];
    if ((lead & 0xC0u) == 0x80u) {
        /* Only continuation bytes at the end: not a sequence this can mend,
         * so it is left as it is. */
        s[len] = '\0';
        return;
    }
    if (utf8SeqLen(lead) > len - start) {
        len = start;  /* the last sequence was cut: drop it */
    }
    s[len] = '\0';
}

void clipGifFillString1(const char *fmt, const char *value, char *out,
                        size_t outLen) {
    static const char token[] = "{string1}";
    const size_t tokenLen = sizeof(token) - 1u;
    if (out == NULL || outLen == 0) {
        return;
    }
    out[0] = '\0';
    if (fmt == NULL) {
        return;
    }
    if (value == NULL) {
        value = "";
    }
    const size_t room = outLen - 1u;
    size_t used = 0;
    bool cut = false;
    const char *p = fmt;
    while (*p != '\0' && !cut) {
        const char *src = p;
        size_t n = 1;
        if (SDL_strncmp(p, token, tokenLen) == 0) {
            src = value;
            n = SDL_strlen(value);
            p += tokenLen;
        } else {
            p++;
        }
        if (n > room - used) {
            n = room - used;
            cut = true;
        }
        SDL_memcpy(out + used, src, n);
        used += n;
    }
    if (cut) {
        clipGifUtf8CutEnd(out, used);
    } else {
        out[used] = '\0';
    }
}
