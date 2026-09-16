/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Name:          map_colours.c
 * Purpose:       See map_colours.h.
 */

#include "map_colours.h"

#include "global.h"       /* the plain terrains, 0..8 */
#include "tilenum.h"      /* the shape-variant ranges */
#include "skin_source.h"  /* the active skin's [MapPalette] section */

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

/* One entry per colour this module hands out. A family is reachable by two
   different numbers - the raw terrain a map file holds, and the shape variant
   a drawer picks for it - and the entry is what makes the two the same
   colour. */
typedef enum {
    COLOUR_ROAD = 0,
    COLOUR_BUILDING,
    COLOUR_HALFBUILDING,
    COLOUR_RIVER,
    COLOUR_DEEP_SEA,
    COLOUR_BOAT,
    COLOUR_FOREST,
    COLOUR_CRATER,
    COLOUR_SWAMP,
    COLOUR_RUBBLE,
    COLOUR_GRASS,
    COLOUR_MARKER_GOOD,
    COLOUR_MARKER_EVIL,
    COLOUR_MARKER_NEUTRAL,
    COLOUR_COUNT
} MapColourSlot;

static const Uint32 kBuiltIn[COLOUR_COUNT] = {
    0x000000u,  /* road */
    0x785e41u,  /* building */
    0x56422cu,  /* half building */
    0x008c9cu,  /* river */
    0x008a9eu,  /* deep sea */
    0x61848bu,  /* boat */
    0x045311u,  /* forest */
    0x292911u,  /* crater */
    0x003933u,  /* swamp */
    0x303819u,  /* rubble */
    0x002806u,  /* grass */
    0x58d858u,  /* marker: good */
    0xff5d5du,  /* marker: evil */
    0xf0b429u   /* marker: neutral */
};

/* The built-ins with the active skin's [MapPalette] entries laid over them,
   and the skin that was read to build it.

   Pulled rather than pushed. A skin becomes active in five places - startup
   and four paths through the settings dialog - and a push would be five call
   sites to keep in step, with a stale palette the price of missing one.
   skinSourceSerial is never reused, so comparing it is enough to notice any
   change, including a skin closed and another opened into the same block. */
static Uint32   s_colour[COLOUR_COUNT];
static uint64_t s_serial;
static bool     s_haveColours;

static void colourApply(MapColourSlot slot, int32_t rgb) {
    if (rgb != SKIN_COLOUR_NONE) {
        s_colour[slot] = (Uint32)rgb & 0xffffffu;
    }
}

static Uint32 colourOf(MapColourSlot slot) {
    SkinSource *src = skinGetActiveSource();
    uint64_t    serial = (src != NULL) ? skinSourceSerial(src) : 0;

    if (!s_haveColours || serial != s_serial) {
        SkinInfo info;
        int      i;

        for (i = 0; i < COLOUR_COUNT; i++) s_colour[i] = kBuiltIn[i];

        /* No skin is the built-in assets, and skinSourceReadIni on a source
           with no ini answers with an empty SkinInfo, so both leave every
           entry as it was. */
        if (src != NULL) {
            skinSourceReadIni(src, &info);
            colourApply(COLOUR_ROAD,            info.mapPalette.road);
            colourApply(COLOUR_BUILDING,        info.mapPalette.building);
            colourApply(COLOUR_HALFBUILDING,    info.mapPalette.halfBuilding);
            colourApply(COLOUR_RIVER,           info.mapPalette.river);
            colourApply(COLOUR_DEEP_SEA,        info.mapPalette.deepSea);
            colourApply(COLOUR_BOAT,            info.mapPalette.boat);
            colourApply(COLOUR_FOREST,          info.mapPalette.forest);
            colourApply(COLOUR_CRATER,          info.mapPalette.crater);
            colourApply(COLOUR_SWAMP,           info.mapPalette.swamp);
            colourApply(COLOUR_RUBBLE,          info.mapPalette.rubble);
            colourApply(COLOUR_GRASS,           info.mapPalette.grass);
            colourApply(COLOUR_MARKER_GOOD,     info.mapPalette.markerGood);
            colourApply(COLOUR_MARKER_EVIL,     info.mapPalette.markerEvil);
            colourApply(COLOUR_MARKER_NEUTRAL,  info.mapPalette.markerNeutral);
        }

        s_serial      = serial;
        s_haveColours = true;
    }
    return s_colour[slot];
}

/* A slot as the float colour the marker drawers take. */
static SDL_FColor markerColour(MapColourSlot slot) {
    Uint32     rgb = colourOf(slot);
    SDL_FColor c;
    c.r = (float)((rgb >> 16) & 0xffu) / 255.0f;
    c.g = (float)((rgb >> 8) & 0xffu) / 255.0f;
    c.b = (float)(rgb & 0xffu) / 255.0f;
    c.a = 1.0f;
    return c;
}

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
        rgb = colourOf(COLOUR_ROAD);
    } else if (item != MAP_COLOUR_ITEM_NONE) {
        rgb = colourOf(COLOUR_GRASS);
    } else if (tile >= ROAD_HORZ && tile <= ROAD_SIDE4) {
        rgb = colourOf(COLOUR_ROAD);
    } else if (tile >= BUILD_SINGLE && tile <= BUILD_MOST4) {
        rgb = colourOf(COLOUR_BUILDING);
    } else if (tile >= RIVER_END1 && tile <= RIVER_CORN4) {
        rgb = colourOf(COLOUR_RIVER);
    } else if (tile >= DEEP_SEA_SOLID && tile <= DEEP_SEA_SIDE4) {
        rgb = colourOf(COLOUR_DEEP_SEA);
    } else if (tile >= FOREST_SINGLE && tile <= FOREST_RIGHT) {
        rgb = colourOf(COLOUR_FOREST);
    } else if (tile >= CRATER_SINGLE && tile <= CRATER_RIGHT) {
        rgb = colourOf(COLOUR_CRATER);
    } else if (tile >= BOAT_0 && tile <= BOAT_8) {
        rgb = colourOf(COLOUR_BOAT);
    } else {
        /* The raw terrains. Four of them reach a drawer as themselves; the
           rest only ever arrive from a caller reading a map file, which is
           what the map choosers do. None of these numbers is also a drawn
           tile, so covering both costs nothing in ambiguity. */
        switch (tile) {
            case BUILDING:     rgb = colourOf(COLOUR_BUILDING);     break;
            case RIVER:        rgb = colourOf(COLOUR_RIVER);        break;
            case SWAMP:        rgb = colourOf(COLOUR_SWAMP);        break;
            case CRATER:       rgb = colourOf(COLOUR_CRATER);       break;
            case ROAD:         rgb = colourOf(COLOUR_ROAD);         break;
            case FOREST:       rgb = colourOf(COLOUR_FOREST);       break;
            case RUBBLE:       rgb = colourOf(COLOUR_RUBBLE);       break;
            case GRASS:        rgb = colourOf(COLOUR_GRASS);        break;
            case HALFBUILDING: rgb = colourOf(COLOUR_HALFBUILDING); break;
            case BOAT:         rgb = colourOf(COLOUR_BOAT);         break;
            case DEEP_SEA:     rgb = colourOf(COLOUR_DEEP_SEA);     break;
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
    return markerColour(COLOUR_MARKER_GOOD);
}

SDL_FColor mapColourMarkerEvil(void) {
    return markerColour(COLOUR_MARKER_EVIL);
}

SDL_FColor mapColourMarkerNeutral(void) {
    return markerColour(COLOUR_MARKER_NEUTRAL);
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
