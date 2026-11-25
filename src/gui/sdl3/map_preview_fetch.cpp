/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          map_preview_fetch.cpp
 * Purpose:       Async, session-cached map-preview fetch.
 *                A bounded pool of detached workers drains a
 *                queue of md5 requests off the frame thread.
 *                Each one looks first for a .map file on this
 *                machine with that md5 (local_map_index.h),
 *                then calls the blocking wbnMapFetchByMd5; the
 *                bytes are copied into a session cache keyed
 *                by md5. Requests dedupe per md5. A lookup
 *                WinBolo.net never answered is asked again
 *                after a wait; "not stored" is final.
 *********************************************************/

#include "map_preview_fetch.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstring>

#include <SDL3/SDL.h>

#include "local_map_index.h"
#include "map_preview_fetch_policy.h"

extern "C" {
#include "../../winbolonet/wbn_map.h"
}

namespace {

/* Cache entry: Ready, Unavailable, or Pending while in flight. Ready is
 * final. An Unavailable entry is looked up again when
 * map_preview_fetch_policy.h says so: after a wait when the request to
 * WinBolo.net failed on the way, never when WinBolo.net answered that it
 * does not store the map.
 * Entries are never erased, so a Ready entry's bytes.data() stays valid
 * for the process lifetime (unordered_map keeps element addresses stable
 * across inserts). */
struct Entry {
    MapPreviewFetchState state;
    std::vector<uint8_t> bytes;
    MapPreviewRetry      retry = { false, 0 };
};

/* All mutable pool state lives in one heap instance reached via pool().
 * The detached workers outlive normal static destruction, so the cv/mutex/
 * containers must never have their destructors run at process exit. */
struct PreviewPool {
    std::mutex                              cacheMtx;
    std::unordered_map<std::string, Entry>  cache;
    std::mutex                              queueMtx;
    std::condition_variable                 queueCv;
    std::deque<std::string>                 queue;
    /* The .map files on this machine, by md5. Both workers use it, one at
     * a time, under localMtx. */
    std::mutex                              localMtx;
    LocalMapIndex                          *local = nullptr;
};

static PreviewPool &pool() {
    static PreviewPool *p = new PreviewPool();  /* intentionally leaked:
        detached workers outlive normal static destruction, so never run
        these destructors — avoids a hang at process exit. */
    return *p;
}

/* On-select preview fetches are far rarer than the server-list pings, so a
 * small pool is ample. */
static constexpr int            kPreviewPoolSize = 2;
std::once_flag                  s_poolOnce;

/* Looks for a .map file on this machine whose md5 is md5Hex and copies its
 * bytes into `out`. The folders are the ones the map chooser lists: the
 * maps the game ships with (subfolders included), the player's own saved
 * maps, and the subscribed Workshop items. They are scanned again on every
 * call, which reads only the files that are new or changed since the last
 * one, so a map downloaded or saved during the session is found. */
bool loadLocalMap(const std::string &md5Hex, std::vector<uint8_t> &out) {
    std::lock_guard<std::mutex> lk(pool().localMtx);
    if (pool().local == nullptr) {
        pool().local = localMapIndexCreate();
        if (pool().local == nullptr) {
            return false;
        }
    }

    std::string prefMaps;
    std::string prefWorkshop;
    char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir != nullptr) {
        prefMaps = std::string(prefDir) + "maps";
        prefWorkshop = std::string(prefDir) + "Workshop";
        SDL_free(prefDir);
    }
    const char *roots[3];
    int rootCount = 0;
    roots[rootCount++] = "data/maps";
    if (!prefMaps.empty()) {
        roots[rootCount++] = prefMaps.c_str();
        roots[rootCount++] = prefWorkshop.c_str();
    }
    localMapIndexScan(pool().local, roots, rootCount);

    uint8_t *bytes = nullptr;
    size_t len = 0;
    if (!localMapIndexLoad(pool().local, md5Hex.c_str(), &bytes, &len)) {
        return false;
    }
    out.assign(bytes, bytes + len);
    SDL_free(bytes);
    return true;
}

void previewWorkerFn() {
    for (;;) {
        std::string md5;
        {
            std::unique_lock<std::mutex> lk(pool().queueMtx);
            pool().queueCv.wait(lk, [] { return !pool().queue.empty(); });
            md5 = pool().queue.front();
            pool().queue.pop_front();
        }

        /* This machine's own copy first: it needs no connection, and it is
         * the only source for a map WinBolo.net does not store under this
         * md5. */
        std::vector<uint8_t> localBytes;
        if (loadLocalMap(md5, localBytes)) {
            std::lock_guard<std::mutex> lk(pool().cacheMtx);
            Entry &e = pool().cache[md5];
            e.state = MapPreviewFetchState::Ready;
            e.bytes = std::move(localBytes);
            continue;
        }

        WbnMapResult r;
        bool ok = wbnMapFetchByMd5(md5.c_str(), &r);

        {
            std::lock_guard<std::mutex> lk(pool().cacheMtx);
            Entry &e = pool().cache[md5];
            if (ok) {
                e.state = MapPreviewFetchState::Ready;
                e.bytes.assign(r.mapData, r.mapData + r.mapDataLen);
            } else {
                e.state = MapPreviewFetchState::Unavailable;
                mapPreviewRetryNoteFailed(&e.retry, r.answered,
                                          SDL_GetTicks());
            }
        }

        wbnMapResultFree(&r);
    }
}

} // namespace

void mapPreviewFetchRequest(const char *md5Hex) {
    if (md5Hex == nullptr || std::strlen(md5Hex) != 32) {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(pool().cacheMtx);
        auto it = pool().cache.find(md5Hex);
        if (it != pool().cache.end()) {
            Entry &e = it->second;
            if (e.state != MapPreviewFetchState::Unavailable ||
                !mapPreviewRetryDue(&e.retry, SDL_GetTicks())) {
                return;  /* already pending or cached — dedupe */
            }
            /* The last lookup never reached an answer and the wait is
             * over: ask again. */
            e.state = MapPreviewFetchState::Pending;
            e.retry.retry = false;
        } else {
            Entry e;
            e.state = MapPreviewFetchState::Pending;
            pool().cache.emplace(md5Hex, std::move(e));
        }
    }

    std::call_once(s_poolOnce, [] {
        for (int i = 0; i < kPreviewPoolSize; i++) {
            std::thread(previewWorkerFn).detach();
        }
    });

    {
        std::lock_guard<std::mutex> lk(pool().queueMtx);
        pool().queue.emplace_back(md5Hex);
    }
    pool().queueCv.notify_one();
}

MapPreviewFetchState mapPreviewFetchTryGet(const char *md5Hex,
                                           const uint8_t **bytesOut,
                                           size_t *lenOut) {
    if (bytesOut != nullptr) {
        *bytesOut = nullptr;
    }
    if (lenOut != nullptr) {
        *lenOut = 0;
    }

    if (md5Hex == nullptr || std::strlen(md5Hex) != 32) {
        return MapPreviewFetchState::Unavailable;
    }

    std::lock_guard<std::mutex> lk(pool().cacheMtx);
    auto it = pool().cache.find(md5Hex);
    if (it == pool().cache.end()) {
        return MapPreviewFetchState::Unavailable;
    }
    if (it->second.state == MapPreviewFetchState::Ready) {
        if (bytesOut != nullptr) {
            *bytesOut = it->second.bytes.data();
        }
        if (lenOut != nullptr) {
            *lenOut = it->second.bytes.size();
        }
        return MapPreviewFetchState::Ready;
    }
    return it->second.state;
}
