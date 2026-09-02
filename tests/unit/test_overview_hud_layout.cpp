/*
 * Tests for the in-window map overview's HUD geometry
 * (overview_hud_layout.cpp) — where the classic status panels and the
 * newswire land once the map owns the whole game window. Nothing here touches
 * SDL, ImGui or a ClientSim: a window size goes in and rectangles come out.
 */

#include <cmath>

#include "test_harness.h"
#include "overview_hud_layout.h"

/* Layout policy, mirroring overview_hud_layout.cpp. The checks below restate
 * the rules rather than hand-copying the numbers they produce. */
static const int   kGap       = 4;     /* source pixels between rows and pairs */
static const int   kMargin    = 8;     /* window pixels around the column */
static const int   kStripPad  = 4;     /* window pixels around the newswire slice */
static const float kMinScale  = 0.75f;
static const float kMaxShare  = 0.30f; /* of the width the column may take */

/* Everything in the column, in stacking order. The newswire is deliberately
 * absent: it lives on its own strip at the bottom. */
static const int kColumn[] = {
    OVERVIEW_HUD_MANSTATUS, OVERVIEW_HUD_KILLSDEATHS,
    OVERVIEW_HUD_TANKS, OVERVIEW_HUD_PILLS, OVERVIEW_HUD_BASES,
    OVERVIEW_HUD_BASEBARS, OVERVIEW_HUD_TANKBARS, OVERVIEW_HUD_BUILDSELECT
};
#define COLUMN_LEN ((int)(sizeof(kColumn) / sizeof(kColumn[0])))

static const char *kNames[OVERVIEW_HUD_COUNT] = {
    "man status", "kills/deaths", "tanks", "pills", "bases",
    "base bars", "tank bars", "build select", "newswire"
};

static int maxInt(int a, int b) { return (a > b) ? a : b; }

/* The column's source height, re-derived from the rows the mockup stacks: the
 * LGM pair, the three grids, the base bars, then the tank-bars/build-select
 * pair, with a gap between every row. */
static int columnSrcHeight(const OverviewHudLayout *lay) {
    return maxInt(lay->el[OVERVIEW_HUD_MANSTATUS].srcH,
                  lay->el[OVERVIEW_HUD_KILLSDEATHS].srcH) + kGap
         + lay->el[OVERVIEW_HUD_TANKS].srcH    + kGap
         + lay->el[OVERVIEW_HUD_PILLS].srcH    + kGap
         + lay->el[OVERVIEW_HUD_BASES].srcH    + kGap
         + lay->el[OVERVIEW_HUD_BASEBARS].srcH + kGap
         + maxInt(lay->el[OVERVIEW_HUD_TANKBARS].srcH,
                  lay->el[OVERVIEW_HUD_BUILDSELECT].srcH);
}

/* The widest row wins: the two pairs are as wide as both pieces plus the gap,
 * every other row as wide as its one piece. */
static int columnSrcWidth(const OverviewHudLayout *lay) {
    int w = lay->el[OVERVIEW_HUD_MANSTATUS].srcW + kGap
          + lay->el[OVERVIEW_HUD_KILLSDEATHS].srcW;
    w = maxInt(w, lay->el[OVERVIEW_HUD_TANKS].srcW);
    w = maxInt(w, lay->el[OVERVIEW_HUD_PILLS].srcW);
    w = maxInt(w, lay->el[OVERVIEW_HUD_BASES].srcW);
    w = maxInt(w, lay->el[OVERVIEW_HUD_BASEBARS].srcW);
    w = maxInt(w, lay->el[OVERVIEW_HUD_TANKBARS].srcW + kGap
                      + lay->el[OVERVIEW_HUD_BUILDSELECT].srcW);
    return w;
}

/* Fit the column and the newswire strip to the height together — the strip is
 * not laid over the column's last row — then hold the column to its share of
 * the width. */
static float expectedScale(const OverviewHudLayout *lay, int viewW, int viewH) {
    float fit = (float)(viewH - 2 * kMargin - 2 * kStripPad) /
                (float)(columnSrcHeight(lay) +
                        lay->el[OVERVIEW_HUD_NEWSWIRE].srcH);
    float cap = kMaxShare * (float)viewW / (float)columnSrcWidth(lay);
    return (fit > cap) ? cap : fit;
}

static bool nearly(float a, float b) { return fabsf(a - b) < 0.001f; }

/* Is the first rectangle wholly inside the second, allowing a rounding ulp? */
static bool rectInside(float x, float y, float w, float h,
                       float ox, float oy, float ow, float oh) {
    const float eps = 0.01f;
    return x >= ox - eps && y >= oy - eps &&
           x + w <= ox + ow + eps && y + h <= oy + oh + eps;
}

/* 1080p less the menu bar: the column fits, and it stays inside the map. */
static int hud_layout_fits_1080p(void) {
    const int viewW = 1920;
    const int viewH = 1058;
    OverviewHudLayout lay;

    UT_ASSERT_MSG(overviewHudLayout(viewW, viewH, &lay),
                  "%dx%d should hold a legible column", viewW, viewH);

    float want = expectedScale(&lay, viewW, viewH);
    UT_ASSERT_MSG(nearly(lay.scale, want),
                  "scale %.4f, expected the height fit %.4f",
                  (double)lay.scale, (double)want);
    UT_ASSERT_MSG(lay.scale >= kMinScale,
                  "scale %.4f is below the legibility floor %.4f",
                  (double)lay.scale, (double)kMinScale);

    UT_ASSERT_MSG(lay.columnX + lay.columnW <= (float)viewW,
                  "column right edge %.2f is outside the %d px view",
                  (double)(lay.columnX + lay.columnW), viewW);
    UT_ASSERT_MSG(lay.columnY + lay.columnH <= (float)viewH,
                  "column bottom edge %.2f is outside the %d px view",
                  (double)(lay.columnY + lay.columnH), viewH);
    return 0;
}

/* The Steam Deck's 800 lines with the menu bar hidden. */
static int hud_layout_fits_deck(void) {
    const int viewW = 1280;
    const int viewH = 800;
    OverviewHudLayout lay;

    UT_ASSERT_MSG(overviewHudLayout(viewW, viewH, &lay),
                  "%dx%d should hold a legible column", viewW, viewH);

    float want = expectedScale(&lay, viewW, viewH);
    UT_ASSERT_MSG(nearly(lay.scale, want),
                  "scale %.4f, expected the height fit %.4f",
                  (double)lay.scale, (double)want);
    UT_ASSERT_MSG(lay.scale >= kMinScale,
                  "scale %.4f is below the legibility floor %.4f",
                  (double)lay.scale, (double)kMinScale);
    return 0;
}

/* Top to bottom in the mockup's order, with the two pairs side by side. */
static int hud_layout_stacks_in_order(void) {
    OverviewHudLayout lay;
    UT_ASSERT(overviewHudLayout(1920, 1058, &lay));

    static const int kDown[] = {
        OVERVIEW_HUD_TANKS, OVERVIEW_HUD_PILLS,
        OVERVIEW_HUD_BASES, OVERVIEW_HUD_BASEBARS
    };
    for (int i = 1; i < (int)(sizeof(kDown) / sizeof(kDown[0])); i++) {
        UT_ASSERT_MSG(lay.el[kDown[i]].dstY > lay.el[kDown[i - 1]].dstY,
                      "%s at y %.2f is not below %s at y %.2f",
                      kNames[kDown[i]], (double)lay.el[kDown[i]].dstY,
                      kNames[kDown[i - 1]], (double)lay.el[kDown[i - 1]].dstY);
    }

    UT_ASSERT_MSG(lay.el[OVERVIEW_HUD_KILLSDEATHS].dstX >
                      lay.el[OVERVIEW_HUD_MANSTATUS].dstX,
                  "kills/deaths at x %.2f is not right of the man status at %.2f",
                  (double)lay.el[OVERVIEW_HUD_KILLSDEATHS].dstX,
                  (double)lay.el[OVERVIEW_HUD_MANSTATUS].dstX);
    UT_ASSERT_MSG(nearly(lay.el[OVERVIEW_HUD_KILLSDEATHS].dstY,
                         lay.el[OVERVIEW_HUD_MANSTATUS].dstY),
                  "kills/deaths at y %.2f does not share the man status row at %.2f",
                  (double)lay.el[OVERVIEW_HUD_KILLSDEATHS].dstY,
                  (double)lay.el[OVERVIEW_HUD_MANSTATUS].dstY);

    UT_ASSERT_MSG(lay.el[OVERVIEW_HUD_BUILDSELECT].dstX >
                      lay.el[OVERVIEW_HUD_TANKBARS].dstX,
                  "build select at x %.2f is not right of the tank bars at %.2f",
                  (double)lay.el[OVERVIEW_HUD_BUILDSELECT].dstX,
                  (double)lay.el[OVERVIEW_HUD_TANKBARS].dstX);
    UT_ASSERT_MSG(nearly(lay.el[OVERVIEW_HUD_BUILDSELECT].dstY,
                         lay.el[OVERVIEW_HUD_TANKBARS].dstY),
                  "build select at y %.2f does not share the tank bars row at %.2f",
                  (double)lay.el[OVERVIEW_HUD_BUILDSELECT].dstY,
                  (double)lay.el[OVERVIEW_HUD_TANKBARS].dstY);

    UT_ASSERT_MSG(lay.el[OVERVIEW_HUD_TANKS].dstY >
                      lay.el[OVERVIEW_HUD_MANSTATUS].dstY,
                  "the tanks grid at y %.2f is not below the man status row at %.2f",
                  (double)lay.el[OVERVIEW_HUD_TANKS].dstY,
                  (double)lay.el[OVERVIEW_HUD_MANSTATUS].dstY);
    UT_ASSERT_MSG(lay.el[OVERVIEW_HUD_TANKBARS].dstY >
                      lay.el[OVERVIEW_HUD_BASEBARS].dstY,
                  "the tank bars at y %.2f are not below the base bars at %.2f",
                  (double)lay.el[OVERVIEW_HUD_TANKBARS].dstY,
                  (double)lay.el[OVERVIEW_HUD_BASEBARS].dstY);
    return 0;
}

/* Nothing in the column is drawn over anything else in it, and nothing in it
 * is drawn under the newswire strip. */
static int hud_layout_column_never_overlaps(void) {
    static const struct { int w, h; } kSizes[] = {
        { 1920, 1058 }, { 1280, 800 }, { 2560, 1400 }, { 1024, 600 }
    };

    for (int s = 0; s < (int)(sizeof(kSizes) / sizeof(kSizes[0])); s++) {
        OverviewHudLayout lay;
        UT_ASSERT_MSG(overviewHudLayout(kSizes[s].w, kSizes[s].h, &lay),
                      "%dx%d should hold a legible column",
                      kSizes[s].w, kSizes[s].h);

        for (int i = 0; i < COLUMN_LEN; i++) {
            for (int j = i + 1; j < COLUMN_LEN; j++) {
                const OverviewHudElement *a = &lay.el[kColumn[i]];
                const OverviewHudElement *b = &lay.el[kColumn[j]];
                bool apart = a->dstX + a->dstW <= b->dstX ||
                             b->dstX + b->dstW <= a->dstX ||
                             a->dstY + a->dstH <= b->dstY ||
                             b->dstY + b->dstH <= a->dstY;
                UT_ASSERT_MSG(apart,
                              "at %dx%d, %s (%.2f,%.2f %.2fx%.2f) overlaps "
                              "%s (%.2f,%.2f %.2fx%.2f)",
                              kSizes[s].w, kSizes[s].h,
                              kNames[kColumn[i]], (double)a->dstX, (double)a->dstY,
                              (double)a->dstW, (double)a->dstH,
                              kNames[kColumn[j]], (double)b->dstX, (double)b->dstY,
                              (double)b->dstW, (double)b->dstH);
            }
        }

        /* The strip is fitted with the column rather than laid on top of it,
           so the backing rectangles never meet. */
        UT_ASSERT_MSG(lay.columnY + lay.columnH <= lay.newswireY,
                      "at %dx%d, the column backing ends at y %.2f, below the "
                      "newswire strip's top edge at %.2f",
                      kSizes[s].w, kSizes[s].h,
                      (double)(lay.columnY + lay.columnH),
                      (double)lay.newswireY);

        for (int i = 0; i < COLUMN_LEN; i++) {
            const OverviewHudElement *e = &lay.el[kColumn[i]];
            UT_ASSERT_MSG(e->dstY + e->dstH <= lay.newswireY,
                          "at %dx%d, %s (%.2f,%.2f %.2fx%.2f) runs into the "
                          "newswire strip, which starts at y %.2f",
                          kSizes[s].w, kSizes[s].h, kNames[kColumn[i]],
                          (double)e->dstX, (double)e->dstY,
                          (double)e->dstW, (double)e->dstH,
                          (double)lay.newswireY);
        }
    }
    return 0;
}

/* The backing strip covers every piece it is drawn behind, and itself sits
 * inside the map rect. */
static int hud_layout_backing_contains_column(void) {
    const int viewW = 1920;
    const int viewH = 1058;
    OverviewHudLayout lay;
    UT_ASSERT(overviewHudLayout(viewW, viewH, &lay));

    UT_ASSERT_MSG(rectInside(lay.columnX, lay.columnY, lay.columnW, lay.columnH,
                             0.0f, 0.0f, (float)viewW, (float)viewH),
                  "column backing (%.2f,%.2f %.2fx%.2f) leaves the %dx%d map rect",
                  (double)lay.columnX, (double)lay.columnY,
                  (double)lay.columnW, (double)lay.columnH, viewW, viewH);

    for (int i = 0; i < COLUMN_LEN; i++) {
        const OverviewHudElement *e = &lay.el[kColumn[i]];
        UT_ASSERT_MSG(rectInside(e->dstX, e->dstY, e->dstW, e->dstH,
                                 lay.columnX, lay.columnY,
                                 lay.columnW, lay.columnH),
                      "%s (%.2f,%.2f %.2fx%.2f) leaves the column backing "
                      "(%.2f,%.2f %.2fx%.2f)",
                      kNames[kColumn[i]], (double)e->dstX, (double)e->dstY,
                      (double)e->dstW, (double)e->dstH,
                      (double)lay.columnX, (double)lay.columnY,
                      (double)lay.columnW, (double)lay.columnH);
    }
    return 0;
}

/* The newswire strip runs the full width along the bottom edge, and holds its
 * slice with the pad above it. */
static int hud_layout_newswire_strip(void) {
    static const struct { int w, h; } kSizes[] = { { 1920, 1058 }, { 1280, 800 } };

    for (int s = 0; s < (int)(sizeof(kSizes) / sizeof(kSizes[0])); s++) {
        const int viewW = kSizes[s].w;
        const int viewH = kSizes[s].h;
        OverviewHudLayout lay;
        UT_ASSERT(overviewHudLayout(viewW, viewH, &lay));

        UT_ASSERT_MSG(nearly(lay.newswireX, 0.0f) &&
                          nearly(lay.newswireW, (float)viewW),
                      "newswire strip spans %.2f..%.2f, not the full %d px width",
                      (double)lay.newswireX,
                      (double)(lay.newswireX + lay.newswireW), viewW);
        UT_ASSERT_MSG(nearly(lay.newswireY + lay.newswireH, (float)viewH),
                      "newswire strip ends at %.2f, not the %d px bottom edge",
                      (double)(lay.newswireY + lay.newswireH), viewH);

        const OverviewHudElement *e = &lay.el[OVERVIEW_HUD_NEWSWIRE];
        UT_ASSERT_MSG(rectInside(e->dstX, e->dstY, e->dstW, e->dstH,
                                 lay.newswireX, lay.newswireY,
                                 lay.newswireW, lay.newswireH),
                      "newswire slice (%.2f,%.2f %.2fx%.2f) leaves its strip "
                      "(%.2f,%.2f %.2fx%.2f)",
                      (double)e->dstX, (double)e->dstY,
                      (double)e->dstW, (double)e->dstH,
                      (double)lay.newswireX, (double)lay.newswireY,
                      (double)lay.newswireW, (double)lay.newswireH);
        UT_ASSERT_MSG(nearly(e->dstY, lay.newswireY + (float)kStripPad),
                      "newswire slice sits at y %.2f, expected %.2f",
                      (double)e->dstY,
                      (double)(lay.newswireY + (float)kStripPad));
    }
    return 0;
}

/* Too small to read: no layout, and the caller's struct is left alone. */
static int hud_layout_rejects_tiny_window(void) {
    OverviewHudLayout lay;
    lay.scale = -1.0f;
    lay.columnX = lay.columnY = lay.columnW = lay.columnH = -1.0f;
    lay.newswireX = lay.newswireY = lay.newswireW = lay.newswireH = -1.0f;

    UT_ASSERT_MSG(!overviewHudLayout(320, 200, &lay),
                  "320x200 is far too small for a legible column");
    UT_ASSERT_MSG(lay.scale == -1.0f && lay.columnW == -1.0f &&
                      lay.newswireW == -1.0f,
                  "a rejected layout wrote into the caller's struct "
                  "(scale %.4f, columnW %.2f, newswireW %.2f)",
                  (double)lay.scale, (double)lay.columnW,
                  (double)lay.newswireW);

    UT_ASSERT_MSG(!overviewHudLayout(1920, 1058, NULL),
                  "a NULL out pointer should be refused");
    UT_ASSERT_MSG(!overviewHudLayout(0, 0, &lay),
                  "a degenerate view size should be refused");
    return 0;
}

/* Every slice is cut from inside the 515x325 classic layout. */
static int hud_layout_source_rects_in_bounds(void) {
    OverviewHudLayout lay;
    UT_ASSERT(overviewHudLayout(1920, 1058, &lay));

    for (int i = 0; i < OVERVIEW_HUD_COUNT; i++) {
        const OverviewHudElement *e = &lay.el[i];
        UT_ASSERT_MSG(e->srcX >= 0 && e->srcY >= 0 && e->srcW > 0 && e->srcH > 0,
                      "%s has a degenerate source rect (%d,%d %dx%d)",
                      kNames[i], e->srcX, e->srcY, e->srcW, e->srcH);
        UT_ASSERT_MSG(e->srcX + e->srcW <= OVERVIEW_HUD_SRC_W,
                      "%s runs to source x %d, past the %d px layout width",
                      kNames[i], e->srcX + e->srcW, OVERVIEW_HUD_SRC_W);
        UT_ASSERT_MSG(e->srcY + e->srcH <= OVERVIEW_HUD_SRC_H,
                      "%s runs to source y %d, past the %d px layout height",
                      kNames[i], e->srcY + e->srcH, OVERVIEW_HUD_SRC_H);
    }
    return 0;
}

extern "C" int run_overview_hud_layout(void) {
    int rc;
    rc = hud_layout_fits_1080p();             if (rc) return rc;
    rc = hud_layout_fits_deck();              if (rc) return rc;
    rc = hud_layout_stacks_in_order();        if (rc) return rc;
    rc = hud_layout_column_never_overlaps();  if (rc) return rc;
    rc = hud_layout_backing_contains_column();if (rc) return rc;
    rc = hud_layout_newswire_strip();         if (rc) return rc;
    rc = hud_layout_rejects_tiny_window();    if (rc) return rc;
    rc = hud_layout_source_rects_in_bounds(); if (rc) return rc;
    return 0;
}
