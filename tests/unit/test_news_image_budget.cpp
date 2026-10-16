/*
 * Tests for NewsImageBudget — which decoded news images the news
 * image cache keeps. The case that matters most is the 2.1 post: 17
 * thumbnails drawn on every frame against a cache that held 16, which
 * evicted one in use each time another arrived, so it was downloaded
 * again and the window flickered for as long as it was open.
 */

#include <string>
#include <vector>

#include "test_harness.h"
#include "news_image_budget.h"

namespace {

std::string url(int i) {
    return "https://www.winbolo.com/img/updates/" + std::to_string(i) + ".png";
}

/* One frame of the news window: every admitted image in `urls` drawn. */
void drawFrame(NewsImageBudget &b, const std::vector<std::string> &urls) {
    b.newFrame();
    for (const auto &u : urls) b.touch(u);
}

} /* anonymous namespace */

/* The 2.1 post at the old count limit's size: 16 images fit, and the
 * 17th, arriving while all 16 are drawn every frame, is refused rather
 * than evicting one. Nothing is ever evicted after that. */
static int budget_never_evicts_images_in_use(void) {
    const size_t thumb = 600 * 400 * 4;
    NewsImageBudget b(16 * thumb);
    std::vector<std::string> drawn;
    std::vector<std::string> evict;

    for (int i = 0; i < 16; i++) {
        drawFrame(b, drawn);
        UT_ASSERT_MSG(b.admit(url(i), thumb, evict), "image %d refused", i);
        UT_ASSERT_MSG(evict.empty(), "image %d evicted %zu", i, evict.size());
        drawn.push_back(url(i));
    }
    for (int frame = 0; frame < 100; frame++) {
        drawFrame(b, drawn);
        bool ok = b.admit(url(16), thumb, evict);
        UT_ASSERT_MSG(!ok, "frame %d: 17th image admitted", frame);
        UT_ASSERT_MSG(evict.empty(), "frame %d: evicted %s", frame,
                      evict.empty() ? "" : evict[0].c_str());
    }
    UT_ASSERT_MSG(b.count() == 16, "count %zu", b.count());
    for (const auto &u : drawn) {
        UT_ASSERT_MSG(b.contains(u), "%s lost", u.c_str());
    }
    return 0;
}

/* An image drawn in this frame or the previous one is kept; one last
 * drawn two frames ago may go. */
static int budget_pins_current_and_previous_frame(void) {
    NewsImageBudget b(200);
    std::vector<std::string> evict;
    b.newFrame();                                     /* frame 1 */
    UT_ASSERT_MSG(b.admit("a", 100, evict), "a refused");
    UT_ASSERT_MSG(b.admit("b", 100, evict), "b refused");

    UT_ASSERT_MSG(!b.admit("c", 100, evict), "c evicted from this frame");
    b.newFrame();                                     /* frame 2 */
    UT_ASSERT_MSG(!b.admit("c", 100, evict), "c evicted from last frame");
    UT_ASSERT_MSG(b.contains("a") && b.contains("b"), "a or b lost");

    b.touch("b");                                     /* b drawn in frame 2 */
    b.newFrame();                                     /* frame 3 */
    UT_ASSERT_MSG(b.admit("c", 100, evict), "c refused in frame 3");
    UT_ASSERT_MSG(evict.size() == 1 && evict[0] == "a",
                  "expected a evicted, got %zu", evict.size());
    UT_ASSERT_MSG(b.contains("b") && b.contains("c") && !b.contains("a"),
                  "wrong survivors");
    UT_ASSERT_MSG(b.usedBytes() == 200, "used %zu", b.usedBytes());
    return 0;
}

/* Eviction takes the least recently drawn first, and only as many as
 * the new image needs. */
static int budget_evicts_least_recently_drawn_first(void) {
    NewsImageBudget b(300);
    std::vector<std::string> evict;
    b.newFrame();
    b.admit("a", 100, evict);
    b.admit("b", 100, evict);
    b.admit("c", 100, evict);
    b.newFrame();
    b.touch("a");                 /* order now a, c, b — b least recent */
    b.newFrame();
    b.newFrame();

    UT_ASSERT_MSG(b.admit("d", 100, evict), "d refused");
    UT_ASSERT_MSG(evict.size() == 1 && evict[0] == "b",
                  "expected only b evicted, got %zu", evict.size());

    UT_ASSERT_MSG(b.admit("e", 200, evict), "e refused");
    UT_ASSERT_MSG(evict.size() == 2 && evict[0] == "c" && evict[1] == "a",
                  "expected c then a evicted, got %zu", evict.size());
    UT_ASSERT_MSG(b.count() == 2 && b.usedBytes() == 300,
                  "count %zu used %zu", b.count(), b.usedBytes());
    return 0;
}

/* A refusal evicts nothing, even when some images could have gone. */
static int budget_refusal_changes_nothing(void) {
    NewsImageBudget b(300);
    std::vector<std::string> evict;
    b.newFrame();
    b.admit("old", 100, evict);
    b.newFrame();
    b.newFrame();
    b.admit("new1", 100, evict);
    b.admit("new2", 100, evict);

    /* Evicting "old" frees 100 but 250 is needed. */
    UT_ASSERT_MSG(!b.admit("big", 250, evict), "big admitted");
    UT_ASSERT_MSG(evict.empty(), "refusal evicted %zu", evict.size());
    UT_ASSERT_MSG(b.contains("old") && b.count() == 3 && b.usedBytes() == 300,
                  "refusal changed the budget");

    UT_ASSERT_MSG(!b.admit("huge", 301, evict), "image over the budget admitted");
    UT_ASSERT_MSG(b.admit("exact", 300, evict) == false,
                  "exact-size image admitted over images in use");
    return 0;
}

/* Without any newFrame() call nothing is evictable, so the budget is a
 * plain hard limit. */
static int budget_without_frames_is_a_hard_limit(void) {
    NewsImageBudget b(200);
    std::vector<std::string> evict;
    UT_ASSERT_MSG(b.admit("a", 100, evict), "a refused");
    UT_ASSERT_MSG(b.admit("b", 100, evict), "b refused");
    UT_ASSERT_MSG(!b.admit("c", 1, evict), "c admitted");
    UT_ASSERT_MSG(evict.empty(), "evicted without frames");
    return 0;
}

static int budget_readmit_forget_clear(void) {
    NewsImageBudget b(300);
    std::vector<std::string> evict;
    b.newFrame();
    b.admit("a", 100, evict);
    b.admit("b", 100, evict);

    /* Admitting "a" again replaces its size rather than adding it, and
     * fits even though the other image is in use. */
    UT_ASSERT_MSG(b.admit("a", 200, evict), "re-admit refused");
    UT_ASSERT_MSG(evict.empty(), "re-admit evicted %zu", evict.size());
    UT_ASSERT_MSG(b.count() == 2 && b.usedBytes() == 300,
                  "count %zu used %zu", b.count(), b.usedBytes());

    b.touch("unknown");
    b.forget("unknown");
    UT_ASSERT_MSG(b.count() == 2 && b.usedBytes() == 300, "unknown URL changed it");

    b.forget("a");
    UT_ASSERT_MSG(b.count() == 1 && b.usedBytes() == 100 && !b.contains("a"),
                  "forget failed");

    b.clear();
    UT_ASSERT_MSG(b.count() == 0 && b.usedBytes() == 0, "clear failed");
    UT_ASSERT_MSG(b.admit("c", 300, evict), "admit after clear refused");
    return 0;
}

/* Scrolling past a long post with a small budget: images drawn earlier
 * and no longer on screen give way, images on screen never do, and
 * each image is evicted at most once per pass. */
static int budget_scrolling_window(void) {
    const size_t img = 100;
    NewsImageBudget b(4 * img);
    std::vector<std::string> evict;
    int evictions = 0;

    for (int top = 0; top < 30; top++) {
        std::vector<std::string> onScreen;
        for (int i = top; i < top + 3; i++) onScreen.push_back(url(i));
        drawFrame(b, onScreen);
        for (const auto &u : onScreen) {
            if (b.contains(u)) continue;
            UT_ASSERT_MSG(b.admit(u, img, evict), "top %d: %s refused", top,
                          u.c_str());
            for (const auto &e : evict) {
                for (const auto &s : onScreen) {
                    UT_ASSERT_MSG(e != s, "top %d: evicted on-screen %s", top,
                                  e.c_str());
                }
            }
            evictions += (int)evict.size();
        }
        UT_ASSERT_MSG(b.usedBytes() <= 4 * img, "top %d: over budget", top);
    }
    UT_ASSERT_MSG(evictions <= 32, "evicted %d times", evictions);
    return 0;
}

extern "C" int run_news_image_budget(void) {
    int rc;
    rc = budget_never_evicts_images_in_use();        if (rc) return rc;
    rc = budget_pins_current_and_previous_frame();   if (rc) return rc;
    rc = budget_evicts_least_recently_drawn_first(); if (rc) return rc;
    rc = budget_refusal_changes_nothing();           if (rc) return rc;
    rc = budget_without_frames_is_a_hard_limit();    if (rc) return rc;
    rc = budget_readmit_forget_clear();              if (rc) return rc;
    rc = budget_scrolling_window();                  if (rc) return rc;
    return 0;
}
