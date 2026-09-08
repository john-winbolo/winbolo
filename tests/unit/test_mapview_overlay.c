/*
 * The entity overlay's placements (test_mapview_overlay.c).
 *
 * mapViewDrawOverlay puts the build cursor, the gunsight, the tank names and
 * the pill and base numbers on screen from the same base and scale as the
 * sprites, through sprite_positions.c's helpers. These cases pin the helpers
 * against the formulas the two views drew by before they shared the pass.
 *
 * gunsight: at an integer scale the top-left is where the classic view drew
 * it — the origin plus the sight's game pixel less one tile, times the zoom,
 * less the scroll edge — and at every rung of the overview ladder it is where
 * the overview drew it: the square 0,0 origin plus the game pixel times the
 * rung.
 *
 * tank_label: the name's top-left is one square to the right of the sprite's
 * game pixel, without the two-pixel offset the classic pass used to add, and
 * is held at the clip's left edge; the same at every rung.
 *
 * cursor: the cursor's top-left is its square's — the classic formula's at an
 * integer zoom, and a whole number of squares from the origin at every rung.
 *
 * item_labels: a number's box is its square's by the same rule, and the
 * numbers are withheld below the minimum scale and drawn from it upward.
 */

#include <math.h>

#include "test_harness.h"
#include "sprite_positions.h"
#include "tiles.h"           /* TILE_SIZE_X / TILE_SIZE_Y */
#include "overview_camera.h" /* the overview's zoom ladder */

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* The classic view at whole-number zooms, drawing at (100, 40) with a
 * (6, 10) pixel scroll. The lists' square 0,0 lands at
 * originX - tileW - edgeX, which is the base every placement starts from. */
static const int   kZooms[]  = { 1, 2, 3 };
static const float kOriginX  = 100.0f, kOriginY = 40.0f;
static const float kEdgeX    = 6.0f,   kEdgeY   = 10.0f;

/* The overview's square 0,0 on screen, fractional as the camera leaves it. */
static const float kO0X = -123.7f, kO0Y = 41.3f;

static float classicBaseX(int zoom) {
    return kOriginX - (float)(TILE_SIZE_X * zoom) - kEdgeX;
}

static float classicBaseY(int zoom) {
    return kOriginY - (float)(TILE_SIZE_Y * zoom) - kEdgeY;
}

int run_mapview_overlay_gunsight(void) {
    const int mx = 8, my = 7, px = 5, py = 12;
    const int gameX = mx * TILE_SIZE_X + px;
    const int gameY = my * TILE_SIZE_Y + py;
    const float kExact = 1.0e-3f;

    /* sdl3draw.c drew the crosshair at
     *     originX + (gsGameX - TILE_SIZE_X) * zoom - edgeX
     * with gsGameX the sight's square times sixteen plus its pixel. */
    for (int z = 0; z < ARRAY_LEN(kZooms); z++) {
        const int zoom = kZooms[z];
        float wantX = kOriginX + (float)((gameX - TILE_SIZE_X) * zoom) - kEdgeX;
        float wantY = kOriginY + (float)((gameY - TILE_SIZE_Y) * zoom) - kEdgeY;
        float gotX = spritePositionGunsight(classicBaseX(zoom), (float)zoom, mx, px);
        float gotY = spritePositionGunsight(classicBaseY(zoom), (float)zoom, my, py);
        UT_ASSERT_MSG(fabsf(gotX - wantX) < kExact && fabsf(gotY - wantY) < kExact,
                      "%dx: gunsight at (%.4f,%.4f), classic formula gives "
                      "(%.4f,%.4f)",
                      zoom, (double)gotX, (double)gotY,
                      (double)wantX, (double)wantY);
    }

    /* overview_view.cpp drew it at originX - tileW + bbx * zoomScale, with
     * originX - tileW the camera's answer for square 0,0 and bbx the same
     * game pixel. */
    OverviewCamera cam;
    overviewCameraInit(&cam);
    for (int z = 0; z < overviewCameraZoomCount(); z++) {
        cam.zoomIndex = z;
        const float zs = overviewCameraZoomScale(&cam);
        float wantX = kO0X + (float)gameX * zs;
        float wantY = kO0Y + (float)gameY * zs;
        float gotX = spritePositionGunsight(kO0X, zs, mx, px);
        float gotY = spritePositionGunsight(kO0Y, zs, my, py);
        UT_ASSERT_MSG(fabsf(gotX - wantX) < kExact && fabsf(gotY - wantY) < kExact,
                      "%.2fx: gunsight at (%.4f,%.4f), overview formula gives "
                      "(%.4f,%.4f)",
                      (double)zs, (double)gotX, (double)gotY,
                      (double)wantX, (double)wantY);
    }

    return 0;
}

int run_mapview_overlay_tank_label(void) {
    const int mx = 3, my = 2, px = 5, py = 9;
    const int gameX = mx * TILE_SIZE_X + px;   /* 53 */
    const int gameY = my * TILE_SIZE_Y + py;   /* 41 */
    const float kExact = 1.0e-3f;

    /* One square to the right of the sprite's own game pixel: at 2x from
     * the base (62, -2), game pixel (53, 41) is (106, 82) on, and the name
     * goes 32 pixels further right, to (200, 80). The pass that used to add
     * two game pixels would have put it at (204, 84). */
    for (int z = 0; z < ARRAY_LEN(kZooms); z++) {
        const int zoom = kZooms[z];
        const float baseX = classicBaseX(zoom);
        const float baseY = classicBaseY(zoom);
        float wantX = baseX + (float)((gameX + TILE_SIZE_X) * zoom);
        float wantY = baseY + (float)(gameY * zoom);
        float oldX  = wantX + 2.0f * (float)zoom;
        float oldY  = wantY + 2.0f * (float)zoom;
        float gotX = 0.0f, gotY = 0.0f;

        spritePositionTankLabel(baseX, baseY, (float)zoom, mx, my, px, py,
                                kOriginX, &gotX, &gotY);
        UT_ASSERT_MSG(fabsf(gotX - wantX) < kExact && fabsf(gotY - wantY) < kExact,
                      "%dx: name at (%.4f,%.4f), expected one square right of "
                      "the sprite, (%.4f,%.4f)",
                      zoom, (double)gotX, (double)gotY,
                      (double)wantX, (double)wantY);
        UT_ASSERT_MSG(fabsf(gotX - oldX) > kExact && fabsf(gotY - oldY) > kExact,
                      "%dx: name at (%.4f,%.4f) still carries the two-pixel "
                      "offset (%.4f,%.4f)",
                      zoom, (double)gotX, (double)gotY,
                      (double)oldX, (double)oldY);

        /* A tank in the buffer's square 0 has its name start one square in,
         * which is left of the game area: the name is held at its edge, and
         * only across — the row is untouched. */
        float edgeWantY = baseY;
        spritePositionTankLabel(baseX, baseY, (float)zoom, 0, 0, 0, 0,
                                kOriginX, &gotX, &gotY);
        UT_ASSERT_MSG(fabsf(gotX - kOriginX) < kExact &&
                      fabsf(gotY - edgeWantY) < kExact,
                      "%dx: name for square 0 at (%.4f,%.4f), expected it "
                      "held at the left edge (%.4f,%.4f)",
                      zoom, (double)gotX, (double)gotY,
                      (double)kOriginX, (double)edgeWantY);
    }

    /* The overview projected mx + px/16 + 1 squares across and my + py/16
     * down, then held a name left of the view at 0. */
    OverviewCamera cam;
    overviewCameraInit(&cam);
    for (int z = 0; z < overviewCameraZoomCount(); z++) {
        cam.zoomIndex = z;
        const float zs = overviewCameraZoomScale(&cam);
        float wantX = kO0X + (float)(gameX + TILE_SIZE_X) * zs;
        float wantY = kO0Y + (float)gameY * zs;
        float gotX = 0.0f, gotY = 0.0f;
        if (wantX < 0.0f) wantX = 0.0f;

        spritePositionTankLabel(kO0X, kO0Y, zs, mx, my, px, py, 0.0f,
                                &gotX, &gotY);
        UT_ASSERT_MSG(fabsf(gotX - wantX) < kExact && fabsf(gotY - wantY) < kExact,
                      "%.2fx: name at (%.4f,%.4f), overview formula gives "
                      "(%.4f,%.4f)",
                      (double)zs, (double)gotX, (double)gotY,
                      (double)wantX, (double)wantY);
    }

    return 0;
}

int run_mapview_overlay_cursor(void) {
    static const int kSquares[] = { 0, 1, 9, 128, 255 };
    const float kExact = 1.0e-3f;

    /* sdl3draw.c drew the cursor at originX + (square - 1) * tileW - edgeX. */
    for (int z = 0; z < ARRAY_LEN(kZooms); z++) {
        const int zoom = kZooms[z];
        const int tileW = TILE_SIZE_X * zoom;
        const int tileH = TILE_SIZE_Y * zoom;
        for (int i = 0; i < ARRAY_LEN(kSquares); i++) {
            const int sq = kSquares[i];
            float wantX = kOriginX + (float)((sq - 1) * tileW) - kEdgeX;
            float wantY = kOriginY + (float)((sq - 1) * tileH) - kEdgeY;
            float gotX = spritePositionSquare(classicBaseX(zoom), (float)zoom, sq);
            float gotY = spritePositionSquare(classicBaseY(zoom), (float)zoom, sq);
            UT_ASSERT_MSG(fabsf(gotX - wantX) < kExact &&
                          fabsf(gotY - wantY) < kExact,
                          "%dx square %d: cursor at (%.4f,%.4f), classic "
                          "formula gives (%.4f,%.4f)",
                          zoom, sq, (double)gotX, (double)gotY,
                          (double)wantX, (double)wantY);
        }
    }

    /* At every rung the cursor is a whole number of squares from the origin:
     * the square it was asked for, and the one the tiles are drawn on. */
    OverviewCamera cam;
    overviewCameraInit(&cam);
    for (int z = 0; z < overviewCameraZoomCount(); z++) {
        cam.zoomIndex = z;
        const float zs = overviewCameraZoomScale(&cam);
        const float squarePx = (float)TILE_SIZE_X * zs;
        for (int i = 0; i < ARRAY_LEN(kSquares); i++) {
            const int sq = kSquares[i];
            float gotX = spritePositionSquare(kO0X, zs, sq);
            float gotY = spritePositionSquare(kO0Y, zs, sq);
            float squaresX = (gotX - kO0X) / squarePx;
            float squaresY = (gotY - kO0Y) / squarePx;
            UT_ASSERT_MSG(fabsf(squaresX - (float)sq) < kExact &&
                          fabsf(squaresY - (float)sq) < kExact,
                          "%.2fx square %d: cursor at (%.4f,%.4f) is "
                          "(%.4f,%.4f) squares from the origin",
                          (double)zs, sq, (double)gotX, (double)gotY,
                          (double)squaresX, (double)squaresY);
        }
    }

    return 0;
}

int run_mapview_overlay_item_labels(void) {
    /* A pill and a base, at the squares their numbers are drawn in. */
    static const struct { int mapX, mapY; } kItems[] = {
        {  5, 6 },
        { 12, 1 }
    };
    const float kMinScale = 1.0f;
    const float kExact = 1.0e-3f;

    /* The number's box is its square: the classic loop's
     * originX + (square - 1) * tileW - edgeX for a buffer square, the
     * overview's the square's own corner. */
    for (int z = 0; z < ARRAY_LEN(kZooms); z++) {
        const int zoom = kZooms[z];
        for (int i = 0; i < ARRAY_LEN(kItems); i++) {
            float wantX = kOriginX + (float)((kItems[i].mapX - 1) * TILE_SIZE_X * zoom) - kEdgeX;
            float wantY = kOriginY + (float)((kItems[i].mapY - 1) * TILE_SIZE_Y * zoom) - kEdgeY;
            float gotX = spritePositionSquare(classicBaseX(zoom), (float)zoom, kItems[i].mapX);
            float gotY = spritePositionSquare(classicBaseY(zoom), (float)zoom, kItems[i].mapY);
            UT_ASSERT_MSG(fabsf(gotX - wantX) < kExact &&
                          fabsf(gotY - wantY) < kExact,
                          "%dx square (%d,%d): number box at (%.4f,%.4f), "
                          "classic loop drew it at (%.4f,%.4f)",
                          zoom, kItems[i].mapX, kItems[i].mapY,
                          (double)gotX, (double)gotY,
                          (double)wantX, (double)wantY);
            UT_ASSERT_MSG(spritePositionItemLabelShown((float)zoom, kMinScale),
                          "%dx: numbers withheld at a whole-number zoom", zoom);
        }
    }

    /* Withheld under the floor, drawn from it up: the overview's ladder has
     * two rungs under 1x and five at or over it. */
    OverviewCamera cam;
    overviewCameraInit(&cam);
    int withheld = 0, drawn = 0;
    for (int z = 0; z < overviewCameraZoomCount(); z++) {
        cam.zoomIndex = z;
        const float zs = overviewCameraZoomScale(&cam);
        bool shown = spritePositionItemLabelShown(zs, kMinScale);
        UT_ASSERT_MSG(shown == (zs >= kMinScale),
                      "%.2fx: numbers %s with the floor at %.2fx",
                      (double)zs, shown ? "drawn" : "withheld", (double)kMinScale);
        if (shown) drawn++; else withheld++;

        for (int i = 0; i < ARRAY_LEN(kItems); i++) {
            float wantX = kO0X + (float)(kItems[i].mapX * TILE_SIZE_X) * zs;
            float wantY = kO0Y + (float)(kItems[i].mapY * TILE_SIZE_Y) * zs;
            float gotX = spritePositionSquare(kO0X, zs, kItems[i].mapX);
            float gotY = spritePositionSquare(kO0Y, zs, kItems[i].mapY);
            UT_ASSERT_MSG(fabsf(gotX - wantX) < kExact &&
                          fabsf(gotY - wantY) < kExact,
                          "%.2fx square (%d,%d): number box at (%.4f,%.4f), "
                          "expected the square's corner (%.4f,%.4f)",
                          (double)zs, kItems[i].mapX, kItems[i].mapY,
                          (double)gotX, (double)gotY,
                          (double)wantX, (double)wantY);
        }
    }
    UT_ASSERT_MSG(withheld == 2 && drawn == 5,
                  "ladder: %d rungs withheld and %d drawn, expected 2 and 5",
                  withheld, drawn);

    return 0;
}
