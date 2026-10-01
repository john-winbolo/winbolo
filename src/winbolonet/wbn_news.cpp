/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          wbn_news
 * Filename:      wbn_news.cpp
 * Purpose:
 *   Async fetch of /api/v1/news, parsed via the pure-C++
 *   parseNewsResponse helper. Worker thread mirrors the
 *   detached pattern used by wbn_comments.cpp.
 *
 *   The fetch state is ref-counted (shared_ptr) between the
 *   caller-owned handle and the detached worker thread. The
 *   worker keeps its own reference, so freeing the handle
 *   while the network GET is still in flight only drops the
 *   caller's reference — the worker tears the state down when
 *   it lands. Without this, a free() that raced the worker's
 *   terminal lock destroyed the mutex out from under it, a
 *   use-after-free that crashed inside mtx_do_lock.
 *********************************************************/

#include "wbn_news.h"
#include "wbn_news_parser.h"

extern "C" {
#include <SDL3/SDL.h>
#include "http.h"
#include "../common/wb_log.h"
}

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* Shared, ref-counted state. Held by the caller-owned handle and by the
 * detached worker; whichever reference is dropped last destroys it. The
 * destructor frees any body_md the worker parsed, so a handle freed while the
 * worker is still in flight leaks nothing — the worker's landing owns the
 * teardown. */
struct WbnNewsState {
    std::atomic<bool>        done{false};
    mutable std::mutex       mtx;
    std::vector<WbnNewsItem> items;
    std::string              countryCode = "XX";
    int                      httpStatus  = -1;

    ~WbnNewsState() {
        for (auto &it : items) {
            if (it.body_md) {
                free(it.body_md);
                it.body_md = nullptr;
            }
        }
    }
};

/* Opaque handle the C caller owns. Holds one reference to the shared state;
 * wbn_news_fetch_free drops it. */
struct WbnNewsFetch {
    std::shared_ptr<WbnNewsState> state;
};

/* Test seam: the network GET the worker issues. Defaults to the real
 * winbolo.net transport; unit tests point it at a controllable stand-in via
 * wbn_news_set_api_get_for_test so they can drive the free()/worker
 * interleaving deterministically. Not part of the shipping API surface. */
namespace {
using WbnNewsApiGetFn = int (*)(const char *path, char **response_out);
WbnNewsApiGetFn g_apiGet = wbn_api_get;
}  // namespace

extern "C" void wbn_news_set_api_get_for_test(
    int (*fn)(const char *path, char **response_out)) {
    g_apiGet = fn ? fn : wbn_api_get;
}

extern "C" WbnNewsFetch *wbn_news_fetch_start(void) {
    auto *f  = new WbnNewsFetch();
    f->state = std::make_shared<WbnNewsState>();

    std::shared_ptr<WbnNewsState> state = f->state;  // worker's own reference
    std::thread([state]() {
        char *response = nullptr;
        int   status   = g_apiGet("news", &response);

        std::vector<WbnNewsItem> parsedItems;
        std::string              parsedCountry = "XX";

        if (status == 200 && response) {
            (void)parseNewsResponse(response, parsedItems, parsedCountry);
        } else if (status == 429) {
            /* Rate-limit hit. Retry-After header isn't exposed by
             * wbn_api_get today; log the status alone. */
            WB_LOG_WARN(WB_LOG_CAT_NET, "news: received HTTP 429 (rate limited)");
        } else if (status != 200) {
            WB_LOG_WARN(WB_LOG_CAT_NET, "news: HTTP %d", status);
        }

        free(response);

        {
            std::lock_guard<std::mutex> lock(state->mtx);
            state->items       = std::move(parsedItems);
            state->countryCode = std::move(parsedCountry);
            state->httpStatus  = status;
        }
        state->done.store(true);
    }).detach();

    return f;
}

extern "C" bool wbn_news_fetch_done(const WbnNewsFetch *f) {
    return f && f->state->done.load();
}

extern "C" int wbn_news_fetch_result(WbnNewsFetch *f,
                                     const WbnNewsItem **out_items,
                                     size_t *out_count) {
    if (!f) {
        if (out_items) *out_items = nullptr;
        if (out_count) *out_count = 0;
        return -1;
    }
    std::lock_guard<std::mutex> lock(f->state->mtx);
    if (out_items) {
        *out_items = f->state->items.empty() ? nullptr : f->state->items.data();
    }
    if (out_count) *out_count = f->state->items.size();
    return f->state->httpStatus;
}

extern "C" const char *wbn_news_fetch_country_code(const WbnNewsFetch *f) {
    if (!f) return "XX";
    std::lock_guard<std::mutex> lock(f->state->mtx);
    return f->state->countryCode.c_str();
}

extern "C" void wbn_news_fetch_free(WbnNewsFetch *f) {
    if (!f) return;
    /* Drop the caller's reference. If the worker is still in flight it holds
     * the last reference and tears the state down (freeing any body_md) when
     * it lands; otherwise the state is destroyed here. */
    delete f;
}
