/*
 * The shared map colours (test_map_colours.c).
 *
 * map_colours.c answers two questions about a tile number: what is standing
 * on the square, and what flat colour the ground under it gets. Every view
 * that draws the map too small for its sprites asks them — the overview, the
 * full screen map, the map choosers and the log viewer — so these cases pin
 * the answers rather than leaving each caller to agree by accident.
 *
 * item_kind: every pill health state on each side classifies alike, the three
 * base tiles land on their own sides, and ordinary ground is nothing.
 *
 * terrain: each family answers across its whole tile range and not just at
 * its first member, an item square answers with the ground under it, and a
 * tile the table has no entry for is refused rather than given a colour.
 *
 * markers: the three layers of a marker's stroke come back outermost first,
 * only the outline is translucent, and the stroke straddles the shape's edge
 * rather than growing the marker.
 */

#include <math.h>

#include "test_harness.h"
#include "map_colours.h"
#include "global.h"
#include "tilenum.h"

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* A colour as one number, for reporting and for "these two agree". */
static unsigned long rgbOf(SDL_Color c) {
    return ((unsigned long)c.r << 16) | ((unsigned long)c.g << 8) |
           (unsigned long)c.b;
}

int run_map_colours_item_kind(void) {
    /* The good pills run PILL_GOOD_15 (full health) up to PILL_GOOD_0, and
     * the evil ones are split: PILL_EVIL_15 sits apart from PILL_EVIL_14..0.
     * Every one of them is a pill, and on the side its name says. */
    for (int t = PILL_GOOD_15; t <= PILL_GOOD_0; t++) {
        UT_ASSERT_MSG(mapColourItemKind((BYTE)t) == MAP_COLOUR_ITEM_PILL_GOOD,
                      "tile %d: good pill classified as %d", t,
                      (int)mapColourItemKind((BYTE)t));
    }
    UT_ASSERT_MSG(mapColourItemKind(PILL_EVIL_15) == MAP_COLOUR_ITEM_PILL_EVIL,
                  "PILL_EVIL_15 classified as %d",
                  (int)mapColourItemKind(PILL_EVIL_15));
    for (int t = PILL_EVIL_14; t <= PILL_EVIL_0; t++) {
        UT_ASSERT_MSG(mapColourItemKind((BYTE)t) == MAP_COLOUR_ITEM_PILL_EVIL,
                      "tile %d: evil pill classified as %d", t,
                      (int)mapColourItemKind((BYTE)t));
    }

    UT_ASSERT_MSG(mapColourItemKind(BASE_GOOD) == MAP_COLOUR_ITEM_BASE_GOOD &&
                  mapColourItemKind(BASE_EVIL) == MAP_COLOUR_ITEM_BASE_EVIL &&
                  mapColourItemKind(BASE_NEUTRAL) ==
                      MAP_COLOUR_ITEM_BASE_NEUTRAL,
                  "the three bases classify as %d / %d / %d",
                  (int)mapColourItemKind(BASE_GOOD),
                  (int)mapColourItemKind(BASE_EVIL),
                  (int)mapColourItemKind(BASE_NEUTRAL));

    /* The bases are the top of the enum, which is how a caller asks "is this
     * a base at all" in one comparison. */
    UT_ASSERT_MSG(MAP_COLOUR_ITEM_BASE_GOOD > MAP_COLOUR_ITEM_PILL_EVIL &&
                  MAP_COLOUR_ITEM_BASE_NEUTRAL > MAP_COLOUR_ITEM_BASE_GOOD,
                  "the bases are not the top of the enum");

    {
        const BYTE ground[] = { GRASS, SWAMP, RUBBLE, CRATER, FOREST,
                                HALFBUILDING, ROAD_HORZ, DEEP_SEA_SOLID };
        for (int i = 0; i < ARRAY_LEN(ground); i++) {
            UT_ASSERT_MSG(mapColourItemKind(ground[i]) == MAP_COLOUR_ITEM_NONE,
                          "tile %d: ground classified as an item (%d)",
                          (int)ground[i],
                          (int)mapColourItemKind(ground[i]));
        }
    }
    return 0;
}

int run_map_colours_terrain(void) {
    /* A family and the range its shape variants occupy. Every tile in the
     * range has to answer with the family's one colour: the table is written
     * as range tests, and a range that stops short leaves tiles in the middle
     * of a river or a forest falling through to the sprite sheet. */
    static const struct {
        const char *name;
        int         first, last;
    } kFamilies[] = {
        { "road",     ROAD_HORZ,       ROAD_SIDE4    },
        { "building", BUILD_SINGLE,    BUILD_MOST4   },
        { "river",    RIVER_END1,      RIVER_CORN4   },
        { "deep sea", DEEP_SEA_SOLID,  DEEP_SEA_SIDE4 },
        { "forest",   FOREST_SINGLE,   FOREST_RIGHT  },
        { "crater",   CRATER_SINGLE,   CRATER_RIGHT  },
        { "boat",     BOAT_0,          BOAT_8        },
    };

    for (int f = 0; f < ARRAY_LEN(kFamilies); f++) {
        SDL_Color first;
        UT_ASSERT_MSG(mapColourTerrain((BYTE)kFamilies[f].first, &first),
                      "%s: tile %d has no colour", kFamilies[f].name,
                      kFamilies[f].first);
        for (int t = kFamilies[f].first; t <= kFamilies[f].last; t++) {
            SDL_Color got;
            UT_ASSERT_MSG(mapColourTerrain((BYTE)t, &got),
                          "%s: tile %d has no colour", kFamilies[f].name, t);
            UT_ASSERT_MSG(rgbOf(got) == rgbOf(first),
                          "%s: tile %d is %06lx, the family is %06lx",
                          kFamilies[f].name, t, rgbOf(got), rgbOf(first));
            UT_ASSERT_MSG(got.a == 255,
                          "%s: tile %d is not opaque (alpha %d)",
                          kFamilies[f].name, t, (int)got.a);
        }
    }

    /* The plain terrains that carry no shape variant. */
    {
        const BYTE plain[] = { SWAMP, RUBBLE, GRASS, HALFBUILDING,
                               FOREST, CRATER };
        for (int i = 0; i < ARRAY_LEN(plain); i++) {
            SDL_Color got;
            UT_ASSERT_MSG(mapColourTerrain(plain[i], &got),
                          "tile %d has no colour", (int)plain[i]);
        }
    }

    /* The shape-variant forest and crater answer as the plain ones do: the
     * two spellings of the same terrain cannot be different colours. */
    {
        SDL_Color plainForest, shapedForest, plainCrater, shapedCrater;
        mapColourTerrain(FOREST, &plainForest);
        mapColourTerrain(FOREST_SINGLE, &shapedForest);
        mapColourTerrain(CRATER, &plainCrater);
        mapColourTerrain(CRATER_SINGLE, &shapedCrater);
        UT_ASSERT_MSG(rgbOf(plainForest) == rgbOf(shapedForest),
                      "forest: plain is %06lx, shaped is %06lx",
                      rgbOf(plainForest), rgbOf(shapedForest));
        UT_ASSERT_MSG(rgbOf(plainCrater) == rgbOf(shapedCrater),
                      "crater: plain is %06lx, shaped is %06lx",
                      rgbOf(plainCrater), rgbOf(shapedCrater));
    }

    /* An item square is the ground under it, so the marker drawn on top sits
     * on the same colour its neighbours have: road under a base, grass under
     * a pill. */
    {
        SDL_Color base, road, pill, grass;
        mapColourTerrain(BASE_GOOD, &base);
        mapColourTerrain(ROAD_HORZ, &road);
        mapColourTerrain(PILL_GOOD_15, &pill);
        mapColourTerrain(GRASS, &grass);
        UT_ASSERT_MSG(rgbOf(base) == rgbOf(road),
                      "a base is %06lx, road is %06lx", rgbOf(base),
                      rgbOf(road));
        UT_ASSERT_MSG(rgbOf(pill) == rgbOf(grass),
                      "a pill is %06lx, grass is %06lx", rgbOf(pill),
                      rgbOf(grass));
    }

    /* Every base and every pill health state answers, not just the ones the
     * item test uses. */
    for (int t = PILL_GOOD_15; t <= PILL_GOOD_0; t++) {
        SDL_Color got;
        UT_ASSERT_MSG(mapColourTerrain((BYTE)t, &got),
                      "pill tile %d has no ground colour", t);
    }

    /* The table answers for what a view actually hands it, and refuses the
     * rest rather than inventing a colour: the caller falls back to the
     * sprite sheet. BUILDING is the case to pin, because the raw terrain
     * never reaches a drawer — viewportCalcSquarePure turns every building
     * into one of the BUILD_* shapes first — so an answer here would mean
     * the table had started returning colours for tile numbers no caller
     * passes it. */
    {
        SDL_Color got = { 1, 2, 3, 4 };
        UT_ASSERT_MSG(!mapColourTerrain(BUILDING, &got),
                      "raw BUILDING was given a terrain colour");
        UT_ASSERT_MSG(got.r == 1 && got.g == 2 && got.b == 3 && got.a == 4,
                      "a refused tile wrote to the colour anyway");
    }

    UT_ASSERT_MSG(!mapColourTerrain(GRASS, NULL),
                  "a NULL colour was accepted");
    return 0;
}

int run_map_colours_markers(void) {
    const float kExact = 1.0e-4f;
    SDL_FColor fills[3];
    fills[0] = mapColourMarkerGood();
    fills[1] = mapColourMarkerEvil();
    fills[2] = mapColourMarkerNeutral();

    UT_ASSERT_MSG(fills[0].g > fills[0].r && fills[0].g > fills[0].b,
                  "the good marker is not green");
    UT_ASSERT_MSG(fills[1].r > fills[1].g && fills[1].r > fills[1].b,
                  "the evil marker is not red");
    UT_ASSERT_MSG(fills[2].r > fills[2].b && fills[2].g > fills[2].b,
                  "the neutral marker is not amber");

    for (int i = 0; i < ARRAY_LEN(fills); i++) {
        SDL_FColor shades[3];
        mapColourMarkerShades(fills[i], shades);

        /* Layer 0 is the outline: black, and the only translucent layer. */
        UT_ASSERT_MSG(shades[0].r == 0.0f && shades[0].g == 0.0f &&
                      shades[0].b == 0.0f,
                      "marker %d: the outline is not black", i);
        UT_ASSERT_MSG(fabsf(shades[0].a - MAP_COLOUR_OUTLINE_ALPHA) < kExact,
                      "marker %d: the outline is %.3f opaque, expected %.3f",
                      i, (double)shades[0].a, (double)MAP_COLOUR_OUTLINE_ALPHA);

        /* Layer 1 is the fill darkened, at the fill's own opacity. */
        UT_ASSERT_MSG(fabsf(shades[1].r - fills[i].r * MAP_COLOUR_DARKEN) < kExact &&
                      fabsf(shades[1].g - fills[i].g * MAP_COLOUR_DARKEN) < kExact &&
                      fabsf(shades[1].b - fills[i].b * MAP_COLOUR_DARKEN) < kExact,
                      "marker %d: the middle layer is not the fill darkened", i);
        UT_ASSERT_MSG(fabsf(shades[1].a - fills[i].a) < kExact,
                      "marker %d: the middle layer is not the fill's opacity", i);

        /* Layer 2 is the fill itself. */
        UT_ASSERT_MSG(fabsf(shades[2].r - fills[i].r) < kExact &&
                      fabsf(shades[2].g - fills[i].g) < kExact &&
                      fabsf(shades[2].b - fills[i].b) < kExact &&
                      fabsf(shades[2].a - fills[i].a) < kExact,
                      "marker %d: the top layer is not the fill", i);
    }

    /* The stroke straddles the shape's edge: half of it outside, the middle
     * layer on the edge itself, half inside. That is what keeps a marker the
     * size it was asked for — a stroke drawn wholly outside would make every
     * marker a stroke wider than the shape the caller sized to its square. */
    UT_ASSERT_MSG(mapColourMarkerLayerGrow(0) > mapColourMarkerLayerGrow(1) &&
                  mapColourMarkerLayerGrow(1) > mapColourMarkerLayerGrow(2),
                  "the layers do not shrink: %.3f, %.3f, %.3f",
                  (double)mapColourMarkerLayerGrow(0),
                  (double)mapColourMarkerLayerGrow(1),
                  (double)mapColourMarkerLayerGrow(2));
    UT_ASSERT_MSG(fabsf(mapColourMarkerLayerGrow(0) -
                        MAP_COLOUR_STROKE_PX * 0.5f) < kExact,
                  "the outline reaches %.3f px, expected half the stroke",
                  (double)mapColourMarkerLayerGrow(0));
    UT_ASSERT_MSG(fabsf(mapColourMarkerLayerGrow(1)) < kExact,
                  "the middle layer is %.3f px off the shape, expected none",
                  (double)mapColourMarkerLayerGrow(1));
    UT_ASSERT_MSG(fabsf(mapColourMarkerLayerGrow(2) +
                        MAP_COLOUR_STROKE_PX * 0.5f) < kExact,
                  "the fill is inset %.3f px, expected half the stroke",
                  (double)mapColourMarkerLayerGrow(2));

    /* Out-of-range layers clamp rather than running off either end, so a
     * caller's loop bound cannot turn into a stray offset. */
    UT_ASSERT_MSG(fabsf(mapColourMarkerLayerGrow(-1) -
                        mapColourMarkerLayerGrow(0)) < kExact &&
                  fabsf(mapColourMarkerLayerGrow(9) -
                        mapColourMarkerLayerGrow(2)) < kExact,
                  "an out-of-range layer did not clamp");
    return 0;
}
