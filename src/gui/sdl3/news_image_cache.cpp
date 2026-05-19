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
 * Name:    news_image_cache
 * Purpose: Texture cache for inline images referenced from
 *          markdown blobs rendered in the news/welcome modal.
 *
 *  - URL allowlist: scheme http/https, host in the four-entry
 *    WBN list below (exact match, case-insensitive). Anything
 *    else is recorded as permanently failed before any network
 *    activity — blocks passive tracking via attacker-hosted
 *    image embeds.
 *  - Per-download size cap: 2 MB (enforced by libcurl's
 *    CURLOPT_MAXFILESIZE and again in the write callback).
 *  - Per-image decoded-dimension cap: 1024 px on either side.
 *    Oversized images are rejected (no downscaling — stb's
 *    image_resize header is not vendored).
 *  - Total cache cap: 16 Ready entries with LRU eviction. Pending
 *    and Failed entries are bounded (at most one Pending per URL;
 *    Failed entries persist permanently so we don't re-fetch known-
 *    bad URLs in the same process).
 *  - Threading: each first-time validated URL spawns a joined
 *    std::thread (collected in a vector and joined at shutdown).
 *    The worker runs a plain libcurl GET into a DynBuf, decodes
 *    via stbi_load_from_memory(..., 4), and hands the RGBA buffer
 *    + dims to the UI thread through a producer queue inside the
 *    cache. The UI thread drains the queue on every newsImageGet
 *    call, converting RGBA into SDL_Textures using the renderer
 *    from sdl3DrawGetRenderer().
 *********************************************************/

#include <SDL3/SDL.h>
#include <curl/curl.h>

#include "stb_image.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include "sdl3draw.h"
}

#include "news_image_cache.h"

/*********************************************************
 *  Configuration
 *********************************************************/
namespace {

constexpr size_t kMaxDownloadBytes = 2 * 1024 * 1024;  /* 2 MB */
constexpr int    kMaxDecodedSide   = 1024;
constexpr size_t kMaxReadyEntries  = 16;
constexpr long   kHttpTimeoutSec   = 30L;

/* WBN host allowlist — case-insensitive exact match, no subdomain
 * wildcarding. News authors upload images to one of these hosts. */
const char *const kAllowedHosts[] = {
    "www.winbolo.com",
    "winbolo.com",
    "www.winbolo.net",
    "winbolo.net",
};

enum class State : uint8_t {
    Pending,    /* worker running, no texture yet                      */
    Ready,      /* texture uploaded, eligible for LRU                  */
    Failed,     /* rejected URL or transport/decode error — permanent  */
};

struct Entry {
    State        state         = State::Pending;
    SDL_Texture *texture       = nullptr;

    /* Worker → UI handoff: when set, the UI thread is responsible
     * for SDL_CreateTexture+SDL_UpdateTexture and freeing pixels. */
    unsigned char *pendingPixels = nullptr;
    int            pendingW      = 0;
    int            pendingH      = 0;

    /* LRU iterator pointing at this entry's node in g_lru. Only
     * meaningful when state == Ready. */
    std::list<std::string>::iterator lruIt;
};

std::mutex                                       g_mutex;
std::unordered_map<std::string, Entry>           g_cache;
std::list<std::string>                           g_lru;        /* MRU front, LRU back */
std::vector<std::thread>                         g_workers;
std::atomic<bool>                                g_shutdown{false};

/* Guarantees the worker vector is empty by the time its static
 * destructor runs. Without this, a process that never explicitly
 * calls newsImageCacheShutdown() reaches ~vector<thread> with
 * joinable threads inside it, which terminates the process via
 * std::terminate from ~thread. Declared last in this namespace so
 * it's destroyed first — the mutex/cache/worker vector are still
 * alive when its destructor runs and forwards to the public
 * shutdown routine. */
struct ShutdownGuard {
    ~ShutdownGuard() { newsImageCacheShutdown(); }
};
ShutdownGuard g_shutdown_guard;

/*********************************************************
 *  URL parsing + host allowlist
 *********************************************************/

bool asciiEqualIgnoreCase(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

/* Extract the host substring out of url, after the scheme://. Stops
 * at the first /, ?, #, or :. Returns true and fills `host` on
 * success; returns false if the URL has no recognisable host. */
bool extractHost(const char *afterScheme, std::string &host) {
    const char *p = afterScheme;
    const char *start = p;
    while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ':') ++p;
    if (p == start) return false;
    host.assign(start, (size_t)(p - start));
    return true;
}

/* Returns true if scheme is http/https AND host is in the allowlist. */
bool validateUrl(const char *url) {
    if (!url) return false;
    const char *afterScheme = nullptr;
    if (strncmp(url, "http://", 7) == 0) {
        afterScheme = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        afterScheme = url + 8;
    } else {
        return false;
    }
    std::string host;
    if (!extractHost(afterScheme, host)) return false;
    for (const char *allowed : kAllowedHosts) {
        if (asciiEqualIgnoreCase(host.c_str(), allowed)) return true;
    }
    return false;
}

/*********************************************************
 *  libcurl write callback (DynBuf with 2 MB hard cap)
 *********************************************************/

struct DynBuf {
    unsigned char *data     = nullptr;
    size_t         size     = 0;
    size_t         capacity = 0;
};

size_t dynWrite(char *ptr, size_t size, size_t nmemb, void *userdata) {
    DynBuf *buf = static_cast<DynBuf *>(userdata);
    size_t incoming = size * nmemb;
    /* Defensive check in addition to CURLOPT_MAXFILESIZE: some
     * transports (e.g. chunked encoding without Content-Length)
     * skip libcurl's pre-flight size check. */
    if (buf->size + incoming > kMaxDownloadBytes) return 0;
    size_t needed = buf->size + incoming;
    if (needed > buf->capacity) {
        size_t newcap = buf->capacity ? buf->capacity * 2 : 4096;
        while (newcap < needed) newcap *= 2;
        unsigned char *tmp = static_cast<unsigned char *>(
            std::realloc(buf->data, newcap));
        if (!tmp) return 0;
        buf->data     = tmp;
        buf->capacity = newcap;
    }
    std::memcpy(buf->data + buf->size, ptr, incoming);
    buf->size += incoming;
    return incoming;
}

/*********************************************************
 *  Worker thread body
 *
 *  Runs on a background std::thread. Performs the libcurl GET,
 *  decodes RGBA, and deposits the pixel buffer back into the
 *  Entry under g_mutex. The UI thread completes the texture
 *  upload on its next newsImageGet call.
 *********************************************************/

void workerBody(std::string url) {
    DynBuf buf;
    bool success = false;
    long httpCode = 0;

    if (!g_shutdown.load(std::memory_order_acquire)) {
        CURL *curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  dynWrite);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT,        kHttpTimeoutSec);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_MAXFILESIZE,
                             (long)kMaxDownloadBytes);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);

            CURLcode rc = curl_easy_perform(curl);
            if (rc == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE,
                                  &httpCode);
                if (httpCode == 200 && buf.size > 0) success = true;
            }
            curl_easy_cleanup(curl);
        }
    }

    unsigned char *pixels = nullptr;
    int w = 0, h = 0, channels = 0;
    if (success && !g_shutdown.load(std::memory_order_acquire)) {
        pixels = stbi_load_from_memory(buf.data, (int)buf.size,
                                        &w, &h, &channels, 4);
    }
    std::free(buf.data);

    if (pixels && (w > kMaxDecodedSide || h > kMaxDecodedSide)) {
        stbi_image_free(pixels);
        pixels = nullptr;
    }

    /* Deposit result. If the cache module is shutting down or the
     * entry has been evicted (it shouldn't have — Pending entries
     * are pinned — but be defensive), drop the pixels on the
     * floor. */
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_shutdown.load(std::memory_order_acquire)) {
        if (pixels) stbi_image_free(pixels);
        return;
    }
    auto it = g_cache.find(url);
    if (it == g_cache.end()) {
        if (pixels) stbi_image_free(pixels);
        return;
    }
    if (!pixels) {
        it->second.state = State::Failed;
        return;
    }
    it->second.pendingPixels = pixels;
    it->second.pendingW      = w;
    it->second.pendingH      = h;
    /* state stays Pending until the UI thread completes the upload */
}

/*********************************************************
 *  UI-thread helpers (caller already holds g_mutex)
 *********************************************************/

void evictOneLru_locked() {
    /* Pop the back of the LRU list — that's the oldest Ready entry. */
    if (g_lru.empty()) return;
    const std::string victimUrl = g_lru.back();
    g_lru.pop_back();
    auto it = g_cache.find(victimUrl);
    if (it == g_cache.end()) return;
    if (it->second.texture) {
        SDL_DestroyTexture(it->second.texture);
        it->second.texture = nullptr;
    }
    g_cache.erase(it);
}

size_t countReady_locked() { return g_lru.size(); }

void touchLru_locked(Entry &e, const std::string &url) {
    /* Move e's existing LRU node to the front. */
    g_lru.erase(e.lruIt);
    g_lru.push_front(url);
    e.lruIt = g_lru.begin();
}

void drainPending_locked() {
    /* Walk the cache, finalising any entries the worker has
     * deposited pixels into. We hold g_mutex throughout — the SDL
     * calls themselves are not under contention with the worker
     * since the worker only touches pendingPixels/W/H. */
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    for (auto &kv : g_cache) {
        Entry &e = kv.second;
        if (e.state != State::Pending || !e.pendingPixels) continue;

        SDL_Texture *tex = nullptr;
        if (renderer) {
            tex = SDL_CreateTexture(renderer,
                                    SDL_PIXELFORMAT_ABGR8888,
                                    SDL_TEXTUREACCESS_STATIC,
                                    e.pendingW, e.pendingH);
            if (tex) {
                SDL_UpdateTexture(tex, NULL, e.pendingPixels,
                                  e.pendingW * 4);
            }
        }
        stbi_image_free(e.pendingPixels);
        e.pendingPixels = nullptr;

        if (!tex) {
            e.state = State::Failed;
            continue;
        }

        /* Enforce the Ready cap before inserting the new LRU node. */
        while (countReady_locked() >= kMaxReadyEntries) {
            evictOneLru_locked();
            /* After eviction, kv may be invalidated if it was the
             * victim — but we just inserted to pendingPixels above,
             * so it can't be the LRU back unless we just made it Ready
             * (which we haven't yet). Safe. */
        }

        e.texture = tex;
        e.state   = State::Ready;
        g_lru.push_front(kv.first);
        e.lruIt   = g_lru.begin();
    }
}

}  /* namespace */

/*********************************************************
 *  Public API
 *********************************************************/

SDL_Texture *newsImageGet(const char *url) {
    if (!url) return nullptr;

    std::unique_lock<std::mutex> lk(g_mutex);
    drainPending_locked();

    const std::string key(url);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        Entry &e = it->second;
        if (e.state == State::Ready && e.texture) {
            touchLru_locked(e, key);
            return e.texture;
        }
        return nullptr;  /* Pending / Failed */
    }

    /* First sighting. Validate before any network. */
    if (!validateUrl(url)) {
        Entry e;
        e.state = State::Failed;
        g_cache.emplace(key, e);
        return nullptr;
    }

    /* Insert as Pending and spawn a worker. The worker captures the
     * URL string by value so map-rehash invalidation doesn't bite. */
    Entry e;
    e.state = State::Pending;
    g_cache.emplace(key, e);
    lk.unlock();

    if (!g_shutdown.load(std::memory_order_acquire)) {
        g_workers.emplace_back(workerBody, key);
    }
    return nullptr;
}

void newsImageCacheShutdown(void) {
    g_shutdown.store(true, std::memory_order_release);

    /* Join workers without holding g_mutex — workers grab it to
     * deposit results. They observe g_shutdown and bail quickly. */
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        workers.swap(g_workers);
    }
    for (auto &t : workers) {
        if (t.joinable()) t.join();
    }

    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto &kv : g_cache) {
        Entry &e = kv.second;
        if (e.texture) {
            SDL_DestroyTexture(e.texture);
            e.texture = nullptr;
        }
        if (e.pendingPixels) {
            stbi_image_free(e.pendingPixels);
            e.pendingPixels = nullptr;
        }
    }
    g_cache.clear();
    g_lru.clear();

    /* g_shutdown stays true after the join. A second
     * newsImageCacheShutdown() call is therefore a no-op, and any
     * stray callers of newsImageGet() after teardown still take the
     * shutdown-guarded path that suppresses worker spawning. */
}
