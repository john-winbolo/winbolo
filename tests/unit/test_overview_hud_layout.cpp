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
static const int   kDividerH  = 3;     /* the ridge between the base and tank bars */
static const int   kMargin    = 8;     /* window pixels around the column */
static const int   kStripPad  = 4;     /* window pixels around the newswire slice */
static const float kMinScale  = 0.75f;
static const float kMaxShare  = 0.30f; /* of the width the column may take */

/* Everything in the column, in stacking order. The newswire and the build
 * select are deliberately absent: each lives on a strip of its own, the
 * newswire across the bottom and the build select down the left edge. */
static const int kColumn[] = {
    OVERVIEW_HUD_MANSTATUS, OVERVIEW_HUD_KILLSDEATHS,
    OVERVIEW_HUD_TANKS, OVERVIEW_HUD_PILLS, OVERVIEW_HUD_BASES,
    OVERVIEW_HUD_BASEBARS, OVERVIEW_HUD_TANKBARS, OVERVIEW_HUD_VOICE
};
#define COLUMN_LEN ((int)(sizeof(kColumn) / sizeof(kColumn[0])))

static const char *kNames[OVERVIEW_HUD_COUNT] = {
    "man status", "kills/deaths", "tanks", "pills", "bases",
    "base bars", "tank bars", "build select", "newswire", "voice"
};

static int maxInt(int a, int b) { return (a > b) ? a : b; }

/* The column's source height, re-derived from the rows the mockup stacks: the
 * LGM pair, the three grids, the base bars, the divider ridge in its own slot,
 * then the tank bars, with a gap between every row. The microphone follows
 * them on the same gap when it was asked for; a layout without it is that row
 * shorter, which is what its zero source height says here. */
static int columnSrcHeight(const OverviewHudLayout *lay) {
    int h = maxInt(lay->el[OVERVIEW_HUD_MANSTATUS].srcH,
                   lay->el[OVERVIEW_HUD_KILLSDEATHS].srcH) + kGap
          + lay->el[OVERVIEW_HUD_TANKS].srcH    + kGap
          + lay->el[OVERVIEW_HUD_PILLS].srcH    + kGap
          + lay->el[OVERVIEW_HUD_BASES].srcH    + kGap
          + lay->el[OVERVIEW_HUD_BASEBARS].srcH + kGap
          + kDividerH                           + kGap
          + lay->el[OVERVIEW_HUD_TANKBARS].srcH;
    if (lay->el[OVERVIEW_HUD_VOICE].srcH > 0) {
        h += kGap + lay->el[OVERVIEW_HUD_VOICE].srcH;
    }
    return h;
}

/* The widest row wins: the LGM pair is as wide as both pieces plus the gap,
 * every other row as wide as its one piece. */
static int columnSrcWidth(const OverviewHudLayout *lay) {
    int w = lay->el[OVERVIEW_HUD_MANSTATUS].srcW + kGap
          + lay->el[OVERVIEW_HUD_KILLSDEATHS].srcW;
    w = maxInt(w, lay->el[OVERVIEW_HUD_TANKS].srcW);
    w = maxInt(w, lay->el[OVERVIEW_HUD_PILLS].srcW);
    w = maxInt(w, lay->el[OVERVIEW_HUD_BASES].srcW);
    w = maxInt(w, lay->el[OVERVIEW_HUD_BASEBARS].srcW);
    w = maxInt(w, lay->el[OVERVIEW_HUD_TANKBARS].srcW);
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

/* Do the two rectangles keep clear of each other on at least one axis? */
static bool rectsApart(float ax, float ay, float aw, float ah,
                       float bx, float by, float bw, float bh) {
    const float eps = 0.01f;
    return ax + aw <= bx + eps || bx + bw <= ax + eps ||
           ay + ah <= by + eps || by + bh <= ay + eps;
}

/* 1080p less the menu bar: the column fits, and it stays inside the map. */
static int hud_layout_fits_1080p(void) {
    const int viewW = 1920;
    const int viewH = 1058;
    OverviewHudLayout lay;

    UT_ASSERT_MSG(overviewHudLayout(viewW, viewH, true, &lay),
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

    UT_ASSERT_MSG(overviewHudLayout(viewW, viewH, true, &lay),
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

/* Top to bottom in the mockup's order, with the LGM pair side by side. */
static int hud_layout_stacks_in_order(void) {
    OverviewHudLayout lay;
    UT_ASSERT(overviewHudLayout(1920, 1058, true, &lay));

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

/* Nothing in the column is drawn over anything else in it, and the column's
 * backing keeps clear of the newswire strip it shares the bottom band with. */
static int hud_layout_column_never_overlaps(void) {
    static const struct { int w, h; } kSizes[] = {
        { 1920, 1058 }, { 1280, 800 }, { 2560, 1400 }, { 1024, 600 }
    };

    for (int s = 0; s < (int)(sizeof(kSizes) / sizeof(kSizes[0])); s++) {
        OverviewHudLayout lay;
        UT_ASSERT_MSG(overviewHudLayout(kSizes[s].w, kSizes[s].h, true, &lay),
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

        /* The column shares the bottom band with the newswire strip — the
           column anchored to the bottom-right corner, the centred strip
           trimmed to stop short of it — so the two backings must be kept
           apart as rectangles, not stacked. */
        UT_ASSERT_MSG(rectsApart(lay.columnX, lay.columnY,
                                 lay.columnW, lay.columnH,
                                 lay.newswireX, lay.newswireY,
                                 lay.newswireW, lay.newswireH),
                      "at %dx%d, the column backing (%.2f,%.2f %.2fx%.2f) "
                      "overlaps the newswire strip (%.2f,%.2f %.2fx%.2f)",
                      kSizes[s].w, kSizes[s].h,
                      (double)lay.columnX, (double)lay.columnY,
                      (double)lay.columnW, (double)lay.columnH,
                      (double)lay.newswireX, (double)lay.newswireY,
                      (double)lay.newswireW, (double)lay.newswireH);
    }
    return 0;
}

/* The backing strip covers every piece it is drawn behind, and itself sits
 * inside the map rect. */
static int hud_layout_backing_contains_column(void) {
    const int viewW = 1920;
    const int viewH = 1058;
    OverviewHudLayout lay;
    UT_ASSERT(overviewHudLayout(viewW, viewH, true, &lay));

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

/* The build select stands on the left edge, vertically centred, and clear of
 * everything else drawn over the map. */
static int hud_layout_build_strip(void) {
    static const struct { int w, h; } kSizes[] = {
        { 1920, 1058 }, { 1280, 800 }, { 2560, 1400 }, { 1024, 600 }
    };

    for (int s = 0; s < (int)(sizeof(kSizes) / sizeof(kSizes[0])); s++) {
        const int viewW = kSizes[s].w;
        const int viewH = kSizes[s].h;
        OverviewHudLayout lay;
        UT_ASSERT_MSG(overviewHudLayout(viewW, viewH, true, &lay),
                      "%dx%d should hold a legible column", viewW, viewH);

        UT_ASSERT_MSG(nearly(lay.buildX, (float)kMargin),
                      "at %dx%d, the build strip starts at x %.2f, not the "
                      "%d px left margin",
                      viewW, viewH, (double)lay.buildX, kMargin);
        UT_ASSERT_MSG(nearly(lay.buildY, ((float)viewH - lay.buildH) * 0.5f),
                      "at %dx%d, the build strip at y %.2f is not centred on "
                      "the view height (expected %.2f)",
                      viewW, viewH, (double)lay.buildY,
                      (double)(((float)viewH - lay.buildH) * 0.5f));
        UT_ASSERT_MSG(lay.buildX + lay.buildW <= lay.columnX,
                      "at %dx%d, the build strip ends at x %.2f, past the "
                      "column's left edge at %.2f",
                      viewW, viewH, (double)(lay.buildX + lay.buildW),
                      (double)lay.columnX);
        UT_ASSERT_MSG(rectsApart(lay.buildX, lay.buildY,
                                 lay.buildW, lay.buildH,
                                 lay.newswireX, lay.newswireY,
                                 lay.newswireW, lay.newswireH),
                      "at %dx%d, the build strip (%.2f,%.2f %.2fx%.2f) "
                      "overlaps the newswire strip (%.2f,%.2f %.2fx%.2f)",
                      viewW, viewH, (double)lay.buildX, (double)lay.buildY,
                      (double)lay.buildW, (double)lay.buildH,
                      (double)lay.newswireX, (double)lay.newswireY,
                      (double)lay.newswireW, (double)lay.newswireH);
        UT_ASSERT_MSG(rectInside(lay.buildX, lay.buildY, lay.buildW, lay.buildH,
                                 0.0f, 0.0f, (float)viewW, (float)viewH),
                      "at %dx%d, the build strip (%.2f,%.2f %.2fx%.2f) leaves "
                      "the map rect",
                      viewW, viewH, (double)lay.buildX, (double)lay.buildY,
                      (double)lay.buildW, (double)lay.buildH);

        /* The strip's own slice is what the backing is drawn behind. */
        const OverviewHudElement *e = &lay.el[OVERVIEW_HUD_BUILDSELECT];
        UT_ASSERT_MSG(rectInside(e->dstX, e->dstY, e->dstW, e->dstH,
                                 lay.buildX, lay.buildY,
                                 lay.buildW, lay.buildH),
                      "at %dx%d, the build slice (%.2f,%.2f %.2fx%.2f) leaves "
                      "its backing (%.2f,%.2f %.2fx%.2f)",
                      viewW, viewH, (double)e->dstX, (double)e->dstY,
                      (double)e->dstW, (double)e->dstH,
                      (double)lay.buildX, (double)lay.buildY,
                      (double)lay.buildW, (double)lay.buildH);
    }
    return 0;
}

/* Five clickable items, evenly stacked down the strip and nothing outside
 * them. */
static int hud_layout_build_items(void) {
    const int viewW = 1920;
    const int viewH = 1058;
    OverviewHudLayout lay;
    UT_ASSERT(overviewHudLayout(viewW, viewH, true, &lay));

    float x[5], y[5], w[5], h[5];
    for (int i = 0; i < 5; i++) {
        UT_ASSERT_MSG(overviewHudBuildItemRect(&lay, i, &x[i], &y[i], &w[i], &h[i]),
                      "build item %d should have a rect", i);
        UT_ASSERT_MSG(rectInside(x[i], y[i], w[i], h[i],
                                 lay.buildX, lay.buildY, lay.buildW, lay.buildH),
                      "build item %d (%.2f,%.2f %.2fx%.2f) leaves the strip "
                      "(%.2f,%.2f %.2fx%.2f)",
                      i, (double)x[i], (double)y[i], (double)w[i], (double)h[i],
                      (double)lay.buildX, (double)lay.buildY,
                      (double)lay.buildW, (double)lay.buildH);
    }

    for (int i = 1; i < 5; i++) {
        UT_ASSERT_MSG(y[i] > y[i - 1],
                      "build item %d at y %.2f is not below item %d at y %.2f",
                      i, (double)y[i], i - 1, (double)y[i - 1]);
        UT_ASSERT_MSG(y[i - 1] + h[i - 1] <= y[i] + 0.01f,
                      "build item %d ends at y %.2f, overlapping item %d at "
                      "y %.2f",
                      i - 1, (double)(y[i - 1] + h[i - 1]), i, (double)y[i]);
        UT_ASSERT_MSG(nearly(x[i], x[i - 1]) && nearly(w[i], w[i - 1]) &&
                          nearly(h[i], h[i - 1]),
                      "build item %d (%.2f,%.2fx%.2f) is not the same size and "
                      "column as item %d (%.2f,%.2fx%.2f)",
                      i, (double)x[i], (double)w[i], (double)h[i],
                      i - 1, (double)x[i - 1], (double)w[i - 1],
                      (double)h[i - 1]);
    }

    float ox = 0.0f, oy = 0.0f, ow = 0.0f, oh = 0.0f;
    UT_ASSERT_MSG(!overviewHudBuildItemRect(&lay, -1, &ox, &oy, &ow, &oh),
                  "index -1 should have no rect");
    UT_ASSERT_MSG(!overviewHudBuildItemRect(&lay, 5, &ox, &oy, &ow, &oh),
                  "index 5 is past the five build items and should have no rect");
    return 0;
}

/* The newswire strip sits centred on the bottom edge, just wide enough for
 * its slice, and holds the slice with the pad above it. */
static int hud_layout_newswire_strip(void) {
    static const struct { int w, h; } kSizes[] = { { 1920, 1058 }, { 1280, 800 } };

    for (int s = 0; s < (int)(sizeof(kSizes) / sizeof(kSizes[0])); s++) {
        const int viewW = kSizes[s].w;
        const int viewH = kSizes[s].h;
        OverviewHudLayout lay;
        UT_ASSERT(overviewHudLayout(viewW, viewH, true, &lay));

        const OverviewHudElement *news = &lay.el[OVERVIEW_HUD_NEWSWIRE];
        UT_ASSERT_MSG(nearly(lay.newswireX,
                             ((float)viewW - lay.newswireW) * 0.5f),
                      "newswire strip at x %.2f is not centred on the %d px "
                      "width (expected %.2f)",
                      (double)lay.newswireX, viewW,
                      (double)(((float)viewW - lay.newswireW) * 0.5f));
        UT_ASSERT_MSG(nearly(lay.newswireW,
                             news->dstW + 2.0f * (float)kStripPad),
                      "newswire strip is %.2f wide, not its %.2f px slice "
                      "plus the pad",
                      (double)lay.newswireW,
                      (double)(news->dstW + 2.0f * (float)kStripPad));
        UT_ASSERT_MSG(lay.newswireX + lay.newswireW <= lay.columnX + 0.01f,
                      "newswire strip ends at x %.2f, past the column's left "
                      "edge at %.2f",
                      (double)(lay.newswireX + lay.newswireW),
                      (double)lay.columnX);
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

    UT_ASSERT_MSG(!overviewHudLayout(320, 200, false, &lay),
                  "320x200 is far too small for a legible column");
    UT_ASSERT_MSG(lay.scale == -1.0f && lay.columnW == -1.0f &&
                      lay.newswireW == -1.0f,
                  "a rejected layout wrote into the caller's struct "
                  "(scale %.4f, columnW %.2f, newswireW %.2f)",
                  (double)lay.scale, (double)lay.columnW,
                  (double)lay.newswireW);

    /* The microphone row only makes the column taller, so a size the shorter
       column cannot hold is refused with it too. */
    UT_ASSERT_MSG(!overviewHudLayout(320, 200, true, &lay),
                  "320x200 is far too small for a legible column, microphone "
                  "row or not");

    UT_ASSERT_MSG(!overviewHudLayout(1920, 1058, false, NULL),
                  "a NULL out pointer should be refused");
    UT_ASSERT_MSG(!overviewHudLayout(0, 0, false, &lay),
                  "a degenerate view size should be refused");
    return 0;
}

/* The microphone row: a square at the foot of the column, clear of the tank
 * bars above it and centred like them. */
static int hud_layout_voice_row(void) {
    const int viewW = 1920;
    const int viewH = 1058;
    OverviewHudLayout lay;
    UT_ASSERT(overviewHudLayout(viewW, viewH, true, &lay));

    const OverviewHudElement *voice = &lay.el[OVERVIEW_HUD_VOICE];
    const OverviewHudElement *bars  = &lay.el[OVERVIEW_HUD_TANKBARS];

    UT_ASSERT_MSG(nearly(voice->dstW, voice->dstH),
                  "the microphone is %.2f x %.2f, not square",
                  (double)voice->dstW, (double)voice->dstH);
    UT_ASSERT_MSG(nearly(voice->dstW,
                         (float)voice->srcW * lay.scale),
                  "the microphone is %.2f px wide, not its %d source pixels "
                  "at the column's scale (%.2f)",
                  (double)voice->dstW, voice->srcW,
                  (double)((float)voice->srcW * lay.scale));

    UT_ASSERT_MSG(voice->dstY > bars->dstY,
                  "the microphone at y %.2f is not below the tank bars at "
                  "y %.2f",
                  (double)voice->dstY, (double)bars->dstY);
    UT_ASSERT_MSG(rectsApart(voice->dstX, voice->dstY, voice->dstW, voice->dstH,
                             bars->dstX, bars->dstY, bars->dstW, bars->dstH),
                  "the microphone (%.2f,%.2f %.2fx%.2f) overlaps the tank bars "
                  "(%.2f,%.2f %.2fx%.2f)",
                  (double)voice->dstX, (double)voice->dstY,
                  (double)voice->dstW, (double)voice->dstH,
                  (double)bars->dstX, (double)bars->dstY,
                  (double)bars->dstW, (double)bars->dstH);

    UT_ASSERT_MSG(rectInside(voice->dstX, voice->dstY, voice->dstW, voice->dstH,
                             lay.columnX, lay.columnY,
                             lay.columnW, lay.columnH),
                  "the microphone (%.2f,%.2f %.2fx%.2f) leaves the column "
                  "backing (%.2f,%.2f %.2fx%.2f)",
                  (double)voice->dstX, (double)voice->dstY,
                  (double)voice->dstW, (double)voice->dstH,
                  (double)lay.columnX, (double)lay.columnY,
                  (double)lay.columnW, (double)lay.columnH);

    /* Centred to within the odd source pixel the integer halving leaves over,
       the same allowance the tank bars above it get. */
    float leftGap  = voice->dstX - lay.columnX;
    float rightGap = (lay.columnX + lay.columnW) - (voice->dstX + voice->dstW);
    UT_ASSERT_MSG(fabsf(leftGap - rightGap) <= lay.scale + 0.01f,
                  "the microphone is not centred in the column: %.2f px clear "
                  "to its left, %.2f px to its right",
                  (double)leftGap, (double)rightGap);
    return 0;
}

/* Without it the element carries nothing at all, and the column is laid out to
 * the shorter stack rather than keeping the row's space empty. */
static int hud_layout_voice_absent(void) {
    const int viewW = 1920;
    const int viewH = 1058;
    OverviewHudLayout lay;
    UT_ASSERT(overviewHudLayout(viewW, viewH, false, &lay));

    const OverviewHudElement *voice = &lay.el[OVERVIEW_HUD_VOICE];
    UT_ASSERT_MSG(voice->srcX == 0 && voice->srcY == 0 &&
                      voice->srcW == 0 && voice->srcH == 0,
                  "an unwanted microphone kept a source rect (%d,%d %dx%d)",
                  voice->srcX, voice->srcY, voice->srcW, voice->srcH);
    UT_ASSERT_MSG(voice->dstX == 0.0f && voice->dstY == 0.0f &&
                      voice->dstW == 0.0f && voice->dstH == 0.0f,
                  "an unwanted microphone kept a destination rect "
                  "(%.2f,%.2f %.2fx%.2f)",
                  (double)voice->dstX, (double)voice->dstY,
                  (double)voice->dstW, (double)voice->dstH);

    float want = expectedScale(&lay, viewW, viewH);
    UT_ASSERT_MSG(nearly(lay.scale, want),
                  "scale %.4f, expected the shorter column's height fit %.4f",
                  (double)lay.scale, (double)want);

    /* The same view with the row asked for has to fit at a smaller scale, or
       the row was never counted into the column. */
    OverviewHudLayout withVoice;
    UT_ASSERT(overviewHudLayout(viewW, viewH, true, &withVoice));
    UT_ASSERT_MSG(withVoice.scale < lay.scale,
                  "the microphone row left the scale at %.4f, the same as the "
                  "column without it (%.4f)",
                  (double)withVoice.scale, (double)lay.scale);
    return 0;
}

/* Every slice is cut from inside the 515x325 classic layout. */
static int hud_layout_source_rects_in_bounds(void) {
    OverviewHudLayout lay;
    UT_ASSERT(overviewHudLayout(1920, 1058, true, &lay));

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
    rc = hud_layout_build_strip();            if (rc) return rc;
    rc = hud_layout_build_items();            if (rc) return rc;
    rc = hud_layout_newswire_strip();         if (rc) return rc;
    rc = hud_layout_rejects_tiny_window();    if (rc) return rc;
    rc = hud_layout_source_rects_in_bounds(); if (rc) return rc;
    rc = hud_layout_voice_row();              if (rc) return rc;
    rc = hud_layout_voice_absent();           if (rc) return rc;
    return 0;
}
