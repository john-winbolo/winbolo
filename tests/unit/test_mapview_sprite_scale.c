/*
 * Sprite placement at a float scale (test_mapview_sprite_scale.c).
 *
 * spritePositionOffset, spritePositionShell and spritePositionLgm
 * (sprite_positions.c) are the arithmetic behind mapViewDrawShells / Tanks /
 * LGMs. The classic view calls them at a whole-number zoom and the map
 * overview at its 0.5x-4x ladder; these cases pin both ends.
 *
 * classic: at an integer scale with the classic view's origin, the three give
 * the positions the classic formula gives, written out from what the numbers
 * mean rather than by running the same sums again.
 *
 * ladder: at every rung of the overview ladder the position matches what the
 * overview drew before it took a float scale — sprites placed at sixteen
 * steps to a game pixel, an origin rounded to a whole step and the renderer
 * scaled by a sixteenth of the rung — within a sixteenth of a pixel, in all
 * three animation modes and at two sheet scales. With an origin off that
 * path's step grid the two differ by exactly the rounding it applied to the
 * origin, and by nothing else.
 *
 * shell_tip: a direction frame comes back by its tip pixel times the scale,
 * so the tip and not the corner sits on the shell's position; an explosion
 * frame is left at the corner.
 *
 * lgm_snap: the on-foot centring lands the sprite a whole number of game
 * pixels from the base under Classic and Match pixelation, and is left where
 * the centring put it under Smooth; the helicopter is not centred at all.
 */

#include <math.h>

#include "test_harness.h"
#include "sprite_positions.h"
#include "gfx_settings.h"    /* GFX_ANIM_CLASSIC / MATCH_PIXELATION / SMOOTH */
#include "tilenum.h"         /* SHELL_DIR0..15, SHELL_EXPLOSION*, LGM0..3 */
#include "tiles.h"           /* TILE_SIZE_X / TILE_SIZE_Y, LGM_WIDTH / LGM_HEIGHT */
#include "overview_camera.h" /* the overview's zoom ladder */

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const int kModes[] = {
    GFX_ANIM_CLASSIC, GFX_ANIM_MATCH_PIXELATION, GFX_ANIM_SMOOTH
};

static const char *modeName(int mode) {
    switch (mode) {
    case GFX_ANIM_CLASSIC:          return "Classic";
    case GFX_ANIM_MATCH_PIXELATION: return "Match pixelation";
    case GFX_ANIM_SMOOTH:           return "Smooth";
    default:                        return "?";
    }
}

/* Each direction's tip pixel, read off the shell shapes in tile.bmp: column
 * and row in game pixels from the sprite's top-left, 4 being its far edge.
 * The north shell's point is the middle of its top row, the east shell's the
 * middle of its right column, the south-east shell's the bottom-right corner,
 * and so on round the compass. */
static const struct { float col, row; } kTip[16] = {
    { 1.5f, 0.0f }, /* N   */
    { 3.0f, 0.0f }, /* NNE */
    { 4.0f, 0.0f }, /* NE  */
    { 4.0f, 0.0f }, /* ENE */
    { 4.0f, 1.5f }, /* E   */
    { 4.0f, 3.0f }, /* ESE */
    { 4.0f, 4.0f }, /* SE  */
    { 3.0f, 4.0f }, /* SSE */
    { 1.5f, 4.0f }, /* S   */
    { 0.0f, 4.0f }, /* SSW */
    { 0.0f, 3.0f }, /* SW  */
    { 0.0f, 3.0f }, /* WSW */
    { 0.0f, 1.5f }, /* W   */
    { 0.0f, 0.0f }, /* WNW */
    { 0.0f, 0.0f }, /* NW  */
    { 0.0f, 0.0f }  /* NNW */
};

/* The on-foot LGM sprite is LGM_WIDTH by LGM_HEIGHT game pixels (3 by 4), so
 * its centre sits 1.5 across and 2 down from the corner. */
#define LGM_CENTRE_X ((float)LGM_WIDTH / 2.0f)
#define LGM_CENTRE_Y ((float)LGM_HEIGHT / 2.0f)

/* The classic view at 2x, drawing at (100, 40) with 32-pixel tiles and a
 * (6, 10) pixel scroll. The lists' square 0,0 lands at
 *     originX - tileW - edgeX = 62,   originY - tileH - edgeY = -2
 * and a sprite in square (mx, my) at game pixel (px, py) goes a further
 * (mx * 16 + px) * 2 across and (my * 16 + py) * 2 down. Every expected value
 * below is that sum written out. */
int run_mapview_sprite_classic(void) {
    const float scale = 2.0f;
    const float originX = 100.0f, originY = 40.0f;
    const float tileW = (float)TILE_SIZE_X * scale;
    const float tileH = (float)TILE_SIZE_Y * scale;
    const float edgeX = 6.0f, edgeY = 10.0f;
    const float baseX = originX - tileW - edgeX;
    const float baseY = originY - tileH - edgeY;
    const float kExact = 1.0e-4f;
    int i;

    UT_ASSERT_MSG(fabsf(baseX - 62.0f) < kExact && fabsf(baseY + 2.0f) < kExact,
                  "base came out at (%.4f,%.4f); the sums below assume (62,-2)",
                  (double)baseX, (double)baseY);

    /* Offsets from the base. Square 3, pixel 5 is game pixel 53; world offset
     * 88 in square 3 is 3 * 256 + 88 = 856 world units, 53.5 game pixels. */
    static const struct {
        int mode, sheetScale, square, pixelOff, worldOff;
        float want;
        const char *why;
    } kOffsets[] = {
        { GFX_ANIM_CLASSIC, 1, 3, 5, 5 << 4, 106.0f,
          "53 game pixels at 2x" },
        { GFX_ANIM_CLASSIC, 1, 3, 5, 88, 106.0f,
          "Classic does not read the world offset" },
        { GFX_ANIM_SMOOTH, 1, 3, 5, 88, 107.0f,
          "53.5 game pixels at 2x" },
        { GFX_ANIM_SMOOTH, 1, 3, 5, 5 << 4, 106.0f,
          "a world offset on the game pixel is Classic's 53" },
        { GFX_ANIM_MATCH_PIXELATION, 1, 3, 5, 88, 108.0f,
          "a sheet texel is a whole game pixel at sheet scale 1, so 53.5 "
          "rounds up to 54" },
        { GFX_ANIM_MATCH_PIXELATION, 2, 3, 5, 88, 107.0f,
          "a sheet texel is half a game pixel at sheet scale 2, so 53.5 "
          "stands" },
        { GFX_ANIM_MATCH_PIXELATION, 1, 3, 5, 5 << 4, 106.0f,
          "already on a game pixel, nothing to snap" },
        { GFX_ANIM_CLASSIC, 1, 0, 0, 0, 0.0f,
          "square 0, pixel 0 is the base itself" },
        { GFX_ANIM_CLASSIC, 1, 255, 15, 15 << 4, 8190.0f,
          "the map's last game pixel, 4095, at 2x" },
        { GFX_ANIM_SMOOTH, 1, 255, 15, 255, 8191.875f,
          "the map's last world unit, 4095 and 15/16 game pixels, at 2x" },
    };

    for (i = 0; i < ARRAY_LEN(kOffsets); i++) {
        float got = spritePositionOffset(kOffsets[i].mode, scale,
                                         kOffsets[i].sheetScale,
                                         kOffsets[i].square,
                                         kOffsets[i].pixelOff,
                                         kOffsets[i].worldOff);
        UT_ASSERT_MSG(fabsf(got - kOffsets[i].want) < kExact,
                      "%s, sheet scale %d, square %d pixel %d world %d: "
                      "offset %.4f, expected %.4f (%s)",
                      modeName(kOffsets[i].mode), kOffsets[i].sheetScale,
                      kOffsets[i].square, kOffsets[i].pixelOff,
                      kOffsets[i].worldOff, (double)got,
                      (double)kOffsets[i].want, kOffsets[i].why);
    }

    /* A shell in square (3, 2) at pixel (5, 9): game pixel (53, 41), so
     * (106, 82) from the base, which is (168, 80) on screen. Facing east its
     * tip is 4 pixels in and 1.5 down, 8 and 3 at 2x, so the sprite starts at
     * (160, 77). An explosion has no tip and stays at (168, 80). */
    {
        float sx = 0.0f, sy = 0.0f;
        spritePositionShell(baseX, baseY, GFX_ANIM_CLASSIC, scale, 1,
                            3, 2, 5, 9, 5 << 4, 9 << 4, SHELL_DIR4, &sx, &sy);
        UT_ASSERT_MSG(fabsf(sx - 160.0f) < kExact && fabsf(sy - 77.0f) < kExact,
                      "east shell at (%.4f,%.4f), expected (160,77)",
                      (double)sx, (double)sy);

        spritePositionShell(baseX, baseY, GFX_ANIM_CLASSIC, scale, 1,
                            3, 2, 5, 9, 5 << 4, 9 << 4, SHELL_EXPLOSION8,
                            &sx, &sy);
        UT_ASSERT_MSG(fabsf(sx - 168.0f) < kExact && fabsf(sy - 80.0f) < kExact,
                      "explosion at (%.4f,%.4f), expected (168,80)",
                      (double)sx, (double)sy);
    }

    /* An LGM on the same square and pixel. Its centre is 1.5 pixels across
     * and 2 down, so the corner wants to be at game pixel (51.5, 39) from the
     * base; the half rounds up to 52, so it is drawn (104, 78) from the base,
     * (166, 76) on screen. The helicopter is drawn from its corner, (168, 80). */
    {
        float sx = 0.0f, sy = 0.0f;
        spritePositionLgm(baseX, baseY, GFX_ANIM_CLASSIC, scale, 1,
                          3, 2, 5, 9, 5 << 4, 9 << 4, LGM0, &sx, &sy);
        UT_ASSERT_MSG(fabsf(sx - 166.0f) < kExact && fabsf(sy - 76.0f) < kExact,
                      "LGM at (%.4f,%.4f), expected (166,76)",
                      (double)sx, (double)sy);

        spritePositionLgm(baseX, baseY, GFX_ANIM_CLASSIC, scale, 1,
                          3, 2, 5, 9, 5 << 4, 9 << 4, LGM3, &sx, &sy);
        UT_ASSERT_MSG(fabsf(sx - 168.0f) < kExact && fabsf(sy - 80.0f) < kExact,
                      "helicopter at (%.4f,%.4f), expected (168,80)",
                      (double)sx, (double)sy);
    }

    return 0;
}

/* The overview's sprite pass before it took a float scale: mapview.c run at
 * zoomFactor SUBPX, sixteen steps to a game pixel, with the origin rounded
 * to a whole step and the renderer scaled by zoomScale / SUBPX. These give
 * that path's answers in steps; times perStep is what reached the screen. */
#define SUBPX 16

static float oldOffsetSteps(int mode, int sheetScale, int square,
                            int pixelOff, int worldOff) {
    if (mode == GFX_ANIM_SMOOTH || mode == GFX_ANIM_MATCH_PIXELATION) {
        float smooth = ((float)(square * 256 + worldOff) / 16.0f) * (float)SUBPX;
        if (mode == GFX_ANIM_SMOOTH) return smooth;
        int ss = sheetScale < 1 ? 1 : sheetScale;
        float step = (float)SUBPX / (float)ss;
        return floorf(smooth / step + 0.5f) * step;
    }
    return (float)((square * TILE_SIZE_X + pixelOff) * SUBPX);
}

static float oldShellSteps(long base, int mode, int sheetScale, int square,
                           int pixelOff, int worldOff, float tip) {
    return (float)base +
           oldOffsetSteps(mode, sheetScale, square, pixelOff, worldOff) -
           tip * (float)SUBPX;
}

static float oldLgmSteps(long base, int mode, int sheetScale, int square,
                         int pixelOff, int worldOff, float centre, bool onFoot) {
    float s = (float)base +
              oldOffsetSteps(mode, sheetScale, square, pixelOff, worldOff);
    if (onFoot) {
        s -= centre * (float)SUBPX;
        if (mode != GFX_ANIM_SMOOTH) {
            s = (float)base +
                floorf((s - (float)base) / (float)SUBPX + 0.5f) * (float)SUBPX;
        }
    }
    return s;
}

int run_mapview_sprite_ladder(void) {
    static const int kSquares[] = { 0, 1, 37, 128, 255 };
    static const int kPixels[]  = { 0, 5, 15 };
    static const int kSub[]     = { 0, 7 };   /* world units past the game pixel */
    static const int kSheets[]  = { 1, 2 };
    static const int kShellFrames[] = {
        SHELL_DIR0,  SHELL_DIR1,  SHELL_DIR2,  SHELL_DIR3,
        SHELL_DIR4,  SHELL_DIR5,  SHELL_DIR6,  SHELL_DIR7,
        SHELL_DIR8,  SHELL_DIR9,  SHELL_DIR10, SHELL_DIR11,
        SHELL_DIR12, SHELL_DIR13, SHELL_DIR14, SHELL_DIR15,
        SHELL_EXPLOSION8, SHELL_EXPLOSION1
    };
    static const int kLgmFrames[] = { LGM0, LGM1, LGM2, LGM3 };
    const float kSixteenth = 1.0f / 16.0f;

    /* Where map square 0,0 sits on screen. Fractional, as the camera hands it
     * over, but a whole number of the old path's steps at every rung: each
     * rung's step is 1/32, 3/64, 1/16, 3/32, 1/8, 3/16 or 1/4 of a pixel, and
     * 0.75 is a multiple of all seven. So the old path's origin rounding is
     * nil here and any difference is in the sprite arithmetic. An origin off
     * the grid is covered after. */
    const float o0x = -123.75f;   /* -165 * 0.75 */
    const float o0y =   41.25f;   /*   55 * 0.75 */

    OverviewCamera cam;
    overviewCameraInit(&cam);

    for (int z = 0; z < overviewCameraZoomCount(); z++) {
        cam.zoomIndex = z;
        const float zs = overviewCameraZoomScale(&cam);
        const float perStep = zs / (float)SUBPX;
        /* The old path's origin in steps, and the base the new path gets:
         * overview_view.cpp passes originX = o0x + tileW with that tileW, and
         * the drawers take the tileW back off. */
        const long ox16 = lroundf(o0x / perStep);
        const long oy16 = lroundf(o0y / perStep);
        const float tileW = (float)TILE_SIZE_X * zs;
        const float tileH = (float)TILE_SIZE_Y * zs;
        const float baseX = (o0x + tileW) - tileW;
        const float baseY = (o0y + tileH) - tileH;

        UT_ASSERT_MSG(fabsf((float)ox16 * perStep - o0x) < 1.0e-4f &&
                      fabsf((float)oy16 * perStep - o0y) < 1.0e-4f,
                      "%.2fx: origin (%.4f,%.4f) is not on the old path's "
                      "step grid, so this check would fold its rounding in",
                      (double)zs, (double)o0x, (double)o0y);

        for (int m = 0; m < ARRAY_LEN(kModes); m++) {
            const int mode = kModes[m];
            for (int s = 0; s < ARRAY_LEN(kSheets); s++) {
                const int ss = kSheets[s];
                for (int i = 0; i < ARRAY_LEN(kSquares); i++) {
                    const int mx = kSquares[i];
                    const int my = kSquares[(i + 2) % ARRAY_LEN(kSquares)];
                    for (int j = 0; j < ARRAY_LEN(kPixels); j++) {
                        const int px = kPixels[j];
                        const int py = kPixels[(j + 1) % ARRAY_LEN(kPixels)];
                        for (int k = 0; k < ARRAY_LEN(kSub); k++) {
                            const int wx = (px << 4) + kSub[k];
                            const int wy = (py << 4) + kSub[k];

                            /* A tank: the base plus the offset. */
                            {
                                float wantX = perStep * ((float)ox16 +
                                    oldOffsetSteps(mode, ss, mx, px, wx));
                                float wantY = perStep * ((float)oy16 +
                                    oldOffsetSteps(mode, ss, my, py, wy));
                                float gotX = baseX +
                                    spritePositionOffset(mode, zs, ss, mx, px, wx);
                                float gotY = baseY +
                                    spritePositionOffset(mode, zs, ss, my, py, wy);
                                UT_ASSERT_MSG(fabsf(gotX - wantX) <= kSixteenth &&
                                              fabsf(gotY - wantY) <= kSixteenth,
                                              "%.2fx %s sheet %d, square (%d,%d) "
                                              "pixel (%d,%d) world (%d,%d): tank at "
                                              "(%.4f,%.4f), old path drew (%.4f,%.4f)",
                                              (double)zs, modeName(mode), ss,
                                              mx, my, px, py, wx, wy,
                                              (double)gotX, (double)gotY,
                                              (double)wantX, (double)wantY);
                            }

                            /* Shells: every direction, and an explosion. */
                            for (int f = 0; f < ARRAY_LEN(kShellFrames); f++) {
                                const int frame = kShellFrames[f];
                                float tipC = 0.0f, tipR = 0.0f;
                                if (frame >= SHELL_DIR0 && frame <= SHELL_DIR15) {
                                    tipC = kTip[frame - SHELL_DIR0].col;
                                    tipR = kTip[frame - SHELL_DIR0].row;
                                }
                                float wantX = perStep *
                                    oldShellSteps(ox16, mode, ss, mx, px, wx, tipC);
                                float wantY = perStep *
                                    oldShellSteps(oy16, mode, ss, my, py, wy, tipR);
                                float gotX = 0.0f, gotY = 0.0f;
                                spritePositionShell(baseX, baseY, mode, zs, ss,
                                                    mx, my, px, py, wx, wy, frame,
                                                    &gotX, &gotY);
                                UT_ASSERT_MSG(fabsf(gotX - wantX) <= kSixteenth &&
                                              fabsf(gotY - wantY) <= kSixteenth,
                                              "%.2fx %s sheet %d, square (%d,%d) "
                                              "pixel (%d,%d) world (%d,%d) frame %d: "
                                              "shell at (%.4f,%.4f), old path drew "
                                              "(%.4f,%.4f)",
                                              (double)zs, modeName(mode), ss,
                                              mx, my, px, py, wx, wy, frame,
                                              (double)gotX, (double)gotY,
                                              (double)wantX, (double)wantY);
                            }

                            /* LGMs on foot, and the helicopter. */
                            for (int f = 0; f < ARRAY_LEN(kLgmFrames); f++) {
                                const int frame = kLgmFrames[f];
                                const bool onFoot = frame == LGM0 ||
                                                    frame == LGM1 ||
                                                    frame == LGM2;
                                float wantX = perStep *
                                    oldLgmSteps(ox16, mode, ss, mx, px, wx,
                                                LGM_CENTRE_X, onFoot);
                                float wantY = perStep *
                                    oldLgmSteps(oy16, mode, ss, my, py, wy,
                                                LGM_CENTRE_Y, onFoot);
                                float gotX = 0.0f, gotY = 0.0f;
                                spritePositionLgm(baseX, baseY, mode, zs, ss,
                                                  mx, my, px, py, wx, wy, frame,
                                                  &gotX, &gotY);
                                UT_ASSERT_MSG(fabsf(gotX - wantX) <= kSixteenth &&
                                              fabsf(gotY - wantY) <= kSixteenth,
                                              "%.2fx %s sheet %d, square (%d,%d) "
                                              "pixel (%d,%d) world (%d,%d) frame %d: "
                                              "LGM at (%.4f,%.4f), old path drew "
                                              "(%.4f,%.4f)",
                                              (double)zs, modeName(mode), ss,
                                              mx, my, px, py, wx, wy, frame,
                                              (double)gotX, (double)gotY,
                                              (double)wantX, (double)wantY);
                            }
                        }
                    }
                }
            }
        }
    }

    /* An origin off the step grid, which is where the camera leaves it most
     * frames. The old path rounded it to the nearest step before drawing; the
     * new one draws from it as it is. So the two answers differ by that
     * rounding — under half a step, an eighth of a pixel at 4x — and by
     * nothing else. */
    {
        const float offX = -123.7f, offY = 41.3f;
        for (int z = 0; z < overviewCameraZoomCount(); z++) {
            cam.zoomIndex = z;
            const float zs = overviewCameraZoomScale(&cam);
            const float perStep = zs / (float)SUBPX;
            const long ox16 = lroundf(offX / perStep);
            const long oy16 = lroundf(offY / perStep);
            const float roundX = (float)ox16 * perStep - offX;
            const float roundY = (float)oy16 * perStep - offY;
            const float tileW = (float)TILE_SIZE_X * zs;
            const float tileH = (float)TILE_SIZE_Y * zs;
            const float baseX = (offX + tileW) - tileW;
            const float baseY = (offY + tileH) - tileH;

            for (int m = 0; m < ARRAY_LEN(kModes); m++) {
                const int mode = kModes[m];
                float wantX = perStep * ((float)ox16 +
                                         oldOffsetSteps(mode, 1, 37, 5, 87));
                float wantY = perStep * ((float)oy16 +
                                         oldOffsetSteps(mode, 1, 128, 15, 247));
                float gotX = baseX + spritePositionOffset(mode, zs, 1, 37, 5, 87);
                float gotY = baseY + spritePositionOffset(mode, zs, 1, 128, 15, 247);
                UT_ASSERT_MSG(fabsf((wantX - gotX) - roundX) <= kSixteenth &&
                              fabsf((wantY - gotY) - roundY) <= kSixteenth,
                              "%.2fx %s: old path drew (%.4f,%.4f) from an origin "
                              "rounded by (%.4f,%.4f); new path put the sprite at "
                              "(%.4f,%.4f), which is not that rounding away",
                              (double)zs, modeName(mode),
                              (double)wantX, (double)wantY,
                              (double)roundX, (double)roundY,
                              (double)gotX, (double)gotY);
            }
        }
    }

    return 0;
}

/* A direction frame comes back by its tip pixel times the scale, in every
 * mode and at fractional scales; a frame outside the direction range does not
 * move. */
int run_mapview_sprite_shell_tip(void) {
    static const float kScales[] = { 1.0f, 2.0f, 3.0f, 0.5f, 1.5f };
    /* The explosion frames, and the first value past the direction range. */
    static const int kNoTip[] = { SHELL_EXPLOSION8, SHELL_EXPLOSION1, SHELL_DIR15 + 1 };
    const float baseX = 10.0f, baseY = -4.0f;
    const int mx = 20, my = 7, px = 3, py = 12;
    const float kExact = 1.0e-3f;

    for (int s = 0; s < ARRAY_LEN(kScales); s++) {
        const float scale = kScales[s];
        /* Where the corner goes with no tip: the game pixel times the scale.
         * The world offset sits on the game pixel, so every mode agrees. */
        const float cornerX = baseX + (float)(mx * TILE_SIZE_X + px) * scale;
        const float cornerY = baseY + (float)(my * TILE_SIZE_Y + py) * scale;

        for (int m = 0; m < ARRAY_LEN(kModes); m++) {
            const int mode = kModes[m];

            for (int dir = 0; dir < 16; dir++) {
                float gotX = 0.0f, gotY = 0.0f;
                float wantX = cornerX - kTip[dir].col * scale;
                float wantY = cornerY - kTip[dir].row * scale;
                spritePositionShell(baseX, baseY, mode, scale, 1,
                                    mx, my, px, py, px << 4, py << 4,
                                    SHELL_DIR0 + dir, &gotX, &gotY);
                UT_ASSERT_MSG(fabsf(gotX - wantX) < kExact &&
                              fabsf(gotY - wantY) < kExact,
                              "%.2fx %s direction %d: shell at (%.4f,%.4f), "
                              "expected the corner (%.4f,%.4f) back by the tip "
                              "(%.1f,%.1f) times the scale, (%.4f,%.4f)",
                              (double)scale, modeName(mode), dir,
                              (double)gotX, (double)gotY,
                              (double)cornerX, (double)cornerY,
                              (double)kTip[dir].col, (double)kTip[dir].row,
                              (double)wantX, (double)wantY);
            }

            for (int f = 0; f < ARRAY_LEN(kNoTip); f++) {
                float gotX = 0.0f, gotY = 0.0f;
                spritePositionShell(baseX, baseY, mode, scale, 1,
                                    mx, my, px, py, px << 4, py << 4,
                                    kNoTip[f], &gotX, &gotY);
                UT_ASSERT_MSG(fabsf(gotX - cornerX) < kExact &&
                              fabsf(gotY - cornerY) < kExact,
                              "%.2fx %s frame %d: at (%.4f,%.4f), expected the "
                              "corner (%.4f,%.4f) with no tip taken off",
                              (double)scale, modeName(mode), kNoTip[f],
                              (double)gotX, (double)gotY,
                              (double)cornerX, (double)cornerY);
            }
        }
    }

    return 0;
}

/* The on-foot centring pulls the corner 1.5 game pixels left and 2 up of the
 * hit pixel, then Classic and Match pixelation round the corner to a whole
 * game pixel from the base — the half rounds up — while Smooth keeps the exact
 * centre. The helicopter is drawn from its corner in every mode. */
int run_mapview_sprite_lgm_snap(void) {
    static const float kScales[] = { 1.0f, 2.0f, 3.0f, 0.5f, 0.75f, 1.5f };
    static const int kOnFoot[] = { LGM0, LGM1, LGM2 };
    const float baseX = 7.0f, baseY = 100.0f;
    const int mx = 12, my = 200, px = 6, py = 0;
    const int gpX = mx * TILE_SIZE_X + px;   /* 198 */
    const int gpY = my * TILE_SIZE_Y + py;   /* 3200 */
    const float kExact = 1.0e-3f;

    for (int s = 0; s < ARRAY_LEN(kScales); s++) {
        const float scale = kScales[s];
        for (int m = 0; m < ARRAY_LEN(kModes); m++) {
            const int mode = kModes[m];
            const bool smooth = mode == GFX_ANIM_SMOOTH;

            for (int f = 0; f < ARRAY_LEN(kOnFoot); f++) {
                float gotX = 0.0f, gotY = 0.0f;
                float wantX, wantY;

                /* On the game pixel: the corner wants to be at gp - 1.5 across
                 * and gp - 2 down. Classic and Match round the half up to
                 * gp - 1; Smooth keeps gp - 1.5. */
                spritePositionLgm(baseX, baseY, mode, scale, 1,
                                  mx, my, px, py, px << 4, py << 4,
                                  kOnFoot[f], &gotX, &gotY);
                wantX = smooth ? baseX + ((float)gpX - 1.5f) * scale
                               : baseX + (float)(gpX - 1) * scale;
                wantY = baseY + (float)(gpY - 2) * scale;
                UT_ASSERT_MSG(fabsf(gotX - wantX) < kExact &&
                              fabsf(gotY - wantY) < kExact,
                              "%.2fx %s frame %d on the pixel: LGM at "
                              "(%.4f,%.4f), expected (%.4f,%.4f)",
                              (double)scale, modeName(mode), kOnFoot[f],
                              (double)gotX, (double)gotY,
                              (double)wantX, (double)wantY);

                /* A quarter pixel past it (world offset +4): the centre is
                 * gp - 1.25 across and gp - 1.75 down. Smooth keeps that.
                 * Classic never reads the world offset and stays at gp - 1,
                 * gp - 2; Match pixelation at sheet scale 1 rounds the quarter
                 * away before centring and answers as Classic does. */
                spritePositionLgm(baseX, baseY, mode, scale, 1,
                                  mx, my, px, py, (px << 4) + 4, (py << 4) + 4,
                                  kOnFoot[f], &gotX, &gotY);
                if (smooth) {
                    wantX = baseX + ((float)gpX - 1.25f) * scale;
                    wantY = baseY + ((float)gpY - 1.75f) * scale;
                } else {
                    wantX = baseX + (float)(gpX - 1) * scale;
                    wantY = baseY + (float)(gpY - 2) * scale;
                }
                UT_ASSERT_MSG(fabsf(gotX - wantX) < kExact &&
                              fabsf(gotY - wantY) < kExact,
                              "%.2fx %s frame %d a quarter pixel on: LGM at "
                              "(%.4f,%.4f), expected (%.4f,%.4f)",
                              (double)scale, modeName(mode), kOnFoot[f],
                              (double)gotX, (double)gotY,
                              (double)wantX, (double)wantY);

                /* Which is to say: Classic and Match land a whole number of
                 * game pixels from the base, and Smooth does not. */
                {
                    float stepsX = (gotX - baseX) / scale;
                    float stepsY = (gotY - baseY) / scale;
                    bool wholeX = fabsf(stepsX - roundf(stepsX)) < kExact;
                    bool wholeY = fabsf(stepsY - roundf(stepsY)) < kExact;
                    UT_ASSERT_MSG(smooth ? (!wholeX && !wholeY)
                                         : (wholeX && wholeY),
                                  "%.2fx %s frame %d: LGM is (%.4f,%.4f) game "
                                  "pixels from the base, %s",
                                  (double)scale, modeName(mode), kOnFoot[f],
                                  (double)stepsX, (double)stepsY,
                                  smooth ? "which Smooth should not have snapped"
                                         : "which should be whole numbers");
                }
            }

            /* The helicopter: its corner is on the pixel, plus the quarter
             * under Smooth. */
            {
                float gotX = 0.0f, gotY = 0.0f;
                float wantX = baseX + ((float)gpX + (smooth ? 0.25f : 0.0f)) * scale;
                float wantY = baseY + ((float)gpY + (smooth ? 0.25f : 0.0f)) * scale;
                spritePositionLgm(baseX, baseY, mode, scale, 1,
                                  mx, my, px, py, (px << 4) + 4, (py << 4) + 4,
                                  LGM3, &gotX, &gotY);
                UT_ASSERT_MSG(fabsf(gotX - wantX) < kExact &&
                              fabsf(gotY - wantY) < kExact,
                              "%.2fx %s: helicopter at (%.4f,%.4f), expected "
                              "(%.4f,%.4f), its corner with no centring",
                              (double)scale, modeName(mode),
                              (double)gotX, (double)gotY,
                              (double)wantX, (double)wantY);
            }
        }
    }

    return 0;
}

/* camera_split: the menu background's camera (mapViewRenderCentered) is a
 * view edge `pos` pixels from the map's left or top, split into the first
 * square drawn and how far into it the view starts. Every square is then
 * drawn at (square_index - firstSquare) * unit - edge, so the map's own
 * square 0 lands at -pos exactly when square * unit + edge == pos. Swept
 * across the left/top edge (pos below 0) and well inside it, at the unit
 * sizes of zoom 1 to 3: the edge stays in 0..unit-1 and square 0 moves one
 * pixel for each pixel the camera moves, never a tile. The old clamp set
 * the square to 0 below the edge and kept the edge, so square 0 went to
 * -edge instead: a sawtooth of up to a tile, the jitter this pins out.
 *
 * Out-of-map squares are drawn as open deep sea; mapViewSquareInMap is
 * the test for that, checked at the four edges of the 256x256 map. */
int run_mapview_camera_split(void) {
    static const int kUnits[] = { 16, 32, 48 };
    for (int u = 0; u < ARRAY_LEN(kUnits); u++) {
        int unit = kUnits[u];
        int prevDrawn = 0;
        for (int pos = 4 * unit; pos >= -4 * unit; pos--) {
            int square = 12345, edge = 12345;
            mapViewCameraSplit(pos, unit, &square, &edge);
            UT_ASSERT_MSG(edge >= 0 && edge < unit,
                          "unit %d pos %d: edge %d outside 0..%d",
                          unit, pos, edge, unit - 1);
            UT_ASSERT_MSG(square * unit + edge == pos,
                          "unit %d pos %d: square %d edge %d is position %d",
                          unit, pos, square, edge, square * unit + edge);
            int drawn = (0 - square) * unit - edge;   /* map square 0's x */
            if (pos != 4 * unit) {
                UT_ASSERT_MSG(drawn == prevDrawn + 1,
                              "unit %d pos %d: square 0 drawn at %d after %d, "
                              "a jump of %d pixels for a 1 pixel camera move",
                              unit, pos, drawn, prevDrawn, drawn - prevDrawn);
            }
            prevDrawn = drawn;
        }
    }

    /* The whole camera, as the renderer computes it (mapViewCameraAxis),
     * for both paths and zoom 1 to 3: the view centre is moved in steps of
     * one camera pixel from four squares inside the left/top edge to four
     * squares past it. Map square 0 must be drawn exactly where the
     * unclamped camera puts it, -camera * pixel, at every step. A clamp of
     * the square added in mapViewCameraAxis fails this past the edge. */
    static const int kViewLens[] = { 640, 641 };
    for (int zf = 1; zf <= 3; zf++) {
        int scaled = 16 * zf;
        for (int v = 0; v < ARRAY_LEN(kViewLens); v++) {
            int viewLen = kViewLens[v];

            /* Classic: one game pixel is 16 world units and zf screen
             * pixels. The centre is kept at or above 0, as a WORLD is. */
            int half = viewLen / (2 * zf);
            for (int cam = 4 * 16; cam >= -4 * 16; cam--) {
                if (cam + half < 0) break;
                int centerW = (cam + half) * 16;
                int square = 12345, edge = 12345;
                mapViewCameraAxis(centerW, NULL, viewLen, zf, &square, &edge);
                UT_ASSERT_MSG(edge >= 0 && edge < scaled,
                              "classic zf %d view %d cam %d: edge %d outside 0..%d",
                              zf, viewLen, cam, edge, scaled - 1);
                int drawn = -square * scaled - edge;
                UT_ASSERT_MSG(drawn == -cam * zf,
                              "classic zf %d view %d cam %d: square 0 drawn at %d, "
                              "the camera puts it at %d",
                              zf, viewLen, cam, drawn, -cam * zf);
            }

            /* Precise: one screen pixel is 16 / zf world units. */
            for (int cam = 4 * scaled; cam >= -4 * scaled; cam--) {
                float centerW = (float)(cam + viewLen / 2) * 16.0f / (float)zf;
                int square = 12345, edge = 12345;
                mapViewCameraAxis(0, &centerW, viewLen, zf, &square, &edge);
                UT_ASSERT_MSG(edge >= 0 && edge < scaled,
                              "precise zf %d view %d cam %d: edge %d outside 0..%d",
                              zf, viewLen, cam, edge, scaled - 1);
                int drawn = -square * scaled - edge;
                UT_ASSERT_MSG(drawn == -cam,
                              "precise zf %d view %d cam %d: square 0 drawn at %d, "
                              "the camera puts it at %d",
                              zf, viewLen, cam, drawn, -cam);
            }
        }
    }

    UT_ASSERT(mapViewSquareInMap(0, 0));
    UT_ASSERT(mapViewSquareInMap(255, 255));
    UT_ASSERT(!mapViewSquareInMap(-1, 10));
    UT_ASSERT(!mapViewSquareInMap(10, -1));
    UT_ASSERT(!mapViewSquareInMap(256, 10));
    UT_ASSERT(!mapViewSquareInMap(10, 256));
    return 0;
}
