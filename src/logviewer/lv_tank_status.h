/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer Tank panel sums
 *Filename:      lv_tank_status.h
 *Purpose:
 *  What the Log Viewer's Tank panel (imgui/imgui_tank.cpp)
 *  shows of the selected tank, and how tall each of its four
 *  gauges is drawn.
 *
 *  The gauges are the game's: the four bars of the status
 *  column (shells, mines, armour, trees), each filled against
 *  its own cap out of the game's rules.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer (C and C++) and the unit tests use the same sums.
 *********************************************************/

#ifndef LV_TANK_STATUS_H
#define LV_TANK_STATUS_H

#include <stdbool.h>
#include <stddef.h>

/* The longest player name the snapshot carries, with its NUL. */
#define LV_TANK_STATUS_NAME_LEN 64

/* The four gauges, in the order the game draws them from the left. */
enum {
    LV_TANK_GAUGE_SHELLS = 0,
    LV_TANK_GAUGE_MINES,
    LV_TANK_GAUGE_ARMOUR,
    LV_TANK_GAUGE_TREES,
    LV_TANK_GAUGE_COUNT
};

/* Everything the panel draws in one frame. */
typedef struct LvTankStatus {
    unsigned char selected;   /* 0: no tank is selected; the rest is unset */
    unsigned char slot;       /* the player's slot */
    unsigned char hasTeam;    /* 0 when the player is in no team */
    unsigned char colour;     /* the palette entry the team is drawn in */
    unsigned char alive;      /* 0 while the tank is dead or not yet on the map */
    unsigned char hasStock;   /* 0: the recording holds no tank resources */
    unsigned char value[LV_TANK_GAUGE_COUNT]; /* what the tank holds now */
    unsigned char full[LV_TANK_GAUGE_COUNT];  /* the most it can hold */
    unsigned char builderOut;  /* the builder is out of the tank */
    unsigned char builderDead; /* the builder is dead (a new one is on its way) */
    unsigned char carriedPills; /* pillboxes the tank carries */
    char name[LV_TANK_STATUS_NAME_LEN];
} LvTankStatus;

/* The tank the panel shows: the viewer's "self" player, when that slot has
 * a player in it. "Self" is the one selection of a tank the viewer has: a
 * click on a tank in the HUD panel picks it, as does a Select Team click on
 * a tank on the map, and Tank Centred follows it. Returns the slot, or -1
 * for no tank. */
static inline int lvTankStatusSelected(int selfSlot, int maxTanks, bool selfInUse) {
    if (selfSlot < 0 || selfSlot >= maxTanks || !selfInUse) return -1;
    return selfSlot;
}

/* The height of a gauge in the game's pixels, as the game works it out
 * (sdl3RenderTankBarsTex): fullPixels for a tank that holds its cap, and in
 * proportion under that. A cap of 0 cannot be divided by and draws empty. A
 * value over its cap draws taller, up to the height of the gauge's well
 * (wellPixels), where the game's drawing is cut off too. */
static inline float lvTankGaugeHeight(int value, int full, int fullPixels,
                                      int wellPixels) {
    float h;
    if (full <= 0 || value <= 0) return 0.0f;
    h = (float)fullPixels / (float)full * (float)value;
    if (h > (float)wellPixels) h = (float)wellPixels;
    return h;
}

#endif /* LV_TANK_STATUS_H */
