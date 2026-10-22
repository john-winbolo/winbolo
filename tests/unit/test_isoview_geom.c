/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* The isometric view's geometry (isoview_geom.c), built from a hand-filled
 * map memory and hand-built lists with no renderer behind it.
 *
 * tall_kinds: one tile of each standing kind classifies to its kind and
 * height, and a grass tile is flat.
 *
 * faces: a 2x2 block of buildings shows only its outer faces, the south-west
 * ones on the south row and the south-east ones on the east column; a lone
 * building shows both.
 *
 * item_order: a tank on the square behind a building draws before it and one
 * on the square in front draws after; a builder on a pill's own square draws
 * after the pill and before the next row's tile.
 *
 * unseen: a square the player has never seen produces one flat quad in the
 * fog colour and no textured one.
 *
 * hidden: a square inside a live region but hidden by line of sight gets the
 * fog tint on its ground quad, and a live square does not.
 *
 * tank_sprite: with the isometric sheet holding the tank it is drawn from
 * that sheet in world facing; without it the classic sprite stands in, turned
 * two frames with the camera.
 *
 * visible_range: the range the ground pass walks is isoVisibleRange with both
 * maxima pushed out for the tallest roof, and clamped to the map.
 *
 * mine: a mined square produces two quads, the ground and the mine over it.
 */

#include <math.h>
#include <string.h>

#include "test_harness.h"
#include "isoview_geom.h"
#include "iso_project.h"
#include "tiles.h"
#include "tilenum.h"
#include "gfx_settings.h"
#include "fog_look.h"

static OverviewMap sMap;

/* A memory of grass, every square live, with nothing on it. */
static void clearMap(void) {
    int x, y;
    memset(&sMap, 0, sizeof(sMap));
    for (x = 0; x < MAP_ARRAY_SIZE; x++) {
        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
            sMap.tile[x][y]  = GRASS;
            sMap.flags[x][y] = OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT;
        }
    }
}

/* A camera over square (cx, cy) at scale 2 on a 640x480 target. */
static void baseInput(IsoGeomInput *in, int cx, int cy) {
    memset(in, 0, sizeof(*in));
    in->cam.centerWX = ((float)cx + 0.5f) * ISO_WORLD_PER_SQUARE;
    in->cam.centerWY = ((float)cy + 0.5f) * ISO_WORLD_PER_SQUARE;
    in->cam.scale    = 2.0f;
    in->targetW      = 640;
    in->targetH      = 480;
    in->map          = &sMap;
    in->animMode     = GFX_ANIM_CLASSIC;
    in->sheetScale   = 2;
    in->fogWashes    = true;
    in->fogR = in->fogG = in->fogB = 0;
}

/* A sheet that holds every sprite, each at a rect that names its kind, team
   and frame so a test can tell which was asked for. */
static bool sheetAll(void *user, IsoSpriteKind kind, IsoTeam team, int frame,
                     IsoGeomRect *out) {
    (void)user;
    out->x = (float)(kind * 100);
    out->y = (float)(team * 100 + frame);
    out->w = 32.0f;
    out->h = (kind >= ISO_SPRITE_FACE_BUILDING) ? 16.0f : 32.0f;
    return true;
}

/* A sheet with the faces and the band and nothing standing: what a stock
   build with no iso_ files gives. */
static bool sheetFacesOnly(void *user, IsoSpriteKind kind, IsoTeam team,
                           int frame, IsoGeomRect *out) {
    if (kind < ISO_SPRITE_FACE_BUILDING) return false;
    return sheetAll(user, kind, team, frame, out);
}

static int groundQuadsAt(const IsoGeomScene *s, int mx, int my,
                         const IsoGeomQuad **first) {
    int i, n = 0;
    *first = NULL;
    for (i = 0; i < s->groundCount; i++) {
        if (s->ground[i].mapX == mx && s->ground[i].mapY == my) {
            if (*first == NULL) *first = &s->ground[i];
            n++;
        }
    }
    return n;
}

static const IsoGeomItem *tallItemAt(const IsoGeomScene *s, int mx, int my) {
    int i;
    for (i = 0; i < s->itemCount; i++) {
        if (s->items[i].kind == ISO_GEOM_ITEM_TALL &&
            s->items[i].mapX == mx && s->items[i].mapY == my) {
            return &s->items[i];
        }
    }
    return NULL;
}

static int itemPosition(const IsoGeomScene *s, int kind, int mx, int my) {
    int i;
    for (i = 0; i < s->itemCount; i++) {
        if (s->items[i].kind == kind && s->items[i].mapX == mx &&
            s->items[i].mapY == my) {
            return i;
        }
    }
    return -1;
}

int run_isoview_geom_tall_kinds(void) {
    UT_ASSERT(isoGeomTallKind(BUILD_SINGLE) == ISO_TALL_BUILDING);
    UT_ASSERT(isoGeomTallKind(BUILD_MOST4) == ISO_TALL_BUILDING);
    UT_ASSERT(isoGeomTallKind(HALFBUILDING) == ISO_TALL_SHOT_BUILDING);
    UT_ASSERT(isoGeomTallKind(PILL_EVIL_15) == ISO_TALL_PILL);
    UT_ASSERT(isoGeomTallKind(PILL_EVIL_0) == ISO_TALL_PILL);
    UT_ASSERT(isoGeomTallKind(PILL_GOOD_7) == ISO_TALL_PILL);
    UT_ASSERT(isoGeomTallKind(BASE_GOOD) == ISO_TALL_BASE);
    UT_ASSERT(isoGeomTallKind(BASE_NEUTRAL) == ISO_TALL_BASE);
    UT_ASSERT(isoGeomTallKind(BASE_EVIL) == ISO_TALL_BASE);
    UT_ASSERT(isoGeomTallKind(FOREST_SINGLE) == ISO_TALL_FOREST);
    UT_ASSERT(isoGeomTallKind(FOREST_RIGHT) == ISO_TALL_FOREST);
    UT_ASSERT(isoGeomTallKind(GRASS) == ISO_TALL_NONE);
    UT_ASSERT(isoGeomTallKind(ROAD_SOLID) == ISO_TALL_NONE);
    UT_ASSERT(isoGeomTallKind(RIVER_SOLID) == ISO_TALL_NONE);
    UT_ASSERT(isoGeomTallKind(OVERVIEW_UNSEEN) == ISO_TALL_NONE);

    UT_ASSERT(isoGeomTallHeight(ISO_TALL_BUILDING) == 10);
    UT_ASSERT(isoGeomTallHeight(ISO_TALL_SHOT_BUILDING) == 5);
    UT_ASSERT(isoGeomTallHeight(ISO_TALL_PILL) == 6);
    UT_ASSERT(isoGeomTallHeight(ISO_TALL_BASE) == 3);
    UT_ASSERT(isoGeomTallHeight(ISO_TALL_FOREST) == 5);
    UT_ASSERT(isoGeomTallHeight(ISO_TALL_NONE) == 0);

    /* Through a build: the roof of each kind is lifted by its height. */
    {
        IsoGeomScene scene;
        IsoGeomInput in;
        const IsoGeomItem *it;
        const IsoGeomQuad *roof, *ground;
        float scale;
        clearMap();
        sMap.tile[10][10] = BUILD_SINGLE;
        sMap.tile[12][10] = HALFBUILDING;
        sMap.tile[14][10] = FOREST_SINGLE;
        baseInput(&in, 12, 10);
        in.sheet = sheetFacesOnly;
        scale = in.cam.scale;
        isoGeomSceneInit(&scene);
        UT_ASSERT(isoGeomBuild(&scene, &in));

        it = tallItemAt(&scene, 10, 10);
        UT_ASSERT(it != NULL && it->tallKind == ISO_TALL_BUILDING);
        UT_ASSERT(!it->isoSprite);
        roof = &scene.stand[it->quadFirst + it->quadCount - 1];
        UT_ASSERT(groundQuadsAt(&scene, 10, 10, &ground) == 1);
        UT_ASSERT(roof->tex == ISO_GEOM_TEX_CLASSIC);
        UT_ASSERT(fabsf((ground->py[0] - roof->py[0]) - 10.0f * scale) < 1e-3f);
        UT_ASSERT(fabsf(roof->px[0] - ground->px[0]) < 1e-3f);

        it = tallItemAt(&scene, 12, 10);
        UT_ASSERT(it != NULL && it->tallKind == ISO_TALL_SHOT_BUILDING);
        roof = &scene.stand[it->quadFirst + it->quadCount - 1];
        UT_ASSERT(groundQuadsAt(&scene, 12, 10, &ground) == 1);
        UT_ASSERT(fabsf((ground->py[0] - roof->py[0]) - 5.0f * scale) < 1e-3f);

        it = tallItemAt(&scene, 14, 10);
        UT_ASSERT(it != NULL && it->tallKind == ISO_TALL_FOREST);
        /* A lone forest: band on both sides, from the band cell. */
        UT_ASSERT(it->faces == (ISO_FACE_SW | ISO_FACE_SE));
        UT_ASSERT(it->quadCount == 4);  /* shadow, two bands, roof */
        UT_ASSERT(scene.stand[it->quadFirst + 1].tex == ISO_GEOM_TEX_ISO);
        UT_ASSERT(fabsf(scene.stand[it->quadFirst + 1].src.x -
                        (float)(ISO_SPRITE_FOREST_BAND * 100)) < 1e-3f);

        /* Grass stands nothing up. */
        UT_ASSERT(tallItemAt(&scene, 11, 10) == NULL);
        isoGeomSceneFree(&scene);
    }
    return 0;
}

int run_isoview_geom_faces(void) {
    IsoGeomScene scene;
    IsoGeomInput in;
    const IsoGeomItem *it;

    clearMap();
    /* A 2x2 block at (20..21, 20..21) and a lone building at (30, 20). */
    sMap.tile[20][20] = BUILD_CORNER1;
    sMap.tile[21][20] = BUILD_CORNER2;
    sMap.tile[20][21] = BUILD_CORNER3;
    sMap.tile[21][21] = BUILD_CORNER4;
    sMap.tile[30][20] = BUILD_SINGLE;

    UT_ASSERT(isoGeomExposedFaces(&sMap, 20, 20) == 0);
    UT_ASSERT(isoGeomExposedFaces(&sMap, 21, 20) == ISO_FACE_SE);
    UT_ASSERT(isoGeomExposedFaces(&sMap, 20, 21) == ISO_FACE_SW);
    UT_ASSERT(isoGeomExposedFaces(&sMap, 21, 21) == (ISO_FACE_SW | ISO_FACE_SE));
    UT_ASSERT(isoGeomExposedFaces(&sMap, 30, 20) == (ISO_FACE_SW | ISO_FACE_SE));
    UT_ASSERT(isoGeomExposedFaces(&sMap, 25, 25) == 0);
    /* A building against a shot building shows the face between them. */
    sMap.tile[31][20] = HALFBUILDING;
    UT_ASSERT(isoGeomExposedFaces(&sMap, 30, 20) == (ISO_FACE_SW | ISO_FACE_SE));
    /* The map edge counts as open. */
    sMap.tile[255][255] = BUILD_SINGLE;
    UT_ASSERT(isoGeomExposedFaces(&sMap, 255, 255) == (ISO_FACE_SW | ISO_FACE_SE));

    baseInput(&in, 25, 20);
    in.sheet = sheetFacesOnly;
    isoGeomSceneInit(&scene);
    UT_ASSERT(isoGeomBuild(&scene, &in));

    it = tallItemAt(&scene, 20, 20);
    UT_ASSERT(it != NULL && it->faces == 0 && it->quadCount == 2); /* shadow, roof */
    it = tallItemAt(&scene, 21, 20);
    UT_ASSERT(it != NULL && it->faces == ISO_FACE_SE && it->quadCount == 3);
    it = tallItemAt(&scene, 20, 21);
    UT_ASSERT(it != NULL && it->faces == ISO_FACE_SW && it->quadCount == 3);
    it = tallItemAt(&scene, 21, 21);
    UT_ASSERT(it != NULL && it->faces == (ISO_FACE_SW | ISO_FACE_SE) &&
              it->quadCount == 4);
    it = tallItemAt(&scene, 30, 20);
    UT_ASSERT(it != NULL && it->faces == (ISO_FACE_SW | ISO_FACE_SE) &&
              it->quadCount == 4);
    /* The two faces: the south-west one brighter, both from the building's
       face cell, each standing on the ground diamond's edge. */
    {
        const IsoGeomQuad *sw = &scene.stand[it->quadFirst + 1];
        const IsoGeomQuad *se = &scene.stand[it->quadFirst + 2];
        const IsoGeomQuad *ground;
        UT_ASSERT(groundQuadsAt(&scene, 30, 20, &ground) == 1);
        UT_ASSERT(sw->tex == ISO_GEOM_TEX_ISO && se->tex == ISO_GEOM_TEX_ISO);
        UT_ASSERT(fabsf(sw->src.x - (float)(ISO_SPRITE_FACE_BUILDING * 100)) < 1e-3f);
        UT_ASSERT(sw->r == ISO_FACE_SHADE_SW && se->r == ISO_FACE_SHADE_SE);
        /* South-west: bottom-left corner on the diamond's left vertex,
           bottom-right on its bottom vertex. */
        UT_ASSERT(fabsf(sw->px[3] - ground->px[3]) < 1e-3f &&
                  fabsf(sw->py[3] - ground->py[3]) < 1e-3f);
        UT_ASSERT(fabsf(sw->px[2] - ground->px[2]) < 1e-3f &&
                  fabsf(sw->py[2] - ground->py[2]) < 1e-3f);
        /* South-east: bottom vertex to right vertex. */
        UT_ASSERT(fabsf(se->px[3] - ground->px[2]) < 1e-3f);
        UT_ASSERT(fabsf(se->px[2] - ground->px[1]) < 1e-3f);
        /* Lifted by the building's height at the top. */
        UT_ASSERT(fabsf((ground->py[3] - sw->py[0]) - 10.0f * in.cam.scale) < 1e-3f);
    }
    isoGeomSceneFree(&scene);
    return 0;
}

int run_isoview_geom_item_order(void) {
    IsoGeomScene scene;
    IsoGeomInput in;
    screenTanks  tanks;
    screenLgm    lgms;
    int building, pill, nextRow, behind, inFront, man;

    clearMap();
    sMap.tile[40][40] = BUILD_SINGLE;
    sMap.tile[50][40] = PILL_GOOD_15;
    sMap.tile[51][40] = BUILD_SINGLE;   /* the next row along x + y */

    /* A tank on (39, 40), behind the building, and one on (41, 40) in front
       of it. The list holds the sprite's top-left, so a tank whose centre is
       on a square is listed with no world offset on that square. */
    screenTanksCreate(&tanks);
    screenTanksAddItem(&tanks, 39, 40, 0, 0, TANK_SELF_0, 0, "a", 0, 0, 0);
    screenTanksAddItem(&tanks, 41, 40, 0, 0, TANK_GOOD_4, 1, "b", 0, 0, 64);
    /* A builder on the pill's own square. */
    screenLgmCreate(&lgms);
    screenLgmAddItem(&lgms, 50, 40, 8, 8, LGM0, 128, 128);

    baseInput(&in, 45, 40);
    in.sheet   = sheetFacesOnly;
    in.tanks   = &tanks;
    in.lgms    = &lgms;
    isoGeomSceneInit(&scene);
    UT_ASSERT(isoGeomBuild(&scene, &in));

    building = itemPosition(&scene, ISO_GEOM_ITEM_TALL, 40, 40);
    behind   = itemPosition(&scene, ISO_GEOM_ITEM_TANK, 39, 40);
    inFront  = itemPosition(&scene, ISO_GEOM_ITEM_TANK, 41, 40);
    pill     = itemPosition(&scene, ISO_GEOM_ITEM_TALL, 50, 40);
    nextRow  = itemPosition(&scene, ISO_GEOM_ITEM_TALL, 51, 40);
    man      = itemPosition(&scene, ISO_GEOM_ITEM_LGM, 50, 40);
    UT_ASSERT_MSG(building >= 0 && behind >= 0 && inFront >= 0,
                  "items %d %d %d", building, behind, inFront);
    UT_ASSERT_MSG(pill >= 0 && nextRow >= 0 && man >= 0,
                  "items %d %d %d", pill, nextRow, man);
    UT_ASSERT_MSG(behind < building && building < inFront,
                  "tank behind %d, building %d, tank in front %d",
                  behind, building, inFront);
    UT_ASSERT_MSG(pill < man && man < nextRow,
                  "pill %d, builder %d, next row %d", pill, man, nextRow);

    /* The sorted items are non-decreasing in depth, and every depth is past
       the ground's. */
    {
        int i;
        for (i = 1; i < scene.itemCount; i++) {
            UT_ASSERT(scene.items[i - 1].depth <= scene.items[i].depth);
        }
        UT_ASSERT(scene.items[0].depth > isoDepth(255, 255, ISO_DEPTH_GROUND));
    }

    screenTanksDestroy(&tanks);
    screenLgmDestroy(&lgms);
    isoGeomSceneFree(&scene);
    return 0;
}

int run_isoview_geom_unseen(void) {
    IsoGeomScene scene;
    IsoGeomInput in;
    const IsoGeomQuad *q;

    clearMap();
    sMap.tile[60][60]  = OVERVIEW_UNSEEN;
    sMap.flags[60][60] = 0;
    baseInput(&in, 60, 60);
    in.fogR = FOG_LOOK_R;
    in.fogG = FOG_LOOK_G;
    in.fogB = FOG_LOOK_B;
    isoGeomSceneInit(&scene);
    UT_ASSERT(isoGeomBuild(&scene, &in));

    UT_ASSERT(groundQuadsAt(&scene, 60, 60, &q) == 1);
    UT_ASSERT(q->tex == ISO_GEOM_TEX_NONE);
    UT_ASSERT(q->r == FOG_LOOK_R && q->g == FOG_LOOK_G && q->b == FOG_LOOK_B);
    UT_ASSERT(q->a == 255);
    /* Its diamond is where the square's ground is: the top vertex at the
       target's middle less half a diamond's height, since the camera is on
       the square's centre. */
    UT_ASSERT(fabsf(q->px[0] - 320.0f) < 1e-3f);
    UT_ASSERT(fabsf(q->py[0] - (240.0f - 8.0f * in.cam.scale)) < 1e-3f);
    UT_ASSERT(tallItemAt(&scene, 60, 60) == NULL);

    /* Under the None look nothing is drawn there at all. */
    in.fogWashes = false;
    UT_ASSERT(isoGeomBuild(&scene, &in));
    UT_ASSERT(groundQuadsAt(&scene, 60, 60, &q) == 0);
    UT_ASSERT(groundQuadsAt(&scene, 61, 60, &q) == 1 && q->tex == ISO_GEOM_TEX_CLASSIC);

    isoGeomSceneFree(&scene);
    return 0;
}

int run_isoview_geom_hidden(void) {
    IsoGeomScene scene;
    IsoGeomInput in;
    const IsoGeomQuad *q;
    const IsoGeomItem *it;
    int tint = (255 * (255 - FOG_LOOK_ALPHA)) / 255;

    clearMap();
    sMap.flags[70][70] |= OVERVIEW_F_HIDDEN;       /* live but behind a wall */
    sMap.flags[72][70]  = 0;                        /* remembered, not live */
    sMap.tile[72][70]   = BUILD_SINGLE;
    baseInput(&in, 71, 70);
    in.sheet = sheetFacesOnly;
    isoGeomSceneInit(&scene);
    UT_ASSERT(isoGeomBuild(&scene, &in));

    UT_ASSERT(groundQuadsAt(&scene, 70, 70, &q) == 1);
    UT_ASSERT(q->tex == ISO_GEOM_TEX_CLASSIC);
    UT_ASSERT_MSG(q->r == tint && q->g == tint && q->b == tint,
                  "hidden square tinted (%d,%d,%d), want %d", q->r, q->g, q->b, tint);
    UT_ASSERT(fabsf(q->src.x - (float)GRASS_X) < 1e-3f &&
              fabsf(q->src.y - (float)GRASS_Y) < 1e-3f);

    /* The live square beside it is drawn as it is. */
    UT_ASSERT(groundQuadsAt(&scene, 71, 70, &q) == 1);
    UT_ASSERT(q->r == 255 && q->g == 255 && q->b == 255);

    /* A remembered square's tall tile is tinted with its ground. */
    UT_ASSERT(groundQuadsAt(&scene, 72, 70, &q) == 1 && q->r == tint);
    it = tallItemAt(&scene, 72, 70);
    UT_ASSERT(it != NULL);
    q = &scene.stand[it->quadFirst + it->quadCount - 1];
    UT_ASSERT(q->tex == ISO_GEOM_TEX_CLASSIC && q->r == tint);

    /* The grey look darkens towards grey. */
    in.fogR = in.fogG = in.fogB = FOG_LOOK_R;
    UT_ASSERT(isoGeomBuild(&scene, &in));
    UT_ASSERT(groundQuadsAt(&scene, 70, 70, &q) == 1);
    tint = (255 * (255 - FOG_LOOK_ALPHA) + FOG_LOOK_R * FOG_LOOK_ALPHA) / 255;
    UT_ASSERT(q->r == tint && q->r > 110 && q->r < 255);

    isoGeomSceneFree(&scene);
    return 0;
}

int run_isoview_geom_tank_sprite(void) {
    IsoGeomScene scene;
    IsoGeomInput in;
    screenTanks  tanks;
    const IsoGeomItem *it;
    const IsoGeomQuad *q, *shadow;
    float cx, cy, scale;

    clearMap();
    screenTanksCreate(&tanks);
    /* An evil tank facing north (frame TANK_EVIL_0) centred on square
       (80, 80): listed at its top-left, half a square back. */
    screenTanksAddItem(&tanks, 79, 79, 8, 8, TANK_EVIL_0, 3, "x", 128, 128, 0);
    baseInput(&in, 80, 80);
    in.tanks = &tanks;
    scale = in.cam.scale;
    isoGeomSceneInit(&scene);

    /* With the sheet holding it. */
    in.sheet = sheetAll;
    UT_ASSERT(isoGeomBuild(&scene, &in));
    UT_ASSERT(scene.itemCount == 1);
    it = &scene.items[0];
    UT_ASSERT(it->kind == ISO_GEOM_ITEM_TANK && it->listIndex == 1);
    UT_ASSERT(it->mapX == 80 && it->mapY == 80);
    UT_ASSERT(it->isoSprite && it->frame == 0);
    UT_ASSERT(it->quadCount == 2);   /* shadow, sprite */
    shadow = &scene.stand[it->quadFirst];
    q      = &scene.stand[it->quadFirst + 1];
    UT_ASSERT(q->tex == ISO_GEOM_TEX_ISO);
    UT_ASSERT(fabsf(q->src.x - (float)(ISO_SPRITE_TANK * 100)) < 1e-3f);
    UT_ASSERT(fabsf(q->src.y - (float)(ISO_TEAM_EVIL * 100 + 0)) < 1e-3f);
    /* The cell is a diamond wide and two tall, its bottom-centre half a
       square past the tank's centre. The centre is the corner of square
       (80, 80), so that is the square's own centre. */
    isoProject(&in.cam, in.targetW, in.targetH, 80, 80, 128, 128, 0.0f, &cx, &cy);
    UT_ASSERT(fabsf((q->px[0] + q->px[1]) * 0.5f - cx) < 1e-3f);
    UT_ASSERT(fabsf(q->py[2] - cy) < 1e-3f);
    UT_ASSERT(fabsf((q->px[1] - q->px[0]) - 32.0f * scale) < 1e-3f);
    UT_ASSERT(fabsf((q->py[2] - q->py[0]) - 32.0f * scale) < 1e-3f);
    /* The shadow: the same sprite in black at 40 percent, three game pixels
       down and right. */
    UT_ASSERT(shadow->tex == ISO_GEOM_TEX_ISO && shadow->r == 0 &&
              shadow->a == ISO_SHADOW_SPRITE_ALPHA);
    UT_ASSERT(fabsf((shadow->px[0] - q->px[0]) - 3.0f * scale) < 1e-3f);
    UT_ASSERT(fabsf((shadow->py[0] - q->py[0]) - 3.0f * scale) < 1e-3f);

    /* Without it: the classic sprite, turned two frames, upright and centred
       on the tank's centre. */
    in.sheet = sheetFacesOnly;
    UT_ASSERT(isoGeomBuild(&scene, &in));
    UT_ASSERT(scene.itemCount == 1);
    it = &scene.items[0];
    UT_ASSERT(!it->isoSprite && it->frame == 2);
    UT_ASSERT(it->quadCount == 2);
    q = &scene.stand[it->quadFirst + 1];
    UT_ASSERT(q->tex == ISO_GEOM_TEX_CLASSIC);
    UT_ASSERT(fabsf(q->src.x - (float)TANK_EVIL_2_X) < 1e-3f &&
              fabsf(q->src.y - (float)TANK_EVIL_2_Y) < 1e-3f);
    UT_ASSERT(fabsf(q->src.w - 16.0f) < 1e-3f);
    /* Centred on the tank's centre: the listed top-left plus half a square,
       which is the corner of square (80, 80). */
    isoProject(&in.cam, in.targetW, in.targetH, 80, 80, 0, 0, 0.0f, &cx, &cy);
    UT_ASSERT(fabsf((q->px[0] + q->px[1]) * 0.5f - cx) < 1e-3f);
    UT_ASSERT(fabsf((q->py[0] + q->py[2]) * 0.5f - cy) < 1e-3f);
    UT_ASSERT(fabsf((q->px[1] - q->px[0]) - 16.0f * scale) < 1e-3f);

    /* A boat frame asks for the boat sprite; facing 15 wraps to 1 as a
       stand-in. */
    screenTanksCreate(&tanks);
    screenTanksAddItem(&tanks, 79, 79, 8, 8, TANK_GOODBOAT_15, 3, "x", 128, 128, 0);
    in.sheet = sheetAll;
    UT_ASSERT(isoGeomBuild(&scene, &in));
    q = &scene.stand[scene.items[0].quadFirst + 1];
    UT_ASSERT(fabsf(q->src.x - (float)(ISO_SPRITE_TANK_BOAT * 100)) < 1e-3f);
    UT_ASSERT(fabsf(q->src.y - (float)(ISO_TEAM_GOOD * 100 + 15)) < 1e-3f);
    in.sheet = sheetFacesOnly;
    UT_ASSERT(isoGeomBuild(&scene, &in));
    UT_ASSERT(scene.items[0].frame == 1);
    q = &scene.stand[scene.items[0].quadFirst + 1];
    UT_ASSERT(fabsf(q->src.x - (float)TANK_GOODBOAT_1_X) < 1e-3f &&
              fabsf(q->src.y - (float)TANK_GOODBOAT_1_Y) < 1e-3f);

    /* The Smooth mode keeps the world offset; Classic snaps it to the game
       pixel. */
    screenTanksCreate(&tanks);
    screenTanksAddItem(&tanks, 79, 79, 8, 8, TANK_SELF_0, 3, "x", 133, 141, 0);
    in.animMode = GFX_ANIM_SMOOTH;
    UT_ASSERT(isoGeomBuild(&scene, &in));
    q = &scene.stand[scene.items[0].quadFirst + 1];
    isoProject(&in.cam, in.targetW, in.targetH, 79, 79, 133 + 128, 141 + 128,
               0.0f, &cx, &cy);
    UT_ASSERT(fabsf((q->px[0] + q->px[1]) * 0.5f - cx) < 1e-3f);
    UT_ASSERT(fabsf((q->py[0] + q->py[2]) * 0.5f - cy) < 1e-3f);
    in.animMode = GFX_ANIM_CLASSIC;
    UT_ASSERT(isoGeomBuild(&scene, &in));
    q = &scene.stand[scene.items[0].quadFirst + 1];
    isoProject(&in.cam, in.targetW, in.targetH, 79, 79, 128 + 128, 128 + 128,
               0.0f, &cx, &cy);
    UT_ASSERT(fabsf((q->px[0] + q->px[1]) * 0.5f - cx) < 1e-3f);
    UT_ASSERT(fabsf((q->py[0] + q->py[2]) * 0.5f - cy) < 1e-3f);

    screenTanksDestroy(&tanks);
    isoGeomSceneFree(&scene);
    return 0;
}

int run_isoview_geom_visible_range(void) {
    IsoCamera cam = { 100.5f * 256.0f, 100.5f * 256.0f, 2.0f };
    int aMinX, aMinY, aMaxX, aMaxY;
    int bMinX, bMinY, bMaxX, bMaxY;

    UT_ASSERT(isoVisibleRange(&cam, 640, 480, &aMinX, &aMinY, &aMaxX, &aMaxY));
    UT_ASSERT(isoGeomVisibleRange(&cam, 640, 480, &bMinX, &bMinY, &bMaxX, &bMaxY));
    UT_ASSERT(bMinX == aMinX && bMinY == aMinY);
    /* ceil(10 / 8) = 2 squares further along each axis. */
    UT_ASSERT_MSG(bMaxX == aMaxX + 2 && bMaxY == aMaxY + 2,
                  "widened to (%d,%d) from (%d,%d)", bMaxX, bMaxY, aMaxX, aMaxY);

    /* Clamped at the far corner of the map. */
    cam.centerWX = 255.5f * 256.0f;
    cam.centerWY = 255.5f * 256.0f;
    UT_ASSERT(isoGeomVisibleRange(&cam, 640, 480, &bMinX, &bMinY, &bMaxX, &bMaxY));
    UT_ASSERT(bMaxX == 255 && bMaxY == 255);

    /* Off the map there is nothing to widen. */
    cam.centerWX = -5000.0f * 256.0f;
    UT_ASSERT(!isoGeomVisibleRange(&cam, 640, 480, &bMinX, &bMinY, &bMaxX, &bMaxY));

    /* The build walks the widened range. */
    {
        IsoGeomScene scene;
        IsoGeomInput in;
        clearMap();
        baseInput(&in, 100, 100);
        isoGeomSceneInit(&scene);
        UT_ASSERT(isoGeomBuild(&scene, &in));
        UT_ASSERT(scene.minX == aMinX && scene.maxX == aMaxX + 2);
        UT_ASSERT(scene.minY == aMinY && scene.maxY == aMaxY + 2);
        UT_ASSERT(scene.groundCount ==
                  (scene.maxX - scene.minX + 1) * (scene.maxY - scene.minY + 1));
        isoGeomSceneFree(&scene);
    }
    return 0;
}

int run_isoview_geom_mine(void) {
    IsoGeomScene scene;
    IsoGeomInput in;
    const IsoGeomQuad *q;

    clearMap();
    sMap.tile[90][90]   = ROAD_SOLID;
    sMap.flags[90][90] |= OVERVIEW_F_MINE;
    baseInput(&in, 90, 90);
    isoGeomSceneInit(&scene);
    UT_ASSERT(isoGeomBuild(&scene, &in));

    UT_ASSERT(groundQuadsAt(&scene, 90, 90, &q) == 2);
    /* The ground first, the mine over it, on the same diamond. */
    UT_ASSERT(q[0].tex == ISO_GEOM_TEX_CLASSIC);
    UT_ASSERT(fabsf(q[0].src.x - (float)ROAD_SOLID_X) < 1e-3f);
    UT_ASSERT(q[1].tex == ISO_GEOM_TEX_CLASSIC);
    UT_ASSERT(fabsf(q[1].src.x - (float)MINE_X) < 1e-3f &&
              fabsf(q[1].src.y - (float)MINE_Y) < 1e-3f);
    UT_ASSERT(fabsf(q[0].px[0] - q[1].px[0]) < 1e-3f &&
              fabsf(q[0].py[2] - q[1].py[2]) < 1e-3f);
    UT_ASSERT(groundQuadsAt(&scene, 91, 90, &q) == 1);

    isoGeomSceneFree(&scene);
    return 0;
}
