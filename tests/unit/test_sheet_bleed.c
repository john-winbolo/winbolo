/*
 * The colour under the tile sheet's transparency
 * (tileLoaderBleedEdges, src/gui/sdl3/tileloader.c).
 *
 * Transparent is not colourless. The PNGs under data/svg store the key
 * colour with alpha 0, and the whole-sheet BMP path blends the key away to
 * black, so a fully transparent texel of the built sheet carries whatever
 * its source left behind. Nearest sampling never shows it; anything that
 * blends across a sprite's edge — Linear or Pixel Art filtering, a rotation,
 * an ImGui draw that forced its own sampler — mixes it into the edge, and a
 * tank comes out wearing a green outline.
 *
 * This pins the invariant that stops it: after a build, no transparent texel
 * of the sheet holds the key colour. The bleed is what makes that true
 * without leaving a dark edge instead — it hands each transparent texel the
 * colour of the sprite beside it.
 *
 * The status slots are the deliberate exception and are checked as one: they
 * are drawn in the key colour as a colour, sdl3DrawSetBasesStatusClear fills
 * black behind them, and makeStatusSlotsOpaque seals them before the bleed
 * runs. Erasing their green would leave the player an empty status bar, so
 * this asserts they still carry it.
 *
 * The two direct tests below drive the helper on surfaces built here, where
 * the answer can be written down exactly, rather than inferring it from a
 * sheet whose art may change.
 */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "tileloader.h"
#include "tilemap.h"
#include "test_harness.h"

/* A slot rect on a sheet built at `scale`. */
static SDL_Rect slotRect(const TileMapEntry *e, int scale) {
    SDL_Rect r = { e->sheetX * scale, e->sheetY * scale,
                   e->width * scale, e->height * scale };
    return r;
}

static bool isStatusName(const char *name) {
    return strncmp(name, "status_", 7) == 0;
}

/* ------------------------------------------------------------------ */
/* The helper on its own                                              */
/* ------------------------------------------------------------------ */

/* One opaque texel in a field of transparent key colour. Two passes reach
   every texel of a 5x5, so afterwards none of them may still be green, the
   ring touching the centre must have taken its colour, and not one may have
   gained alpha. */
static int checkBleedSpreadsColour(void) {
    SDL_Surface *s = SDL_CreateSurface(5, 5, SDL_PIXELFORMAT_RGBA32);
    if (s == NULL) UT_FAIL("SDL_CreateSurface failed: %s", SDL_GetError());

    for (int y = 0; y < 5; y++) {
        unsigned char *row = (unsigned char *)s->pixels + (size_t)y * (size_t)s->pitch;
        for (int x = 0; x < 5; x++) {
            unsigned char *p = row + (size_t)x * 4;
            p[0] = 0; p[1] = 255; p[2] = 0; p[3] = 0;
        }
    }
    {
        unsigned char *c = (unsigned char *)s->pixels + (size_t)2 * (size_t)s->pitch + 2 * 4;
        c[0] = 200; c[1] = 40; c[2] = 10; c[3] = 255;
    }

    tileLoaderBleedEdges(s, NULL);

    for (int y = 0; y < 5; y++) {
        const unsigned char *row = (const unsigned char *)s->pixels +
                                   (size_t)y * (size_t)s->pitch;
        for (int x = 0; x < 5; x++) {
            const unsigned char *p = row + (size_t)x * 4;
            bool centre = (x == 2 && y == 2);

            if (centre) {
                if (p[3] != 255) {
                    SDL_DestroySurface(s);
                    UT_FAIL("the opaque texel lost its alpha (%d)", p[3]);
                }
                continue;
            }

            if (p[3] != 0) {
                SDL_DestroySurface(s);
                UT_FAIL("transparent texel %d,%d gained alpha %d; the bleed "
                        "must change colour only", x, y, p[3]);
            }
            if (tileLoaderIsSheetKeyColor(p[0], p[1], p[2])) {
                SDL_DestroySurface(s);
                UT_FAIL("transparent texel %d,%d still holds the key colour", x, y);
            }
            /* The eight around the centre see it and nothing else, so they
               take its colour exactly. */
            if (x >= 1 && x <= 3 && y >= 1 && y <= 3) {
                if (p[0] != 200 || p[1] != 40 || p[2] != 10) {
                    SDL_DestroySurface(s);
                    UT_FAIL("texel %d,%d beside the sprite is %d,%d,%d, not the "
                            "200,40,10 it should have taken", x, y, p[0], p[1], p[2]);
                }
            }
        }
    }

    SDL_DestroySurface(s);
    return 0;
}

/* The sheet packs its sprites edge to edge, so the bleed has to stay inside
   the rect it is given. Two 2x2 quarters of a 4x2 surface: bleeding the left
   one must not read from or write to the right one. */
static int checkBleedStaysInsideClip(void) {
    SDL_Surface *s = SDL_CreateSurface(4, 2, SDL_PIXELFORMAT_RGBA32);
    if (s == NULL) UT_FAIL("SDL_CreateSurface failed: %s", SDL_GetError());

    /* Left half: one opaque red over one transparent green.
       Right half: one opaque blue over one transparent green. */
    static const unsigned char px[2][4][4] = {
        { {200, 0, 0, 255}, {0, 255, 0, 0}, {0, 0, 200, 255}, {0, 255, 0, 0} },
        { {0, 255, 0, 0},   {0, 255, 0, 0}, {0, 255, 0, 0},   {0, 255, 0, 0} },
    };
    for (int y = 0; y < 2; y++) {
        unsigned char *row = (unsigned char *)s->pixels + (size_t)y * (size_t)s->pitch;
        for (int x = 0; x < 4; x++) {
            memcpy(row + (size_t)x * 4, px[y][x], 4);
        }
    }

    SDL_Rect left = { 0, 0, 2, 2 };
    tileLoaderBleedEdges(s, &left);

    const unsigned char *r0 = (const unsigned char *)s->pixels;
    const unsigned char *r1 = (const unsigned char *)s->pixels + (size_t)s->pitch;

    /* Inside the clip the red spread. */
    if (r0[1 * 4 + 0] != 200 || r0[1 * 4 + 1] != 0 || r0[1 * 4 + 2] != 0) {
        SDL_DestroySurface(s);
        UT_FAIL("the texel beside the red one is %d,%d,%d, not the red it "
                "should have taken", r0[4], r0[5], r0[6]);
    }

    /* Outside it nothing moved — neither the blue sprite nor the green
       beside it, which a whole-surface pass would have rewritten. */
    if (r0[2 * 4 + 2] != 200 || r0[2 * 4 + 3] != 255) {
        SDL_DestroySurface(s);
        UT_FAIL("the sprite outside the clip was written to");
    }
    if (r0[3 * 4 + 1] != 255 || r0[3 * 4 + 3] != 0 ||
        r1[2 * 4 + 1] != 255 || r1[3 * 4 + 1] != 255) {
        SDL_DestroySurface(s);
        UT_FAIL("a texel outside the clip was bled into");
    }

    SDL_DestroySurface(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* The sheet the game actually draws from                             */
/* ------------------------------------------------------------------ */

static int checkBuiltSheetHasNoKeyUnderAlpha(void) {
    /* No skin and Classic detail: the built-in chain, which is what every
       player gets by default and the one carrying the key colour in its
       PNGs. Scale 1 keeps the slot arithmetic below trivial. */
    SDL_Surface *sheet = tileLoaderBuildSheetFor(NULL, TILE_SIZE_X,
                                                 TILE_DETAIL_CLASSIC);
    if (sheet == NULL) UT_FAIL("tileLoaderBuildSheetFor returned NULL");
    if (sheet->format != SDL_PIXELFORMAT_RGBA32) {
        SDL_DestroySurface(sheet);
        UT_FAIL("the built sheet is not RGBA32, so its bytes cannot be read here");
    }

    int rc = 0;
    int statusKeyOpaqueTotal = 0;

    for (int i = 0; gTileMap[i].name != NULL && rc == 0; i++) {
        const TileMapEntry *e = &gTileMap[i];
        SDL_Rect r = slotRect(e, 1);
        bool status = isStatusName(e->name);
        int keyUnderAlpha = 0, keyOpaque = 0, transparent = 0;

        for (int y = r.y; y < r.y + r.h && y < sheet->h; y++) {
            const unsigned char *row = (const unsigned char *)sheet->pixels +
                                       (size_t)y * (size_t)sheet->pitch;
            for (int x = r.x; x < r.x + r.w && x < sheet->w; x++) {
                const unsigned char *p = row + (size_t)x * 4;
                bool key = tileLoaderIsSheetKeyColor(p[0], p[1], p[2]);
                if (p[3] == 0) {
                    transparent++;
                    if (key) keyUnderAlpha++;
                } else if (key) {
                    keyOpaque++;
                }
            }
        }

        if (keyUnderAlpha != 0) {
            fprintf(stderr,
                    "FAIL %s:%d: %s holds %d transparent texels still carrying "
                    "the key colour; a sampler that blends across its edge will "
                    "show them as a green fringe\n",
                    __FILE__, __LINE__, e->name, keyUnderAlpha);
            rc = 1;
            break;
        }

        /* Sealed opaque by makeStatusSlotsOpaque before the bleed runs, which
           is both what leaves the bleed nothing to touch here and what lets
           the full screen map's status column blend the panel at the player's
           transparency without the icons dropping out. */
        if (status) {
            statusKeyOpaqueTotal += keyOpaque;
            if (transparent != 0) {
                fprintf(stderr,
                        "FAIL %s:%d: status slot %s has %d transparent texels; "
                        "sealing them is what keeps the bleed off these icons "
                        "and what stops them dropping out of a transparent "
                        "status column\n",
                        __FILE__, __LINE__, e->name, transparent);
                rc = 1;
                break;
            }
        }
    }

    SDL_DestroySurface(sheet);
    if (rc != 0) return rc;

    /* Five of the ten status icons are drawn in the key colour as a colour —
       the art stores it as their transparency and the seal turns it back into
       green. Not every icon has any, so this is the aggregate rather than a
       per-slot rule: zero across all of them means something erased it, which
       is the status bar coming out empty. */
    UT_ASSERT_MSG(statusKeyOpaqueTotal > 0,
                  "no status slot carries the key colour opaquely any more; "
                  "the icons drawn in green have been erased");
    return 0;
}

/* ------------------------------------------------------------------ */

/* test_main.c owns the SDL runtime for the whole binary, so neither of these
   inits or quits it — an SDL_Quit() here would take the runtime out from
   under every test that runs after. */

int run_sheet_bleed_edges(void) {
    int rc = checkBleedSpreadsColour();
    if (rc == 0) rc = checkBleedStaysInsideClip();
    return rc;
}

int run_sheet_no_key_under_alpha(void) {
    return checkBuiltSheetHasNoKeyUnderAlpha();
}
