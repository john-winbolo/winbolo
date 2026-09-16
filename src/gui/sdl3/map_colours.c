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

bool mapColourTerrain(BYTE tile, SDL_Color *out) {
    Uint32 rgb;
    MapColourItem item = mapColourItemKind(tile);

    if (!out) return false;

    /* An item square answers with the ground under it: road under a base,
       grass under a pill. The marker goes on top afterwards. */
    if (item >= MAP_COLOUR_ITEM_BASE_GOOD) {
        rgb = 0x000000;
    } else if (item != MAP_COLOUR_ITEM_NONE) {
        rgb = 0x002806;
    } else if (tile >= ROAD_HORZ && tile <= ROAD_SIDE4) {
        rgb = 0x000000;
    } else if (tile >= BUILD_SINGLE && tile <= BUILD_MOST4) {
        rgb = 0x785e41;
    } else if (tile >= RIVER_END1 && tile <= RIVER_CORN4) {
        rgb = 0x008c9c;
    } else if (tile >= DEEP_SEA_SOLID && tile <= DEEP_SEA_SIDE4) {
        rgb = 0x008a9e;
    } else if (tile == FOREST || (tile >= FOREST_SINGLE && tile <= FOREST_RIGHT)) {
        rgb = 0x045311;
    } else if (tile == CRATER || (tile >= CRATER_SINGLE && tile <= CRATER_RIGHT)) {
        rgb = 0x292911;
    } else if (tile >= BOAT_0 && tile <= BOAT_8) {
        rgb = 0x61848b;
    } else {
        switch (tile) {
            case SWAMP:        rgb = 0x003933; break;
            case RUBBLE:       rgb = 0x303819; break;
            case GRASS:        rgb = 0x002806; break;
            case HALFBUILDING: rgb = 0x56422c; break;
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
