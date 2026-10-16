/*
 * Tests for newsUnwrapLinkedImages and newsFindImageLink — the rewrite
 * that turns the website's linked thumbnails, [![alt](thumb)](full),
 * into plain images imgui_markdown can draw, keeping the link aside.
 * The body comes from the network, so most cases here are malformed
 * or hostile input that must come back unchanged.
 */

#include <cstring>
#include <string>
#include <vector>

#include "test_harness.h"
#include "wbn_news_parser.h"

namespace {

/* Runs the rewrite and returns its output; links go to `links`. */
std::string unwrap(const std::string &md,
                   std::vector<WbnNewsImageLink> &links) {
    links.clear();
    return newsUnwrapLinkedImages(md, links);
}

/* Asserts the input comes back byte-for-byte with no links recorded. */
int expectUnchanged(const std::string &md, const char *what) {
    std::vector<WbnNewsImageLink> links;
    std::string out = unwrap(md, links);
    UT_ASSERT_MSG(out == md, "%s: changed to \"%s\"", what, out.c_str());
    UT_ASSERT_MSG(links.empty(), "%s: recorded %zu links", what,
                  links.size());
    return 0;
}

} /* anonymous namespace */

/* The exact shape the 2.1 post uses. */
static int md_unwraps_live_feed_shape(void) {
    const std::string md =
        "Intro.\n\n"
        "[![The script chooser in the lobby](https://www.winbolo.com/img/"
        "updates/lobby-mods-thumb.png)](https://www.winbolo.com/img/updates/"
        "lobby-mods.png)\n\n### Next";
    std::vector<WbnNewsImageLink> links;
    std::string out = unwrap(md, links);
    const std::string want =
        "Intro.\n\n"
        "![The script chooser in the lobby](https://www.winbolo.com/img/"
        "updates/lobby-mods-thumb.png)\n\n### Next";
    UT_ASSERT_MSG(out == want, "got \"%s\"", out.c_str());
    UT_ASSERT_MSG(links.size() == 1, "expected 1 link, got %zu", links.size());
    UT_ASSERT_MSG(links[0].image ==
                  "https://www.winbolo.com/img/updates/lobby-mods-thumb.png",
                  "image \"%s\"", links[0].image.c_str());
    UT_ASSERT_MSG(links[0].href ==
                  "https://www.winbolo.com/img/updates/lobby-mods.png",
                  "href \"%s\"", links[0].href.c_str());
    return 0;
}

static int md_unwraps_several_in_order(void) {
    const std::string md =
        "a [![x](https://h/1.png)](https://h/1) b "
        "[![y](https://h/2.png)](http://h/2)c";
    std::vector<WbnNewsImageLink> links;
    std::string out = unwrap(md, links);
    UT_ASSERT_MSG(out == "a ![x](https://h/1.png) b ![y](https://h/2.png)c",
                  "got \"%s\"", out.c_str());
    UT_ASSERT_MSG(links.size() == 2, "expected 2 links, got %zu", links.size());
    UT_ASSERT_MSG(links[0].href == "https://h/1", "href0 \"%s\"",
                  links[0].href.c_str());
    UT_ASSERT_MSG(links[1].href == "http://h/2", "href1 \"%s\"",
                  links[1].href.c_str());
    return 0;
}

static int md_leaves_plain_markdown_alone(void) {
    int rc;
    rc = expectUnchanged("", "empty");                              if (rc) return rc;
    rc = expectUnchanged("no links here", "text");                  if (rc) return rc;
    rc = expectUnchanged("![alt](https://h/i.png)", "plain image"); if (rc) return rc;
    rc = expectUnchanged("[text](https://h/)", "plain link");       if (rc) return rc;
    rc = expectUnchanged("[![", "bare opener");                     if (rc) return rc;
    return 0;
}

/* The image is still unwrapped, but a non-http link is never recorded
 * and so can never be opened. */
static int md_drops_non_http_href(void) {
    const char *hrefs[] = {
        "javascript:alert`1`", "file:///etc/passwd", "steam://run/1",
        "data:text/html,x", "//h/x", "relative/x",
    };
    for (const char *href : hrefs) {
        std::string md = std::string("[![a](https://h/i.png)](") + href + ")";
        std::vector<WbnNewsImageLink> links;
        std::string out = unwrap(md, links);
        UT_ASSERT_MSG(out == "![a](https://h/i.png)", "%s: got \"%s\"", href,
                      out.c_str());
        UT_ASSERT_MSG(links.empty(), "%s: link recorded", href);
    }
    return 0;
}

static int md_accepts_uppercase_scheme(void) {
    std::vector<WbnNewsImageLink> links;
    unwrap("[![a](https://h/i.png)](HTTPS://h/full)", links);
    UT_ASSERT_MSG(links.size() == 1 && links[0].href == "HTTPS://h/full",
                  "uppercase scheme not recorded");
    return 0;
}

static int md_rejects_malformed(void) {
    const char *cases[] = {
        "[![a](https://h/i.png)](https://h/full",      /* no final ')' */
        "[![a](https://h/i.png](https://h/full)",      /* no ')' after image */
        "[![a](https://h/i.png) ](https://h/full)",    /* space before ']' */
        "[![a](https://h/i.png)] (https://h/full)",    /* space before '(' */
        "[![a] (https://h/i.png)](https://h/full)",    /* space after alt */
        "[![a]()](https://h/full)",                    /* empty image */
        "[![a](https://h/i.png)]()",                   /* empty href */
        "[![a](https://h/i png)](https://h/full)",     /* space in image */
        "[![a](https://h/i.png)](https://h/f ull)",    /* space in href */
        "[![a](https://h/i.png)](https://h/f\null)",   /* newline in href */
        "[![a](https://h/i.png)](https://h/f\tull)",   /* tab in href */
        "[![a](https://h/i\x7f.png)](https://h/full)", /* DEL in image */
        "[![a](https://h/(i).png)](https://h/full)",   /* paren in image */
        "[![a](https://h/i.png)](https://h/<b>)",      /* angle in href */
        "[![a](https://h/[i].png)](https://h/full)",   /* bracket in image */
        "[![a[b]](https://h/i.png)](https://h/full)",  /* bracket in alt */
        "[![a](https://h/i.png)]",                     /* no outer link */
    };
    for (const char *md : cases) {
        int rc = expectUnchanged(md, md);
        if (rc) return rc;
    }
    /* A control byte that is not a C escape. */
    std::string md = "[![a](https://h/i.png)](https://h/f";
    md.push_back('\x01');
    md += ")";
    return expectUnchanged(md, "SOH in href");
}

/* Every prefix of a valid construct is incomplete and must come back
 * unchanged; this walks the parser off the end of the string at each
 * position. */
static int md_truncated_at_every_byte(void) {
    const std::string full = "[![alt](https://h/i.png)](https://h/full)";
    for (size_t n = 0; n < full.size(); n++) {
        std::string md = full.substr(0, n);
        int rc = expectUnchanged(md, md.c_str());
        if (rc) return rc;
    }
    return 0;
}

/* A failed match must not swallow a good one that follows it. */
static int md_recovers_after_failed_match(void) {
    std::vector<WbnNewsImageLink> links;
    std::string out = unwrap(
        "[![x](bad url)[![a](https://h/i.png)](https://h/full) [[![b](i)](https://h/2)",
        links);
    UT_ASSERT_MSG(out == "[![x](bad url)![a](https://h/i.png) [![b](i)",
                  "got \"%s\"", out.c_str());
    UT_ASSERT_MSG(links.size() == 2, "expected 2 links, got %zu", links.size());
    return 0;
}

static int md_url_length_cap(void) {
    const std::string prefix = "https://h/";
    std::string atCap = prefix + std::string(WBN_NEWS_MD_MAX_URL - prefix.size(), 'a');
    std::string overCap = atCap + "a";

    std::vector<WbnNewsImageLink> links;
    std::string out = unwrap("[![a](" + atCap + ")](" + atCap + ")", links);
    UT_ASSERT_MSG(out == "![a](" + atCap + ")", "URL at the cap not unwrapped");
    UT_ASSERT_MSG(links.size() == 1 && links[0].href.size() == WBN_NEWS_MD_MAX_URL,
                  "URL at the cap not recorded");

    int rc;
    rc = expectUnchanged("[![a](" + overCap + ")](https://h/full)", "long image");
    if (rc) return rc;
    rc = expectUnchanged("[![a](https://h/i.png)](" + overCap + ")", "long href");
    return rc;
}

static int md_alt_length_cap(void) {
    std::string atCap(WBN_NEWS_MD_MAX_ALT, 'x');
    std::vector<WbnNewsImageLink> links;
    std::string out = unwrap("[![" + atCap + "](https://h/i.png)](https://h/full)",
                             links);
    UT_ASSERT_MSG(out == "![" + atCap + "](https://h/i.png)",
                  "alt at the cap not unwrapped");
    return expectUnchanged("[![" + atCap + "x](https://h/i.png)](https://h/full)",
                           "long alt");
}

/* Large bodies built from near-misses. Each must come back unchanged;
 * with the scans stopping at the next '[' this runs in linear time,
 * and a quadratic rewrite would make this case take minutes. */
static int md_hostile_bodies(void) {
    const char *units[] = {
        "[![",
        "[![a",
        "[![a](",
        "[![a](https://h/i.png)",
        "[![a](https://h/i.png)](",
        "[![a](https://h/i.png)](https://h/full",
    };
    for (const char *unit : units) {
        std::string md;
        while (md.size() < 512 * 1024) md += unit;
        int rc = expectUnchanged(md, unit);
        if (rc) return rc;
    }
    /* One opener, then a URL that runs on for the rest of the body. */
    std::string md = "[![a](" + std::string(512 * 1024, 'u');
    return expectUnchanged(md, "endless URL");
}

static int md_find_image_link(void) {
    std::vector<WbnNewsImageLink> links;
    unwrap("[![a](https://h/i.png)](https://h/first) "
           "[![b](https://h/i.png)](https://h/second) "
           "[![c](https://h/j.png)](https://h/j)",
           links);
    UT_ASSERT_MSG(links.size() == 3, "expected 3 links, got %zu", links.size());

    /* imgui_markdown hands over a pointer into the body with a length,
     * not a terminated string. */
    const char *body = "https://h/i.pngXYZ";
    const char *href = newsFindImageLink(links, body, 15);
    UT_ASSERT_MSG(href && strcmp(href, "https://h/first") == 0,
                  "first link should win, got \"%s\"", href ? href : "(null)");

    href = newsFindImageLink(links, "https://h/j.png", 15);
    UT_ASSERT_MSG(href && strcmp(href, "https://h/j") == 0, "j.png not found");

    UT_ASSERT_MSG(!newsFindImageLink(links, body, 14), "prefix matched");
    UT_ASSERT_MSG(!newsFindImageLink(links, body, 16), "longer string matched");
    UT_ASSERT_MSG(!newsFindImageLink(links, "https://h/k.png", 15),
                  "unknown image matched");
    UT_ASSERT_MSG(!newsFindImageLink(links, nullptr, 15), "null image matched");

    std::vector<WbnNewsImageLink> none;
    UT_ASSERT_MSG(!newsFindImageLink(none, "https://h/i.png", 15),
                  "empty list matched");
    return 0;
}

extern "C" int run_wbn_news_markdown(void) {
    int rc;
    rc = md_unwraps_live_feed_shape();     if (rc) return rc;
    rc = md_unwraps_several_in_order();    if (rc) return rc;
    rc = md_leaves_plain_markdown_alone(); if (rc) return rc;
    rc = md_drops_non_http_href();         if (rc) return rc;
    rc = md_accepts_uppercase_scheme();    if (rc) return rc;
    rc = md_rejects_malformed();           if (rc) return rc;
    rc = md_truncated_at_every_byte();     if (rc) return rc;
    rc = md_recovers_after_failed_match(); if (rc) return rc;
    rc = md_url_length_cap();              if (rc) return rc;
    rc = md_alt_length_cap();              if (rc) return rc;
    rc = md_hostile_bodies();              if (rc) return rc;
    rc = md_find_image_link();             if (rc) return rc;
    return 0;
}
