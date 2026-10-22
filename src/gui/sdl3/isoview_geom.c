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
 * Name:          isoview_geom.c
 * Purpose:
 *   The isometric view's picture as geometry: see
 *   isoview_geom.h. Arithmetic over the snapshot's memory
 *   and lists, the projection in iso_project.c and the
 *   sheet tables in tiles.h; no renderer and no sim.
 *********************************************************/

#include "isoview_geom.h"
#include "sprite_positions.h"  /* mapViewPosX / Y: a tile's place on the sheet */
#include "gfx_settings.h"      /* the GfxAnimSmoothness values */
#include "fog_look.h"          /* FOG_LOOK_ALPHA */
#include "../tiles.h"
#include "tilenum.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Half a square in world units: the distance from a tank's listed top-left
   to its centre, and from the point a man stands on to the bottom vertex of
   the diamond round him. */
#define ISO_HALF_SQUARE (ISO_WORLD_PER_SQUARE / 2)

/* Where each classic tank frame is on the sheet, indexed by the frame as
   the list carries it: 16 facings each of self, self on a boat, good, good
   on a boat, evil and evil on a boat. */
#define TANK_FRAMES_OF(P)                                                     \
    { P##_0_X, P##_0_Y },   { P##_1_X, P##_1_Y },   { P##_2_X, P##_2_Y },     \
    { P##_3_X, P##_3_Y },   { P##_4_X, P##_4_Y },   { P##_5_X, P##_5_Y },     \
    { P##_6_X, P##_6_Y },   { P##_7_X, P##_7_Y },   { P##_8_X, P##_8_Y },     \
    { P##_9_X, P##_9_Y },   { P##_10_X, P##_10_Y }, { P##_11_X, P##_11_Y },   \
    { P##_12_X, P##_12_Y }, { P##_13_X, P##_13_Y }, { P##_14_X, P##_14_Y },   \
    { P##_15_X, P##_15_Y }

static const int kTankSheet[96][2] = {
    TANK_FRAMES_OF(TANK_SELF),
    TANK_FRAMES_OF(TANK_SELFBOAT),
    TANK_FRAMES_OF(TANK_GOOD),
    TANK_FRAMES_OF(TANK_GOODBOAT),
    TANK_FRAMES_OF(TANK_EVIL),
    TANK_FRAMES_OF(TANK_EVILBOAT)
};

/* The shell sprites by direction, each its own size. */
#define SHELL_OF(N) { SHELL_##N##_X, SHELL_##N##_Y, SHELL_##N##_WIDTH, SHELL_##N##_HEIGHT }
static const int kShellSheet[16][4] = {
    SHELL_OF(0),  SHELL_OF(1),  SHELL_OF(2),  SHELL_OF(3),
    SHELL_OF(4),  SHELL_OF(5),  SHELL_OF(6),  SHELL_OF(7),
    SHELL_OF(8),  SHELL_OF(9),  SHELL_OF(10), SHELL_OF(11),
    SHELL_OF(12), SHELL_OF(13), SHELL_OF(14), SHELL_OF(15)
};

/* The explosion frames, indexed by frame - SHELL_EXPLOSION8: the list counts
   them down from 8 as the explosion grows. */
static const int kExplosionSheet[8][2] = {
    { EXPLOSION8_X, EXPLOSION8_Y }, { EXPLOSION7_X, EXPLOSION7_Y },
    { EXPLOSION6_X, EXPLOSION6_Y }, { EXPLOSION5_X, EXPLOSION5_Y },
    { EXPLOSION4_X, EXPLOSION4_Y }, { EXPLOSION3_X, EXPLOSION3_Y },
    { EXPLOSION2_X, EXPLOSION2_Y }, { EXPLOSION1_X, EXPLOSION1_Y }
};

/*********************************************************
 * The arrays.
 *********************************************************/

static bool growArray(void **arr, int *cap, int need, size_t elem) {
    int   newCap;
    void *grown;
    if (need <= *cap) return true;
    newCap = (*cap > 0) ? *cap : 256;
    while (newCap < need) newCap *= 2;
    grown = realloc(*arr, (size_t)newCap * elem);
    if (grown == NULL) return false;
    *arr = grown;
    *cap = newCap;
    return true;
}

void isoGeomSceneInit(IsoGeomScene *s) {
    memset(s, 0, sizeof(*s));
    s->maxX = s->maxY = -1;
}

void isoGeomSceneFree(IsoGeomScene *s) {
    free(s->ground);
    free(s->stand);
    free(s->items);
    isoGeomSceneInit(s);
}

static void sceneClear(IsoGeomScene *s) {
    s->groundCount = 0;
    s->standCount  = 0;
    s->itemCount   = 0;
    memset(&s->overlay, 0, sizeof(s->overlay));
    s->minX = s->minY = 0;
    s->maxX = s->maxY = -1;
}

static IsoGeomQuad *pushGround(IsoGeomScene *s) {
    if (!growArray((void **)&s->ground, &s->groundCap, s->groundCount + 1,
                   sizeof(IsoGeomQuad))) {
        return NULL;
    }
    return &s->ground[s->groundCount++];
}

static IsoGeomQuad *pushStand(IsoGeomScene *s) {
    if (!growArray((void **)&s->stand, &s->standCap, s->standCount + 1,
                   sizeof(IsoGeomQuad))) {
        return NULL;
    }
    return &s->stand[s->standCount++];
}

static IsoGeomItem *pushItem(IsoGeomScene *s) {
    if (!growArray((void **)&s->items, &s->itemCap, s->itemCount + 1,
                   sizeof(IsoGeomItem))) {
        return NULL;
    }
    return &s->items[s->itemCount++];
}

/*********************************************************
 * Classification.
 *********************************************************/

IsoTallKind isoGeomTallKind(BYTE tile) {
    if (tile >= BUILD_SINGLE && tile <= BUILD_MOST4) return ISO_TALL_BUILDING;
    if (tile == HALFBUILDING) return ISO_TALL_SHOT_BUILDING;
    if (tile == PILL_EVIL_15 ||
        (tile >= PILL_EVIL_14 && tile <= PILL_EVIL_0) ||
        (tile >= PILL_GOOD_15 && tile <= PILL_GOOD_0)) {
        return ISO_TALL_PILL;
    }
    if (tile == BASE_GOOD || tile == BASE_NEUTRAL || tile == BASE_EVIL) {
        return ISO_TALL_BASE;
    }
    if (tile >= FOREST_SINGLE && tile <= FOREST_RIGHT) return ISO_TALL_FOREST;
    return ISO_TALL_NONE;
}

int isoGeomTallHeight(IsoTallKind kind) {
    switch (kind) {
        case ISO_TALL_BUILDING:      return ISO_HEIGHT_BUILDING;
        case ISO_TALL_SHOT_BUILDING: return ISO_HEIGHT_SHOT_BUILDING;
        case ISO_TALL_PILL:          return ISO_HEIGHT_PILL;
        case ISO_TALL_BASE:          return ISO_HEIGHT_BASE;
        case ISO_TALL_FOREST:        return ISO_HEIGHT_FOREST;
        default:                     return 0;
    }
}

/* The pillbox a tile draws: whose, and how much armour it has left, which is
   the frame the isometric sheet is indexed by. */
static void pillTeamFrame(BYTE tile, IsoTeam *team, int *frame) {
    if (tile == PILL_EVIL_15) {
        *team = ISO_TEAM_EVIL;
        *frame = 15;
    } else if (tile >= PILL_EVIL_14 && tile <= PILL_EVIL_0) {
        *team = ISO_TEAM_EVIL;
        *frame = 14 - (tile - PILL_EVIL_14);
    } else {
        *team = ISO_TEAM_GOOD;
        *frame = 15 - (tile - PILL_GOOD_15);
    }
}

static IsoTeam baseTeam(BYTE tile) {
    if (tile == BASE_GOOD) return ISO_TEAM_GOOD;
    if (tile == BASE_NEUTRAL) return ISO_TEAM_NEUTRAL;
    return ISO_TEAM_EVIL;
}

static IsoSpriteKind faceKind(IsoTallKind kind) {
    switch (kind) {
        case ISO_TALL_BUILDING:      return ISO_SPRITE_FACE_BUILDING;
        case ISO_TALL_SHOT_BUILDING: return ISO_SPRITE_FACE_SHOT_BUILDING;
        case ISO_TALL_PILL:          return ISO_SPRITE_FACE_PILL;
        case ISO_TALL_BASE:          return ISO_SPRITE_FACE_BASE;
        default:                     return ISO_SPRITE_FOREST_BAND;
    }
}

static IsoTallKind kindAt(const OverviewMap *map, int mx, int my) {
    if (mx < 0 || my < 0 || mx >= MAP_ARRAY_SIZE || my >= MAP_ARRAY_SIZE) {
        return ISO_TALL_NONE;
    }
    return isoGeomTallKind(map->tile[mx][my]);
}

unsigned isoGeomExposedFaces(const OverviewMap *map, int mx, int my) {
    IsoTallKind kind = kindAt(map, mx, my);
    unsigned faces = 0;
    if (kind == ISO_TALL_NONE) return 0;
    if (kindAt(map, mx, my + 1) != kind) faces |= ISO_FACE_SW;
    if (kindAt(map, mx + 1, my) != kind) faces |= ISO_FACE_SE;
    return faces;
}

bool isoGeomTankTeam(int frame, IsoTeam *outTeam, bool *outBoat,
                     int *outFacing) {
    if (frame < TANK_SELF_0 || frame > TANK_EVILBOAT_15) return false;
    switch (frame / 32) {
        case 0:  *outTeam = ISO_TEAM_SELF; break;
        case 1:  *outTeam = ISO_TEAM_GOOD; break;
        default: *outTeam = ISO_TEAM_EVIL; break;
    }
    *outBoat   = (frame % 32) >= TANK_BOAT_ADD;
    *outFacing = frame % 16;
    return true;
}

bool isoGeomVisibleRange(const IsoCamera *cam, int targetW, int targetH,
                         int *outMinX, int *outMinY, int *outMaxX, int *outMaxY) {
    /* ceil(h / 8): a roof of h game pixels reaches h / 8 squares along each
       ground axis past the bottom edge. */
    const int extra = (ISO_HEIGHT_TALLEST + 7) / 8;
    if (!isoVisibleRange(cam, targetW, targetH,
                         outMinX, outMinY, outMaxX, outMaxY)) {
        return false;
    }
    *outMaxX += extra;
    *outMaxY += extra;
    if (*outMaxX > MAP_ARRAY_SIZE - 1) *outMaxX = MAP_ARRAY_SIZE - 1;
    if (*outMaxY > MAP_ARRAY_SIZE - 1) *outMaxY = MAP_ARRAY_SIZE - 1;
    return true;
}

/*********************************************************
 * Placement.
 *********************************************************/

static void project(const IsoGeomInput *in, int mx, int my, int wx, int wy,
                    float height, float *sx, float *sy) {
    isoProject(&in->cam, in->targetW, in->targetH, mx, my, wx, wy, height,
               sx, sy);
}

void isoGeomSquareCentre(const IsoGeomInput *in, int mx, int my, float height,
                         float *outX, float *outY) {
    project(in, mx, my, ISO_HALF_SQUARE, ISO_HALF_SQUARE, height, outX, outY);
}

/* The world offset a sprite is drawn at under an animation mode: Classic
   keeps whole game pixels, so the offset is the pixel's; the two finer modes
   keep the world offset whole. The same split spritePositionOffset makes. */
static int modeOffset(int mode, int worldOff, int pixelOff) {
    if (mode == GFX_ANIM_SMOOTH || mode == GFX_ANIM_MATCH_PIXELATION) {
        return worldOff;
    }
    return pixelOff << 4;
}

/* Match pixelation's second half: the screen position snapped to the size
   of one sheet texel on screen, as spritePositionOffset snaps it. The other
   modes leave the position alone. */
static void modeSnap(const IsoGeomInput *in, int mode, float *sx, float *sy) {
    if (mode != GFX_ANIM_MATCH_PIXELATION) return;
    {
        int   ss   = in->sheetScale < 1 ? 1 : in->sheetScale;
        float step = in->cam.scale / (float)ss;
        if (step <= 0.0f) return;
        *sx = floorf(*sx / step + 0.5f) * step;
        *sy = floorf(*sy / step + 0.5f) * step;
    }
}

/* The ground diamond of square (mx, my) raised `height`, written into a quad
   top, right, bottom, left. */
static void diamondAt(const IsoGeomInput *in, int mx, int my, float height,
                      IsoGeomQuad *q) {
    project(in, mx,     my,     0, 0, height, &q->px[0], &q->py[0]);
    project(in, mx + 1, my,     0, 0, height, &q->px[1], &q->py[1]);
    project(in, mx + 1, my + 1, 0, 0, height, &q->px[2], &q->py[2]);
    project(in, mx,     my + 1, 0, 0, height, &q->px[3], &q->py[3]);
}

/* An upright rect of w by h screen pixels with its top-left at (x, y). */
static void rectAt(float x, float y, float w, float h, IsoGeomQuad *q) {
    q->px[0] = x;     q->py[0] = y;
    q->px[1] = x + w; q->py[1] = y;
    q->px[2] = x + w; q->py[2] = y + h;
    q->px[3] = x;     q->py[3] = y + h;
}

static void setSrc(IsoGeomQuad *q, IsoGeomTex tex, float x, float y,
                   float w, float h) {
    q->tex   = (unsigned char)tex;
    q->src.x = x;
    q->src.y = y;
    q->src.w = w;
    q->src.h = h;
}

static void setColour(IsoGeomQuad *q, int r, int g, int b, int a) {
    q->r = (unsigned char)r;
    q->g = (unsigned char)g;
    q->b = (unsigned char)b;
    q->a = (unsigned char)a;
}

/* A square's fog: whether it is washed, and the vertex colour that does it.
   The wash is fogLookColour's colour at FOG_LOOK_ALPHA, folded into one
   multiplier per channel: white times (255 - alpha) plus the fog colour times
   alpha. For the black looks that is exactly the darkening the classic view's
   wash leaves on a black clear; the grey look comes out as a darkening
   towards grey rather than a lift, since a vertex colour can only take
   brightness away. */
static bool squareFogged(const OverviewMap *map, int mx, int my) {
    BYTE flags = map->flags[mx][my];
    return (flags & OVERVIEW_F_LIVE) == 0 || (flags & OVERVIEW_F_HIDDEN) != 0;
}

static void fogTint(const IsoGeomInput *in, int *r, int *g, int *b) {
    const int a = FOG_LOOK_ALPHA;
    *r = (255 * (255 - a) + in->fogR * a) / 255;
    *g = (255 * (255 - a) + in->fogG * a) / 255;
    *b = (255 * (255 - a) + in->fogB * a) / 255;
}

static void squareTint(const IsoGeomInput *in, int mx, int my,
                       int *r, int *g, int *b) {
    *r = *g = *b = 255;
    if (in->fogWashes && squareFogged(in->map, mx, my)) fogTint(in, r, g, b);
}

/*********************************************************
 * The ground pass.
 *********************************************************/

static bool buildGround(IsoGeomScene *s, const IsoGeomInput *in) {
    const OverviewMap *map = in->map;
    int mx, my;

    for (mx = s->minX; mx <= s->maxX; mx++) {
        for (my = s->minY; my <= s->maxY; my++) {
            BYTE tile  = map->tile[mx][my];
            BYTE flags = map->flags[mx][my];
            int  r, g, b;
            IsoGeomQuad *q;

            if (tile == OVERVIEW_UNSEEN) {
                /* Ground the player has never seen: the fog colour alone,
                   and nothing at all under the None look. */
                if (!in->fogWashes) continue;
                q = pushGround(s);
                if (q == NULL) return false;
                diamondAt(in, mx, my, 0.0f, q);
                setSrc(q, ISO_GEOM_TEX_NONE, 0, 0, 0, 0);
                setColour(q, in->fogR, in->fogG, in->fogB, 255);
                q->mapX = (short)mx;
                q->mapY = (short)my;
                continue;
            }

            squareTint(in, mx, my, &r, &g, &b);

            q = pushGround(s);
            if (q == NULL) return false;
            diamondAt(in, mx, my, 0.0f, q);
            setSrc(q, ISO_GEOM_TEX_CLASSIC, (float)mapViewPosX[tile],
                   (float)mapViewPosY[tile], TILE_SIZE_X, TILE_SIZE_Y);
            setColour(q, r, g, b, 255);
            q->mapX = (short)mx;
            q->mapY = (short)my;

            if ((flags & OVERVIEW_F_MINE) != 0) {
                /* The mine over its square, as the game draws it. */
                IsoGeomQuad *m = pushGround(s);
                if (m == NULL) return false;
                *m = s->ground[s->groundCount - 2];
                setSrc(m, ISO_GEOM_TEX_CLASSIC, MINE_X, MINE_Y,
                       TILE_SIZE_X, TILE_SIZE_Y);
            }
        }
    }
    return true;
}

/*********************************************************
 * Tall tiles.
 *********************************************************/

/* The ground shadow: the footprint moved down and right by half the height
   along the east axis, so it falls away from the light. */
static bool pushTallShadow(IsoGeomScene *s, const IsoGeomInput *in,
                           int mx, int my, int height) {
    IsoGeomQuad *q = pushStand(s);
    float dx = (float)height * in->cam.scale * 0.5f;
    float dy = (float)height * in->cam.scale * 0.25f;
    int i;
    if (q == NULL) return false;
    diamondAt(in, mx, my, 0.0f, q);
    for (i = 0; i < 4; i++) {
        q->px[i] += dx;
        q->py[i] += dy;
    }
    setSrc(q, ISO_GEOM_TEX_NONE, 0, 0, 0, 0);
    setColour(q, 0, 0, 0, ISO_SHADOW_TALL_ALPHA);
    q->mapX = (short)mx;
    q->mapY = (short)my;
    return true;
}

/* One face: the wall between two ground vertices and the same two raised by
   the height, textured from the sheet's face cell. */
static bool pushFace(IsoGeomScene *s, const IsoGeomInput *in,
                     int mx, int my, int height, unsigned which,
                     const IsoGeomRect *src, int shade,
                     int r, int g, int b) {
    IsoGeomQuad ground, roof;
    IsoGeomQuad *q;
    int a, bIdx;

    diamondAt(in, mx, my, 0.0f, &ground);
    diamondAt(in, mx, my, (float)height, &roof);
    /* The south-west face runs left vertex to bottom; the south-east runs
       bottom to right. Both read left to right on screen. */
    if (which == ISO_FACE_SW) { a = 3; bIdx = 2; }
    else                      { a = 2; bIdx = 1; }

    q = pushStand(s);
    if (q == NULL) return false;
    q->px[0] = roof.px[a];      q->py[0] = roof.py[a];
    q->px[1] = roof.px[bIdx];   q->py[1] = roof.py[bIdx];
    q->px[2] = ground.px[bIdx]; q->py[2] = ground.py[bIdx];
    q->px[3] = ground.px[a];    q->py[3] = ground.py[a];
    setSrc(q, ISO_GEOM_TEX_ISO, src->x, src->y, src->w, src->h);
    setColour(q, r * shade / 255, g * shade / 255, b * shade / 255, 255);
    q->mapX = (short)mx;
    q->mapY = (short)my;
    return true;
}

static bool buildTall(IsoGeomScene *s, const IsoGeomInput *in,
                      int mx, int my, BYTE tile, IsoTallKind kind) {
    int          height = isoGeomTallHeight(kind);
    int          r, g, b;
    IsoGeomItem *item;
    IsoGeomQuad *q;
    IsoGeomRect  cell;
    bool         standing = false;

    squareTint(in, mx, my, &r, &g, &b);

    item = pushItem(s);
    if (item == NULL) return false;
    memset(item, 0, sizeof(*item));
    item->depth     = isoDepth(mx, my, ISO_DEPTH_TALL);
    item->kind      = ISO_GEOM_ITEM_TALL;
    item->tallKind  = (unsigned char)kind;
    item->frame     = tile;
    item->mapX      = mx;
    item->mapY      = my;
    item->quadFirst = s->standCount;

    if (!pushTallShadow(s, in, mx, my, height)) return false;

    /* A pillbox or base the isometric sheet holds stands as its own sprite
       in place of the roof and faces. */
    if (kind == ISO_TALL_PILL || kind == ISO_TALL_BASE) {
        IsoTeam team = ISO_TEAM_NEUTRAL;
        int     frame = 0;
        if (kind == ISO_TALL_PILL) pillTeamFrame(tile, &team, &frame);
        else                       team = baseTeam(tile);
        if (in->sheet != NULL &&
            in->sheet(in->sheetUser,
                      kind == ISO_TALL_PILL ? ISO_SPRITE_PILLBOX : ISO_SPRITE_BASE,
                      team, frame, &cell)) {
            standing = true;
            item->isoSprite = true;
            item->frame = frame;
        }
    }

    if (standing) {
        /* The cell's bottom vertex on the square's: the cell is a diamond
           wide and two diamonds tall at the sheet's density 1. */
        float bx, by, side = (float)ISO_SHEET_CELL_W * in->cam.scale;
        project(in, mx + 1, my + 1, 0, 0, 0.0f, &bx, &by);
        q = pushStand(s);
        if (q == NULL) return false;
        rectAt(bx - side * 0.5f, by - side, side, side, q);
        setSrc(q, ISO_GEOM_TEX_ISO, cell.x, cell.y, cell.w, cell.h);
        setColour(q, r, g, b, 255);
        q->mapX = (short)mx;
        q->mapY = (short)my;
    } else {
        unsigned faces = isoGeomExposedFaces(in->map, mx, my);
        item->faces = (unsigned char)faces;
        if (faces != 0 && in->sheet != NULL &&
            in->sheet(in->sheetUser, faceKind(kind), ISO_TEAM_SELF, 0, &cell)) {
            if ((faces & ISO_FACE_SW) != 0 &&
                !pushFace(s, in, mx, my, height, ISO_FACE_SW, &cell,
                          ISO_FACE_SHADE_SW, r, g, b)) {
                return false;
            }
            if ((faces & ISO_FACE_SE) != 0 &&
                !pushFace(s, in, mx, my, height, ISO_FACE_SE, &cell,
                          ISO_FACE_SHADE_SE, r, g, b)) {
                return false;
            }
        } else {
            item->faces = 0;
        }
        /* The roof: the classic tile lifted by the height. */
        q = pushStand(s);
        if (q == NULL) return false;
        diamondAt(in, mx, my, (float)height, q);
        setSrc(q, ISO_GEOM_TEX_CLASSIC, (float)mapViewPosX[tile],
               (float)mapViewPosY[tile], TILE_SIZE_X, TILE_SIZE_Y);
        setColour(q, r, g, b, 255);
        q->mapX = (short)mx;
        q->mapY = (short)my;
    }

    item = &s->items[s->itemCount - 1];
    item->quadCount = s->standCount - item->quadFirst;
    return true;
}

static bool buildTallTiles(IsoGeomScene *s, const IsoGeomInput *in) {
    int mx, my;
    for (mx = s->minX; mx <= s->maxX; mx++) {
        for (my = s->minY; my <= s->maxY; my++) {
            BYTE tile = in->map->tile[mx][my];
            IsoTallKind kind;
            if (tile == OVERVIEW_UNSEEN) continue;
            kind = isoGeomTallKind(tile);
            if (kind == ISO_TALL_NONE) continue;
            if (!buildTall(s, in, mx, my, tile, kind)) return false;
        }
    }
    return true;
}

/*********************************************************
 * Moving sprites.
 *********************************************************/

/* A sprite as an upright rect centred on a screen point, with its drop
   shadow first: the same rect in black, moved down and right. */
static bool pushUpright(IsoGeomScene *s, const IsoGeomInput *in,
                        float cx, float cy, float w, float h,
                        IsoGeomTex tex, const IsoGeomRect *src,
                        bool shadow, int mx, int my) {
    float x = cx - w * 0.5f, y = cy - h * 0.5f;
    IsoGeomQuad *q;
    if (shadow) {
        float off = (float)ISO_SHADOW_SPRITE_OFFSET * in->cam.scale;
        q = pushStand(s);
        if (q == NULL) return false;
        rectAt(x + off, y + off, w, h, q);
        setSrc(q, tex, src->x, src->y, src->w, src->h);
        setColour(q, 0, 0, 0, ISO_SHADOW_SPRITE_ALPHA);
        q->mapX = (short)mx;
        q->mapY = (short)my;
    }
    q = pushStand(s);
    if (q == NULL) return false;
    rectAt(x, y, w, h, q);
    setSrc(q, tex, src->x, src->y, src->w, src->h);
    setColour(q, 255, 255, 255, 255);
    q->mapX = (short)mx;
    q->mapY = (short)my;
    return true;
}

/* A standing cell from the isometric sheet with its bottom vertex on the
   projection of (mx, my) + the world offset, shadow first. */
static bool pushStanding(IsoGeomScene *s, const IsoGeomInput *in,
                         int mode, int mx, int my, int wx, int wy,
                         const IsoGeomRect *cell, int sqX, int sqY) {
    float bx, by, side = (float)ISO_SHEET_CELL_W * in->cam.scale;
    project(in, mx, my, wx, wy, 0.0f, &bx, &by);
    modeSnap(in, mode, &bx, &by);
    return pushUpright(s, in, bx, by - side * 0.5f, side, side,
                       ISO_GEOM_TEX_ISO, cell, true, sqX, sqY);
}

static IsoGeomItem *beginItem(IsoGeomScene *s, IsoGeomItemKind kind,
                              int index, int sqX, int sqY) {
    IsoGeomItem *item = pushItem(s);
    if (item == NULL) return NULL;
    memset(item, 0, sizeof(*item));
    item->depth     = isoDepth(sqX, sqY, ISO_DEPTH_SPRITE);
    item->kind      = (unsigned char)kind;
    item->listIndex = index;
    item->mapX      = sqX;
    item->mapY      = sqY;
    item->quadFirst = s->standCount;
    return item;
}

static void endItem(IsoGeomScene *s) {
    IsoGeomItem *item = &s->items[s->itemCount - 1];
    item->quadCount = s->standCount - item->quadFirst;
}

static bool buildTanks(IsoGeomScene *s, const IsoGeomInput *in) {
    int  mode  = in->animMode;
    BYTE total = screenTanksGetNumEntries(in->tanks);
    BYTE count;

    for (count = 1; count <= total; count++) {
        BYTE mx, my, px, py, frame, playerNum, wx, wy, angle = 0;
        char name[256];
        IsoTeam team;
        bool boat;
        int  facing, ox, oy, sqX, sqY;
        IsoGeomRect cell;
        IsoGeomItem *item;

        screenTanksGetItem(in->tanks, count, &mx, &my, &px, &py, &frame,
                           &playerNum, name);
        if (!isoGeomTankTeam(frame, &team, &boat, &facing)) continue;
        wx = (BYTE)(px << 4);
        wy = (BYTE)(py << 4);
        screenTanksGetSubPixel(in->tanks, count, &wx, &wy, &angle);
        ox = modeOffset(mode, wx, px);
        oy = modeOffset(mode, wy, py);
        /* The list holds the sprite's top-left; the tank is half a square
           on from it, and that is the square its depth is keyed on. */
        sqX = mx + (ox + ISO_HALF_SQUARE) / ISO_WORLD_PER_SQUARE;
        sqY = my + (oy + ISO_HALF_SQUARE) / ISO_WORLD_PER_SQUARE;

        item = beginItem(s, ISO_GEOM_ITEM_TANK, count, sqX, sqY);
        if (item == NULL) return false;

        if (in->sheet != NULL &&
            in->sheet(in->sheetUser,
                      boat ? ISO_SPRITE_TANK_BOAT : ISO_SPRITE_TANK,
                      team, facing, &cell)) {
            item->isoSprite = true;
            item->frame     = isoFacingFrame(facing, true);
            /* The cell's bottom vertex is the bottom vertex of the square
               round the tank's centre: a whole square on from the listed
               top-left. */
            if (!pushStanding(s, in, mode, mx + 1, my + 1, ox, oy, &cell,
                              sqX, sqY)) {
                return false;
            }
        } else {
            int   turned = isoFacingFrame(facing, false);
            int   classic = (int)team * 32 + (boat ? TANK_BOAT_ADD : 0) + turned;
            float cx, cy, side = (float)TILE_SIZE_X * in->cam.scale;
            IsoGeomRect src;
            item->frame = turned;
            src.x = (float)kTankSheet[classic][0];
            src.y = (float)kTankSheet[classic][1];
            src.w = TILE_SIZE_X;
            src.h = TILE_SIZE_Y;
            project(in, mx, my, ox + ISO_HALF_SQUARE, oy + ISO_HALF_SQUARE,
                    0.0f, &cx, &cy);
            modeSnap(in, mode, &cx, &cy);
            if (!pushUpright(s, in, cx, cy, side, side, ISO_GEOM_TEX_CLASSIC,
                             &src, true, sqX, sqY)) {
                return false;
            }
        }
        endItem(s);
    }
    return true;
}

static bool buildLgms(IsoGeomScene *s, const IsoGeomInput *in) {
    int  mode  = in->animMode;
    BYTE total = screenLgmGetNumEntries(in->lgms);
    BYTE count;

    for (count = 1; count <= total; count++) {
        BYTE mx, my, px, py, frame, wx, wy;
        int  ox, oy;
        bool onFoot;
        IsoGeomRect cell;
        IsoGeomItem *item;

        screenLgmGetItem(in->lgms, count, &mx, &my, &px, &py, &frame);
        wx = (BYTE)(px << 4);
        wy = (BYTE)(py << 4);
        screenLgmGetSubPixel(in->lgms, count, &wx, &wy);
        ox = modeOffset(mode, wx, px);
        oy = modeOffset(mode, wy, py);
        onFoot = (frame == LGM0 || frame == LGM1 || frame == LGM2);

        /* On foot the position is the man's own pixel; the helicopter is a
           whole square drawn from its top-left, like a tank. */
        if (onFoot) {
            item = beginItem(s, ISO_GEOM_ITEM_LGM, count, mx, my);
        } else {
            item = beginItem(s, ISO_GEOM_ITEM_LGM, count,
                             mx + (ox + ISO_HALF_SQUARE) / ISO_WORLD_PER_SQUARE,
                             my + (oy + ISO_HALF_SQUARE) / ISO_WORLD_PER_SQUARE);
        }
        if (item == NULL) return false;
        item->frame = frame;

        if (in->sheet != NULL &&
            in->sheet(in->sheetUser,
                      onFoot ? ISO_SPRITE_LGM : ISO_SPRITE_LGM_HELICOPTER,
                      ISO_TEAM_SELF, onFoot ? (int)frame : 0, &cell)) {
            item->isoSprite = true;
            /* The diamond's centre is the man's point, so its bottom vertex
               is half a square on; for the helicopter a whole one. */
            if (onFoot) {
                if (!pushStanding(s, in, mode, mx, my, ox + ISO_HALF_SQUARE,
                                  oy + ISO_HALF_SQUARE, &cell,
                                  item->mapX, item->mapY)) {
                    return false;
                }
            } else if (!pushStanding(s, in, mode, mx + 1, my + 1, ox, oy,
                                     &cell, item->mapX, item->mapY)) {
                return false;
            }
        } else {
            float cx, cy;
            IsoGeomRect src;
            if (onFoot) {
                src.x = (float)(frame == LGM0 ? LGM0_X : frame == LGM1 ? LGM1_X : LGM2_X);
                src.y = (float)(frame == LGM0 ? LGM0_Y : frame == LGM1 ? LGM1_Y : LGM2_Y);
                src.w = LGM_WIDTH;
                src.h = LGM_HEIGHT;
                project(in, mx, my, ox, oy, 0.0f, &cx, &cy);
            } else {
                src.x = LGM_HELICOPTER_X;
                src.y = LGM_HELICOPTER_Y;
                src.w = TILE_SIZE_X;
                src.h = TILE_SIZE_Y;
                project(in, mx, my, ox + ISO_HALF_SQUARE, oy + ISO_HALF_SQUARE,
                        0.0f, &cx, &cy);
            }
            modeSnap(in, mode, &cx, &cy);
            if (!pushUpright(s, in, cx, cy, src.w * in->cam.scale,
                             src.h * in->cam.scale, ISO_GEOM_TEX_CLASSIC,
                             &src, true, item->mapX, item->mapY)) {
                return false;
            }
        }
        endItem(s);
    }
    return true;
}

static bool buildShells(IsoGeomScene *s, const IsoGeomInput *in) {
    /* Shells are small and fast, so the smooth-shells setting puts them in
       Smooth whatever the mode, as mapViewDrawShells does. */
    int mode  = in->smoothShells ? (int)GFX_ANIM_SMOOTH : in->animMode;
    int total = screenBulletsGetNumEntries(in->bullets);
    int count;

    for (count = 1; count <= total; count++) {
        BYTE mx, my, px, py, frame, wx, wy;
        int  ox, oy;
        float cx, cy;
        IsoGeomRect src;
        IsoGeomItem *item;

        screenBulletsGetItem(in->bullets, count, &mx, &my, &px, &py, &frame);
        wx = (BYTE)(px << 4);
        wy = (BYTE)(py << 4);
        screenBulletsGetSubPixel(in->bullets, count, &wx, &wy);
        ox = modeOffset(mode, wx, px);
        oy = modeOffset(mode, wy, py);

        if (frame >= SHELL_DIR0 && frame <= SHELL_DIR15) {
            const int *sh = kShellSheet[frame - SHELL_DIR0];
            src.x = (float)sh[0];
            src.y = (float)sh[1];
            src.w = (float)sh[2];
            src.h = (float)sh[3];
            /* The shell's position is the shell itself: centre on it. */
            project(in, mx, my, ox, oy, 0.0f, &cx, &cy);
        } else if (frame >= SHELL_EXPLOSION8 && frame <= SHELL_EXPLOSION1) {
            const int *ex = kExplosionSheet[frame - SHELL_EXPLOSION8];
            src.x = (float)ex[0];
            src.y = (float)ex[1];
            src.w = TILE_SIZE_X;
            src.h = TILE_SIZE_Y;
            /* An explosion is a whole square drawn from its top-left. */
            project(in, mx, my, ox + ISO_HALF_SQUARE, oy + ISO_HALF_SQUARE,
                    0.0f, &cx, &cy);
        } else {
            continue;
        }
        modeSnap(in, mode, &cx, &cy);

        /* An explosion's centre is half a square on from its listed
           square, so that is the square it sorts with, as a tank does. */
        if (frame < SHELL_DIR0) {
            mx = (BYTE)(mx + (ox + ISO_HALF_SQUARE) / ISO_WORLD_PER_SQUARE);
            my = (BYTE)(my + (oy + ISO_HALF_SQUARE) / ISO_WORLD_PER_SQUARE);
        }
        item = beginItem(s, ISO_GEOM_ITEM_SHELL, count, mx, my);
        if (item == NULL) return false;
        item->frame = frame;
        if (!pushUpright(s, in, cx, cy, src.w * in->cam.scale,
                         src.h * in->cam.scale, ISO_GEOM_TEX_CLASSIC, &src,
                         false, mx, my)) {
            return false;
        }
        endItem(s);
    }
    return true;
}

/*********************************************************
 * The overlay's anchors.
 *********************************************************/

static void buildOverlay(IsoGeomScene *s, const IsoGeomInput *in) {
    IsoGeomOverlay *ov = &s->overlay;
    int i;

    ov->cursorShown = in->cursorShown;
    ov->cursorFaint = in->cursorFaint;
    if (in->cursorShown) {
        IsoGeomQuad q;
        diamondAt(in, in->cursorMapX, in->cursorMapY, 0.0f, &q);
        for (i = 0; i < 4; i++) {
            ov->cursorX[i] = q.px[i];
            ov->cursorY[i] = q.py[i];
        }
    }

    /* The aim point is the middle of the sight's game pixel: the classic
       view puts the crosshair's centre texel there. */
    ov->gunsightShown = in->gunsightShown;
    if (in->gunsightShown) {
        project(in, in->gsMapX, in->gsMapY,
                in->gsPixelX * 16 + 8, in->gsPixelY * 16 + 8, 0.0f,
                &ov->gunsightX, &ov->gunsightY);
    }

    /* A name beside its tank: off the tank's centre, up and to the right,
       in the same animation mode the sprite was placed in. */
    ov->tankLabelCount = 0;
    if (in->tanks != NULL) {
        BYTE total = screenTanksGetNumEntries(in->tanks);
        BYTE count;
        for (count = 1; count <= total && ov->tankLabelCount < MAX_TANKS; count++) {
            BYTE mx, my, px, py, frame, playerNum, wx, wy, angle = 0;
            char name[256];
            IsoGeomAnchor *a;
            float cx, cy;
            screenTanksGetItem(in->tanks, count, &mx, &my, &px, &py, &frame,
                               &playerNum, name);
            if (frame > TANK_EVILBOAT_15) continue;
            wx = (BYTE)(px << 4);
            wy = (BYTE)(py << 4);
            screenTanksGetSubPixel(in->tanks, count, &wx, &wy, &angle);
            project(in, mx, my,
                    modeOffset(in->animMode, wx, px) + ISO_HALF_SQUARE,
                    modeOffset(in->animMode, wy, py) + ISO_HALF_SQUARE,
                    0.0f, &cx, &cy);
            modeSnap(in, in->animMode, &cx, &cy);
            a = &ov->tankLabels[ov->tankLabelCount++];
            a->index = count;
            a->x = cx + (float)(TILE_SIZE_X / 2 + 2) * in->cam.scale;
            a->y = cy - (float)(TILE_SIZE_Y / 2 + 4) * in->cam.scale;
        }
    }

    /* A number over its pill or base: the square's centre at the top of the
       tile, so it sits on the roof rather than behind the faces. */
    ov->itemLabelCount = 0;
    for (i = 0; i < in->itemLabelCount &&
                ov->itemLabelCount < (int)(MAX_PILLS + MAX_BASES); i++) {
        const OverviewItemLabel *l = &in->itemLabels[i];
        IsoGeomAnchor *a = &ov->itemLabels[ov->itemLabelCount++];
        int height = l->isBase ? ISO_HEIGHT_BASE : ISO_HEIGHT_PILL;
        a->index = i;
        isoGeomSquareCentre(in, l->mapX, l->mapY, (float)height, &a->x, &a->y);
    }
}

/*********************************************************
 * The build.
 *********************************************************/

static int compareItems(const void *pa, const void *pb) {
    const IsoGeomItem *a = (const IsoGeomItem *)pa;
    const IsoGeomItem *b = (const IsoGeomItem *)pb;
    if (a->depth != b->depth) return a->depth < b->depth ? -1 : 1;
    /* The same depth keeps the build order, which the quad index records:
       qsort alone is not stable. */
    if (a->quadFirst != b->quadFirst) return a->quadFirst < b->quadFirst ? -1 : 1;
    return 0;
}

bool isoGeomBuild(IsoGeomScene *s, const IsoGeomInput *in) {
    bool ok = true;

    sceneClear(s);
    if (in == NULL) return true;

    /* mapViewPosX/Y are what every ground and roof quad reads; idempotent,
       so a build does not depend on a sheet having been loaded first. */
    mapViewInit();

    if (in->map != NULL &&
        isoGeomVisibleRange(&in->cam, in->targetW, in->targetH,
                            &s->minX, &s->minY, &s->maxX, &s->maxY)) {
        ok = buildGround(s, in) && buildTallTiles(s, in);
    } else {
        s->minX = s->minY = 0;
        s->maxX = s->maxY = -1;
    }
    if (ok && in->tanks != NULL)   ok = buildTanks(s, in);
    if (ok && in->lgms != NULL)    ok = buildLgms(s, in);
    if (ok && in->bullets != NULL) ok = buildShells(s, in);

    if (!ok) {
        sceneClear(s);
        return false;
    }

    if (s->itemCount > 1) {
        qsort(s->items, (size_t)s->itemCount, sizeof(IsoGeomItem), compareItems);
    }
    buildOverlay(s, in);
    return true;
}
