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

#include <functional>
#include <string>
#include <vector>

#include "wbn_news.h"

bool parseNewsResponse(const char *raw,
                       std::vector<WbnNewsItem> &outItems,
                       std::string &outCountryCode);

/* Longest image or link URL newsUnwrapLinkedImages accepts, in bytes.
 * imgui_news.cpp copies a URL into a 2048-byte buffer before using it. */
#define WBN_NEWS_MD_MAX_URL 2047
/* Longest alt text it accepts, in bytes. */
#define WBN_NEWS_MD_MAX_ALT 1024

/* The full-size picture a thumbnail in a news body linked to. */
struct WbnNewsImageLink {
    std::string image; /* the thumbnail URL, as it appears in the body */
    std::string href;  /* the URL the thumbnail linked to; http(s) only */
};

/* imgui_markdown cannot draw an image inside a link, which is how the
 * website writes a thumbnail that opens the full-size picture:
 *
 *   [![alt](thumb.png)](full.png)
 *
 * It draws "![alt" as link text and the rest as plain text instead.
 * Returns md with each such construct replaced by the plain image
 * "![alt](thumb.png)", and appends {thumb.png, full.png} to outLinks so
 * the caller can still open full.png when the image is clicked.
 *
 * A construct is only rewritten when both URLs are 1 to
 * WBN_NEWS_MD_MAX_URL bytes with no whitespace, control characters,
 * '(', ')', '[', ']', '<' or '>', and the alt text is at most
 * WBN_NEWS_MD_MAX_ALT bytes with no '[' or ']'. Anything else is
 * copied unchanged. The outer link is only recorded in outLinks when
 * it starts with http:// or https://; otherwise the image is unwrapped
 * and the link dropped. The image URL is not checked here:
 * news_image_cache applies its own host allowlist. */
std::string newsUnwrapLinkedImages(const std::string &md,
                                   std::vector<WbnNewsImageLink> &outLinks);

/* Replaces each image "![alt](image)" in md for which failed(image) is
 * true with a link "[alt](target)", so an image that cannot be shown
 * can still be opened in the browser. target is the href recorded in
 * links for that image, or the image URL itself; the link text is the
 * alt text, or the image URL when there is none. Images are read by
 * the same rules as newsUnwrapLinkedImages, and anything else is
 * copied unchanged. Clicks still go through the caller's own scheme
 * check. */
std::string newsLinkFailedImages(
    const std::string &md, const std::vector<WbnNewsImageLink> &links,
    const std::function<bool(const std::string &)> &failed);

/* Returns the href recorded for the image URL image[0..imageLen), or
 * nullptr if that image had no outer link. The first entry wins when
 * the same thumbnail was linked twice. */
const char *newsFindImageLink(const std::vector<WbnNewsImageLink> &links,
                              const char *image, size_t imageLen);

#endif /* WBN_NEWS_PARSER_H */
