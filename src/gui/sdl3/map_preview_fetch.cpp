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
 *                queue of md5 requests, calling the blocking
 *                wbnMapFetchByMd5 off the frame thread; the
 *                decoded bytes are copied into a session
 *                cache keyed by md5. Requests dedupe per md5.
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

extern "C" {
#include "../../winbolonet/wbn_map.h"
}

namespace {

/* Cache entry: terminal Ready/Unavailable, or Pending while in flight.
 * Entries are never erased, so a Ready entry's bytes.data() stays valid
 * for the process lifetime (unordered_map keeps element addresses stable
 * across inserts). */
struct Entry {
    MapPreviewFetchState state;
    std::vector<uint8_t> bytes;
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

void previewWorkerFn() {
    for (;;) {
        std::string md5;
        {
            std::unique_lock<std::mutex> lk(pool().queueMtx);
            pool().queueCv.wait(lk, [] { return !pool().queue.empty(); });
            md5 = pool().queue.front();
            pool().queue.pop_front();
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
        if (pool().cache.find(md5Hex) != pool().cache.end()) {
            return;  /* already pending or cached — dedupe */
        }
        pool().cache.emplace(md5Hex, Entry{MapPreviewFetchState::Pending, {}});
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
