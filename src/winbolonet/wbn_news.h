/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          wbn_news
 * Filename:      wbn_news.h
 * Purpose:
 *   Async fetch of the WinBolo.net news feed.
 *   GETs /api/v1/news, parses the JSON object response
 *   (items + countryCode) and exposes the result through
 *   a small poll-based handle. Called from the SDL3 client
 *   welcome-screen news popup.
 *********************************************************/

#ifndef WBN_NEWS_H
#define WBN_NEWS_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   id;
    char  title[256];
    char *body_md;        /* heap-allocated; UTF-8 markdown; freed by wbn_news_fetch_free */
    int   comments;
    char  url[512];
    char  date[20];       /* "YYYY-MM-DD HH:MM:SS" UTC, or empty if the
                           * response omitted the field. */
} WbnNewsItem;

typedef struct WbnNewsFetch WbnNewsFetch;

/* Kicks off a background fetch. Caller owns the handle and must pass it
 * to wbn_news_fetch_free. */
WbnNewsFetch *wbn_news_fetch_start(void);

/* True once the worker thread has finished. Safe to call every frame. */
bool          wbn_news_fetch_done (const WbnNewsFetch *f);

/* Returns HTTP status (200 on success, -1 transport error, or the
 * server status code). On 200 with a well-formed response,
 * out_items / out_count are populated and remain owned by the fetch.
 * On any other status (or if the body fails to parse, or the top
 * level isn't an object, or `news` isn't an array), out_count is 0. */
int           wbn_news_fetch_result(WbnNewsFetch *f,
                                    const WbnNewsItem **out_items,
                                    size_t *out_count);

/* Country code from the most recent successful fetch — uppercase
 * 2-char ISO 3166-1 alpha-2. Returns "XX" if the fetch is not done,
 * failed, or the response had no usable countryCode. Valid until
 * wbn_news_fetch_free is called. */
const char   *wbn_news_fetch_country_code(const WbnNewsFetch *f);

void          wbn_news_fetch_free  (WbnNewsFetch *f);

#ifdef __cplusplus
}
#endif

#endif /* WBN_NEWS_H */
