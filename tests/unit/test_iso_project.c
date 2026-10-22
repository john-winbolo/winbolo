/*
 * The isometric projection (test_iso_project.c).
 *
 * iso_project.c is the arithmetic the isometric view places everything with:
 * where a map position lands on the target, what is under a screen point,
 * which squares a target can show, the order things draw in and the frame a
 * classic sprite stands in with. These cases pin each one.
 *
 * forward: with the camera on the map origin and a target of no size, the
 * origin lands at (0, 0); one square east at (16, 8), one square south at
 * (-16, 8) and a point 10 game pixels up at (0, -10), all times the scale. A
 * target of a size puts the camera's centre at its middle, and the square
 * the camera is centred on sits there.
 *
 * inverse: for a grid of squares and offsets, cameras and scales, the ground
 * position under a projected point is the position projected, within a
 * hundredth of a world unit, and the square and offset come back exactly.
 *
 * visible_range: a target the size of one diamond, centred on square 128,
 * reaches 127..129 on both axes; a target larger than the map clamps to
 * 0..255; a camera far off the map sees nothing. Every square whose diamond
 * overlaps an ordinary target is inside the range, and no square outside the
 * range does.
 *
 * depth: all ground is below anything standing; a tank on the square behind
 * a building draws before it and one on the square in front draws after; a
 * tank on a bridge draws after the bridge; a builder beside a pill is behind
 * it from the far side and in front of it from the near side, and on the
 * pill's own square draws after the pill and before the next row's tiles.
 *
 * facing_frame: a classic stand-in frame advances by two and wraps at 16; an
 * isometric sprite's frame is left alone.
 */

#include <math.h>

#include "test_harness.h"
#include "iso_project.h"

static const float kExact = 1.0e-3f;

static int projectAt(const IsoCamera *cam, int tw, int th,
                     int mx, int my, int ox, int oy, float h,
                     float wantX, float wantY, const char *what) {
    float sx = 12345.0f, sy = 12345.0f;
    isoProject(cam, tw, th, mx, my, ox, oy, h, &sx, &sy);
    UT_ASSERT_MSG(fabsf(sx - wantX) < kExact && fabsf(sy - wantY) < kExact,
                  "%s: square (%d,%d) off (%d,%d) height %g at scale %g -> "
                  "(%g, %g), want (%g, %g)",
                  what, mx, my, ox, oy, h, cam->scale, sx, sy, wantX, wantY);
    return 0;
}

int run_iso_project_forward(void) {
    static const float kScales[] = { 0.5f, 1.0f, 2.0f, 3.0f };
    int i;

    UT_ASSERT(fabsf(isoDiamondHalfWidth(1.0f) - 16.0f) < kExact);
    UT_ASSERT(fabsf(isoDiamondHalfWidth(2.5f) - 40.0f) < kExact);

    for (i = 0; i < (int)(sizeof(kScales) / sizeof(kScales[0])); i++) {
        float s = kScales[i];
        IsoCamera cam = { 0.0f, 0.0f, s };
        float t = isoDiamondHalfWidth(s);

        /* Camera on the map origin, a target with no size: the origin's
           projection is the screen origin. */
        if (projectAt(&cam, 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, "origin")) return 1;
        /* One square east: right by a half-width and down by a quarter of
           the diamond's width (half its height). */
        if (projectAt(&cam, 0, 0, 1, 0, 0, 0, 0.0f, t, t / 2.0f, "east")) return 1;
        /* One square south: left by a half-width and down the same. */
        if (projectAt(&cam, 0, 0, 0, 1, 0, 0, 0.0f, -t, t / 2.0f, "south")) return 1;
        /* One square south-east: straight down a diamond's height. */
        if (projectAt(&cam, 0, 0, 1, 1, 0, 0, 0.0f, 0.0f, t, "south-east")) return 1;
        /* Ten game pixels up: straight up the screen by ten times the scale. */
        if (projectAt(&cam, 0, 0, 0, 0, 0, 0, 10.0f, 0.0f, -10.0f * s, "height")) return 1;
        /* The middle of square 0: half a square along each axis, which is
           the diamond's centre, half its height below the top vertex. */
        if (projectAt(&cam, 0, 0, 0, 0, 128, 128, 0.0f, 0.0f, t / 2.0f, "centre")) return 1;
        /* A world offset east alone: a quarter square is a quarter of the
           east step. */
        if (projectAt(&cam, 0, 0, 0, 0, 64, 0, 0.0f, t / 4.0f, t / 8.0f, "offset")) return 1;

        /* isoProjectWorld is the same sum in world units. */
        {
            float sx = 0.0f, sy = 0.0f;
            isoProjectWorld(s, 256.0f, 0.0f, 0.0f, &sx, &sy);
            UT_ASSERT_MSG(fabsf(sx - t) < kExact && fabsf(sy - t / 2.0f) < kExact,
                          "world east at scale %g -> (%g, %g)", s, sx, sy);
            isoProjectWorld(s, 0.0f, 0.0f, 4.0f, &sx, &sy);
            UT_ASSERT_MSG(fabsf(sx) < kExact && fabsf(sy + 4.0f * s) < kExact,
                          "world height at scale %g -> (%g, %g)", s, sx, sy);
        }
    }

    /* A target of a size puts the camera's centre at its middle. */
    {
        IsoCamera cam = { 128.5f * 256.0f, 128.5f * 256.0f, 1.0f };
        if (projectAt(&cam, 320, 200, 128, 128, 128, 128, 0.0f,
                      160.0f, 100.0f, "centred square")) return 1;
        /* Its top vertex is half a diamond's height above the middle. */
        if (projectAt(&cam, 320, 200, 128, 128, 0, 0, 0.0f,
                      160.0f, 92.0f, "centred top vertex")) return 1;
        /* The square to the east is a half-width right and a quarter down. */
        if (projectAt(&cam, 320, 200, 129, 128, 128, 128, 0.0f,
                      176.0f, 108.0f, "centred east neighbour")) return 1;
    }

    /* A camera between two squares, as the smooth modes hand in. */
    {
        IsoCamera cam = { 10.25f * 256.0f, 20.75f * 256.0f, 2.0f };
        /* The point under the camera is at the middle whatever its square
           and offset: square (10,20) offset (64,192). */
        if (projectAt(&cam, 100, 60, 10, 20, 64, 192, 0.0f,
                      50.0f, 30.0f, "fractional camera")) return 1;
    }
    return 0;
}

int run_iso_project_inverse(void) {
    static const float kScales[] = { 0.5f, 1.0f, 1.5f, 3.0f };
    static const float kCentres[] = { 0.0f, 37.5f * 256.0f, 128.0f * 256.0f, 255.9f * 256.0f };
    const int tw = 640, th = 480;
    int si, ci, mx, my, ox, oy;

    for (si = 0; si < (int)(sizeof(kScales) / sizeof(kScales[0])); si++) {
        for (ci = 0; ci < (int)(sizeof(kCentres) / sizeof(kCentres[0])); ci++) {
            IsoCamera cam = { kCentres[ci], kCentres[(ci + 1) % 4], kScales[si] };
            for (mx = 0; mx < 256; mx += 17) {
                for (my = 0; my < 256; my += 23) {
                    for (ox = 0; ox < 256; ox += 51) {
                        for (oy = 0; oy < 256; oy += 85) {
                            float sx, sy, wx, wy;
                            float wantX = (float)(mx * 256 + ox);
                            float wantY = (float)(my * 256 + oy);
                            int gx, gy, gox, goy;
                            bool in;
                            isoProject(&cam, tw, th, mx, my, ox, oy, 0.0f, &sx, &sy);
                            in = isoUnproject(&cam, tw, th, sx, sy, &wx, &wy);
                            UT_ASSERT_MSG(in, "scale %g centre (%g,%g): square (%d,%d) "
                                          "off (%d,%d) came back off the map",
                                          cam.scale, cam.centerWX, cam.centerWY,
                                          mx, my, ox, oy);
                            UT_ASSERT_MSG(fabsf(wx - wantX) < 0.01f && fabsf(wy - wantY) < 0.01f,
                                          "scale %g centre (%g,%g): world (%g,%g) -> "
                                          "screen (%g,%g) -> world (%g,%g)",
                                          cam.scale, cam.centerWX, cam.centerWY,
                                          wantX, wantY, sx, sy, wx, wy);
                            in = isoUnprojectSquare(&cam, tw, th, sx, sy, &gx, &gy, &gox, &goy);
                            UT_ASSERT_MSG(in && gx == mx && gy == my && gox == ox && goy == oy,
                                          "scale %g centre (%g,%g): square (%d,%d) off (%d,%d) "
                                          "-> square (%d,%d) off (%d,%d)",
                                          cam.scale, cam.centerWX, cam.centerWY,
                                          mx, my, ox, oy, gx, gy, gox, goy);
                        }
                    }
                }
            }
        }
    }

    /* The last world unit of the map is on it; the first past it is not. */
    {
        IsoCamera cam = { 255.5f * 256.0f, 255.5f * 256.0f, 1.0f };
        float sx, sy, wx, wy;
        int gx, gy, gox, goy;
        isoProject(&cam, tw, th, 255, 255, 255, 255, 0.0f, &sx, &sy);
        UT_ASSERT(isoUnproject(&cam, tw, th, sx, sy, &wx, &wy));
        UT_ASSERT(isoUnprojectSquare(&cam, tw, th, sx, sy, &gx, &gy, &gox, &goy));
        UT_ASSERT(gx == 255 && gy == 255 && gox == 255 && goy == 255);
        isoProject(&cam, tw, th, 256, 255, 0, 0, 0.0f, &sx, &sy);
        UT_ASSERT(!isoUnproject(&cam, tw, th, sx, sy, &wx, &wy));
        UT_ASSERT_MSG(fabsf(wx - 256.0f * 256.0f) < 0.01f, "past the east edge: %g", wx);
        UT_ASSERT(!isoUnprojectSquare(&cam, tw, th, sx, sy, &gx, &gy, &gox, &goy));
        UT_ASSERT_MSG(gx == 255 && gox == 255 && gy == 255 && goy == 0,
                      "past the east edge clamps to (%d,%d) off (%d,%d)", gx, gy, gox, goy);
        isoProject(&cam, tw, th, -1, 0, 255, 0, 0.0f, &sx, &sy);
        UT_ASSERT(!isoUnprojectSquare(&cam, tw, th, sx, sy, &gx, &gy, &gox, &goy));
        UT_ASSERT_MSG(gx == 0 && gox == 0 && gy == 0 && goy == 0,
                      "past the west edge clamps to (%d,%d) off (%d,%d)", gx, gy, gox, goy);
    }
    return 0;
}

/* Whether square (mx, my)'s diamond, projected under cam, overlaps the
   target. Both are convex, so they overlap when their shadows overlap on
   each of the four edge directions: the target's edges are the screen axes,
   where the diamond's shadow is its bounding box, and the diamond's edges
   are the two ground axes, where the target's shadow is the span of its
   corners' ground positions. */
static bool diamondTouches(const IsoCamera *cam, int tw, int th, int mx, int my) {
    float cx[4], cy[4];
    float minX, maxX, minY, maxY;
    float minPX, maxPX, minPY, maxPY;
    int i;
    isoProject(cam, tw, th, mx, my, 0, 0, 0.0f, &cx[0], &cy[0]);       /* top */
    isoProject(cam, tw, th, mx + 1, my, 0, 0, 0.0f, &cx[1], &cy[1]);   /* right */
    isoProject(cam, tw, th, mx + 1, my + 1, 0, 0, 0.0f, &cx[2], &cy[2]); /* bottom */
    isoProject(cam, tw, th, mx, my + 1, 0, 0, 0.0f, &cx[3], &cy[3]);   /* left */
    minX = maxX = cx[0];
    minY = maxY = cy[0];
    for (i = 1; i < 4; i++) {
        if (cx[i] < minX) minX = cx[i];
        if (cx[i] > maxX) maxX = cx[i];
        if (cy[i] < minY) minY = cy[i];
        if (cy[i] > maxY) maxY = cy[i];
    }
    if (maxX < 0.0f || minX > (float)tw || maxY < 0.0f || minY > (float)th) return false;

    minPX = minPY = 1.0e9f;
    maxPX = maxPY = -1.0e9f;
    for (i = 0; i < 4; i++) {
        float wx, wy, px, py;
        isoUnproject(cam, tw, th, (i & 1) ? (float)tw : 0.0f,
                     (i & 2) ? (float)th : 0.0f, &wx, &wy);
        px = wx / 256.0f;
        py = wy / 256.0f;
        if (px < minPX) minPX = px;
        if (px > maxPX) maxPX = px;
        if (py < minPY) minPY = py;
        if (py > maxPY) maxPY = py;
    }
    return (float)(mx + 1) >= minPX && (float)mx <= maxPX &&
           (float)(my + 1) >= minPY && (float)my <= maxPY;
}

int run_iso_project_visible_range(void) {
    int minX, minY, maxX, maxY;

    /* A target exactly one diamond, centred on square 128: the diamond's
       bounding rectangle reaches the four neighbours' diamonds and, at its
       corners, the squares along the row, so 127..129 on both axes. */
    {
        IsoCamera cam = { 128.5f * 256.0f, 128.5f * 256.0f, 1.0f };
        UT_ASSERT(isoVisibleRange(&cam, 32, 16, &minX, &minY, &maxX, &maxY));
        UT_ASSERT_MSG(minX == 127 && maxX == 129 && minY == 127 && maxY == 129,
                      "one diamond: x %d..%d y %d..%d", minX, maxX, minY, maxY);
    }

    /* A target larger than the whole map clamps to the map. */
    {
        IsoCamera cam = { 128.0f * 256.0f, 128.0f * 256.0f, 1.0f };
        UT_ASSERT(isoVisibleRange(&cam, 100000, 100000, &minX, &minY, &maxX, &maxY));
        UT_ASSERT_MSG(minX == 0 && maxX == 255 && minY == 0 && maxY == 255,
                      "whole map: x %d..%d y %d..%d", minX, maxX, minY, maxY);
    }

    /* A camera at the map's corner shows only the squares on the map. */
    {
        IsoCamera cam = { 0.0f, 0.0f, 1.0f };
        UT_ASSERT(isoVisibleRange(&cam, 320, 200, &minX, &minY, &maxX, &maxY));
        UT_ASSERT_MSG(minX == 0 && minY == 0 && maxX > 0 && maxY > 0 && maxX < 255,
                      "corner: x %d..%d y %d..%d", minX, maxX, minY, maxY);
    }

    /* A camera far off the map sees nothing. */
    {
        IsoCamera cam = { -100.0f * 256.0f, -100.0f * 256.0f, 1.0f };
        UT_ASSERT(!isoVisibleRange(&cam, 320, 200, &minX, &minY, &maxX, &maxY));
        UT_ASSERT_MSG(minX > maxX && minY > maxY,
                      "off the map: x %d..%d y %d..%d", minX, maxX, minY, maxY);
    }

    /* Every square whose diamond overlaps the target is in the range, and
       none outside it overlaps, at several cameras and scales. */
    {
        static const IsoCamera kCams[] = {
            { 128.5f * 256.0f, 128.5f * 256.0f, 1.0f },
            { 40.3f * 256.0f, 200.7f * 256.0f, 0.5f },
            { 10.0f * 256.0f, 10.0f * 256.0f, 3.0f },
            { 250.0f * 256.0f, 3.0f * 256.0f, 2.0f },
        };
        const int tw = 320, th = 200;
        int c, mx, my;
        for (c = 0; c < (int)(sizeof(kCams) / sizeof(kCams[0])); c++) {
            const IsoCamera *cam = &kCams[c];
            UT_ASSERT(isoVisibleRange(cam, tw, th, &minX, &minY, &maxX, &maxY));
            UT_ASSERT(minX >= 0 && minY >= 0 && maxX <= 255 && maxY <= 255);
            for (mx = 0; mx < 256; mx++) {
                for (my = 0; my < 256; my++) {
                    bool inRange = mx >= minX && mx <= maxX && my >= minY && my <= maxY;
                    bool touches = diamondTouches(cam, tw, th, mx, my);
                    UT_ASSERT_MSG(!touches || inRange,
                                  "camera %d: square (%d,%d) touches the target but is "
                                  "outside x %d..%d y %d..%d",
                                  c, mx, my, minX, maxX, minY, maxY);
                }
            }
            /* The range is a bounding range: its edges hold a square that
               touches, so it is not padded past what the target shows. */
            {
                bool edgeTouches = false;
                for (my = minY; my <= maxY && !edgeTouches; my++)
                    edgeTouches = diamondTouches(cam, tw, th, minX, my);
                UT_ASSERT_MSG(edgeTouches, "camera %d: nothing on column %d touches", c, minX);
                edgeTouches = false;
                for (my = minY; my <= maxY && !edgeTouches; my++)
                    edgeTouches = diamondTouches(cam, tw, th, maxX, my);
                UT_ASSERT_MSG(edgeTouches, "camera %d: nothing on column %d touches", c, maxX);
                edgeTouches = false;
                for (mx = minX; mx <= maxX && !edgeTouches; mx++)
                    edgeTouches = diamondTouches(cam, tw, th, mx, minY);
                UT_ASSERT_MSG(edgeTouches, "camera %d: nothing on row %d touches", c, minY);
                edgeTouches = false;
                for (mx = minX; mx <= maxX && !edgeTouches; mx++)
                    edgeTouches = diamondTouches(cam, tw, th, mx, maxY);
                UT_ASSERT_MSG(edgeTouches, "camera %d: nothing on row %d touches", c, maxY);
            }
        }
    }
    return 0;
}

int run_iso_project_depth(void) {
    /* All ground is below anything standing, wherever either is. */
    UT_ASSERT(isoDepth(255, 255, ISO_DEPTH_GROUND) < isoDepth(0, 0, ISO_DEPTH_TALL));
    UT_ASSERT(isoDepth(255, 255, ISO_DEPTH_GROUND) < isoDepth(0, 0, ISO_DEPTH_SPRITE));

    /* A tank behind a building: the tank on (5,4) is one row further from
       the camera than the building on (5,5), so it draws first and the
       building covers it. A tank on (5,6) is a row nearer and draws over
       the building. Beside it on (4,5) is the same row as (5,4). */
    UT_ASSERT(isoDepth(5, 4, ISO_DEPTH_SPRITE) < isoDepth(5, 5, ISO_DEPTH_TALL));
    UT_ASSERT(isoDepth(4, 5, ISO_DEPTH_SPRITE) < isoDepth(5, 5, ISO_DEPTH_TALL));
    UT_ASSERT(isoDepth(5, 6, ISO_DEPTH_SPRITE) > isoDepth(5, 5, ISO_DEPTH_TALL));
    UT_ASSERT(isoDepth(6, 5, ISO_DEPTH_SPRITE) > isoDepth(5, 5, ISO_DEPTH_TALL));

    /* A tank on a bridge square: the bridge is ground and draws first, and
       the ground of the squares in front of the tank still draws before the
       tank. */
    UT_ASSERT(isoDepth(5, 5, ISO_DEPTH_GROUND) < isoDepth(5, 5, ISO_DEPTH_SPRITE));
    UT_ASSERT(isoDepth(5, 6, ISO_DEPTH_GROUND) < isoDepth(5, 5, ISO_DEPTH_SPRITE));
    UT_ASSERT(isoDepth(6, 6, ISO_DEPTH_GROUND) < isoDepth(5, 5, ISO_DEPTH_SPRITE));

    /* A builder beside a pill on (5,5): from the far side, on (4,5), he is
       behind it; from the near side, on (6,5), in front of it. On the pill's
       own square he draws after the pill and before the tall tiles of the
       next row, which stand in front of him. */
    UT_ASSERT(isoDepth(4, 5, ISO_DEPTH_SPRITE) < isoDepth(5, 5, ISO_DEPTH_TALL));
    UT_ASSERT(isoDepth(6, 5, ISO_DEPTH_SPRITE) > isoDepth(5, 5, ISO_DEPTH_TALL));
    UT_ASSERT(isoDepth(5, 5, ISO_DEPTH_SPRITE) > isoDepth(5, 5, ISO_DEPTH_TALL));
    UT_ASSERT(isoDepth(5, 5, ISO_DEPTH_SPRITE) < isoDepth(5, 6, ISO_DEPTH_TALL));
    UT_ASSERT(isoDepth(5, 5, ISO_DEPTH_SPRITE) < isoDepth(6, 5, ISO_DEPTH_TALL));

    /* On one row the tall tiles share a key and the sprites share a key, so
       a stable sort leaves things along a row in the order they were given. */
    UT_ASSERT(isoDepth(3, 7, ISO_DEPTH_TALL) == isoDepth(7, 3, ISO_DEPTH_TALL));
    UT_ASSERT(isoDepth(3, 7, ISO_DEPTH_SPRITE) == isoDepth(7, 3, ISO_DEPTH_SPRITE));
    UT_ASSERT(isoDepth(3, 7, ISO_DEPTH_TALL) < isoDepth(3, 7, ISO_DEPTH_SPRITE));

    /* The key climbs with every row, right to the map's far corner. */
    {
        int row, prev = isoDepth(0, 0, ISO_DEPTH_SPRITE);
        for (row = 1; row <= 510; row++) {
            int mx = row > 255 ? 255 : row;
            int my = row - mx;
            int tall = isoDepth(mx, my, ISO_DEPTH_TALL);
            int sprite = isoDepth(mx, my, ISO_DEPTH_SPRITE);
            UT_ASSERT_MSG(tall > prev && sprite > tall,
                          "row %d: tall %d sprite %d after %d", row, tall, sprite, prev);
            prev = sprite;
        }
    }
    return 0;
}

int run_iso_project_facing_frame(void) {
    int frame;
    /* A classic sprite stands in turned with the camera: two frames on. */
    UT_ASSERT(isoFacingFrame(0, false) == 2);
    UT_ASSERT(isoFacingFrame(4, false) == 6);
    UT_ASSERT(isoFacingFrame(8, false) == 10);
    UT_ASSERT(isoFacingFrame(12, false) == 14);
    UT_ASSERT(isoFacingFrame(13, false) == 15);
    /* The last two frames wrap round to the first two. */
    UT_ASSERT(isoFacingFrame(14, false) == 0);
    UT_ASSERT(isoFacingFrame(15, false) == 1);
    /* An isometric sprite is drawn in world facing as it is. */
    for (frame = 0; frame < 16; frame++) {
        UT_ASSERT_MSG(isoFacingFrame(frame, true) == frame,
                      "iso frame %d -> %d", frame, isoFacingFrame(frame, true));
        UT_ASSERT_MSG(isoFacingFrame(frame, false) == (frame + 2) % 16,
                      "classic frame %d -> %d", frame, isoFacingFrame(frame, false));
    }
    return 0;
}
