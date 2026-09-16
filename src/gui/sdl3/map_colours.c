/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Name:          map_colours.c
 * Purpose:       See map_colours.h.
 */

#include "map_colours.h"

#include "global.h"    /* the plain terrains, 0..8 */
#include "tilenum.h"   /* the shape-variant ranges */

MapColourItem mapColourItemKind(BYTE tile) {
    if (tile == BASE_GOOD)    return MAP_COLOUR_ITEM_BASE_GOOD;
    if (tile == BASE_NEUTRAL) return MAP_COLOUR_ITEM_BASE_NEUTRAL;
    if (tile == BASE_EVIL)    return MAP_COLOUR_ITEM_BASE_EVIL;
    if (tile >= PILL_GOOD_15 && tile <= PILL_GOOD_0) {
        return MAP_COLOUR_ITEM_PILL_GOOD;
    }
    if (tile == PILL_EVIL_15 || (tile >= PILL_EVIL_14 && tile <= PILL_EVIL_0)) {
        return MAP_COLOUR_ITEM_PILL_EVIL;
    }
    return MAP_COLOUR_ITEM_NONE;
}

/* One entry per terrain family. Named, because a family is reachable by two
   different numbers - the raw terrain a map file holds, and the shape variant
   a drawer picks for it - and the two have to be the same colour. */
#define RGB_ROAD          0x000000u
#define RGB_BUILDING      0x785e41u
#define RGB_HALFBUILDING  0x56422cu
#define RGB_RIVER         0x008c9cu
#define RGB_DEEP_SEA      0x008a9eu
#define RGB_BOAT          0x61848bu
#define RGB_FOREST        0x045311u
#define RGB_CRATER        0x292911u
#define RGB_SWAMP         0x003933u
#define RGB_RUBBLE        0x303819u
#define RGB_GRASS         0x002806u

bool mapColourTerrain(BYTE tile, SDL_Color *out) {
    Uint32 rgb;
    MapColourItem item;

    if (!out) return false;

    /* A mine is drawn over the ground, not instead of it, so a mined square
       answers with the ground and the caller marks the mine itself. The six
       mine values are raw terrain only: no drawer emits one. */
    if (tile >= MINE_START && tile <= MINE_END) {
        tile = (BYTE)(tile - MINE_SUBTRACT);
    }

    item = mapColourItemKind(tile);

    /* An item square answers with the ground under it: road under a base,
       grass under a pill. The marker goes on top afterwards. */
    if (item >= MAP_COLOUR_ITEM_BASE_GOOD) {
        rgb = RGB_ROAD;
    } else if (item != MAP_COLOUR_ITEM_NONE) {
        rgb = RGB_GRASS;
    } else if (tile >= ROAD_HORZ && tile <= ROAD_SIDE4) {
        rgb = RGB_ROAD;
    } else if (tile >= BUILD_SINGLE && tile <= BUILD_MOST4) {
        rgb = RGB_BUILDING;
    } else if (tile >= RIVER_END1 && tile <= RIVER_CORN4) {
        rgb = RGB_RIVER;
    } else if (tile >= DEEP_SEA_SOLID && tile <= DEEP_SEA_SIDE4) {
        rgb = RGB_DEEP_SEA;
    } else if (tile >= FOREST_SINGLE && tile <= FOREST_RIGHT) {
        rgb = RGB_FOREST;
    } else if (tile >= CRATER_SINGLE && tile <= CRATER_RIGHT) {
        rgb = RGB_CRATER;
    } else if (tile >= BOAT_0 && tile <= BOAT_8) {
        rgb = RGB_BOAT;
    } else {
        /* The raw terrains. Four of them reach a drawer as themselves; the
           rest only ever arrive from a caller reading a map file, which is
           what the map choosers do. None of these numbers is also a drawn
           tile, so covering both costs nothing in ambiguity. */
        switch (tile) {
            case BUILDING:     rgb = RGB_BUILDING;     break;
            case RIVER:        rgb = RGB_RIVER;        break;
            case SWAMP:        rgb = RGB_SWAMP;        break;
            case CRATER:       rgb = RGB_CRATER;       break;
            case ROAD:         rgb = RGB_ROAD;         break;
            case FOREST:       rgb = RGB_FOREST;       break;
            case RUBBLE:       rgb = RGB_RUBBLE;       break;
            case GRASS:        rgb = RGB_GRASS;        break;
            case HALFBUILDING: rgb = RGB_HALFBUILDING; break;
            case BOAT:         rgb = RGB_BOAT;         break;
            case DEEP_SEA:     rgb = RGB_DEEP_SEA;     break;
            default: return false;
        }
    }

    out->r = (Uint8)(rgb >> 16);
    out->g = (Uint8)(rgb >> 8);
    out->b = (Uint8)rgb;
    out->a = 255;
    return true;
}

SDL_FColor mapColourMarkerGood(void) {
    SDL_FColor c = { 88 / 255.0f, 216 / 255.0f, 88 / 255.0f, 1.0f };
    return c;
}

SDL_FColor mapColourMarkerEvil(void) {
    SDL_FColor c = { 255 / 255.0f, 93 / 255.0f, 93 / 255.0f, 1.0f };
    return c;
}

SDL_FColor mapColourMarkerNeutral(void) {
    SDL_FColor c = { 240 / 255.0f, 180 / 255.0f, 41 / 255.0f, 1.0f };
    return c;
}

void mapColourMarkerShades(SDL_FColor fill, SDL_FColor out[3]) {
    if (!out) return;
    out[0].r = 0.0f;
    out[0].g = 0.0f;
    out[0].b = 0.0f;
    out[0].a = MAP_COLOUR_OUTLINE_ALPHA;
    out[1].r = fill.r * MAP_COLOUR_DARKEN;
    out[1].g = fill.g * MAP_COLOUR_DARKEN;
    out[1].b = fill.b * MAP_COLOUR_DARKEN;
    out[1].a = fill.a;
    out[2] = fill;
}

float mapColourMarkerLayerGrow(int layer) {
    if (layer < 0) layer = 0;
    if (layer > 2) layer = 2;
    return MAP_COLOUR_STROKE_PX * 0.5f * (float)(1 - layer);
}
