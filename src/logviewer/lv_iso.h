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
 * Name:          lv_iso.h
 * Purpose:
 *   The replay's half of the isometric view: the parts the
 *   shared drawer (gui/sdl3/isoview.c) reads, filled from
 *   the viewer's own map, players, pillboxes, bases and
 *   shells at the playback time, and the replay camera that
 *   goes with them.
 *
 *   This header has no includes but the C library's, so the
 *   viewer's own files (which see the viewer's struct
 *   layouts) and the bolo-side file that calls the drawer
 *   (game_view.c, which sees the game's) can both include
 *   it. The tank, builder and shell lists are the viewer's
 *   own screenTanks, screenLgm and screenBullets, carried
 *   as void *: the drawer reads them through the accessors
 *   bolo_shim.c routes to the viewer's, as the game view
 *   already does for its lists.
 *
 *   The map and its lists are rotated before they leave
 *   here, so the drawer never knows the camera turned: a
 *   quarter turn clockwise puts a square (x, y) at
 *   (255 - y, x) and a tank that faced north facing east.
 *********************************************************/

#ifndef LV_ISO_H
#define LV_ISO_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV_ISO_MAP_SIZE   256
#define LV_ISO_ITEMS_MAX  32      /* MAX_PILLS + MAX_BASES */
#define LV_ISO_NAME_LEN   256

/* The square flags, with the values of OVERVIEW_F_* in overview_types.h; the
 * bolo-side file checks the two agree. */
#define LV_ISO_F_MINE   0x01
#define LV_ISO_F_LIVE   0x02
#define LV_ISO_F_SIGHT  0x04

/* World units to a square, the sub-square precision the viewer's positions
 * carry, and the whole map in them. */
#define LV_ISO_WORLD_PER_SQUARE 256
#define LV_ISO_WORLD_MAX        (LV_ISO_MAP_SIZE * LV_ISO_WORLD_PER_SQUARE - 1)

/* A pillbox or base number to draw, on the (rotated) square it stands on. */
typedef struct LvIsoItemLabel {
    unsigned char mapX, mapY;
    unsigned char number;   /* from 0, as the views draw it */
    bool          isBase;
} LvIsoItemLabel;

/* Everything one isometric frame of the replay is drawn from. */
typedef struct LvIsoParts {
    /* The whole map at the playback time, each square's tile as the viewer
       calculates it for its own picture, rotated. Every square is live and in
       sight: a replay has no fog. */
    unsigned char tile[LV_ISO_MAP_SIZE][LV_ISO_MAP_SIZE];   /* [x][y] */
    unsigned char flags[LV_ISO_MAP_SIZE][LV_ISO_MAP_SIZE];  /* [x][y] LV_ISO_F_* */

    /* The viewer's lists in map coordinates (not the classic view's), rotated.
       They belong to lv_iso.c and are good until the next lv_isoBuild. */
    void *tanks;     /* screenTanks * */
    void *lgms;      /* screenLgm * */
    void *bullets;   /* screenBullets * */

    LvIsoItemLabel items[LV_ISO_ITEMS_MAX];
    int            itemCount;

    /* The camera: the rotated world position at the middle of the picture,
       and screen pixels per game pixel. */
    float camWX, camWY, camScale;
    int   rotation;        /* quarter turns clockwise, 0..3 */
    int   followSlot;      /* the tank followed, or -1 for the map's centre */

    /* The tanks before rotation, for the overlays the viewer draws over the
       blit: whole-map pixels of each sprite's top left (16 to a square), as
       the classic view records them, and whose tank it is. */
    struct {
        int           x, y;
        unsigned char slot;
        char          name[LV_ISO_NAME_LEN];
    } tankPos[16];
    int tankPosCount;
} LvIsoParts;

/* The mode switch and the rotation, kept here so the viewer's draw code and
 * the tests read one place. The mode is off at start and not saved: the
 * game view it sits beside is not saved either. */
bool lv_isoEnabled(void);
void lv_isoSetEnabled(bool on);
int  lv_isoRotation(void);
void lv_isoSetRotation(int quarterTurns);   /* wrapped into 0..3 */

/* The one parts block the viewer draws from. */
LvIsoParts *lv_isoParts(void);

/* Rotation sums, all pure. A world position is 0..LV_ISO_WORLD_MAX on each
 * axis; a square 0..255; a tank frame is a classic tank sprite number
 * (TANK_SELF_0 .. TANK_EVILBOAT_15), turned four facings per quarter turn.
 * lv_isoRotateWorldF takes fractional world units, for a camera centre. */
void lv_isoRotateWorld(int rotation, int wx, int wy, int *outX, int *outY);
void lv_isoRotateWorldF(int rotation, float wx, float wy, float *outX, float *outY);
void lv_isoRotateSquare(int rotation, int mx, int my, int *outX, int *outY);
int  lv_isoRotateTankFrame(int rotation, int frame);
int  lv_isoRotateShellFrame(int rotation, int frame);

/* The camera's scale for a zoom step: the step times LV_ISO_SCALE_PER_STEP,
 * so the 1.0 step draws a square's texels at the classic picture's size (a
 * 32 by 16 diamond holds a 16 by 16 tile's texels). */
#define LV_ISO_SCALE_PER_STEP 1.0f
float lv_isoScaleForZoom(float zoomStep);

/* Fill `out` from the viewer's state at the playback time: the map, the
 * lists, the item labels and the camera, all rotated by lv_isoRotation. The
 * camera follows the selected tank when one is selected and in the game, else
 * it sits on the map's centre. Hold the client mutex. */
void lv_isoBuild(LvIsoParts *out, float camScale);

/* Where the point `height` game pixels above the unrotated map position
 * (mapX, mapY), in fractional squares, lands on a window where the picture
 * is blitted at (rectX, rectY), rectW by rectH pixels: the rotation and then
 * isoSquareToWindow. */
void lv_isoMapToWindow(const LvIsoParts *p, float rectX, float rectY,
                       int rectW, int rectH, float mapX, float mapY,
                       float height, float *outWinX, float *outWinY);

#ifdef __cplusplus
}
#endif

#endif /* LV_ISO_H */
