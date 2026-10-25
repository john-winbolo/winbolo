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
 *  - Total cache cap: 64 MB of decoded images, kept by
 *    NewsImageBudget, which never evicts an image drawn in this
 *    frame or the previous one (see news_image_budget.h). The news
 *    window only asks for the images in view, so an image scrolled
 *    out of view ages out and the cap is a least-recently-viewed
 *    cache. An image that cannot fit without evicting one in view
 *    is Deferred: its pixels are dropped and it is downloaded again
 *    the next time it is in view and the budget has room for it.
 *    Pending and Failed entries are bounded (at most one Pending per
 *    URL; Failed entries persist permanently so we don't re-fetch
 *    known-bad URLs in the same process).
 *  - Threading: each first-time validated URL spawns a std::thread,
 *    joined by the UI thread once it has finished, or at shutdown.
 *    The worker runs a plain libcurl GET into a DynBuf, decodes
 *    via stbi_load_from_memory(..., 4), and hands the RGBA buffer
 *    + dims to the UI thread through a producer queue inside the
 *    cache. The UI thread drains the queue on every newsImageGet
 *    call, converting RGBA into SDL_Textures using the renderer
 *    from sdl3DrawGetRenderer().
 *********************************************************/

#include <SDL3/SDL.h>
#if defined(WBN_HTTP_NSURL) || defined(WBN_HTTP_JNI)
/* No libcurl on iOS or Android: the fetch goes through the platform's HTTP
 * module. */
extern "C" {
#include "../../winbolonet/http.h"
}
#else
#include <curl/curl.h>
#endif

#include "stb_image.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include "sdl3draw.h"
}

#include "news_image_budget.h"
#include "news_image_cache.h"

/*********************************************************
 *  Configuration
 *********************************************************/
namespace {

constexpr size_t kMaxDownloadBytes = 2 * 1024 * 1024;  /* 2 MB */
constexpr int    kMaxDecodedSide   = 1024;
constexpr size_t kMaxReadyBytes    = 64 * 1024 * 1024;  /* decoded RGBA */
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
    Ready,      /* texture uploaded, held in g_budget                  */
    Deferred,   /* decoded but refused for room; no worker, no pixels  */
    Failed,     /* rejected URL or transport/decode error — permanent  */
};

struct Entry {
    State        state         = State::Pending;
    SDL_Texture *texture       = nullptr;
    int          w             = 0;   /* texture size, when Ready */
    int          h             = 0;

    /* Worker → UI handoff: when set, the UI thread is responsible
     * for SDL_CreateTexture+SDL_UpdateTexture and freeing pixels. */
    unsigned char *pendingPixels = nullptr;
    int            pendingW      = 0;
    int            pendingH      = 0;

    /* Decoded size of a Deferred image, so the budget can be asked
     * whether it would fit before it is downloaded again. */
    size_t         deferredBytes = 0;
};

/* A download thread and the flag it sets as its last act, so the UI
 * thread can join it as soon as it has finished. */
struct Worker {
    std::thread                        thread;
    std::shared_ptr<std::atomic<bool>> done;
};

std::mutex                                       g_mutex;
std::unordered_map<std::string, Entry>           g_cache;
NewsImageBudget                                  g_budget(kMaxReadyBytes);
std::vector<Worker>                              g_workers;    /* UI thread only */
std::atomic<bool>                                g_shutdown{false};
unsigned                                         g_failedCount = 0; /* under g_mutex */

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

#if !defined(WBN_HTTP_NSURL) && !defined(WBN_HTTP_JNI)
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
#endif

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

#if defined(WBN_HTTP_NSURL) || defined(WBN_HTTP_JNI)
    if (!g_shutdown.load(std::memory_order_acquire)) {
        uint8_t *data = nullptr;
        size_t   size = 0;
        httpCode = httpGetUrlToMemory(url.c_str(), &data, &size,
                                      kMaxDownloadBytes,
                                      kHttpTimeoutSec * 1000L);
        if (httpCode == 200 && data != nullptr && size > 0) {
            buf.data     = data;
            buf.size     = size;
            buf.capacity = size;
            success = true;
        } else {
            std::free(data);
        }
    }
#else
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
#endif

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
        g_failedCount++;
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

/* Destroys the texture of an image the budget evicted and drops its
 * entry, so a later sighting downloads it again. The budget only
 * evicts images not drawn in this frame or the previous one. */
void evict_locked(const std::string &url) {
    auto it = g_cache.find(url);
    if (it == g_cache.end()) return;
    if (it->second.texture) {
        SDL_DestroyTexture(it->second.texture);
        it->second.texture = nullptr;
    }
    g_cache.erase(it);
}

void drainPending_locked() {
    /* Walk the cache, finalising any entries the worker has
     * deposited pixels into. We hold g_mutex throughout — the SDL
     * calls themselves are not under contention with the worker
     * since the worker only touches pendingPixels/W/H. */
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    std::vector<std::string> evicted;
    for (auto &kv : g_cache) {
        Entry &e = kv.second;
        if (e.state != State::Pending || !e.pendingPixels) continue;

        /* Make room first. The evicted entries are all Ready, so
         * erasing them never invalidates kv, the Pending entry this
         * loop is on. No room means the images in view fill the
         * budget; drop the pixels and try again when it is in view
         * and the budget can take it (see newsImageGet). */
        const size_t bytes = (size_t)e.pendingW * (size_t)e.pendingH * 4;
        if (!g_budget.admit(kv.first, bytes, evicted)) {
            stbi_image_free(e.pendingPixels);
            e.pendingPixels = nullptr;
            e.state         = State::Deferred;
            e.deferredBytes = bytes;
            continue;
        }
        for (const std::string &url : evicted) evict_locked(url);

        SDL_Texture *tex = nullptr;
        if (renderer) {
            /* stb_image's R, G, B, A bytes, on any host */
            tex = SDL_CreateTexture(renderer,
                                    SDL_PIXELFORMAT_RGBA32,
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
            g_budget.forget(kv.first);
            e.state = State::Failed;
            g_failedCount++;
            continue;
        }

        e.texture = tex;
        e.w       = e.pendingW;
        e.h       = e.pendingH;
        e.state   = State::Ready;
    }
}

/* Joins the download threads that have finished. UI thread only. */
void reapWorkers() {
    for (auto it = g_workers.begin(); it != g_workers.end();) {
        if (it->done->load(std::memory_order_acquire)) {
            it->thread.join();
            it = g_workers.erase(it);
        } else {
            ++it;
        }
    }
}

/* Starts the download of key, whose entry is already Pending. Called
 * without g_mutex held; the worker captures the URL string by value
 * so map-rehash invalidation doesn't bite. UI thread only. */
void startWorker(const std::string &key) {
    reapWorkers();
    if (g_shutdown.load(std::memory_order_acquire)) return;
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t([key, done]() {
        workerBody(key);
        done->store(true, std::memory_order_release);
    });
    g_workers.push_back(Worker{std::move(t), done});
}

}  /* namespace */

/*********************************************************
 *  Public API
 *********************************************************/

SDL_Texture *newsImageGet(const char *url) {
    if (!url) return nullptr;

    std::unique_lock<std::mutex> lk(g_mutex);
    const std::string key(url);
    auto it = g_cache.find(key);

    /* A Ready image is touched before the drain below, so an arrival
     * that needs room cannot take this image, which is about to be
     * drawn. Draining only erases other, evictable entries, so `it`
     * stays valid. */
    if (it != g_cache.end() && it->second.state == State::Ready &&
        it->second.texture) {
        g_budget.touch(key);
        drainPending_locked();
        return it->second.texture;
    }

    drainPending_locked();
    if (it != g_cache.end()) {
        Entry &e = it->second;
        if (e.state == State::Ready && e.texture) {
            /* Arrived in that drain, which admitted it as drawn. */
            return e.texture;
        }
        if (e.state == State::Deferred &&
            g_budget.fits(key, e.deferredBytes)) {
            e.state = State::Pending;
            lk.unlock();
            startWorker(key);
        }
        return nullptr;  /* Pending / Deferred / Failed */
    }

    /* First sighting. Validate before any network. */
    if (!validateUrl(url)) {
        Entry e;
        e.state = State::Failed;
        g_cache.emplace(key, e);
        g_failedCount++;
        return nullptr;
    }

    Entry e;
    e.state = State::Pending;
    g_cache.emplace(key, e);
    lk.unlock();

    startWorker(key);
    return nullptr;
}

bool newsImagePeek(const char *url, SDL_Texture **tex, float *w, float *h) {
    if (!url) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_cache.find(url);
    if (it == g_cache.end() || it->second.state != State::Ready ||
        !it->second.texture) {
        return false;
    }
    if (tex) *tex = it->second.texture;
    if (w) *w = (float)it->second.w;
    if (h) *h = (float)it->second.h;
    return true;
}

bool newsImageIsFailed(const char *url) {
    if (!url) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_cache.find(url);
    return it != g_cache.end() && it->second.state == State::Failed;
}

unsigned newsImageFailedCount(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_failedCount;
}

void newsImageCacheNewFrame(void) {
    reapWorkers();
    std::lock_guard<std::mutex> lk(g_mutex);
    g_budget.newFrame();
}

void newsImageCacheShutdown(void) {
    g_shutdown.store(true, std::memory_order_release);

    /* Join workers without holding g_mutex — workers grab it to
     * deposit results. They observe g_shutdown and bail quickly. */
    std::vector<Worker> workers;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        workers.swap(g_workers);
    }
    for (auto &w : workers) {
        if (w.thread.joinable()) w.thread.join();
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
    g_budget.clear();
    g_failedCount = 0;

    /* g_shutdown stays true after the join. A second
     * newsImageCacheShutdown() call is therefore a no-op, and any
     * stray callers of newsImageGet() after teardown still take the
     * shutdown-guarded path that suppresses worker spawning. */
}
