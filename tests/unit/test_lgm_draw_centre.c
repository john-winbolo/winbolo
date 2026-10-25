/*
 * Where the player's own man is drawn (test_lgm_draw_centre.c).
 *
 * The server sends every man as a map square and a game pixel inside it
 * (lgmGetMX/MY/PX/PY: the drawing offset taken off, then rounded down to a
 * 16-world-unit game pixel). The client draws the other players' men straight
 * from those four values. For the player's own man it first puts them back
 * together into a world position (clientApplySnapshot, lgmPutWorld) and then
 * asks lgmGetScreenCoords for a square and pixel again. That second step used
 * to take the offset off and round down a second time, which drew the own man
 * one game pixel up and left of where the same values draw anybody else's,
 * and the own helicopter half a square up and left.
 *
 * own_matches_wire: across every position in a square, and across squares at
 * both ends of the map, the server's real getters followed by the client's
 * real lgmPutWorld / lgmOnScreen / lgmGetScreenCoords hand back exactly the
 * square and pixel that were sent, on foot and on the helicopter frame. The
 * getters the server's own rules read (lgmGetMX/MY/PX/PY) are pinned against
 * the arithmetic they have always done, so this change is seen not to move
 * them.
 *
 * error_bounds: the same chain carried on through spritePositionLgm, the
 * drawn sprite's centre measured against the true server position in world
 * units, own man and other players' men alike. spritePositionLgm centres the
 * sprite on the middle of the game pixel that was sent, which is as close as
 * a rounded-down position allows; the bounds are pinned as they stand so a
 * later change to the rounding has to move them on purpose.
 *
 * Not covered: the two lines in clientApplySnapshot that build the world
 * position from the snapshot fields are repeated here (ldc_decode) rather
 * than called, and screenLgmPrepare / playersMakeScreenLgm, which only carry
 * the values to the draw list, are not run.
 */

#include <stdio.h>

#include "global.h"
#include "lgm.h"
#include "sprite_positions.h"
#include "gfx_settings.h"    /* GFX_ANIM_CLASSIC / MATCH_PIXELATION / SMOOTH */
#include "tilenum.h"         /* LGM0..3 */
#include "tiles.h"           /* LGM_WIDTH / LGM_HEIGHT, TILE_SIZE_X */
#include "test_harness.h"

/* World units to a game pixel. */
#define LDC_PIXEL 16

/* What the server sends for one man. */
typedef struct {
    BYTE mx, my, px, py;
} LdcWire;

/* A man out of his tank at a true world position, as the server holds him. */
static void ldc_place_server(lgm *l, WORLD x, WORLD y, BYTE frame) {
    (*l)->inTank = FALSE;
    (*l)->state  = LGM_STATE_GOING;
    (*l)->isDead = (frame == LGM_HELICOPTER_FRAME) ? TRUE : FALSE;
    (*l)->frame  = frame;
    (*l)->x = x;
    (*l)->y = y;
}

/* serverSimBuildSnapshot's four lines. */
static LdcWire ldc_encode(lgm *l) {
    LdcWire w;
    w.mx = lgmGetMX(l);
    w.my = lgmGetMY(l);
    w.px = lgmGetPX(l);
    w.py = lgmGetPY(l);
    return w;
}

/* clientApplySnapshot's own-man branch: the same fields, the same order. */
static void ldc_decode(lgm *l, LdcWire w, BYTE frame) {
    WORLD wx = (WORLD)((w.mx << 8) + (w.px << 4));
    WORLD wy = (WORLD)((w.my << 8) + (w.py << 4));
    (*l)->inTank = FALSE;
    (*l)->isDead = (frame == LGM_HELICOPTER_FRAME) ? TRUE : FALSE;
    (*l)->frame  = frame;
    lgmPutWorld(l, wx, wy, frame);
}

/* The arithmetic lgmGetMX/MY/PX/PY have always done, written out again. */
static LdcWire ldc_expected_wire(WORLD x, WORLD y, BYTE frame) {
    LdcWire w;
    WORLD ox = (frame == LGM_HELICOPTER_FRAME) ? MAP_SQUARE_MIDDLE : 1;
    WORLD oy = (frame == LGM_HELICOPTER_FRAME) ? MAP_SQUARE_MIDDLE : 2;
    WORLD cx = (WORLD)(x - ox);
    WORLD cy = (WORLD)(y - oy);
    w.mx = (BYTE)(cx >> 8);
    w.my = (BYTE)(cy >> 8);
    w.px = (BYTE)((cx & 0xFF) >> 4);
    w.py = (BYTE)((cy & 0xFF) >> 4);
    return w;
}

/* Runs one true position through server encode and the client's own-man
 * decode and screen coordinates. Returns 0 and fills *out when the client
 * hands back what was sent. */
static int ldc_own_chain(lgm *srv, lgm *cli, WORLD x, WORLD y, BYTE frame,
                         LdcWire *out) {
    LdcWire sent;
    LdcWire want;
    BYTE mx = 0, my = 0, px = 0, py = 0, fr = 0;

    ldc_place_server(srv, x, y, frame);
    sent = ldc_encode(srv);
    want = ldc_expected_wire(x, y, frame);
    UT_ASSERT_MSG(sent.mx == want.mx && sent.my == want.my &&
                      sent.px == want.px && sent.py == want.py,
                  "frame %u at (%u,%u): server getters gave %u,%u,%u,%u, "
                  "they have always given %u,%u,%u,%u",
                  (unsigned)frame, (unsigned)x, (unsigned)y,
                  (unsigned)sent.mx, (unsigned)sent.my,
                  (unsigned)sent.px, (unsigned)sent.py,
                  (unsigned)want.mx, (unsigned)want.my,
                  (unsigned)want.px, (unsigned)want.py);

    ldc_decode(cli, sent, frame);
    UT_ASSERT_MSG(lgmOnScreen(cli, sent.mx, (BYTE)(sent.mx + 1),
                              sent.my, (BYTE)(sent.my + 1)) == TRUE,
                  "frame %u at (%u,%u): own man is not on the one-square "
                  "screen at the square he was sent on (%u,%u)",
                  (unsigned)frame, (unsigned)x, (unsigned)y,
                  (unsigned)sent.mx, (unsigned)sent.my);
    lgmGetScreenCoords(cli, 0, 0, &mx, &my, &px, &py, &fr);
    UT_ASSERT_MSG(mx == sent.mx && my == sent.my && px == sent.px &&
                      py == sent.py && fr == frame,
                  "frame %u at (%u,%u): sent %u,%u,%u,%u but the own man "
                  "draws from %u,%u,%u,%u frame %u",
                  (unsigned)frame, (unsigned)x, (unsigned)y,
                  (unsigned)sent.mx, (unsigned)sent.my,
                  (unsigned)sent.px, (unsigned)sent.py,
                  (unsigned)mx, (unsigned)my, (unsigned)px, (unsigned)py,
                  (unsigned)fr);
    *out = sent;
    return 0;
}

int run_lgm_draw_own_matches_wire(void) {
    /* Squares in the middle of the map and at the mined border either end,
       so the borrow into the square before is exercised where it matters. */
    static const BYTE kSquares[] = { 1, 20, 127, 128, 235, 254 };
    static const BYTE kFrames[]  = { 0, 1, 2, LGM_HELICOPTER_FRAME };
    lgm srv = lgmCreate(0);
    lgm cli = lgmCreate(0);
    int s, f, ox, oy;

    UT_ASSERT_MSG(srv != NULL && cli != NULL, "lgmCreate returned NULL");

    for (f = 0; f < (int)(sizeof(kFrames) / sizeof(kFrames[0])); f++) {
        for (s = 0; s < (int)(sizeof(kSquares) / sizeof(kSquares[0])); s++) {
            /* Every world unit of the square on both axes: all sixteen
               offsets inside a game pixel, every pixel, and the first units
               of the square where the offset borrows from the one before. */
            for (oy = 0; oy < 256; oy++) {
                for (ox = 0; ox < 256; ox++) {
                    LdcWire w;
                    WORLD x = (WORLD)((kSquares[s] << 8) + ox);
                    WORLD y = (WORLD)((kSquares[(s + 2) % 6] << 8) + oy);
                    if (ldc_own_chain(&srv, &cli, x, y, kFrames[f], &w) != 0) {
                        lgmDestroy(&srv);
                        lgmDestroy(&cli);
                        return 1;
                    }
                }
            }
        }
    }

    lgmDestroy(&srv);
    lgmDestroy(&cli);
    return 0;
}

/* Smallest and largest drawn-centre error seen, in world units. */
typedef struct {
    float minX, maxX, minY, maxY;
} LdcRange;

static void ldc_range_reset(LdcRange *r) {
    r->minX = r->minY = 1.0e9f;
    r->maxX = r->maxY = -1.0e9f;
}

static void ldc_range_add(LdcRange *r, float ex, float ey) {
    if (ex < r->minX) r->minX = ex;
    if (ex > r->maxX) r->maxX = ex;
    if (ey < r->minY) r->minY = ey;
    if (ey > r->maxY) r->maxY = ey;
}

/* The drawn sprite's centre in world units for one draw-list entry, at a
 * scale of one screen pixel to a game pixel and no scroll. */
static void ldc_drawn_centre(int mode, BYTE mx, BYTE my, BYTE px, BYTE py,
                             BYTE frame, float *cx, float *cy) {
    float sx = 0.0f, sy = 0.0f;
    int spriteFrame = (frame == LGM_HELICOPTER_FRAME) ? LGM3 : LGM0 + frame;
    float halfW = (frame == LGM_HELICOPTER_FRAME) ? (float)TILE_SIZE_X / 2.0f
                                                  : (float)LGM_WIDTH / 2.0f;
    float halfH = (frame == LGM_HELICOPTER_FRAME) ? (float)TILE_SIZE_Y / 2.0f
                                                  : (float)LGM_HEIGHT / 2.0f;

    /* Both callers hand the sub-square world offset over as pixel << 4. */
    spritePositionLgm(0.0f, 0.0f, mode, 1.0f, 1, (int)mx, (int)my,
                      (int)px, (int)py, (int)(px << 4), (int)(py << 4),
                      spriteFrame, &sx, &sy);
    *cx = (sx + halfW) * (float)LDC_PIXEL;
    *cy = (sy + halfH) * (float)LDC_PIXEL;
}

int run_lgm_draw_centre_error_bounds(void) {
    static const int kModes[] = {
        GFX_ANIM_CLASSIC, GFX_ANIM_MATCH_PIXELATION, GFX_ANIM_SMOOTH
    };
    static const char *kModeNames[] = { "Classic", "Match pixelation", "Smooth" };
    static const BYTE kSquares[] = { 1, 127, 254 };
    lgm srv = lgmCreate(0);
    lgm cli = lgmCreate(0);
    int m, s, ox, oy, heli;

    UT_ASSERT_MSG(srv != NULL && cli != NULL, "lgmCreate returned NULL");
    UT_ASSERT_MSG(LGM0 + 1 == LGM1 && LGM0 + 2 == LGM2,
                  "the on-foot sprite frames are not consecutive");

    for (heli = 0; heli < 2; heli++) {
        BYTE frame = heli ? LGM_HELICOPTER_FRAME : 0;
        for (m = 0; m < 3; m++) {
            LdcRange own, other;
            ldc_range_reset(&own);
            ldc_range_reset(&other);
            for (s = 0; s < 3; s++) {
                for (oy = 0; oy < 256; oy++) {
                    for (ox = 0; ox < 256; ox++) {
                        LdcWire w;
                        BYTE mx = 0, my = 0, px = 0, py = 0, fr = 0;
                        float cx = 0.0f, cy = 0.0f;
                        WORLD x = (WORLD)((kSquares[s] << 8) + ox);
                        WORLD y = (WORLD)((kSquares[2 - s] << 8) + oy);

                        if (ldc_own_chain(&srv, &cli, x, y, frame, &w) != 0) {
                            lgmDestroy(&srv);
                            lgmDestroy(&cli);
                            return 1;
                        }
                        /* Own man: what lgmGetScreenCoords hands the list. */
                        lgmGetScreenCoords(&cli, 0, 0, &mx, &my, &px, &py, &fr);
                        ldc_drawn_centre(kModes[m], mx, my, px, py, fr, &cx, &cy);
                        ldc_range_add(&own, cx - (float)x, cy - (float)y);
                        /* Another player's man: the sent values as they are. */
                        ldc_drawn_centre(kModes[m], w.mx, w.my, w.px, w.py,
                                         frame, &cx, &cy);
                        ldc_range_add(&other, cx - (float)x, cy - (float)y);
                    }
                }
            }
            printf("  %-10s %-16s own x %+.0f..%+.0f y %+.0f..%+.0f | "
                   "other x %+.0f..%+.0f y %+.0f..%+.0f (world units)\n",
                   heli ? "helicopter" : "on foot", kModeNames[m],
                   own.minX, own.maxX, own.minY, own.maxY,
                   other.minX, other.maxX, other.minY, other.maxY);

            UT_ASSERT_MSG(own.minX == other.minX && own.maxX == other.maxX &&
                              own.minY == other.minY && own.maxY == other.maxY,
                          "%s %s: the own man's drawn-centre error differs "
                          "from another player's",
                          heli ? "helicopter" : "on foot", kModeNames[m]);

            if (heli && kModes[m] == GFX_ANIM_SMOOTH) {
                /* The middle of the pixel the square-sized sprite's corner
                   was rounded down to. */
                UT_ASSERT_MSG(own.minX == -7.0f && own.maxX == 8.0f &&
                                  own.minY == -7.0f && own.maxY == 8.0f,
                              "Smooth helicopter: centre error x %.1f..%.1f "
                              "y %.1f..%.1f, wanted -7..8 on both",
                              own.minX, own.maxX, own.minY, own.maxY);
            } else if (heli) {
                /* On the grid the round-down stays: the next pixel along
                   would be +1..+16. */
                UT_ASSERT_MSG(own.minX == -15.0f && own.maxX == 0.0f &&
                                  own.minY == -15.0f && own.maxY == 0.0f,
                              "%s helicopter: centre error x %.1f..%.1f "
                              "y %.1f..%.1f, wanted -15..0 on both",
                              kModeNames[m], own.minX, own.maxX,
                              own.minY, own.maxY);
            } else if (kModes[m] == GFX_ANIM_SMOOTH) {
                /* The middle of the pixel on both axes; the odd unit is the
                   1 and 2 the server takes off before it rounds. */
                UT_ASSERT_MSG(own.minX == -8.0f && own.maxX == 7.0f &&
                                  own.minY == -9.0f && own.maxY == 6.0f,
                              "Smooth on foot: centre error x %.1f..%.1f "
                              "y %.1f..%.1f, wanted -8..7 and -9..6",
                              own.minX, own.maxX, own.minY, own.maxY);
            } else {
                /* On the grid the three-pixel-wide sprite's centre falls on
                   the middle of a pixel, the four-pixel-high sprite's on a
                   pixel edge: the nearer edge is the one below. */
                UT_ASSERT_MSG(own.minX == -8.0f && own.maxX == 7.0f &&
                                  own.minY == -1.0f && own.maxY == 14.0f,
                              "%s on foot: centre error x %.1f..%.1f "
                              "y %.1f..%.1f, wanted -8..7 and -1..14",
                              kModeNames[m], own.minX, own.maxX,
                              own.minY, own.maxY);
            }
        }
    }

    lgmDestroy(&srv);
    lgmDestroy(&cli);
    return 0;
}
