/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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

/* True for a byte that may appear in a URL newsUnwrapLinkedImages
 * rewrites. Spaces and control characters end a Markdown URL, and round
 * brackets would confuse imgui_markdown's own bracket counting. Square
 * brackets are refused too so a failed match never reads past the next
 * '[', which keeps the whole pass linear in the body's length. */
bool urlByteOk(unsigned char c) {
    if (c <= 0x20 || c == 0x7f) return false;
    return c != '(' && c != ')' && c != '<' && c != '>' &&
           c != '[' && c != ']';
}

/* Reads a URL starting at md[pos] up to the closing ')'. On success
 * returns true, stores the URL's [start, end) and leaves pos on the ')'. */
bool readUrl(const std::string &md, size_t &pos, size_t &start, size_t &end) {
    start = pos;
    while (pos < md.size() && md[pos] != ')') {
        if (!urlByteOk((unsigned char)md[pos])) return false;
        if (pos - start >= WBN_NEWS_MD_MAX_URL) return false;
        pos++;
    }
    if (pos >= md.size() || pos == start) return false;
    end = pos;
    return true;
}

bool hasHttpScheme(const std::string &url) {
    return SDL_strncasecmp(url.c_str(), "http://", 7) == 0 ||
           SDL_strncasecmp(url.c_str(), "https://", 8) == 0;
}

/* Tries to read "[![alt](image)](href)" starting at md[pos], which is
 * known to hold "[![". On success returns true and sets the parts and
 * `next`, the index just past the final ')'. */
bool readLinkedImage(const std::string &md, size_t pos,
                     size_t &altStart, size_t &altEnd,
                     size_t &imgStart, size_t &imgEnd,
                     size_t &hrefStart, size_t &hrefEnd,
                     size_t &next) {
    size_t p = pos + 3;
    altStart = p;
    while (p < md.size() && md[p] != ']') {
        if (md[p] == '[') return false;
        if (p - altStart >= WBN_NEWS_MD_MAX_ALT) return false;
        p++;
    }
    if (p >= md.size()) return false;
    altEnd = p++;
    if (p >= md.size() || md[p] != '(') return false;
    p++;
    if (!readUrl(md, p, imgStart, imgEnd)) return false;
    p++; /* past ')' */
    if (p + 1 >= md.size() || md[p] != ']' || md[p + 1] != '(') return false;
    p += 2;
    if (!readUrl(md, p, hrefStart, hrefEnd)) return false;
    next = p + 1;
    return true;
}

} /* anonymous namespace */

std::string newsUnwrapLinkedImages(const std::string &md,
                                   std::vector<WbnNewsImageLink> &outLinks) {
    std::string out;
    out.reserve(md.size());
    size_t pos = 0;
    while (pos < md.size()) {
        size_t hit = md.find("[![", pos);
        if (hit == std::string::npos) {
            out.append(md, pos, std::string::npos);
            break;
        }
        out.append(md, pos, hit - pos);

        size_t altStart, altEnd, imgStart, imgEnd, hrefStart, hrefEnd, next;
        if (!readLinkedImage(md, hit, altStart, altEnd, imgStart, imgEnd,
                             hrefStart, hrefEnd, next)) {
            /* Not a linked image: keep the '[' and look again from the
             * '!' so a well-formed construct straight after still works. */
            out.push_back('[');
            pos = hit + 1;
            continue;
        }

        out.append("![");
        out.append(md, altStart, altEnd - altStart);
        out.append("](");
        out.append(md, imgStart, imgEnd - imgStart);
        out.push_back(')');

        WbnNewsImageLink link;
        link.image.assign(md, imgStart, imgEnd - imgStart);
        link.href.assign(md, hrefStart, hrefEnd - hrefStart);
        if (hasHttpScheme(link.href)) {
            outLinks.push_back(std::move(link));
        }
        pos = next;
    }
    return out;
}

const char *newsFindImageLink(const std::vector<WbnNewsImageLink> &links,
                              const char *image, size_t imageLen) {
    if (!image) return nullptr;
    for (const auto &l : links) {
        if (l.image.size() == imageLen &&
            memcmp(l.image.data(), image, imageLen) == 0) {
            return l.href.c_str();
        }
    }
    return nullptr;
}

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
