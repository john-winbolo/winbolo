/*
 * Tests for the map overview's fog mask (overview_fog.cpp) — the bytes the
 * renderer stretches over the map to dim everything outside a live region.
 * Nothing here touches SDL: rects in, bytes out.
 */

#include <cmath>
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

static void fogBuildDark(const OverviewRect *live, int liveCount,
                         const BYTE *lift, const BYTE *dark) {
    memset(fogBuf, FOG_GUARD_FILL, sizeof(fogBuf));
    overviewFogBuildMask(live, liveCount, lift, dark, fogBuf);
}

/* The regions and a lift, with no ground the player cannot see into — which is
 * every case written before line of sight existed and every experiment with
 * the toggle off. */
static void fogBuild(const OverviewRect *live, int liveCount,
                     const BYTE *lift) {
    fogBuildDark(live, liveCount, lift, NULL);
}

/* The ramp overview_fog.cpp builds, repeated here so the cases below can name
 * the exact byte a region at full alpha carries `d` squares out. Deliberate
 * duplication: it is what turns "the fade did not change" into an assertion
 * rather than a shape check, so a change to the ramp has to be made twice. */
static BYTE fogRampRef(float d) {
    float t = d / (float)OVERVIEW_FOG_RAMP;
    float s = t * t * (3.0f - 2.0f * t);
    return (BYTE)lroundf((float)OVERVIEW_FOG_ALPHA * s);
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
    OverviewRect live = { 100, 100, 120, 120, 255, 0 };

    fogBuild(&live, 1, NULL);

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
    OverviewRect live = { 100, 100, 120, 120, 255, 0 };

    fogBuild(&live, 1, NULL);

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
    OverviewRect live = { 100, 100, 120, 120, 255, 0 };

    fogBuild(&live, 1, NULL);

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
    OverviewRect live[2] = { { 100, 100, 120, 120, 255, 0 },
                             { 118, 118, 130, 130, 255, 0 } };

    fogBuild(live, ARRAY_LEN(live), NULL);

    UT_ASSERT_MSG(fogAt(121, 121) == 0,
                  "a square the second region has live carries %u fog from "
                  "sitting in the first's ramp", (unsigned)fogAt(121, 121));
    UT_ASSERT_MSG(fogAt(119, 119) == 0,
                  "a square both regions have live carries %u fog",
                  (unsigned)fogAt(119, 119));

    /* Between two regions, near either one is brighter than midway between
     * them: each square takes its distance from whichever is closer. */
    OverviewRect pair[2] = { { 100, 100, 120, 120, 255, 0 },
                             { 124, 100, 140, 120, 255, 0 } };
    fogBuild(pair, ARRAY_LEN(pair), NULL);

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
        {   0,   0,  10,  10, 255, 0 },
        { 245, 245, 255, 255, 255, 0 },
        {   0, 245,  10, 255, 255, 0 },
        { 245,   0, 255,  10, 255, 0 }
    };

    for (int i = 0; i < ARRAY_LEN(kCorners); i++) {
        fogBuild(&kCorners[i], 1, NULL);

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
    OverviewRect live = { 100, 100, 120, 120, 255, 0 };

    fogBuild(&live, 0, NULL);
    UT_ASSERT_MSG(fogAt(110, 110) == OVERVIEW_FOG_ALPHA,
                  "with no regions the map carries %u fog, expected %u",
                  (unsigned)fogAt(110, 110), (unsigned)OVERVIEW_FOG_ALPHA);

    fogBuild(NULL, 1, NULL);
    UT_ASSERT_MSG(fogAt(110, 110) == OVERVIEW_FOG_ALPHA,
                  "with a NULL region list the map carries %u fog, expected %u",
                  (unsigned)fogAt(110, 110), (unsigned)OVERVIEW_FOG_ALPHA);

    /* And a NULL mask is a no-op, like the rest of the overview's maths. */
    overviewFogBuildMask(&live, 1, NULL, NULL, NULL);
    return fogGuardIntact();
}

/* A region at full alpha is one the player holds outright, and carries the
 * ramp byte for byte — the mask a region that cannot fade has always drawn. */
static int fog_full_alpha_carries_the_plain_ramp(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0 };

    fogBuild(&live, 1, NULL);

    UT_ASSERT_MSG(fogAt(110, 110) == 0,
                  "a full-alpha region's interior carries %u fog",
                  (unsigned)fogAt(110, 110));
    for (int d = 0; d < OVERVIEW_FOG_RAMP; d++) {
        BYTE want = fogRampRef((float)d);
        BYTE got  = fogAt(120 + d, 110);
        UT_ASSERT_MSG(got == want,
                      "%d squares out of a full-alpha region carries %u fog, "
                      "expected the plain ramp's %u",
                      d, (unsigned)got, (unsigned)want);
    }
    UT_ASSERT_MSG(fogAt(120 + OVERVIEW_FOG_RAMP, 110) == OVERVIEW_FOG_ALPHA,
                  "the square past the ramp carries %u fog, expected %u",
                  (unsigned)fogAt(120 + OVERVIEW_FOG_RAMP, 110),
                  (unsigned)OVERVIEW_FOG_ALPHA);
    return fogGuardIntact();
}

/* The other end of a fade: a region at alpha 0 has run its decay window out
 * and has to leave the map exactly as it found it, ramp included — the block
 * is gone rather than sitting there at one byte under full fog. */
static int fog_faded_out_region_is_not_there(void) {
    OverviewRect live = { 100, 100, 120, 120, 0, 0 };

    fogBuild(&live, 1, NULL);

    for (int i = 0; i < OVERVIEW_FOG_MASK_BYTES; i++) {
        UT_ASSERT_MSG(fogBuf[i] == OVERVIEW_FOG_ALPHA,
                      "square %d carries %u fog under a region faded to 0, "
                      "expected %u", i, (unsigned)fogBuf[i],
                      (unsigned)OVERVIEW_FOG_ALPHA);
    }
    return fogGuardIntact();
}

/* Mid-fade the whole block dims together: every square the region covers takes
 * the same part-way value, the ramp still climbs out of that value rather than
 * out of clear, and a region still at full alpha over the top wins. */
static int fog_mid_fade_dims_the_whole_block(void) {
    OverviewRect live = { 100, 100, 120, 120, 128, 0 };

    fogBuild(&live, 1, NULL);

    BYTE inside = fogAt(110, 110);
    UT_ASSERT_MSG(inside > 0 && inside < OVERVIEW_FOG_ALPHA,
                  "a half-faded region's interior carries %u fog, expected "
                  "between 0 and %u", (unsigned)inside,
                  (unsigned)OVERVIEW_FOG_ALPHA);

    /* The edge is as bright as the middle: the fade dims the block, and the
     * ramp starts from whatever brightness the block is left with. */
    UT_ASSERT_MSG(fogAt(120, 120) == inside,
                  "the region's corner carries %u fog where its middle carries "
                  "%u", (unsigned)fogAt(120, 120), (unsigned)inside);
    UT_ASSERT_MSG(fogAt(100, 100) == inside,
                  "the region's top-left carries %u fog where its middle "
                  "carries %u", (unsigned)fogAt(100, 100), (unsigned)inside);

    BYTE oneOut = fogAt(121, 110);
    UT_ASSERT_MSG(oneOut > inside && oneOut < OVERVIEW_FOG_ALPHA,
                  "one square out of a half-faded region carries %u fog, "
                  "expected between the block's %u and %u",
                  (unsigned)oneOut, (unsigned)inside,
                  (unsigned)OVERVIEW_FOG_ALPHA);
    UT_ASSERT_MSG(fogAt(120 + OVERVIEW_FOG_RAMP, 110) == OVERVIEW_FOG_ALPHA,
                  "the square past a half-faded region's ramp carries %u fog, "
                  "expected %u", (unsigned)fogAt(120 + OVERVIEW_FOG_RAMP, 110),
                  (unsigned)OVERVIEW_FOG_ALPHA);

    /* A pill going dark under the tank's block must not dim it. */
    OverviewRect pair[2] = { { 100, 100, 120, 120, 128, 0 },
                             { 110, 110, 114, 114, 255, 0 } };
    fogBuild(pair, ARRAY_LEN(pair), NULL);

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
 * This is the whole of what puts the halo at half fog: it carries alpha 128
 * and the builder needs nothing of its own to read it that way. Full alpha is
 * checked in the same case, so a change that moved the halo would have to move
 * the block the map has always drawn with it to pass. */
static int fog_half_alpha_lifts_half_way(void) {
    OverviewRect full = { 100, 100, 120, 120, 255, 0 };
    OverviewRect half = { 100, 100, 120, 120, 128, 0 };

    fogBuild(&full, 1, NULL);
    BYTE fullFog = fogAt(110, 110);
    UT_ASSERT_MSG(fullFog == 0,
                  "a full-alpha region's interior carries %u fog, expected "
                  "clear", (unsigned)fullFog);

    fogBuild(&half, 1, NULL);
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
        OverviewRect live = { 100, 100, 120, 120, (BYTE)a, 0 };

        fogBuild(&live, 1, NULL);

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

/* A per-square lift, the shape the map's own fade hands the builder: nothing
 * anywhere except the one square a case is about, so what comes back says what
 * that square's lift did and nothing else. */
static BYTE fogLiftBuf[OVERVIEW_FOG_MASK_BYTES];

static const BYTE *fogLiftOne(int x, int y, BYTE value) {
    memset(fogLiftBuf, 0, sizeof(fogLiftBuf));
    fogLiftBuf[(size_t)y * MAP_ARRAY_SIZE + (size_t)x] = value;
    return fogLiftBuf;
}

/* Every byte of the mask one full-alpha region draws, worked out square by
 * square from the ramp rather than read back off the builder. A region at full
 * alpha lifts each square the whole way, so what is left is the ramp value at
 * the distance to the rect, and full fog past the end of the ramp. */
static int fogAssertPlainMask(const OverviewRect *r) {
    for (int y = 0; y < MAP_ARRAY_SIZE; y++) {
        for (int x = 0; x < MAP_ARRAY_SIZE; x++) {
            int dx = 0;
            int dy = 0;

            if (x < r->left) {
                dx = r->left - x;
            } else if (x > r->right) {
                dx = x - r->right;
            }
            if (y < r->top) {
                dy = r->top - y;
            } else if (y > r->bottom) {
                dy = y - r->bottom;
            }

            float d = sqrtf((float)(dx * dx + dy * dy));
            BYTE want = (d >= (float)OVERVIEW_FOG_RAMP)
                            ? (BYTE)OVERVIEW_FOG_ALPHA
                            : fogRampRef(d);
            UT_ASSERT_MSG(fogAt(x, y) == want,
                          "square %d,%d carries %u fog, expected %u",
                          x, y, (unsigned)fogAt(x, y), (unsigned)want);
        }
    }
    return 0;
}

/* With no per-square lift the mask is the one the regions alone draw, every
 * byte of it — which is what says the experiments that hand over no lift are
 * drawing the map they have always drawn. */
static int fog_no_lift_is_the_mask_the_regions_draw(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0 };
    int rc; /* Result of the whole-mask check */

    fogBuild(&live, 1, NULL);
    rc = fogAssertPlainMask(&live);
    if (rc) return rc;

    /* A lift that lifts nothing says nothing either, byte for byte. */
    memset(fogLiftBuf, 0, sizeof(fogLiftBuf));
    fogBuild(&live, 1, fogLiftBuf);
    rc = fogAssertPlainMask(&live);
    if (rc) return rc;

    return fogGuardIntact();
}

/* A square no region covers is in full fog until its lift says otherwise: at
 * 255 it comes out as clear as a live one. It is the square's own lift and
 * nothing else's, so the square beside it is untouched — the ramp belongs to
 * the regions, and ground that is fading is lit one square at a time. */
static int fog_full_lift_clears_ground_no_region_covers(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0 };

    fogBuild(&live, 1, NULL);
    UT_ASSERT_MSG(fogAt(200, 200) == OVERVIEW_FOG_ALPHA,
                  "ground away from the region carries %u fog before any lift, "
                  "expected %u", (unsigned)fogAt(200, 200),
                  (unsigned)OVERVIEW_FOG_ALPHA);

    fogBuild(&live, 1, fogLiftOne(200, 200, 255));
    UT_ASSERT_MSG(fogAt(200, 200) == 0,
                  "a square lifted the whole way carries %u fog, expected it "
                  "clear", (unsigned)fogAt(200, 200));
    UT_ASSERT_MSG(fogAt(201, 200) == OVERVIEW_FOG_ALPHA,
                  "the square beside a lifted one carries %u fog, expected the "
                  "full %u", (unsigned)fogAt(201, 200),
                  (unsigned)OVERVIEW_FOG_ALPHA);
    return fogGuardIntact();
}

/* Half way through its fade a square is half way out of the fog, which is what
 * makes the ground dissolve rather than switch off. */
static int fog_half_lift_leaves_the_square_half_way(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0 };

    fogBuild(&live, 1, fogLiftOne(200, 200, 128));

    int halfLift = OVERVIEW_FOG_ALPHA - (int)fogAt(200, 200);
    UT_ASSERT_MSG(halfLift * 2 >= OVERVIEW_FOG_ALPHA - 2 &&
                      halfLift * 2 <= OVERVIEW_FOG_ALPHA + 2,
                  "a lift of 128 takes a square %d out of the fog, expected "
                  "half of %u to within rounding",
                  halfLift, (unsigned)OVERVIEW_FOG_ALPHA);
    return fogGuardIntact();
}

/* The brightest answer wins here as it does between regions: a square a region
 * already holds cannot be darkened by whatever its lift happens to say. */
static int fog_lift_never_darkens_a_square(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0 };
    static const BYTE kLifts[] = { 0, 1, 10, 128, 254, 255 };

    for (int i = 0; i < ARRAY_LEN(kLifts); i++) {
        fogBuild(&live, 1, fogLiftOne(110, 110, kLifts[i]));
        UT_ASSERT_MSG(fogAt(110, 110) == 0,
                      "a square the region has live carries %u fog under a "
                      "lift of %u, expected clear",
                      (unsigned)fogAt(110, 110), (unsigned)kLifts[i]);
    }

    /* And part way up the ramp, where the region leaves the square dimmer than
     * clear but brighter than a small lift would. */
    fogBuild(&live, 1, NULL);
    BYTE plain = fogAt(121, 110);
    fogBuild(&live, 1, fogLiftOne(121, 110, 1));
    UT_ASSERT_MSG(fogAt(121, 110) == plain,
                  "a square the ramp leaves at %u fog is at %u under a lift of "
                  "1, which is darker than the ramp had it",
                  (unsigned)plain, (unsigned)fogAt(121, 110));
    return fogGuardIntact();
}

/* Ground the player cannot see into, the shape line of sight hands the builder:
 * full fog on the one square a case is about and nothing anywhere else. */
static BYTE fogDarkBuf[OVERVIEW_FOG_MASK_BYTES];

static const BYTE *fogDarkOne(int x, int y, BYTE value) {
    memset(fogDarkBuf, 0, sizeof(fogDarkBuf));
    fogDarkBuf[(size_t)y * MAP_ARRAY_SIZE + (size_t)x] = value;
    return fogDarkBuf;
}

/* A square behind a building sits inside the block, so the regions leave it
 * clear and something has to put the fog back over it. This is that: the
 * darkest answer wins, over a region and over a fade alike. */
static int fog_dark_wins_over_a_region_and_a_lift(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0 };

    fogBuild(&live, 1, NULL);
    UT_ASSERT_MSG(fogAt(110, 110) == 0,
                  "a square the region holds carries %u fog before any of it",
                  (unsigned)fogAt(110, 110));

    fogBuildDark(&live, 1, NULL, fogDarkOne(110, 110, OVERVIEW_FOG_ALPHA));
    UT_ASSERT_MSG(fogAt(110, 110) == OVERVIEW_FOG_ALPHA,
                  "a hidden square the region holds carries %u fog, expected "
                  "the full %u", (unsigned)fogAt(110, 110),
                  (unsigned)OVERVIEW_FOG_ALPHA);
    UT_ASSERT_MSG(fogAt(111, 110) == 0,
                  "the square beside a hidden one carries %u fog, expected it "
                  "clear", (unsigned)fogAt(111, 110));

    /* And over a fade, which is the same square lit from the other side. */
    fogBuildDark(&live, 1, fogLiftOne(110, 110, 255),
                 fogDarkOne(110, 110, OVERVIEW_FOG_ALPHA));
    UT_ASSERT_MSG(fogAt(110, 110) == OVERVIEW_FOG_ALPHA,
                  "a hidden square carries %u fog under a lift of 255, "
                  "expected the full %u", (unsigned)fogAt(110, 110),
                  (unsigned)OVERVIEW_FOG_ALPHA);

    /* Half way is half way here too, so a partly dark square is not simply
     * on or off. */
    fogBuildDark(&live, 1, NULL, fogDarkOne(110, 110, 64));
    UT_ASSERT_MSG(fogAt(110, 110) == 64,
                  "a square handed 64 of fog carries %u",
                  (unsigned)fogAt(110, 110));
    return fogGuardIntact();
}

/* Nothing hidden is the map the regions and the fade alone draw, byte for
 * byte — which is what says the toggle being off costs the picture nothing. */
static int fog_no_dark_is_the_mask_without_it(void) {
    OverviewRect live = { 100, 100, 120, 120, 255, 0 };
    int rc; /* Result of the whole-mask check */

    fogBuildDark(&live, 1, NULL, NULL);
    rc = fogAssertPlainMask(&live);
    if (rc) return rc;

    /* Ground that is hidden nowhere says nothing either. */
    memset(fogDarkBuf, 0, sizeof(fogDarkBuf));
    fogBuildDark(&live, 1, NULL, fogDarkBuf);
    rc = fogAssertPlainMask(&live);
    if (rc) return rc;

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
    rc = fog_full_alpha_carries_the_plain_ramp();    if (rc) return rc;
    rc = fog_faded_out_region_is_not_there();        if (rc) return rc;
    rc = fog_mid_fade_dims_the_whole_block();        if (rc) return rc;
    rc = fog_half_alpha_lifts_half_way();            if (rc) return rc;
    rc = fog_lower_alpha_never_brightens();          if (rc) return rc;
    rc = fog_no_lift_is_the_mask_the_regions_draw(); if (rc) return rc;
    rc = fog_full_lift_clears_ground_no_region_covers(); if (rc) return rc;
    rc = fog_half_lift_leaves_the_square_half_way(); if (rc) return rc;
    rc = fog_lift_never_darkens_a_square();          if (rc) return rc;
    rc = fog_dark_wins_over_a_region_and_a_lift();   if (rc) return rc;
    rc = fog_no_dark_is_the_mask_without_it();       if (rc) return rc;
    return 0;
}
