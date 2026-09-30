/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          imgui_mapchooser.cpp
 * Purpose:       Reusable map chooser widget for ImGui.
 *                Discovers maps in data/maps/, shows a
 *                scrollable list with minimap preview.
 *                Includes "Random Map" option with inline
 *                generator controls.
 *********************************************************/

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(__IPHONEOS__)
#include <dirent.h>
#endif

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_dialog_utils.h"

extern "C" {
#include "../sdl3draw.h"
#include "global.h"
#include "../minimap_render.h"
#include "../map_colours.h"   /* mapColourPaletteKey — part of the cache key */
#include "../map_preview_popup.h"
#include "mapgen.h"
#include "mapgen_maze.h"
#include "imgui_mapchooser.h"
#include "../../lang.h"
#include "../map_stars.h"
}
#include "../../../mapeditor/mapeditor_imgui.h"

extern "C" {
#include "../../../third_party/stb/stb_image_write.h"
#include "../../../third_party/stb/stb_image.h"
}

#define PREVIEW_SIZE 256

/* ─── Per-row thumbnail cache ───────────────────────────────────────
 * Disk-cached at data/preview_cache/<hash>.png. On a cache miss, the
 * provider's generatePreview hook is invoked on a single worker
 * thread; on success the PNG is written to disk and the UI thread
 * decodes it into an SDL_Texture next frame. Live across the dialog
 * lifetime; textures are owned here and reset on shutdown.
 *
 * The cache is provider-agnostic — keyed by the entry's `path`
 * field — so all three tabs reuse the same code. */
namespace {

constexpr const char *kCacheDir = "data/preview_cache";

enum PreviewState {
    PrevNotRequested = 0,
    PrevInFlight,
    PrevReady,
    PrevFailed,
};

struct PreviewEntry {
    PreviewState  state = PrevNotRequested;
    SDL_Texture  *tex   = nullptr;
    /* On-disk filename derived from path+mtime — stashed so PollDisk
     * can reload the PNG without re-hashing. Empty when the entry
     * was created via the warm-start disk path that already loaded
     * the texture inline. */
    std::string   cacheFile;
    /* Bounding box of opaque (non-transparent) pixels in the cached
     * texture, computed once when the texture is decoded. Lets the
     * hover-preview popup crop water away without storing a second
     * cached image. Values are pixel coordinates in the texture, and
     * texW/texH carry the full image size so UVs can be scaled.
     * If cropMaxX < cropMinX the texture is all-transparent and
     * crop == full. */
    int           texW = 0, texH = 0;
    int           cropMinX = 0, cropMinY = 0;
    int           cropMaxX = 0, cropMaxY = 0;
};

/* Emscripten links without -pthread (ASYNCIFY rules it out), so
 * pthread_create there is the stub that returns ENOTSUP — and
 * constructing a std::thread on top of that is fatal, not merely
 * unsuccessful: libc++ calls the [[noreturn]] __throw_system_error and
 * the whole module goes down. The browser build therefore carries no
 * worker at all and generates previews inline (generateInline below);
 * everything queue- and thread-shaped is compiled out. */
#ifndef __EMSCRIPTEN__
struct PreviewRequest {
    std::string         key;          /* entry->path (unadorned, for provider) */
    std::string         compositeKey; /* path|mtime (in-memory map key) */
    std::string         cacheFile;    /* full path under kCacheDir */
    MapFsProvider       provider;     /* captured by value — function ptrs + ctx */
};
#endif

static std::mutex                       gCacheMutex;
static std::map<std::string, PreviewEntry> gCache;          /* key → state/tex */
#ifndef __EMSCRIPTEN__
static std::deque<PreviewRequest>       gRequestQueue;
static std::condition_variable          gQueueCv;
static std::atomic<bool>                gWorkerRun{false};
static std::thread                      gWorker;
#endif

/* djb2 of the path mixed with the entry's modTime and the provider's
 * cache-scope namespace. Mixing mtime invalidates the cache when a
 * local .map is edited in place. Mixing scope keeps identically-named
 * maps on different sources (Upload vs server vs winbolo.net) from
 * contaminating each other. WBN rows have mtime=0 (or the upload
 * timestamp, which is also stable) so the hash is dominated by
 * path + scope there. */
static std::string hashKeyToFilename(const char *scope,
                                      const std::string &key,
                                      int64_t modTime) {
    uint64_t h = 5381;
    if (scope) {
        for (const char *s = scope; *s; s++) {
            h = ((h << 5) + h) + (unsigned char)*s;
        }
        h = ((h << 5) + h) + '|';
    }
    for (char c : key) h = ((h << 5) + h) + (unsigned char)c;
    /* The palette the thumbnail will be drawn in. A skin can change it, and
       the cached PNG carries the colours it was written with, so without this
       a skin change would leave every thumbnail on disk in the old colours
       until its map file's modification time changed - never, for a map that
       shipped with the game. Mixed in as a different file name rather than by
       clearing the cache, so switching skins back and forth finds both sets
       already rendered. */
    uint64_t pal = mapColourPaletteKey();
    for (int i = 0; i < 8; i++) {
        h = ((h << 5) + h) + (unsigned char)(pal & 0xFF);
        pal >>= 8;
    }
    uint64_t m = (uint64_t)modTime;
    for (int i = 0; i < 8; i++) {
        h = ((h << 5) + h) + (unsigned char)(m & 0xFF);
        m >>= 8;
    }
    char buf[32];
    SDL_snprintf(buf, sizeof(buf), "%016llx.png",
                 (unsigned long long)h);
    return std::string(buf);
}

static std::string cachePathFor(const char *scope,
                                 const std::string &key,
                                 int64_t modTime) {
    std::string s = kCacheDir;
    s += '/';
    s += hashKeyToFilename(scope, key, modTime);
    return s;
}

/* Try to decode the cached PNG into an SDL_Texture. Returns nullptr
 * on missing file or any decode error (caller treats both as "miss"
 * and either re-requests generation or marks failed). On success,
 * fills outEntry's tex, texW/H, and cropMinX..cropMaxY — scans the
 * alpha channel of the just-decoded buffer to find the bounding box
 * of opaque pixels so the list-view hover popup can crop water
 * away without keeping a second cached image. */
static bool loadCachedTexture(SDL_Renderer *renderer,
                              const std::string &cacheFile,
                              PreviewEntry *outEntry) {
    int w = 0, h = 0, ch = 0;
    unsigned char *rgba = stbi_load(cacheFile.c_str(), &w, &h, &ch, 4);
    if (!rgba) return false;
    SDL_Surface *surf = SDL_CreateSurfaceFrom(
        w, h, SDL_PIXELFORMAT_RGBA32, rgba, w * 4);
    SDL_Texture *tex = surf ? SDL_CreateTextureFromSurface(renderer, surf) : nullptr;
    if (surf) SDL_DestroySurface(surf);
    if (!tex) { stbi_image_free(rgba); return false; }

    /* Scan alpha for opaque-pixel bbox. minX > maxX afterwards means
     * "all transparent" — caller treats that as crop=full. */
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            if (row[x * 4 + 3] != 0) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    stbi_image_free(rgba);
    outEntry->tex      = tex;
    outEntry->texW     = w;
    outEntry->texH     = h;
    outEntry->cropMinX = minX;
    outEntry->cropMinY = minY;
    outEntry->cropMaxX = maxX;
    outEntry->cropMaxY = maxY;
    return true;
}

static void writeCachePng(const std::string &cacheFile,
                          const MapPreviewPixels &pix) {
    /* Ensure the dir exists — idempotent. */
    SDL_CreateDirectory(kCacheDir);

    /* Post-process: knock out deep-water pixels (RGB 0,0,80 from
     * minimapTerrainColor's DEEP_SEA case, plus 0,0,255 as a
     * common alternative deep-water shade some sources may emit)
     * by setting alpha=0. The cropped/transparent preview reads
     * as the interesting part of the map against whatever the
     * row/cell background is. Mutates pix.pixels in place — the
     * worker frees the buffer right after writing. */
    if (pix.pixels && pix.w > 0 && pix.h > 0) {
        size_t n = (size_t)pix.w * (size_t)pix.h;
        uint8_t *p = pix.pixels;
        for (size_t i = 0; i < n; i++, p += 4) {
            if (p[0] == 0 && p[1] == 0 && (p[2] == 80 || p[2] == 255)) {
                p[3] = 0;
            }
        }
    }
    /* TODO: also crop the image to the smallest rect enclosing the
     * remaining opaque pixels before writing, so each row's
     * thumbnail focuses on the interesting part of the map. */
    stbi_write_png(cacheFile.c_str(), pix.w, pix.h, 4,
                   pix.pixels, pix.w * 4);
}

#ifndef __EMSCRIPTEN__
static void workerThreadMain(void) {
    while (gWorkerRun.load()) {
        PreviewRequest req;
        {
            std::unique_lock<std::mutex> lk(gCacheMutex);
            gQueueCv.wait(lk, [] {
                return !gWorkerRun.load() || !gRequestQueue.empty();
            });
            if (!gWorkerRun.load()) return;
            req = std::move(gRequestQueue.front());
            gRequestQueue.pop_front();
        }

        bool ok = false;
        MapPreviewPixels buf = {0, 0, nullptr};
        if (req.provider.generatePreview) {
            ok = req.provider.generatePreview(req.key.c_str(), &buf,
                                              req.provider.ctx);
        }
        if (ok && buf.pixels && buf.w > 0 && buf.h > 0) {
            writeCachePng(req.cacheFile, buf);
            SDL_free(buf.pixels);
        } else if (buf.pixels) {
            SDL_free(buf.pixels);
        }

        /* Either way the UI thread re-checks the disk on the next
         * mapPreviewCacheGet call. Mark the in-memory state Ready or
         * Failed so we don't re-enqueue infinitely. */
        std::lock_guard<std::mutex> lk(gCacheMutex);
        auto it = gCache.find(req.compositeKey);
        if (it != gCache.end()) {
            it->second.state = ok ? PrevReady : PrevFailed;
        }
    }
}

static void cacheStartWorker(void) {
    if (gWorkerRun.load()) return;
    gWorkerRun.store(true);
    gWorker = std::thread(workerThreadMain);
}

static void cacheStopWorker(void) {
    if (!gWorkerRun.load()) return;
    gWorkerRun.store(false);
    {
        std::lock_guard<std::mutex> lk(gCacheMutex);
        gRequestQueue.clear();
    }
    gQueueCv.notify_all();
    if (gWorker.joinable()) gWorker.join();
}

/* Static guard whose destructor stops + joins the worker at program
 * exit. Without this, gWorker's own ~thread() runs while joinable
 * and std::terminate fires — which is the crash users see when they
 * close the lobby and quit before any explicit cache-shutdown call
 * had a chance to fire. Declared AFTER gWorker so static destruction
 * order (LIFO) tears down the guard first. */
struct WorkerGuard { ~WorkerGuard() { cacheStopWorker(); } };
static WorkerGuard gWorkerGuard;

#else  /* __EMSCRIPTEN__ */

/* Generate one preview on the calling thread and cache the result.
 * Costs a map parse plus a PREVIEW_SIZE-square rasterise, and only on
 * the first sighting of each map — the PNG cache absorbs every later
 * hover, and a row is only ever asked for when the pointer is over it.
 * Returns the finished texture, or nullptr when generation or decode
 * failed (recorded as PrevFailed so it is not retried). */
static SDL_Texture *generateInline(SDL_Renderer *renderer,
                                    const std::string &compositeKey,
                                    const char *key,
                                    const std::string &cacheFile,
                                    const MapFsProvider *provider) {
    MapPreviewPixels buf = {0, 0, nullptr};
    bool ok = provider->generatePreview(key, &buf, provider->ctx);
    if (ok && buf.pixels && buf.w > 0 && buf.h > 0) {
        writeCachePng(cacheFile, buf);
    } else {
        ok = false;
    }
    if (buf.pixels) {
        SDL_free(buf.pixels);
    }

    /* Round-trip through the PNG the same way the threaded path does,
     * so both hosts share one decode + crop-bbox routine. */
    PreviewEntry e;
    if (ok && loadCachedTexture(renderer, cacheFile, &e)) {
        e.state     = PrevReady;
        e.cacheFile = cacheFile;
    } else {
        e.state = PrevFailed;
    }
    SDL_Texture *tex = e.tex;
    std::lock_guard<std::mutex> lk(gCacheMutex);
    gCache[compositeKey] = std::move(e);
    return tex;
}

#endif /* __EMSCRIPTEN__ */

}  /* anonymous namespace */

extern "C" void mapChooserStopPreviewWorker(void) {
#ifndef __EMSCRIPTEN__
    cacheStopWorker();
#endif
}

/* Public: look up (and lazily request) a thumbnail texture for
 * `key`. Returns nullptr while the cache is still building it; the
 * caller (row renderer) just skips drawing the image that frame
 * and tries again next frame. Always cheap — no work happens on the
 * UI thread beyond a small map lookup + occasional PNG decode. */
/* Compose the in-memory cache key from scope + path + mtime. Scope
 * (provider->cacheScope) keeps cross-source name collisions apart;
 * mtime auto-invalidates when a local .map is edited. */
static std::string composeCacheKey(const char *scope, const char *path,
                                    int64_t modTime) {
    std::string s = scope ? scope : "";
    s += '|';
    s += path ? path : "";
    s += '|';
    char buf[24];
    SDL_snprintf(buf, sizeof(buf), "%lld", (long long)modTime);
    s += buf;
    return s;
}

static SDL_Texture *mapPreviewCacheGet(SDL_Renderer *renderer,
                                        const char *key,
                                        int64_t modTime,
                                        const MapFsProvider *provider) {
    if (!key || !*key || !provider) return nullptr;
#ifndef __EMSCRIPTEN__
    cacheStartWorker();
#endif

    std::string k = composeCacheKey(provider->cacheScope, key, modTime);
    {
        std::lock_guard<std::mutex> lk(gCacheMutex);
        auto it = gCache.find(k);
        if (it != gCache.end()) {
            return it->second.tex;  /* nullptr while InFlight/Failed */
        }
    }

    /* Not seen this entry yet — try disk first (warm-start path). */
    std::string cacheFile = cachePathFor(provider->cacheScope, key, modTime);
    PreviewEntry warm;
    if (loadCachedTexture(renderer, cacheFile, &warm)) {
        std::lock_guard<std::mutex> lk(gCacheMutex);
        warm.state     = PrevReady;
        warm.cacheFile = cacheFile;
        gCache[k]      = std::move(warm);
        return gCache[k].tex;
    }

    /* Disk miss — hand it to whatever does the generating. */
    if (!provider->generatePreview) {
        std::lock_guard<std::mutex> lk(gCacheMutex);
        PreviewEntry e;
        e.state = PrevFailed;
        gCache[k] = std::move(e);
        return nullptr;
    }
#ifdef __EMSCRIPTEN__
    /* No worker to hand it to — generate now and return the texture
     * from this same call. */
    return generateInline(renderer, k, key, cacheFile, provider);
#else
    /* Enqueue a generation request. Worker fills the PNG on disk; the
     * next mapPreviewCacheGet call for this key picks it up via the
     * disk path above. */
    {
        std::lock_guard<std::mutex> lk(gCacheMutex);
        PreviewEntry e;
        e.state     = PrevInFlight;
        e.tex       = nullptr;
        e.cacheFile = cacheFile;
        gCache[k]   = std::move(e);

        PreviewRequest req;
        /* The generatePreview hook gets the unadorned path — it
         * doesn't need to know about the cache key composition. */
        req.key          = key;
        req.compositeKey = k;
        req.cacheFile    = cacheFile;
        req.provider     = *provider;
        gRequestQueue.push_back(std::move(req));
    }
    gQueueCv.notify_one();
    return nullptr;
#endif
}

/* Companion to mapPreviewCacheGet that also reports the opaque-pixel
 * bounding box as UV0/UV1 + cropped pixel dimensions, so the caller
 * (list-view hover popup) can render only the interesting part of
 * the map without keeping a second cached image. Returns nullptr if
 * the texture isn't ready or the entry has no bounds yet. */
static SDL_Texture *mapPreviewCacheGetCropped(SDL_Renderer *renderer,
                                               const char *key,
                                               int64_t modTime,
                                               const MapFsProvider *provider,
                                               ImVec2 *outUV0,
                                               ImVec2 *outUV1,
                                               int *outCropW,
                                               int *outCropH) {
    SDL_Texture *tex = mapPreviewCacheGet(renderer, key, modTime, provider);
    if (!tex) return nullptr;
    std::lock_guard<std::mutex> lk(gCacheMutex);
    std::string k = composeCacheKey(provider->cacheScope, key, modTime);
    auto it = gCache.find(k);
    if (it == gCache.end() || it->second.texW <= 0 || it->second.texH <= 0) {
        if (outUV0) *outUV0 = ImVec2(0.0f, 0.0f);
        if (outUV1) *outUV1 = ImVec2(1.0f, 1.0f);
        if (outCropW) *outCropW = 0;
        if (outCropH) *outCropH = 0;
        return tex;
    }
    int w = it->second.texW;
    int h = it->second.texH;
    int x0 = it->second.cropMinX;
    int y0 = it->second.cropMinY;
    int x1 = it->second.cropMaxX;
    int y1 = it->second.cropMaxY;
    if (x1 < x0 || y1 < y0) {
        /* All-transparent — fall back to the full image. */
        x0 = 0; y0 = 0; x1 = w - 1; y1 = h - 1;
    }
    /* Single-pixel padding so the cropped rect doesn't visually
     * touch the popup border. */
    if (x0 > 0)        x0--;
    if (y0 > 0)        y0--;
    if (x1 < w - 1)    x1++;
    if (y1 < h - 1)    y1++;
    if (outUV0) *outUV0 = ImVec2((float)x0 / (float)w,
                                  (float)y0 / (float)h);
    if (outUV1) *outUV1 = ImVec2((float)(x1 + 1) / (float)w,
                                  (float)(y1 + 1) / (float)h);
    if (outCropW) *outCropW = x1 - x0 + 1;
    if (outCropH) *outCropH = y1 - y0 + 1;
    return tex;
}

/* Worker wrote a PNG to disk for an InFlight entry — poll for those
 * and upgrade them to Ready (with a decoded texture). Called from
 * the row-render loop so promotions piggy-back the natural traffic
 * rather than running a separate timer. */
static void mapPreviewCachePollDisk(SDL_Renderer *renderer) {
    /* Snapshot pending keys + their on-disk paths. Stash the cache
     * filename per entry so we can reload without recomputing the
     * hash (we'd need the mtime, which the in-memory key encodes
     * but is awkward to parse back out). */
    std::vector<std::pair<std::string, std::string>> readyKeys;
    {
        std::lock_guard<std::mutex> lk(gCacheMutex);
        for (auto &kv : gCache) {
            if (kv.second.state == PrevReady && !kv.second.tex
                && !kv.second.cacheFile.empty()) {
                readyKeys.emplace_back(kv.first, kv.second.cacheFile);
            }
        }
    }
    for (const auto &kf : readyKeys) {
        PreviewEntry tmp;
        if (!loadCachedTexture(renderer, kf.second, &tmp)) {
            continue;
        }
        std::lock_guard<std::mutex> lk(gCacheMutex);
        auto it = gCache.find(kf.first);
        if (it != gCache.end()) {
            it->second.tex      = tmp.tex;
            it->second.texW     = tmp.texW;
            it->second.texH     = tmp.texH;
            it->second.cropMinX = tmp.cropMinX;
            it->second.cropMinY = tmp.cropMinY;
            it->second.cropMaxX = tmp.cropMaxX;
            it->second.cropMaxY = tmp.cropMaxY;
        }
    }
}

/* Drop all in-memory textures + cancel pending work. Called from
 * mapChooserDestroy on the last surviving state. Doesn't touch the
 * on-disk PNGs — those are intentionally persistent. */
static void mapPreviewCacheShutdown(SDL_Renderer * /*renderer*/) {
#ifndef __EMSCRIPTEN__
    cacheStopWorker();
#endif
    std::lock_guard<std::mutex> lk(gCacheMutex);
    for (auto &kv : gCache) {
        if (kv.second.tex) {
            SDL_DestroyTexture(kv.second.tex);
            kv.second.tex = nullptr;
        }
    }
    gCache.clear();
}

/* Build a minimap preview texture from a .map file path.
 * Delegates to the shared minimap renderer. */
static SDL_Texture *buildPreviewFromMapPath(SDL_Renderer *renderer,
                                             const char *mapPath,
                                             int *bMinX, int *bMinY, int *bMaxX, int *bMaxY,
                                             int *outPills, int *outBases, int *outStarts) {
    MinimapBounds mb;
    SDL_Texture *tex = minimapFromFile(renderer, mapPath, &mb,
                                       outPills, outBases, outStarts);
    if (tex) {
        *bMinX = mb.minX; *bMinY = mb.minY;
        *bMaxX = mb.maxX; *bMaxY = mb.maxY;
    }
    return tex;
}

/* Build preview for the embedded Everard Island map */
static SDL_Texture *buildEverardPreview(SDL_Renderer *renderer,
                                         int *bMinX, int *bMinY, int *bMaxX, int *bMaxY,
                                         int *outPills, int *outBases, int *outStarts) {
    const char *paths[] = { "Everard Island.map", "data/maps/Everard Island.map" };
    for (int i = 0; i < 2; i++) {
        SDL_Texture *tex = buildPreviewFromMapPath(renderer, paths[i],
            bMinX, bMinY, bMaxX, bMaxY, outPills, outBases, outStarts);
        if (tex) return tex;
    }
    return NULL;
}

static void SDLCALL mapChooserFileDialogCallback(void *userdata, const char *const *filelist, int filter) {
    MapChooserState *state = (MapChooserState *)userdata;
    (void)filter;
    state->fileDialogResult[0] = '\0';
    if (filelist && filelist[0]) {
        SDL_strlcpy(state->fileDialogResult, filelist[0], FILENAME_MAX);
    }
    state->fileDialogGotResult = true;
    state->fileDialogPending = false;
}

/* Helpers — check if a path is a directory using SDL3's path-info API. */
static bool pathIsDirectory(const char *path) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return false;
    return info.type == SDL_PATHTYPE_DIRECTORY;
}

/* Recursive walker for the local-fs provider (Upload tab).
 * Appends every .map file in `dir` and its subdirectories whose
 * basename contains `queryLower` (case-insensitive substring) to
 * state->maps, with `name` set to the basename and `path` set to
 * the on-disk path. Stops at MAP_CHOOSER_MAX_MAPS. Depth limited
 * so a pathological symlink loop can't pin the UI. */
static void discoverMapsRecursive(MapChooserState *state,
                                    const char *dir,
                                    const char *subRel,
                                    const char *queryLower,
                                    size_t qlen,
                                    int depth) {
    if (state->numMaps >= MAP_CHOOSER_MAX_MAPS) return;
    if (depth > 8) return;

    int count = 0;
    char **list = SDL_GlobDirectory(dir, NULL, 0, &count);
    if (!list) return;

    for (int i = 0; i < count && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
        const char *name = list[i];
        if (!name || name[0] == '.') continue;

        char full[FILENAME_MAX];
        SDL_snprintf(full, sizeof(full), "%s/%s", dir, name);

        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(full, &pi)) continue;
        bool isDir = (pi.type == SDL_PATHTYPE_DIRECTORY);

        /* Build the entry's relative-path-from-search-root: e.g.
         * at depth 0 this is just `name`, deeper it becomes
         * "Map Test/Baringi.map" etc. Used as the display name
         * for recursive hits so duplicates in different folders
         * are visually distinct. */
        char rel[256];
        if (subRel[0] == '\0') {
            SDL_strlcpy(rel, name, sizeof(rel));
        } else {
            SDL_snprintf(rel, sizeof(rel), "%s/%s", subRel, name);
        }

        if (isDir) {
            discoverMapsRecursive(state, full, rel, queryLower,
                                  qlen, depth + 1);
            continue;
        }
        size_t nlen = SDL_strlen(name);
        if (nlen <= 4 ||
            SDL_strcasecmp(name + nlen - 4, ".map") != 0) continue;

        /* Case-insensitive substring match against basename only. */
        bool match = false;
        for (size_t k = 0; k + qlen <= nlen; k++) {
            size_t m;
            for (m = 0; m < qlen; m++) {
                char hc = name[k + m];
                if (hc >= 'A' && hc <= 'Z') hc = (char)(hc + 32);
                if (hc != queryLower[m]) break;
            }
            if (m == qlen) { match = true; break; }
        }
        if (!match) continue;

        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        SDL_strlcpy(e->path, full, sizeof(e->path));
        /* Display name = basename only (without .map); the enclosing
         * folder lives in crumbsPath for the hover tooltip and the
         * preview-side breadcrumb. */
        SDL_strlcpy(e->name, name, sizeof(e->name));
        e->modTime = (int64_t)pi.modify_time;
        e->scripted = mapChooserMapHasScript(full);
        size_t dlen = SDL_strlen(e->name);
        if (dlen > 4 &&
            SDL_strcasecmp(e->name + dlen - 4, ".map") == 0) {
            e->name[dlen - 4] = '\0';
        }
        if (subRel[0] != '\0') {
            SDL_strlcpy(e->crumbsPath, subRel, sizeof(e->crumbsPath));
        }
    }
    SDL_free(list);
}

/* Lazily-loaded SVG icons for the list / grid view toggle. Cached
 * for the lifetime of the renderer they were uploaded against; we
 * track the renderer to invalidate cleanly if it ever changes. */
static SDL_Texture *s_iconListView = nullptr;
static SDL_Texture *s_iconGridView = nullptr;
static SDL_Texture *s_iconFolder   = nullptr;
static SDL_Texture *s_iconMaximize = nullptr;
static SDL_Texture *s_iconStarEmpty = nullptr;
static SDL_Texture *s_iconStarFull  = nullptr;
static SDL_Renderer *s_iconsRenderer = nullptr;

static void loadViewModeIconsOnce(SDL_Renderer *renderer, int sizePx) {
    if (s_iconsRenderer == renderer &&
        s_iconListView && s_iconGridView && s_iconFolder)
        return;
    if (s_iconsRenderer != renderer) {
        if (s_iconListView)  { SDL_DestroyTexture(s_iconListView);  s_iconListView  = nullptr; }
        if (s_iconGridView)  { SDL_DestroyTexture(s_iconGridView);  s_iconGridView  = nullptr; }
        if (s_iconFolder)    { SDL_DestroyTexture(s_iconFolder);    s_iconFolder    = nullptr; }
        if (s_iconMaximize)  { SDL_DestroyTexture(s_iconMaximize);  s_iconMaximize  = nullptr; }
        if (s_iconStarEmpty) { SDL_DestroyTexture(s_iconStarEmpty); s_iconStarEmpty = nullptr; }
        if (s_iconStarFull)  { SDL_DestroyTexture(s_iconStarFull);  s_iconStarFull  = nullptr; }
        s_iconsRenderer = renderer;
    }
    if (!s_iconListView) {
        s_iconListView = imguiLoadSvgIconWhite(renderer,
            "data/ui/list-view.svg", sizePx);
        if (!s_iconListView) {
            char buf[FILENAME_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(buf, sizeof(buf),
                             "%sdata/ui/list-view.svg", base);
                s_iconListView = imguiLoadSvgIconWhite(renderer, buf, sizePx);
            }
        }
    }
    if (!s_iconGridView) {
        s_iconGridView = imguiLoadSvgIconWhite(renderer,
            "data/ui/grid-view.svg", sizePx);
        if (!s_iconGridView) {
            char buf[FILENAME_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(buf, sizeof(buf),
                             "%sdata/ui/grid-view.svg", base);
                s_iconGridView = imguiLoadSvgIconWhite(renderer, buf, sizePx);
            }
        }
    }
    if (!s_iconFolder) {
        /* Folder icon is rendered at the row's text height; pick a
         * size matching that rather than the toggle's iconPx. */
        int folderPx = (int)ImGui::GetTextLineHeight();
        if (folderPx < 12) folderPx = 12;
        s_iconFolder = imguiLoadSvgIconWhite(renderer,
            "data/ui/icon-folder.svg", folderPx);
        if (!s_iconFolder) {
            char buf[FILENAME_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(buf, sizeof(buf),
                             "%sdata/ui/icon-folder.svg", base);
                s_iconFolder = imguiLoadSvgIconWhite(renderer, buf, folderPx);
            }
        }
    }
    if (!s_iconMaximize) {
        s_iconMaximize = imguiLoadSvgIconWhite(renderer,
            "data/ui/maximize.svg", sizePx);
        if (!s_iconMaximize) {
            char buf[FILENAME_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(buf, sizeof(buf),
                             "%sdata/ui/maximize.svg", base);
                s_iconMaximize = imguiLoadSvgIconWhite(renderer, buf, sizePx);
            }
        }
    }
    /* Star icons — keep them at the same sizePx as the view-mode
     * toggle so they read as part of the same set. star_empty is
     * the unstarred state, star_full is starred. The chooser tints
     * the empty variant when hovered and the full variant always
     * renders in its authored colour. */
    if (!s_iconStarEmpty) {
        s_iconStarEmpty = imguiLoadSvgIcon(renderer,
            "data/ui/star_empty.svg", sizePx);
        if (!s_iconStarEmpty) {
            char buf[FILENAME_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(buf, sizeof(buf),
                             "%sdata/ui/star_empty.svg", base);
                s_iconStarEmpty = imguiLoadSvgIcon(renderer, buf, sizePx);
            }
        }
    }
    if (!s_iconStarFull) {
        s_iconStarFull = imguiLoadSvgIcon(renderer,
            "data/ui/star_full.svg", sizePx);
        if (!s_iconStarFull) {
            char buf[FILENAME_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(buf, sizeof(buf),
                             "%sdata/ui/star_full.svg", base);
                s_iconStarFull = imguiLoadSvgIcon(renderer, buf, sizePx);
            }
        }
    }
}

/* Public-within-TU accessor for the maximize SVG. Returns nullptr if
 * the icon hasn't been loaded yet (caller falls back to glyph). */
SDL_Texture *mapChooserGetMaximizeIcon(void) {
    return s_iconMaximize;
}

/* Render the list/grid view-mode toggle as two SVG icon buttons.
 * Mutates state->viewMode. Falls back to text labels when the SVG
 * files can't be loaded. */
static void renderViewModeToggle(MapChooserState *state,
                                  SDL_Renderer *renderer) {
    const int iconPx = (int)(ImGui::GetFrameHeight() - 6.0f);
    loadViewModeIconsOnce(renderer, iconPx);

    auto drawBtn = [&](const char *id, SDL_Texture *icon,
                        const char *fallback, int mode,
                        const char *tooltip) {
        bool active = (state->viewMode == mode);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        bool clicked = false;
        if (icon) {
            ImVec2 sz((float)iconPx, (float)iconPx);
            clicked = ImGui::ImageButton(id, (ImTextureID)icon, sz);
        } else {
            clicked = ImGui::Button(fallback);
        }
        if (active) ImGui::PopStyleColor();
        imguiHelpTooltip(tooltip);
        if (clicked) state->viewMode = mode;
    };

    drawBtn("##mcViewList", s_iconListView, "List", 0, "List view");
    ImGui::SameLine();
    drawBtn("##mcViewGrid", s_iconGridView, "Grid", 1, "Grid view");
}

/* Render a "/" -separated path as a row of clickable segment
 * buttons. Each click latches the prefix up to and including that
 * segment into state->pendingJumpPath for the tab-side consumer.
 *
 * `rightAnchor` flips the layout from left-flush (the path label
 * above the search box) to right-flush (the breadcrumb under the
 * preview) and shares the colour + hover-underline treatment between
 * both call sites. Caller is responsible for placing the cursor on
 * the right line before invoking. */
static void renderBreadcrumbSegments(MapChooserState *state,
                                      const char *crumbsPath,
                                      bool rightAnchor) {
    if (!crumbsPath || crumbsPath[0] == '\0') return;

    const char *kSep = " / ";
    float sepW = ImGui::CalcTextSize(kSep).x;

    char segBuf[FILENAME_MAX];
    SDL_strlcpy(segBuf, crumbsPath, sizeof(segBuf));
    const char *segs[32];
    int numSegs = 0;
    char *p = segBuf;
    segs[numSegs++] = p;
    while (*p && numSegs < 32) {
        if (*p == '/') {
            *p = '\0';
            segs[numSegs++] = p + 1;
        }
        p++;
    }

    if (rightAnchor) {
        float totalW = 0.0f;
        for (int k = 0; k < numSegs; k++) {
            if (k > 0) totalW += sepW;
            totalW += ImGui::CalcTextSize(segs[k]).x;
        }
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > totalW) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                                  + (avail - totalW));
        }
    }

    std::string prefix;
    for (int k = 0; k < numSegs; k++) {
        if (k > 0) {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(kSep);
            ImGui::SameLine(0.0f, 0.0f);
        }
        if (!prefix.empty()) prefix += '/';
        prefix += segs[k];

        /* Last segment = the current folder. Render it as plain text:
         * clicking it would just reload the same folder, and the link
         * styling falsely implies "go somewhere else". */
        bool isLast = (k == numSegs - 1);
        if (!isLast) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImVec4(0.45f, 0.70f, 1.0f, 1.0f));
        }
        ImGui::TextUnformatted(segs[k]);
        if (!isLast) {
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImVec2 a = ImGui::GetItemRectMin();
                ImVec2 b = ImGui::GetItemRectMax();
                dl->AddLine(ImVec2(a.x, b.y - 1.0f),
                            ImVec2(b.x, b.y - 1.0f),
                            IM_COL32(115, 178, 255, 255));
            }
            if (ImGui::IsItemClicked()) {
                SDL_strlcpy(state->pendingJumpPath, prefix.c_str(),
                            sizeof(state->pendingJumpPath));
            }
        }
    }
}

/* Post-pass for the recursive-search modes: scan the file rows the
 * caller just appended, collect each unique enclosing folder, and
 * push a folder row for it at the top of the list. The folder row's
 * path is the on-disk (or server-relative) prefix the caller passes
 * via `pathPrefix` joined with the relative folder — clicking it
 * navigates the chooser into that folder via the normal folder-click
 * flow. `pathPrefix` ends with no trailing slash; "" means the relPath
 * convention is the same as the crumbsPath. */
void mapChooserEmitFolderRowsForSearchHits(MapChooserState *state,
                                            const char *pathPrefix) {
    /* Snapshot the existing entries so we can rewrite the list in
     * folder-first order without losing the file rows. */
    int origCount = state->numMaps;
    if (origCount <= 0) return;

    /* Lowercase the active query once; folder leaf-segment match
     * filters out aggregated parents that don't share the query the
     * file rows did (e.g. searching "Andromeda" shouldn't surface
     * "Canuck's Maps" as a folder result just because a hit lives
     * inside). Empty query (defensive — caller wouldn't normally
     * invoke us then) skips the filter. */
    const char *q = state->searchFilter;
    char qlow[64];
    size_t qlen = SDL_strlen(q);
    if (qlen >= sizeof(qlow)) qlen = sizeof(qlow) - 1;
    for (size_t k = 0; k < qlen; k++) {
        char c = q[k];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        qlow[k] = c;
    }
    qlow[qlen] = '\0';

    /* Collect unique non-empty crumbsPath values whose LEAF segment
     * contains the query (case-insensitive substring). */
    char folders[MAP_CHOOSER_MAX_MAPS][FILENAME_MAX];
    int  numFolders = 0;
    for (int i = 0; i < origCount; i++) {
        const char *c = state->maps[i].crumbsPath;
        if (c[0] == '\0') continue;
        const char *lastSep = SDL_strrchr(c, '/');
        const char *leaf = lastSep ? lastSep + 1 : c;
        if (qlen > 0) {
            size_t llen = SDL_strlen(leaf);
            bool match = false;
            for (size_t k = 0; k + qlen <= llen; k++) {
                size_t m;
                for (m = 0; m < qlen; m++) {
                    char hc = leaf[k + m];
                    if (hc >= 'A' && hc <= 'Z') hc = (char)(hc + 32);
                    if (hc != qlow[m]) break;
                }
                if (m == qlen) { match = true; break; }
            }
            if (!match) continue;
        }
        bool dup = false;
        for (int j = 0; j < numFolders; j++) {
            if (SDL_strcasecmp(folders[j], c) == 0) { dup = true; break; }
        }
        if (!dup) {
            SDL_strlcpy(folders[numFolders++], c, sizeof(folders[0]));
            if (numFolders >= MAP_CHOOSER_MAX_MAPS) break;
        }
    }
    if (numFolders == 0) return;

    /* Sort folder list alphabetically (case-insensitive). */
    for (int i = 1; i < numFolders; i++) {
        char tmp[FILENAME_MAX];
        SDL_strlcpy(tmp, folders[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && SDL_strcasecmp(folders[j], tmp) > 0) {
            SDL_strlcpy(folders[j + 1], folders[j], sizeof(folders[0]));
            j--;
        }
        SDL_strlcpy(folders[j + 1], tmp, sizeof(folders[0]));
    }

    /* Stash the original file rows, then rewrite numMaps = 0 and
     * push folder rows first, file rows after. */
    MapChooserEntry saved[MAP_CHOOSER_MAX_MAPS];
    for (int i = 0; i < origCount; i++) saved[i] = state->maps[i];
    state->numMaps = 0;

    for (int i = 0; i < numFolders && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        e->isFolder = true;
        /* Display name = the last path segment of the folder. */
        const char *lastSep = SDL_strrchr(folders[i], '/');
        SDL_strlcpy(e->name, lastSep ? lastSep + 1 : folders[i],
                    sizeof(e->name));
        /* `path` drives folder-click navigation (sets state->currentDir).
         * Prepend the caller-supplied prefix so the click jumps to the
         * right place under the tab's own currentDir convention. */
        if (pathPrefix && pathPrefix[0] != '\0') {
            SDL_snprintf(e->path, sizeof(e->path), "%s/%s",
                         pathPrefix, folders[i]);
        } else {
            SDL_strlcpy(e->path, folders[i], sizeof(e->path));
        }
        /* crumbsPath on the folder row itself so its hover/breadcrumb
         * shows where it lives, not just the leaf name. */
        SDL_strlcpy(e->crumbsPath, folders[i], sizeof(e->crumbsPath));
    }

    for (int i = 0; i < origCount && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
        state->maps[state->numMaps++] = saved[i];
    }
}

/* Normalise backslashes to forward slashes in every entry's display
 * name and on-disk path, then drop duplicates whose path collapses
 * to the same string after normalisation. Some platforms surface
 * mixed-separator paths from SDL_GlobDirectory or stash native
 * separators in user-typed config — normalising at display time
 * stops "Map Test/Foo" and "Map Test\Foo" rows from appearing twice
 * for the same file. */
static void normaliseAndDedupe(MapChooserState *state) {
    for (int i = 0; i < state->numMaps; i++) {
        for (char *p = state->maps[i].name; *p; p++) {
            if (*p == '\\') *p = '/';
        }
        for (char *p = state->maps[i].path; *p; p++) {
            if (*p == '\\') *p = '/';
        }
    }
    int writeIdx = 0;
    for (int i = 0; i < state->numMaps; i++) {
        bool dup = false;
        for (int j = 0; j < writeIdx; j++) {
            /* Match on the on-disk path when it's set; otherwise
             * the name (inbuilt / synthetic entries have empty
             * path so we fall back to comparing display names). */
            const char *ap = state->maps[i].path;
            const char *bp = state->maps[j].path;
            if (ap[0] != '\0' && bp[0] != '\0') {
                if (SDL_strcasecmp(ap, bp) == 0) { dup = true; break; }
            } else if (SDL_strcasecmp(state->maps[i].name,
                                       state->maps[j].name) == 0) {
                dup = true; break;
            }
        }
        if (dup) continue;
        if (writeIdx != i) state->maps[writeIdx] = state->maps[i];
        writeIdx++;
    }
    state->numMaps = writeIdx;
}

void mapChooserSetWorkshopDir(MapChooserState *state, const char *dir) {
    if (!state) return;
    state->workshopDir[0] = '\0';
    if (!dir || dir[0] == '\0') return;
    SDL_strlcpy(state->workshopDir, dir, sizeof(state->workshopDir));
    for (char *p = state->workshopDir; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    size_t len = SDL_strlen(state->workshopDir);
    while (len > 0 && state->workshopDir[len - 1] == '/') {
        state->workshopDir[--len] = '\0';
    }
}

/* Whether path is the tab's Workshop directory or lies under it. On true,
 * rest holds what follows the directory and its separator, or "" for the
 * directory itself. Either separator is accepted in path, and a trailing
 * one is ignored. False when the tab has no Workshop directory. */
static bool mapChooserWorkshopRest(const MapChooserState *state,
                                   const char *path,
                                   char *rest, size_t restLen) {
    size_t wsLen = SDL_strlen(state->workshopDir);
    if (wsLen == 0 || !path) return false;
    char norm[FILENAME_MAX];
    SDL_strlcpy(norm, path, sizeof(norm));
    for (char *p = norm; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    size_t len = SDL_strlen(norm);
    while (len > 0 && norm[len - 1] == '/') norm[--len] = '\0';
    if (SDL_strncmp(norm, state->workshopDir, wsLen) != 0) return false;
    if (norm[wsLen] == '\0') {
        rest[0] = '\0';
        return true;
    }
    if (norm[wsLen] != '/') return false;
    SDL_strlcpy(rest, norm + wsLen + 1, restLen);
    return true;
}

/* The part of a map path the breadcrumbs show after the tab's root label:
 *   "data/maps/Sub/X"   → "Sub/X"
 *   "data/maps"         → ""
 *   "<workshopDir>/Sub" → "Workshop/Sub"
 *   "<workshopDir>"     → "Workshop"
 * Any other path, such as the Server Maps and WinBolo.net tabs' relative
 * ones, is copied unchanged. The Workshop directory's absolute path is
 * never shown. */
static void mapChooserCrumbRel(const MapChooserState *state,
                               const char *path,
                               char *out, size_t outLen) {
    static const char kRoot[]     = "data/maps/";
    static const char kRootBare[] = "data/maps";
    char rest[FILENAME_MAX];
    if (SDL_strncmp(path, kRoot, sizeof(kRoot) - 1) == 0) {
        SDL_strlcpy(out, path + sizeof(kRoot) - 1, outLen);
    } else if (SDL_strcasecmp(path, kRootBare) == 0) {
        out[0] = '\0';
    } else if (mapChooserWorkshopRest(state, path, rest, sizeof(rest))) {
        if (rest[0] != '\0') {
            SDL_snprintf(out, outLen, "Workshop/%s", rest);
        } else {
            SDL_strlcpy(out, "Workshop", outLen);
        }
    } else {
        SDL_strlcpy(out, path, outLen);
    }
}

void mapChooserLocalFsEnumerate(MapChooserState *state,
                                 const char *relPath, void *ctx) {
    (void)ctx;

    /* Recursive-search shortcut for the local-fs case. Skips the
     * synthetic ".." / inbuilt Everard since results span multiple
     * folders. */
    if (state->searchRecursive && state->searchFilter[0] != '\0') {
        const char *root = (relPath && relPath[0] != '\0')
                            ? relPath : "data/maps";
        char qlow[64];
        size_t qlen = SDL_strlen(state->searchFilter);
        if (qlen >= sizeof(qlow)) qlen = sizeof(qlow) - 1;
        for (size_t i = 0; i < qlen; i++) {
            char c = state->searchFilter[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            qlow[i] = c;
        }
        qlow[qlen] = '\0';
        discoverMapsRecursive(state, root, "", qlow, qlen, 0);
        /* `root` is the on-disk parent (e.g. "data/maps"); folder rows
         * need that prefix so a click sets currentDir to the absolute
         * path the local-fs provider expects. */
        mapChooserEmitFolderRowsForSearchHits(state, root);
        return;
    }

    /* Source directory — explicit relPath if set, else default. */
    const char *dir  = (relPath && relPath[0] != '\0') ? relPath : "data/maps";
    const char *root = "data/maps";

    /* At the root, the first entry is always "Everard Island (Inbuilt)".
     * Inside a subfolder, the first entry is a synthetic ".." that
     * navigates back up. */
    bool inSubfolder = (SDL_strcasecmp(dir, root) != 0);
    if (inSubfolder) {
        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        SDL_strlcpy(e->name, "[..]", sizeof(e->name));
        SDL_strlcpy(e->path, dir, sizeof(e->path));
        size_t plen = SDL_strlen(e->path);
        while (plen > 0 && e->path[plen - 1] != '/' && e->path[plen - 1] != '\\') {
            e->path[--plen] = '\0';
        }
        if (plen > 0) e->path[plen - 1] = '\0';
        if (e->path[0] == '\0') {
            SDL_strlcpy(e->path, root, sizeof(e->path));
        }
        /* The Workshop folder is listed at the root, so going up from it
         * goes back there rather than to the directory that holds it. */
        char wsRest[FILENAME_MAX];
        if (mapChooserWorkshopRest(state, dir, wsRest, sizeof(wsRest)) &&
            wsRest[0] == '\0') {
            SDL_strlcpy(e->path, root, sizeof(e->path));
        }
        e->isFolder = true;
        e->isParentUp = true;
    } else {
        SDL_strlcpy(state->maps[0].name, langGetText(STR_MAPCHOOSER_EVERARD),
                    sizeof(state->maps[0].name));
        state->maps[0].path[0] = '\0';   /* empty = inbuilt */
        state->maps[0].isFolder   = false;
        state->maps[0].isParentUp = false;
        state->numMaps = 1;
    }

    int count = 0;
    char **list = SDL_GlobDirectory(dir, NULL, 0, &count);
    if (list) {
        for (int i = 0; i < count && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
            const char *name = list[i];
            if (name[0] == '.') continue;

            char full[FILENAME_MAX];
            SDL_snprintf(full, sizeof(full), "%s/%s", dir, name);

            SDL_PathInfo pi;
            if (!SDL_GetPathInfo(full, &pi)) continue;
            bool isDir = (pi.type == SDL_PATHTYPE_DIRECTORY);

            if (isDir) {
                MapChooserEntry *e = &state->maps[state->numMaps++];
                memset(e, 0, sizeof(*e));
                SDL_strlcpy(e->name, name, sizeof(e->name));
                SDL_strlcpy(e->path, full, sizeof(e->path));
                e->isFolder = true;
                e->modTime  = (int64_t)pi.modify_time;
                continue;
            }

            size_t nlen = SDL_strlen(name);
            if (nlen <= 4 ||
                SDL_strcasecmp(name + nlen - 4, ".map") != 0) continue;
            if (!inSubfolder &&
                SDL_strcasecmp(name, "Everard Island.map") == 0) continue;

            MapChooserEntry *e = &state->maps[state->numMaps++];
            memset(e, 0, sizeof(*e));
            SDL_strlcpy(e->path, full, sizeof(e->path));
            SDL_strlcpy(e->name, name, sizeof(e->name));
            e->modTime  = (int64_t)pi.modify_time;
            e->scripted = mapChooserMapHasScript(full);
            size_t dlen = SDL_strlen(e->name);
            if (dlen > 4 &&
                SDL_strcasecmp(e->name + dlen - 4, ".map") == 0) {
                e->name[dlen - 4] = '\0';
            }
        }
        SDL_free(list);
    }

    /* Root view also offers the Workshop directory as a folder named
     * "Workshop", when the tab has one and it exists. Its row carries the
     * absolute directory, which a click makes currentDir; the listing
     * above reads any directory path, so the folder's own view needs
     * nothing more. A real data/maps/Workshop folder is listed instead. */
    if (!inSubfolder && state->workshopDir[0] != '\0' &&
        state->numMaps < MAP_CHOOSER_MAX_MAPS) {
        bool present = false;
        for (int j = 0; j < state->numMaps; j++) {
            if (state->maps[j].isFolder &&
                SDL_strcasecmp(state->maps[j].name, "Workshop") == 0) {
                present = true; break;
            }
        }
        SDL_PathInfo pi;
        if (!present && SDL_GetPathInfo(state->workshopDir, &pi) &&
            pi.type == SDL_PATHTYPE_DIRECTORY) {
            MapChooserEntry *e = &state->maps[state->numMaps++];
            memset(e, 0, sizeof(*e));
            SDL_strlcpy(e->name, "Workshop", sizeof(e->name));
            SDL_strlcpy(e->path, state->workshopDir, sizeof(e->path));
            e->isFolder = true;
            e->modTime  = (int64_t)pi.modify_time;
        }
    }

    /* Root view also surfaces .map files saved to the writable per-user
     * directory (where controller Save Map writes), so they're loadable
     * here. Subfolder navigation stays within data/maps. */
    if (!inSubfolder) {
        char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
        if (prefDir) {
            char mapsDir[FILENAME_MAX];
            SDL_snprintf(mapsDir, sizeof(mapsDir), "%smaps", prefDir);
            int pcount = 0;
            char **plist = SDL_GlobDirectory(mapsDir, NULL, 0, &pcount);
            if (plist) {
                for (int i = 0; i < pcount && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
                    const char *name = plist[i];
                    if (name[0] == '.') continue;
                    size_t nlen = SDL_strlen(name);
                    if (nlen <= 4 ||
                        SDL_strcasecmp(name + nlen - 4, ".map") != 0) continue;
                    if (SDL_strcasecmp(name, "Everard Island.map") == 0) continue;

                    char full[FILENAME_MAX];
                    SDL_snprintf(full, sizeof(full), "%s/%s", mapsDir, name);
                    SDL_PathInfo pi;
                    if (!SDL_GetPathInfo(full, &pi)) continue;
                    if (pi.type == SDL_PATHTYPE_DIRECTORY) continue;

                    /* Display name = basename minus ".map". */
                    char disp[FILENAME_MAX];
                    SDL_strlcpy(disp, name, sizeof(disp));
                    size_t dlen = SDL_strlen(disp);
                    if (dlen > 4 &&
                        SDL_strcasecmp(disp + dlen - 4, ".map") == 0) {
                        disp[dlen - 4] = '\0';
                    }

                    /* A data/maps map of the same basename wins the tie:
                     * skip a pref-dir map whose name is already listed. */
                    bool dup = false;
                    for (int j = 0; j < state->numMaps; j++) {
                        if (!state->maps[j].isFolder &&
                            SDL_strcasecmp(state->maps[j].name, disp) == 0) {
                            dup = true; break;
                        }
                    }
                    if (dup) continue;

                    MapChooserEntry *e = &state->maps[state->numMaps++];
                    memset(e, 0, sizeof(*e));
                    SDL_strlcpy(e->path, full, sizeof(e->path));
                    SDL_strlcpy(e->name, disp, sizeof(e->name));
                    e->modTime = (int64_t)pi.modify_time;
                    e->scripted = mapChooserMapHasScript(full);
                }
                SDL_free(plist);
            }
            SDL_free(prefDir);
        }
    }

    /* Folders first, then files, alphabetical within each group;
     * preserve the pinned ".." / Everard at index 0. */
    int sortFrom = inSubfolder ? 1 : 0;
    if (state->numMaps - sortFrom > 1) {
        std::sort(&state->maps[sortFrom], &state->maps[state->numMaps],
            [](const MapChooserEntry &a, const MapChooserEntry &b) {
                if (a.isFolder != b.isFolder) return a.isFolder;
                return SDL_strcasecmp(a.name, b.name) < 0;
            });
    }
}

void mapChooserRefresh(MapChooserState *state);

static void discoverMaps(MapChooserState *state) {
    state->numMaps = 0;
    state->searchRowsUntagged = false;
    if (state->provider.enumerate) {
        state->provider.enumerate(state, state->currentDir,
                                   state->provider.ctx);
    }
    normaliseAndDedupe(state);

    /* Re-anchor selection by path. Providers that rebuild maps[]
     * (especially WBN, which runs every frame to pick up async cache
     * updates) shuffle row indices, and a stale selectedIdx would
     * point the breadcrumb + preview at a different row than the one
     * the user clicked. */
    if (state->selectedPath[0] != '\0') {
        for (int i = 0; i < state->numMaps; i++) {
            if (SDL_strcmp(state->maps[i].path,
                           state->selectedPath) == 0) {
                state->selectedIdx = i;
                break;
            }
        }
    }
}

void mapChooserRefresh(MapChooserState *state) {
    if (state) discoverMaps(state);
}

static void updatePreview(MapChooserState *state, SDL_Renderer *renderer) {
    /* Close the popup if it's showing the old map */
    mapPreviewPopupClose();

    if (state->randomMapSelected) {
        /* Random map preview is managed by generateRandomPreview */
        return;
    }

    /* Clear stashed compressed data from random maps */
    if (state->compressedData) {
        SDL_free(state->compressedData);
        state->compressedData = NULL;
        state->compressedLen = 0;
    }

    if (state->previewTex) {
        SDL_DestroyTexture(state->previewTex);
        state->previewTex = NULL;
    }

    int pills = 0, bases = 0, starts = 0;

    /* Inbuilt Everard is selected when the active path is empty.
     * The legacy form also keyed off selectedIdx == 0, but with
     * the WBN tab populating maps[0] with a real catalogue entry
     * that test misfired (clicking the first WBN row would render
     * Everard instead of a "loading" state).  Empty-path is the
     * canonical inbuilt marker and is enough. */
    if (state->selectedPath[0] == '\0') {
        /* Inbuilt Everard Island */
        state->previewTex = buildEverardPreview(renderer,
            &state->previewBoundsMinX, &state->previewBoundsMinY,
            &state->previewBoundsMaxX, &state->previewBoundsMaxY,
            &pills, &bases, &starts);
    } else {
        state->previewTex = buildPreviewFromMapPath(renderer,
            state->selectedPath,
            &state->previewBoundsMinX, &state->previewBoundsMinY,
            &state->previewBoundsMaxX, &state->previewBoundsMaxY,
            &pills, &bases, &starts);
    }

    state->previewPills = pills;
    state->previewBases = bases;
    state->previewStarts = starts;

    /* Feed the same selection into the interactive widget so its
     * tile-based preview matches. Inbuilt Everard falls back to the
     * known path; everything else uses the discovered map path. */
    if (state->previewView) {
        if (state->selectedPath[0] == '\0') {
            mapPreviewViewLoadFile(state->previewView,
                                    "data/maps/Everard Island.map");
        } else {
            mapPreviewViewLoadFile(state->previewView, state->selectedPath);
        }
        mapPreviewViewSetInitialBounds(state->previewView,
            state->previewBoundsMinX, state->previewBoundsMinY,
            state->previewBoundsMaxX, state->previewBoundsMaxY);
    }
}

/* Select the first map row the "Scenarios only" tick lets through and
 * preview it, for a folder click. With no such row and the tick off,
 * falls back to the empty path, the inbuilt Everard marker. With the
 * tick on, Everard is hidden too, so the selection and preview are
 * cleared instead. The provider's onSelect is not called: a folder
 * click only moves the chooser's own selection. */
static void selectFirstShownMap(MapChooserState *state,
                                SDL_Renderer *renderer,
                                bool scenariosOnly) {
    int firstFile = -1;
    for (int j = 0; j < state->numMaps; j++) {
        if (!state->maps[j].isFolder &&
            mapChooserEntryPassesScenarioFilter(&state->maps[j],
                                                scenariosOnly)) {
            firstFile = j; break;
        }
    }
    if (firstFile >= 0) {
        state->selectedIdx = firstFile;
        SDL_strlcpy(state->selectedPath, state->maps[firstFile].path,
                    sizeof(state->selectedPath));
        SDL_strlcpy(state->selectedName, state->maps[firstFile].name,
                    sizeof(state->selectedName));
        updatePreview(state, renderer);
        return;
    }
    state->selectedPath[0] = '\0';
    state->selectedName[0] = '\0';
    if (!scenariosOnly) {
        state->selectedIdx = 0;
        updatePreview(state, renderer);
        return;
    }
    /* Nothing shown to select. A fresh preview widget has no map, so
     * the pane goes blank rather than keep the hidden map. */
    state->selectedIdx = -1;
    mapPreviewPopupClose();
    if (state->previewTex) {
        SDL_DestroyTexture(state->previewTex);
        state->previewTex = NULL;
    }
    state->previewPills  = 0;
    state->previewBases  = 0;
    state->previewStarts = 0;
    if (state->previewView) {
        mapPreviewViewDestroy(state->previewView);
        state->previewView = mapPreviewViewCreate();
    }
}

/* Reentry guard for generateRandomPreview. Generation is currently
 * synchronous on the GUI thread, so this can't trip in practice —
 * but if mapGenImguiControls ever grew a callback that ran during
 * the generate call (or if generation moves off-thread later), this
 * keeps the old map texture visible and prevents a double-enter
 * corrupting the chooser state. */
static bool s_generateInFlight = false;

/* Generate a random map preview from the current config.
 * When keepCamera is true the interactive preview widget snapshots
 * its zoom/pan across the reload so tweaking a slider doesn't
 * snap the view back to auto-fit. Caller passes false on first
 * generation (no prior camera worth keeping). */
static void generateRandomPreview(MapChooserState *state, SDL_Renderer *renderer,
                                  bool keepCamera) {
    if (s_generateInFlight) return;
    s_generateInFlight = true;

    /* Close the popup if it's showing the old random map */
    mapPreviewPopupClose();

    /* Set region to full playable area */
    state->genConfig.x1 = MAP_MINE_EDGE_LEFT + 1; state->genConfig.y1 = MAP_MINE_EDGE_TOP + 1;
    state->genConfig.x2 = MAP_MINE_EDGE_RIGHT - 1; state->genConfig.y2 = MAP_MINE_EDGE_BOTTOM - 1;

    /* Generate map + compressed serialization in one call. The mapeditor
     * owns the substruct lifetimes; the returned MapPreview is heap-owned
     * and read via clientMapPreview* accessors. */
    BYTE *buf = (BYTE *)SDL_malloc(256 * 1024);
    int   compressedLen = 0;
    MapPreview *view = NULL;
    if (buf) {
        view = mapGenRunAsPreview(&state->genConfig, buf, 256 * 1024, &compressedLen);
    }

    /* Build preview texture */
    if (state->previewTex) {
        SDL_DestroyTexture(state->previewTex);
        state->previewTex = NULL;
    }

    MinimapBounds mb = {0, 0, 0, 0};
    if (view) {
        state->previewTex = minimapCreateTexture(renderer, view, &mb, 0);
        state->previewPills  = clientMapPreviewGetLivePillCount(view);
        state->previewBases  = clientMapPreviewGetLiveBaseCount(view);
        state->previewStarts = clientMapPreviewGetLiveStartCount(view);
        clientMapPreviewDestroy(view);
    }
    state->previewBoundsMinX = mb.minX;
    state->previewBoundsMinY = mb.minY;
    state->previewBoundsMaxX = mb.maxX;
    state->previewBoundsMaxY = mb.maxY;

    /* Stash compressed map data for popup preview */
    if (state->compressedData) { SDL_free(state->compressedData); state->compressedData = NULL; }
    if (buf && compressedLen > 0) {
        state->compressedData = (BYTE *)SDL_realloc(buf, compressedLen);
        if (!state->compressedData) state->compressedData = buf;
        state->compressedLen = compressedLen;
    } else if (buf) {
        SDL_free(buf);
    }

    /* Mirror into the interactive preview widget. Live preview
     * rebuilds (slider tweak, lock toggle, etc.) take the
     * keep-camera path so the user's zoom/pan survives; the first
     * generation falls through to SetInitialBounds so it gets a
     * sensible auto-fit. */
    if (state->previewView && state->compressedData && state->compressedLen > 0) {
        if (keepCamera) {
            mapPreviewViewLoadCompressedKeepCamera(state->previewView,
                                                    state->compressedData,
                                                    state->compressedLen);
        } else {
            mapPreviewViewLoadCompressed(state->previewView,
                                          state->compressedData,
                                          state->compressedLen);
            mapPreviewViewSetInitialBounds(state->previewView,
                state->previewBoundsMinX, state->previewBoundsMinY,
                state->previewBoundsMaxX, state->previewBoundsMaxY);
        }
    }

    /* Update seed display */
    mapGenConfigToSeed(&state->genConfig, state->genSeedBuf, sizeof(state->genSeedBuf));

    /* Store the seed as the selected path for the caller */
    SDL_snprintf(state->selectedPath, FILENAME_MAX, "randommap:%s", state->genSeedBuf);

    /* Refresh the chooser title above the preview to match the name
     * the server will use for this generation. Shared helper
     * (mapGenBuildDisplayName) so the two stay in sync. */
    mapGenBuildDisplayName(&state->genConfig,
                           state->selectedName,
                           sizeof(state->selectedName));

    s_generateInFlight = false;
}

static void initGenConfig(MapChooserState *state) {
    if (state->genConfigInit) return;
    state->genConfig = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
    state->genConfig.bases = 16;
    state->genConfig.pills = 16;
    state->genConfig.starts = 16;
    state->genConfig.locks |= MAPGEN_LOCK_BASES | MAPGEN_LOCK_PILLS | MAPGEN_LOCK_STARTS;
    state->genConfig.seed = (uint32_t)time(NULL) ^ ((uint32_t)SDL_GetTicks() << 16);
    if (state->genConfig.seed == 0) state->genConfig.seed = 1;
    state->genSeedBuf[0] = '\0';
    state->genConfigInit = true;
}

void mapChooserInit(MapChooserState *state, SDL_Renderer *renderer) {
    SDL_memset(state, 0, sizeof(*state));
    state->selectedIdx = 0;
    state->selectedPath[0] = '\0';
    SDL_strlcpy(state->selectedName, langGetText(STR_MAPCHOOSER_EVERARD), sizeof(state->selectedName));
    state->fileDialogPending = false;
    state->fileDialogGotResult = false;
    state->randomMapSelected = false;
    state->genConfigInit = false;

    /* Ensure the shared generate controls can load lock icons */
    mapEditorImguiSetRenderer(renderer);

    /* Interactive preview widget — same renderer as the lobby's
     * inline preview. mapChooserDestroy frees it. */
    state->previewView = mapPreviewViewCreate();
    state->previewLastW = 0;
    state->previewLastH = 0;

    discoverMaps(state);

    /* Default selection is Everard (the inbuilt map — empty path). The
     * alphabetical sort in discoverMaps may have moved it from index 0;
     * find its new position so the highlighted row matches the
     * "currently selected" name we wrote above. */
    for (int i = 0; i < state->numMaps; i++) {
        if (state->maps[i].path[0] == '\0') {
            state->selectedIdx = i;
            break;
        }
    }

    updatePreview(state, renderer);
    state->initialized = true;
}

/* Render the zoomed minimap preview and return true if displayed */
static bool renderPreviewImage(MapChooserState *state) {
    if (!state->previewTex) return false;

    int pad = 4;
    int bx0 = state->previewBoundsMinX - pad; if (bx0 < 0) bx0 = 0;
    int by0 = state->previewBoundsMinY - pad; if (by0 < 0) by0 = 0;
    int bx1 = state->previewBoundsMaxX + pad; if (bx1 >= PREVIEW_SIZE) bx1 = PREVIEW_SIZE - 1;
    int by1 = state->previewBoundsMaxY + pad; if (by1 >= PREVIEW_SIZE) by1 = PREVIEW_SIZE - 1;
    /* Make region square */
    int bw = bx1 - bx0;
    int bh = by1 - by0;
    if (bw > bh) {
        int diff = bw - bh;
        by0 -= diff / 2;
        by1 += (diff + 1) / 2;
        if (by0 < 0) { by1 -= by0; by0 = 0; }
        if (by1 >= PREVIEW_SIZE) { by0 -= (by1 - PREVIEW_SIZE + 1); by1 = PREVIEW_SIZE - 1; }
        if (by0 < 0) by0 = 0;
    } else if (bh > bw) {
        int diff = bh - bw;
        bx0 -= diff / 2;
        bx1 += (diff + 1) / 2;
        if (bx0 < 0) { bx1 -= bx0; bx0 = 0; }
        if (bx1 >= PREVIEW_SIZE) { bx0 -= (bx1 - PREVIEW_SIZE + 1); bx1 = PREVIEW_SIZE - 1; }
        if (bx0 < 0) bx0 = 0;
    }
    ImVec2 uv0((float)bx0 / PREVIEW_SIZE, (float)by0 / PREVIEW_SIZE);
    ImVec2 uv1((float)(bx1 + 1) / PREVIEW_SIZE, (float)(by1 + 1) / PREVIEW_SIZE);

    float panelWidth = ImGui::GetContentRegionAvail().x;
    /* Reserve space for one line of text + spacing below the image */
    float reserveH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    float availH = ImGui::GetContentRegionAvail().y - reserveH;
    float previewSize = panelWidth;
    if (availH < previewSize) previewSize = availH;
    if (previewSize < 32) previewSize = 32;
    /* Snap to integer pixels to avoid sub-pixel blurriness */
    previewSize = floorf(previewSize);

    float offsetX = floorf((panelWidth - previewSize) * 0.5f);
    if (offsetX > 0) ImGui::SetCursorPosX(floorf(ImGui::GetCursorPosX() + offsetX));
    SDL_SetTextureScaleMode(state->previewTex, SDL_SCALEMODE_NEAREST);
    ImGui::Image((ImTextureID)state->previewTex, ImVec2(previewSize, previewSize), uv0, uv1);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsItemClicked()) {
        if (state->compressedData && state->compressedLen > 0) {
            /* Random map — use stashed compressed data */
            mapPreviewPopupOpenCompressed(state->compressedData, state->compressedLen,
                                          state->previewBoundsMinX, state->previewBoundsMinY,
                                          state->previewBoundsMaxX, state->previewBoundsMaxY);
        } else {
            const char *popupPath = state->selectedPath;
            if (popupPath[0] == '\0') popupPath = "data/maps/Everard Island.map";
            mapPreviewPopupOpenFile(popupPath,
                                    state->previewBoundsMinX, state->previewBoundsMinY,
                                    state->previewBoundsMaxX, state->previewBoundsMaxY);
        }
    }
    return true;
}

bool mapChooserRender(MapChooserState *state, SDL_Renderer *renderer,
                      float width, float height, float scale) {
    bool changed = false;

    /* Reset hover-row tracking; the row render block re-sets this
     * for whichever row is hovered this frame. -1 means none. */
    state->lastHoveredIdx = -1;

    /* Promote any thumbnail that the worker thread finished writing
     * to disk since the last frame. Cheap when nothing's pending. */
    /* Always poll — both grid view (inline thumbs) and list view
     * (hover-preview tooltips) need cache-ready promotions. Cheap
     * when nothing's pending. */
    mapPreviewCachePollDisk(renderer);

    /* Handle file dialog result */
    if (state->fileDialogGotResult) {
        state->fileDialogGotResult = false;
        if (state->fileDialogResult[0] != '\0') {
            state->selectedIdx = -1; /* custom file, not in list */
            state->randomMapSelected = false;
            SDL_strlcpy(state->selectedPath, state->fileDialogResult, FILENAME_MAX);

            /* Extract filename for display */
            const char *base = state->fileDialogResult;
            for (const char *p = state->fileDialogResult; *p; p++) {
                if (*p == '/' || *p == '\\') base = p + 1;
            }
            SDL_strlcpy(state->selectedName, base, sizeof(state->selectedName));
            size_t len = SDL_strlen(state->selectedName);
            if (len > 4 && SDL_strcasecmp(state->selectedName + len - 4, ".map") == 0) {
                state->selectedName[len - 4] = '\0';
            }

            updatePreview(state, renderer);
            changed = true;
            /* Apply the picked file the same way a row click does —
             * the provider loads it (single player) or queues the
             * upload (multiplayer). selectedPath holds the absolute
             * on-disk path the dialog returned. */
            if (state->provider.onSelect) {
                state->provider.onSelect(state, state->provider.ctx);
            }
        }
    }

    /* Layout: left panel (map list) + draggable splitter + preview.
     * Default split: if leftPanelMaxW > 0 the caller wants a narrow
     * list (lobby chooser); otherwise the preview keeps its natural
     * minimap-sized panel and the list claims the rest. The user's
     * splitDragOffset (persisted on state) shifts the divider after
     * the default is computed — so they can give either side more
     * room and that ratio survives until the next re-open. */
    const float kSplitterW = 6.0f;
    float leftPanelW;
    float previewPanelW;
    if (state->leftPanelMaxW > 0.0f) {
        leftPanelW = state->leftPanelMaxW;
        if (leftPanelW > width * 0.5f) leftPanelW = width * 0.5f;
        previewPanelW = width - leftPanelW - kSplitterW;
        if (previewPanelW < 100.0f) {
            previewPanelW = (PREVIEW_SIZE + 40) * (scale > 1.5f ? 0.75f : 1.0f);
            if (previewPanelW > width * 0.55f) previewPanelW = width * 0.55f;
            leftPanelW = width - previewPanelW - kSplitterW;
        }
    } else {
        previewPanelW = (PREVIEW_SIZE + 40) * (scale > 1.5f ? 0.75f : 1.0f);
        if (previewPanelW > width * 0.55f) previewPanelW = width * 0.55f;
        leftPanelW = width - previewPanelW - kSplitterW;
    }
    /* Apply the persisted drag delta and clamp both sides to a
     * minimum so the user can't drag either panel down to zero. */
    {
        const float kMinPanel = 120.0f;
        leftPanelW    += state->splitDragOffset;
        previewPanelW -= state->splitDragOffset;
        if (leftPanelW < kMinPanel) {
            float over = kMinPanel - leftPanelW;
            leftPanelW    += over;
            previewPanelW -= over;
            state->splitDragOffset += over;
        }
        if (previewPanelW < kMinPanel) {
            float over = kMinPanel - previewPanelW;
            previewPanelW += over;
            leftPanelW    -= over;
            state->splitDragOffset -= over;
        }
    }

    /* Left panel */
    ImGui::BeginChild("##MapLeft", ImVec2(leftPanelW, height), ImGuiChildFlags_Borders);

    if (state->randomMapSelected || state->randomTabOnly) {
        /* --- Random map generator mode ---
         *
         * randomTabOnly clients (lobby's Random tab) skip the
         * "Back to map list" button — they live inside a separate
         * tab and there's no list to go back to here. */
        if (!state->randomTabOnly) {
            if (ImGui::Button(langGetText(STR_MAPCHOOSER_LOADMAP), ImVec2(-1, 0))) {
                state->randomMapSelected = false;
                updatePreview(state, renderer);
                changed = true;
            }
            ImGui::Separator();
        } else if (!state->randomMapSelected) {
            /* First render in randomTabOnly mode — bootstrap so the
             * controls and preview show up immediately. */
            state->randomMapSelected = true;
            state->selectedIdx = MAP_CHOOSER_IDX_RANDOM;
            initGenConfig(state);
            generateRandomPreview(state, renderer, /*keepCamera=*/false);
            state->genSeq++;
            changed = true;
        }

        /* Scrollable generator options */
        ImGui::BeginChild("##GenOptions", ImVec2(0, 0));

        MapGenConfig *cfg = &state->genConfig;

        if (mapGenImguiControls(cfg)) {
            generateRandomPreview(state, renderer, /*keepCamera=*/true);
            state->genSeq++;
            changed = true;
        }

        ImGui::EndChild(); /* ##GenOptions */
    } else {
        /* --- Normal map list mode --- */

        if (!state->hideExtras || state->showDeviceLoad) {
            /* Load from device button — shown whenever extras are
             * enabled, or on demand via showDeviceLoad (lobby's
             * local/upload tab) so a single-file picker sits above the
             * list without dragging the Generate Random button along. */
            {
                bool disabled = state->fileDialogPending;
                if (disabled) ImGui::BeginDisabled();
                if (ImGui::Button(langGetText(STR_MAPCHOOSER_LOADDEVICE), ImVec2(-1, 0))) {
                    SDL_Window *window = sdl3DrawGetWindow();
                    SDL_DialogFileFilter filters[] = {
                        { langGetText(STR_MAPCHOOSER_MAPFILES), "map" },
                        { langGetText(STR_MAPCHOOSER_ALLFILES), "*" },
                    };
                    state->fileDialogPending = true;
                    state->fileDialogGotResult = false;
                    SDL_ShowOpenFileDialog(mapChooserFileDialogCallback, state, window, filters, 2, NULL, false);
                }
                if (disabled) ImGui::EndDisabled();
            }

            /* Generate Random Map button — gated on the in-flight
             * flag so a reentrant call (e.g. ImGui repeated activation
             * across one frame) can't start a second generate on top
             * of an already-running one. Suppressed for showDeviceLoad-
             * only callers (the lobby keeps generation on its own tab). */
            if (!state->hideExtras) {
                if (s_generateInFlight) ImGui::BeginDisabled();
                bool genClicked = ImGui::Button(langGetText(STR_MAPCHOOSER_GENRANDOM), ImVec2(-1, 0));
                if (s_generateInFlight) ImGui::EndDisabled();
                if (genClicked) {
                    state->randomMapSelected = true;
                    state->selectedIdx = MAP_CHOOSER_IDX_RANDOM;
                    SDL_strlcpy(state->selectedName, langGetText(STR_MAPCHOOSER_RANDOMMAP), sizeof(state->selectedName));
                    initGenConfig(state);
                    generateRandomPreview(state, renderer, /*keepCamera=*/false);
                    changed = true;
                }
            }

            ImGui::Separator();
        }

        /* Path breadcrumb above the search field — shares its
         * renderer with the right-anchored breadcrumb under the
         * preview, so segments are clickable here too. The pieces:
         *   - state->crumbsRootLabel ("Maps" on all tabs today)
         *   - state->currentDir, stripped of any "data/maps[/]" prefix
         *     and with the Workshop directory read as "Workshop", so
         *     Upload's absolute path matches the Server / WBN
         *     conventions before the root label is prepended.
         * Full absolute path lives in the hover tooltip
         * (pathTooltipPrefix) for callers that want one. */
        {
            char rel[FILENAME_MAX];
            mapChooserCrumbRel(state, state->currentDir, rel, sizeof(rel));
            char pathLine[FILENAME_MAX + 64];
            if (state->crumbsRootLabel[0] != '\0' && rel[0] != '\0') {
                SDL_snprintf(pathLine, sizeof(pathLine), "%s/%s",
                             state->crumbsRootLabel, rel);
            } else if (state->crumbsRootLabel[0] != '\0') {
                SDL_strlcpy(pathLine, state->crumbsRootLabel,
                            sizeof(pathLine));
            } else if (rel[0] != '\0') {
                SDL_snprintf(pathLine, sizeof(pathLine), "maps/%s", rel);
            } else {
                SDL_strlcpy(pathLine, "maps", sizeof(pathLine));
            }

            renderBreadcrumbSegments(state, pathLine, false);

            /* Tooltip on hover anywhere over the breadcrumb row —
             * uses the last-drawn item's rect as a proxy since each
             * segment is its own item. */
            if (state->pathTooltipPrefix[0] != '\0' &&
                (ImGui::IsItemHovered() || ImGui::IsItemFocused())) {
                char tipBuf[FILENAME_MAX * 2];
                if (state->currentDir[0] != '\0') {
                    SDL_snprintf(tipBuf, sizeof(tipBuf), "%s/%s",
                                 state->pathTooltipPrefix,
                                 state->currentDir);
                } else {
                    SDL_strlcpy(tipBuf, state->pathTooltipPrefix,
                                sizeof(tipBuf));
                }
                ImGui::SetTooltip("%s", tipBuf);
            }
        }

        /* Search filter + recursive toggle. The text field is a
         * case-insensitive substring match on the displayed map
         * names; "Search subfolders" extends the search to the
         * entire subtree (re-runs discoverMaps so the list is
         * actually replaced, not just visually filtered).
         *
         * Recursive search hides the synthetic ".." and inbuilt
         * Everard since results may come from any depth. The user
         * untoggles or clears the search to get the navigable
         * folder view back. */
        /* Track text edits and recursive-toggle edits independently
         * so we can drive discoverMaps with the right policy:
         *
         *   - Text edit:
         *       - in recursive mode → re-run discoverMaps so the
         *         provider can fire a fresh search request.
         *       - in plain mode → the per-row client filter handles
         *         it; no rescan needed.
         *   - Recursive toggle:
         *       - ALWAYS re-run discoverMaps. Switching ON wants the
         *         server's recursive results; switching OFF wants the
         *         plain folder listing back — both replace
         *         state->maps wholesale. */
        bool textChanged   = false;
        bool toggleChanged = false;
        {
            char prevFilter[64];
            SDL_strlcpy(prevFilter, state->searchFilter,
                        sizeof(prevFilter));
            /* Reserve a square button slot on the right for the
             * clear-X — the InputText fills everything else. */
            float xBtnW = ImGui::GetFrameHeight();
            float gap   = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetNextItemWidth(-(xBtnW + gap));
            ImGui::InputTextWithHint("##MapSearch", "Search...",
                                     state->searchFilter,
                                     sizeof(state->searchFilter));
            if (SDL_strcmp(prevFilter, state->searchFilter) != 0) {
                textChanged = true;
            }
            ImGui::SameLine(0.0f, gap);
            bool emptyFilter = (state->searchFilter[0] == '\0');
            if (emptyFilter) ImGui::BeginDisabled();
            if (ImGui::Button("X##MapSearchClear",
                              ImVec2(xBtnW, xBtnW))) {
                state->searchFilter[0] = '\0';
                textChanged = true;
            }
            if (emptyFilter) ImGui::EndDisabled();
            if ((ImGui::IsItemHovered() || ImGui::IsItemFocused()) && !emptyFilter) {
                ImGui::SetTooltip("Clear search");
            }
            /* The two search-option checkboxes draw at a smaller
             * font and frame so they read as secondary to the search
             * box above them. */
            ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 0.85f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                ImVec2(ImGui::GetStyle().FramePadding.x,
                       ImGui::GetStyle().FramePadding.y * 0.5f));
            bool prevRecursive = state->searchRecursive;
            ImGui::Checkbox("Search subfolders", &state->searchRecursive);
            if (prevRecursive != state->searchRecursive) {
                toggleChanged = true;
            }
            /* "Created at" toggle — controls whether the table renders
             * the Modified column. Off by default so the list stays
             * compact; hover gives a tooltip explaining the trade. */
            ImGui::SameLine();
            ImGui::Checkbox("Created at", &state->showModifiedColumn);
            imguiHelpTooltip("Show file modification times in a second column.");
            ImGui::PopStyleVar();
            ImGui::PopFont();
        }
        /* Stale-state safety net: in non-recursive mode the legacy
         * enumerate populates state->maps with basenames only — no
         * path separators. If any entry's name contains "/" or "\"
         * we know state is left over from a previous recursive search
         * (the entries are relative paths like "Map Test/Foo"), and
         * a refresh is required even when neither text nor toggle
         * just changed (e.g. dialog reopened with stale state). */
        bool staleRecursive = false;
        if (!state->searchRecursive) {
            for (int i = 0; i < state->numMaps; i++) {
                if (SDL_strchr(state->maps[i].name, '/')  != NULL ||
                    SDL_strchr(state->maps[i].name, '\\') != NULL) {
                    staleRecursive = true;
                    break;
                }
            }
        }
        if (toggleChanged || staleRecursive ||
            (textChanged && state->searchRecursive) ||
            (textChanged && state->searchFilter[0] == '\0')) {
            discoverMaps(state);
        }

        /* Skip the per-row client-side filter when recursive search
         * is active — discoverMaps already filtered the entries via
         * the recursive walker (or the network search response),
         * and the display names are relative paths that don't
         * necessarily contain the query as a substring (the query
         * matched the BASENAME, not the parent dir). */
        bool hasFilter = state->searchFilter[0] != '\0'
                      && !state->searchRecursive;
        ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg
                                   | ImGuiTableFlags_ScrollY
                                   | ImGuiTableFlags_Resizable
                                   | ImGuiTableFlags_Sortable
                                   | ImGuiTableFlags_SortTristate
                                   | ImGuiTableFlags_BordersInnerH
                                   | ImGuiTableFlags_BordersInnerV;
        /* List / grid view-mode toggle, directly above the
         * list/grid area. */
        renderViewModeToggle(state, renderer);
        /* "Scenarios only" — a client-side filter like the plain
         * search, so a change needs no rescan. Sits right of the view
         * toggle, at the search options' smaller font and frame, or
         * on its own line below when the list panel is too narrow.
         * Greyed out while the rows come from a server-wide search,
         * whose reply does not say which hits have a script; ticked,
         * it would hide every hit. */
        if (state->offerScenariosOnly) {
            ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 0.85f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                ImVec2(ImGui::GetStyle().FramePadding.x,
                       ImGui::GetStyle().FramePadding.y * 0.5f));
            float boxW = ImGui::GetFrameHeight()
                       + ImGui::GetStyle().ItemInnerSpacing.x
                       + ImGui::CalcTextSize(
                             langGetText(STR_MAPCHOOSER_SCENARIOSONLY)).x;
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < boxW) {
                ImGui::NewLine();
            }
            bool untagged = state->searchRowsUntagged;
            if (untagged) ImGui::BeginDisabled();
            ImGui::Checkbox(langGetText(STR_MAPCHOOSER_SCENARIOSONLY),
                            &state->scenariosOnly);
            if (untagged) ImGui::EndDisabled();
            if (untagged) {
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip |
                                         ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s", langGetText(
                        STR_MAPCHOOSER_SCENARIOSONLY_NOSEARCH));
                }
            } else {
                imguiHelpTooltip(
                    langGetText(STR_MAPCHOOSER_SCENARIOSONLY_TIP));
            }
            ImGui::PopStyleVar();
            ImGui::PopFont();
        }
        bool scenariosOnly = state->offerScenariosOnly
                          && state->scenariosOnly
                          && !state->searchRowsUntagged;
        /* The tick can hide the selected map (ticked just now, or the
         * rows were replaced). Only drop the row highlight, the same
         * "not in the list" state a picked custom file uses. The path
         * and preview stay on that map, because it is still the map
         * "Use This Map" / OK will use: on the Server Maps tab the
         * server keeps the map the last click sent it. Moving the
         * selection to another row would show one map and commit
         * another, and calling onSelect would send a preview to the
         * whole lobby on a filter click. discoverMaps puts the index
         * back by path on a refresh; this check drops it again. */
        if (scenariosOnly && state->selectedIdx >= 0 &&
            state->selectedIdx < state->numMaps) {
            const MapChooserEntry *sel = &state->maps[state->selectedIdx];
            if (SDL_strcmp(sel->path, state->selectedPath) == 0 &&
                !mapChooserEntryPassesScenarioFilter(sel, true)) {
                state->selectedIdx = -1;
            }
        }
        /* Map rows the filters let through this frame, so an empty
         * "Scenarios only" result can say so instead of leaving a
         * blank list. */
        int shownMapRows = 0;

        /* Grid view branches off here entirely — a flowable
         * thumbnail layout. List view continues into the table
         * block below. */
        if (state->viewMode == 1) {
            const float kCellThumb = 256.0f;
            const float kCellPad   = 6.0f;
            const float kCellW     = kCellThumb + kCellPad * 2.0f;
            const float kLabelH    = ImGui::GetTextLineHeightWithSpacing();
            const float kCellH     = kCellThumb + kCellPad * 2.0f + kLabelH;

            ImGui::BeginChild("##MapGridScroll", ImVec2(0.0f, 0.0f),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar);
            float availW = ImGui::GetContentRegionAvail().x;
            float curX   = 0.0f;
            ImGuiStyle &style = ImGui::GetStyle();
            float gap = style.ItemSpacing.x;

            for (int i = 0; i < state->numMaps; i++) {
                const MapChooserEntry &ent = state->maps[i];
                if (hasFilter) {
                    /* Same case-insensitive substring check the
                     * list view applies. */
                    const char *hay = ent.name;
                    size_t needleLen = SDL_strlen(state->searchFilter);
                    bool match = false;
                    for (size_t k = 0; hay[k] != '\0'; k++) {
                        size_t j;
                        for (j = 0; j < needleLen; j++) {
                            char a = hay[k + j];
                            if (a == '\0') break;
                            if (SDL_tolower((unsigned char)a)
                                != SDL_tolower((unsigned char)
                                state->searchFilter[j])) break;
                        }
                        if (j == needleLen) { match = true; break; }
                    }
                    if (!match) continue;
                }
                if (!state->searchRecursive && !ent.isParentUp) {
                    if (SDL_strchr(ent.name, '/') ||
                        SDL_strchr(ent.name, '\\')) continue;
                }
                if (!mapChooserEntryPassesScenarioFilter(&ent,
                                                         scenariosOnly)) {
                    continue;
                }
                if (!ent.isFolder && !ent.isParentUp) shownMapRows++;

                /* Wrap to a new row when the next cell would
                 * overflow horizontally. curX == 0 means we're
                 * already at the left edge. */
                if (curX > 0.0f && curX + kCellW > availW) {
                    ImGui::NewLine();
                    curX = 0.0f;
                }
                if (curX > 0.0f) {
                    ImGui::SameLine();
                }

                ImVec2 cellStart = ImGui::GetCursorScreenPos();
                /* Subtle highlight backdrop for provider-pinned
                 * cells. Drawn BEFORE the Selectable so the
                 * Selectable's hover/active colour still wins on
                 * top — the tint is just an ambient cue. */
                if (ent.highlighted) {
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        cellStart,
                        ImVec2(cellStart.x + kCellW,
                               cellStart.y + kCellH),
                        IM_COL32(255, 215, 80, 28),
                        3.0f);
                }
                char id[32];
                SDL_snprintf(id, sizeof(id), "##gcell%d", i);
                bool selected = (!ent.isFolder) &&
                                (i == state->selectedIdx);
                bool clicked = ImGui::Selectable(id, selected, 0,
                    ImVec2(kCellW, kCellH));
                bool cellHovered = ImGui::IsItemHovered();

                /* Draw the thumbnail centred in the top portion of
                 * the cell. Folder cells use the folder icon as a
                 * larger glyph; file cells pull from the preview
                 * cache. Parent-up cells get a "[..]" placeholder. */
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImVec2 imgTL(cellStart.x + kCellPad,
                              cellStart.y + kCellPad);
                ImVec2 imgBR(imgTL.x + kCellThumb,
                              imgTL.y + kCellThumb);

                if (ent.isParentUp) {
                    /* Centred "[..]" text inside the thumb area. */
                    const char *upLbl = "[..]";
                    ImVec2 ts = ImGui::CalcTextSize(upLbl);
                    dl->AddText(
                        ImVec2(imgTL.x + (kCellThumb - ts.x) * 0.5f,
                               imgTL.y + (kCellThumb - ts.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_Text), upLbl);
                } else if (ent.isFolder) {
                    if (s_iconFolder) {
                        float fs = kCellThumb * 0.7f;
                        ImVec2 ftl(imgTL.x + (kCellThumb - fs) * 0.5f,
                                    imgTL.y + (kCellThumb - fs) * 0.5f);
                        dl->AddImage((ImTextureID)s_iconFolder, ftl,
                            ImVec2(ftl.x + fs, ftl.y + fs));
                    }
                } else if (state->provider.generatePreview &&
                           ent.path[0] != '\0') {
                    SDL_Texture *thumb = mapPreviewCacheGet(renderer,
                        ent.path, ent.modTime, &state->provider);
                    if (thumb) {
                        float texW = 0.0f, texH = 0.0f;
                        SDL_GetTextureSize(thumb, &texW, &texH);
                        float scale = 1.0f;
                        if (texW > 0.0f && texH > 0.0f) {
                            float maxDim = (texW > texH) ? texW : texH;
                            scale = kCellThumb / maxDim;
                        }
                        float drawW = texW * scale;
                        float drawH = texH * scale;
                        ImVec2 t(imgTL.x + (kCellThumb - drawW) * 0.5f,
                                  imgTL.y + (kCellThumb - drawH) * 0.5f);
                        dl->AddImage((ImTextureID)thumb, t,
                            ImVec2(t.x + drawW, t.y + drawH));
                    }
                }

                /* Label below the thumb. Centred + truncated to
                 * cell width. */
                {
                    const char *lbl = ent.name;
                    ImVec2 ts = ImGui::CalcTextSize(lbl);
                    float lx = cellStart.x +
                               (kCellW - ts.x) * 0.5f;
                    if (lx < cellStart.x + kCellPad)
                        lx = cellStart.x + kCellPad;
                    ImVec2 lpos(lx, cellStart.y + kCellPad
                                     + kCellThumb + 2.0f);
                    /* PushClipRect bounds the text to the cell. */
                    ImVec2 clipMin(cellStart.x + kCellPad, lpos.y);
                    ImVec2 clipMax(cellStart.x + kCellW - kCellPad,
                                    cellStart.y + kCellH - kCellPad);
                    dl->PushClipRect(clipMin, clipMax, true);
                    dl->AddText(lpos,
                        ImGui::GetColorU32(ImGuiCol_Text), lbl);
                    dl->PopClipRect();
                }

                /* No hover tooltip in grid view — the inline
                 * thumbnail already shows the preview at full size,
                 * so a popup would be redundant. */
                (void)cellHovered;

                /* Click handling: same logic as list view —
                 * folders navigate, files select + onSelect. */
                if (clicked) {
                    if (ent.isFolder) {
                        SDL_strlcpy(state->currentDir, ent.path,
                                    sizeof(state->currentDir));
                        discoverMaps(state);
                        state->searchFilter[0] = '\0';
                        selectFirstShownMap(state, renderer,
                                            scenariosOnly);
                        changed = true;
                    } else if (state->selectedIdx != i) {
                        state->selectedIdx = i;
                        SDL_strlcpy(state->selectedPath, ent.path,
                                    FILENAME_MAX);
                        SDL_strlcpy(state->selectedName, ent.name,
                                    sizeof(state->selectedName));
                        updatePreview(state, renderer);
                        changed = true;
                        if (state->provider.onSelect) {
                            state->provider.onSelect(state,
                                state->provider.ctx);
                        }
                    }
                }

                curX += kCellW + gap;
            }
            if (scenariosOnly && shownMapRows == 0) {
                ImGui::TextDisabled("%s",
                    langGetText(STR_MAPCHOOSER_NOSCENARIOMAPS));
            }
            ImGui::EndChild();
        } else
        /* ── List view (the original table) ──────────────── */
        if (true) {
        const int kNumCols = (state->showModifiedColumn ? 2 : 1) + 1;
        const int kStarColIdx = state->showModifiedColumn ? 2 : 1;
        const float kStarColW = ImGui::GetFrameHeight() + 4.0f;
        if (ImGui::BeginTable("##MapTable", kNumCols, tableFlags)) {
            ImGui::TableSetupColumn("Name",
                ImGuiTableColumnFlags_WidthStretch
                | ImGuiTableColumnFlags_PreferSortAscending, 1.0f,
                0 /* user_id 0 = name column */);
            /* "YYYY-MM-DD HH:MM" plus a few pixels of padding — at the
             * default font size 130 px clears the trailing minutes
             * with a touch of breathing room so nothing clips. */
            if (state->showModifiedColumn) {
                ImGui::TableSetupColumn("Created",
                    ImGuiTableColumnFlags_WidthFixed
                    | ImGuiTableColumnFlags_PreferSortDescending, 130.0f,
                    1 /* user_id 1 = modified column */);
            }
            /* Star column — fixed-width icon-only, no sort, no header
             * label. Sits at the right edge of every row. */
            ImGui::TableSetupColumn("##Star",
                ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_NoSort
                | ImGuiTableColumnFlags_NoResize, kStarColW,
                2 /* user_id 2 = star column */);
            ImGui::TableSetupScrollFreeze(0, 1);
            /* Headers stay navigable (so the user can press Up from the top
             * row to reach the sortable column headers), but they must not be
             * the spot nav lands on when entering the list — that's handled by
             * SetItemDefaultFocus on a row below. NoNavDefaultFocus keeps the
             * headers out of the running for default focus so the row wins. */
            ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
            ImGui::TableHeadersRow();
            ImGui::PopItemFlag();

            /* Click-to-sort. ImGui's TableSortSpecs is set by the
             * header click; we re-sort state->maps when SpecsDirty
             * flips true. SortTristate lets the user click a column
             * three times to remove the sort — when no sort spec is
             * active we fall back to the natural order produced by
             * discoverMaps (folders first, alphabetical within).
             *
             * Folders are pinned to the top of any sort so navigation
             * doesn't get buried under a hundred .map files; ".." is
             * always first when present. */
            ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs();
            if (sortSpecs && sortSpecs->SpecsDirty && state->numMaps > 1) {
                int sortCol = -1;
                bool ascending = true;
                if (sortSpecs->SpecsCount > 0) {
                    sortCol   = sortSpecs->Specs[0].ColumnUserID;
                    ascending = (sortSpecs->Specs[0].SortDirection
                                 == ImGuiSortDirection_Ascending);
                }
                /* Find the first non-parent-up index — keep ".." at
                 * row 0 if it's present. Everything from that index
                 * onward gets sorted. */
                int sortFrom = 0;
                if (state->numMaps > 0 && state->maps[0].isParentUp) {
                    sortFrom = 1;
                }
                if (sortCol >= 0 && state->numMaps - sortFrom > 1) {
                    auto cmp = [&](const MapChooserEntry &a,
                                   const MapChooserEntry &b) -> bool {
                        if (a.isFolder != b.isFolder) return a.isFolder;
                        int c;
                        if (sortCol == 1) {
                            /* Modified — int64 compare. Equal mtimes
                             * fall back to name so the order is
                             * deterministic. */
                            if (a.modTime != b.modTime) {
                                bool aFirst = a.modTime < b.modTime;
                                return ascending ? aFirst : !aFirst;
                            }
                            c = SDL_strcasecmp(a.name, b.name);
                        } else {
                            c = SDL_strcasecmp(a.name, b.name);
                        }
                        return ascending ? (c < 0) : (c > 0);
                    };
                    std::sort(&state->maps[sortFrom],
                              &state->maps[state->numMaps], cmp);
                } else if (sortCol < 0) {
                    /* Tristate "no sort" — restore the default
                     * order by re-running discoverMaps. */
                    discoverMaps(state);
                }
                sortSpecs->SpecsDirty = false;
            }

            /* "Starred" pre-section. Renders every starred entry that
             * belongs to this provider's scope at the top of the
             * table, in insertion order. Hidden in recursive-search
             * mode (results-driven view) and when no stars exist for
             * the scope. Entries are deliberately also still rendered
             * in their normal place below — clicking a star elsewhere
             * shouldn't make a row pop out of the main list. */
            struct StarRenderCtx {
                MapChooserState *state;
                SDL_Renderer *renderer;
                bool *outChanged;
                int rowSeq;
                const float starColW;
            };
            auto renderStarredRow = [](const char *path, const char *name,
                                        bool isFolder, void *vctx) -> bool {
                StarRenderCtx *ctx = (StarRenderCtx *)vctx;
                MapChooserState *s = ctx->state;
                const char *scope = s->provider.cacheScope
                                     ? s->provider.cacheScope : "";

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                bool isSelected = !isFolder &&
                                  s->selectedIdx == -2 - ctx->rowSeq;
                char id[32];
                SDL_snprintf(id, sizeof(id), "##starrow%d", ctx->rowSeq);
                ImVec2 rowStart = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::Selectable(id, isSelected,
                    ImGuiSelectableFlags_SpanAllColumns
                    | ImGuiSelectableFlags_AllowOverlap);
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::SameLine(0.0f, 0.0f);

                /* Folder indent so the icon + name align with the
                 * main-list rows below. */
                bool hasFolderIcon = isFolder && s_iconFolder != nullptr;
                if (hasFolderIcon) {
                    float iconSz = ImGui::GetTextLineHeight();
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                                          + iconSz + 8.0f);
                    ImVec2 iconPos(rowStart.x + 2.0f, rowStart.y);
                    ImGui::GetWindowDrawList()->AddImage(
                        (ImTextureID)s_iconFolder,
                        iconPos,
                        ImVec2(iconPos.x + iconSz, iconPos.y + iconSz));
                }
                ImGui::TextUnformatted(name);

                /* Star column — always full, click un-stars. */
                int starCol = s->showModifiedColumn ? 2 : 1;
                ImGui::TableSetColumnIndex(starCol);
                {
                    float sz = ImGui::GetTextLineHeight();
                    char btnId[32];
                    SDL_snprintf(btnId, sizeof(btnId), "##starbtn%d",
                                  ctx->rowSeq);
                    /* Zero FramePadding so the button doesn't stretch
                     * the row past the Selectable's hit area (see
                     * matching note on the main-list star column). */
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(0.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(1,1,1,0.08f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                          ImVec4(1,1,1,0.15f));
                    bool toggled = s_iconStarFull
                        ? ImGui::ImageButton(btnId,
                            (ImTextureID)s_iconStarFull, ImVec2(sz, sz))
                        : ImGui::SmallButton("*");
                    imguiHelpTooltip("Click to unstar");
                    ImGui::PopStyleColor(3);
                    ImGui::PopStyleVar();
                    if (toggled) {
                        mapStarsToggle(scope, path, name, isFolder);
                    }
                }

                if (clicked) {
                    if (isFolder) {
                        SDL_strlcpy(s->currentDir, path,
                                    sizeof(s->currentDir));
                        discoverMaps(s);
                        s->searchFilter[0] = '\0';
                        int firstFile = -1;
                        for (int j = 0; j < s->numMaps; j++) {
                            if (!s->maps[j].isFolder) { firstFile = j; break; }
                        }
                        s->selectedIdx = (firstFile >= 0) ? firstFile : 0;
                        if (firstFile >= 0) {
                            SDL_strlcpy(s->selectedPath,
                                s->maps[firstFile].path,
                                sizeof(s->selectedPath));
                            SDL_strlcpy(s->selectedName,
                                s->maps[firstFile].name,
                                sizeof(s->selectedName));
                        } else {
                            s->selectedPath[0] = '\0';
                            s->selectedName[0] = '\0';
                        }
                        updatePreview(s, ctx->renderer);
                        *(ctx->outChanged) = true;
                    } else {
                        /* File: use a negative-index sentinel so the
                         * main-list rows don't pick up "selected"
                         * accidentally (their selectedIdx values are
                         * non-negative). */
                        s->selectedIdx = -2 - ctx->rowSeq;
                        SDL_strlcpy(s->selectedPath, path,
                                    sizeof(s->selectedPath));
                        SDL_strlcpy(s->selectedName, name,
                                    sizeof(s->selectedName));
                        updatePreview(s, ctx->renderer);
                        *(ctx->outChanged) = true;
                        if (s->provider.onSelect) {
                            s->provider.onSelect(s, s->provider.ctx);
                        }
                    }
                }

                ctx->rowSeq++;
                return true;
            };
            int starredRowsRendered = 0;
            /* Starred rows do not carry the scripted flag, so the
             * "Scenarios only" tick hides the section the way a
             * recursive search does. */
            if (!(state->searchRecursive &&
                  state->searchFilter[0] != '\0') && !scenariosOnly) {
                const char *scope = state->provider.cacheScope
                                     ? state->provider.cacheScope : "";
                StarRenderCtx ctx{ state, renderer, &changed,
                                    0, kStarColW };
                starredRowsRendered =
                    (int)mapStarsVisitScope(scope, renderStarredRow, &ctx);
            }
            /* Divider row between the starred section and the main
             * list. Implemented as a 1px row using TableSetBgColor on
             * a near-empty row — keeps it in-table so column widths
             * stay aligned. */
            if (starredRowsRendered > 0) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 1.0f);
                for (int c = 0; c < kNumCols; c++) {
                    ImGui::TableSetColumnIndex(c);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                        IM_COL32(255, 255, 255, 40));
                    ImGui::Dummy(ImVec2(0.0f, 1.0f));
                }
            }

            /* When no main-list row is the current selection (custom file /
             * nothing chosen), let the first rendered row claim default nav
             * focus so entering the list still lands on a row, not a header.
             * selectedIdx <= -2 means a starred row is selected — that row
             * claims focus itself, so don't fall back here. */
            bool wantFirstRowFocus = (state->selectedIdx == -1);
            bool firstRowFocusClaimed = false;
            for (int i = 0; i < state->numMaps; i++) {
                const MapChooserEntry &ent = state->maps[i];
                /* Defensive: in non-recursive mode, an entry whose
                 * name contains a path separator is a recursive
                 * remnant left over from a previous search. The
                 * stale-state detection at the top of this scope
                 * triggers a discoverMaps refresh, but in case a
                 * frame slips through with stale state, skip the
                 * row here so the user never sees a "Subdir/Foo"
                 * row in the current-folder view. */
                if (!state->searchRecursive && !ent.isParentUp) {
                    if (SDL_strchr(ent.name, '/')  != NULL ||
                        SDL_strchr(ent.name, '\\') != NULL) {
                        continue;
                    }
                }
                if (hasFilter) {
                    const char *hay = ent.name;
                    const char *needle = state->searchFilter;
                    size_t needleLen = SDL_strlen(needle);
                    bool match = false;
                    for (size_t k = 0; hay[k] != '\0'; k++) {
                        size_t j;
                        for (j = 0; j < needleLen; j++) {
                            char a = hay[k + j];
                            if (a == '\0') break;
                            if (SDL_tolower((unsigned char)a)
                                != SDL_tolower((unsigned char)needle[j])) break;
                        }
                        if (j == needleLen) { match = true; break; }
                    }
                    if (!match) continue;
                }
                if (!mapChooserEntryPassesScenarioFilter(&ent,
                                                         scenariosOnly)) {
                    continue;
                }
                if (!ent.isFolder && !ent.isParentUp) shownMapRows++;

                /* List view rows are name-only; preview lives in
                 * the hover tooltip below, and the grid view (a
                 * separate code path above) is what shows inline
                 * thumbnails. Default row height suffices. */
                ImGui::TableNextRow();
                /* Subtle highlight tint for provider-pinned rows.
                 * TableSetBgColor paints the row background on top
                 * of the alternating RowBg. ~12 alpha keeps it
                 * unobtrusive while still readable as "featured". */
                if (ent.highlighted) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        IM_COL32(255, 215, 80, 28));
                }

                /* Column 0 — selectable name, spans the row so a
                 * click anywhere on the line picks the entry. The
                 * synthetic parent reads as "[..]"; other folder
                 * rows get a small folder icon overlaid before the
                 * name (drawn after the Selectable below). */
                ImGui::TableSetColumnIndex(0);
                char displayBuf[160];
                bool wantFolderIcon = ent.isFolder && !ent.isParentUp
                                    && s_iconFolder != nullptr;
                SDL_strlcpy(displayBuf, ent.name, sizeof(displayBuf));
                /* ImGui clips text to the cell width automatically,
                 * so a long name silently truncates and a hover
                 * tooltip with the full name kicks in below. */
                char selId[24];
                SDL_snprintf(selId, sizeof(selId), "##sel%d", i);
                bool selected = (!ent.isFolder) &&
                                (i == state->selectedIdx);
                ImVec2 rowStart = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::Selectable(selId, selected,
                    ImGuiSelectableFlags_SpanAllColumns
                    | ImGuiSelectableFlags_AllowOverlap);
                /* Make the selected row the default nav landing spot so a
                 * controller "enter list" (A) focuses the current map rather
                 * than the sortable "Name" header. Fall back to the first row
                 * when nothing in this list is the current selection. */
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                } else if (wantFirstRowFocus && !firstRowFocusClaimed) {
                    ImGui::SetItemDefaultFocus();
                    firstRowFocusClaimed = true;
                }
                /* Capture the Selectable's hover rect for the row
                 * tooltip below — IsItemHovered after the text would
                 * only fire on the narrow text strip, missing the
                 * right side of short rows. */
                bool rowHovered = ImGui::IsItemHovered();
                ImGui::SameLine(0.0f, 0.0f);
                /* For folder rows, advance the cursor past the
                 * icon + a 4 px breathing gap before drawing the
                 * name. Using cursor positioning instead of leading
                 * spaces keeps the gap consistent across fonts. */
                if (wantFolderIcon) {
                    float iconSz = ImGui::GetTextLineHeight();
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                                          + iconSz + 8.0f);
                }
                ImGui::TextUnformatted(displayBuf);
                /* A map with a script beside it says so, quietly: one dim
                 * word after the name rather than a column of its own, so
                 * the list reads the same for the plain maps that are most
                 * of it. */
                if (ent.scripted && !ent.isFolder) {
                    ImGui::SameLine(0.0f, 6.0f);
                    ImGui::TextDisabled("%s",
                        langGetText(STR_DLGGAMEINFO_SCRIPTED));
                }
                /* Folder icon drawn directly into the space we just
                 * skipped, at row baseline. */
                if (wantFolderIcon) {
                    float iconSz = ImGui::GetTextLineHeight();
                    ImVec2 iconPos(rowStart.x + 2.0f,
                                   rowStart.y);
                    ImGui::GetWindowDrawList()->AddImage(
                        (ImTextureID)s_iconFolder,
                        iconPos,
                        ImVec2(iconPos.x + iconSz,
                               iconPos.y + iconSz));
                }
                if (rowHovered) {
                    state->lastHoveredIdx = i;
                    /* Hover tooltip — two cases share one popup:
                     *   - In recursive-search mode, surface the
                     *     enclosing folder so the user can tell hits
                     *     in different folders apart.
                     *   - In list view on any file row, also include
                     *     a large preview image when the cache has
                     *     one for this row. Free since we already
                     *     generated the bytes for the inline thumb. */
                    /* Don't open the tooltip inline — at a row-row
                     * boundary, both adjacent Selectables briefly
                     * report hovered=true and we'd stack two popups.
                     * Just record this iteration's index in
                     * state->lastHoveredIdx; the LAST iteration to
                     * set it wins, and the tooltip is rendered once
                     * after the table loop finishes. */
                }

                /* Column 1 — formatted mtime. SDL_TimeToDateTime
                 * gives us a calendar struct in local time which we
                 * render as "YYYY-MM-DD HH:MM". A zero/unknown
                 * modTime shows a dim "—" so the column doesn't
                 * read as broken. Only rendered when the user has
                 * toggled the "Created at" checkbox on. */
                if (state->showModifiedColumn) {
                    ImGui::TableSetColumnIndex(1);
                    if (ent.modTime > 0) {
                        SDL_DateTime dt;
                        if (SDL_TimeToDateTime((SDL_Time)ent.modTime,
                                               &dt, true)) {
                            ImGui::Text("%04d-%02d-%02d %02d:%02d",
                                        dt.year, dt.month, dt.day,
                                        dt.hour, dt.minute);
                        } else {
                            ImGui::TextDisabled("-");
                        }
                    } else {
                        ImGui::TextDisabled("-");
                    }
                }

                /* Star column. Skip on the synthetic ".." row — there
                 * is nothing to favourite about navigating up. The
                 * filled icon shows currently-starred state; clicking
                 * either icon toggles, with the click captured here
                 * (Selectable above has AllowOverlap so the row
                 * selection doesn't fire underneath). */
                if (!ent.isParentUp) {
                    ImGui::TableSetColumnIndex(kStarColIdx);
                    const char *scope = state->provider.cacheScope
                                         ? state->provider.cacheScope : "";
                    bool isStarred = mapStarsIsStarred(scope, ent.path);
                    SDL_Texture *tex = isStarred ? s_iconStarFull
                                                  : s_iconStarEmpty;
                    char btnId[24];
                    SDL_snprintf(btnId, sizeof(btnId), "##star%d", i);
                    bool toggled = false;
                    if (tex) {
                        /* Icon sized to the text line height so it
                         * fits in the row without stretching it. */
                        float sz = ImGui::GetTextLineHeight();
                        /* Zero out ImageButton's default FramePadding
                         * — otherwise it adds ~6 px of vertical slack
                         * that makes the cell taller than the row
                         * Selectable's hit area, leaving a dead
                         * un-hoverable strip below the highlight. */
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(0.0f, 0.0f));
                        ImGui::PushStyleColor(ImGuiCol_Button,
                            ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(1, 1, 1, 0.08f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                            ImVec4(1, 1, 1, 0.15f));
                        toggled = ImGui::ImageButton(btnId,
                            (ImTextureID)tex, ImVec2(sz, sz));
                        imguiHelpTooltip(isStarred
                            ? "Click to unstar"
                            : "Click to star to always appear at the top");
                        ImGui::PopStyleColor(3);
                        ImGui::PopStyleVar();
                    } else {
                        /* Textual fallback if the SVG didn't load. */
                        toggled = ImGui::SmallButton(
                            isStarred ? "*" : "+");
                        imguiHelpTooltip(isStarred
                            ? "Click to unstar"
                            : "Click to star to always appear at the top");
                    }
                    if (toggled) {
                        mapStarsToggle(scope, ent.path, ent.name,
                                        ent.isFolder);
                    }
                }

                if (clicked) {
                    if (ent.isFolder) {
                        SDL_strlcpy(state->currentDir, ent.path,
                                    sizeof(state->currentDir));
                        discoverMaps(state);
                        state->searchFilter[0] = '\0';
                        selectFirstShownMap(state, renderer,
                                            scenariosOnly);
                        changed = true;
                    } else if (state->selectedIdx != i) {
                        state->selectedIdx = i;
                        SDL_strlcpy(state->selectedPath, ent.path,
                                    FILENAME_MAX);
                        SDL_strlcpy(state->selectedName, ent.name,
                                    sizeof(state->selectedName));
                        updatePreview(state, renderer);
                        changed = true;
                        /* Hand off to the provider for tab-specific
                         * follow-up (push to server, queue upload,
                         * kick a WBN download, …). selectedPath /
                         * selectedName are already populated. */
                        if (state->provider.onSelect) {
                            state->provider.onSelect(state,
                                state->provider.ctx);
                        }
                    }
                }
            }
            /* One dim row instead of a blank list when the tick hides
             * every map here. Folder rows above it still show. */
            if (scenariosOnly && shownMapRows == 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s",
                    langGetText(STR_MAPCHOOSER_NOSCENARIOMAPS));
            }
            ImGui::EndTable();
        }

        /* Hover tooltip — one popup per frame, shown for whichever
         * row was the last to report itself hovered. Two adjacent
         * Selectables touching at a pixel boundary both fire
         * IsItemHovered briefly, so we defer the BeginTooltip until
         * after the loop and let the later row win. */
        if (state->lastHoveredIdx >= 0 &&
            state->lastHoveredIdx < state->numMaps) {
            const MapChooserEntry &ent =
                state->maps[state->lastHoveredIdx];
            bool searchMode = state->searchRecursive &&
                               state->searchFilter[0] != '\0';
            bool fileRow    = !ent.isFolder && !ent.isParentUp
                              && ent.path[0] != '\0';
            SDL_Texture *hoverTex = nullptr;
            ImVec2 hoverUV0(0.0f, 0.0f), hoverUV1(1.0f, 1.0f);
            int hoverCropW = 0, hoverCropH = 0;
            if (fileRow && state->provider.generatePreview) {
                hoverTex = mapPreviewCacheGetCropped(renderer,
                    ent.path, ent.modTime, &state->provider,
                    &hoverUV0, &hoverUV1, &hoverCropW, &hoverCropH);
            }
            bool showTooltip = (searchMode && !ent.isParentUp)
                               || hoverTex != nullptr;
            if (showTooltip) {
                /* Cap the tooltip body width to roughly the preview
                 * image's own width + padding so the popup stays
                 * compact. The breadcrumb wraps to fit. */
                const float kHoverMaxDim = 256.0f;
                const float kHoverPopupW = 260.0f;
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(0.0f, 0.0f),
                    ImVec2(kHoverPopupW, FLT_MAX));
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(kHoverPopupW - 8.0f);

                /* In search mode the breadcrumb is the only thing
                 * that disambiguates two same-named files in
                 * different folders — keep it. Title + modified date
                 * stripped from the non-search tooltip: both are
                 * already visible on the row itself, so the popup
                 * just shows the preview. */
                if (searchMode) {
                    char fullPath[FILENAME_MAX];
                    const char *root = state->crumbsRootLabel;
                    const char *crumbs = ent.crumbsPath;
                    if (root[0] != '\0' && crumbs[0] != '\0') {
                        SDL_snprintf(fullPath, sizeof(fullPath),
                                     "%s / %s / %s",
                                     root, crumbs, ent.name);
                    } else if (root[0] != '\0') {
                        SDL_snprintf(fullPath, sizeof(fullPath),
                                     "%s / %s", root, ent.name);
                    } else if (crumbs[0] != '\0') {
                        SDL_snprintf(fullPath, sizeof(fullPath),
                                     "%s / %s", crumbs, ent.name);
                    } else {
                        SDL_strlcpy(fullPath, ent.name, sizeof(fullPath));
                    }
                    ImGui::TextWrapped("%s", fullPath);
                }
                if (hoverTex) {
                    /* Use the cropped bbox the cache computed at
                     * decode time so the popup focuses on the
                     * interesting part of the map. Fall back to
                     * the full texture if no bbox was recorded
                     * (all-transparent / mid-load). */
                    float drawW = (float)hoverCropW;
                    float drawH = (float)hoverCropH;
                    if (drawW <= 0.0f || drawH <= 0.0f) {
                        float tw = 0.0f, th = 0.0f;
                        SDL_GetTextureSize(hoverTex, &tw, &th);
                        drawW = tw; drawH = th;
                    }
                    float scale = 1.0f;
                    if (drawW > 0.0f && drawH > 0.0f) {
                        float maxDim = (drawW > drawH) ? drawW : drawH;
                        if (maxDim > kHoverMaxDim) scale = kHoverMaxDim / maxDim;
                    }
                    ImGui::Image((ImTextureID)hoverTex,
                        ImVec2(drawW * scale, drawH * scale),
                        hoverUV0, hoverUV1);
                }
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        } /* end list-view branch */
    }

    ImGui::EndChild(); /* ##MapLeft */

    /* Draggable splitter between the list and the preview. We render
     * an InvisibleButton at the column boundary, set a horizontal-
     * resize cursor on hover, and feed the delta-x from any drag
     * back into splitDragOffset (clamped above so neither panel
     * collapses). A faint vertical line draws the visual handle —
     * ImGui's default theme doesn't render the InvisibleButton's
     * background. */
    ImGui::SameLine(0, 0.0f);
    {
        ImVec2 splPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##MapChooserSplit",
                               ImVec2(kSplitterW, height));
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();
        if (hovered || active) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (active) {
            state->splitDragOffset += ImGui::GetIO().MouseDelta.x;
        }
        ImU32 col = active   ? IM_COL32(180, 180, 220, 200) :
                    hovered  ? IM_COL32(140, 140, 170, 200) :
                                IM_COL32( 80,  80, 100, 160);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float cx = splPos.x + kSplitterW * 0.5f;
        dl->AddLine(ImVec2(cx, splPos.y + 2.0f),
                    ImVec2(cx, splPos.y + height - 2.0f),
                    col, 1.5f);
    }
    ImGui::SameLine(0, 0.0f);

    /* Right panel: preview + stats */
    ImGui::BeginChild("##MapPreview", ImVec2(previewPanelW, height),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

    ImGui::Text("%s", state->selectedName);
    ImGui::Separator();

    /* Interactive preview — same widget as the lobby's inline view.
     * Wheel zoom, drag pan, deep zoom-out falls back to minimap
     * colours.  Falls through to the static thumbnail (and the
     * "no preview" placeholder) if the widget isn't ready yet.
     *
     * RenderOffscreen lazily parses the source data and flips the
     * IsReady flag — so we call it unconditionally and then check
     * for a valid texture, not the other way around. */
    bool previewShown = false;
    if (state->previewView) {
        float availW = ImGui::GetContentRegionAvail().x;
        float availH = ImGui::GetContentRegionAvail().y
                     - ImGui::GetTextLineHeightWithSpacing() * 1.5f;
        if (availW < 64.0f) availW = 64.0f;
        if (availH < 64.0f) availH = 64.0f;
        /* mapChooserRender is invoked inside the ImGui frame so we
         * swap SDL render target mid-frame — same pattern the
         * chooser already uses for buildPreviewFromMapPath. */
        mapPreviewViewRenderOffscreen(state->previewView, renderer,
                                       (int)availW, (int)availH);
        state->previewLastW = (int)availW;
        state->previewLastH = (int)availH;
        SDL_Texture *tex = mapPreviewViewGetTexture(state->previewView);
        if (tex && mapPreviewViewIsReady(state->previewView)) {
            /* Draw the texture, then overlay an InvisibleButton sized
             * the same way to *claim* the click/drag for this item.
             * Without that, ImGui::Image is non-interactive — a
             * drag-pan on it would bubble up to the parent window,
             * which is now movable, and end up dragging the chooser
             * window itself instead of panning the map. */
            ImVec2 imgPos = ImGui::GetCursorScreenPos();
            bool nearest = mapPreviewViewWantsNearestSampling(state->previewView);
            if (nearest) imguiPushNearestSampling();
            ImGui::Image((ImTextureID)tex, ImVec2(availW, availH));
            if (nearest) imguiPopNearestSampling();
            ImGui::SetCursorScreenPos(imgPos);
            /* Mark the upcoming InvisibleButton as allow-overlap so the
             * maximize icon we draw afterwards (covering a small corner
             * of the same area) can claim its own clicks. Without this,
             * the InvisibleButton — submitted first and covering the
             * whole image — wins hit-testing across the entire region
             * and the maximize button never fires. */
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##MapPreviewDrag",
                                    ImVec2(availW, availH));
            bool hovered = ImGui::IsItemHovered();
            MapPreviewInputOpts opts = { true, true, true, false };
            mapPreviewViewHandleInput(state->previewView, hovered, &opts);

            /* Zoom-level indicator — bottom-right corner of the
             * preview image. Drawn as an overlay so it doesn't
             * disturb the layout. */
            {
                char zoomText[16];
                SDL_snprintf(zoomText, sizeof(zoomText), "%.2fx",
                             mapPreviewViewGetZoom(state->previewView));
                ImVec2 textSize = ImGui::CalcTextSize(zoomText);
                float pad = 6.0f;
                ImVec2 textPos(imgPos.x + availW - textSize.x - pad,
                               imgPos.y + availH - textSize.y - pad);
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImVec2 bgMin(textPos.x - 4.0f, textPos.y - 2.0f);
                ImVec2 bgMax(textPos.x + textSize.x + 4.0f,
                             textPos.y + textSize.y + 2.0f);
                dl->AddRectFilled(bgMin, bgMax,
                                  IM_COL32(0, 0, 0, 160), 4.0f);
                dl->AddText(textPos,
                            IM_COL32(255, 255, 255, 220), zoomText);
            }

            /* Maximize / restore toggle — top-right corner of the
             * preview image. Only rendered if the surrounding window
             * wired up a maximize flag. Translucent black backing so
             * it stays legible over any map terrain.
             *
             * Uses data/ui/maximize.svg when the icon is loaded;
             * falls back to unicode glyphs (⤢ / ⤦) when the SVG
             * texture isn't available (asset missing or render
             * context not initialised yet). */
            if (state->maximizePtr) {
                bool maxed = *state->maximizePtr;
                float btnSz = 24.0f;
                ImVec2 btnPos(imgPos.x + availW - btnSz - 6.0f,
                              imgPos.y + 6.0f);
                ImVec2 saved = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(btnPos);
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImVec4(0.2f, 0.2f, 0.2f, 0.80f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(1.0f, 1.0f, 1.0f, 0.9f));

                bool clicked = false;
                if (s_iconMaximize) {
                    /* Pad the image inside the 24px button so it
                     * matches the glyph-fallback footprint. */
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                        ImVec2(4.0f, 4.0f));
                    ImVec2 imgSz(btnSz - 8.0f, btnSz - 8.0f);
                    clicked = ImGui::ImageButton(
                        maxed ? "##chooseMapRestore" : "##chooseMapMaximize",
                        (ImTextureID)s_iconMaximize, imgSz);
                    ImGui::PopStyleVar();
                } else {
                    const char *label = maxed
                        ? "\xe2\xa4\xa6"   /* ⤦ : restore */
                        : "\xe2\xa4\xa2";  /* ⤢ : maximize */
                    clicked = ImGui::Button(label, ImVec2(btnSz, btnSz));
                }
                if (clicked) {
                    *state->maximizePtr = !maxed;
                }
                imguiHelpTooltip(maxed
                    ? "Restore default size (Esc)"
                    : "Maximize");
                ImGui::PopStyleColor(3);
                ImGui::SetCursorScreenPos(saved);
                /* Submit a zero-size dummy so ImGui re-anchors the
                 * "last item" rect to the restored cursor position
                 * — silences the SetCursorScreenPos warning about
                 * extending boundaries without an item. */
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
            }

            previewShown = true;
        }
    }
    if (!previewShown) {
        if (!renderPreviewImage(state)) {
            if (state->randomMapSelected) {
                ImGui::TextDisabled("%s", langGetText(STR_MAPCHOOSER_CLICKGEN));
            } else if (strncmp(state->selectedPath, "wbn:", 4) == 0) {
                /* WBN download in progress — selectedPath is the
                 * synthetic "wbn:<id>" placeholder until the host
                 * drains the wbn_map_source download and points the
                 * chooser at the map bytes. Render a tiny rotating
                 * spinner glyph so the user sees activity rather
                 * than "no preview". */
                double t = ImGui::GetTime() * 8.0;
                const char *frames[] = {"|", "/", "-", "\\"};
                int idx = ((int)t) & 3;
                ImGui::TextDisabled("%s  Loading preview...",
                                     frames[idx]);
            } else {
                ImGui::TextDisabled("%s", langGetText(STR_MAPCHOOSER_NOPREVIEW));
            }
        }
    }

    if (state->previewTex ||
        (state->previewView && mapPreviewViewIsReady(state->previewView))) {
        MessageArgs args = {};
        args.number = state->previewPills;
        args.number2 = state->previewBases;
        args.number3 = state->previewStarts;
        ImGui::TextUnformatted(langGetTextFmt(STR_MAPCHOOSER_STATS, &args));

        /* Right-anchored breadcrumb of buttons on the same Y line
         * as the stats. Source priority:
         *   1. The selected entry's explicit crumbsPath (set e.g.
         *      by the WBN tab after /maps/info/<id> resolves).
         *   2. The path-shaped parent of the selected entry —
         *      strip "data/maps/" prefix and drop the basename,
         *      so "data/maps/Uploads/Foo.map" → "Uploads".
         *   3. state->activeCrumbsPath — the most recent value
         *      we showed; survives selectedIdx flips during
         *      async file-swap callers like mapChooserSetSelectedFile.
         * Click on a segment jumps to that prefix.  Slash sep
         * renders as plain text so the line reads as a path. */
        char crumbsRendered[FILENAME_MAX] = {0};
        if (state->selectedIdx >= 0 &&
            state->selectedIdx < state->numMaps) {
            const MapChooserEntry *ent =
                &state->maps[state->selectedIdx];
            if (ent->crumbsPath[0] != '\0') {
                SDL_strlcpy(crumbsRendered, ent->crumbsPath,
                            sizeof(crumbsRendered));
            } else if (!ent->isParentUp &&
                       ent->path[0] != '\0' &&
                       strncmp(ent->path, "wbn:", 4) != 0 &&
                       strncmp(ent->path, "randommap:", 10) != 0) {
                /* Filesystem fallback. Folders show their own
                 * path; files show their enclosing folder.
                 *   data/maps/Uploads/Foo.map → "Uploads"
                 *   data/maps/Foo.map         → "data/maps"
                 *   data/maps/Uploads (folder) → "Uploads"
                 *   data/maps (folder root)   → "data/maps"  */
                char tmp[FILENAME_MAX];
                SDL_strlcpy(tmp, ent->path, sizeof(tmp));
                for (char *p = tmp; *p; p++) if (*p == '\\') *p = '/';
                /* If it's a file, drop the basename so we display
                 * the enclosing folder. Folders keep their path. */
                if (!ent->isFolder) {
                    char *lastSep = strrchr(tmp, '/');
                    if (lastSep) *lastSep = '\0';
                }
                /* Strip the on-disk root so the breadcrumb reads
                 * relative to data/maps, with the Workshop directory
                 * read as "Workshop"; if nothing's left, fall back to
                 * "data/maps" so root-level files still surface a path
                 * indicator. */
                char rel[FILENAME_MAX];
                mapChooserCrumbRel(state, tmp, rel, sizeof(rel));
                if (rel[0] == '\0') {
                    /* When the tab supplies a crumbsRootLabel the
                     * label itself IS the root indicator — adding
                     * "data/maps" on top would render as e.g.
                     * "Maps / data / maps". Leave empty so only the
                     * label appears. */
                    if (state->crumbsRootLabel[0] == '\0') {
                        SDL_strlcpy(crumbsRendered, "data/maps",
                                    sizeof(crumbsRendered));
                    }
                } else {
                    SDL_strlcpy(crumbsRendered, rel,
                                sizeof(crumbsRendered));
                }
            }
        }
        if (crumbsRendered[0] != '\0') {
            /* Sync into the persistent slot. The fallback case
             * below uses it when selection has been flipped to
             * -1 (e.g. mapChooserSetSelectedFile post-download). */
            SDL_strlcpy(state->activeCrumbsPath, crumbsRendered,
                        sizeof(state->activeCrumbsPath));
        } else if (state->activeCrumbsPath[0] != '\0' &&
                   state->selectedIdx < 0) {
            /* Custom-file mode: show the breadcrumb we last had. */
            SDL_strlcpy(crumbsRendered, state->activeCrumbsPath,
                        sizeof(crumbsRendered));
        } else if (state->currentDir[0] != '\0') {
            /* No selection AND no active breadcrumb (e.g. just after
             * a breadcrumb-jump cleared selectedIdx) — fall back to
             * the current folder, same source the upper path label
             * uses. Strip the local "data/maps[/]" prefix, and read
             * the Workshop directory as "Workshop", so the Upload tab's
             * absolute currentDir lines up with the relative scheme
             * Server Maps / WBN already use. */
            char rel[FILENAME_MAX];
            mapChooserCrumbRel(state, state->currentDir, rel, sizeof(rel));
            if (rel[0] != '\0') {
                SDL_strlcpy(crumbsRendered, rel, sizeof(crumbsRendered));
            }
        }
        /* Tab-supplied root label (e.g. "Maps" for the Server Maps
         * tab) gets prepended so the breadcrumb reads as a path from
         * the tab's root rather than starting at the first subfolder.
         * Stored unprefixed in activeCrumbsPath so the prefix doesn't
         * compound across frames. The leaf-jump click target below
         * strips the same prefix back off before handing pendingJumpPath
         * to the tab. When crumbsRendered is empty (root) and a label
         * is set, the label alone IS the breadcrumb. */
        if (state->crumbsRootLabel[0] != '\0') {
            char tmp[FILENAME_MAX];
            if (crumbsRendered[0] != '\0') {
                SDL_snprintf(tmp, sizeof(tmp), "%s/%s",
                             state->crumbsRootLabel, crumbsRendered);
            } else {
                SDL_strlcpy(tmp, state->crumbsRootLabel, sizeof(tmp));
            }
            SDL_strlcpy(crumbsRendered, tmp, sizeof(crumbsRendered));
        }
        if (crumbsRendered[0] != '\0') {
            /* Right-anchored on the same Y line as the stats. */
            ImGui::SameLine();
            renderBreadcrumbSegments(state, crumbsRendered, true);
        }
    }

    ImGui::EndChild(); /* ##MapPreview */

    return changed;
}

bool mapChooserConsumeFolderJump(MapChooserState *state,
                                  char *outPath, size_t outPathSz) {
    if (!state || state->pendingJumpPath[0] == '\0') return false;
    if (outPath && outPathSz > 0) {
        SDL_strlcpy(outPath, state->pendingJumpPath, outPathSz);
    }
    state->pendingJumpPath[0] = '\0';
    return true;
}

void mapChooserSetSelectedFile(MapChooserState *state,
                                SDL_Renderer *renderer,
                                const char *path,
                                const char *displayName) {
    if (!state || !path) return;
    SDL_strlcpy(state->selectedPath, path, sizeof(state->selectedPath));
    if (displayName) {
        SDL_strlcpy(state->selectedName, displayName,
                    sizeof(state->selectedName));
    }
    /* Selecting a "custom" file the chooser didn't list itself.
     * selectedIdx = -1 is the existing convention for that. */
    state->selectedIdx = -1;
    updatePreview(state, renderer);
}

void mapChooserSetSelectedMapBytes(MapChooserState *state,
                                   SDL_Renderer *renderer,
                                   const BYTE *bytes, int len,
                                   const char *displayName) {
    if (!state || !bytes || len <= 0) return;

    /* The bytes are an on-disk .map file image (e.g. a WBN download
     * held in RAM). Convert to the runtime compressed format in
     * memory so the chooser's texture, interactive widget and the
     * click-to-enlarge popup — all of which speak compressed — work
     * with no temp file on disk. */
    int compLen = 0;
    BYTE *comp = clientMapConvertFileToCompressed(bytes, len, &compLen);
    if (!comp || compLen <= 0) { free(comp); return; }

    mapPreviewPopupClose();
    state->randomMapSelected = false;

    /* Replace the stashed compressed map (popup / keep-camera read
     * this). Copy into an SDL-managed buffer so the rest of the
     * chooser's SDL_free path stays consistent. */
    if (state->compressedData) {
        SDL_free(state->compressedData);
        state->compressedData = NULL;
        state->compressedLen = 0;
    }
    state->compressedData = (BYTE *)SDL_malloc(compLen);
    if (state->compressedData) {
        SDL_memcpy(state->compressedData, comp, compLen);
        state->compressedLen = compLen;
    }
    free(comp);
    if (!state->compressedData) return;

    /* Static preview texture + stats, straight from the compressed
     * map — mirrors generateRandomPreview's population. */
    if (state->previewTex) {
        SDL_DestroyTexture(state->previewTex);
        state->previewTex = NULL;
    }
    MinimapBounds mb = {0, 0, 0, 0};
    MapPreview *mp = clientMapPreviewLoadFromBuffer(state->compressedData,
                                                    state->compressedLen);
    if (mp) {
        state->previewTex = minimapCreateTexture(renderer, mp, &mb, 0);
        state->previewPills  = clientMapPreviewGetLivePillCount(mp);
        state->previewBases  = clientMapPreviewGetLiveBaseCount(mp);
        state->previewStarts = clientMapPreviewGetLiveStartCount(mp);
        clientMapPreviewDestroy(mp);
    }
    state->previewBoundsMinX = mb.minX;
    state->previewBoundsMinY = mb.minY;
    state->previewBoundsMaxX = mb.maxX;
    state->previewBoundsMaxY = mb.maxY;

    /* Mirror into the interactive widget. */
    if (state->previewView) {
        mapPreviewViewLoadCompressed(state->previewView,
                                     state->compressedData,
                                     state->compressedLen);
        mapPreviewViewSetInitialBounds(state->previewView,
            mb.minX, mb.minY, mb.maxX, mb.maxY);
    }

    /* Selection bookkeeping — mirrors the previous post-download
     * file flow (selectedIdx = -1, name set). selectedPath is a
     * synthetic marker so the "wbn:" loading spinner stops and no
     * code mistakes it for a real on-disk file. */
    state->selectedIdx = -1;
    if (displayName) {
        SDL_strlcpy(state->selectedName, displayName,
                    sizeof(state->selectedName));
    }
    SDL_snprintf(state->selectedPath, FILENAME_MAX, "wbnmem:%s",
                 displayName ? displayName : "");
}

void mapChooserDestroy(MapChooserState *state) {
    if (state->previewTex) {
        SDL_DestroyTexture(state->previewTex);
        state->previewTex = NULL;
    }
    if (state->compressedData) {
        SDL_free(state->compressedData);
        state->compressedData = NULL;
        state->compressedLen = 0;
    }
    if (state->previewView) {
        mapPreviewViewDestroy(state->previewView);
        state->previewView = NULL;
    }
    state->initialized = false;
}
