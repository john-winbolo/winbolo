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

/* ── Crop frame ────────────────────────────────────────────────────── */

ClipGifBox clipGifFrameToBox(const ClipGifFrame *f, const ClipGifBox *image) {
    ClipGifBox b = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (f == NULL || image == NULL) {
        return b;
    }
    const float w = image->x1 - image->x0;
    const float h = image->y1 - image->y0;
    b.x0 = image->x0 + f->x0 * w;
    b.y0 = image->y0 + f->y0 * h;
    b.x1 = image->x0 + f->x1 * w;
    b.y1 = image->y0 + f->y1 * h;
    return b;
}

void clipGifFrameFromBox(ClipGifFrame *f, const ClipGifBox *box,
                         const ClipGifBox *image) {
    if (f == NULL || box == NULL || image == NULL) {
        return;
    }
    const float w = image->x1 - image->x0;
    const float h = image->y1 - image->y0;
    if (!(w > 0.0f) || !(h > 0.0f)) {
        return;
    }
    f->x0 = (box->x0 - image->x0) / w;
    f->y0 = (box->y0 - image->y0) / h;
    f->x1 = (box->x1 - image->x0) / w;
    f->y1 = (box->y1 - image->y0) / h;
}

int clipGifFrameHit(const ClipGifBox *box, float mx, float my, float grab) {
    if (box == NULL) {
        return 0;
    }
    const float g = grab;
    if (!(mx >= box->x0 - g && mx <= box->x1 + g &&
          my >= box->y0 - g && my <= box->y1 + g)) {
        return 0;
    }
    int hot = 0;
    if (mx >= box->x0 - g && mx <= box->x0 + g) hot |= CLIP_GIF_GRAB_LEFT;
    if (mx >= box->x1 - g && mx <= box->x1 + g) hot |= CLIP_GIF_GRAB_RIGHT;
    if (my >= box->y0 - g && my <= box->y0 + g) hot |= CLIP_GIF_GRAB_TOP;
    if (my >= box->y1 - g && my <= box->y1 + g) hot |= CLIP_GIF_GRAB_BOTTOM;
    if (hot == 0 && mx > box->x0 && mx < box->x1 && my > box->y0 &&
        my < box->y1) {
        hot = CLIP_GIF_GRAB_MOVE;
    }
    return hot;
}

void clipGifFrameDrag(ClipGifBox *box, int grab, float dx, float dy,
                      const ClipGifBox *image, float minSize) {
    if (box == NULL || image == NULL || grab == 0) {
        return;
    }
    float x0 = box->x0, y0 = box->y0, x1 = box->x1, y1 = box->y1;
    if (grab & CLIP_GIF_GRAB_MOVE) {
        /* Both edges together, stopped by the image rather than squashed
         * against it. */
        const float w = x1 - x0, h = y1 - y0;
        x0 += dx;
        y0 += dy;
        if (x0 < image->x0) x0 = image->x0;
        if (y0 < image->y0) y0 = image->y0;
        if (x0 + w > image->x1) x0 = image->x1 - w;
        if (y0 + h > image->y1) y0 = image->y1 - h;
        x1 = x0 + w;
        y1 = y0 + h;
    } else {
        if (grab & CLIP_GIF_GRAB_LEFT)   x0 += dx;
        if (grab & CLIP_GIF_GRAB_RIGHT)  x1 += dx;
        if (grab & CLIP_GIF_GRAB_TOP)    y0 += dy;
        if (grab & CLIP_GIF_GRAB_BOTTOM) y1 += dy;
        if (x0 < image->x0) x0 = image->x0;
        if (y0 < image->y0) y0 = image->y0;
        if (x1 > image->x1) x1 = image->x1;
        if (y1 > image->y1) y1 = image->y1;
        /* No turning inside out: the edge that moves stops minSize short
         * of the edge across from it. */
        if (x1 - x0 < minSize) {
            if (grab & CLIP_GIF_GRAB_LEFT) x0 = x1 - minSize;
            else                           x1 = x0 + minSize;
        }
        if (y1 - y0 < minSize) {
            if (grab & CLIP_GIF_GRAB_TOP) y0 = y1 - minSize;
            else                          y1 = y0 + minSize;
        }
        /* On a very small view that can put an edge out of the image again;
         * once more, so it never is. */
        if (x0 < image->x0) {
            x0 = image->x0;
            if (x1 < x0 + minSize) x1 = x0 + minSize;
        }
        if (y0 < image->y0) {
            y0 = image->y0;
            if (y1 < y0 + minSize) y1 = y0 + minSize;
        }
    }
    box->x0 = x0;
    box->y0 = y0;
    box->x1 = x1;
    box->y1 = y1;
}

bool clipGifCropFromFrame(const ClipGifFrame *f, int viewX, int viewY,
                          int viewW, int viewH, ClipGifRect *out) {
    if (out == NULL) {
        return false;
    }
    SDL_zerop(out);
    if (f == NULL || viewW <= 0 || viewH <= 0) {
        return false;
    }
    int fx0 = (int)(f->x0 * (float)viewW + 0.5f);
    int fy0 = (int)(f->y0 * (float)viewH + 0.5f);
    int fx1 = (int)(f->x1 * (float)viewW + 0.5f);
    int fy1 = (int)(f->y1 * (float)viewH + 0.5f);
    if (fx0 < 0) fx0 = 0;
    if (fy0 < 0) fy0 = 0;
    if (fx1 > viewW) fx1 = viewW;
    if (fy1 > viewH) fy1 = viewH;
    /* At least one pixel, and inside the view. */
    if (fx0 > viewW - 1) fx0 = viewW - 1;
    if (fy0 > viewH - 1) fy0 = viewH - 1;
    int w = fx1 - fx0;
    int h = fy1 - fy0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    out->x = viewX + fx0;
    out->y = viewY + fy0;
    out->w = w;
    out->h = h;
    return true;
}

/* ── The Log Viewer's map ──────────────────────────────────────────── */

/* One axis of clipGifBlitView. srcStart is in render-target pixels, the
 * rest in frame buffer pixels. False when no whole pixel shows. */
static bool clipGifBlitAxis(float srcStart, float srcLen, float dstStart,
                            float dstLen, float areaStart, float areaEnd,
                            int *sliceStart, int *sliceLen, float *viewStart,
                            float *viewEnd) {
    if (!(srcLen > 0.0f) || !(dstLen > 0.0f)) {
        return false;
    }
    const float scale = dstLen / srcLen;  /* frame buffer pixels a pixel */
    float lo = (areaStart > dstStart) ? areaStart : dstStart;
    float hi = (areaEnd < dstStart + dstLen) ? areaEnd : dstStart + dstLen;
    if (!(hi > lo)) {
        return false;
    }
    /* Whole pixels only: the first one that starts in the area and the last
     * one that ends in it. The small allowance keeps a pixel whose edge is
     * on the area's edge, give or take the error of a float. */
    float k0 = ceilf((lo - dstStart) / scale - 0.001f);
    float k1 = floorf((hi - dstStart) / scale + 0.001f);
    if (k0 < 0.0f) k0 = 0.0f;
    if (k1 > floorf(srcLen + 0.001f)) k1 = floorf(srcLen + 0.001f);
    if (!(k1 > k0)) {
        return false;
    }
    *sliceStart = (int)floorf(srcStart + 0.5f) + (int)k0;
    *sliceLen   = (int)(k1 - k0);
    *viewStart  = dstStart + k0 * scale;
    *viewEnd    = dstStart + k1 * scale;
    return true;
}

bool clipGifBlitView(const ClipGifBox *src, const ClipGifBox *dst,
                     const ClipGifBox *area, float pxScale,
                     ClipGifRect *slice, ClipGifBox *view) {
    if (slice == NULL || view == NULL) {
        return false;
    }
    SDL_zerop(slice);
    SDL_zerop(view);
    if (src == NULL || dst == NULL || area == NULL) {
        return false;
    }
    if (!(pxScale > 0.0f)) {
        pxScale = 1.0f;
    }
    ClipGifRect s;
    ClipGifBox  v;
    if (!clipGifBlitAxis(src->x0, src->x1 - src->x0, dst->x0, dst->x1 - dst->x0,
                         area->x0 * pxScale, area->x1 * pxScale, &s.x, &s.w,
                         &v.x0, &v.x1) ||
        !clipGifBlitAxis(src->y0, src->y1 - src->y0, dst->y0, dst->y1 - dst->y0,
                         area->y0 * pxScale, area->y1 * pxScale, &s.y, &s.h,
                         &v.y0, &v.y1)) {
        return false;
    }
    *slice = s;
    view->x0 = v.x0 / pxScale;
    view->y0 = v.y0 / pxScale;
    view->x1 = v.x1 / pxScale;
    view->y1 = v.y1 / pxScale;
    return true;
}

/* ── Recording mode state ──────────────────────────────────────────── */

void clipGifModeReset(ClipGifMode *m) {
    if (m == NULL) {
        return;
    }
    const ClipGifMode init = CLIP_GIF_MODE_INIT;
    *m = init;
}

bool clipGifModeToggle(ClipGifMode *m, bool captureActive) {
    if (m == NULL) {
        return false;
    }
    if (!captureActive) {
        m->on = !m->on;
    }
    return m->on;
}

bool clipGifModeToggleCrop(ClipGifMode *m, bool captureActive) {
    if (m == NULL) {
        return false;
    }
    if (!captureActive) {
        m->cropOn = !m->cropOn;
    }
    return m->cropOn;
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
    /* Left out when no replay was open, so the file reads the same as one
     * written before the field was added. */
    if (m->game[0] != '\0') {
        cJSON_AddStringToObject(root, "game", m->game);
    }
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
    /* Missing in a file written before the field was added: the GIF is then
     * listed with the earlier games. */
    metaString(root, "game", out->game, sizeof(out->game));
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

bool clipGifDeleteFiles(const char *gifPath) {
    if (gifPath == NULL || gifPath[0] == '\0') {
        return false;
    }
    /* The GIF first: when it cannot be removed, its metadata stays with it,
     * so the row the player still sees keeps its details. */
    if (!SDL_RemovePath(gifPath)) {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(gifPath, &info)) {
            return false;  /* still there */
        }
    }
    char metaPath[1024];
    if (clipGifMetaPathFor(gifPath, metaPath, sizeof(metaPath))) {
        /* A GIF with no metadata file is normal, so a failure here is not
         * one the player needs to hear about. */
        SDL_RemovePath(metaPath);
    }
    return true;
}

int clipGifSelectAfterRemove(int count, int removed) {
    if (count <= 1 || removed < 0 || removed >= count) {
        return (count > 1) ? 0 : -1;
    }
    /* The row below moves up into the removed row's place; the last row has
     * none below it, so the row above is selected. */
    return (removed < count - 1) ? removed : removed - 1;
}

/* ── Games ─────────────────────────────────────────────────────────── */

void clipGifGameIdFromBytes(const void *data, size_t len, char *out,
                            size_t outLen) {
    if (out == NULL || outLen == 0) {
        return;
    }
    out[0] = '\0';
    if (data == NULL || len == 0) {
        return;
    }
    /* 64-bit FNV-1a. */
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    SDL_snprintf(out, outLen, "%016llx", (unsigned long long)h);
}

bool clipGifMetaIsCurrentGame(const ClipGifMeta *m, const char *currentGame) {
    if (m == NULL || currentGame == NULL || currentGame[0] == '\0' ||
        m->game[0] == '\0') {
        return false;
    }
    return SDL_strcmp(m->game, currentGame) == 0;
}

int clipGifMetaCurrentGameFirst(ClipGifMeta *items, int count,
                                const char *currentGame) {
    if (items == NULL || count <= 0) {
        return 0;
    }
    /* Each item from the current game moves up to just after the ones
     * already moved, and the items it passes move down one place, so both
     * groups keep their order. The lists are short, so moving the items
     * one by one is fast enough and needs no second array. */
    int front = 0;
    for (int i = 0; i < count; i++) {
        if (!clipGifMetaIsCurrentGame(&items[i], currentGame)) {
            continue;
        }
        if (i != front) {
            ClipGifMeta keep = items[i];
            SDL_memmove(&items[front + 1], &items[front],
                        (size_t)(i - front) * sizeof(items[0]));
            items[front] = keep;
        }
        front++;
    }
    return front;
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

/* ── Capture ─────────────────────────────────────────────────────── */

uint32_t clipGifFrameCount(uint32_t durationMs) {
    uint32_t frames = durationMs / CLIP_GIF_FRAME_MS;
    if (frames > CLIP_GIF_MAX_FRAMES) {
        frames = CLIP_GIF_MAX_FRAMES;
    }
    if (frames < CLIP_GIF_MIN_FRAMES) {
        frames = CLIP_GIF_MIN_FRAMES;
    }
    return frames;
}

bool clipGifCentreCrop(int srcX, int srcY, int srcW, int srcH, int maxW,
                       ClipGifRect *out) {
    if (out == NULL) {
        return false;
    }
    SDL_zerop(out);
    if (srcW <= 0 || srcH <= 0) {
        return false;
    }
    int cropW = srcW;
    int cropH = srcH;
    if (maxW > 0 && cropW > maxW) {
        cropW = maxW;
        cropH = (int)((float)srcH * (float)cropW / (float)srcW);
    }
    if (cropW < 1) cropW = 1;
    if (cropH < 1) cropH = 1;
    if (cropH > srcH) cropH = srcH;
    out->x = srcX + (srcW - cropW) / 2;
    out->y = srcY + (srcH - cropH) / 2;
    out->w = cropW;
    out->h = cropH;
    return true;
}

/* ── File names ──────────────────────────────────────────────────── */

void clipGifBaseName(char *out, size_t outLen, const char *mapName,
                     unsigned mins, unsigned secs) {
    if (out == NULL || outLen == 0) {
        return;
    }
    char base[64];
    if (mapName != NULL && mapName[0] != '\0') {
        snprintf(base, sizeof(base), "%s", mapName);
        size_t blen = SDL_strlen(base);
        if (blen > 4 && SDL_strcmp(base + blen - 4, ".map") == 0) {
            base[blen - 4] = '\0';
        }
    } else {
        snprintf(base, sizeof(base), "clip");
    }
    for (char *p = base; *p != '\0'; p++) {
        char c = *p;
        bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') || c == '-' || c == '_';
        if (!keep) {
            *p = '_';
        }
    }
    snprintf(out, outLen, "%s_%02u%02u", base, mins, secs);
}

bool clipGifFileName(char *out, size_t outLen, const char *baseName,
                     const char *stamp, int n) {
    if (out == NULL || outLen == 0) {
        return false;
    }
    out[0] = '\0';
    if (n < 1 || n > CLIP_GIF_FILE_NAME_MAX_N) {
        return false;
    }
    const char *base =
        (baseName != NULL && baseName[0] != '\0') ? baseName : "clip";
    if (stamp == NULL) {
        stamp = "";
    }
    int len;
    if (n == 1) {
        len = snprintf(out, outLen, "%s_%s.gif", base, stamp);
    } else {
        len = snprintf(out, outLen, "%s_%s_%d.gif", base, stamp, n);
    }
    if (len <= 0 || (size_t)len >= outLen) {
        out[0] = '\0';
        return false;
    }
    return true;
}

void clipGifMetaSetMap(ClipGifMeta *m, const char *mapName) {
    if (m == NULL) {
        return;
    }
    m->map[0] = '\0';
    if (mapName == NULL || mapName[0] == '\0') {
        return;
    }
    SDL_utf8strlcpy(m->map, mapName, sizeof(m->map));
    size_t n = SDL_strlen(m->map);
    if (n > 4 && SDL_strcasecmp(m->map + n - 4, ".map") == 0) {
        m->map[n - 4] = '\0';
    }
}
