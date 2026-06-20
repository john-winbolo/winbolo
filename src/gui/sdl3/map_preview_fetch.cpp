/*
 * Copyright (c) 1998-2008 John Morrison.
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

std::mutex                              s_cacheMtx;
std::unordered_map<std::string, Entry>  s_cache;

/* On-select preview fetches are far rarer than the server-list pings, so a
 * small pool is ample. */
static constexpr int            kPreviewPoolSize = 2;
std::mutex                      s_queueMtx;
std::condition_variable         s_queueCv;
std::deque<std::string>         s_queue;
std::once_flag                  s_poolOnce;

void previewWorkerFn() {
    for (;;) {
        std::string md5;
        {
            std::unique_lock<std::mutex> lk(s_queueMtx);
            s_queueCv.wait(lk, [] { return !s_queue.empty(); });
            md5 = s_queue.front();
            s_queue.pop_front();
        }

        WbnMapResult r;
        bool ok = wbnMapFetchByMd5(md5.c_str(), &r);

        {
            std::lock_guard<std::mutex> lk(s_cacheMtx);
            Entry &e = s_cache[md5];
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
        std::lock_guard<std::mutex> lk(s_cacheMtx);
        if (s_cache.find(md5Hex) != s_cache.end()) {
            return;  /* already pending or cached — dedupe */
        }
        s_cache.emplace(md5Hex, Entry{MapPreviewFetchState::Pending, {}});
    }

    std::call_once(s_poolOnce, [] {
        for (int i = 0; i < kPreviewPoolSize; i++) {
            std::thread(previewWorkerFn).detach();
        }
    });

    {
        std::lock_guard<std::mutex> lk(s_queueMtx);
        s_queue.emplace_back(md5Hex);
    }
    s_queueCv.notify_one();
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

    std::lock_guard<std::mutex> lk(s_cacheMtx);
    auto it = s_cache.find(md5Hex);
    if (it == s_cache.end()) {
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
