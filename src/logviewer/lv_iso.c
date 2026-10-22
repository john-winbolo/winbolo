/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          lv_iso.c
 * Purpose:
 *   Fills the isometric view's parts from the replayed
 *   state: see lv_iso.h. This file sees the viewer's own
 *   types, so it reads the map, the players, the pillboxes,
 *   the bases and the shells directly and converts.
 *********************************************************/

#include "lv_iso.h"

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_players.h"
#include "lv_pillbox.h"
#include "lv_bases.h"
#include "lv_bolo_map.h"
#include "lv_shells.h"
#include "lv_screentank.h"
#include "lv_screenlgm.h"
#include "lv_screenbullet.h"
#include "lv_tilenum.h"
#include "tiles.h"
#include "../gui/sdl3/iso_project.h"

#include <math.h>
#include <string.h>

/* screen.c: the tile the viewer draws a square with, from the map and the
   items on it. The two trailing arguments name the square's place in the
   screen buffer, whose mine flag it writes; 0, 0 here. That flag belongs
   to the classic picture's first square, but the classic draw never reads
   it after this: leaving the isometric mode marks the whole screen dirty
   and the next screen update recalculates every square first. */
extern BYTE lv_screenCalcSquare(BYTE xValue, BYTE yValue, BYTE scrX, BYTE scrY);

static bool s_enabled  = false;
static int  s_rotation = 0;

/* The lists the parts point at, rebuilt on every build. */
static LvIsoParts    s_parts;
static screenTanks   s_tanks;
static screenLgm     s_lgms    = NULL;
static screenBullets s_bullets = NULL;
static bool          s_listsMade = false;

bool lv_isoEnabled(void) {
    return s_enabled;
}

void lv_isoSetEnabled(bool on) {
    s_enabled = on;
}

int lv_isoRotation(void) {
    return s_rotation;
}

void lv_isoSetRotation(int quarterTurns) {
    s_rotation = ((quarterTurns % 4) + 4) % 4;
}

LvIsoParts *lv_isoParts(void) {
    return &s_parts;
}

/*********************************************************
 * Rotation.
 *********************************************************/

void lv_isoRotateWorld(int rotation, int wx, int wy, int *outX, int *outY) {
    int x = wx, y = wy;
    switch (((rotation % 4) + 4) % 4) {
        case 1:  x = LV_ISO_WORLD_MAX - wy; y = wx;                   break;
        case 2:  x = LV_ISO_WORLD_MAX - wx; y = LV_ISO_WORLD_MAX - wy; break;
        case 3:  x = wy;                   y = LV_ISO_WORLD_MAX - wx; break;
        default: break;
    }
    if (outX) *outX = x;
    if (outY) *outY = y;
}

void lv_isoRotateWorldF(int rotation, float wx, float wy, float *outX, float *outY) {
    const float span = (float)(LV_ISO_WORLD_MAX + 1);
    float x = wx, y = wy;
    switch (((rotation % 4) + 4) % 4) {
        case 1:  x = span - wy; y = wx;        break;
        case 2:  x = span - wx; y = span - wy; break;
        case 3:  x = wy;        y = span - wx; break;
        default: break;
    }
    if (outX) *outX = x;
    if (outY) *outY = y;
}

void lv_isoRotateSquare(int rotation, int mx, int my, int *outX, int *outY) {
    int wx, wy;
    lv_isoRotateWorld(rotation, mx * LV_ISO_WORLD_PER_SQUARE,
                      my * LV_ISO_WORLD_PER_SQUARE, &wx, &wy);
    if (outX) *outX = wx / LV_ISO_WORLD_PER_SQUARE;
    if (outY) *outY = wy / LV_ISO_WORLD_PER_SQUARE;
}

/* The tank sprites sit in blocks of sixteen facings from TANK_SELF_0, so the
   block is the frame's high bits and the facing its low four. */
int lv_isoRotateTankFrame(int rotation, int frame) {
    int block, facing;
    if (frame < TANK_SELF_0 || frame > TANK_EVILBOAT_15) return frame;
    block  = (frame - TANK_SELF_0) & ~15;
    facing = (frame - TANK_SELF_0) & 15;
    facing = (facing + 4 * (((rotation % 4) + 4) % 4)) & 15;
    return TANK_SELF_0 + block + facing;
}

/* A shell's sixteen directions run SHELL_DIR0 to SHELL_DIR15; every other
   frame (the explosions) has no facing. */
int lv_isoRotateShellFrame(int rotation, int frame) {
    int facing;
    if (frame < SHELL_DIR0 || frame > SHELL_DIR15) return frame;
    facing = (frame - SHELL_DIR0 + 4 * (((rotation % 4) + 4) % 4)) & 15;
    return SHELL_DIR0 + facing;
}

float lv_isoScaleForZoom(float zoomStep) {
    if (zoomStep <= 0.0f) zoomStep = 1.0f;
    return zoomStep * LV_ISO_SCALE_PER_STEP;
}

/*********************************************************
 * The build.
 *********************************************************/

/* A world position split into the square and the 0..255 offset in it. */
static void lvIsoSplit(int w, BYTE *mapV, BYTE *pix, BYTE *worldOff) {
    if (w < 0) w = 0;
    if (w > LV_ISO_WORLD_MAX) w = LV_ISO_WORLD_MAX;
    *mapV     = (BYTE)(w >> 8);
    *worldOff = (BYTE)(w & 0xFF);
    *pix      = (BYTE)((w & 0xF0) >> 4);
}

static void lvIsoFreeLists(void) {
    if (!s_listsMade) return;
    lv_screenLgmDestroy(&s_lgms);
    lv_screenBulletsDestroy(&s_bullets);
    s_listsMade = false;
}

static void lvIsoMakeLists(void) {
    lvIsoFreeLists();
    lv_screenTanksCreate(&s_tanks);
    lv_screenLgmCreate(&s_lgms);
    s_bullets = lv_screenBulletsCreate();
    s_listsMade = true;
}

static void lvIsoBuildMap(LvIsoParts *out, LogViewerState *lv, int rotation) {
    int x, y;
    for (x = 0; x < LV_ISO_MAP_SIZE; x++) {
        for (y = 0; y < LV_ISO_MAP_SIZE; y++) {
            int rx, ry;
            unsigned char flags = LV_ISO_F_LIVE | LV_ISO_F_SIGHT;
            BYTE tile = lv_screenCalcSquare((BYTE)x, (BYTE)y, 0, 0);
            if (lv_mapIsMine(&lv->mp, (BYTE)x, (BYTE)y)) flags |= LV_ISO_F_MINE;
            lv_isoRotateSquare(rotation, x, y, &rx, &ry);
            out->tile[rx][ry]  = tile;
            out->flags[rx][ry] = flags;
        }
    }
}

/* The tanks, as lv_playersMakeScreenTanks makes them for the classic picture
   but over the whole map and rotated: the sprite's top left is half a square
   up and left of the tank's position, taken after the turn so the sprite
   stays on its tank. */
static void lvIsoBuildTanks(LvIsoParts *out, int rotation) {
    BYTE self = lv_playersGetSelf();
    BYTE slot;

    out->tankPosCount = 0;
    for (slot = 0; slot < MAX_TANKS; slot++) {
        BYTE rmx = 0, rmy = 0, rpx = 0, rpy = 0, facing = 0;
        bool onBoat = false;
        int cx, cy, rx, ry, frame, dir;
        BYTE mx, my, px, py, wx, wy;
        char name[LV_ISO_NAME_LEN];

        if (!lv_playersIsInUse(slot)) continue;
        lv_playersGetTankDetails(slot, &rmx, &rmy, &rpx, &rpy, &facing, &onBoat);
        /* The (0, 0) corner is where a tank with no position sits. */
        if (rmx == 0 && rmy == 0 && rpx == 0 && rpy == 0) continue;

        cx = ((int)rmx << TANK_SHIFT_MAPSIZE) + ((int)rpx << TANK_SHIFT_RIGHT2);
        cy = ((int)rmy << TANK_SHIFT_MAPSIZE) + ((int)rpy << TANK_SHIFT_RIGHT2);

        name[0] = '\0';
        lv_playersMakeScreenName(slot, name);

        /* Before the turn, for the overlays over the blit. */
        if (out->tankPosCount < MAX_TANKS) {
            int tx = cx - TANK_SUBTRACT, ty = cy - TANK_SUBTRACT;
            out->tankPos[out->tankPosCount].x = (tx >> 8) * TILE_SIZE_X + ((tx & 0xF0) >> 4);
            out->tankPos[out->tankPosCount].y = (ty >> 8) * TILE_SIZE_Y + ((ty & 0xF0) >> 4);
            out->tankPos[out->tankPosCount].slot = slot;
            strncpy(out->tankPos[out->tankPosCount].name, name, LV_ISO_NAME_LEN - 1);
            out->tankPos[out->tankPosCount].name[LV_ISO_NAME_LEN - 1] = '\0';
            out->tankPosCount++;
        }

        lv_isoRotateWorld(rotation, cx, cy, &rx, &ry);
        lvIsoSplit(rx - TANK_SUBTRACT, &mx, &px, &wx);
        lvIsoSplit(ry - TANK_SUBTRACT, &my, &py, &wy);

        dir   = (facing + 4 * rotation) & 15;
        frame = dir;
        if (onBoat) frame += TANK_BOAT_ADD;
        if (slot != self) {
            frame += lv_playersIsAllie(slot, self) ? TANK_GOOD_ADD : TANK_EVIL_ADD;
        }
        /* The turned offset in full, not the four bits the log carries: a
           turn moves a position by a whole number of units, and rounding it
           to a pixel would put the sprite a unit off its square. */
        lv_screenTanksAddItem(&s_tanks, mx, my, px, py, (BYTE)frame,
                              lv_playersGetTeamForOwner(slot), (BYTE)dir, onBoat,
                              name, wx, wy, (BYTE)(dir << 4));
        lv_screenTanksSetLastSlot(&s_tanks, slot);
    }
}

static void lvIsoBuildLgms(int rotation) {
    BYTE slot;
    for (slot = 0; slot < MAX_TANKS; slot++) {
        bool isOut = false, isDead = false;
        BYTE lmx = 0, lmy = 0, lpx = 0, lpy = 0, lframe = 0;
        BYTE mx, my, px, py, wx, wy;
        int rx, ry;

        if (!lv_playersIsInUse(slot)) continue;
        lv_playersGetLgmStatus(slot, &isOut, &isDead);
        if (!isOut) continue;
        lv_playersGetLgmDetails(slot, &lmx, &lmy, &lpx, &lpy, &lframe);
        if (lmx == 0 && lmy == 0) continue;

        lv_isoRotateWorld(rotation,
                          ((int)lmx << TANK_SHIFT_MAPSIZE) + ((int)lpx << TANK_SHIFT_RIGHT2),
                          ((int)lmy << TANK_SHIFT_MAPSIZE) + ((int)lpy << TANK_SHIFT_RIGHT2),
                          &rx, &ry);
        lvIsoSplit(rx, &mx, &px, &wx);
        lvIsoSplit(ry, &my, &py, &wy);
        lv_screenLgmAddItem(&s_lgms, mx, my, px, py, lframe, wx, wy);
    }
}

static void lvIsoBuildShells(LogViewerState *lv, int rotation) {
    shells q = lv->shs;
    while (q != NULL) {
        BYTE mx, my, px, py, wx, wy;
        int rx, ry;
        lv_isoRotateWorld(rotation,
                          ((int)q->mx << TANK_SHIFT_MAPSIZE) + ((int)q->px << TANK_SHIFT_RIGHT2),
                          ((int)q->my << TANK_SHIFT_MAPSIZE) + ((int)q->py << TANK_SHIFT_RIGHT2),
                          &rx, &ry);
        lvIsoSplit(rx, &mx, &px, &wx);
        lvIsoSplit(ry, &my, &py, &wy);
        lv_screenBulletsAddItem(&s_bullets, mx, my, px, py,
                                (BYTE)lv_isoRotateShellFrame(rotation, q->frame), wx, wy);
        q = q->next;
    }
}

static void lvIsoBuildItems(LvIsoParts *out, LogViewerState *lv, int rotation) {
    BYTE total, i;
    out->itemCount = 0;

    total = lv_pillsGetNumPills(&lv->pb);
    for (i = 1; i <= total && out->itemCount < LV_ISO_ITEMS_MAX; i++) {
        pillbox p;
        int rx, ry;
        if (!lv_pillsIsActive(&lv->pb, i)) continue;
        memset(&p, 0, sizeof(p));
        lv_pillsGetPill(&lv->pb, &p, i);
        if (p.inTank) continue;
        lv_isoRotateSquare(rotation, p.x, p.y, &rx, &ry);
        out->items[out->itemCount].mapX   = (unsigned char)rx;
        out->items[out->itemCount].mapY   = (unsigned char)ry;
        out->items[out->itemCount].number = (unsigned char)(i - 1);
        out->items[out->itemCount].isBase = false;
        out->itemCount++;
    }

    total = lv_basesGetNumBases(&lv->bs);
    for (i = 1; i <= total && out->itemCount < LV_ISO_ITEMS_MAX; i++) {
        base b;
        int rx, ry;
        if (!lv_basesIsActive(&lv->bs, i)) continue;
        memset(&b, 0, sizeof(b));
        lv_basesGetBase(&lv->bs, &b, i);
        lv_isoRotateSquare(rotation, b.x, b.y, &rx, &ry);
        out->items[out->itemCount].mapX   = (unsigned char)rx;
        out->items[out->itemCount].mapY   = (unsigned char)ry;
        out->items[out->itemCount].number = (unsigned char)(i - 1);
        out->items[out->itemCount].isBase = true;
        out->itemCount++;
    }
}

static void lvIsoBuildCamera(LvIsoParts *out, int rotation, float camScale) {
    int selected = lv_screenSelectedTank();
    out->camScale   = camScale;
    out->followSlot = -1;
    out->camWX = 128.0f * LV_ISO_WORLD_PER_SQUARE;
    out->camWY = 128.0f * LV_ISO_WORLD_PER_SQUARE;
    if (selected >= 0 && selected < MAX_TANKS && lv_playersIsInUse((BYTE)selected)) {
        BYTE mx = 0, my = 0, px = 0, py = 0, frame = 0;
        bool onBoat = false;
        lv_playersGetTankDetails((BYTE)selected, &mx, &my, &px, &py, &frame, &onBoat);
        if (mx != 0 || my != 0 || px != 0 || py != 0) {
            float wx = (float)(((int)mx << TANK_SHIFT_MAPSIZE) + ((int)px << TANK_SHIFT_RIGHT2));
            float wy = (float)(((int)my << TANK_SHIFT_MAPSIZE) + ((int)py << TANK_SHIFT_RIGHT2));
            lv_isoRotateWorldF(rotation, wx, wy, &out->camWX, &out->camWY);
            out->followSlot = selected;
        }
    }
}

void lv_isoBuild(LvIsoParts *out, float camScale) {
    LogViewerState *lv = lv_screenGetState();
    int rotation = s_rotation;

    if (out == NULL || lv == NULL) return;
    out->rotation = rotation;
    lvIsoMakeLists();
    lvIsoBuildMap(out, lv, rotation);
    lvIsoBuildTanks(out, rotation);
    lvIsoBuildLgms(rotation);
    lvIsoBuildShells(lv, rotation);
    lvIsoBuildItems(out, lv, rotation);
    lvIsoBuildCamera(out, rotation, camScale);
    out->tanks   = &s_tanks;
    out->lgms    = &s_lgms;
    out->bullets = &s_bullets;
}

void lv_isoMapToWindow(const LvIsoParts *p, float rectX, float rectY,
                       int rectW, int rectH, float mapX, float mapY,
                       float height, float *outWinX, float *outWinY) {
    IsoCamera cam;
    float wx, wy;
    int sq, pix;
    int mX, mY, pX, pY;

    cam.centerWX = p->camWX;
    cam.centerWY = p->camWY;
    cam.scale    = p->camScale;
    lv_isoRotateWorldF(p->rotation, mapX * LV_ISO_WORLD_PER_SQUARE,
                       mapY * LV_ISO_WORLD_PER_SQUARE, &wx, &wy);
    sq  = (int)floorf(wx / LV_ISO_WORLD_PER_SQUARE);
    pix = (int)(wx - (float)sq * LV_ISO_WORLD_PER_SQUARE);
    mX = sq; pX = pix;
    sq  = (int)floorf(wy / LV_ISO_WORLD_PER_SQUARE);
    pix = (int)(wy - (float)sq * LV_ISO_WORLD_PER_SQUARE);
    mY = sq; pY = pix;
    isoSquareToWindow(&cam, rectX, rectY, rectW, rectH, mX, mY, pX, pY, height,
                      outWinX, outWinY);
}
