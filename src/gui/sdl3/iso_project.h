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
 * Name:          iso_project.h
 * Purpose:
 *   The isometric view's projection: where a map position
 *   lands on screen, which map position a screen point is
 *   over, which squares a view of a given size can show, and
 *   the order things on the map are drawn in so that near
 *   ones cover far ones. Plain arithmetic over its arguments,
 *   with no renderer and no sim behind it, so the unit tests
 *   link it directly and every drawer that places something
 *   on the isometric ground calls the same sums.
 *
 *   The camera is dimetric 2:1. World north (-y) is screen
 *   up-right, world east (+x) is screen down-right. A map
 *   square is a diamond ISO_DIAMOND_W wide and ISO_DIAMOND_H
 *   tall at scale 1, and the four corners of square (x, y)
 *   project to
 *     top    (x,     y    )
 *     right  (x + 1, y    )
 *     bottom (x + 1, y + 1)
 *     left   (x,     y + 1)
 *   Height is in game pixels and moves a point straight up
 *   the screen by that many pixels times the scale.
 *********************************************************/

#ifndef ISO_PROJECT_H
#define ISO_PROJECT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ground diamond of one map square, in screen pixels at scale 1. The
   half-width is also how far one square moves a point along each of the
   two ground axes: T = ISO_DIAMOND_W / 2, and a square east adds (T, T/2)
   while a square south adds (-T, T/2). */
#define ISO_DIAMOND_W 32
#define ISO_DIAMOND_H 16

/* World units per map square, the sub-square precision the sim reports
   positions in and the one MapViewPreciseCam's centre is held in. */
#define ISO_WORLD_PER_SQUARE 256

/* The isometric camera. The centre is the map position that lands at the
   middle of the target, in fractional world units (256 per square) so a
   position between two sim ticks can be handed in exactly as it is for
   MapViewPreciseCam. The scale is screen pixels per game pixel, the same
   quantity the classic view's zoom and the overview's ladder express, so a
   scale of 1 draws a square's diamond ISO_DIAMOND_W by ISO_DIAMOND_H. */
typedef struct IsoCamera {
    float centerWX, centerWY;
    float scale;
} IsoCamera;

/* Half the width of a square's diamond at this scale, in screen pixels. */
float isoDiamondHalfWidth(float scale);

/* Screen offset of a world position from the map origin's projection, at
   `scale`, with no camera applied. wx/wy are fractional world units (256
   per square) and height is in game pixels; a positive height moves the
   point up the screen. The camera-relative call below is this plus the
   centring; it is exposed so a caller can project differences. */
void isoProjectWorld(float scale, float wx, float wy, float height,
                     float *outSX, float *outSY);

/* Screen position, relative to the top-left of a target targetW by targetH
   pixels, of map square (mapX, mapY) plus a 0..255 world offset inside it,
   raised `height` game pixels off the ground. The camera's centre lands at
   the middle of the target. The result is a float screen position; the
   caller decides whether to snap it. mapX/mapY are not clamped: a square
   off the map projects to where it would be. */
void isoProject(const IsoCamera *cam, int targetW, int targetH,
                int mapX, int mapY, int worldOffX, int worldOffY,
                float height, float *outSX, float *outSY);

/* The map position on the ground plane under screen point (sx, sy) of the
   target, in fractional world units. Returns whether that position is on
   the 256x256 map; the position comes back either way, so a caller that
   wants the nearest map square can clamp it. This undoes isoProject at
   height 0. */
bool isoUnproject(const IsoCamera *cam, int targetW, int targetH,
                  float sx, float sy, float *outWX, float *outWY);

/* isoUnproject split into the map square and the 0..255 world offset the
   sim speaks in. Returns whether the square is on the map; off it the
   outputs are the clamped nearest square and its edge offset. */
bool isoUnprojectSquare(const IsoCamera *cam, int targetW, int targetH,
                        float sx, float sy, int *outMapX, int *outMapY,
                        int *outWorldOffX, int *outWorldOffY);

/* The inclusive range of map squares whose ground diamond can touch a
   target of targetW by targetH pixels under this camera, clamped to the
   map. It is the bounding range of the squares under the target's four
   corners, so it holds every square the target shows and a few off its
   corners; it is an iteration bound, not a hit test. Returns false, with
   min above max, when no square of the map reaches the target. Tall tiles
   stand above their diamond, so a drawer that wants the roofs of squares
   just below the bottom edge widens the range it gets back: a roof of
   height h game pixels can reach up h * scale screen pixels, which is
   h * scale / (8 * scale) = h / 8 squares along each ground axis at the
   bottom edge, so add ceil(h / 8) to both outMax values. Growing targetH
   instead moves the centre as well and gains only half of that. */
bool isoVisibleRange(const IsoCamera *cam, int targetW, int targetH,
                     int *outMinX, int *outMinY, int *outMaxX, int *outMaxY);

/* What a thing on the map is, for the draw order. */
typedef enum IsoDepthKind {
    ISO_DEPTH_GROUND = 0, /* the square's ground diamond */
    ISO_DEPTH_TALL   = 1, /* a tile that stands up: building, pill, base, forest */
    ISO_DEPTH_SPRITE = 2  /* a tank, builder, shell or explosion on the square */
} IsoDepthKind;

/* The order things are drawn in: lower first. All ground comes before
   anything that stands on it, so the ground plane is laid down whole. Tall
   tiles and sprites then go by their square's x + y, the camera's depth,
   so a square further down the screen draws over the one behind it. On the
   same x + y a sprite draws after the tall tile of its own square, so a tank
   on a square is not hidden under that square's own tile, and before the
   tall tiles of the next row, which stand in front of it. A sprite with a
   world offset is keyed by the square it is on. */
int isoDepth(int mapX, int mapY, IsoDepthKind kind);

/* The 16-step facing frame to draw a sprite with. An isometric sprite is
   drawn in world facing and comes back unchanged. A classic sprite drawn
   upright as a stand-in is turned with the camera: each frame is 22.5
   degrees and the camera has turned 45, so its frame advances by two,
   wrapping at 16. */
int isoFacingFrame(int frame, bool isoSprite);

#ifdef __cplusplus
}
#endif

#endif /* ISO_PROJECT_H */
