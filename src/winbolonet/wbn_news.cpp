/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          wbn_news
 * Filename:      wbn_news.cpp
 * Purpose:
 *   Async fetch of /api/v1/news, parsed via the pure-C++
 *   parseNewsResponse helper. Worker thread mirrors the
 *   detached pattern used by wbn_comments.cpp.
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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct WbnNewsFetch {
    std::atomic<bool>        done{false};
    mutable std::mutex       mtx;
    std::vector<WbnNewsItem> items;
    std::string              countryCode = "XX";
    int                      httpStatus  = -1;
};

extern "C" WbnNewsFetch *wbn_news_fetch_start(void) {
    auto *f = new WbnNewsFetch();

    std::thread([f]() {
        char *response = nullptr;
        int   status   = wbn_api_get("news", &response);

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
            std::lock_guard<std::mutex> lock(f->mtx);
            f->items       = std::move(parsedItems);
            f->countryCode = std::move(parsedCountry);
            f->httpStatus  = status;
        }
        f->done.store(true);
    }).detach();

    return f;
}

extern "C" bool wbn_news_fetch_done(const WbnNewsFetch *f) {
    return f && f->done.load();
}

extern "C" int wbn_news_fetch_result(WbnNewsFetch *f,
                                     const WbnNewsItem **out_items,
                                     size_t *out_count) {
    if (!f) {
        if (out_items) *out_items = nullptr;
        if (out_count) *out_count = 0;
        return -1;
    }
    std::lock_guard<std::mutex> lock(f->mtx);
    if (out_items) *out_items = f->items.empty() ? nullptr : f->items.data();
    if (out_count) *out_count = f->items.size();
    return f->httpStatus;
}

extern "C" const char *wbn_news_fetch_country_code(const WbnNewsFetch *f) {
    if (!f) return "XX";
    std::lock_guard<std::mutex> lock(f->mtx);
    return f->countryCode.c_str();
}

extern "C" void wbn_news_fetch_free(WbnNewsFetch *f) {
    if (!f) return;
    {
        std::lock_guard<std::mutex> lock(f->mtx);
        for (auto &it : f->items) {
            if (it.body_md) {
                free(it.body_md);
                it.body_md = nullptr;
            }
        }
        f->items.clear();
    }
    delete f;
}
