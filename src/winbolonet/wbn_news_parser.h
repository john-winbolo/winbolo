/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          wbn_news_parser
 * Filename:      wbn_news_parser.h
 * Purpose:
 *   Pure C++ JSON parser for the WinBolo.net /api/v1/news
 *   response. Extracted from wbn_news.cpp so the unit tests
 *   can link it without libcurl, http.c, or any threading
 *   primitives.
 *
 *   On success, populates outItems and outCountryCode from
 *   the JSON object. On any structural failure (parse error,
 *   non-object top level, missing/wrong-type `news` array)
 *   the function returns false, outItems is cleared, and
 *   outCountryCode is set to "XX".
 *
 *   Each WbnNewsItem.body_md returned in outItems is
 *   heap-allocated (strdup). The caller (production fetcher
 *   or test) is responsible for free()ing it.
 *********************************************************/

#ifndef WBN_NEWS_PARSER_H
#define WBN_NEWS_PARSER_H

#include <string>
#include <vector>

#include "wbn_news.h"

bool parseNewsResponse(const char *raw,
                       std::vector<WbnNewsItem> &outItems,
                       std::string &outCountryCode);

#endif /* WBN_NEWS_PARSER_H */
