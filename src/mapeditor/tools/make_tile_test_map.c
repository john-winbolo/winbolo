/*
 * make_tile_test_map — writes a small .map that puts the terrain-shape
 * lookups in src/bolo/screencalc.c side by side, so each one can be
 * eyeballed in the game or the map editor.
 *
 * Each demo is a matched pair: a reference layout that renders correctly
 * and, a few squares away, the same layout with one river square turned
 * into a road (a bridge). The lookups are supposed to read a road as
 * "the water continues here", so both halves of a pair should draw the
 * same shape. Where they don't, the right-hand half is the defect.
 *
 * Build:
 *     cmake --build build --target MakeTileTestMap
 *     ./build/MakeTileTestMap tile_test.map
 *
 * The program prints the tile each marked square resolves to, so the
 * expected-vs-actual comparison can be made without launching anything.
 *
 * It lives under src/mapeditor/ rather than tools/ because it writes .map
 * files through bolo's T2 map-data headers — the scope of the mapeditor
 * privileged exception in docs/ARCHITECTURE.md. tools/ is public-only.
 */
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "bolo_map.h"
#include "screencalc.h"
#include "starts.h"
#include "bases.h"
#include "pillbox.h"
#include "bolo_map_validate.h"

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/* The island. mapCreate() fills all 256x256 with deep sea, so the sea is
 * simply everything this rectangle does not cover — which is what gives
 * the shoreline demo real deep sea to sit against. */
#define ISLAND_X0  96
#define ISLAND_X1 150
#define ISLAND_Y0  96
#define ISLAND_Y1 144

/* Demo 1 — the deep-sea shoreline. Two rivers run east to the coast at
 * x = ISLAND_X1; the second has a bridge on its mouth square. The sea
 * square just east of each mouth is what to look at. */
#define SEA_REF_Y  104
#define SEA_BUG_Y  110
#define SEA_X     (ISLAND_X1 + 1)

/* Demo 2 — a boat with water beside it, then the same boat with a
 * bridge in place of that water. */
#define BOAT_REF_X 108
#define BOAT_BUG_X 118
#define BOAT_Y     119

/* Demo 3 — the river cross with a road laid on its centre square, and a
 * plain river cross beside it for comparison. */
#define CROSS_BUG_X 108
#define CROSS_REF_X 118
#define CROSS_Y     130

static map        mp;
static pillboxes  pb;
static bases      bs;
static starts     ss;

static BYTE terrainOf(char c) {
    switch (c) {
    case 'D': return DEEP_SEA;
    case 'R': return RIVER;
    case 'B': return BOAT;
    case 'O': return ROAD;
    case 'G': return GRASS;
    case '#': return BUILDING;
    case 'T': return FOREST;
    default:  return GRASS;
    }
}

/* Every square this file touches has to sit one square inside the array, so
 * that resolve() below can read the eight neighbours without walking off the
 * end. Demos live deep in the island's interior, so a failure here means a
 * newly added layout was placed badly — say so loudly rather than corrupting
 * memory. */
static int inBounds(int x, int y) {
    if (x < 1 || x >= MAP_ARRAY_SIZE - 1 || y < 1 || y >= MAP_ARRAY_SIZE - 1) {
        fprintf(stderr, "square (%d,%d) is outside the paintable area "
                        "(1..%d in each axis)\n", x, y, MAP_ARRAY_SIZE - 2);
        return 0;
    }
    return 1;
}

/* Paint one row of the ASCII notation with its top-left corner at x,y.
 * '.' leaves the square alone. */
static void paint(int x, int y, const char *row) {
    for (int i = 0; row[i] != '\0'; i++) {
        if (row[i] != '.' && inBounds(x + i, y)) {
            mp->mapItem[x + i][y] = terrainOf(row[i]);
        }
    }
}

static void fillRect(int x0, int y0, int x1, int y1, BYTE terrain) {
    for (int x = x0; x <= x1; x++) {
        for (int y = y0; y <= y1; y++) {
            if (inBounds(x, y)) {
                mp->mapItem[x][y] = terrain;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Reporting — resolve a square the way viewport.c does                */
/* ------------------------------------------------------------------ */

static const char *tileName(BYTE v) {
    switch (v) {
    case DEEP_SEA_SOLID:  return "DEEP_SEA_SOLID";
    case DEEP_SEA_CORN1:  return "DEEP_SEA_CORN1";
    case DEEP_SEA_CORN2:  return "DEEP_SEA_CORN2";
    case DEEP_SEA_CORN3:  return "DEEP_SEA_CORN3";
    case DEEP_SEA_CORN4:  return "DEEP_SEA_CORN4";
    case DEEP_SEA_SIDE1:  return "DEEP_SEA_SIDE1";
    case DEEP_SEA_SIDE2:  return "DEEP_SEA_SIDE2";
    case DEEP_SEA_SIDE3:  return "DEEP_SEA_SIDE3";
    case DEEP_SEA_SIDE4:  return "DEEP_SEA_SIDE4";
    case RIVER_SOLID:     return "RIVER_SOLID";
    case RIVER_SURROUND:  return "RIVER_SURROUND";
    case RIVER_END1:      return "RIVER_END1";
    case RIVER_END2:      return "RIVER_END2";
    case RIVER_END3:      return "RIVER_END3";
    case RIVER_END4:      return "RIVER_END4";
    case RIVER_SIDE1:     return "RIVER_SIDE1";
    case RIVER_SIDE2:     return "RIVER_SIDE2";
    case RIVER_ONESIDE1:  return "RIVER_ONESIDE1";
    case RIVER_ONESIDE2:  return "RIVER_ONESIDE2";
    case RIVER_ONESIDE3:  return "RIVER_ONESIDE3";
    case RIVER_ONESIDE4:  return "RIVER_ONESIDE4";
    case RIVER_CORN1:     return "RIVER_CORN1";
    case RIVER_CORN2:     return "RIVER_CORN2";
    case RIVER_CORN3:     return "RIVER_CORN3";
    case RIVER_CORN4:     return "RIVER_CORN4";
    case BOAT_0:          return "BOAT_0";
    case BOAT_1:          return "BOAT_1";
    case BOAT_2:          return "BOAT_2";
    case BOAT_3:          return "BOAT_3";
    case BOAT_4:          return "BOAT_4";
    case BOAT_5:          return "BOAT_5";
    case BOAT_6:          return "BOAT_6";
    case BOAT_7:          return "BOAT_7";
    case ROAD_WATER11:    return "ROAD_WATER11";
    default:              return "(other)";
    }
}

static BYTE sq(int x, int y) { return mp->mapItem[x][y]; }

/* The same terrain switch viewport.c and the log viewer's screen.c run.
 * Reads all eight neighbours, so the square must be an interior one. */
static BYTE resolve(int x, int y) {
    if (!inBounds(x, y)) {
        return DEEP_SEA;
    }

    BYTE al = sq(x - 1, y - 1), a = sq(x, y - 1), ar = sq(x + 1, y - 1);
    BYTE l  = sq(x - 1, y),                       r  = sq(x + 1, y);
    BYTE bl = sq(x - 1, y + 1), b = sq(x, y + 1), br = sq(x + 1, y + 1);

    switch (sq(x, y)) {
    case ROAD:     return screenCalcRoad(al, a, ar, l, r, bl, b, br);
    case RIVER:    return screenCalcRiver(al, a, ar, l, r, bl, b, br);
    case DEEP_SEA: return screenCalcDeepSea(al, a, ar, l, r, bl, b, br);
    case BOAT:     return screenCalcBoat(al, a, ar, l, r, bl, b, br);
    default:       return sq(x, y);
    }
}

static void report(const char *what, int refX, int refY, int bugX, int bugY) {
    BYTE ref = resolve(refX, refY);
    BYTE bug = resolve(bugX, bugY);
    printf("  %-34s reference (%d,%d) %-16s vs bridge (%d,%d) %-16s  %s\n",
           what, refX, refY, tileName(ref), bugX, bugY, tileName(bug),
           ref == bug ? "match" : "*** DIFFERENT ***");
}

/* ------------------------------------------------------------------ */

/* Read the file we just wrote back through the real loader. mapRead runs
 * mapCenter, so the loaded map is the same layout shifted; work the shift
 * out from the land bounding box, check every square under it, and report
 * the coordinates the game will actually show. Owns its four handles and
 * frees them on every exit path. */
static int verifyReadBack(char *path) {
    map        rmp;
    pillboxes  rpb;
    bases      rbs;
    starts     rss;
    char       name[256] = {0};
    int        dx, dy;
    int        rc = 0;

    if (!boloMapValidate(path, name, sizeof name)) {
        fprintf(stderr, "\n%s failed boloMapValidate\n", path);
        return 1;
    }
    mapCreate(&rmp);
    pillsCreate(&rpb);
    basesCreate(&rbs);
    startsCreate(&rss);

    if (!mapRead(path, &rmp, &rpb, &rbs, &rss)) {
        fprintf(stderr, "\n%s failed to read back\n", path);
        rc = 1;
        goto done;
    }

    {
        int wx0 = MAP_ARRAY_SIZE, wy0 = MAP_ARRAY_SIZE;
        int rx0 = MAP_ARRAY_SIZE, ry0 = MAP_ARRAY_SIZE;
        for (int x = 0; x < MAP_ARRAY_SIZE; x++) {
            for (int y = 0; y < MAP_ARRAY_SIZE; y++) {
                if (mp->mapItem[x][y] != DEEP_SEA) {
                    if (x < wx0) wx0 = x;
                    if (y < wy0) wy0 = y;
                }
                if (rmp->mapItem[x][y] != DEEP_SEA) {
                    if (x < rx0) rx0 = x;
                    if (y < ry0) ry0 = y;
                }
            }
        }
        dx = rx0 - wx0;
        dy = ry0 - wy0;
    }

    for (int x = 0; x < MAP_ARRAY_SIZE; x++) {
        for (int y = 0; y < MAP_ARRAY_SIZE; y++) {
            int sx = x + dx, sy = y + dy;
            BYTE want = mp->mapItem[x][y];
            BYTE got  = (sx >= 0 && sx < MAP_ARRAY_SIZE &&
                         sy >= 0 && sy < MAP_ARRAY_SIZE)
                        ? rmp->mapItem[sx][sy] : (BYTE)DEEP_SEA;
            if (want != got) {
                fprintf(stderr,
                        "\nsquare (%d,%d) wrote %d, came back at (%d,%d) as %d\n",
                        x, y, want, sx, sy, got);
                rc = 1;
                goto done;
            }
        }
    }

    printf("\nread back and verified: %d starts, %d bases, %d pills\n",
           startsGetNumStarts(&rss), basesGetNumBases(&rbs),
           pillsGetNumPills(&rpb));
    printf("the loader recentres the map by (%+d,%+d), so in game look at:\n",
           dx, dy);
    printf("  (%d,%d) and (%d,%d)   river mouths, east coast   <-- deep sea\n",
           SEA_X + dx, SEA_REF_Y + dy, SEA_X + dx, SEA_BUG_Y + dy);
    printf("  (%d,%d) and (%d,%d)   boats\n",
           BOAT_REF_X + 2 + dx, BOAT_Y + 1 + dy,
           BOAT_BUG_X + 2 + dx, BOAT_Y + 1 + dy);
    printf("  (%d,%d) and (%d,%d)   river crosses\n",
           CROSS_REF_X + 2 + dx, CROSS_Y + 1 + dy,
           CROSS_BUG_X + 2 + dx, CROSS_Y + 1 + dy);

done:
    mapDestroy(&rmp);
    pillsDestroy(&rpb);
    basesDestroy(&rbs);
    startsDestroy(&rss);
    return rc;
}

int main(int argc, char *argv[]) {
    /* mapWrite / mapRead take a non-const char *, so keep the path in a
     * writable buffer rather than casting the const away from argv or from
     * the default string literal. */
    char path[512];
    int  rc = 0;

    snprintf(path, sizeof path, "%s",
             (argc > 1) ? argv[1] : "tile_test.map");

    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);

    /* Land. Everything outside this is the deep sea mapCreate laid down. */
    fillRect(ISLAND_X0, ISLAND_Y0, ISLAND_X1, ISLAND_Y1, GRASS);

    /* ---- Demo 1: river mouths at the east coast -------------------- */
    /* Reference: the river reaches the coast as river.
     *     RRRRRR|D    the sea square right of the mouth should blend */
    paint(ISLAND_X1 - 5, SEA_REF_Y, "RRRRRR");
    /* Bug: a bridge sits on the mouth square.
     *     RRRRRO|D    same shoreline, but the sea square goes solid */
    paint(ISLAND_X1 - 5, SEA_BUG_Y, "RRRRRO");

    /* ---- Demo 2: a boat beside water, then beside a bridge ---------- */
    paint(BOAT_REF_X, BOAT_Y + 0, "GGGGG");
    paint(BOAT_REF_X, BOAT_Y + 1, "GRBRG");
    paint(BOAT_REF_X, BOAT_Y + 2, "GGRGG");
    paint(BOAT_REF_X, BOAT_Y + 3, "GGGGG");

    paint(BOAT_BUG_X, BOAT_Y + 0, "GGGGG");
    paint(BOAT_BUG_X, BOAT_Y + 1, "GOBRG");
    paint(BOAT_BUG_X, BOAT_Y + 2, "GGRGG");
    paint(BOAT_BUG_X, BOAT_Y + 3, "GGGGG");

    /* ---- Demo 3: the river cross, with and without a bridge --------- */
    paint(CROSS_BUG_X, CROSS_Y + 0, "GGRGG");
    paint(CROSS_BUG_X, CROSS_Y + 1, "GRORG");
    paint(CROSS_BUG_X, CROSS_Y + 2, "GGRGG");

    paint(CROSS_REF_X, CROSS_Y + 0, "GGRGG");
    paint(CROSS_REF_X, CROSS_Y + 1, "GRRRG");
    paint(CROSS_REF_X, CROSS_Y + 2, "GGRGG");

    /* ---- Somewhere to spawn, and bases so the round doesn't end ----- */
    /* startsSetStart / basesSetBase are 1-based: they reject index 0. */
    startsSetNumStarts(&ss, 8);
    for (BYTE i = 1; i <= 8; i++) {
        start s;
        s.x   = (BYTE)(ISLAND_X0 + 4 + (i - 1) * 2);
        s.y   = (BYTE)(ISLAND_Y0 + 4);
        s.dir = 0;
        startsSetStart(&ss, &s, i);
    }

    basesSetNumBases(&bs, 2);
    for (BYTE i = 1; i <= 2; i++) {
        base bse;
        memset(&bse, 0, sizeof bse);
        bse.x          = (BYTE)(ISLAND_X0 + 6 + (i - 1) * 40);
        bse.y          = (BYTE)(ISLAND_Y1 - 4);
        bse.owner      = NEUTRAL;
        bse.armour     = 90;
        bse.shells     = 90;
        bse.mines      = 90;
        bse.refuelTime = 0;
        bse.baseTime   = 0;
        basesSetBase(&bs, &bse, i);
        if (inBounds(bse.x, bse.y)) {
            mp->mapItem[bse.x][bse.y] = ROAD;
        }
    }

    pillsSetNumPills(&pb, 0);

    printf("What each pair should look like (both halves should match):\n\n");
    report("deep sea beside a river mouth",
           SEA_X, SEA_REF_Y, SEA_X, SEA_BUG_Y);
    report("boat beside water",
           BOAT_REF_X + 2, BOAT_Y + 1, BOAT_BUG_X + 2, BOAT_Y + 1);
    report("river cross arm (top)",
           CROSS_REF_X + 2, CROSS_Y, CROSS_BUG_X + 2, CROSS_Y);
    report("river cross arm (left)",
           CROSS_REF_X + 1, CROSS_Y + 1, CROSS_BUG_X + 1, CROSS_Y + 1);

    if (!mapWrite(path, &mp, &pb, &bs, &ss)) {
        fprintf(stderr, "\nfailed to write %s\n", path);
        rc = 1;
    } else if (verifyReadBack(path) != 0) {
        rc = 1;
    } else {
        printf("wrote %s\n", path);
    }

    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    return rc;
}
