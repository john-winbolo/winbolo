/*
 * Tests for the fog of war look the player picks (fog_look.h), and for the
 * fog line band the Darker with fog edge look draws (fog_roads.h).
 *
 * Two pure pieces, no SDL and no renderer:
 *
 *   - the look table: which colour each of the four styles washes towards,
 *     which of them washes at all, which draws the fog edge, and that the
 *     setting store clamps a value from outside the enum back to the
 *     default the game had before the setting existed;
 *   - the edge mask: which sides of a square get a band, given the fog and
 *     the tiles round it.
 *
 * Which terrain is banded is the build switch FOG_EDGE_ALL_TERRAIN, not a
 * value the test can set: it is read at compile time by an inline function.
 * So the terrain cases below are written for both settings of it and the
 * build decides which half runs. The game is built with 1 — every fogged
 * square at the fog line is banded — and the 0 half is what keeps the roads
 * only build honest for whoever flips the switch back.
 */

#include <string.h>

#include "test_harness.h"

#include "global.h" /* GRASS — a terrain number doubling as a tile number */
#include "fog_look.h"
#include "fog_roads.h"
#include "gfx_settings.h"

/* ------------------------------------------------------------------ */
/* The look table                                                     */
/* ------------------------------------------------------------------ */

int run_fog_style_looks(void) {
    unsigned char r = 1, g = 2, b = 3;

    /* Grey is 0, so a prefs file that has never carried the key reads as the
     * look the game already drew. */
    UT_ASSERT_MSG((int)FOG_STYLE_GREY == 0, "Grey must be style 0, is %d",
                  (int)FOG_STYLE_GREY);

    UT_ASSERT_MSG(fogLookColour(FOG_STYLE_GREY, &r, &g, &b) != 0,
                  "Grey has to wash");
    UT_ASSERT_MSG(r == FOG_LOOK_R && g == FOG_LOOK_G && b == FOG_LOOK_B,
                  "Grey washes towards %d,%d,%d, expected %d,%d,%d",
                  (int)r, (int)g, (int)b, FOG_LOOK_R, FOG_LOOK_G, FOG_LOOK_B);

    /* Darker is the dimming the grey replaced: a wash to black. */
    r = g = b = 200;
    UT_ASSERT_MSG(fogLookColour(FOG_STYLE_DARK, &r, &g, &b) != 0,
                  "Darker has to wash");
    UT_ASSERT_MSG(r == 0 && g == 0 && b == 0,
                  "Darker washes towards %d,%d,%d, expected black",
                  (int)r, (int)g, (int)b);

    /* Darker with fog edge is the same wash. Only the band is added. */
    r = g = b = 200;
    UT_ASSERT_MSG(fogLookColour(FOG_STYLE_DARK_EDGE, &r, &g, &b) != 0,
                  "Darker with fog edge has to wash");
    UT_ASSERT_MSG(r == 0 && g == 0 && b == 0,
                  "Darker with fog edge washes towards %d,%d,%d, expected "
                  "black", (int)r, (int)g, (int)b);

    /* None draws nothing, and leaves the caller's bytes where they were. */
    r = 11; g = 22; b = 33;
    UT_ASSERT_MSG(fogLookColour(FOG_STYLE_NONE, &r, &g, &b) == 0,
                  "None must not wash");
    UT_ASSERT_MSG(r == 11 && g == 22 && b == 33,
                  "None wrote a colour: %d,%d,%d", (int)r, (int)g, (int)b);

    /* Only the one look draws the band. */
    UT_ASSERT_MSG(fogLookDrawsFogEdge(FOG_STYLE_DARK_EDGE) != 0,
                  "Darker with fog edge has to draw the band");
    UT_ASSERT_MSG(fogLookDrawsFogEdge(FOG_STYLE_GREY) == 0 &&
                      fogLookDrawsFogEdge(FOG_STYLE_DARK) == 0 &&
                      fogLookDrawsFogEdge(FOG_STYLE_NONE) == 0,
                  "Only Darker with fog edge draws the band");

    /* The fade runs down to nothing and never back up. */
    {
        int band;
        int prev = 256;
        for (band = 0; band < FOG_ROAD_BANDS; band++) {
            int a = (int)fogRoadBandAlpha(band);
            UT_ASSERT_MSG(a < prev, "band %d alpha %d did not fall below %d",
                          band, a, prev);
            UT_ASSERT_MSG(a > 0, "band %d alpha is %d, expected above 0",
                          band, a);
            prev = a;
        }
        UT_ASSERT_MSG(fogRoadBandAlpha(0) == FOG_ROAD_ALPHA,
                      "the band at the edge carries %d, expected %d",
                      (int)fogRoadBandAlpha(0), FOG_ROAD_ALPHA);
        UT_ASSERT_MSG(fogRoadBandAlpha(FOG_ROAD_BANDS) == 0,
                      "past the last band has to be nothing, got %d",
                      (int)fogRoadBandAlpha(FOG_ROAD_BANDS));
    }

    return 0;
}

/* The store the prefs loader writes through: every value of the enum survives
 * a set and a get, and anything else lands on the default. That is what keeps
 * a hand-edited prefs file from leaving the views in a look neither of them
 * knows how to draw. */
int run_fog_style_setting(void) {
    int i; /* Looping variable */
    FogStyle was = gfxGetFogStyle();

    /* Back to a fresh install first. The store is process-wide, so "what does
     * a player who has never picked get" can only be asked from a known
     * state; without this the assertion below would only hold while this case
     * happened to run before anything else that sets a graphics setting. */
    gfxSettingsResetDefaults();

    /* The default has to be a style, or the store starts in a look neither
     * view can draw and every assertion below is meaningless. */
    UT_ASSERT_MSG((int)FOG_STYLE_DEFAULT >= (int)FOG_STYLE_GREY &&
                      (int)FOG_STYLE_DEFAULT < FOG_STYLE_COUNT,
                  "FOG_STYLE_DEFAULT is %d, outside the enum",
                  (int)FOG_STYLE_DEFAULT);

    /* A player who has never picked gets the default, not style 0. The two
     * parted company when the default moved to Darker with fog edge; Grey
     * keeps 0 because that number is in prefs files already written. This is
     * what catches the store's own initial value drifting from
     * FOG_STYLE_DEFAULT, which is the drift the constant exists to stop. */
    UT_ASSERT_MSG(gfxGetFogStyle() == FOG_STYLE_DEFAULT,
                  "a fresh install starts at %d, expected the default (%d)",
                  (int)gfxGetFogStyle(), (int)FOG_STYLE_DEFAULT);

    for (i = 0; i < FOG_STYLE_COUNT; i++) {
        gfxSetFogStyle((FogStyle)i);
        UT_ASSERT_MSG((int)gfxGetFogStyle() == i,
                      "set fog style %d, read back %d", i,
                      (int)gfxGetFogStyle());
    }

    gfxSetFogStyle((FogStyle)FOG_STYLE_COUNT);
    UT_ASSERT_MSG(gfxGetFogStyle() == FOG_STYLE_DEFAULT,
                  "a style past the end left %d, expected the default (%d)",
                  (int)gfxGetFogStyle(), (int)FOG_STYLE_DEFAULT);

    gfxSetFogStyle((FogStyle)99);
    UT_ASSERT_MSG(gfxGetFogStyle() == FOG_STYLE_DEFAULT,
                  "a nonsense style left %d, expected the default (%d)",
                  (int)gfxGetFogStyle(), (int)FOG_STYLE_DEFAULT);

    /* Below the enum as well as above it: atoi on a prefs value that is not a
     * number at all can hand a negative in, and gamefront.c leans on that by
     * reading the key with "-1" when it is missing. */
    gfxSetFogStyle((FogStyle)-1);
    UT_ASSERT_MSG(gfxGetFogStyle() == FOG_STYLE_DEFAULT,
                  "a negative style left %d, expected the default (%d)",
                  (int)gfxGetFogStyle(), (int)FOG_STYLE_DEFAULT);

    gfxSetFogStyle(was);
    return 0;
}

/* ------------------------------------------------------------------ */
/* The edge mask                                                      */
/* ------------------------------------------------------------------ */

/* Two squares that are not road, named so a grid below reads as a picture. */
#define G ((unsigned char)GRASS)  /* plain ground */
#define W ((unsigned char)DEEP_SEA_SOLID)
#define R ((unsigned char)ROAD_SOLID)

/* Fogged and in plain sight, for the flag arguments. */
#define FOGD 1
#define SEEN 0

int run_fog_road_edges(void) {
    /* The run of tile numbers that counts as road, at both ends and just
     * outside each. */
    UT_ASSERT_MSG(fogRoadIsRoadTile((unsigned char)ROAD_HORZ) &&
                      fogRoadIsRoadTile((unsigned char)ROAD_SOLID) &&
                      fogRoadIsRoadTile((unsigned char)ROAD_WATER11) &&
                      fogRoadIsRoadTile((unsigned char)ROAD_SIDE4),
                  "a road piece was not read as road");
    UT_ASSERT_MSG(!fogRoadIsRoadTile((unsigned char)(ROAD_HORZ - 1)) &&
                      !fogRoadIsRoadTile((unsigned char)(ROAD_SIDE4 + 1)) &&
                      !fogRoadIsRoadTile((unsigned char)BASE_GOOD) &&
                      !fogRoadIsRoadTile((unsigned char)BUILD_SOLID) &&
                      !fogRoadIsRoadTile((unsigned char)DEEP_SEA_SOLID),
                  "something that is not a road piece was read as road");

    /* A square that is never seen comes over as 255, which is not road. That
     * alone keeps a band off it only in a roads only build; with
     * FOG_EDGE_ALL_TERRAIN at 1 nothing here looks at the terrain, and it is
     * the full screen map that drops an unseen square, beside the same test
     * its terrain pass makes. */
    UT_ASSERT_MSG(!fogRoadIsRoadTile((unsigned char)255),
                  "an unseen square was read as road");
    UT_ASSERT_MSG(fogEdges((unsigned char)255, FOGD, FOGD, SEEN, FOGD, FOGD) ==
                      (FOG_EDGE_ALL_TERRAIN ? FOG_ROAD_EDGE_RIGHT : 0),
                  "an unseen square at the fog line was banded %d, with "
                  "FOG_EDGE_ALL_TERRAIN %d - if this is 0 in an all terrain "
                  "build the view no longer needs its own unseen test",
                  (int)fogEdges((unsigned char)255, FOGD, FOGD, SEEN, FOGD,
                                FOGD),
                  (int)FOG_EDGE_ALL_TERRAIN);

    /* The case the look is drawn for: a fogged road square with the square to
     * its east in plain sight is banded on its right hand side and nowhere
     * else. The band is the fog line, so it is on the side that faces out. */
    UT_ASSERT_MSG(fogEdges(R, FOGD, FOGD, SEEN, FOGD, FOGD) ==
                      FOG_ROAD_EDGE_RIGHT,
                  "a fogged road with the east square seen was banded %d, "
                  "expected the right only",
                  (int)fogEdges(R, FOGD, FOGD, SEEN, FOGD, FOGD));

    /* Road well inside the fog: there is no fog line at it, so no band. */
    UT_ASSERT_MSG(fogEdges(R, FOGD, FOGD, FOGD, FOGD, FOGD) == 0,
                  "road inside the fog was banded %d, expected none",
                  (int)fogEdges(R, FOGD, FOGD, FOGD, FOGD, FOGD));

    /* The same line seen from the other side: the road in plain sight next to
     * fog is drawn as itself and takes no band. The band belongs to the
     * fogged square. */
    UT_ASSERT_MSG(fogEdges(R, SEEN, FOGD, FOGD, FOGD, FOGD) == 0,
                  "a road in plain sight beside fog was banded %d",
                  (int)fogEdges(R, SEEN, FOGD, FOGD, FOGD, FOGD));

    /* A fogged road square with fog on none of its sides — an island of fog
     * one square across — is banded all the way round. */
    UT_ASSERT_MSG(fogEdges(R, FOGD, SEEN, SEEN, SEEN, SEEN) ==
                      (FOG_ROAD_EDGE_LEFT | FOG_ROAD_EDGE_RIGHT |
                       FOG_ROAD_EDGE_TOP | FOG_ROAD_EDGE_BOTTOM),
                  "a lone fogged road square was banded %d, expected all four",
                  (int)fogEdges(R, FOGD, SEEN, SEEN, SEEN, SEEN));

    /* The neighbour's own terrain does not come into it: what is beside a
     * fogged road square only matters in as much as it is fogged or not. Road
     * on both sides, both in plain sight, still bands both sides. */
    UT_ASSERT_MSG(fogEdges(R, FOGD, SEEN, SEEN, FOGD, FOGD) ==
                      (FOG_ROAD_EDGE_LEFT | FOG_ROAD_EDGE_RIGHT),
                  "a fogged road seen on both sides was banded %d, expected "
                  "left and right", (int)fogEdges(R, FOGD, SEEN, SEEN,
                                                  FOGD, FOGD));

    /* Which terrain wants a band is the switch and nothing else, so the two
     * have to say the same thing about a square that is not road. */
    UT_ASSERT_MSG(fogEdgeTileWantsBand(R) != 0, "road has to want a band");
    UT_ASSERT_MSG((fogEdgeTileWantsBand(G) != 0) == (FOG_EDGE_ALL_TERRAIN != 0)
                      && (fogEdgeTileWantsBand(W) != 0) ==
                             (FOG_EDGE_ALL_TERRAIN != 0),
                  "grass and water want a band %d/%d, with "
                  "FOG_EDGE_ALL_TERRAIN %d",
                  fogEdgeTileWantsBand(G), fogEdgeTileWantsBand(W),
                  (int)FOG_EDGE_ALL_TERRAIN);

    /* Ground that is not road, at the fog line. With the switch on, which is
     * how the game is built, it is banded by the same rule as road, so the
     * fog line reads as one line all the way along. With the switch off it is
     * left alone, because the darkening already shows the line over it. */
    UT_ASSERT_MSG(fogEdges(G, FOGD, FOGD, SEEN, FOGD, FOGD) ==
                      (FOG_EDGE_ALL_TERRAIN ? FOG_ROAD_EDGE_RIGHT : 0),
                  "fogged grass beside a seen square was banded %d, with "
                  "FOG_EDGE_ALL_TERRAIN %d",
                  (int)fogEdges(G, FOGD, FOGD, SEEN, FOGD, FOGD),
                  (int)FOG_EDGE_ALL_TERRAIN);
    UT_ASSERT_MSG(fogEdges(W, FOGD, SEEN, SEEN, SEEN, SEEN) ==
                      (FOG_EDGE_ALL_TERRAIN
                           ? (FOG_ROAD_EDGE_LEFT | FOG_ROAD_EDGE_RIGHT |
                              FOG_ROAD_EDGE_TOP | FOG_ROAD_EDGE_BOTTOM)
                           : 0),
                  "fogged water with every neighbour seen was banded %d",
                  (int)fogEdges(W, FOGD, SEEN, SEEN, SEEN, SEEN));

    /* A square in plain sight is never banded, whatever its terrain and
     * whatever the switch says. */
    UT_ASSERT_MSG(fogEdges(R, SEEN, SEEN, SEEN, SEEN, SEEN) == 0 &&
                      fogEdges(G, SEEN, SEEN, SEEN, SEEN, SEEN) == 0,
                  "a square in plain sight was banded");

    /* fogEdgeSides is the half of the rule the full screen map asks first, so
     * that it can drop a square before reading its terrain. It has to give the
     * same side bits fogEdges does for a fogged square whose terrain wants a
     * band, or that walk and this one would disagree about where the fog line
     * is. Every arrangement of the four neighbours is checked, which is only
     * sixteen. */
    {
        int bits; /* Which of the four neighbours are fogged */
        for (bits = 0; bits < 16; bits++) {
            int lfg = (bits & 1) != 0, rfg = (bits & 2) != 0;
            int ufg = (bits & 4) != 0, dfg = (bits & 8) != 0;
            unsigned char sides = fogEdgeSides(lfg, rfg, ufg, dfg);
            unsigned char want  = 0;
            if (!lfg) want |= FOG_ROAD_EDGE_LEFT;
            if (!rfg) want |= FOG_ROAD_EDGE_RIGHT;
            if (!ufg) want |= FOG_ROAD_EDGE_TOP;
            if (!dfg) want |= FOG_ROAD_EDGE_BOTTOM;
            UT_ASSERT_MSG(sides == want,
                          "fogEdgeSides(%d,%d,%d,%d) gave %d, expected %d",
                          lfg, rfg, ufg, dfg, (int)sides, (int)want);
            /* Road always wants a band, so for road the two agree outright. */
            UT_ASSERT_MSG(fogEdges(R, FOGD, lfg, rfg, ufg, dfg) == sides,
                          "fogEdges and fogEdgeSides disagree on road at "
                          "neighbours %d,%d,%d,%d: %d vs %d",
                          lfg, rfg, ufg, dfg,
                          (int)fogEdges(R, FOGD, lfg, rfg, ufg, dfg),
                          (int)sides);
        }
    }

    /* Fog on all four sides is the case the walk leans on: no fog line here,
     * so the answer is 0 and the caller can drop the square without ever
     * reading what is on it. */
    UT_ASSERT_MSG(fogEdgeSides(FOGD, FOGD, FOGD, FOGD) == 0,
                  "fog on all four sides gave %d, expected none",
                  (int)fogEdgeSides(FOGD, FOGD, FOGD, FOGD));

    return 0;
}

/* The whole-grid walk: the same answers, reached from a picture rather than
 * from four neighbours handed in by hand, plus what happens at the edge of
 * the grid itself. */
int run_fog_road_edge_masks(void) {
    /* A four by four with a road running across the middle row and one arm
     * going down from it, and the road touching the right hand edge of the
     * grid.
     *
     *   . . . .
     *   R R R R   <- runs off the right edge
     *   . R . .
     *   . . . .
     */
    static const unsigned char tiles[16] = {
        G, G, G, G,
        R, R, R, R,
        G, R, G, G,
        G, G, G, G
    };
    /* The fog over it. Three squares are in plain sight — one above the road,
     * one in the road itself, and one under the arm — so the fog line runs
     * round each of them and the four sides are all put to work.
     *
     *   # . # #
     *   # # . #
     *   # # # #
     *   # . # #
     */
    static const unsigned char fogged[16] = {
        1, 0, 1, 1,
        1, 1, 0, 1,
        1, 1, 1, 1,
        1, 0, 1, 1
    };
    unsigned char out[16];
    int i; /* Looping variable */

    memset(out, 0xEE, sizeof(out));
    fogEdgeMasks(tiles, fogged, 4, 4, out);

    /* Nothing in plain sight carries a band, however the fog runs round it.
     * That holds whichever way the switch is set: the band is on the fogged
     * side of the line and only there. */
    for (i = 0; i < 16; i++) {
        if (fogged[i] == 0) {
            UT_ASSERT_MSG(out[i] == 0,
                          "square %d is in plain sight but was banded %d", i,
                          (int)out[i]);
        }
    }

    /* With the switch off, a fogged square that is not road carries nothing
     * either. With it on, the square's terrain does not come into the answer
     * at all, which the grass cases further down check one by one. */
    if (!FOG_EDGE_ALL_TERRAIN) {
        for (i = 0; i < 16; i++) {
            if (fogged[i] == 0) continue;
            if (fogRoadIsRoadTile(tiles[i])) continue;
            UT_ASSERT_MSG(out[i] == 0,
                          "square %d is not road but was banded %d", i,
                          (int)out[i]);
        }
    }

    /* The left end of the run. Fog all round it and the grid ending on its
     * left, which counts as fog too, so nothing is banded: this is road
     * inside the fog and there is no fog line at it. */
    UT_ASSERT_MSG(out[4] == 0,
                  "the left end of the run was banded %d, expected none",
                  (int)out[4]);

    /* The square with plain sight above it and to its right: banded on those
     * two sides and no other. */
    UT_ASSERT_MSG(out[5] == (FOG_ROAD_EDGE_TOP | FOG_ROAD_EDGE_RIGHT),
                  "the square under the seen ground was banded %d, expected "
                  "the top and the right", (int)out[5]);

    /* The road square in plain sight itself: no band, though it is next to
     * fog on three sides. The band is on the fogged side of the line. */
    UT_ASSERT_MSG(out[6] == 0,
                  "the road in plain sight was banded %d, expected none",
                  (int)out[6]);

    /* The right hand end, which runs off the grid: banded on its left, where
     * it faces the square in plain sight, and not on the right, where the
     * grid ends and the fog is taken to go on. */
    UT_ASSERT_MSG(out[7] == FOG_ROAD_EDGE_LEFT,
                  "the right end of the run was banded %d, expected the left "
                  "only", (int)out[7]);

    /* The arm, with the plain sight under it: banded on the bottom only. */
    UT_ASSERT_MSG(out[9] == FOG_ROAD_EDGE_BOTTOM,
                  "the arm was banded %d, expected the bottom only",
                  (int)out[9]);

    /* The grass in the same picture. With the switch on it is banded by the
     * same rule as the road, so the fog line carries on across it instead of
     * stopping where the road does; with the switch off it carries nothing.
     * Every grass square in the grid is named, at the line and away from it,
     * so the whole answer is written down rather than sampled. */
    {
        static const unsigned char grassSlot[9] = {
            0, 2, 3, 8, 10, 11, 12, 14, 15
        };
        static const unsigned char grassBands[9] = {
            FOG_ROAD_EDGE_RIGHT,                            /* 0  */
            FOG_ROAD_EDGE_LEFT | FOG_ROAD_EDGE_BOTTOM,      /* 2  */
            0,                                              /* 3  */
            0,                                              /* 8  */
            FOG_ROAD_EDGE_TOP,                              /* 10 */
            0,                                              /* 11 */
            FOG_ROAD_EDGE_RIGHT,                            /* 12 */
            FOG_ROAD_EDGE_LEFT,                             /* 14 */
            0                                               /* 15 */
        };
        for (i = 0; i < 9; i++) {
            int slot = (int)grassSlot[i];
            int want = FOG_EDGE_ALL_TERRAIN ? (int)grassBands[i] : 0;
            UT_ASSERT_MSG((int)out[slot] == want,
                          "grass square %d was banded %d, expected %d with "
                          "FOG_EDGE_ALL_TERRAIN %d", slot, (int)out[slot],
                          want, (int)FOG_EDGE_ALL_TERRAIN);
        }
    }

    /* The same grid with every square fogged: no fog line anywhere on it, so
     * no band anywhere either. */
    {
        static const unsigned char allFog[16] = {
            1, 1, 1, 1,
            1, 1, 1, 1,
            1, 1, 1, 1,
            1, 1, 1, 1
        };
        unsigned char deep[16];
        memset(deep, 0xEE, sizeof(deep));
        fogEdgeMasks(tiles, allFog, 4, 4, deep);
        for (i = 0; i < 16; i++) {
            UT_ASSERT_MSG(deep[i] == 0,
                          "square %d was banded %d inside solid fog", i,
                          (int)deep[i]);
        }
    }

    /* A grid with no road in it. With the switch on, which is how the game is
     * built, the two fogged squares are banded on the side facing the plain
     * sight beside them and nowhere else; with the switch off the whole grid
     * writes zeroes, which is the roads only build this case was written for
     * and which the switch keeps one define away. */
    {
        static const unsigned char empty[4] = { G, G, W, W };
        static const unsigned char half[4]  = { 1, 0, 1, 0 };
        unsigned char none[4];
        int want = FOG_EDGE_ALL_TERRAIN ? FOG_ROAD_EDGE_RIGHT : 0;
        memset(none, 0xEE, sizeof(none));
        fogEdgeMasks(empty, half, 2, 2, none);
        UT_ASSERT_MSG((int)none[0] == want && none[1] == 0 &&
                          (int)none[2] == want && none[3] == 0,
                      "a grid with no road was banded %d,%d,%d,%d, expected "
                      "%d,0,%d,0 with FOG_EDGE_ALL_TERRAIN %d",
                      (int)none[0], (int)none[1], (int)none[2], (int)none[3],
                      want, want, (int)FOG_EDGE_ALL_TERRAIN);
    }

    return 0;
}

#undef G
#undef W
#undef R
