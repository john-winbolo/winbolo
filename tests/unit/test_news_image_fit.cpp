/*
 * Tests for newsImageFit — the size the news window draws an image at:
 * its own size when it fits, shrunk to fit otherwise, never enlarged.
 */

#include <cmath>

#include "test_harness.h"
#include "news_image_fit.h"

namespace {

int expectFit(float imgW, float imgH, float maxW, float maxH,
              float wantW, float wantH) {
    float w = -1.0f, h = -1.0f;
    newsImageFit(imgW, imgH, maxW, maxH, &w, &h);
    UT_ASSERT_MSG(w == wantW && h == wantH,
                  "%gx%g in %gx%g: got %gx%g, want %gx%g",
                  imgW, imgH, maxW, maxH, w, h, wantW, wantH);
    return 0;
}

} /* anonymous namespace */

/* An image that fits is drawn at its own size, never enlarged. */
static int fit_keeps_size_when_it_fits(void) {
    int rc;
    rc = expectFit(600, 481, 1800, 900, 600, 481); if (rc) return rc;
    rc = expectFit(600, 481, 600, 481, 600, 481);  if (rc) return rc;
    rc = expectFit(1, 1, 2, 2, 1, 1);              if (rc) return rc;
    return 0;
}

/* The 2.1 post's thumbnails in a narrow news window. */
static int fit_shrinks_to_width(void) {
    int rc;
    rc = expectFit(600, 481, 300, 900, 300, 240);  if (rc) return rc;
    rc = expectFit(600, 400, 450, 900, 450, 300);  if (rc) return rc;
    return 0;
}

static int fit_shrinks_to_height(void) {
    int rc;
    rc = expectFit(600, 1200, 1000, 300, 150, 300); if (rc) return rc;
    /* Width first brings it to 500 x 1000, still too tall. */
    rc = expectFit(1000, 2000, 500, 400, 200, 400); if (rc) return rc;
    return 0;
}

/* Fractional limits round down so the result always fits. */
static int fit_rounds_down_inside_the_limit(void) {
    float w, h;
    newsImageFit(600, 481, 333.7f, 900, &w, &h);
    UT_ASSERT_MSG(w <= 333.7f && h == std::floor(h) && w == std::floor(w),
                  "got %gx%g", w, h);
    UT_ASSERT_MSG(w == 333.0f && h == 267.0f, "got %gx%g", w, h);
    return 0;
}

/* A sliver of space still gives a 1 x 1 image rather than 0 or a
 * negative size, and an empty or broken size draws nothing. */
static int fit_degenerate_inputs(void) {
    int rc;
    rc = expectFit(600, 400, 0, 900, 1, 1);           if (rc) return rc;
    rc = expectFit(600, 400, -50, -50, 1, 1);         if (rc) return rc;
    rc = expectFit(10000, 10, 100, 100, 100, 1);      if (rc) return rc;
    rc = expectFit(0, 400, 100, 100, 0, 0);           if (rc) return rc;
    rc = expectFit(400, -1, 100, 100, 0, 0);          if (rc) return rc;
    rc = expectFit(NAN, 400, 100, 100, 0, 0);         if (rc) return rc;
    rc = expectFit(600, 400, NAN, 100, 1, 1);         if (rc) return rc;
    return 0;
}

extern "C" int run_news_image_fit(void) {
    int rc;
    rc = fit_keeps_size_when_it_fits();      if (rc) return rc;
    rc = fit_shrinks_to_width();             if (rc) return rc;
    rc = fit_shrinks_to_height();            if (rc) return rc;
    rc = fit_rounds_down_inside_the_limit(); if (rc) return rc;
    rc = fit_degenerate_inputs();            if (rc) return rc;
    return 0;
}
