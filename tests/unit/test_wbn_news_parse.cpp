/*
 * Tests for parseNewsResponse — the pure-C++ JSON parser
 * extracted from wbn_news.cpp so it can be exercised here
 * without libcurl, http.c, or any threading.
 *
 * Every case allocates body_md per-item via SDL_strdup, so
 * each case is responsible for freeing those strings before
 * returning.
 */

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "test_harness.h"
#include "wbn_news.h"
#include "wbn_news_parser.h"

namespace {

void freeItems(std::vector<WbnNewsItem> &items) {
    for (auto &it : items) {
        if (it.body_md) {
            free(it.body_md);
            it.body_md = nullptr;
        }
    }
    items.clear();
}

} /* anonymous namespace */

static int parse_valid_one_item_au(void) {
    const char *raw =
        "{\"news\":[{\"id\":1,\"title\":\"T\",\"body\":\"B\","
        "\"comments\":3,\"url\":\"https://winbolo.net/x\","
        "\"date\":\"2026-05-18 14:32:00\"}],"
        "\"countryCode\":\"AU\"}";
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse(raw, items, country);
    UT_ASSERT_MSG(ok, "expected true, got false");
    UT_ASSERT_MSG(items.size() == 1, "expected 1 item, got %zu", items.size());
    UT_ASSERT_MSG(items[0].id == 1, "expected id=1, got %d", items[0].id);
    UT_ASSERT_MSG(strcmp(items[0].title, "T") == 0,
                  "title mismatch: \"%s\"", items[0].title);
    UT_ASSERT_MSG(items[0].body_md && strcmp(items[0].body_md, "B") == 0,
                  "body mismatch: \"%s\"", items[0].body_md ? items[0].body_md : "(null)");
    UT_ASSERT_MSG(items[0].comments == 3,
                  "comments=%d expected 3", items[0].comments);
    UT_ASSERT_MSG(strcmp(items[0].url, "https://winbolo.net/x") == 0,
                  "url mismatch: \"%s\"", items[0].url);
    UT_ASSERT_MSG(strcmp(items[0].date, "2026-05-18 14:32:00") == 0,
                  "date mismatch: \"%s\"", items[0].date);
    UT_ASSERT_MSG(country == "AU", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_date_missing(void) {
    /* date is optional — older responses (or servers that haven't been
     * upgraded yet) must still parse, leaving the buffer empty. */
    const char *raw =
        "{\"news\":[{\"id\":1,\"title\":\"T\",\"body\":\"B\","
        "\"url\":\"u\"}],\"countryCode\":\"AU\"}";
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse(raw, items, country);
    UT_ASSERT_MSG(ok, "expected true");
    UT_ASSERT_MSG(items.size() == 1, "size=%zu", items.size());
    UT_ASSERT_MSG(items[0].date[0] == '\0',
                  "expected empty date, got \"%s\"", items[0].date);
    freeItems(items);
    return 0;
}

static int parse_date_not_string(void) {
    /* Wrong type — server sent a number instead of a string. Treat as
     * missing rather than rejecting the whole item. */
    const char *raw =
        "{\"news\":[{\"id\":1,\"title\":\"T\",\"body\":\"B\",\"url\":\"u\","
        "\"date\":42}],\"countryCode\":\"AU\"}";
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse(raw, items, country);
    UT_ASSERT_MSG(ok, "expected true");
    UT_ASSERT_MSG(items.size() == 1, "size=%zu", items.size());
    UT_ASSERT_MSG(items[0].date[0] == '\0',
                  "expected empty date, got \"%s\"", items[0].date);
    freeItems(items);
    return 0;
}

static int parse_empty_news_array(void) {
    const char *raw = "{\"news\":[],\"countryCode\":\"NZ\"}";
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse(raw, items, country);
    UT_ASSERT_MSG(ok, "expected true");
    UT_ASSERT_MSG(items.empty(), "expected empty, got %zu", items.size());
    UT_ASSERT_MSG(country == "NZ", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_legacy_array_top_level(void) {
    const char *raw = "[{\"id\":1,\"title\":\"T\",\"body\":\"B\",\"url\":\"u\"}]";
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse(raw, items, country);
    UT_ASSERT_MSG(!ok, "expected false, got true");
    UT_ASSERT_MSG(items.empty(), "items should be empty");
    UT_ASSERT_MSG(country == "XX", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_top_level_null(void) {
    std::vector<WbnNewsItem> items;
    std::string country;
    UT_ASSERT(!parseNewsResponse("null", items, country));
    UT_ASSERT(items.empty());
    UT_ASSERT_MSG(country == "XX", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_top_level_string(void) {
    std::vector<WbnNewsItem> items;
    std::string country;
    UT_ASSERT(!parseNewsResponse("\"hi\"", items, country));
    UT_ASSERT_MSG(country == "XX", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_top_level_number(void) {
    std::vector<WbnNewsItem> items;
    std::string country;
    UT_ASSERT(!parseNewsResponse("42", items, country));
    UT_ASSERT_MSG(country == "XX", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_missing_news_field(void) {
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse("{\"countryCode\":\"AU\"}", items, country);
    UT_ASSERT_MSG(!ok, "expected false");
    UT_ASSERT(items.empty());
    UT_ASSERT_MSG(country == "XX", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_news_not_array(void) {
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse("{\"news\":\"oops\"}", items, country);
    UT_ASSERT_MSG(!ok, "expected false");
    UT_ASSERT_MSG(country == "XX", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_country_missing(void) {
    const char *raw = "{\"news\":[{\"id\":1,\"title\":\"T\",\"body\":\"B\",\"url\":\"u\"}]}";
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse(raw, items, country);
    UT_ASSERT_MSG(ok, "expected true");
    UT_ASSERT_MSG(items.size() == 1, "size=%zu", items.size());
    UT_ASSERT_MSG(country == "XX", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_country_lowercase(void) {
    const char *raw = "{\"news\":[],\"countryCode\":\"au\"}";
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse(raw, items, country);
    UT_ASSERT_MSG(ok, "expected true");
    UT_ASSERT_MSG(country == "AU", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_country_wrong_length(void) {
    const char *raw = "{\"news\":[{\"id\":1,\"title\":\"T\",\"body\":\"B\",\"url\":\"u\"}],"
                      "\"countryCode\":\"AUS\"}";
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse(raw, items, country);
    UT_ASSERT_MSG(ok, "expected true");
    UT_ASSERT_MSG(items.size() == 1, "size=%zu", items.size());
    UT_ASSERT_MSG(country == "XX", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_item_missing_id(void) {
    const char *raw =
        "{\"news\":[{\"title\":\"T\",\"body\":\"B\",\"url\":\"u\"},"
                  "{\"id\":2,\"title\":\"T2\",\"body\":\"B2\",\"url\":\"u2\"}],"
         "\"countryCode\":\"AU\"}";
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse(raw, items, country);
    UT_ASSERT_MSG(ok, "expected true");
    UT_ASSERT_MSG(items.size() == 1, "expected 1 item, got %zu", items.size());
    UT_ASSERT_MSG(items[0].id == 2, "expected id=2, got %d", items[0].id);
    UT_ASSERT_MSG(strcmp(items[0].title, "T2") == 0,
                  "title=\"%s\"", items[0].title);
    UT_ASSERT_MSG(country == "AU", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_item_body_not_string(void) {
    const char *raw =
        "{\"news\":[{\"id\":1,\"title\":\"T\",\"body\":42,\"url\":\"u\"}],"
         "\"countryCode\":\"AU\"}";
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse(raw, items, country);
    UT_ASSERT_MSG(ok, "expected true");
    UT_ASSERT_MSG(items.empty(), "expected 0 items, got %zu", items.size());
    UT_ASSERT_MSG(country == "AU", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_malformed_json(void) {
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse("{not json", items, country);
    UT_ASSERT_MSG(!ok, "expected false");
    UT_ASSERT_MSG(country == "XX", "country=\"%s\"", country.c_str());
    freeItems(items);
    return 0;
}

static int parse_url_truncation(void) {
    /* Build a URL of length 600 — longer than the 512-byte url field
     * — to exercise SDL_strlcpy truncation. */
    std::string longUrl = "https://winbolo.net/";
    while (longUrl.size() < 600) longUrl.push_back('a');
    std::string raw = "{\"news\":[{\"id\":1,\"title\":\"T\",\"body\":\"B\","
                      "\"url\":\"" + longUrl + "\"}],\"countryCode\":\"AU\"}";
    std::vector<WbnNewsItem> items;
    std::string country;
    bool ok = parseNewsResponse(raw.c_str(), items, country);
    UT_ASSERT_MSG(ok, "expected true");
    UT_ASSERT_MSG(items.size() == 1, "expected 1 item, got %zu", items.size());
    /* sizeof(url) is 512 — index 511 is the terminator slot. */
    UT_ASSERT_MSG(items[0].url[sizeof(items[0].url) - 1] == '\0',
                  "url buffer not NUL-terminated after truncation");
    UT_ASSERT_MSG(strncmp(items[0].url, "https://winbolo.net/", 20) == 0,
                  "prefix lost: \"%s\"", items[0].url);
    freeItems(items);
    return 0;
}

extern "C" int run_wbn_news_parse(void) {
    int rc;
    rc = parse_valid_one_item_au();      if (rc) return rc;
    rc = parse_date_missing();           if (rc) return rc;
    rc = parse_date_not_string();        if (rc) return rc;
    rc = parse_empty_news_array();       if (rc) return rc;
    rc = parse_legacy_array_top_level(); if (rc) return rc;
    rc = parse_top_level_null();         if (rc) return rc;
    rc = parse_top_level_string();       if (rc) return rc;
    rc = parse_top_level_number();       if (rc) return rc;
    rc = parse_missing_news_field();     if (rc) return rc;
    rc = parse_news_not_array();         if (rc) return rc;
    rc = parse_country_missing();        if (rc) return rc;
    rc = parse_country_lowercase();      if (rc) return rc;
    rc = parse_country_wrong_length();   if (rc) return rc;
    rc = parse_item_missing_id();        if (rc) return rc;
    rc = parse_item_body_not_string();   if (rc) return rc;
    rc = parse_malformed_json();         if (rc) return rc;
    rc = parse_url_truncation();         if (rc) return rc;
    return 0;
}
