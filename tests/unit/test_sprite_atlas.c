/*
 * What sits in the texel ring around a sprite slot (test_sprite_atlas.c).
 *
 * SDL_RenderTexture's source rect picks texture coordinates; it does not
 * clamp the filter kernel. Under Linear or Pixel Art filtering the sample
 * nearest a rect edge mixes in the texel just outside it, and on a sheet
 * whose slots are packed edge to edge that texel is another sprite. On the
 * classic layout tank_selfboat_07..15 sit directly under pill_evil_02..10,
 * so a tank on a boat draws a line of the pillbox's bottom row above itself
 * — black, with the pillbox's two red texels showing at columns 7 and 8.
 *
 * The rule that stops it, and what these check: every texel in the ring
 * around a slot equals the slot's own nearest edge texel, so a sampler that
 * reads across the edge blends the sprite into itself whatever it reaches.
 * The sprite atlas is built to hold it. tileLoaderBleedEdges cannot: it only
 * rewrites transparent texels inside a slot, which leaves an opaque
 * neighbour exactly where it was.
 *
 * isolated: the ring holds, at both sheet scales, for every slot of the
 * atlas — and the atlas covers every sprite the tank, shell and LGM drawers
 * can ask for.
 *
 * packed: the same walk over the classic sheet, which is the layout the
 * atlas is copied out of and the one skins are authored against. It is
 * expected to fail the rule; this states by how much and names the boat
 * under the pillbox, so the reason the atlas exists is written down rather
 * than remembered.
 */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "sprite_atlas.h"
#include "tileloader.h"
#include "tilemap.h"
#include "test_harness.h"

/* A texel of an RGBA32 surface. */
static const unsigned char *texel(const SDL_Surface *s, int x, int y) {
    return (const unsigned char *)s->pixels + (size_t)y * (size_t)s->pitch +
           (size_t)x * 4;
}

/* Which gTileMap slot owns (x, y) on a sheet built at `scale`, for failure
   messages. NULL when the texel is in none of them. */
static const char *sheetOwnerAt(int x, int y, int scale) {
    for (int i = 0; gTileMap[i].name != NULL; i++) {
        const TileMapEntry *e = &gTileMap[i];
        if (x >= e->sheetX * scale && x < (e->sheetX + e->width) * scale &&
            y >= e->sheetY * scale && y < (e->sheetY + e->height) * scale) {
            return e->name;
        }
    }
    return NULL;
}

/* Count the ring texels around `slot` that are not a copy of the slot's
   nearest edge texel. `report` names the first one found, or is left alone
   when the ring holds. A ring texel outside the surface is not counted:
   nothing is there to be sampled. */
static int ringMismatches(const SDL_Surface *s, const SDL_Rect *slot,
                          int scale, char *report, size_t reportLen) {
    int bad = 0;

    for (int y = slot->y - 1; y <= slot->y + slot->h; y++) {
        for (int x = slot->x - 1; x <= slot->x + slot->w; x++) {
            /* Inside the slot is the sprite itself. */
            if (x >= slot->x && x < slot->x + slot->w &&
                y >= slot->y && y < slot->y + slot->h) {
                continue;
            }
            if (x < 0 || y < 0 || x >= s->w || y >= s->h) continue;

            /* The edge texel this ring position should be a copy of: the
               slot corner nearest it, so the four corners take the corner
               texel and the sides take the one they touch. */
            int ex = x < slot->x ? slot->x
                                 : (x >= slot->x + slot->w ? slot->x + slot->w - 1 : x);
            int ey = y < slot->y ? slot->y
                                 : (y >= slot->y + slot->h ? slot->y + slot->h - 1 : y);

            const unsigned char *got  = texel(s, x, y);
            const unsigned char *want = texel(s, ex, ey);
            if (memcmp(got, want, 4) == 0) continue;

            if (bad == 0 && report != NULL) {
                const char *owner = sheetOwnerAt(x, y, scale);
                SDL_snprintf(report, reportLen,
                             "texel %d,%d is %d,%d,%d,%d (%s) where the "
                             "slot's own edge texel %d,%d is %d,%d,%d,%d",
                             x, y, got[0], got[1], got[2], got[3],
                             owner ? owner : "no slot",
                             ex, ey, want[0], want[1], want[2], want[3]);
            }
            bad++;
        }
    }
    return bad;
}

/* ------------------------------------------------------------------ */

/* Every slot of the atlas, at a sheet scale of 1 and of 2. */
static int checkAtlasAtScale(int scale) {
    SDL_Surface *sheet = tileLoaderBuildSheetFor(NULL, TILE_SIZE_X * scale,
                                                 TILE_DETAIL_CLASSIC);
    if (sheet == NULL) UT_FAIL("tileLoaderBuildSheetFor returned NULL");
    if (sheet->format != SDL_PIXELFORMAT_RGBA32) {
        SDL_DestroySurface(sheet);
        UT_FAIL("the built sheet is not RGBA32, so its bytes cannot be read here");
    }

    SpriteAtlas *atlas = tileLoaderBuildSpriteAtlas(sheet, scale);
    SDL_DestroySurface(sheet);
    if (atlas == NULL) UT_FAIL("tileLoaderBuildSpriteAtlas returned NULL");

    SDL_Surface *s = atlas->surface;
    if (s == NULL || s->format != SDL_PIXELFORMAT_RGBA32) {
        tileLoaderFreeSpriteAtlas(atlas);
        UT_FAIL("the atlas came back without an RGBA32 surface");
    }

    int rc = 0;
    for (int i = 0; i < atlas->count && rc == 0; i++) {
        const SpriteAtlasSlot *sl = &atlas->slots[i];
        SDL_Rect r = { sl->x, sl->y, sl->w * scale, sl->h * scale };
        char why[256] = { 0 };

        /* A slot on the atlas edge would have nowhere to put its ring, and
           ringMismatches skips what it cannot read — so check the room is
           there rather than letting the walk pass by default. */
        if (r.x < SPRITE_ATLAS_GUTTER || r.y < SPRITE_ATLAS_GUTTER ||
            r.x + r.w + SPRITE_ATLAS_GUTTER > s->w ||
            r.y + r.h + SPRITE_ATLAS_GUTTER > s->h) {
            fprintf(stderr,
                    "FAIL %s:%d: at scale %d the slot for sheet %d,%d lands at "
                    "%d,%d %dx%d on a %dx%d atlas, with no room for its ring\n",
                    __FILE__, __LINE__, scale, sl->srcX, sl->srcY,
                    r.x, r.y, r.w, r.h, s->w, s->h);
            rc = 1;
            break;
        }

        int bad = ringMismatches(s, &r, scale, why, sizeof(why));
        if (bad != 0) {
            fprintf(stderr,
                    "FAIL %s:%d: at scale %d the atlas slot for sheet %d,%d has "
                    "%d ring texels that are not a copy of its own edge: %s\n",
                    __FILE__, __LINE__, scale, sl->srcX, sl->srcY, bad, why);
            rc = 1;
        }
    }

    if (rc == 0) {
        /* Every sprite the drawers can ask for has to be in there, or the
           ones left behind quietly keep sampling the packed sheet. */
        int want = 0;
        for (int i = 0; gTileMap[i].name != NULL; i++) {
            if (tileLoaderIsSpriteSlot(gTileMap[i].name)) want++;
        }
        if (atlas->count != want) {
            fprintf(stderr,
                    "FAIL %s:%d: the atlas holds %d slots where gTileMap has %d "
                    "sprite entries\n",
                    __FILE__, __LINE__, atlas->count, want);
            rc = 1;
        }
    }

    tileLoaderFreeSpriteAtlas(atlas);
    return rc;
}

int run_sprite_atlas_isolated(void) {
    int rc = checkAtlasAtScale(1);
    if (rc == 0) rc = checkAtlasAtScale(2);
    return rc;
}

/* ------------------------------------------------------------------ */

/* The drawers hand spriteAtlasFind the tiles.h coordinates of the frame they
 * picked, so every sprite slot has to come back by those, and a terrain slot
 * has to not — that is what leaves the terrain drawing from the sheet. */
int run_sprite_atlas_lookup(void) {
    const int scale = 2;
    SDL_Surface *sheet = tileLoaderBuildSheetFor(NULL, TILE_SIZE_X * scale,
                                                 TILE_DETAIL_CLASSIC);
    if (sheet == NULL) UT_FAIL("tileLoaderBuildSheetFor returned NULL");

    SpriteAtlas *atlas = tileLoaderBuildSpriteAtlas(sheet, scale);
    SDL_DestroySurface(sheet);
    if (atlas == NULL) UT_FAIL("tileLoaderBuildSpriteAtlas returned NULL");

    int rc = 0;
    for (int i = 0; gTileMap[i].name != NULL && rc == 0; i++) {
        const TileMapEntry *e = &gTileMap[i];
        SDL_FRect r = { 0, 0, 0, 0 };
        bool got = spriteAtlasFind(atlas, e->sheetX, e->sheetY,
                                   e->width, e->height, &r);
        bool want = tileLoaderIsSpriteSlot(e->name);

        if (got != want) {
            fprintf(stderr,
                    "FAIL %s:%d: spriteAtlasFind %s %s, and it should %s\n",
                    __FILE__, __LINE__, e->name,
                    got ? "found" : "did not find",
                    want ? "have" : "not have");
            rc = 1;
            break;
        }
        if (!got) continue;

        if (r.w != (float)(e->width * scale) || r.h != (float)(e->height * scale)) {
            fprintf(stderr,
                    "FAIL %s:%d: %s came back %gx%g, not the %dx%d it is drawn "
                    "at a sheet scale of %d\n",
                    __FILE__, __LINE__, e->name, (double)r.w, (double)r.h,
                    e->width * scale, e->height * scale, scale);
            rc = 1;
        }
    }

    /* The frame from the screenshot, by the constant the drawer switches to,
       so the lookup is pinned against tiles.h and not only against the
       table it was built from. */
    if (rc == 0) {
        SDL_FRect r = { 0, 0, 0, 0 };
        if (!spriteAtlasFind(atlas, TANK_SELFBOAT_14_X, TANK_SELFBOAT_14_Y,
                             TILE_SIZE_X, TILE_SIZE_Y, &r)) {
            fprintf(stderr,
                    "FAIL %s:%d: the boat facing north-west is not in the "
                    "atlas, so it still draws the pillbox row above itself\n",
                    __FILE__, __LINE__);
            rc = 1;
        }
    }

    /* Deep sea is terrain: it draws on the tile grid at whole pixels, and
       leaving it on the sheet is what keeps the copy small. */
    if (rc == 0) {
        SDL_FRect r = { 0, 0, 0, 0 };
        if (spriteAtlasFind(atlas, DEEP_SEA_SOLID_X, DEEP_SEA_SOLID_Y,
                            TILE_SIZE_X, TILE_SIZE_Y, &r)) {
            fprintf(stderr,
                    "FAIL %s:%d: deep sea is in the sprite atlas; the terrain "
                    "is meant to keep drawing from the sheet\n",
                    __FILE__, __LINE__);
            rc = 1;
        }
    }

    tileLoaderFreeSpriteAtlas(atlas);
    return rc;
}

/* ------------------------------------------------------------------ */

int run_sprite_atlas_packed_sheet_unsafe(void) {
    SDL_Surface *sheet = tileLoaderBuildSheetFor(NULL, TILE_SIZE_X,
                                                 TILE_DETAIL_CLASSIC);
    if (sheet == NULL) UT_FAIL("tileLoaderBuildSheetFor returned NULL");

    int leaky = 0, checked = 0;
    bool boatSeen = false;

    for (int i = 0; gTileMap[i].name != NULL; i++) {
        const TileMapEntry *e = &gTileMap[i];
        if (!tileLoaderIsSpriteSlot(e->name)) continue;
        SDL_Rect r = { e->sheetX, e->sheetY, e->width, e->height };
        checked++;
        if (ringMismatches(sheet, &r, 1, NULL, 0) != 0) {
            leaky++;
            if (strcmp(e->name, "tank_selfboat_09") == 0) boatSeen = true;
        }
    }

    /* The one from the screenshot: pill_evil_09's bottom row is 16 opaque
       texels sitting on tank_selfboat_09's top edge. */
    int aboveBoat = 0;
    for (int i = 0; gTileMap[i].name != NULL; i++) {
        const TileMapEntry *e = &gTileMap[i];
        if (strcmp(e->name, "tank_selfboat_09") != 0) continue;
        for (int x = e->sheetX; x < e->sheetX + e->width; x++) {
            if (texel(sheet, x, e->sheetY - 1)[3] == 255) aboveBoat++;
        }
        break;
    }

    SDL_DestroySurface(sheet);

    UT_ASSERT_MSG(leaky > 0,
                  "every one of the %d sprite slots on the classic sheet now "
                  "has a clean ring; if the sheet gained gutters the atlas is "
                  "no longer buying anything and this can go", checked);
    UT_ASSERT_MSG(boatSeen,
                  "tank_selfboat_09's ring is clean on the classic sheet, "
                  "which is not the layout the artefact was traced on");
    UT_ASSERT_MSG(aboveBoat == 16,
                  "%d of the 16 texels above tank_selfboat_09 are opaque, not "
                  "16; the pillbox row that leaks into the boat has moved",
                  aboveBoat);
    return 0;
}
