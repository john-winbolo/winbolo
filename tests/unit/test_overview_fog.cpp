/*
 * Tests for the map overview's fog mask (overview_fog.cpp) — the bytes the
 * renderer stretches over the map to dim everything outside a live region.
 * Nothing here touches SDL: rects in, bytes out.
 */

#include <cstring>

#include "test_harness.h"
#include "overview_fog.h"

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* The mask plus a tail the builder has no business touching: every write it
 * makes is driven by a rect, and a rect at the map edge that failed to clamp
 * would run off the end of the last row into this. */
#define FOG_GUARD_BYTES 64
#define FOG_GUARD_FILL  0xA5

static BYTE fogBuf[OVERVIEW_FOG_MASK_BYTES + FOG_GUARD_BYTES];

static BYTE fogAt(int x, int y) {
    return fogBuf[(size_t)y * MAP_ARRAY_SIZE + (size_t)x];
}

static void fogBuild(const OverviewRect *live, int liveCount) {
    memset(fogBuf, FOG_GUARD_FILL, sizeof(fogBuf));
    overviewFogBuildMask(live, liveCount, fogBuf);
}

static int fogGuardIntact(void) {
    for (int i = 0; i < FOG_GUARD_BYTES; i++) {
        BYTE got = fogBuf[OVERVIEW_FOG_MASK_BYTES + i];
        UT_ASSERT_MSG(got == FOG_GUARD_FILL,
                      "guard byte %d was written: 0x%02X, expected 0x%02X",
                      i, (unsigned)got, (unsigned)FOG_GUARD_FILL);
    }
    return 0;
}

/* A live square is clear, ground well past the ramp is fully fogged, and the
 * region's own edge — the square the fade starts from — is clear too. */
static int fog_live_is_clear_and_far_is_fogged(void) {
    OverviewRect live = { 100, 100, 120, 120 };

    fogBuild(&live, 1);

    UT_ASSERT_MSG(fogAt(110, 110) == 0,
                  "square in the middle of the region carries %u fog",
                  (unsigned)fogAt(110, 110));
    UT_ASSERT_MSG(fogAt(120, 120) == 0,
                  "the region's corner square carries %u fog, expected clear",
                  (unsigned)fogAt(120, 120));
    UT_ASSERT_MSG(fogAt(200, 200) == OVERVIEW_FOG_ALPHA,
                  "ground far from the region carries %u fog, expected %u",
                  (unsigned)fogAt(200, 200), (unsigned)OVERVIEW_FOG_ALPHA);
    return fogGuardIntact();
}

/* Walking straight out from an edge, the fog rises square by square and has
 * reached full fog by the end of the ramp — the shape the whole overlay is
 * for, since a step from clear to full in one square is the hard edge it
 * replaces. */
static int fog_ramps_out_of_the_region(void) {
    OverviewRect live = { 100, 100, 120, 120 };

    fogBuild(&live, 1);

    BYTE prev = 0;
    for (int d = 1; d < OVERVIEW_FOG_RAMP; d++) {
        BYTE got = fogAt(120 + d, 110);
        UT_ASSERT_MSG(got > prev,
                      "%d squares out carries %u fog, no more than the %u "
                      "one square nearer", d, (unsigned)got, (unsigned)prev);
        UT_ASSERT_MSG(got < OVERVIEW_FOG_ALPHA,
                      "%d squares out is already fully fogged at %u, with the "
                      "ramp %d squares long", d, (unsigned)got,
                      OVERVIEW_FOG_RAMP);
        prev = got;
    }

    for (int d = OVERVIEW_FOG_RAMP; d <= OVERVIEW_FOG_RAMP + 2; d++) {
        BYTE got = fogAt(120 + d, 110);
        UT_ASSERT_MSG(got == OVERVIEW_FOG_ALPHA,
                      "%d squares out — past the %d square ramp — carries %u "
                      "fog, expected %u", d, OVERVIEW_FOG_RAMP,
                      (unsigned)got, (unsigned)OVERVIEW_FOG_ALPHA);
    }

    /* The same fade on all four sides, so the region does not drift. */
    UT_ASSERT_MSG(fogAt(99, 110) == fogAt(121, 110),
                  "left edge fades to %u where the right fades to %u",
                  (unsigned)fogAt(99, 110), (unsigned)fogAt(121, 110));
    UT_ASSERT_MSG(fogAt(110, 99) == fogAt(110, 121),
                  "top edge fades to %u where the bottom fades to %u",
                  (unsigned)fogAt(110, 99), (unsigned)fogAt(110, 121));
    return fogGuardIntact();
}

/* Diagonally out from a corner is further from the region than straight out
 * from an edge, so it is darker — which is what stops the lit area coming to
 * a square point at every corner. */
static int fog_corners_are_rounded(void) {
    OverviewRect live = { 100, 100, 120, 120 };

    fogBuild(&live, 1);

    BYTE corner = fogAt(122, 122); /* 2 out and 2 down: 2.83 squares away */
    BYTE edge   = fogAt(122, 110); /* 2 out: 2 squares away */
    UT_ASSERT_MSG(corner > edge,
                  "the diagonal off a corner carries %u fog, no more than the "
                  "%u the same distance along an edge does",
                  (unsigned)corner, (unsigned)edge);
    return fogGuardIntact();
}

/* Regions overlap — a pill's block over the tank's, or two pills' — and the
 * brightest answer has to win. A square another region has live must come out
 * clear however deep in this one's ramp it sits. */
static int fog_overlapping_regions_take_the_brightest(void) {
    OverviewRect live[2] = { { 100, 100, 120, 120 }, { 118, 118, 130, 130 } };

    fogBuild(live, ARRAY_LEN(live));

    UT_ASSERT_MSG(fogAt(121, 121) == 0,
                  "a square the second region has live carries %u fog from "
                  "sitting in the first's ramp", (unsigned)fogAt(121, 121));
    UT_ASSERT_MSG(fogAt(119, 119) == 0,
                  "a square both regions have live carries %u fog",
                  (unsigned)fogAt(119, 119));

    /* Between two regions, near either one is brighter than midway between
     * them: each square takes its distance from whichever is closer. */
    OverviewRect pair[2] = { { 100, 100, 120, 120 }, { 124, 100, 140, 120 } };
    fogBuild(pair, ARRAY_LEN(pair));

    UT_ASSERT_MSG(fogAt(121, 110) == fogAt(123, 110),
                  "the gap between two regions is lit unevenly: %u one square "
                  "from the left, %u one square from the right",
                  (unsigned)fogAt(121, 110), (unsigned)fogAt(123, 110));
    UT_ASSERT_MSG(fogAt(122, 110) > fogAt(121, 110),
                  "midway between two regions carries %u fog, no more than "
                  "the %u right beside one",
                  (unsigned)fogAt(122, 110), (unsigned)fogAt(121, 110));
    return fogGuardIntact();
}

/* A region against the map border keeps its brightness to the border rather
 * than fading against squares that are not there — and writes nothing past
 * the end of the mask getting there. */
static int fog_regions_clamp_to_the_map(void) {
    static const OverviewRect kCorners[] = {
        {   0,   0,  10,  10 },
        { 245, 245, 255, 255 },
        {   0, 245,  10, 255 },
        { 245,   0, 255,  10 }
    };

    for (int i = 0; i < ARRAY_LEN(kCorners); i++) {
        fogBuild(&kCorners[i], 1);

        UT_ASSERT_MSG(fogAt(kCorners[i].left, kCorners[i].top) == 0,
                      "corner region %d leaves its own top-left at %u fog",
                      i, (unsigned)fogAt(kCorners[i].left, kCorners[i].top));
        UT_ASSERT_MSG(fogAt(kCorners[i].right, kCorners[i].bottom) == 0,
                      "corner region %d leaves its own bottom-right at %u fog",
                      i,
                      (unsigned)fogAt(kCorners[i].right, kCorners[i].bottom));

        int rc = fogGuardIntact();
        if (rc) return rc;
    }
    return 0;
}

/* No regions at all is a real state — a dead tank holding no block and no
 * pill to see through — and fogs the whole map rather than clearing it. */
static int fog_no_regions_fogs_the_map(void) {
    OverviewRect live = { 100, 100, 120, 120 };

    fogBuild(&live, 0);
    UT_ASSERT_MSG(fogAt(110, 110) == OVERVIEW_FOG_ALPHA,
                  "with no regions the map carries %u fog, expected %u",
                  (unsigned)fogAt(110, 110), (unsigned)OVERVIEW_FOG_ALPHA);

    fogBuild(NULL, 1);
    UT_ASSERT_MSG(fogAt(110, 110) == OVERVIEW_FOG_ALPHA,
                  "with a NULL region list the map carries %u fog, expected %u",
                  (unsigned)fogAt(110, 110), (unsigned)OVERVIEW_FOG_ALPHA);

    /* And a NULL mask is a no-op, like the rest of the overview's maths. */
    overviewFogBuildMask(&live, 1, NULL);
    return fogGuardIntact();
}

extern "C" int run_overview_fog(void) {
    int rc;
    rc = fog_live_is_clear_and_far_is_fogged();      if (rc) return rc;
    rc = fog_ramps_out_of_the_region();              if (rc) return rc;
    rc = fog_corners_are_rounded();                  if (rc) return rc;
    rc = fog_overlapping_regions_take_the_brightest(); if (rc) return rc;
    rc = fog_regions_clamp_to_the_map();             if (rc) return rc;
    rc = fog_no_regions_fogs_the_map();              if (rc) return rc;
    return 0;
}
