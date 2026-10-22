/*
 * The isometric sprite sheet (test_iso_sheet.c).
 *
 * iso_sheet.c lays the isometric sprites out on a sheet of their own, each
 * in a fixed cell with a ring of its own edge texels around it, reads each
 * one through the chain tileloader.c resolves a name with, and makes the
 * wall faces and the forest band from classic sprites when no file supplies
 * them. These check the layout (every cell inside the sheet, none
 * overlapping), what the stock art fills, what the fallbacks fill when the
 * art directory holds no iso_ files, and the ring.
 */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "iso_sheet.h"
#include "tileloader.h"
#include "test_harness.h"

/* A texel of an RGBA32 surface. */
static const unsigned char *texel(const SDL_Surface *s, int x, int y) {
    return (const unsigned char *)s->pixels + (size_t)y * (size_t)s->pitch +
           (size_t)x * 4;
}

/* The rect of slot i at a density, through the public lookup. */
static bool rectOf(int i, int density, SDL_Rect *r, bool *present) {
    const IsoSheetSlot *slot = isoSheetSlotAt(i);
    if (slot == NULL) return false;
    return isoSheetRect(slot->kind, slot->team, slot->frame, density, r, present);
}

static bool rectsOverlap(const SDL_Rect *a, const SDL_Rect *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

/* ------------------------------------------------------------------ */

/* Every cell has a rect, the rect and its ring sit inside the sheet, no two
 * cells touch, and the index round-trips through the lookup. At density 1
 * and 3, since the layout is arithmetic on the density. */
static int checkLayoutAt(int density) {
    int sheetW = 0, sheetH = 0;
    SDL_Rect rects[ISO_SHEET_SLOT_COUNT];

    isoSheetSize(density, &sheetW, &sheetH);
    UT_ASSERT_MSG(sheetW > 0 && sheetH > 0, "isoSheetSize gave %dx%d", sheetW, sheetH);
    UT_ASSERT_MSG(isoSheetSlotCount() == ISO_SHEET_SLOT_COUNT,
                  "isoSheetSlotCount is %d", isoSheetSlotCount());

    for (int i = 0; i < ISO_SHEET_SLOT_COUNT; i++) {
        const IsoSheetSlot *slot = isoSheetSlotAt(i);
        bool present = false;
        UT_ASSERT_MSG(slot != NULL && slot->name != NULL, "slot %d is missing", i);
        UT_ASSERT_MSG(strncmp(slot->name, "iso_", 4) == 0,
                      "slot %d is named %s, not iso_*", i, slot->name);
        UT_ASSERT_MSG(isoSheetSlotIndex(slot->kind, slot->team, slot->frame) == i,
                      "%s is slot %d but its kind, team and frame look up as %d",
                      slot->name, i,
                      isoSheetSlotIndex(slot->kind, slot->team, slot->frame));
        UT_ASSERT_MSG(rectOf(i, density, &rects[i], &present),
                      "%s has no rect", slot->name);
        UT_ASSERT_MSG(rects[i].w == slot->w * density && rects[i].h == slot->h * density,
                      "%s is %dx%d at density %d, not %dx%d", slot->name,
                      rects[i].w, rects[i].h, density,
                      slot->w * density, slot->h * density);
        UT_ASSERT_MSG(rects[i].x >= ISO_SHEET_GUTTER && rects[i].y >= ISO_SHEET_GUTTER &&
                      rects[i].x + rects[i].w + ISO_SHEET_GUTTER <= sheetW &&
                      rects[i].y + rects[i].h + ISO_SHEET_GUTTER <= sheetH,
                      "%s at %d,%d %dx%d has no room for its ring on a %dx%d sheet",
                      slot->name, rects[i].x, rects[i].y, rects[i].w, rects[i].h,
                      sheetW, sheetH);
    }

    for (int i = 0; i < ISO_SHEET_SLOT_COUNT; i++) {
        for (int j = i + 1; j < ISO_SHEET_SLOT_COUNT; j++) {
            /* Grown by the gutter, so two rings may not share a texel
               either. */
            SDL_Rect a = { rects[i].x - ISO_SHEET_GUTTER, rects[i].y - ISO_SHEET_GUTTER,
                           rects[i].w + 2 * ISO_SHEET_GUTTER, rects[i].h + 2 * ISO_SHEET_GUTTER };
            SDL_Rect b = { rects[j].x - ISO_SHEET_GUTTER, rects[j].y - ISO_SHEET_GUTTER,
                           rects[j].w + 2 * ISO_SHEET_GUTTER, rects[j].h + 2 * ISO_SHEET_GUTTER };
            UT_ASSERT_MSG(!rectsOverlap(&a, &b), "%s and %s overlap at density %d",
                          isoSheetSlotAt(i)->name, isoSheetSlotAt(j)->name, density);
        }
    }

    /* The combinations that name no cell. */
    {
        SDL_Rect r;
        bool present;
        UT_ASSERT(!isoSheetRect(ISO_SPRITE_TANK, ISO_TEAM_NEUTRAL, 0, density, &r, &present));
        UT_ASSERT(!isoSheetRect(ISO_SPRITE_TANK, ISO_TEAM_SELF, 16, density, &r, &present));
        UT_ASSERT(!isoSheetRect(ISO_SPRITE_PILLBOX, ISO_TEAM_SELF, 15, density, &r, &present));
        UT_ASSERT(!isoSheetRect(ISO_SPRITE_LGM, ISO_TEAM_SELF, 3, density, &r, &present));
        UT_ASSERT(!isoSheetRect(ISO_SPRITE_FACE_PILL, ISO_TEAM_SELF, 1, density, &r, &present));
        UT_ASSERT(isoSheetRect(ISO_SPRITE_BASE, ISO_TEAM_NEUTRAL, 0, density, &r, &present));
    }
    return 0;
}

int run_iso_sheet_layout(void) {
    int rc = checkLayoutAt(1);
    if (rc == 0) rc = checkLayoutAt(3);
    return rc;
}

/* ------------------------------------------------------------------ */

/* The stock data/svg beside the binary holds every iso_ file, so a build
 * against no skin fills every cell, and the surface is the size the layout
 * says. */
int run_iso_sheet_stock_present(void) {
    const int density = 2;
    int sheetW = 0, sheetH = 0;
    SDL_Surface *sheet = isoSheetBuild(NULL, density, TILE_DETAIL_CLASSIC);
    if (sheet == NULL) UT_FAIL("isoSheetBuild returned NULL");

    isoSheetSize(density, &sheetW, &sheetH);
    if (sheet->format != SDL_PIXELFORMAT_RGBA32 || sheet->w != sheetW || sheet->h != sheetH) {
        int w = sheet->w, h = sheet->h;
        SDL_DestroySurface(sheet);
        UT_FAIL("the sheet is %dx%d, not the %dx%d RGBA32 the layout says", w, h, sheetW, sheetH);
    }

    for (int i = 0; i < ISO_SHEET_SLOT_COUNT; i++) {
        SDL_Rect r;
        bool present = false;
        const IsoSheetSlot *slot = isoSheetSlotAt(i);
        if (!rectOf(i, density, &r, &present) || !present) {
            SDL_DestroySurface(sheet);
            UT_FAIL("%s is absent from a stock build", slot->name);
        }
        /* Present means something landed there: at least one texel of the
           cell is not fully transparent. */
        bool anyOpaque = false;
        for (int y = r.y; y < r.y + r.h && !anyOpaque; y++) {
            for (int x = r.x; x < r.x + r.w; x++) {
                if (texel(sheet, x, y)[3] != 0) { anyOpaque = true; break; }
            }
        }
        if (!anyOpaque) {
            SDL_DestroySurface(sheet);
            UT_FAIL("%s reports present but its cell is fully transparent", slot->name);
        }
    }

    SDL_DestroySurface(sheet);
    return 0;
}

/* ------------------------------------------------------------------ */

/* An art directory under the binary holding only the five classic sprites
 * the fallbacks are cut from and none of the iso_ files. */
#define FALLBACK_DIR "iso_sheet_fallback_test"

static const char *kFallbackSources[] = {
    "building_solid", "shot_building", "pillbox_good_15", "base_neutral", "forest"
};
#define FALLBACK_SOURCE_COUNT ((int)(sizeof(kFallbackSources) / sizeof(kFallbackSources[0])))

/* Copy one classic PNG from the stock art into the test directory. */
static bool copyClassicPng(const char *base, const char *dir, const char *name) {
    char from[1024], to[1024];
    size_t len = 0;
    void *buf;
    bool ok;

    SDL_snprintf(from, sizeof(from), "%sdata/svg/%s.png", base, name);
    SDL_snprintf(to, sizeof(to), "%sdata/svg/%s.png", dir, name);
    buf = SDL_LoadFile(from, &len);
    if (buf == NULL) return false;
    ok = SDL_SaveFile(to, buf, len);
    SDL_free(buf);
    return ok;
}

static void removeFallbackDir(const char *dir) {
    char path[1024];
    for (int i = 0; i < FALLBACK_SOURCE_COUNT; i++) {
        SDL_snprintf(path, sizeof(path), "%sdata/svg/%s.png", dir, kFallbackSources[i]);
        SDL_RemovePath(path);
    }
    SDL_snprintf(path, sizeof(path), "%sdata/svg", dir);
    SDL_RemovePath(path);
    SDL_snprintf(path, sizeof(path), "%sdata", dir);
    SDL_RemovePath(path);
    SDL_RemovePath(dir);
}

/* With the iso_ files missing, the four faces and the band are made from the
 * classic sprites and report present, darker than their source; a tank, a
 * pillbox, a base and the builder report absent and their cells stay
 * transparent. */
int run_iso_sheet_fallbacks(void) {
    const int density = 1;
    const char *base = SDL_GetBasePath();
    char dir[1024], svgDir[1024];
    SDL_Surface *sheet;
    int rc = 0;

    if (base == NULL) base = "";
    /* A directory private to this process and test, so two runs on one
       build directory never share it. isoSheetBuildFrom wants the trailing
       slash SDL_GetBasePath carries. */
    if (!utScratchPath(dir, sizeof(dir) - 1, FALLBACK_DIR)) {
        UT_FAIL("could not make a scratch directory");
    }
    SDL_strlcat(dir, "/", sizeof(dir));
    SDL_snprintf(svgDir, sizeof(svgDir), "%sdata/svg", dir);

    if (!SDL_CreateDirectory(svgDir)) {
        UT_FAIL("could not create %s: %s", svgDir, SDL_GetError());
    }
    for (int i = 0; i < FALLBACK_SOURCE_COUNT; i++) {
        if (!copyClassicPng(base, dir, kFallbackSources[i])) {
            removeFallbackDir(dir);
            UT_FAIL("could not copy %s.png into %s", kFallbackSources[i], svgDir);
        }
    }

    sheet = isoSheetBuildFrom(NULL, density, TILE_DETAIL_CLASSIC, dir);
    if (sheet == NULL) {
        removeFallbackDir(dir);
        UT_FAIL("isoSheetBuildFrom returned NULL");
    }

    static const IsoSpriteKind made[] = {
        ISO_SPRITE_FACE_BUILDING, ISO_SPRITE_FACE_SHOT_BUILDING,
        ISO_SPRITE_FACE_PILL, ISO_SPRITE_FACE_BASE, ISO_SPRITE_FOREST_BAND
    };
    for (int i = 0; i < (int)(sizeof(made) / sizeof(made[0])) && rc == 0; i++) {
        SDL_Rect r;
        bool present = false;
        const IsoSheetSlot *slot = isoSheetSlotAt(isoSheetSlotIndex(made[i], ISO_TEAM_SELF, 0));
        if (!isoSheetRect(made[i], ISO_TEAM_SELF, 0, density, &r, &present) || !present) {
            fprintf(stderr, "FAIL %s:%d: %s is absent with no file; the fallback did not fill it\n",
                    __FILE__, __LINE__, slot->name);
            rc = 1;
            break;
        }
        /* Something opaque landed in the cell and nothing in it is at full
           brightness on every channel: the fallback darkens what it cuts. */
        bool anyOpaque = false, anyFullBright = false;
        for (int y = r.y; y < r.y + r.h; y++) {
            for (int x = r.x; x < r.x + r.w; x++) {
                const unsigned char *p = texel(sheet, x, y);
                if (p[3] == 0) continue;
                anyOpaque = true;
                if (p[0] == 255 || p[1] == 255 || p[2] == 255) anyFullBright = true;
            }
        }
        if (!anyOpaque) {
            fprintf(stderr, "FAIL %s:%d: %s reports present but its cell is fully transparent\n",
                    __FILE__, __LINE__, slot->name);
            rc = 1;
        } else if (anyFullBright) {
            fprintf(stderr, "FAIL %s:%d: %s has a channel at 255; a generated face is darkened\n",
                    __FILE__, __LINE__, slot->name);
            rc = 1;
        }
    }

    static const struct { IsoSpriteKind kind; IsoTeam team; int frame; } absent[] = {
        { ISO_SPRITE_TANK,           ISO_TEAM_SELF,    0 },
        { ISO_SPRITE_TANK_BOAT,      ISO_TEAM_EVIL,    9 },
        { ISO_SPRITE_PILLBOX,        ISO_TEAM_GOOD,   15 },
        { ISO_SPRITE_BASE,           ISO_TEAM_NEUTRAL, 0 },
        { ISO_SPRITE_LGM_HELICOPTER, ISO_TEAM_SELF,    0 }
    };
    for (int i = 0; i < (int)(sizeof(absent) / sizeof(absent[0])) && rc == 0; i++) {
        SDL_Rect r;
        bool present = true;
        const IsoSheetSlot *slot = isoSheetSlotAt(
            isoSheetSlotIndex(absent[i].kind, absent[i].team, absent[i].frame));
        if (!isoSheetRect(absent[i].kind, absent[i].team, absent[i].frame, density, &r, &present)) {
            fprintf(stderr, "FAIL %s:%d: %s has no rect\n", __FILE__, __LINE__, slot->name);
            rc = 1;
            break;
        }
        if (present) {
            fprintf(stderr, "FAIL %s:%d: %s reports present with no file and no fallback\n",
                    __FILE__, __LINE__, slot->name);
            rc = 1;
            break;
        }
        for (int y = r.y - ISO_SHEET_GUTTER; y < r.y + r.h + ISO_SHEET_GUTTER && rc == 0; y++) {
            for (int x = r.x - ISO_SHEET_GUTTER; x < r.x + r.w + ISO_SHEET_GUTTER; x++) {
                const unsigned char *p = texel(sheet, x, y);
                if (p[0] | p[1] | p[2] | p[3]) {
                    fprintf(stderr, "FAIL %s:%d: %s is absent but texel %d,%d is %d,%d,%d,%d\n",
                            __FILE__, __LINE__, slot->name, x, y, p[0], p[1], p[2], p[3]);
                    rc = 1;
                    break;
                }
            }
        }
    }

    SDL_DestroySurface(sheet);
    removeFallbackDir(dir);

    /* Put the stock sheet's presence back for whatever runs next in the
       same process: isoSheetRect describes the last build. */
    sheet = isoSheetBuild(NULL, density, TILE_DETAIL_CLASSIC);
    if (sheet != NULL) SDL_DestroySurface(sheet);
    return rc;
}

/* ------------------------------------------------------------------ */

/* Count the ring texels around `cell` that are not a copy of the cell's
   nearest edge texel, naming the first in `report`. */
static int ringMismatches(const SDL_Surface *s, const SDL_Rect *cell,
                          char *report, size_t reportLen) {
    int bad = 0;

    for (int y = cell->y - ISO_SHEET_GUTTER; y < cell->y + cell->h + ISO_SHEET_GUTTER; y++) {
        for (int x = cell->x - ISO_SHEET_GUTTER; x < cell->x + cell->w + ISO_SHEET_GUTTER; x++) {
            if (x >= cell->x && x < cell->x + cell->w &&
                y >= cell->y && y < cell->y + cell->h) {
                continue;
            }
            if (x < 0 || y < 0 || x >= s->w || y >= s->h) {
                /* Nothing is there to sample, and checkLayoutAt already
                   insists the ring is inside the sheet. */
                bad++;
                continue;
            }
            int ex = x < cell->x ? cell->x
                                 : (x >= cell->x + cell->w ? cell->x + cell->w - 1 : x);
            int ey = y < cell->y ? cell->y
                                 : (y >= cell->y + cell->h ? cell->y + cell->h - 1 : y);
            const unsigned char *got  = texel(s, x, y);
            const unsigned char *want = texel(s, ex, ey);
            if (memcmp(got, want, 4) == 0) continue;
            if (bad == 0 && report != NULL) {
                SDL_snprintf(report, reportLen,
                             "texel %d,%d is %d,%d,%d,%d where the cell's edge texel "
                             "%d,%d is %d,%d,%d,%d",
                             x, y, got[0], got[1], got[2], got[3],
                             ex, ey, want[0], want[1], want[2], want[3]);
            }
            bad++;
        }
    }
    return bad;
}

/* On a stock sheet at density 2 the ring around the player's north-facing
 * tank and around the building face is a copy of the cell's edge on all
 * four sides and at the corners. The face is the one that proves the copy
 * ran: its texture fills its 32x16 cell, so every edge texel is opaque and a
 * ring of zeros would show. The tank's edge is transparent and zero (the
 * bleed colours only the texels next to the sprite), so its ring passes the
 * rule by being zero too, which is what a sampler wants there. */
int run_iso_sheet_gutter(void) {
    const int density = 2;
    SDL_Surface *sheet = isoSheetBuild(NULL, density, TILE_DETAIL_CLASSIC);
    if (sheet == NULL) UT_FAIL("isoSheetBuild returned NULL");

    static const struct { IsoSpriteKind kind; IsoTeam team; int frame; bool opaqueEdge; } cells[] = {
        { ISO_SPRITE_TANK,          ISO_TEAM_SELF, 0, false },
        { ISO_SPRITE_FACE_BUILDING, ISO_TEAM_SELF, 0, true  }
    };
    int rc = 0;
    for (int i = 0; i < (int)(sizeof(cells) / sizeof(cells[0])) && rc == 0; i++) {
        SDL_Rect r;
        bool present = false;
        char why[256] = { 0 };
        const IsoSheetSlot *slot = isoSheetSlotAt(
            isoSheetSlotIndex(cells[i].kind, cells[i].team, cells[i].frame));

        if (!isoSheetRect(cells[i].kind, cells[i].team, cells[i].frame, density, &r, &present) ||
            !present) {
            fprintf(stderr, "FAIL %s:%d: %s is not on the stock sheet\n",
                    __FILE__, __LINE__, slot->name);
            rc = 1;
            break;
        }

        int bad = ringMismatches(sheet, &r, why, sizeof(why));
        if (bad != 0) {
            fprintf(stderr, "FAIL %s:%d: %d ring texels around %s are not a copy of its "
                    "edge: %s\n", __FILE__, __LINE__, bad, slot->name, why);
            rc = 1;
            break;
        }

        if (!cells[i].opaqueEdge) continue;

        /* Each side's edge row or column carries at least one texel that is
           not all zero, so the ring on that side was copied from something. */
        bool top = false, bottom = false, left = false, right = false;
        for (int x = r.x; x < r.x + r.w; x++) {
            const unsigned char *t = texel(sheet, x, r.y);
            const unsigned char *b = texel(sheet, x, r.y + r.h - 1);
            if (t[0] | t[1] | t[2] | t[3]) top = true;
            if (b[0] | b[1] | b[2] | b[3]) bottom = true;
        }
        for (int y = r.y; y < r.y + r.h; y++) {
            const unsigned char *l = texel(sheet, r.x, y);
            const unsigned char *rr = texel(sheet, r.x + r.w - 1, y);
            if (l[0] | l[1] | l[2] | l[3]) left = true;
            if (rr[0] | rr[1] | rr[2] | rr[3]) right = true;
        }
        if (!(top && bottom && left && right)) {
            fprintf(stderr, "FAIL %s:%d: %s's edge is all zero on a side (top %d bottom %d "
                    "left %d right %d), so its ring proves nothing there\n",
                    __FILE__, __LINE__, slot->name, top, bottom, left, right);
            rc = 1;
        }
    }

    SDL_DestroySurface(sheet);
    return rc;
}
