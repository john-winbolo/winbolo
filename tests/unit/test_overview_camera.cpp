/*
 * Tests for the map overview's camera maths (overview_camera.cpp) — the
 * renderer-free half of the overview view. Nothing here touches ImGui, an
 * SDL_Renderer or a ClientSim: a camera is a POD, and every check is state
 * in, numbers out.
 */

#include <cmath>

#include "test_harness.h"
#include "overview_camera.h"

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* The continuous world point under a view pixel, straight from the
 * coordinate model overview_camera.h documents. overviewCameraScreenToWorld
 * floors this to a whole square; the cursor-anchor checks need the
 * sub-square part as well. */
static float worldUnderPixelX(const OverviewCamera *cam, int viewW, float sx) {
    return (sx - (float)viewW * 0.5f) /
               ((float)OVERVIEW_TILE_PX * overviewCameraZoomScale(cam)) +
           cam->cx;
}

static float worldUnderPixelY(const OverviewCamera *cam, int viewH, float sy) {
    return (sy - (float)viewH * 0.5f) /
               ((float)OVERVIEW_TILE_PX * overviewCameraZoomScale(cam)) +
           cam->cy;
}

/* A square's centre survives the trip out to a view pixel and back, at every
 * ladder step. The centre rather than the corner keeps a stray ulp from
 * landing the answer on a square boundary. */
static int camera_round_trip_every_zoom(void) {
    const int viewW = 800;
    const int viewH = 600;
    static const int kSquares[] = { 0, 1, 37, 128, 200, 254, 255 };
    static const struct { float cx, cy; } kCentres[] = {
        { 128.0f, 128.0f },  /* the map centre overviewCameraInit picks */
        {  73.25f, 190.75f } /* off centre, and not on a square boundary */
    };

    for (int c = 0; c < ARRAY_LEN(kCentres); c++) {
        for (int z = 0; z < overviewCameraZoomCount(); z++) {
            for (int i = 0; i < ARRAY_LEN(kSquares); i++) {
                for (int j = 0; j < ARRAY_LEN(kSquares); j++) {
                    OverviewCamera cam;
                    overviewCameraInit(&cam);
                    cam.follow = false;
                    cam.zoomIndex = z;
                    cam.cx = kCentres[c].cx;
                    cam.cy = kCentres[c].cy;

                    int mx = kSquares[i];
                    int my = kSquares[j];
                    float sx = 0.0f;
                    float sy = 0.0f;
                    overviewCameraWorldToScreen(&cam, viewW, viewH,
                                                (float)mx + 0.5f,
                                                (float)my + 0.5f, &sx, &sy);

                    int gotX = -1;
                    int gotY = -1;
                    bool onMap = overviewCameraScreenToWorld(&cam, viewW, viewH,
                                                             sx, sy,
                                                             &gotX, &gotY);
                    UT_ASSERT_MSG(onMap,
                                  "zoom index %d (%.2fx), centre (%.2f,%.2f): "
                                  "square (%d,%d) centre -> pixel (%.4f,%.4f) "
                                  "came back as off-map (%d,%d)",
                                  z, (double)overviewCameraZoomScale(&cam),
                                  (double)cam.cx, (double)cam.cy, mx, my,
                                  (double)sx, (double)sy, gotX, gotY);
                    UT_ASSERT_MSG(gotX == mx && gotY == my,
                                  "zoom index %d (%.2fx), centre (%.2f,%.2f): "
                                  "square (%d,%d) round-tripped through pixel "
                                  "(%.4f,%.4f) to (%d,%d)",
                                  z, (double)overviewCameraZoomScale(&cam),
                                  (double)cam.cx, (double)cam.cy, mx, my,
                                  (double)sx, (double)sy, gotX, gotY);
                }
            }
        }
    }
    return 0;
}

/* Zooming holds the world point under the cursor still — and does not, when
 * follow owns the centre. */
static int camera_zoom_anchors_cursor(void) {
    const int viewW = 800;
    const int viewH = 600;
    /* World units. The arithmetic is exact at these magnitudes; this is
     * headroom for the two divisions, not a fudge factor. */
    const float kEps = 1.0e-3f;
    static const struct { float x, y; } kCursors[] = {
        {   0.0f,   0.0f },
        { 200.0f, 150.0f },
        { 400.0f, 300.0f }, /* dead centre: the anchor is the centre */
        { 799.0f, 599.0f }
    };
    /* Well inside the map at every ladder step, so the centre clamp never
     * gets a say in where the anchor ends up. */
    const float startX = 100.0f;
    const float startY = 90.0f;

    for (int i = 0; i < ARRAY_LEN(kCursors); i++) {
        for (int dir = -1; dir <= 1; dir += 2) {
            OverviewCamera cam;
            overviewCameraInit(&cam);
            cam.follow = false;
            cam.cx = startX;
            cam.cy = startY;

            float wantX = worldUnderPixelX(&cam, viewW, kCursors[i].x);
            float wantY = worldUnderPixelY(&cam, viewH, kCursors[i].y);
            int startZoom = cam.zoomIndex;

            overviewCameraZoomAt(&cam, viewW, viewH,
                                 kCursors[i].x, kCursors[i].y, dir);
            UT_ASSERT_MSG(cam.zoomIndex == startZoom + dir,
                          "cursor (%.1f,%.1f): %+d step from zoom index %d "
                          "landed on %d, expected %d",
                          (double)kCursors[i].x, (double)kCursors[i].y, dir,
                          startZoom, cam.zoomIndex, startZoom + dir);

            float gotX = worldUnderPixelX(&cam, viewW, kCursors[i].x);
            float gotY = worldUnderPixelY(&cam, viewH, kCursors[i].y);
            UT_ASSERT_MSG(std::fabs(gotX - wantX) < kEps,
                          "cursor (%.1f,%.1f): after %+d step to zoom index %d "
                          "(%.2fx) the world x under it moved from %.6f to "
                          "%.6f",
                          (double)kCursors[i].x, (double)kCursors[i].y, dir,
                          cam.zoomIndex, (double)overviewCameraZoomScale(&cam),
                          (double)wantX, (double)gotX);
            UT_ASSERT_MSG(std::fabs(gotY - wantY) < kEps,
                          "cursor (%.1f,%.1f): after %+d step to zoom index %d "
                          "(%.2fx) the world y under it moved from %.6f to "
                          "%.6f",
                          (double)kCursors[i].x, (double)kCursors[i].y, dir,
                          cam.zoomIndex, (double)overviewCameraZoomScale(&cam),
                          (double)wantY, (double)gotY);

            /* Step back the other way: same anchor, and the centre returns
             * to where it started. */
            overviewCameraZoomAt(&cam, viewW, viewH,
                                 kCursors[i].x, kCursors[i].y, -dir);
            UT_ASSERT_MSG(cam.zoomIndex == startZoom,
                          "cursor (%.1f,%.1f): %+d then %+d left zoom index "
                          "%d, expected %d",
                          (double)kCursors[i].x, (double)kCursors[i].y, dir,
                          -dir, cam.zoomIndex, startZoom);

            gotX = worldUnderPixelX(&cam, viewW, kCursors[i].x);
            gotY = worldUnderPixelY(&cam, viewH, kCursors[i].y);
            UT_ASSERT_MSG(std::fabs(gotX - wantX) < kEps,
                          "cursor (%.1f,%.1f): after %+d then %+d the world x "
                          "under it moved from %.6f to %.6f",
                          (double)kCursors[i].x, (double)kCursors[i].y, dir,
                          -dir, (double)wantX, (double)gotX);
            UT_ASSERT_MSG(std::fabs(gotY - wantY) < kEps,
                          "cursor (%.1f,%.1f): after %+d then %+d the world y "
                          "under it moved from %.6f to %.6f",
                          (double)kCursors[i].x, (double)kCursors[i].y, dir,
                          -dir, (double)wantY, (double)gotY);
            UT_ASSERT_MSG(std::fabs(cam.cx - startX) < kEps &&
                              std::fabs(cam.cy - startY) < kEps,
                          "cursor (%.1f,%.1f): after %+d then %+d the centre "
                          "moved from (%.6f,%.6f) to (%.6f,%.6f)",
                          (double)kCursors[i].x, (double)kCursors[i].y, dir,
                          -dir, (double)startX, (double)startY,
                          (double)cam.cx, (double)cam.cy);
        }
    }

    /* Follow owns the centre, so an off-centre cursor must not shift it. */
    for (int i = 0; i < ARRAY_LEN(kCursors); i++) {
        OverviewCamera cam;
        overviewCameraInit(&cam); /* follow on */
        cam.cx = startX;
        cam.cy = startY;
        int startZoom = cam.zoomIndex;

        overviewCameraZoomAt(&cam, viewW, viewH,
                             kCursors[i].x, kCursors[i].y, 1);
        UT_ASSERT_MSG(cam.zoomIndex == startZoom + 1,
                      "following, cursor (%.1f,%.1f): zoom index %d, "
                      "expected %d",
                      (double)kCursors[i].x, (double)kCursors[i].y,
                      cam.zoomIndex, startZoom + 1);
        UT_ASSERT_MSG(cam.cx == startX && cam.cy == startY,
                      "following, cursor (%.1f,%.1f): zoom to index %d moved "
                      "the centre from (%.6f,%.6f) to (%.6f,%.6f)",
                      (double)kCursors[i].x, (double)kCursors[i].y,
                      cam.zoomIndex, (double)startX, (double)startY,
                      (double)cam.cx, (double)cam.cy);
        UT_ASSERT_MSG(cam.follow,
                      "following, cursor (%.1f,%.1f): zoom to index %d cleared "
                      "follow",
                      (double)kCursors[i].x, (double)kCursors[i].y,
                      cam.zoomIndex);
    }
    return 0;
}

/* Centre-on-tank puts the tank's square centre at the view centre and turns
 * follow on; the per-frame tick does the same without touching the flag, and
 * nothing at all once the flag is off. */
static int camera_follow_centres_on_tank(void) {
    const int viewW = 800;
    const int viewH = 600;
    /* Far enough from every edge that the centre clamp is not in play at any
     * ladder step. */
    const int tankMX = 120;
    const int tankMY = 130;

    for (int z = 0; z < overviewCameraZoomCount(); z++) {
        OverviewCamera cam;
        overviewCameraInit(&cam);
        cam.follow = false;
        cam.zoomIndex = z;
        cam.cx = 10.0f;
        cam.cy = 240.0f;

        overviewCameraCenterOnTank(&cam, viewW, viewH,
                                   (float)tankMX + 0.5f, (float)tankMY + 0.5f);
        UT_ASSERT_MSG(cam.follow,
                      "zoom index %d: centre-on-tank (%d,%d) left follow off",
                      z, tankMX, tankMY);

        float sx = 0.0f;
        float sy = 0.0f;
        overviewCameraWorldToScreen(&cam, viewW, viewH,
                                    (float)tankMX + 0.5f,
                                    (float)tankMY + 0.5f, &sx, &sy);
        UT_ASSERT_MSG(std::fabs(sx - (float)viewW * 0.5f) <= 1.0f,
                      "zoom index %d (%.2fx): tank (%d,%d) drew at x %.4f, "
                      "expected the view centre %.4f",
                      z, (double)overviewCameraZoomScale(&cam), tankMX, tankMY,
                      (double)sx, (double)((float)viewW * 0.5f));
        UT_ASSERT_MSG(std::fabs(sy - (float)viewH * 0.5f) <= 1.0f,
                      "zoom index %d (%.2fx): tank (%d,%d) drew at y %.4f, "
                      "expected the view centre %.4f",
                      z, (double)overviewCameraZoomScale(&cam), tankMX, tankMY,
                      (double)sy, (double)((float)viewH * 0.5f));

        /* The tank moves on; the tick follows it — at sub-square precision,
         * so the centre lands exactly on the position handed in. */
        overviewCameraFollowTick(&cam, viewW, viewH, 60.25f, 200.75f);
        UT_ASSERT_MSG(cam.cx == 60.25f && cam.cy == 200.75f,
                      "zoom index %d: follow tick to (60.25,200.75) put the "
                      "centre at (%.4f,%.4f)",
                      z, (double)cam.cx, (double)cam.cy);
        UT_ASSERT_MSG(cam.follow,
                      "zoom index %d: follow tick cleared follow", z);
    }

    /* A drag turns follow off, and the tick stops moving the camera. */
    OverviewCamera cam;
    overviewCameraInit(&cam);
    overviewCameraPan(&cam, viewW, viewH, 32.0f, -48.0f);
    UT_ASSERT_MSG(!cam.follow, "pan did not clear follow");

    cam.cx = 10.0f;
    cam.cy = 20.0f;
    overviewCameraFollowTick(&cam, viewW, viewH, 200.5f, 30.5f);
    UT_ASSERT_MSG(cam.cx == 10.0f && cam.cy == 20.0f,
                  "follow off: tick to (200.5,30.5) moved the centre from "
                  "(10.0,20.0) to (%.4f,%.4f)",
                  (double)cam.cx, (double)cam.cy);
    UT_ASSERT_MSG(!cam.follow, "follow off: tick turned follow back on");
    return 0;
}

/* The nudge that keeps the tank on screen once the view has stopped following
 * it: the least move that brings it back inside, with a margin of ground on
 * the edge it came in over, and nothing at all while it is already inside. */
static int camera_keeps_tank_on_screen(void) {
    const int viewW = 800;
    const int viewH = 600;

    for (int z = 0; z < overviewCameraZoomCount(); z++) {
        OverviewCamera cam;
        overviewCameraInit(&cam);
        cam.follow = false;
        cam.zoomIndex = z;

        float tilePx = (float)OVERVIEW_TILE_PX * overviewCameraZoomScale(&cam);
        float halfW  = (float)viewW / (2.0f * tilePx);
        float halfH  = (float)viewH / (2.0f * tilePx);
        /* Well clear of both map edges at every rung, so what is asserted is
         * the nudge and never the map clamp underneath it. */
        const float tankX = 128.5f;
        const float tankY = 128.5f;

        /* A tank comfortably inside is left where it is, camera untouched. */
        cam.cx = tankX + halfW * 0.25f;
        cam.cy = tankY - halfH * 0.25f;
        float wasX = cam.cx;
        float wasY = cam.cy;
        overviewCameraKeepTankOnScreen(&cam, viewW, viewH, tankX, tankY);
        UT_ASSERT_MSG(cam.cx == wasX && cam.cy == wasY,
                      "zoom index %d: a tank already on screen moved the "
                      "centre from (%.4f,%.4f) to (%.4f,%.4f)",
                      z, (double)wasX, (double)wasY, (double)cam.cx,
                      (double)cam.cy);

        /* The view is a long way right of the tank, so the tank is off to the
         * left: the centre comes left only as far as it has to, which is the
         * margin inside the left edge — not onto the tank. */
        if (halfW > OVERVIEW_TANK_EDGE_MARGIN) {
            cam.cx = tankX + halfW * 3.0f;
            cam.cy = tankY;
            overviewCameraKeepTankOnScreen(&cam, viewW, viewH, tankX, tankY);

            float want = tankX + halfW - OVERVIEW_TANK_EDGE_MARGIN;
            UT_ASSERT_MSG(std::fabs(cam.cx - want) < 0.001f,
                          "zoom index %d: a tank off to the left put the "
                          "centre at %.4f, expected %.4f",
                          z, (double)cam.cx, (double)want);
            UT_ASSERT_MSG(cam.cx != tankX,
                          "zoom index %d: the nudge centred on the tank", z);

            /* Which is to say: on screen, that far in from the edge it came
             * in over. */
            float sx = 0.0f;
            overviewCameraWorldToScreen(&cam, viewW, viewH, tankX, tankY, &sx,
                                        nullptr);
            UT_ASSERT_MSG(
                sx >= 0.0f && sx <= (float)viewW,
                "zoom index %d: the nudged tank drew at x %.4f, outside the "
                "view", z, (double)sx);
            UT_ASSERT_MSG(
                std::fabs(sx - OVERVIEW_TANK_EDGE_MARGIN * tilePx) < 1.0f,
                "zoom index %d: the nudged tank drew %.4f px from the left "
                "edge, expected %.4f",
                z, (double)sx,
                (double)(OVERVIEW_TANK_EDGE_MARGIN * tilePx));
        }

        /* And the other three edges, each in the direction that has to move. */
        if (halfW > OVERVIEW_TANK_EDGE_MARGIN) {
            cam.cx = tankX - halfW * 3.0f;
            cam.cy = tankY;
            overviewCameraKeepTankOnScreen(&cam, viewW, viewH, tankX, tankY);
            float want = tankX - halfW + OVERVIEW_TANK_EDGE_MARGIN;
            UT_ASSERT_MSG(std::fabs(cam.cx - want) < 0.001f,
                          "zoom index %d: a tank off to the right put the "
                          "centre at %.4f, expected %.4f",
                          z, (double)cam.cx, (double)want);
        }
        if (halfH > OVERVIEW_TANK_EDGE_MARGIN) {
            cam.cx = tankX;
            cam.cy = tankY + halfH * 3.0f;
            overviewCameraKeepTankOnScreen(&cam, viewW, viewH, tankX, tankY);
            float want = tankY + halfH - OVERVIEW_TANK_EDGE_MARGIN;
            UT_ASSERT_MSG(std::fabs(cam.cy - want) < 0.001f,
                          "zoom index %d: a tank off the top put the centre at "
                          "%.4f, expected %.4f",
                          z, (double)cam.cy, (double)want);

            cam.cy = tankY - halfH * 3.0f;
            overviewCameraKeepTankOnScreen(&cam, viewW, viewH, tankX, tankY);
            want = tankY - halfH + OVERVIEW_TANK_EDGE_MARGIN;
            UT_ASSERT_MSG(std::fabs(cam.cy - want) < 0.001f,
                          "zoom index %d: a tank off the bottom put the centre "
                          "at %.4f, expected %.4f",
                          z, (double)cam.cy, (double)want);
        }

        /* Follow is not claimed on the way past — the nudge corrects a free
         * camera, it does not take it over. */
        UT_ASSERT_MSG(!cam.follow, "zoom index %d: the nudge turned follow on",
                      z);
    }

    /* A view too small to hold both margins has no centre that satisfies
     * either edge, and puts the tank in the middle rather than favouring one.
     * Sixteen pixels at 0.5x is one square across, well inside two. */
    {
        OverviewCamera cam;
        overviewCameraInit(&cam);
        cam.follow = false;
        cam.zoomIndex = 0;
        cam.cx = 200.0f;
        cam.cy = 40.0f;
        overviewCameraKeepTankOnScreen(&cam, 16, 16, 128.5f, 128.5f);
        UT_ASSERT_MSG(cam.cx == 128.5f && cam.cy == 128.5f,
                      "a view smaller than the margins put the centre at "
                      "(%.4f,%.4f), expected the tank",
                      (double)cam.cx, (double)cam.cy);
    }

    /* A tank against the map's own edge: the map clamp has the last word, and
     * it must not undo the nudge — the tank still has to be on screen. */
    {
        OverviewCamera cam;
        overviewCameraInit(&cam);
        cam.follow = false;
        cam.zoomIndex = overviewCameraZoomCount() - 1;
        cam.cx = 200.0f;
        cam.cy = 200.0f;
        overviewCameraKeepTankOnScreen(&cam, viewW, viewH, 0.5f, 0.5f);

        float sx = 0.0f;
        float sy = 0.0f;
        overviewCameraWorldToScreen(&cam, viewW, viewH, 0.5f, 0.5f, &sx, &sy);
        UT_ASSERT_MSG(sx >= 0.0f && sx <= (float)viewW && sy >= 0.0f &&
                          sy <= (float)viewH,
                      "a tank in the map's corner drew at (%.4f,%.4f), outside "
                      "the view", (double)sx, (double)sy);
    }
    return 0;
}

/* Zoom steps stop at the ladder's ends and pans stop at the map's. */
static int camera_clamps_zoom_and_centre(void) {
    const int viewW = 800;
    const int viewH = 600;

    for (int z = 0; z < overviewCameraZoomCount(); z++) {
        for (int steps = -20; steps <= 20; steps += 5) {
            OverviewCamera cam;
            overviewCameraInit(&cam);
            cam.follow = false;
            cam.zoomIndex = z;

            overviewCameraZoomAt(&cam, viewW, viewH, 250.0f, 175.0f, steps);
            UT_ASSERT_MSG(cam.zoomIndex >= 0 &&
                              cam.zoomIndex < overviewCameraZoomCount(),
                          "zoom index %d %+d steps landed on %d, expected "
                          "0..%d",
                          z, steps, cam.zoomIndex,
                          overviewCameraZoomCount() - 1);

            float scale = overviewCameraZoomScale(&cam);
            UT_ASSERT_MSG(scale >= 0.5f && scale <= 4.0f,
                          "zoom index %d %+d steps gave scale %.4f at index "
                          "%d, expected 0.5..4.0",
                          z, steps, (double)scale, cam.zoomIndex);
            UT_ASSERT_MSG(cam.cx >= 0.0f && cam.cx <= (float)MAP_ARRAY_SIZE &&
                              cam.cy >= 0.0f &&
                              cam.cy <= (float)MAP_ARRAY_SIZE,
                          "zoom index %d %+d steps put the centre at "
                          "(%.4f,%.4f), expected 0..%d in both axes",
                          z, steps, (double)cam.cx, (double)cam.cy,
                          MAP_ARRAY_SIZE);
        }
    }

    /* The two ends by name. */
    OverviewCamera cam;
    overviewCameraInit(&cam);
    cam.follow = false;
    overviewCameraZoomAt(&cam, viewW, viewH, 400.0f, 300.0f, -100);
    UT_ASSERT_MSG(cam.zoomIndex == 0,
                  "-100 steps from index 2 landed on %d, expected 0",
                  cam.zoomIndex);
    UT_ASSERT_MSG(overviewCameraZoomScale(&cam) == 0.5f,
                  "zoom index %d gave scale %.4f, expected the 0.5 floor",
                  cam.zoomIndex, (double)overviewCameraZoomScale(&cam));

    overviewCameraZoomAt(&cam, viewW, viewH, 400.0f, 300.0f, 100);
    UT_ASSERT_MSG(cam.zoomIndex == overviewCameraZoomCount() - 1,
                  "+100 steps from index 0 landed on %d, expected %d",
                  cam.zoomIndex, overviewCameraZoomCount() - 1);
    UT_ASSERT_MSG(overviewCameraZoomScale(&cam) == 4.0f,
                  "zoom index %d gave scale %.4f, expected the 4.0 ceiling",
                  cam.zoomIndex, (double)overviewCameraZoomScale(&cam));

    /* A pan far past each edge stops the centre on the map's bounds — the
     * view may hang half a window off, never more. */
    static const struct {
        float dx, dy, wantX, wantY;
        const char *what;
    } kPans[] = {
        { -1.0e6f,     0.0f,   0.0f, 128.0f, "far left"  },
        {  1.0e6f,     0.0f, 256.0f, 128.0f, "far right" },
        {     0.0f, -1.0e6f, 128.0f,   0.0f, "far up"    },
        {     0.0f,  1.0e6f, 128.0f, 256.0f, "far down"  }
    };
    for (int i = 0; i < ARRAY_LEN(kPans); i++) {
        OverviewCamera panned;
        overviewCameraInit(&panned);
        overviewCameraPan(&panned, viewW, viewH, kPans[i].dx, kPans[i].dy);
        UT_ASSERT_MSG(!panned.follow,
                      "%s pan did not clear follow", kPans[i].what);
        UT_ASSERT_MSG(panned.cx == kPans[i].wantX &&
                          panned.cy == kPans[i].wantY,
                      "%s pan (%.1f,%.1f px) put the centre at (%.4f,%.4f), "
                      "expected (%.1f,%.1f)",
                      kPans[i].what, (double)kPans[i].dx, (double)kPans[i].dy,
                      (double)panned.cx, (double)panned.cy,
                      (double)kPans[i].wantX, (double)kPans[i].wantY);
    }
    return 0;
}

/* The square range a view covers, worked out by hand at the map's edges and
 * in its middle. */
static int camera_visible_range_at_edges(void) {
    const int viewW = 640;
    const int viewH = 640;
    int left = -1;
    int top = -1;
    int right = -1;
    int bottom = -1;

    /* 640 px window at 1x: a square is OVERVIEW_TILE_PX * 1.0 = 16 px, so
     * the view is 640 / 16 = 40 squares across and the half-span is 20.
     * Centred on (100,100) it covers world x in [80,120): squares 80..119.
     * Square 120 begins exactly on the far edge and draws no pixels. */
    OverviewCamera cam;
    overviewCameraInit(&cam);
    cam.follow = false;
    cam.zoomIndex = 2; /* 1.0x */
    cam.cx = 100.0f;
    cam.cy = 100.0f;
    UT_ASSERT_MSG(overviewCameraVisibleRange(&cam, viewW, viewH,
                                             &left, &top, &right, &bottom),
                  "interior camera (%.1f,%.1f) at zoom index %d in a %dx%d "
                  "view reported nothing visible",
                  (double)cam.cx, (double)cam.cy, cam.zoomIndex, viewW, viewH);
    UT_ASSERT_MSG(left == 80 && right == 119,
                  "interior camera (%.1f,%.1f) at zoom index %d: x range "
                  "%d..%d, expected 80..119",
                  (double)cam.cx, (double)cam.cy, cam.zoomIndex, left, right);
    UT_ASSERT_MSG(top == 80 && bottom == 119,
                  "interior camera (%.1f,%.1f) at zoom index %d: y range "
                  "%d..%d, expected 80..119",
                  (double)cam.cx, (double)cam.cy, cam.zoomIndex, top, bottom);

    /* Panned hard into the top-left. The clamp stops the centre at (0,0), so
     * the view covers [-20,20): squares 0..19 once the off-map half is
     * dropped. */
    overviewCameraInit(&cam);
    cam.zoomIndex = 2;
    overviewCameraPan(&cam, viewW, viewH, -1.0e6f, -1.0e6f);
    UT_ASSERT_MSG(overviewCameraVisibleRange(&cam, viewW, viewH,
                                             &left, &top, &right, &bottom),
                  "top-left camera (%.1f,%.1f) reported nothing visible",
                  (double)cam.cx, (double)cam.cy);
    UT_ASSERT_MSG(left == 0 && top == 0,
                  "top-left camera (%.1f,%.1f): range starts at (%d,%d), "
                  "expected (0,0)",
                  (double)cam.cx, (double)cam.cy, left, top);
    UT_ASSERT_MSG(right == 19 && bottom == 19,
                  "top-left camera (%.1f,%.1f): range ends at (%d,%d), "
                  "expected (19,19)",
                  (double)cam.cx, (double)cam.cy, right, bottom);

    /* And hard into the bottom-right. The centre stops at (256,256), so the
     * view covers [236,276): squares 236..255 after the clamp. */
    overviewCameraInit(&cam);
    cam.zoomIndex = 2;
    overviewCameraPan(&cam, viewW, viewH, 1.0e6f, 1.0e6f);
    UT_ASSERT_MSG(overviewCameraVisibleRange(&cam, viewW, viewH,
                                             &left, &top, &right, &bottom),
                  "bottom-right camera (%.1f,%.1f) reported nothing visible",
                  (double)cam.cx, (double)cam.cy);
    UT_ASSERT_MSG(right == MAP_ARRAY_SIZE - 1 && bottom == MAP_ARRAY_SIZE - 1,
                  "bottom-right camera (%.1f,%.1f): range ends at (%d,%d), "
                  "expected (%d,%d)",
                  (double)cam.cx, (double)cam.cy, right, bottom,
                  MAP_ARRAY_SIZE - 1, MAP_ARRAY_SIZE - 1);
    UT_ASSERT_MSG(left == 236 && top == 236,
                  "bottom-right camera (%.1f,%.1f): range starts at (%d,%d), "
                  "expected (236,236)",
                  (double)cam.cx, (double)cam.cy, left, top);

    /* A view with no pixels in it has no squares in it either. */
    overviewCameraInit(&cam);
    UT_ASSERT_MSG(!overviewCameraVisibleRange(&cam, 0, viewH,
                                              &left, &top, &right, &bottom),
                  "a 0x%d view reported squares %d..%d, %d..%d visible",
                  viewH, left, right, top, bottom);
    UT_ASSERT_MSG(!overviewCameraVisibleRange(&cam, viewW, 0,
                                              &left, &top, &right, &bottom),
                  "a %dx0 view reported squares %d..%d, %d..%d visible",
                  viewW, left, right, top, bottom);
    return 0;
}

/* Setting the zoom by scale is the inverse of reading it: every rung on the
   ladder survives a round trip, a scale off either end lands on that end, and
   a scale between two rungs takes the nearer one. This is the conversion the
   saved [WINDOW] Overview Zoom preference goes through on the way back in. */
static int camera_set_zoom_scale(void) {
    OverviewCamera cam;
    overviewCameraInit(&cam);

    for (int i = 0; i < overviewCameraZoomCount(); i++) {
        OverviewCamera restored;
        overviewCameraInit(&restored);
        cam.zoomIndex = i;
        float s = overviewCameraZoomScale(&cam);
        overviewCameraSetZoomScale(&restored, s);
        UT_ASSERT_MSG(overviewCameraZoomScale(&restored) == s,
                      "zoom index %d has scale %.4f, but setting that scale "
                      "gave back %.4f",
                      i, (double)s, (double)overviewCameraZoomScale(&restored));
    }

    /* Below the ladder's floor. */
    overviewCameraSetZoomScale(&cam, 0.1f);
    UT_ASSERT_MSG(overviewCameraZoomScale(&cam) == 0.5f,
                  "scale 0.1 clamped to %.4f, expected 0.5",
                  (double)overviewCameraZoomScale(&cam));

    /* Above its ceiling. */
    overviewCameraSetZoomScale(&cam, 99.0f);
    UT_ASSERT_MSG(overviewCameraZoomScale(&cam) == 4.0f,
                  "scale 99 clamped to %.4f, expected 4.0",
                  (double)overviewCameraZoomScale(&cam));

    /* Between 1.0 and 1.5, and nearer 1.0: 0.2 below against 0.3 above. */
    overviewCameraSetZoomScale(&cam, 1.2f);
    UT_ASSERT_MSG(overviewCameraZoomScale(&cam) == 1.0f,
                  "scale 1.2 snapped to %.4f, expected 1.0",
                  (double)overviewCameraZoomScale(&cam));

    /* A NULL camera is a no-op, like the rest of the camera calls. */
    overviewCameraSetZoomScale(NULL, 1.0f);
    return 0;
}

extern "C" int run_overview_camera(void) {
    int rc;
    rc = camera_round_trip_every_zoom();    if (rc) return rc;
    rc = camera_zoom_anchors_cursor();      if (rc) return rc;
    rc = camera_follow_centres_on_tank();   if (rc) return rc;
    rc = camera_keeps_tank_on_screen();     if (rc) return rc;
    rc = camera_clamps_zoom_and_centre();   if (rc) return rc;
    rc = camera_visible_range_at_edges();   if (rc) return rc;
    rc = camera_set_zoom_scale();           if (rc) return rc;
    return 0;
}
