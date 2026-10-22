/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The replay's isometric parts (lv_iso.c), built from a small replayed state
 * with no window: a decoder with a map, two players, a pillbox and a base.
 *
 *   lv_iso_map_tiles — every square of the parts carries the tile the viewer
 *       calculates for it (grass, a lone building, a pillbox, a base) and the
 *       live and in-sight flags, with a mined square flagged as mined.
 *
 *   lv_iso_tanks_and_camera — the tank list holds one entry per placed tank in
 *       map coordinates, half a square up and left of the tank as the classic
 *       picture has it, with its slot; the camera sits on the selected tank
 *       and on the map's centre when none is selected.
 *
 *   lv_iso_rotation — a quarter turn puts square (x, y) at (255 - y, x) and a
 *       tank's facing four steps on; the turned tank list's entry and the
 *       turned pillbox square project to where the turned map puts them.
 */

#include <math.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_players.h"
#include "lv_pillbox.h"
#include "lv_bases.h"
#include "lv_bolo_map.h"
#include "lv_screentank.h"
#include "lv_tilenum.h"
#include "lv_iso.h"
#include "../../src/gui/sdl3/iso_project.h"
#include "test_harness.h"

static LvIsoParts s_parts;

/* The grass tile on the sheet; GRASS in lv_global.h is the terrain. */
#define GRASS_TILE GRASS

/* A decoder with a grass map, a lone building at (40, 60), a mine at
   (41, 60), a pillbox at (50, 50), a base at (60, 70), and players in slots
   0 and 2 with their tanks at (100, 120) and (130, 90). */
static LogViewerState *isoDecoder(void) {
    LogViewerState *lv = lv_decoderCreate(false);
    int x, y;
    pillbox p;
    base b;
    BYTE allies[1] = { 0 };

    if (lv == NULL) return NULL;
    lv_screenSetSizeX(16);
    lv_screenSetSizeY(16);
    lv_screenDestroy();
    lv_screenSetup();

    for (x = 0; x < 256; x++) {
        for (y = 0; y < 256; y++) {
            lv_mapSetPos(&lv->mp, (BYTE)x, (BYTE)y, GRASS);
        }
    }
    lv_mapSetPos(&lv->mp, 40, 60, BUILDING);
    lv_mapSetPos(&lv->mp, 41, 60, (BYTE)(GRASS + MINE_SUBTRACT));

    lv_pillsSetNumPills(&lv->pb, 1);
    memset(&p, 0, sizeof(p));
    p.x = 50; p.y = 50; p.owner = NEUTRAL; p.armour = 15; p.inTank = false;
    lv_pillsSetPill(&lv->pb, &p, 1);

    lv_basesSetNumBases(&lv->bs, 1);
    memset(&b, 0, sizeof(b));
    b.x = 60; b.y = 70; b.owner = NEUTRAL;
    lv_basesSetBase(&lv->bs, &b, 1);

    lv_playersSetPlayer(0, "Alice", "", 100, 120, 8, 8, 2, false, 0, allies,
                        false, true, 0);
    lv_playersSetPlayer(2, "Carol", "", 130, 90, 0, 0, 12, false, 0, allies,
                        false, true, 0);
    lv_playersSetSelf(0);
    lv->logLoaded = TRUE;
    /* Naming self selects its tank; the cases pick their own selection. */
    lv_screenDeselectTank();
    return lv;
}

static int tankIndexForSlot(screenTanks *tks, BYTE slot) {
    BYTE n = lv_screenTanksGetNumEntries(tks), i;
    for (i = 1; i <= n; i++) {
        if (lv_screenTanksGetSlot(tks, i) == slot) return i;
    }
    return 0;
}

int run_lv_iso_map_tiles(void) {
    LogViewerState *lv = isoDecoder();
    int x, y, live = 0;
    UT_ASSERT_MSG(lv != NULL, "decoder failed to create");

    lv_isoSetRotation(0);
    lv_isoBuild(&s_parts, 2.0f);

    UT_ASSERT_MSG(s_parts.rotation == 0, "no turn");
    UT_ASSERT_MSG(s_parts.tile[100][100] == GRASS_TILE, "grass is grass: %d", s_parts.tile[100][100]);
    /* The map's mined border, as the viewer shows it. */
    UT_ASSERT_MSG((s_parts.flags[10][10] & LV_ISO_F_MINE) != 0, "the border is mined");
    UT_ASSERT_MSG(s_parts.tile[40][60] == BUILD_SINGLE, "a lone building: %d", s_parts.tile[40][60]);
    UT_ASSERT_MSG(s_parts.tile[50][50] == PILL_EVIL_15, "a neutral pillbox at full armour: %d",
                  s_parts.tile[50][50]);
    UT_ASSERT_MSG(s_parts.tile[60][70] == BASE_NEUTRAL || s_parts.tile[60][70] == BASE_GOOD ||
                  s_parts.tile[60][70] == BASE_EVIL, "a base: %d", s_parts.tile[60][70]);
    UT_ASSERT_MSG(s_parts.tile[41][60] == GRASS_TILE, "the mined square shows its ground: %d",
                  s_parts.tile[41][60]);
    UT_ASSERT_MSG((s_parts.flags[41][60] & LV_ISO_F_MINE) != 0, "the mine is flagged");
    UT_ASSERT_MSG((s_parts.flags[40][60] & LV_ISO_F_MINE) == 0, "no mine under the building");
    for (x = 0; x < 256; x++) {
        for (y = 0; y < 256; y++) {
            if ((s_parts.flags[x][y] & (LV_ISO_F_LIVE | LV_ISO_F_SIGHT)) ==
                (LV_ISO_F_LIVE | LV_ISO_F_SIGHT)) {
                live++;
            }
        }
    }
    UT_ASSERT_MSG(live == 256 * 256, "every square live and in sight: %d", live);

    /* The item labels: the pillbox as number 0, the base as number 0. */
    UT_ASSERT_MSG(s_parts.itemCount == 2, "two item labels: %d", s_parts.itemCount);
    UT_ASSERT_MSG(!s_parts.items[0].isBase && s_parts.items[0].mapX == 50 &&
                  s_parts.items[0].mapY == 50 && s_parts.items[0].number == 0, "the pillbox label");
    UT_ASSERT_MSG(s_parts.items[1].isBase && s_parts.items[1].mapX == 60 &&
                  s_parts.items[1].mapY == 70 && s_parts.items[1].number == 0, "the base label");

    lv_decoderDestroy(lv);
    return 0;
}

int run_lv_iso_tanks_and_camera(void) {
    LogViewerState *lv = isoDecoder();
    screenTanks *tks;
    BYTE mx, my, px, py, frame, team, dir;
    bool onBoat;
    char name[256];
    int i;
    UT_ASSERT_MSG(lv != NULL, "decoder failed to create");

    lv_isoSetRotation(0);
    lv_isoBuild(&s_parts, 1.5f);
    tks = (screenTanks *)s_parts.tanks;
    UT_ASSERT_MSG(tks != NULL, "a tank list");
    UT_ASSERT_MSG(lv_screenTanksGetNumEntries(tks) == 2, "two tanks: %d",
                  lv_screenTanksGetNumEntries(tks));

    /* Alice at (100, 120) pixel (8, 8): the sprite's top left is half a
       square up and left, (100, 120) pixel (0, 0). Self, facing 2. */
    i = tankIndexForSlot(tks, 0);
    UT_ASSERT_MSG(i > 0, "Alice is listed");
    lv_screenTanksGetItem(tks, (BYTE)i, &mx, &my, &px, &py, &frame, &team, &dir, &onBoat, name);
    UT_ASSERT_MSG(mx == 100 && my == 120 && px == 0 && py == 0,
                  "Alice's sprite: %d,%d pixel %d,%d", mx, my, px, py);
    UT_ASSERT_MSG(frame == TANK_SELF_0 + 2 && dir == 2, "Alice is self facing 2: %d %d", frame, dir);
    UT_ASSERT_MSG(strstr(name, "Alice") != NULL, "Alice's name: %s", name);

    /* Carol at (130, 90) pixel (0, 0): top left (129, 89) pixel (8, 8). Not
       allied, so an enemy facing 12. */
    i = tankIndexForSlot(tks, 2);
    UT_ASSERT_MSG(i > 0, "Carol is listed");
    lv_screenTanksGetItem(tks, (BYTE)i, &mx, &my, &px, &py, &frame, &team, &dir, &onBoat, name);
    UT_ASSERT_MSG(mx == 129 && my == 89 && px == 8 && py == 8,
                  "Carol's sprite: %d,%d pixel %d,%d", mx, my, px, py);
    UT_ASSERT_MSG(frame == TANK_EVIL_0 + 12 && dir == 12, "Carol is an enemy facing 12: %d", frame);

    /* The places kept for the overlays, before any turn: whole-map pixels of
       the sprite's top left. */
    UT_ASSERT_MSG(s_parts.tankPosCount == 2, "two places");
    for (i = 0; i < s_parts.tankPosCount; i++) {
        if (s_parts.tankPos[i].slot == 2) {
            UT_ASSERT_MSG(s_parts.tankPos[i].x == 129 * 16 + 8 && s_parts.tankPos[i].y == 89 * 16 + 8,
                          "Carol's place: %d,%d", s_parts.tankPos[i].x, s_parts.tankPos[i].y);
        }
    }

    /* No tank selected: the camera is on the map's centre at the scale asked. */
    UT_ASSERT_MSG(s_parts.followSlot == -1, "nothing followed");
    UT_ASSERT_MSG(s_parts.camWX == 128.0f * 256.0f && s_parts.camWY == 128.0f * 256.0f,
                  "the map's centre: %g %g", (double)s_parts.camWX, (double)s_parts.camWY);
    UT_ASSERT_MSG(s_parts.camScale == 1.5f, "the scale asked");

    /* Carol selected: the camera is on her tank's position. */
    lv_screenSelectTank(2);
    lv_isoBuild(&s_parts, 1.5f);
    UT_ASSERT_MSG(s_parts.followSlot == 2, "Carol followed: %d", s_parts.followSlot);
    UT_ASSERT_MSG(s_parts.camWX == 130.0f * 256.0f && s_parts.camWY == 90.0f * 256.0f,
                  "on Carol: %g %g", (double)s_parts.camWX, (double)s_parts.camWY);
    /* Alice, with her pixel offset: 100 * 256 + 8 * 16. */
    lv_screenSelectTank(0);
    lv_isoBuild(&s_parts, 1.5f);
    UT_ASSERT_MSG(s_parts.followSlot == 0, "Alice followed");
    UT_ASSERT_MSG(s_parts.camWX == 100.0f * 256.0f + 128.0f && s_parts.camWY == 120.0f * 256.0f + 128.0f,
                  "on Alice: %g %g", (double)s_parts.camWX, (double)s_parts.camWY);

    UT_ASSERT_MSG(lv_isoScaleForZoom(1.0f) == LV_ISO_SCALE_PER_STEP, "the 1.0 step");
    UT_ASSERT_MSG(lv_isoScaleForZoom(0.5f) == 0.5f * LV_ISO_SCALE_PER_STEP, "half");
    UT_ASSERT_MSG(lv_isoScaleForZoom(0.0f) == LV_ISO_SCALE_PER_STEP, "a bad step is 1.0");

    lv_decoderDestroy(lv);
    return 0;
}

int run_lv_iso_rotation(void) {
    LogViewerState *lv;
    screenTanks *tks;
    BYTE mx, my, px, py, frame, team, dir;
    bool onBoat;
    char name[256];
    int rx, ry, i;
    IsoCamera cam;
    float ax, ay, bx, by;

    /* The sums alone. A quarter turn clockwise: north goes east. */
    lv_isoRotateSquare(0, 10, 20, &rx, &ry);
    UT_ASSERT_MSG(rx == 10 && ry == 20, "no turn");
    lv_isoRotateSquare(1, 10, 20, &rx, &ry);
    UT_ASSERT_MSG(rx == 255 - 20 && ry == 10, "one turn: %d,%d", rx, ry);
    lv_isoRotateSquare(2, 10, 20, &rx, &ry);
    UT_ASSERT_MSG(rx == 255 - 10 && ry == 255 - 20, "two turns: %d,%d", rx, ry);
    lv_isoRotateSquare(3, 10, 20, &rx, &ry);
    UT_ASSERT_MSG(rx == 20 && ry == 255 - 10, "three turns: %d,%d", rx, ry);
    lv_isoRotateSquare(4, 10, 20, &rx, &ry);
    UT_ASSERT_MSG(rx == 10 && ry == 20, "four turns is none");
    lv_isoRotateSquare(-1, 10, 20, &rx, &ry);
    UT_ASSERT_MSG(rx == 20 && ry == 255 - 10, "a turn back is three on");
    /* The centre of the map stays within a unit of itself. */
    lv_isoRotateWorld(1, 128 * 256, 128 * 256, &rx, &ry);
    UT_ASSERT_MSG(rx == 128 * 256 - 1 && ry == 128 * 256, "the centre: %d,%d", rx, ry);

    UT_ASSERT_MSG(lv_isoRotateTankFrame(1, TANK_SELF_0 + 0) == TANK_SELF_0 + 4, "north goes east");
    UT_ASSERT_MSG(lv_isoRotateTankFrame(1, TANK_GOOD_0 + 14) == TANK_GOOD_0 + 2, "wraps in its block");
    UT_ASSERT_MSG(lv_isoRotateTankFrame(2, TANK_EVILBOAT_0 + 3) == TANK_EVILBOAT_0 + 11, "two turns");
    UT_ASSERT_MSG(lv_isoRotateTankFrame(3, TANK_SELF_0 + 0) == TANK_SELF_0 + 12, "three turns");
    UT_ASSERT_MSG(lv_isoRotateTankFrame(1, 200) == 200, "not a tank frame");
    UT_ASSERT_MSG(lv_isoRotateShellFrame(1, SHELL_DIR0) == SHELL_DIR4, "a shell turns too");
    UT_ASSERT_MSG(lv_isoRotateShellFrame(1, SHELL_DIR13) == SHELL_DIR1, "and wraps");
    UT_ASSERT_MSG(lv_isoRotateShellFrame(1, SHELL_DIR15 + 1) == SHELL_DIR15 + 1, "an explosion does not");

    lv_isoSetRotation(5);
    UT_ASSERT_MSG(lv_isoRotation() == 1, "the rotation wraps: %d", lv_isoRotation());
    lv_isoSetRotation(-1);
    UT_ASSERT_MSG(lv_isoRotation() == 3, "and wraps back: %d", lv_isoRotation());

    lv = isoDecoder();
    UT_ASSERT_MSG(lv != NULL, "decoder failed to create");
    lv_isoSetRotation(1);
    lv_isoBuild(&s_parts, 2.0f);
    UT_ASSERT_MSG(s_parts.rotation == 1, "one turn recorded");

    /* The map turned: the building at (40, 60) is at (255 - 60, 40). */
    UT_ASSERT_MSG(s_parts.tile[195][40] == BUILD_SINGLE, "the building turned: %d",
                  s_parts.tile[195][40]);
    UT_ASSERT_MSG(s_parts.tile[40][60] == GRASS_TILE, "and left grass behind");
    UT_ASSERT_MSG((s_parts.flags[195][41] & LV_ISO_F_MINE) != 0, "the mine turned with it");
    /* The pillbox label on the turned square. */
    UT_ASSERT_MSG(s_parts.itemCount == 2 && s_parts.items[0].mapX == 205 &&
                  s_parts.items[0].mapY == 50, "the pillbox label turned: %d,%d",
                  s_parts.items[0].mapX, s_parts.items[0].mapY);

    /* Carol's tank at (130, 90) with no pixel offset turns to world
       (65535 - 90 * 256, 130 * 256), and the sprite's top left is half a
       square up and left of that: (165, 129) pixel (7, 8), facing 12 + 4. */
    tks = (screenTanks *)s_parts.tanks;
    i = tankIndexForSlot(tks, 2);
    UT_ASSERT_MSG(i > 0, "Carol is listed");
    lv_screenTanksGetItem(tks, (BYTE)i, &mx, &my, &px, &py, &frame, &team, &dir, &onBoat, name);
    UT_ASSERT_MSG(mx == 165 && my == 129 && px == 7 && py == 8,
                  "Carol's turned sprite: %d,%d pixel %d,%d", mx, my, px, py);
    UT_ASSERT_MSG(dir == 0 && frame == TANK_EVIL_0 + 0, "Carol faces north after the turn: %d", dir);

    /* Her projected position is where the turned map puts her: the tank's
       centre, half a square on from the list entry's top left, projects to
       the same window point as her unturned centre put through the turn. */
    cam.centerWX = s_parts.camWX;
    cam.centerWY = s_parts.camWY;
    cam.scale    = s_parts.camScale;
    {
        BYTE wx = 0, wy = 0, angle = 0;
        int cx, cy, tx, ty;
        lv_screenTanksGetSubPixel(tks, (BYTE)i, &wx, &wy, &angle);
        cx = mx * 256 + wx + 128;
        cy = my * 256 + wy + 128;
        lv_isoRotateWorld(1, 130 * 256, 90 * 256, &tx, &ty);
        UT_ASSERT_MSG(cx == tx && cy == ty, "the list's centre is the turned centre: %d,%d vs %d,%d",
                      cx, cy, tx, ty);
        isoSquareToWindow(&cam, 0.0f, 0.0f, 800, 600, cx >> 8, cy >> 8, cx & 255, cy & 255,
                          0.0f, &ax, &ay);
        isoSquareToWindow(&cam, 0.0f, 0.0f, 800, 600, tx >> 8, ty >> 8, tx & 255, ty & 255,
                          0.0f, &bx, &by);
        UT_ASSERT_MSG(fabsf(ax - bx) < 0.01f && fabsf(ay - by) < 0.01f,
                      "the turned entry projects where the turned map puts it: %g,%g vs %g,%g",
                      (double)ax, (double)ay, (double)bx, (double)by);
        UT_ASSERT_MSG(angle == 0, "the angle turned with the facing: %d", angle);
    }

    /* The map position path the overlays take agrees with the list: the
       unturned centre of Carol's tank, through lv_isoMapToWindow, lands on
       the turned square's centre. */
    lv_isoMapToWindow(&s_parts, 0.0f, 0.0f, 800, 600, 130.5f, 90.5f, 0.0f, &ax, &ay);
    isoSquareToWindow(&cam, 0.0f, 0.0f, 800, 600, 165, 130, 128, 128, 0.0f, &bx, &by);
    UT_ASSERT_MSG(fabsf(ax - bx) < 1.0f && fabsf(ay - by) < 1.0f,
                  "the overlay path lands on the turned square: %g,%g vs %g,%g",
                  (double)ax, (double)ay, (double)bx, (double)by);

    /* The camera turns with the map: Carol selected and followed. */
    lv_screenSelectTank(2);
    lv_isoBuild(&s_parts, 2.0f);
    UT_ASSERT_MSG(s_parts.followSlot == 2, "Carol followed");
    UT_ASSERT_MSG(fabsf(s_parts.camWX - (65536.0f - 90.0f * 256.0f)) < 0.5f &&
                  fabsf(s_parts.camWY - 130.0f * 256.0f) < 0.5f,
                  "the camera on the turned tank: %g %g", (double)s_parts.camWX, (double)s_parts.camWY);

    lv_isoSetRotation(0);
    lv_decoderDestroy(lv);
    return 0;
}
