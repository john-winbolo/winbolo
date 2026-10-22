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
 * Name:          isoview.c
 * Purpose:       The isometric view's renderer: see
 *                isoview.h. Owns the offscreen target and the
 *                camera, asks isoview_geom.c for the quads
 *                and anchors of a frame, and submits them:
 *                the ground and the standing things through
 *                SDL_RenderGeometry in runs batched by
 *                texture, the overlay's marks flat on top.
 *********************************************************/

#include "isoview.h"
#include "isoview_geom.h"
#include "iso_sheet.h"
#include "mapview.h"          /* mapViewSpriteSrc, mapViewAtlasSrc */
#include "gfx_settings.h"
#include "fog_look.h"
#include "sdl3draw.h"         /* sdl3DrawOnRenderThread, sdl3DrawScaleModeForFilter */
#include "build_cursor.h"     /* the shared build cursor's square */
#include "ping_marker.h"
#include "../ping_kinds.h"    /* pingDisplayAlpha */
#include "../tiles.h"
#include "client_sim.h"
#include "overview_types.h"

#include <string.h>

/* The Edit-menu Smooth Scrolling preference (winbolo.c): on, follow glides
   with the tank's sub-square position; off, it steps whole squares. */
extern bool smoothScrollingEnabled;

/* The View-menu Pillbox Labels and Base Labels switches (winbolo.c). */
extern bool showPillLabels;
extern bool showBaseLabels;

/* The camera's starting scale, in screen pixels per game pixel: a square's
   diamond is 64 by 32. The host picks the scale it wants; this is what a
   fresh view draws at until it does. */
#define ISOVIEW_DEFAULT_SCALE 2.0f

/* Side of the crosshair sprite in game pixels: one more than a tile, so its
   centre texel sits on the aim point. */
#define ISOVIEW_CROSSHAIR_PX 17

/* How many quads one SDL_RenderGeometry call takes at most. */
#define ISOVIEW_BATCH_QUADS 1024

struct IsoView {
    IsoCamera     cam;
    bool          follow;
    float         insetLeft, insetTop, insetRight, insetBottom;

    SDL_Texture  *target;
    SDL_Renderer *targetRenderer;
    int           targetW, targetH;

    IsoGeomScene  scene;

    /* The frame's textures, held while the arrays are submitted. */
    SDL_Texture  *tilesTex;
    int           sheetScale;
    SDL_Texture  *spritesTex;
    const SpriteAtlas *sprites;
    SDL_Texture  *isoTex;
    float         isoTexW, isoTexH;
};

/*********************************************************
 * The target.
 *********************************************************/

static bool isoViewEnsureTarget(IsoView *v, SDL_Renderer *r, int w, int h) {
    if (v->target && v->targetRenderer == r &&
        v->targetW == w && v->targetH == h) {
        return true;
    }
    if (v->target) {
        SDL_DestroyTexture(v->target);
        v->target = NULL;
    }
    v->targetRenderer = NULL;
    v->targetW = 0;
    v->targetH = 0;

    v->target = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                  SDL_TEXTUREACCESS_TARGET, w, h);
    if (!v->target) return false;
    SDL_SetTextureScaleMode(v->target, SDL_SCALEMODE_NEAREST);
    v->targetRenderer = r;
    v->targetW = w;
    v->targetH = h;
    return true;
}

/*********************************************************
 * The isometric sheet's answer for the geometry.
 *********************************************************/

static bool isoViewSheetLookup(void *user, IsoSpriteKind kind, IsoTeam team,
                               int frame, IsoGeomRect *out) {
    IsoView *v = (IsoView *)user;
    SDL_Rect rect;
    bool present = false;
    if (v->isoTex == NULL) return false;
    if (!isoSheetRect(kind, team, frame, v->sheetScale, &rect, &present)) {
        return false;
    }
    if (!present) return false;
    out->x = (float)rect.x;
    out->y = (float)rect.y;
    out->w = (float)rect.w;
    out->h = (float)rect.h;
    return true;
}

/*********************************************************
 * Submitting quads.
 *********************************************************/

/* The texture a quad samples and its source rect in that texture's texels:
   the classic sheet goes through mapViewSpriteSrc, so a moving sprite is
   read from the padded atlas when there is one and the terrain from the
   sheet with its inset. */
static SDL_Texture *isoViewQuadTexture(IsoView *v, const IsoGeomQuad *q,
                                       SDL_FRect *src) {
    MapViewCtx ctx;
    switch (q->tex) {
        case ISO_GEOM_TEX_CLASSIC:
            memset(&ctx, 0, sizeof(ctx));
            ctx.tilesTex   = v->tilesTex;
            ctx.sheetScale = v->sheetScale;
            ctx.spritesTex = v->spritesTex;
            ctx.sprites    = v->sprites;
            return mapViewSpriteSrc(&ctx, (int)q->src.x, (int)q->src.y,
                                    (int)q->src.w, (int)q->src.h, src);
        case ISO_GEOM_TEX_ISO:
            src->x = q->src.x;
            src->y = q->src.y;
            src->w = q->src.w;
            src->h = q->src.h;
            return v->isoTex;
        default:
            return NULL;
    }
}

typedef struct IsoViewBatch {
    SDL_Vertex   verts[ISOVIEW_BATCH_QUADS * 4];
    int          indices[ISOVIEW_BATCH_QUADS * 6];
    int          quads;
    SDL_Texture *tex;
    float        texW, texH;
} IsoViewBatch;

static void isoViewBatchFlush(SDL_Renderer *r, IsoViewBatch *b) {
    if (b->quads == 0) return;
    SDL_RenderGeometry(r, b->tex, b->verts, b->quads * 4,
                       b->indices, b->quads * 6);
    b->quads = 0;
}

/* Adds one quad to the batch, flushing first when the texture changes or
   the batch is full. The source rect's corners go to the quad's corners in
   the same order, top-left first and round clockwise. */
static void isoViewBatchQuad(SDL_Renderer *r, IsoViewBatch *b, SDL_Texture *tex,
                             const SDL_FRect *src, const IsoGeomQuad *q) {
    SDL_FColor colour;
    int base, i;
    static const float kU[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    static const float kV[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

    if (b->quads == ISOVIEW_BATCH_QUADS || (b->quads > 0 && b->tex != tex)) {
        isoViewBatchFlush(r, b);
    }
    if (b->quads == 0) {
        b->tex  = tex;
        b->texW = 1.0f;
        b->texH = 1.0f;
        if (tex != NULL) {
            SDL_GetTextureSize(tex, &b->texW, &b->texH);
            if (b->texW <= 0.0f) b->texW = 1.0f;
            if (b->texH <= 0.0f) b->texH = 1.0f;
        }
    }

    colour.r = (float)q->r / 255.0f;
    colour.g = (float)q->g / 255.0f;
    colour.b = (float)q->b / 255.0f;
    colour.a = (float)q->a / 255.0f;

    base = b->quads * 4;
    for (i = 0; i < 4; i++) {
        SDL_Vertex *vt = &b->verts[base + i];
        vt->position.x = q->px[i];
        vt->position.y = q->py[i];
        vt->color      = colour;
        if (tex != NULL && src != NULL) {
            vt->tex_coord.x = (src->x + kU[i] * src->w) / b->texW;
            vt->tex_coord.y = (src->y + kV[i] * src->h) / b->texH;
        } else {
            vt->tex_coord.x = 0.0f;
            vt->tex_coord.y = 0.0f;
        }
    }
    {
        int *ix = &b->indices[b->quads * 6];
        ix[0] = base;     ix[1] = base + 1; ix[2] = base + 2;
        ix[3] = base;     ix[4] = base + 2; ix[5] = base + 3;
    }
    b->quads++;
}

static void isoViewSubmitQuads(IsoView *v, SDL_Renderer *r, IsoViewBatch *b,
                               const IsoGeomQuad *quads, int count) {
    int i;
    for (i = 0; i < count; i++) {
        SDL_FRect src;
        SDL_Texture *tex = isoViewQuadTexture(v, &quads[i], &src);
        /* A textured quad with no texture to draw it from is left out
           rather than drawn as a flat block. */
        if (quads[i].tex != ISO_GEOM_TEX_NONE && tex == NULL) continue;
        isoViewBatchQuad(r, b, tex, tex ? &src : NULL, &quads[i]);
    }
}

/*********************************************************
 * The overlay.
 *********************************************************/

/* The pill and base numbers the snapshot lists, filtered as the full screen
   map filters them: the square has to be live and still show a pill or base
   tile, and the Labels switches apply. */
static int isoViewPickItemLabels(const OverviewSnapshot *snap,
                                 OverviewItemLabel *out, int cap) {
    const OverviewMap *om = overviewSnapshotMap(snap);
    int total, n = 0, i;
    const OverviewItemLabel *all;
    if (om == NULL) return 0;
    total = overviewSnapshotItemLabelCount(snap);
    all   = overviewSnapshotItemLabels(snap);
    for (i = 0; i < total && n < cap; i++) {
        const OverviewItemLabel *l = &all[i];
        bool wanted = l->isBase ? showBaseLabels : showPillLabels;
        if (!wanted) continue;
        if ((om->flags[l->mapX][l->mapY] & OVERVIEW_F_LIVE) == 0) continue;
        {
            BYTE tile = om->tile[l->mapX][l->mapY];
            bool onTile = l->isBase ? mapViewTileIsBase(tile)
                                    : mapViewTileIsPill(tile);
            if (!onTile) continue;
        }
        out[n++] = *l;
    }
    return n;
}

/* One number as a texture, as mapview_overlay.c renders it. */
static SDL_Texture *isoViewRenderNumber(SDL_Renderer *r, TTF_Font *font,
                                        int number) {
    SDL_Color    white = { 200, 200, 200, 255 };
    char         str[8];
    SDL_Surface *surf;
    SDL_Texture *tex;
    SDL_snprintf(str, sizeof(str), "%d", number);
    surf = TTF_RenderText_Blended(font, str, 0, white);
    if (surf == NULL) return NULL;
    tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_DestroySurface(surf);
    return tex;
}

/* The item label cache is kept in step the way mapViewDrawOverlay keeps it:
   dropped whole when the renderer or either face has changed under it. That
   pass places its numbers on the classic grid, so the numbers here are drawn
   from the same cache by hand, centred on the anchor the geometry gives. */
static void isoViewDrawItemLabels(IsoView *v, SDL_Renderer *r,
                                  const IsoViewLabels *labels,
                                  const OverviewItemLabel *items,
                                  float displayScale) {
    const IsoGeomOverlay *ov = &v->scene.overlay;
    ItemLabelCache *c = labels->itemLabelCache;
    int i;

    if (labels->pillFont == NULL && labels->baseFont == NULL) return;
    if (c != NULL) {
        float ps = labels->pillFont ? TTF_GetFontSize(labels->pillFont) : 0.0f;
        float bs = labels->baseFont ? TTF_GetFontSize(labels->baseFont) : 0.0f;
        if (r != c->renderer || labels->pillFont != c->pillFont ||
            labels->baseFont != c->baseFont || ps != c->pillFontSize ||
            bs != c->baseFontSize) {
            itemLabelCacheFlush(c);
            c->renderer     = r;
            c->pillFont     = labels->pillFont;
            c->baseFont     = labels->baseFont;
            c->pillFontSize = ps;
            c->baseFontSize = bs;
        }
    }

    for (i = 0; i < ov->itemLabelCount; i++) {
        const IsoGeomAnchor *a = &ov->itemLabels[i];
        const OverviewItemLabel *l = &items[a->index];
        TTF_Font    *font  = l->isBase ? labels->baseFont : labels->pillFont;
        int          num   = (int)l->number;
        SDL_Texture *tex   = NULL;
        bool         owned = false;
        float        texW = 0.0f, texH = 0.0f;

        if (font == NULL) continue;
        if (c != NULL && num >= 0 && num < ITEM_LABEL_MAX_NUM) {
            SDL_Texture **slot = l->isBase ? &c->baseTex[num] : &c->pillTex[num];
            if (*slot == NULL) *slot = isoViewRenderNumber(r, font, num);
            tex = *slot;
        } else {
            tex   = isoViewRenderNumber(r, font, num);
            owned = (tex != NULL);
        }
        if (tex == NULL) continue;

        SDL_GetTextureSize(tex, &texW, &texH);
        {
            float tw = texW * displayScale, th = texH * displayScale;
            float tx = a->x - tw * 0.5f, ty = a->y - th * 0.5f;
            SDL_FRect bg = { tx - displayScale, ty - displayScale,
                             tw + 2.0f * displayScale, th + 2.0f * displayScale };
            SDL_FRect d  = { tx, ty, tw, th };
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            SDL_RenderFillRect(r, &bg);
            SDL_RenderTexture(r, tex, NULL, &d);
        }
        if (owned) SDL_DestroyTexture(tex);
    }
}

static void isoViewDrawOverlay(IsoView *v, SDL_Renderer *r,
                               SDL_Texture *crosshair,
                               const OverviewSnapshot *snap,
                               const IsoViewLabels *labels,
                               const OverviewItemLabel *items) {
    const IsoGeomOverlay *ov = &v->scene.overlay;
    float scale = v->cam.scale;
    float displayScale = 1.0f;
    int   mainZoom = 1;

    if (labels != NULL) {
        mainZoom = labels->mainZoom < 1 ? 1 : labels->mainZoom;
        displayScale = scale / (float)mainZoom;
    }

    /* The build cursor: its square's diamond as an outline, solid for a
       cursor the player is steering and half strength for a target that is
       only locked in. */
    if (ov->cursorShown) {
        SDL_FPoint pts[5];
        int i;
        for (i = 0; i < 4; i++) {
            pts[i].x = ov->cursorX[i];
            pts[i].y = ov->cursorY[i];
        }
        pts[4] = pts[0];
        SDL_SetRenderDrawColor(r, 255, 255, 255, ov->cursorFaint ? 128 : 255);
        SDL_RenderLines(r, pts, 5);
    }

    /* Tank names, through the host's cache and face. */
    if (labels != NULL && labels->labelCache != NULL &&
        labels->labelFont != NULL) {
        const screenTanks *tks = overviewSnapshotTanks(snap);
        int i;
        for (i = 0; tks != NULL && i < ov->tankLabelCount; i++) {
            const IsoGeomAnchor *a = &ov->tankLabels[i];
            BYTE mx, my, px, py, frame, playerNum;
            char name[256];
            screenTanksGetItem(tks, (BYTE)a->index, &mx, &my, &px, &py, &frame,
                               &playerNum, name);
            if (name[0] == '\0') continue;
            if (a->x > (float)v->targetW || a->y > (float)v->targetH) continue;
            tankLabelDraw(labels->labelCache, r, labels->labelFont, name,
                          playerNum, a->x, a->y, displayScale);
        }
    }

    /* The gunsight over everything, centred on the aim point. */
    if (ov->gunsightShown && crosshair != NULL) {
        float side = (float)ISOVIEW_CROSSHAIR_PX * scale;
        SDL_FRect dst = { ov->gunsightX - side * 0.5f,
                          ov->gunsightY - side * 0.5f, side, side };
        SDL_SetTextureScaleMode(crosshair, SDL_SCALEMODE_NEAREST);
        SDL_RenderTexture(r, crosshair, NULL, &dst);
    }

    if (labels != NULL && items != NULL) {
        isoViewDrawItemLabels(v, r, labels, items, displayScale);
    }
}

/* Smart pings on the ground, at the centre of the square each landed in,
   one classic tile wide at this scale. Drawn after the terrain and before
   the sprites, as the other views draw them. */
static void isoViewDrawPings(IsoView *v, SDL_Renderer *r,
                             const IsoGeomInput *in,
                             const OverviewSnapshot *snap,
                             const IsoViewLabels *labels) {
    const ClientPing *pl = overviewSnapshotPings(snap);
    int    np    = overviewSnapshotPingCount(snap);
    Uint32 nowMs = (Uint32)SDL_GetTicks();
    float  tilePx = (float)TILE_SIZE_X * v->cam.scale;
    PingMarkerLabel label;
    int i;

    memset(&label, 0, sizeof(label));
    if (labels != NULL) {
        int mainZoom = labels->mainZoom < 1 ? 1 : labels->mainZoom;
        label.cache = labels->pingNameCache;
        label.font  = labels->labelFont;
        label.scale = v->cam.scale / (float)mainZoom;
    }
    for (i = 0; i < np; i++) {
        Uint32 ageMs = nowMs - pl[i].recvMs;
        float  sx = 0.0f, sy = 0.0f;
        if (nowMs < pl[i].recvMs || pingDisplayAlpha((int)ageMs) <= 0.0f) continue;
        isoGeomSquareCentre(in, pl[i].worldX >> 8, pl[i].worldY >> 8, 0.0f,
                            &sx, &sy);
        label.name = pl[i].senderName;
        label.slot = pl[i].sender;
        pingMarkerDraw(r, pl[i].kind, sx, sy, tilePx, tilePx, ageMs, &label);
    }
}

/*********************************************************
 * The camera.
 *********************************************************/

/* Puts world position (wx, wy) at the centre of the part of the target the
   host's panels leave clear. The insets move that centre off the target's
   middle in screen pixels; the camera's centre is moved the other way by the
   same distance on the ground plane. */
static void isoViewCentreOn(IsoView *v, int w, int h, float wx, float wy) {
    float shiftX = (v->insetLeft - v->insetRight) * 0.5f;
    float shiftY = (v->insetTop - v->insetBottom) * 0.5f;
    IsoCamera cam = v->cam;
    float ax, ay, bx, by;

    cam.centerWX = wx;
    cam.centerWY = wy;
    if (shiftX == 0.0f && shiftY == 0.0f) {
        v->cam = cam;
        return;
    }
    /* The ground position under the clear centre less the one under the
       target's middle is how far the camera has to move. */
    isoUnproject(&cam, w, h, (float)w * 0.5f, (float)h * 0.5f, &ax, &ay);
    isoUnproject(&cam, w, h, (float)w * 0.5f + shiftX, (float)h * 0.5f + shiftY,
                 &bx, &by);
    cam.centerWX = wx - (bx - ax);
    cam.centerWY = wy - (by - ay);
    v->cam = cam;
}

static void isoViewFollowTick(IsoView *v, int w, int h,
                              const OverviewSnapshot *snap) {
    float mapX = 0.0f, mapY = 0.0f;
    if (!v->follow || snap == NULL) return;
    if (overviewSnapshotInItemView(snap)) {
        int itemMX = 0, itemMY = 0;
        overviewSnapshotItemViewSquare(snap, &itemMX, &itemMY);
        isoViewCentreOn(v, w, h, ((float)itemMX + 0.5f) * ISO_WORLD_PER_SQUARE,
                        ((float)itemMY + 0.5f) * ISO_WORLD_PER_SQUARE);
        return;
    }
    /* The live block's centre comes first, as it does for the full screen
       map: a player who has scrolled the classic view off the tank keeps
       that scroll here. The tank is the fallback when no block reports. */
    if (overviewSnapshotWindowCentre(snap, &mapX, &mapY) ||
        overviewSnapshotTankPos(snap, &mapX, &mapY)) {
        if (!smoothScrollingEnabled) {
            mapX = SDL_floorf(mapX) + 0.5f;
            mapY = SDL_floorf(mapY) + 0.5f;
        }
        isoViewCentreOn(v, w, h, mapX * ISO_WORLD_PER_SQUARE,
                        mapY * ISO_WORLD_PER_SQUARE);
    }
}

/*********************************************************
 * The public surface.
 *********************************************************/

IsoView *isoViewCreate(void) {
    IsoView *v = (IsoView *)SDL_calloc(1, sizeof(*v));
    if (!v) return NULL;
    mapViewInit();
    isoGeomSceneInit(&v->scene);
    v->cam.centerWX = 128.0f * ISO_WORLD_PER_SQUARE;
    v->cam.centerWY = 128.0f * ISO_WORLD_PER_SQUARE;
    v->cam.scale    = ISOVIEW_DEFAULT_SCALE;
    v->follow       = true;
    return v;
}

void isoViewDestroy(IsoView *v) {
    if (!v) return;
    isoGeomSceneFree(&v->scene);
    if (v->target) {
        SDL_DestroyTexture(v->target);
        v->target = NULL;
    }
    SDL_free(v);
}

SDL_Texture *isoViewGetTexture(const IsoView *v) {
    return v ? v->target : NULL;
}

void isoViewGetSize(const IsoView *v, int *outW, int *outH) {
    if (outW) *outW = v ? v->targetW : 0;
    if (outH) *outH = v ? v->targetH : 0;
}

IsoCamera *isoViewCamera(IsoView *v) {
    return v ? &v->cam : NULL;
}

void isoViewSetFollow(IsoView *v, bool follow) {
    if (v) v->follow = follow;
}

bool isoViewGetFollow(const IsoView *v) {
    return v ? v->follow : false;
}

void isoViewSetHudInsets(IsoView *v, float left, float top,
                         float right, float bottom) {
    if (!v) return;
    v->insetLeft   = left;
    v->insetTop    = top;
    v->insetRight  = right;
    v->insetBottom = bottom;
}

void isoViewRenderOffscreen(IsoView *v, SDL_Renderer *r,
                            SDL_Texture *tilesTex, int sheetScale,
                            SDL_Texture *spritesTex, const SpriteAtlas *sprites,
                            SDL_Texture *isoTex, SDL_Texture *crosshair,
                            int w, int h,
                            const OverviewSnapshot *snap,
                            const IsoViewLabels *labels) {
    const OverviewMap *om;
    IsoGeomInput in;
    OverviewItemLabel items[MAX_PILLS + MAX_BASES];
    SDL_BlendMode wasBlend = SDL_BLENDMODE_NONE;

    /* The GPU is touched here, so this has to be the render thread. */
    SDL_assert(sdl3DrawOnRenderThread());

    if (!v || !r || w <= 0 || h <= 0) return;
    if (!isoViewEnsureTarget(v, r, w, h)) return;

    SDL_SetRenderTarget(r, v->target);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    isoViewFollowTick(v, w, h, snap);

    om = overviewSnapshotMap(snap);
    if (om != NULL && tilesTex != NULL) {
        FogStyle style = gfxGetFogStyle();
        IsoViewBatch *batch;
        int i;

        if (sheetScale < 1) sheetScale = 1;
        v->tilesTex   = tilesTex;
        v->sheetScale = sheetScale;
        v->spritesTex = spritesTex;
        v->sprites    = sprites;
        v->isoTex     = isoTex;

        /* The ImGui backend sets the sampler per draw, so the player's filter
           is put back on the sheets here, as the full screen map does. */
        SDL_SetTextureColorMod(tilesTex, 255, 255, 255);
        SDL_SetTextureScaleMode(tilesTex,
                                sdl3DrawScaleModeForFilter(gfxGetTextureFilter()));
        if (spritesTex) {
            SDL_SetTextureScaleMode(spritesTex,
                                    sdl3DrawScaleModeForFilter(gfxGetTextureFilter()));
        }
        if (isoTex) {
            SDL_SetTextureScaleMode(isoTex,
                                    sdl3DrawScaleModeForFilter(gfxGetTextureFilter()));
        }

        memset(&in, 0, sizeof(in));
        in.cam      = v->cam;
        in.targetW  = w;
        in.targetH  = h;
        in.map      = om;
        in.tanks    = overviewSnapshotTanks(snap);
        in.lgms     = overviewSnapshotLgms(snap);
        in.bullets  = overviewSnapshotBullets(snap);
        in.animMode = (int)gfxGetAnimSmoothness();
        in.sheetScale   = sheetScale;
        in.smoothShells = gfxGetSmoothShells();
        in.fogWashes = fogLookColour(style, &in.fogR, &in.fogG, &in.fogB) != 0;
        in.sheet     = isoViewSheetLookup;
        in.sheetUser = v;

        /* Where a build will land, from the shared build cursor: solid while
           cursor mode steers it, faint for a target that is only locked
           in. */
        {
            BYTE bcX = 0, bcY = 0;
            bool cursorMode = buildCursorGetTile(&bcX, &bcY);
            in.cursorShown = cursorMode || buildCursorGetTargetTile(&bcX, &bcY);
            in.cursorFaint = !cursorMode;
            in.cursorMapX  = bcX;
            in.cursorMapY  = bcY;
        }
        /* The reticle only when the tank sprite was drawn, for the reason
           overview_view.cpp gives: it would mark where a hidden tank is. */
        {
            BYTE gx = 0, gy = 0, gpx = 0, gpy = 0;
            in.gunsightShown = crosshair != NULL &&
                               overviewSnapshotSelfDrawn(snap) &&
                               overviewSnapshotGunsight(snap, &gx, &gy, &gpx, &gpy);
            in.gsMapX = gx;
            in.gsMapY = gy;
            in.gsPixelX = gpx;
            in.gsPixelY = gpy;
        }
        in.itemLabelCount = isoViewPickItemLabels(snap, items,
                                                  (int)(MAX_PILLS + MAX_BASES));
        in.itemLabels = items;

        if (isoGeomBuild(&v->scene, &in)) {
            batch = (IsoViewBatch *)SDL_malloc(sizeof(*batch));
            if (batch != NULL) {
                batch->quads = 0;
                batch->tex   = NULL;
                SDL_GetRenderDrawBlendMode(r, &wasBlend);
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

                /* The ground plane whole, then the pings on it, then
                   everything that stands, in depth order. */
                isoViewSubmitQuads(v, r, batch, v->scene.ground,
                                   v->scene.groundCount);
                isoViewBatchFlush(r, batch);
                isoViewDrawPings(v, r, &in, snap, labels);
                for (i = 0; i < v->scene.itemCount; i++) {
                    const IsoGeomItem *it = &v->scene.items[i];
                    isoViewSubmitQuads(v, r, batch,
                                       v->scene.stand + it->quadFirst,
                                       it->quadCount);
                }
                isoViewBatchFlush(r, batch);
                SDL_free(batch);

                isoViewDrawOverlay(v, r, crosshair, snap, labels, items);
                SDL_SetRenderDrawBlendMode(r, wasBlend);
            }
        }
        v->tilesTex   = NULL;
        v->spritesTex = NULL;
        v->sprites    = NULL;
        v->isoTex     = NULL;
    }

    /* Black over the lot while the sim says the death is in its blackout.
       The static is the host's. */
    if (overviewSnapshotBlackout(snap)) {
        SDL_FRect dst = { 0.0f, 0.0f, (float)w, (float)h };
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderFillRect(r, &dst);
    }

    SDL_SetRenderTarget(r, NULL);
}
