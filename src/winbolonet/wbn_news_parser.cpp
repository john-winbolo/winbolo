/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          wbn_news_parser
 * Filename:      wbn_news_parser.cpp
 * Purpose:
 *   Pure cJSON parser for the WinBolo.net news response.
 *   Deliberately libcurl-free so the unit tests can link
 *   it without dragging http.c or its dependency closure.
 *********************************************************/

#include "wbn_news_parser.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>

extern "C" {
#include "cJSON.h"
}

namespace {

void clearItems(std::vector<WbnNewsItem> &items) {
    for (auto &it : items) {
        if (it.body_md) {
            free(it.body_md);
            it.body_md = nullptr;
        }
    }
    items.clear();
}

/* Reads `countryCode` from root. Returns "XX" if it's missing, not a
 * 2-char string, or has any non-alphabetic char. Uppercases otherwise. */
std::string readCountryCode(cJSON *root) {
    cJSON *cc = cJSON_GetObjectItem(root, "countryCode");
    if (!cc || !cJSON_IsString(cc) || !cc->valuestring) return "XX";
    const char *s = cc->valuestring;
    if (strlen(s) != 2) return "XX";
    unsigned char c0 = (unsigned char)s[0];
    unsigned char c1 = (unsigned char)s[1];
    if (!isalpha(c0) || !isalpha(c1)) return "XX";
    std::string out;
    out.push_back((char)toupper(c0));
    out.push_back((char)toupper(c1));
    return out;
}

bool readItem(cJSON *o, WbnNewsItem &out) {
    if (!o || !cJSON_IsObject(o)) return false;

    cJSON *idJ      = cJSON_GetObjectItem(o, "id");
    cJSON *titleJ   = cJSON_GetObjectItem(o, "title");
    cJSON *bodyJ    = cJSON_GetObjectItem(o, "body");
    cJSON *urlJ     = cJSON_GetObjectItem(o, "url");
    cJSON *commJ    = cJSON_GetObjectItem(o, "comments");
    cJSON *dateJ    = cJSON_GetObjectItem(o, "date");

    if (!idJ    || !cJSON_IsNumber(idJ))                          return false;
    if (!titleJ || !cJSON_IsString(titleJ) || !titleJ->valuestring) return false;
    if (!bodyJ  || !cJSON_IsString(bodyJ)  || !bodyJ->valuestring)  return false;
    if (!urlJ   || !cJSON_IsString(urlJ)   || !urlJ->valuestring)   return false;

    out = WbnNewsItem{};
    out.id = idJ->valueint;
    SDL_strlcpy(out.title, titleJ->valuestring, sizeof(out.title));
    SDL_strlcpy(out.url,   urlJ->valuestring,   sizeof(out.url));
    out.comments = (commJ && cJSON_IsNumber(commJ)) ? commJ->valueint : 0;
    /* date is optional. Missing / non-string / empty string leaves the
     * buffer NUL (already zeroed by the WbnNewsItem{} initialiser); the
     * renderer skips the date line in that case. */
    if (dateJ && cJSON_IsString(dateJ) && dateJ->valuestring) {
        SDL_strlcpy(out.date, dateJ->valuestring, sizeof(out.date));
    }

    /* body_md is malloc'd so the fetch struct and the unit tests can
     * release it with plain free(). */
    size_t bodyLen = strlen(bodyJ->valuestring);
    out.body_md = (char *)malloc(bodyLen + 1);
    if (!out.body_md) return false;
    memcpy(out.body_md, bodyJ->valuestring, bodyLen + 1);
    return true;
}

} /* anonymous namespace */

bool parseNewsResponse(const char *raw,
                       std::vector<WbnNewsItem> &outItems,
                       std::string &outCountryCode) {
    clearItems(outItems);
    outCountryCode = "XX";

    if (!raw) return false;

    cJSON *root = cJSON_Parse(raw);
    if (!root) return false;

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    /* Read countryCode first into a local; we only commit it to the out
     * param if the response also has a valid `news` array. */
    std::string country = readCountryCode(root);

    cJSON *news = cJSON_GetObjectItem(root, "news");
    if (!news || !cJSON_IsArray(news)) {
        cJSON_Delete(root);
        return false;
    }

    int n = cJSON_GetArraySize(news);
    outItems.reserve((size_t)n);
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_GetArrayItem(news, i);
        WbnNewsItem item{};
        if (readItem(o, item)) {
            outItems.push_back(item);
        }
    }

    outCountryCode = country;
    cJSON_Delete(root);
    return true;
}
