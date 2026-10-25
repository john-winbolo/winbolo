/*
 * Tests for NewsImageBudget — which decoded news images the news
 * image cache keeps. The case that matters most is the 2.1 post: 17
 * thumbnails drawn on every frame against a cache that held 16, which
 * evicted one in use each time another arrived, so it was downloaded
 * again and the window flickered for as long as it was open.
 */

#include <algorithm>
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

/* fits() gives admit()'s answer without changing anything. */
static int budget_fits_is_a_dry_run(void) {
    NewsImageBudget b(300);
    std::vector<std::string> evict;
    b.newFrame();                                     /* frame 1 */
    b.admit("a", 100, evict);
    b.admit("b", 100, evict);
    b.newFrame();
    b.newFrame();                                     /* frame 3 */
    b.admit("c", 100, evict);                         /* a, b evictable; c pinned */

    UT_ASSERT_MSG(b.fits("d", 100), "d 100 should fit by evicting a");
    UT_ASSERT_MSG(b.fits("d", 200), "d 200 should fit by evicting a and b");
    UT_ASSERT_MSG(!b.fits("d", 250), "d 250 fits only by evicting c");
    UT_ASSERT_MSG(!b.fits("d", 301), "d 301 is over the whole budget");
    UT_ASSERT_MSG(b.fits("c", 300), "c 300 should fit by replacing c");
    UT_ASSERT_MSG(b.count() == 3 && b.usedBytes() == 300 && b.contains("a") &&
                  b.contains("b") && b.contains("c"),
                  "fits changed the budget: count %zu used %zu", b.count(),
                  b.usedBytes());

    UT_ASSERT_MSG(b.admit("d", 200, evict), "admit disagrees with fits");
    UT_ASSERT_MSG(evict.size() == 2 && evict[0] == "a" && evict[1] == "b",
                  "expected a then b evicted, got %zu", evict.size());
    return 0;
}

namespace {

/* The news window scrolling through a long post, as its image callback
 * drives the cache. Each frame, the downloads that have arrived are
 * admitted first, since newsImageGet drains them before it touches
 * anything; then every image in view is touched if it is loaded, or
 * requested if it is not. A request arrives at the start of the next
 * frame. Images out of view are neither touched nor requested. */
struct ScrollModel {
    NewsImageBudget          b;
    int                      view;      /* images in view at once */
    size_t                   img;       /* bytes per image */
    std::vector<std::string> arriving;  /* requested last frame */
    std::vector<int>         requests;  /* per image, this pass */
    int refusals        = 0;
    int evictions       = 0;
    int inViewEvictions = 0;

    ScrollModel(size_t budget, int view_, size_t img_, int images)
        : b(budget), view(view_), img(img_), requests((size_t)images, 0) {}

    void frame(int top) {
        std::vector<std::string> inView;
        for (int i = top; i < top + view; i++) inView.push_back(url(i));

        b.newFrame();
        std::vector<std::string> arrived;
        arrived.swap(arriving);
        std::vector<std::string> evict;
        for (const auto &u : arrived) {
            if (!b.admit(u, img, evict)) {
                refusals++;
                continue;
            }
            for (const auto &e : evict) {
                evictions++;
                for (const auto &s : inView) {
                    if (e == s) inViewEvictions++;
                }
            }
        }
        for (int i = top; i < top + view; i++) {
            const std::string u = url(i);
            if (b.contains(u)) {
                b.touch(u);
            } else {
                requests[(size_t)i]++;
                arriving.push_back(u);
            }
        }
    }

    void newPass() { std::fill(requests.begin(), requests.end(), 0); }

    /* How many images were requested more than once this pass: the
     * flicker bug's signature, an image downloaded again while the
     * window stays on it. */
    int repeats() const {
        int n = 0;
        for (int r : requests) n += r > 1;
        return n;
    }
    int requested() const {
        int n = 0;
        for (int r : requests) n += r > 0;
        return n;
    }
};

} /* anonymous namespace */

/* Scrolling down through 30 images three at a time in view, with a
 * budget of one screen plus the one image that enters it each frame:
 * every image is downloaded once, none is refused, nothing in view is
 * evicted, and the budget stays full rather than growing. Scrolling
 * back up downloads the evicted images again, once each. */
static int budget_scrolling_window(void) {
    const int    images = 30, view = 3;
    const size_t img    = 100;
    ScrollModel  m((size_t)(view + 1) * img, view, img, images);

    for (int top = 0; top + view <= images; top++) m.frame(top);
    m.frame(images - view);                          /* arrivals settle */
    UT_ASSERT_MSG(m.refusals == 0, "down: %d refused", m.refusals);
    UT_ASSERT_MSG(m.inViewEvictions == 0, "down: %d in-view evictions",
                  m.inViewEvictions);
    UT_ASSERT_MSG(m.repeats() == 0, "down: %d images downloaded twice",
                  m.repeats());
    UT_ASSERT_MSG(m.requested() == images, "down: %d of %d requested",
                  m.requested(), images);
    UT_ASSERT_MSG(m.b.usedBytes() <= (size_t)(view + 1) * img, "over budget");
    UT_ASSERT_MSG(m.evictions == images - view - 1, "down: %d evictions",
                  m.evictions);

    m.newPass();
    for (int top = images - view; top >= 0; top--) m.frame(top);
    m.frame(0);
    UT_ASSERT_MSG(m.refusals == 0, "up: %d refused", m.refusals);
    UT_ASSERT_MSG(m.inViewEvictions == 0, "up: %d in-view evictions",
                  m.inViewEvictions);
    UT_ASSERT_MSG(m.repeats() == 0, "up: %d images downloaded twice",
                  m.repeats());
    /* The images still held from the way down are not fetched again. */
    UT_ASSERT_MSG(m.requested() == images - view - 1, "up: %d requested",
                  m.requested());
    return 0;
}

/* Scrolling two images a frame needs room for two arrivals beside the
 * screen that was drawn last frame. With that, nothing is refused. */
static int budget_scrolling_fast(void) {
    const int    images = 40, view = 3, step = 2;
    const size_t img    = 100;
    ScrollModel  m((size_t)(view + step) * img, view, img, images);

    for (int top = 0; top + view <= images; top += step) m.frame(top);
    m.frame(images - view);
    UT_ASSERT_MSG(m.refusals == 0, "%d refused", m.refusals);
    UT_ASSERT_MSG(m.inViewEvictions == 0, "%d in-view evictions",
                  m.inViewEvictions);
    UT_ASSERT_MSG(m.repeats() == 0, "%d images downloaded twice", m.repeats());
    return 0;
}

/* Staying put on one screen that needs more than the budget: the
 * images that fit are kept and the rest are refused every frame, but
 * nothing that is in view is ever evicted to make room for them, so
 * the images that did load do not flicker. */
static int budget_screen_over_budget_is_stable(void) {
    const int    view = 5;
    const size_t img  = 100;
    ScrollModel  m(3 * img, view, img, view);

    for (int frame = 0; frame < 50; frame++) m.frame(0);
    UT_ASSERT_MSG(m.b.count() == 3, "held %zu images", m.b.count());
    UT_ASSERT_MSG(m.evictions == 0, "%d evictions", m.evictions);
    UT_ASSERT_MSG(m.refusals > 0, "nothing refused");
    for (int i = 0; i < 3; i++) {
        UT_ASSERT_MSG(m.b.contains(url(i)), "%s lost", url(i).c_str());
    }
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
    rc = budget_fits_is_a_dry_run();                 if (rc) return rc;
    rc = budget_scrolling_window();                  if (rc) return rc;
    rc = budget_scrolling_fast();                    if (rc) return rc;
    rc = budget_screen_over_budget_is_stable();      if (rc) return rc;
    return 0;
}
