/*
 * Tests for the map overview's fog mask (overview_fog.cpp) — the bytes the
 * renderer stretches over the map to dim everything outside a live region.
 * Nothing here touches SDL: rects in, bytes out.
 */

#include <cstring>

#include "test_harness.h"
#include "overview_fog.h"

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* OverviewRect literals below carry the trailing originX, originY explicitly.
   The fog builder never reads them, so they stay zero; naming them keeps the
   initialisers complete rather than relying on the tail being zero-filled. */

/* The mask plus a tail the builder has no business touching: every write it
 * makes is driven by a rect, and a rect at the map edge that failed to clamp
 * would run off the end of the last row into this. */
#define FOG_GUARD_BYTES 64
#define FOG_GUARD_FILL  0xA5

static BYTE fogBuf[OVERVIEW_FOG_MASK_BYTES + FOG_GUARD_BYTES];

/* What a square carries. One texel to a square, so this is the square's own
 * byte. */
static BYTE fogAt(int x, int y) {
    return fogBuf[(size_t)y * (size_t)OVERVIEW_FOG_MASK_SIDE + (size_t)x];
}

static void fogBuildDark(const OverviewRect *live, int liveCount,
                         const BYTE *dark) {
    memset(fogBuf, FOG_GUARD_FILL, sizeof(fogBuf));
    overviewFogBuildMask(live, liveCount, dark, fogBuf);
}

/* The regions alone, with no ground the player cannot see into — which is every
 * case written before line of sight existed, and every one with sight off. */
static void fogBuild(const OverviewRect *live, int liveCount) {
    fogBuildDark(live, liveCount, NULL);
}

/* The edge overview_fog.cpp draws, repeated here so the cases below can name
 * the exact byte a region at full alpha carries: clear on the squares it covers
 * and the whole of the fog on every other one. Deliberate duplication — it is
 * what turns "the edge did not soften" into an assertion rather than a shape
 * check, so a change to the edge has to be made twice. */
static BYTE fogFullAlphaAt(const OverviewRect *r, int x, int y) {
    bool inside = (x >= r->left && x <= r->right &&
                   y >= r->top  && y <= r->bottom);
    return inside ? (BYTE)0 : (BYTE)OVERVIEW_FOG_ALPHA;
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

/* A live square is clear, ground away from the region is fully fogged, and the
 * region's own edge square is clear too. */
static int fog_live_is_clear_and_far_is_fogged(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0, 0 };

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

/* Walking straight out from an edge the fog is at full in one square: the last
 * square the region covers is clear and the first square outside it carries
 * the whole of the fog, with nothing part way between them. The same step on
 * all four sides, so the lit area does not drift off the region. */
static int fog_steps_to_full_outside_the_region(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0, 0 };

    fogBuild(&live, 1);

    UT_ASSERT_MSG(fogAt(120, 110) == 0,
                  "the last square the region covers carries %u fog, expected "
                  "it clear", (unsigned)fogAt(120, 110));
    for (int d = 1; d <= 4; d++) {
        BYTE got = fogAt(120 + d, 110);
        UT_ASSERT_MSG(got == OVERVIEW_FOG_ALPHA,
                      "%d squares out of the region carries %u fog, expected "
                      "the full %u", d, (unsigned)got,
                      (unsigned)OVERVIEW_FOG_ALPHA);
    }

    UT_ASSERT_MSG(fogAt(100, 110) == 0 && fogAt(99, 110) == OVERVIEW_FOG_ALPHA,
                  "the left edge is %u inside and %u one square out, expected "
                  "0 then %u", (unsigned)fogAt(100, 110),
                  (unsigned)fogAt(99, 110), (unsigned)OVERVIEW_FOG_ALPHA);
    UT_ASSERT_MSG(fogAt(110, 100) == 0 && fogAt(110, 99) == OVERVIEW_FOG_ALPHA,
                  "the top edge is %u inside and %u one square out, expected "
                  "0 then %u", (unsigned)fogAt(110, 100),
                  (unsigned)fogAt(110, 99), (unsigned)OVERVIEW_FOG_ALPHA);
    UT_ASSERT_MSG(fogAt(110, 120) == 0 && fogAt(110, 121) == OVERVIEW_FOG_ALPHA,
                  "the bottom edge is %u inside and %u one square out, expected "
                  "0 then %u", (unsigned)fogAt(110, 120),
                  (unsigned)fogAt(110, 121), (unsigned)OVERVIEW_FOG_ALPHA);
    return fogGuardIntact();
}

/* A corner is as sharp as a side: the region's own corner square is clear, the
 * square diagonally off it is in full fog, and so is every other square in the
 * ring just outside the rect. Nothing outside the region is ever part lit,
 * whichever way it lies from it. */
static int fog_corners_are_as_sharp_as_the_sides(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0, 0 };

    fogBuild(&live, 1);

    UT_ASSERT_MSG(fogAt(120, 120) == 0,
                  "the region's corner square carries %u fog, expected clear",
                  (unsigned)fogAt(120, 120));
    UT_ASSERT_MSG(fogAt(121, 121) == OVERVIEW_FOG_ALPHA,
                  "the square diagonally off the corner carries %u fog, "
                  "expected the full %u", (unsigned)fogAt(121, 121),
                  (unsigned)OVERVIEW_FOG_ALPHA);

    for (int x = 99; x <= 121; x++) {
        UT_ASSERT_MSG(fogAt(x, 99) == OVERVIEW_FOG_ALPHA,
                      "square %d,99 in the ring above the region carries %u "
                      "fog, expected the full %u", x, (unsigned)fogAt(x, 99),
                      (unsigned)OVERVIEW_FOG_ALPHA);
        UT_ASSERT_MSG(fogAt(x, 121) == OVERVIEW_FOG_ALPHA,
                      "square %d,121 in the ring below the region carries %u "
                      "fog, expected the full %u", x, (unsigned)fogAt(x, 121),
                      (unsigned)OVERVIEW_FOG_ALPHA);
    }
    for (int y = 99; y <= 121; y++) {
        UT_ASSERT_MSG(fogAt(99, y) == OVERVIEW_FOG_ALPHA,
                      "square 99,%d in the ring left of the region carries %u "
                      "fog, expected the full %u", y, (unsigned)fogAt(99, y),
                      (unsigned)OVERVIEW_FOG_ALPHA);
        UT_ASSERT_MSG(fogAt(121, y) == OVERVIEW_FOG_ALPHA,
                      "square 121,%d in the ring right of the region carries "
                      "%u fog, expected the full %u", y,
                      (unsigned)fogAt(121, y), (unsigned)OVERVIEW_FOG_ALPHA);
    }
    return fogGuardIntact();
}

/* Regions overlap — a pill's block over the tank's, or two pills' — and the
 * brightest answer has to win. A square another region has live must come out
 * clear however far outside this one it sits. */
static int fog_overlapping_regions_take_the_brightest(void) {
    OverviewRect live[2] = { { 100, 100, 120, 120, 255, 0, 0 },
                             { 118, 118, 130, 130, 255, 0, 0 } };

    fogBuild(live, ARRAY_LEN(live));

    UT_ASSERT_MSG(fogAt(121, 121) == 0,
                  "a square the second region has live carries %u fog from "
                  "sitting outside the first", (unsigned)fogAt(121, 121));
    UT_ASSERT_MSG(fogAt(119, 119) == 0,
                  "a square both regions have live carries %u fog",
                  (unsigned)fogAt(119, 119));

    /* Two regions with ground between them: each is clear to its own edge and
     * the gap is in full fog, rather than the two running into one lit patch. */
    OverviewRect pair[2] = { { 100, 100, 120, 120, 255, 0, 0 },
                             { 124, 100, 140, 120, 255, 0, 0 } };
    fogBuild(pair, ARRAY_LEN(pair));

    UT_ASSERT_MSG(fogAt(120, 110) == 0 && fogAt(124, 110) == 0,
                  "the two regions' facing edges carry %u and %u fog, expected "
                  "both clear", (unsigned)fogAt(120, 110),
                  (unsigned)fogAt(124, 110));
    for (int x = 121; x <= 123; x++) {
        UT_ASSERT_MSG(fogAt(x, 110) == OVERVIEW_FOG_ALPHA,
                      "square %d,110 in the gap between two regions carries %u "
                      "fog, expected the full %u", x, (unsigned)fogAt(x, 110),
                      (unsigned)OVERVIEW_FOG_ALPHA);
    }
    return fogGuardIntact();
}

/* A region against the map border keeps its brightness to the border rather
 * than fading against squares that are not there — and writes nothing past
 * the end of the mask getting there. */
static int fog_regions_clamp_to_the_map(void) {
    static const OverviewRect kCorners[] = {
        { 0, 0, 10, 10, 255, 0, 0 },
        { 245, 245, 255, 255, 255, 0, 0 },
        { 0, 245, 10, 255, 255, 0, 0 },
        { 245, 0, 255, 10, 255, 0, 0 }
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
    OverviewRect live = { 100, 100, 120, 120, 255, 0, 0 };

    fogBuild(&live, 0);
    UT_ASSERT_MSG(fogAt(110, 110) == OVERVIEW_FOG_ALPHA,
                  "with no regions the map carries %u fog, expected %u",
                  (unsigned)fogAt(110, 110), (unsigned)OVERVIEW_FOG_ALPHA);

    fogBuild(NULL, 1);
    UT_ASSERT_MSG(fogAt(110, 110) == OVERVIEW_FOG_ALPHA,
                  "with a NULL region list the map carries %u fog, expected %u",
                  (unsigned)fogAt(110, 110), (unsigned)OVERVIEW_FOG_ALPHA);

    /* And a NULL mask is a no-op, like the rest of the overview's maths. */
    overviewFogBuildMask(&live, 1, NULL, NULL);
    return fogGuardIntact();
}

/* A region at full alpha is one the player holds outright: every square it
 * covers is clear, corners and edges included, and the ground immediately
 * outside it is in full fog on all four sides. */
static int fog_full_alpha_is_clear_inside_and_fogged_outside(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0, 0 };

    fogBuild(&live, 1);

    for (int y = live.top; y <= live.bottom; y++) {
        for (int x = live.left; x <= live.right; x++) {
            UT_ASSERT_MSG(fogAt(x, y) == 0,
                          "square %d,%d inside a full-alpha region carries %u "
                          "fog, expected it clear", x, y,
                          (unsigned)fogAt(x, y));
        }
    }

    static const int kOutside[4][2] = {
        { 99, 110 }, { 121, 110 }, { 110, 99 }, { 110, 121 }
    };
    for (int i = 0; i < ARRAY_LEN(kOutside); i++) {
        BYTE got = fogAt(kOutside[i][0], kOutside[i][1]);
        UT_ASSERT_MSG(got == OVERVIEW_FOG_ALPHA,
                      "square %d,%d just outside a full-alpha region carries "
                      "%u fog, expected the full %u", kOutside[i][0],
                      kOutside[i][1], (unsigned)got,
                      (unsigned)OVERVIEW_FOG_ALPHA);
    }
    return fogGuardIntact();
}

/* The other end of a fade: a region at alpha 0 has run its decay window out
 * and has to leave the map exactly as it found it — the block is gone rather
 * than sitting there at one byte under full fog. */
static int fog_faded_out_region_is_not_there(void) {
    OverviewRect live = { 100, 100, 120, 120, 0, 0, 0 };

    fogBuild(&live, 1);

    for (int i = 0; i < OVERVIEW_FOG_MASK_BYTES; i++) {
        UT_ASSERT_MSG(fogBuf[i] == OVERVIEW_FOG_ALPHA,
                      "square %d carries %u fog under a region faded to 0, "
                      "expected %u", i, (unsigned)fogBuf[i],
                      (unsigned)OVERVIEW_FOG_ALPHA);
    }
    return fogGuardIntact();
}

/* Mid-fade the whole block dims together: every square the region covers takes
 * the same part-way value, the ground outside it is in full fog as it is round
 * any region, and a region still at full alpha over the top wins. */
static int fog_mid_fade_dims_the_whole_block(void) {
    OverviewRect live = { 100, 100, 120, 120, 128, 0, 0 };

    fogBuild(&live, 1);

    BYTE inside = fogAt(110, 110);
    UT_ASSERT_MSG(inside > 0 && inside < OVERVIEW_FOG_ALPHA,
                  "a half-faded region's interior carries %u fog, expected "
                  "between 0 and %u", (unsigned)inside,
                  (unsigned)OVERVIEW_FOG_ALPHA);

    /* The edge is as bright as the middle: the fade dims the block evenly
     * rather than eating into it from the outside. */
    UT_ASSERT_MSG(fogAt(120, 120) == inside,
                  "the region's corner carries %u fog where its middle carries "
                  "%u", (unsigned)fogAt(120, 120), (unsigned)inside);
    UT_ASSERT_MSG(fogAt(100, 100) == inside,
                  "the region's top-left carries %u fog where its middle "
                  "carries %u", (unsigned)fogAt(100, 100), (unsigned)inside);

    BYTE oneOut = fogAt(121, 110);
    UT_ASSERT_MSG(oneOut == OVERVIEW_FOG_ALPHA,
                  "one square out of a half-faded region carries %u fog, "
                  "expected the full %u past the block's %u",
                  (unsigned)oneOut, (unsigned)OVERVIEW_FOG_ALPHA,
                  (unsigned)inside);

    /* A pill going dark under the tank's block must not dim it. */
    OverviewRect pair[2] = { { 100, 100, 120, 120, 128, 0, 0 },
                             { 110, 110, 114, 114, 255, 0, 0 } };
    fogBuild(pair, ARRAY_LEN(pair));

    UT_ASSERT_MSG(fogAt(112, 112) == 0,
                  "a square a full-alpha region has live carries %u fog from "
                  "the half-faded region over it", (unsigned)fogAt(112, 112));
    UT_ASSERT_MSG(fogAt(105, 105) == inside,
                  "a square only the half-faded region covers carries %u fog, "
                  "expected the block's %u",
                  (unsigned)fogAt(105, 105), (unsigned)inside);
    return fogGuardIntact();
}

/* Half alpha lifts a square half as far out of the fog as full alpha does.
 * This is the whole of what puts a region part way through its decay fade at
 * half fog: it carries alpha 128 and the builder needs nothing of its own to
 * read it that way. Full alpha is checked in the same case, so a change that
 * moved the half-lit one would have to move the block the map has always drawn
 * with it to pass. */
static int fog_half_alpha_lifts_half_way(void) {
    OverviewRect full = { 100, 100, 120, 120, 255, 0, 0 };
    OverviewRect half = { 100, 100, 120, 120, 128, 0, 0 };

    fogBuild(&full, 1);
    BYTE fullFog = fogAt(110, 110);
    UT_ASSERT_MSG(fullFog == 0,
                  "a full-alpha region's interior carries %u fog, expected "
                  "clear", (unsigned)fullFog);

    fogBuild(&half, 1);
    BYTE halfFog = fogAt(110, 110);

    int fullLift = OVERVIEW_FOG_ALPHA - (int)fullFog;
    int halfLift = OVERVIEW_FOG_ALPHA - (int)halfFog;
    UT_ASSERT_MSG(halfLift * 2 >= fullLift - 2 && halfLift * 2 <= fullLift + 2,
                  "alpha 128 lifts a square %d out of the fog where alpha 255 "
                  "lifts it %d, expected half of it to within rounding",
                  halfLift, fullLift);
    return fogGuardIntact();
}

/* A decay window only ever runs one way, so the block only ever gets darker:
 * dropping alpha a step must never brighten a square. */
static int fog_lower_alpha_never_brightens(void) {
    BYTE prev = 0;

    for (int a = 255; a >= 0; a--) {
        OverviewRect live = { 100, 100, 120, 120, (BYTE)a, 0, 0 };

        fogBuild(&live, 1);

        BYTE got = fogAt(110, 110);
        UT_ASSERT_MSG(got >= prev,
                      "alpha %d carries %u fog, less than the %u one alpha "
                      "brighter", a, (unsigned)got, (unsigned)prev);
        prev = got;
    }

    UT_ASSERT_MSG(prev == OVERVIEW_FOG_ALPHA,
                  "the fade ends at %u fog, expected the full %u",
                  (unsigned)prev, (unsigned)OVERVIEW_FOG_ALPHA);
    return fogGuardIntact();
}

/* Every byte of the mask one full-alpha region draws, worked out square by
 * square rather than read back off the builder. A region at full alpha lifts
 * each square it covers the whole way out of the fog, and there is no square
 * outside it the region touches at all. */
static int fogAssertPlainMask(const OverviewRect *r) {
    for (int y = 0; y < OVERVIEW_FOG_MASK_SIDE; y++) {
        for (int x = 0; x < OVERVIEW_FOG_MASK_SIDE; x++) {
            BYTE want = fogFullAlphaAt(r, x, y);
            BYTE got  = fogBuf[(size_t)y * OVERVIEW_FOG_MASK_SIDE + (size_t)x];
            UT_ASSERT_MSG(got == want,
                          "square %d,%d carries %u fog, expected %u",
                          x, y, (unsigned)got, (unsigned)want);
        }
    }
    return 0;
}

/* Ground the player cannot see into, the shape line of sight hands the builder:
 * full fog on the one square a case is about and nothing anywhere else. */
static BYTE fogDarkBuf[OVERVIEW_FOG_SQUARE_BYTES];

static const BYTE *fogDarkOne(int x, int y, BYTE value) {
    memset(fogDarkBuf, 0, sizeof(fogDarkBuf));
    fogDarkBuf[(size_t)y * MAP_ARRAY_SIZE + (size_t)x] = value;
    return fogDarkBuf;
}

/* A square behind a building sits inside the block, so the regions leave it
 * clear and something has to put the fog back over it. This is that: the
 * darkest answer wins, over a region at full alpha included. */
static int fog_dark_wins_over_a_region(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0, 0 };

    fogBuild(&live, 1);
    UT_ASSERT_MSG(fogAt(110, 110) == 0,
                  "a square the region holds carries %u fog before any of it",
                  (unsigned)fogAt(110, 110));

    fogBuildDark(&live, 1, fogDarkOne(110, 110, OVERVIEW_FOG_ALPHA));
    UT_ASSERT_MSG(fogAt(110, 110) == OVERVIEW_FOG_ALPHA,
                  "a hidden square the region holds carries %u fog, expected "
                  "the full %u", (unsigned)fogAt(110, 110),
                  (unsigned)OVERVIEW_FOG_ALPHA);
    UT_ASSERT_MSG(fogAt(111, 110) == 0,
                  "the square beside a hidden one carries %u fog, expected it "
                  "clear", (unsigned)fogAt(111, 110));

    /* Half way is half way here too, so a partly dark square is not simply
     * on or off. */
    fogBuildDark(&live, 1, fogDarkOne(110, 110, 64));
    UT_ASSERT_MSG(fogAt(110, 110) == 64,
                  "a square handed 64 of fog carries %u",
                  (unsigned)fogAt(110, 110));
    return fogGuardIntact();
}

/* Nothing hidden is the map the regions alone draw, byte for byte — which is
 * what says sight being off costs the picture nothing. */
static int fog_no_dark_is_the_mask_without_it(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0, 0 };
    int rc; /* Result of the whole-mask check */

    fogBuildDark(&live, 1, NULL);
    rc = fogAssertPlainMask(&live);
    if (rc) return rc;

    /* Ground that is hidden nowhere says nothing either. */
    memset(fogDarkBuf, 0, sizeof(fogDarkBuf));
    fogBuildDark(&live, 1, fogDarkBuf);
    rc = fogAssertPlainMask(&live);
    if (rc) return rc;

    return fogGuardIntact();
}

extern "C" int run_overview_fog(void) {
    int rc;
    rc = fog_live_is_clear_and_far_is_fogged();      if (rc) return rc;
    rc = fog_steps_to_full_outside_the_region();     if (rc) return rc;
    rc = fog_corners_are_as_sharp_as_the_sides();    if (rc) return rc;
    rc = fog_overlapping_regions_take_the_brightest(); if (rc) return rc;
    rc = fog_regions_clamp_to_the_map();             if (rc) return rc;
    rc = fog_no_regions_fogs_the_map();              if (rc) return rc;
    rc = fog_full_alpha_is_clear_inside_and_fogged_outside(); if (rc) return rc;
    rc = fog_faded_out_region_is_not_there();        if (rc) return rc;
    rc = fog_mid_fade_dims_the_whole_block();        if (rc) return rc;
    rc = fog_half_alpha_lifts_half_way();            if (rc) return rc;
    rc = fog_lower_alpha_never_brightens();          if (rc) return rc;
    rc = fog_dark_wins_over_a_region();              if (rc) return rc;
    rc = fog_no_dark_is_the_mask_without_it();       if (rc) return rc;
    return 0;
}
