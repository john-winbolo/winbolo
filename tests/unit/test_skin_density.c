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

/* gTileMap's order is not a contract, so look names up rather than
   hardcoding positions. -1 when the name has gone. */
static int spriteIndexOf(const char *name) {
    for (int i = 0; gTileMap[i].name != NULL; i++) {
        if (strcmp(gTileMap[i].name, name) == 0) return i;
    }
    return -1;
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
    return 0;
}
