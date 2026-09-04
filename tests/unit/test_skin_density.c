/*
 * Skin density coverage scan and per-mode density choice
 * (src/gui/sdl3/tileloader.c).
 *
 * The committed directory fixture ships MaxPixelDensity=2 and three sprites
 * above density 1: tank_self_00 and grass as @2x PNGs, road_t1 as an SVG.
 * That is enough to pin every branch of the scan — a density some sprites
 * serve, a density the SVG cap shuts out, and the built-in floor at 1 — plus
 * what each Tile Detail mode makes of the result.
 *
 * The two @2x PNGs are byte copies of the density-1 art, not real 2x
 * drawings. The scan only ever asks whether a name is in the index, so their
 * pixels never matter here.
 *
 * Two more things ride on top of that fixture. tileLoaderSheetDensityFromBmp
 * is checked against hand-built headers, and a skin whose only art is one
 * 2x whole sheet - written into the working directory and deleted again - has
 * to scan as covering density 2 for every sprite, including the ones it
 * carries no file for.
 *
 * That same sheet-only skin is then built into an output sheet, which is
 * what puts a 2x source into a 1x slot and exercises the reducing arm of
 * blitSheetSprite. Its pixels are a checkerboard so that averaging the
 * block each output pixel covers and taking one pixel out of it give
 * different answers.
 *
 * The sheet key's tolerance band is pinned here too. It is the other thing a
 * whole-sheet BMP is read for, and it is a pure function of three channel
 * values, so it needs no fixture at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "skin_source.h"
#include "tileloader.h"
#include "tilemap.h"
#include "test_harness.h"

#ifndef WB_SKINS_FIXTURE_DIR
#define WB_SKINS_FIXTURE_DIR "tests/fixtures/skins"
#endif

/* Built and deleted by this test; nothing is committed under this name. */
#define SHEET_SKIN_DIR "skin_sheet_density_test"

/* gTileMap's order is not a contract, so look names up rather than
   hardcoding positions. -1 when the name has gone. */
static int spriteIndexOf(const char *name) {
    for (int i = 0; gTileMap[i].name != NULL; i++) {
        if (strcmp(gTileMap[i].name, name) == 0) return i;
    }
    return -1;
}

/* Little-endian 32-bit field, the byte order every BMP header uses. */
static void putLE32(unsigned char *p, long v) {
    unsigned long u = (unsigned long)v;
    p[0] = (unsigned char)(u & 0xFF);
    p[1] = (unsigned char)((u >> 8) & 0xFF);
    p[2] = (unsigned char)((u >> 16) & 0xFF);
    p[3] = (unsigned char)((u >> 24) & 0xFF);
}

/* The 26 header bytes the density check reads: the magic, then the width and
   height. Everything else is left zero, which is all the check looks at. */
static void makeBmpHeader(unsigned char *hdr, long w, long h) {
    memset(hdr, 0, 26);
    hdr[0] = 'B';
    hdr[1] = 'M';
    putLE32(hdr + 18, w);
    putLE32(hdr + 22, h);
}

/* A whole sheet's multiple comes out of its header alone. */
static int checkSheetDensity(void) {
    unsigned char hdr[26];
    long over = (long)(SKIN_DENSITY_MAX + 1);

    makeBmpHeader(hdr, 496, 176);
    UT_ASSERT_MSG(tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)) == 1,
                  "496x176 read as %d, it is the classic 1x sheet",
                  tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)));

    makeBmpHeader(hdr, 992, 352);
    UT_ASSERT_MSG(tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)) == 2,
                  "992x352 read as %d, it is 2x on both axes",
                  tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)));

    makeBmpHeader(hdr, 1488, 528);
    UT_ASSERT_MSG(tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)) == 3,
                  "1488x528 read as %d, it is 3x on both axes",
                  tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)));

    makeBmpHeader(hdr, 992, -352);
    UT_ASSERT_MSG(tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)) == 2,
                  "992x-352 read as %d, a top-down BMP is the same size",
                  tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)));

    makeBmpHeader(hdr, 500, 176);
    UT_ASSERT_MSG(tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)) == 0,
                  "500x176 read as %d, its width is not a whole multiple",
                  tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)));

    makeBmpHeader(hdr, 992, 176);
    UT_ASSERT_MSG(tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)) == 0,
                  "992x176 read as %d, the two axes disagree",
                  tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)));

    makeBmpHeader(hdr, 496 * over, 176 * over);
    UT_ASSERT_MSG(tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)) == 0,
                  "a sheet one step past SKIN_DENSITY_MAX read as %d",
                  tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)));

    makeBmpHeader(hdr, 992, 352);
    hdr[0] = 'X';
    UT_ASSERT_MSG(tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)) == 0,
                  "bytes that do not start BM read as %d",
                  tileLoaderSheetDensityFromBmp(hdr, sizeof(hdr)));

    makeBmpHeader(hdr, 992, 352);
    UT_ASSERT_MSG(tileLoaderSheetDensityFromBmp(hdr, 10) == 0,
                  "a 10-byte buffer read as %d, it is too short to hold the "
                  "size fields", tileLoaderSheetDensityFromBmp(hdr, 10));
    UT_ASSERT_MSG(tileLoaderSheetDensityFromBmp(NULL, sizeof(hdr)) == 0,
                  "a NULL buffer read as %d",
                  tileLoaderSheetDensityFromBmp(NULL, sizeof(hdr)));
    return 0;
}

/* Both edges of the sheet key's tolerance band. Wide enough to swallow the
   near-greens an anti-aliased sheet carries beside the pure one, narrow
   enough to leave art drawn in green alone. */
static int checkSheetKeyColor(void) {
    UT_ASSERT_MSG(tileLoaderIsSheetKeyColor(0, 255, 0),
                  "pure green is not read as the key colour");
    UT_ASSERT_MSG(tileLoaderIsSheetKeyColor(0, 254, 0),
                  "(0,254,0) is not read as the key colour; it is one step of "
                  "encoding noise off pure green");
    UT_ASSERT_MSG(tileLoaderIsSheetKeyColor(1, 255, 1),
                  "(1,255,1) is not read as the key colour");
    UT_ASSERT_MSG(tileLoaderIsSheetKeyColor(8, 247, 8),
                  "(8,247,8) is not read as the key colour; it is the far "
                  "corner of the band and still inside it");

    UT_ASSERT_MSG(!tileLoaderIsSheetKeyColor(9, 247, 0),
                  "(9,247,0) is read as the key colour; red is one past the "
                  "band");
    UT_ASSERT_MSG(!tileLoaderIsSheetKeyColor(0, 246, 0),
                  "(0,246,0) is read as the key colour; green is one past the "
                  "band");
    UT_ASSERT_MSG(!tileLoaderIsSheetKeyColor(0, 255, 9),
                  "(0,255,9) is read as the key colour; blue is one past the "
                  "band");
    UT_ASSERT_MSG(!tileLoaderIsSheetKeyColor(28, 255, 28),
                  "(28,255,28) is read as the key colour; that is art-class "
                  "green and would be erased from every sheet");
    UT_ASSERT_MSG(!tileLoaderIsSheetKeyColor(0, 128, 0),
                  "mid green is read as the key colour");
    UT_ASSERT_MSG(!tileLoaderIsSheetKeyColor(255, 255, 255),
                  "white is read as the key colour");
    return 0;
}

/* Write a 992x352 1-bit-per-pixel BMP - two palette entries, 124 bytes a row,
   about 44KB. The scan reads the header and stops, but the pixels are a
   black and white checkerboard rather than a flat fill: that is what the
   reduction check below reads back, and a flat sheet would pass a 2:1
   reduction whether it averaged or point-sampled. Returns 0 on any write
   failure. */
static int writeSheetBmp(const char *path) {
    const long w = 992;
    const long h = 352;
    const int rowBytes = (int)((w + 7) / 8);      /* 124, already 4-aligned */
    const long pixelBytes = (long)rowBytes * h;
    const long offBits = 14 + 40 + 8;             /* headers + 2-colour table */
    unsigned char hdr[62];
    unsigned char row[124];
    FILE *f;
    long y;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    putLE32(hdr + 2, offBits + pixelBytes);       /* file size */
    putLE32(hdr + 10, offBits);                   /* where the pixels start */
    putLE32(hdr + 14, 40);                        /* BITMAPINFOHEADER */
    putLE32(hdr + 18, w);
    putLE32(hdr + 22, h);
    hdr[26] = 1;                                  /* planes */
    hdr[28] = 1;                                  /* bits per pixel */
    putLE32(hdr + 34, pixelBytes);
    putLE32(hdr + 46, 2);                         /* colours used */
    putLE32(hdr + 50, 2);                         /* colours that matter */
    hdr[58] = 0xFF;                               /* colour 1: white, BGRA */
    hdr[59] = 0xFF;
    hdr[60] = 0xFF;

    if (rowBytes > (int)sizeof(row)) return 0;

    f = fopen(path, "wb");
    if (f == NULL) return 0;
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return 0;
    }
    /* 0xAA is 10101010 and 0x55 its opposite, MSB first being the leftmost
       pixel, so alternating them row by row lays down a checkerboard one
       pixel on a side. 992 is a whole number of bytes, so no row runs a
       partial byte out of phase. */
    for (y = 0; y < h; y++) {
        memset(row, (y & 1) ? 0x55 : 0xAA, sizeof(row));
        if (fwrite(row, 1, (size_t)rowBytes, f) != (size_t)rowBytes) {
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return 1;
}

static int writeSheetIni(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return 0;
    fputs("[Skin]\nName=Sheet Only Test Skin\nAuthor=WinBolo Tests\n", f);
    fclose(f);
    return 1;
}

/* What a skin whose only art is one 2x whole sheet has to scan as. */
static int checkSheetOnlyScan(SkinSource *src, int iBare) {
    SkinDensityInfo info;

    tileLoaderScanDensity(src, &info);

    UT_ASSERT_MSG(info.coverage[1] == SKIN_DENSITY_COVER_ALL,
                  "density 1 coverage is %d for a sheet-only skin",
                  (int)info.coverage[1]);
    UT_ASSERT_MSG(info.coverage[2] == SKIN_DENSITY_COVER_ALL,
                  "density 2 coverage is %d; a 2x whole sheet carries every "
                  "sprite at 2", (int)info.coverage[2]);
    UT_ASSERT_MSG(info.coverage[3] == SKIN_DENSITY_COVER_NONE,
                  "density 3 coverage is %d; the sheet is only 2x and there "
                  "are no @3x files", (int)info.coverage[3]);

    UT_ASSERT_MSG(info.highestAll == 2,
                  "highestAll is %d; the sheet covers 2 in full",
                  info.highestAll);
    UT_ASSERT_MSG(info.highestAny == 2,
                  "highestAny is %d; nothing goes above the sheet",
                  info.highestAny);
    UT_ASSERT_MSG(info.spriteMax[iBare] == 2,
                  "deep_sea spriteMax is %d; the skin has no file of its own "
                  "for it, but the sheet holds it at 2",
                  (int)info.spriteMax[iBare]);

    UT_ASSERT_MSG(tileLoaderPickDensity(&info, iBare,
                                        TILE_DETAIL_MATCH_ZOOM, 2) == 2,
                  "Match to Zoom picked %d at scale 2, the sheet serves 2",
                  tileLoaderPickDensity(&info, iBare,
                                        TILE_DETAIL_MATCH_ZOOM, 2));

    /* The cached scan has to be this skin's, not the fixture's.  The
       fixture was scanned through the cache and then closed, so this source
       may well sit at the same address; the fixture's highestAll is 1 and
       this skin's is 2, which tells the two apart. */
    UT_ASSERT_MSG(tileLoaderGetDensityInfo(src)->highestAll == 2,
                  "cached scan reports highestAll %d for the sheet-only skin; "
                  "it is the previous source's scan",
                  tileLoaderGetDensityInfo(src)->highestAll);
    return 0;
}

/* Cutting a 2x sheet into a 1x slot is a reduction, and a checkerboard is
   what tells the two ways of doing it apart. Averaging each output pixel
   over the 2x2 block it covers gives a mid-tone everywhere, because every
   2x2 window of a checkerboard holds two black pixels and two white.
   Point sampling takes one pixel of the four and comes out solid black or
   solid white. */
static int checkSheetReduction(SkinSource *skin, int iBare) {
    const TileMapEntry *e = &gTileMap[iBare];
    SDL_Surface *sheet;
    int lo = 255, hi = 0, alphaLo = 255;
    int x, y;

    /* Classic keeps every sprite at density 1, and a tile size of
       TILE_SIZE_X builds the sheet at scale 1, so the whole 2x skin sheet
       is reduced into it. */
    sheet = tileLoaderBuildSheetFor(skin, TILE_SIZE_X, TILE_DETAIL_CLASSIC);
    if (sheet == NULL) {
        UT_FAIL("tileLoaderBuildSheetFor returned NULL for the sheet-only skin");
    }
    if (sheet->format != SDL_PIXELFORMAT_RGBA32) {
        SDL_DestroySurface(sheet);
        UT_FAIL("the built sheet is not RGBA32, so its bytes cannot be read here");
    }

    for (y = 0; y < e->height; y++) {
        const unsigned char *row = (const unsigned char *)sheet->pixels +
                                   (size_t)(e->sheetY + y) * (size_t)sheet->pitch;
        for (x = 0; x < e->width; x++) {
            const unsigned char *p = row + (size_t)(e->sheetX + x) * 4;
            if (p[0] < lo) lo = p[0];
            if (p[0] > hi) hi = p[0];
            if (p[3] < alphaLo) alphaLo = p[3];
        }
    }
    SDL_DestroySurface(sheet);

    UT_ASSERT_MSG(lo >= 64 && hi <= 191,
                  "deep_sea's slot spans red %d..%d after the 2x sheet was "
                  "reduced into it; a checkerboard averages to a mid-tone, "
                  "and 0 or 255 means one parity was taken and the other "
                  "thrown away", lo, hi);
    UT_ASSERT_MSG(alphaLo == 255,
                  "the reduced slot's lowest alpha is %d; a black and white "
                  "sheet holds no key colour, so every pixel stays opaque",
                  alphaLo);
    return 0;
}

/* Build that skin in the working directory, scan it, and take it away again
   whether the scan passed or not. */
static int checkSheetOnlySkin(int iBare) {
    const char *dir = SHEET_SKIN_DIR;
    char iniPath[512];
    char bmpPath[512];
    SkinSource *src;
    int rc;

    snprintf(iniPath, sizeof(iniPath), "%s/skin.ini", dir);
    snprintf(bmpPath, sizeof(bmpPath), "%s/tiles.bmp", dir);

    if (!SDL_CreateDirectory(dir)) {
        UT_FAIL("could not create %s: %s", dir, SDL_GetError());
    }
    if (!writeSheetIni(iniPath) || !writeSheetBmp(bmpPath)) {
        remove(bmpPath);
        remove(iniPath);
        SDL_RemovePath(dir);
        UT_FAIL("could not write the sheet-only skin into %s", dir);
    }

    src = skinSourceOpen(dir);
    if (src == NULL) {
        remove(bmpPath);
        remove(iniPath);
        SDL_RemovePath(dir);
        UT_FAIL("skinSourceOpen('%s') failed", dir);
    }

    rc = checkSheetOnlyScan(src, iBare);
    if (rc == 0) rc = checkSheetReduction(src, iBare);

    skinSourceClose(src);
    remove(bmpPath);
    remove(iniPath);
    SDL_RemovePath(dir);
    return rc;
}

/* What the fixture's three above-density-1 sprites and its MaxPixelDensity=2
   have to add up to. */
static int checkScan(const SkinDensityInfo *info,
                     int iTank, int iGrass, int iRoad, int iBare) {
    UT_ASSERT_MSG(info->spriteCount == (int)TILE_MAP_COUNT,
                  "spriteCount is %d, tilemap holds %d",
                  info->spriteCount, (int)TILE_MAP_COUNT);

    UT_ASSERT_MSG(info->coverage[1] == SKIN_DENSITY_COVER_ALL,
                  "density 1 coverage is %d, the built-in fallback makes it "
                  "ALL for every skin", (int)info->coverage[1]);
    UT_ASSERT_MSG(info->coverage[2] == SKIN_DENSITY_COVER_SOME,
                  "density 2 coverage is %d; three sprites carry it and the "
                  "rest do not", (int)info->coverage[2]);
    UT_ASSERT_MSG(info->coverage[3] == SKIN_DENSITY_COVER_NONE,
                  "density 3 coverage is %d; there are no @3x files and the "
                  "SVG stops counting above MaxPixelDensity=2",
                  (int)info->coverage[3]);

    UT_ASSERT_MSG(info->highestAll == 1,
                  "highestAll is %d; no density above 1 is fully covered",
                  info->highestAll);
    UT_ASSERT_MSG(info->highestAny == 2,
                  "highestAny is %d; the best any sprite reaches is 2",
                  info->highestAny);

    UT_ASSERT_MSG(info->spriteMax[iTank] == 2,
                  "tank_self_00 spriteMax is %d, it has an @2x PNG",
                  (int)info->spriteMax[iTank]);
    UT_ASSERT_MSG(info->spriteMax[iGrass] == 2,
                  "grass spriteMax is %d, it has an @2x PNG",
                  (int)info->spriteMax[iGrass]);
    UT_ASSERT_MSG(info->spriteMax[iRoad] == 2,
                  "road_t1 spriteMax is %d, its SVG counts up to "
                  "MaxPixelDensity=2", (int)info->spriteMax[iRoad]);
    UT_ASSERT_MSG(info->spriteMax[iBare] == 1,
                  "deep_sea spriteMax is %d, the fixture does not carry it",
                  (int)info->spriteMax[iBare]);
    return 0;
}

/* Each Tile Detail mode against that same scan, then the malformed inputs. */
static int checkPick(const SkinDensityInfo *info, int iTank, int iBare) {
    UT_ASSERT_MSG(tileLoaderPickDensity(info, iTank, TILE_DETAIL_CLASSIC, 1) == 1,
                  "Classic gave a covered sprite something other than 1 at scale 1");
    UT_ASSERT_MSG(tileLoaderPickDensity(info, iTank, TILE_DETAIL_CLASSIC, 4) == 1,
                  "Classic gave a covered sprite something other than 1 at scale 4");
    UT_ASSERT_MSG(tileLoaderPickDensity(info, iBare, TILE_DETAIL_CLASSIC, 1) == 1,
                  "Classic gave an uncovered sprite something other than 1 at scale 1");
    UT_ASSERT_MSG(tileLoaderPickDensity(info, iBare, TILE_DETAIL_CLASSIC, 4) == 1,
                  "Classic gave an uncovered sprite something other than 1 at scale 4");

    UT_ASSERT_MSG(tileLoaderPickDensity(info, iTank, TILE_DETAIL_MATCH_ZOOM, 2) == 1,
                  "Match to Zoom went above highestAll (1) at scale 2");
    UT_ASSERT_MSG(tileLoaderPickDensity(info, iTank, TILE_DETAIL_MATCH_ZOOM, 1) == 1,
                  "Match to Zoom is not 1 at scale 1");

    UT_ASSERT_MSG(tileLoaderPickDensity(info, iTank, TILE_DETAIL_HIGH, 1) == 2,
                  "High Detail did not take tank_self_00's @2x");
    UT_ASSERT_MSG(tileLoaderPickDensity(info, iBare, TILE_DETAIL_HIGH, 1) == 1,
                  "High Detail found something above 1 for an uncovered sprite");

    UT_ASSERT_MSG(tileLoaderPickDensity(NULL, iTank, TILE_DETAIL_HIGH, 1) == 1,
                  "a NULL info did not fall back to 1");
    UT_ASSERT_MSG(tileLoaderPickDensity(info, -1, TILE_DETAIL_HIGH, 1) == 1,
                  "a negative sprite index did not fall back to 1");
    UT_ASSERT_MSG(tileLoaderPickDensity(info, SKIN_DENSITY_MAX_SPRITES,
                                        TILE_DETAIL_HIGH, 1) == 1,
                  "an off-the-end sprite index did not fall back to 1");
    UT_ASSERT_MSG(tileLoaderPickDensity(info, iTank, TILE_DETAIL_MATCH_ZOOM, 0) == 1,
                  "scale 0 did not fall back to 1");
    return 0;
}

int run_skin_density_scan(void) {
    const char *dir = getenv("WB_SKINS_FIXTURE_DIR");
    char fixture[512];
    SkinSource *src;
    SkinDensityInfo info;
    int iTank, iGrass, iRoad, iBare;
    int rc;

    iTank  = spriteIndexOf("tank_self_00");
    iGrass = spriteIndexOf("grass");
    iRoad  = spriteIndexOf("road_t1");
    iBare  = spriteIndexOf("deep_sea");
    UT_ASSERT_MSG(iTank >= 0 && iGrass >= 0 && iRoad >= 0 && iBare >= 0,
                  "gTileMap no longer holds a name this test looks up "
                  "(tank_self_00 %d, grass %d, road_t1 %d, deep_sea %d)",
                  iTank, iGrass, iRoad, iBare);

    if (dir == NULL || dir[0] == '\0') dir = WB_SKINS_FIXTURE_DIR;
    snprintf(fixture, sizeof(fixture), "%s/basic", dir);

    src = skinSourceOpen(fixture);
    if (src == NULL) {
        UT_FAIL("fixture missing or unreadable: %s", fixture);
    }

    tileLoaderScanDensity(src, &info);
    rc = checkScan(&info, iTank, iGrass, iRoad, iBare);
    if (rc == 0) rc = checkPick(&info, iTank, iBare);
    /* Prime the one-entry cache with this source before it goes away, so
       the sheet-only skin opened later can show it is not served the
       fixture's scan. */
    if (rc == 0 && tileLoaderGetDensityInfo(src)->highestAll != 1) {
        rc = 1;
        UT_FAIL("cached scan of the fixture reports highestAll %d, not 1",
                tileLoaderGetDensityInfo(src)->highestAll);
    }
    skinSourceClose(src);
    if (rc != 0) return rc;

    /* No skin at all still serves density 1 in full, and nothing above. */
    tileLoaderScanDensity(NULL, &info);
    UT_ASSERT_MSG(info.coverage[1] == SKIN_DENSITY_COVER_ALL,
                  "a NULL skin reported density 1 coverage %d",
                  (int)info.coverage[1]);
    UT_ASSERT_MSG(info.highestAll == 1,
                  "a NULL skin reported highestAll %d", info.highestAll);
    UT_ASSERT_MSG(info.highestAny == 1,
                  "a NULL skin reported highestAny %d", info.highestAny);

    rc = checkSheetDensity();
    if (rc != 0) return rc;
    rc = checkSheetKeyColor();
    if (rc != 0) return rc;
    return checkSheetOnlySkin(iBare);
}
