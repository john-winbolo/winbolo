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
 * Name:          iso_project.c
 * Purpose:
 *   The isometric view's projection and its inverse, the
 *   square range a target can show, the draw order and the
 *   facing frame of a stand-in sprite. See iso_project.h for
 *   the camera and the axes. Nothing here touches SDL or the
 *   sim: every function is arithmetic over its arguments,
 *   which is what lets the unit tests link this file.
 *********************************************************/

#include "iso_project.h"

#include <math.h>

/* The map is 256 squares on a side. Spelled here rather than taken from
   the sim's headers so this file links with nothing behind it. */
#define ISO_MAP_SQUARES 256

float isoDiamondHalfWidth(float scale) {
    return (float)(ISO_DIAMOND_W / 2) * scale;
}

/* The sums are done in double. The screen position of a point on the far
   side of the map is tens of thousands of pixels from the origin, and the
   inverse has to land back on the same world unit after a subtraction of
   two such numbers; float carries that only to a few hundredths. */

/* Ground axes in squares to screen pixels, before the height. */
static void isoGroundToScreen(double scale, double squaresX, double squaresY,
                              double *outSX, double *outSY) {
    double t = (double)(ISO_DIAMOND_W / 2) * scale;
    *outSX = (squaresX - squaresY) * t;
    *outSY = (squaresX + squaresY) * t * 0.5;
}

void isoProjectWorld(float scale, float wx, float wy, float height,
                     float *outSX, float *outSY) {
    double sx, sy;
    isoGroundToScreen((double)scale,
                      (double)wx / ISO_WORLD_PER_SQUARE,
                      (double)wy / ISO_WORLD_PER_SQUARE, &sx, &sy);
    *outSX = (float)sx;
    *outSY = (float)(sy - (double)height * (double)scale);
}

/* Where the camera's centre sits on the ground axes, in squares. */
static void isoCameraCentreSquares(const IsoCamera *cam, double *outX, double *outY) {
    *outX = (double)cam->centerWX / ISO_WORLD_PER_SQUARE;
    *outY = (double)cam->centerWY / ISO_WORLD_PER_SQUARE;
}

void isoProject(const IsoCamera *cam, int targetW, int targetH,
                int mapX, int mapY, int worldOffX, int worldOffY,
                float height, float *outSX, float *outSY) {
    double cx, cy, csx, csy, px, py, sx, sy;
    isoCameraCentreSquares(cam, &cx, &cy);
    isoGroundToScreen((double)cam->scale, cx, cy, &csx, &csy);
    px = (double)mapX + (double)worldOffX / ISO_WORLD_PER_SQUARE;
    py = (double)mapY + (double)worldOffY / ISO_WORLD_PER_SQUARE;
    isoGroundToScreen((double)cam->scale, px, py, &sx, &sy);
    *outSX = (float)((double)targetW * 0.5 + (sx - csx));
    *outSY = (float)((double)targetH * 0.5 + (sy - csy) -
                     (double)height * (double)cam->scale);
}

/* The ground axes, in squares, under a point of the target. */
static void isoScreenToSquares(const IsoCamera *cam, int targetW, int targetH,
                               double sx, double sy, double *outX, double *outY) {
    /* A scale of zero or less has no inverse; read it as 1 rather than
       divide by it. */
    double scale = (cam->scale > 0.0f) ? (double)cam->scale : 1.0;
    double t = (double)(ISO_DIAMOND_W / 2) * scale;
    double cx, cy;
    /* Offsets from the target centre, in half-widths along each axis:
       u = x - y and v = x + y. */
    double u = (sx - (double)targetW * 0.5) / t;
    double v = (sy - (double)targetH * 0.5) * 2.0 / t;
    isoCameraCentreSquares(cam, &cx, &cy);
    *outX = cx + (u + v) * 0.5;
    *outY = cy + (v - u) * 0.5;
}

bool isoUnproject(const IsoCamera *cam, int targetW, int targetH,
                  float sx, float sy, float *outWX, float *outWY) {
    double px, py;
    isoScreenToSquares(cam, targetW, targetH, (double)sx, (double)sy, &px, &py);
    *outWX = (float)(px * ISO_WORLD_PER_SQUARE);
    *outWY = (float)(py * ISO_WORLD_PER_SQUARE);
    /* On the map is judged on the nearest world unit, as isoUnprojectSquare
       splits it, so the map's first unit is on it however the rounding of
       the sums above fell. */
    {
        double ux = floor(px * ISO_WORLD_PER_SQUARE + 0.5);
        double uy = floor(py * ISO_WORLD_PER_SQUARE + 0.5);
        double edge = (double)(ISO_MAP_SQUARES * ISO_WORLD_PER_SQUARE);
        return ux >= 0.0 && ux < edge && uy >= 0.0 && uy < edge;
    }
}

/* One axis of isoUnprojectSquare: a position in squares split into the
   square and the 0..255 offset inside it, clamped to the map's edge. The
   position is rounded to the nearest world unit first, so a screen point
   that projects from a world unit comes back as that unit and not the one
   below it by a rounding error. */
static bool isoSplitSquare(double pos, int *outSquare, int *outOff) {
    double units = floor(pos * ISO_WORLD_PER_SQUARE + 0.5);
    if (units < 0.0) {
        *outSquare = 0;
        *outOff = 0;
        return false;
    }
    if (units >= (double)(ISO_MAP_SQUARES * ISO_WORLD_PER_SQUARE)) {
        *outSquare = ISO_MAP_SQUARES - 1;
        *outOff = ISO_WORLD_PER_SQUARE - 1;
        return false;
    }
    *outSquare = (int)(units / ISO_WORLD_PER_SQUARE);
    *outOff = (int)units - *outSquare * ISO_WORLD_PER_SQUARE;
    return true;
}

bool isoUnprojectSquare(const IsoCamera *cam, int targetW, int targetH,
                        float sx, float sy, int *outMapX, int *outMapY,
                        int *outWorldOffX, int *outWorldOffY) {
    double px, py;
    bool inX, inY;
    isoScreenToSquares(cam, targetW, targetH, (double)sx, (double)sy, &px, &py);
    inX = isoSplitSquare(px, outMapX, outWorldOffX);
    inY = isoSplitSquare(py, outMapY, outWorldOffY);
    return inX && inY;
}

/* The squares a run from `lo` to `hi` on one ground axis touches, inclusive:
   from the square whose far edge reaches lo to the square whose near edge
   reaches hi. Clamped to the map; false when none of the map is in it. */
static bool isoAxisRange(double lo, double hi, int *outMin, int *outMax) {
    double first = ceil(lo) - 1.0;
    double last = floor(hi);
    if (last < 0.0 || first > (double)(ISO_MAP_SQUARES - 1)) {
        *outMin = 0;
        *outMax = -1;
        return false;
    }
    if (first < 0.0) first = 0.0;
    if (last > (double)(ISO_MAP_SQUARES - 1)) last = (double)(ISO_MAP_SQUARES - 1);
    *outMin = (int)first;
    *outMax = (int)last;
    return true;
}

bool isoVisibleRange(const IsoCamera *cam, int targetW, int targetH,
                     int *outMinX, int *outMinY, int *outMaxX, int *outMaxY) {
    /* The target's corners in screen order: top-left, top-right,
       bottom-left, bottom-right. The ground axes run diagonally across the
       screen, so the extremes along each are at these corners. */
    static const double kCorner[4][2] = {
        { 0.0, 0.0 }, { 1.0, 0.0 }, { 0.0, 1.0 }, { 1.0, 1.0 }
    };
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
    bool okX, okY;
    int i;
    for (i = 0; i < 4; i++) {
        double px, py;
        isoScreenToSquares(cam, targetW, targetH,
                           kCorner[i][0] * (double)targetW,
                           kCorner[i][1] * (double)targetH, &px, &py);
        if (i == 0 || px < minX) minX = px;
        if (i == 0 || px > maxX) maxX = px;
        if (i == 0 || py < minY) minY = py;
        if (i == 0 || py > maxY) maxY = py;
    }
    okX = isoAxisRange(minX, maxX, outMinX, outMaxX);
    okY = isoAxisRange(minY, maxY, outMinY, outMaxY);
    if (!okX || !okY) {
        *outMinX = 0;
        *outMaxX = -1;
        *outMinY = 0;
        *outMaxY = -1;
        return false;
    }
    return true;
}

int isoDepth(int mapX, int mapY, IsoDepthKind kind) {
    /* Ground keys run 0..510 by row; everything standing starts past them,
       two keys per row so a row's tall tiles come before its sprites. */
    int row = mapX + mapY;
    if (kind == ISO_DEPTH_GROUND) return row;
    return 2 * ISO_MAP_SQUARES + row * 2 + (kind == ISO_DEPTH_SPRITE ? 1 : 0);
}

int isoFacingFrame(int frame, bool isoSprite) {
    if (isoSprite) return frame;
    return (frame + 2) & 15;
}
