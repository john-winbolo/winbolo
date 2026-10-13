/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The sums of the ring the log viewer flashes on the map round the place of
 * an event the user went to (a click on a line of the Events panel or on a
 * marker over the seek bar).
 *
 * The ring starts LV_FLASH_RADIUS_TILES map squares out from the middle of the
 * event's square, grows by LV_FLASH_GROW_TILES and fades away, all inside
 * LV_FLASH_MS of the clock on the wall: it runs the same with playback
 * stopped. It is full strength for the first quarter of that time, so it is
 * seen before it starts to go.
 *
 * Also here: whether the map has to move to show the square at all.
 *
 * Header only, no SDL and no ImGui: the unit tests include it.
 */

#ifndef LV_EVENT_FLASH_H
#define LV_EVENT_FLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How long the ring is on the map, in ms of real time. */
#define LV_FLASH_MS 1200u
/* The part of that time, from its start, the ring is at full strength. */
#define LV_FLASH_HOLD 0.25f
/* The radius at the start, and how much it grows, in map squares. */
#define LV_FLASH_RADIUS_TILES 1.5f
#define LV_FLASH_GROW_TILES 0.5f
/* The width of the ring, in points: white alone, or a white band outside a
   band in the team's colour. */
#define LV_FLASH_RING_PT 3.0f
#define LV_FLASH_WHITE_PT 2.0f
#define LV_FLASH_TEAM_PT 2.0f
/* The map moves to the event when its square is nearer than this many
   squares to an edge of the view, or outside it. */
#define LV_FLASH_EDGE_TILES 2

/* 1 while the ring is on the map: from its start up to, not at, its end. */
static inline int lvFlashActive(uint32_t elapsedMs) {
    return elapsedMs < LV_FLASH_MS;
}

/* How far through its time the ring is, 0 to 1. */
static inline float lvFlashProgress(uint32_t elapsedMs) {
    if (elapsedMs >= LV_FLASH_MS) return 1.0f;
    return (float)elapsedMs / (float)LV_FLASH_MS;
}

/* How solid the ring is, 1 to 0: full for the first quarter, then a straight
   line down to nothing at the end. */
static inline float lvFlashAlpha(uint32_t elapsedMs) {
    float u = lvFlashProgress(elapsedMs);
    if (u <= LV_FLASH_HOLD) return 1.0f;
    return (1.0f - u) / (1.0f - LV_FLASH_HOLD);
}

/* The radius to the middle of the ring, in map squares. It grows fast at
   first and slows to a stop. */
static inline float lvFlashRadiusTiles(uint32_t elapsedMs) {
    float u = lvFlashProgress(elapsedMs);
    float eased = 1.0f - (1.0f - u) * (1.0f - u);
    return LV_FLASH_RADIUS_TILES + LV_FLASH_GROW_TILES * eased;
}

/* Whether the view has to move, on one axis, to show a map square: the view
   starts at square `offset` and is `size` squares long. 1 when the square is
   outside the view or nearer than `edge` squares to either end of it. */
static inline int lvFlashAxisNeedsCentre(int cell, int offset, int size, int edge) {
    if (size <= 0) return 0;
    if (cell < offset + edge) return 1;
    if (cell > offset + size - 1 - edge) return 1;
    return 0;
}

/* The same for both axes. */
static inline int lvFlashNeedsCentre(int cellX, int cellY, int offX, int offY,
                                     int sizeX, int sizeY, int edge) {
    return lvFlashAxisNeedsCentre(cellX, offX, sizeX, edge) ||
           lvFlashAxisNeedsCentre(cellY, offY, sizeY, edge);
}

#ifdef __cplusplus
}
#endif

#endif /* LV_EVENT_FLASH_H */
