/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          overview_view.cpp
 * Purpose:       Implementation of the map overview's
 *                drawing and input — see overview_view.h.
 *                Terrain and mines straight from the client's
 *                OverviewMap, the fog that dims everything
 *                outside a live region over the top of them,
 *                then the tanks, men and shells standing on
 *                the squares it says are visible; the camera
 *                maths it sits on lives in overview_camera.cpp
 *                and the fog mask in overview_fog.cpp.
 *
 *                Death is answered here too: the fog closing
 *                over the wreck is the sim's doing, and this
 *                file draws the static that follows it — the
 *                map goes dark, then the snow comes up and
 *                holds until the tank is back.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"

#include "overview_view.h"
#include "overview_fog.h"   /* overviewFogBuildMask and the fog's constants */
#include "key_claims.h"     /* keyIsClaimedByGame */
#include "build_cursor.h"   /* buildCursorSetTile */

extern "C" {
#include "global.h"
#include "client_sim.h"     /* clientSimGetOverviewMap, clientSimGetMyTankMapPosF,
                               clientSimIsMyTankAlive,
                               clientSimIsMyTankDeathBlackout,
                               clientSimPrepareOverviewEntities,
                               clientSimManMoveToMap,
                               clientSimGetCurrentBuildSelect */
#include "../clientmutex.h" /* the build dispatch runs on the sim's data */
#include "cursor.h"         /* cursorSetCursor — the game's crosshair pointer */
#include "input.h"          /* inputBumpGunsight — the wheel's other job */
#include "overview_types.h"
#include "screentank.h"
#include "screenlgm.h"
#include "screenbullet.h"
#include "../tiles.h"       /* MINE_X / MINE_Y, TILE_SIZE_X / TILE_SIZE_Y */
#include "sprite_positions.h"
#include "mapview.h"        /* mapViewDrawShells / Tanks / LGMs */
}
#include "sdl3draw_status.h" /* sdl3DrawGetMessageFont — the newswire's face */
#include "sdl3draw.h"        /* sdl3DrawGetZoomFactor — the size it is opened at */
#include "tank_label.h"      /* the shared name + flag / brain-icon drawer */

/* The Edit-menu Smooth Scrolling preference (winbolo.c). On, follow glides
 * with the tank's sub-square position; off, it steps whole squares the way
 * the classic view's scroll does. */
extern "C" bool smoothScrollingEnabled;

/* View pixels an arrow key moves the centre per frame. In pixels rather than
 * squares so the map slides at the same apparent speed at every zoom. */
#define OVERVIEW_ARROW_STEP_PX 12.0f

/* Sub-steps of a game pixel the sprite pass works in.
 *
 * mapview.c places sprites with an integer zoom factor, which cannot express
 * the 0.5x, 0.75x and 1.5x rungs of the overview's ladder. So the factor is
 * fixed at this many steps per game pixel and the renderer's own scale — set
 * to zoomScale / OVERVIEW_ENTITY_SUBPX — carries the real zoom, exactly as
 * bg_game.c scales its debug text. Sixteen steps keep the rounding of the
 * camera origin, the one value that has to be handed over as an int, under a
 * quarter of a view pixel at every rung. */
#define OVERVIEW_ENTITY_SUBPX 16

/* Side of the crosshair sprite in game pixels. One more than a tile, so it
 * straddles the aim point rather than sitting in a corner of it. */
#define OVERVIEW_CROSSHAIR_PX 17

/* Zoom at or above which tank names are drawn. Below it the labels are wider
 * than the tanks are apart and the picture turns into text. */
#define OVERVIEW_LABEL_MIN_ZOOM 1.0f

/* The death blackout, drawn over the whole view from the tick the sim says a
 * death has stopped being watchable (clientSimIsMyTankDeathBlackout) through
 * to the respawn. The player watches their own explosion up to that point and
 * black is the last thing they see before the tank is back; it is what keeps
 * a dead player from sitting over the square that killed them and watching the
 * killer reposition, which is the same job the classic view's static does.
 *
 * A screen of snow the size of this one is a lot of noise for that, so this
 * view fades instead: the picture goes down to black over
 * OVERVIEW_DEATH_BLACK_FADE_MS and holds there, and the respawn cuts back to
 * the map. The memory underneath keeps stamping the whole time — what is
 * being taken away is the picture, not the block. */
#define OVERVIEW_DEATH_BLACK_FADE_MS 700    /* to reach full black */

struct OverviewView {
    OverviewCamera cam;

    /* The offscreen the host blits. Textures belong to the renderer that
     * made them, so the renderer is tracked alongside the size and all
     * three are what a reallocation tests. */
    SDL_Texture   *target;
    SDL_Renderer  *targetRenderer;
    int            targetW;
    int            targetH;

    /* The fog overlay: one texel per map square, stretched over the whole map
     * and filtered, so the fade out of a live region is smooth at every zoom
     * rather than stepping a square at a time. Bound to a renderer the same
     * way the offscreen above is. The mask is rebuilt only when the live
     * regions move — the tank crossing a square, or a pill's view coming and
     * going — so fogLive/fogLiveCount hold the set it was last built from and
     * fogValid says whether they mean anything yet. */
    SDL_Texture   *fog;
    SDL_Renderer  *fogRenderer;
    OverviewRect   fogLive[OVERVIEW_MAX_REGIONS];
    int            fogLiveCount;
    bool           fogValid;
    BYTE           fogMask[OVERVIEW_FOG_MASK_BYTES];

    /* The OS pointer is switched to the game's crosshair while it is over the
     * map, so the view has to remember that it did the switching — nothing
     * else will put the system cursor back. dragWasActive carries the drag
     * one frame further, for the reason in overviewViewHandleInput. */
    bool           crosshairOn;
    bool           dragWasActive;

    /* True while the pointer sits over a map square. The hover keeps the
     * shared build cursor on that square, and the main view draws its mouse
     * square solid — so this view does too, keeping the faint rendering for
     * a target the gamepad flow left behind. */
    bool           mouseOnMap;

    /* When the death blackout came up, and 0 when there is none — the edge the
     * fade is measured from, so one death is one fade however many frames the
     * view draws in it. */
    Uint64         blackoutTick;

    /* This view's tank-label cache — the shared drawer in tank_label.c
     * builds its textures on whichever renderer hosts the view (the classic
     * pass's cache is the main window's and cannot be shared). It flushes
     * itself when the font (reopened on zoom change) or the renderer
     * changes. */
    TankLabelCache labelCache;
};

/* (Re)create the offscreen when the host asks for a size — or a renderer —
 * the current one does not match. Returns false when there is no target to
 * draw into. */
static bool overviewViewEnsureTarget(OverviewView *v, SDL_Renderer *r,
                                     int w, int h) {
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

/* Every square the view covers, all at full brightness — what the player can
 * see this instant and what they are only remembering alike. The fog pass
 * below takes the second kind back down; keeping the two apart is what lets
 * the boundary between them be softer than one square. */
static void overviewViewDrawTerrain(SDL_Renderer *r, SDL_Texture *tiles, int ss,
                                    const OverviewCamera *cam,
                                    int viewW, int viewH,
                                    const OverviewMap *om,
                                    int left, int top, int right, int bottom) {
    /* The in-window overview draws from the main window's tile sheet, which
     * the classic view mods for its own purposes. */
    SDL_SetTextureColorMod(tiles, 255, 255, 255);

    float tilePx = (float)OVERVIEW_TILE_PX * overviewCameraZoomScale(cam);
    SDL_FRect mineSrc = mapViewAtlasSrc(MINE_X, MINE_Y,
                                        TILE_SIZE_X, TILE_SIZE_Y, ss);

    /* x outer, y inner: the memory is [x][y], so the inner walk is
     * contiguous. */
    for (int mx = left; mx <= right; mx++) {
        for (int my = top; my <= bottom; my++) {
            BYTE tile = om->tile[mx][my];
            if (tile == OVERVIEW_UNSEEN) continue;  /* the black clear shows */

            BYTE flags = om->flags[mx][my];
            float sx = 0.0f, sy = 0.0f;
            overviewCameraWorldToScreen(cam, viewW, viewH,
                                        (float)mx, (float)my, &sx, &sy);
            /* Whole pixels. The camera is continuous, so a tile boundary can
             * land on a half-pixel, and neighbouring tiles then either leave
             * a gap that shows the black clear colour or sample a texel from
             * the next atlas cell. Rounding here is exact rather than
             * approximate: tilePx is OVERVIEW_TILE_PX * zoomScale, and every
             * rung of the zoom ladder makes that a whole number, so
             * round(sx + tilePx) == round(sx) + tilePx. Tiles keep their
             * exact size and abut. */
            sx = SDL_roundf(sx);
            sy = SDL_roundf(sy);
            SDL_FRect dest = { sx, sy, tilePx, tilePx };
            SDL_FRect src  = mapViewAtlasSrc(mapViewPosX[tile],
                                             mapViewPosY[tile],
                                             TILE_SIZE_X, TILE_SIZE_Y, ss);
            SDL_RenderTexture(r, tiles, &src, &dest);

            if ((flags & OVERVIEW_F_MINE) != 0) {
                SDL_RenderTexture(r, tiles, &mineSrc, &dest);
            }
        }
    }
}

/* (Re)create the fog texture when the renderer changes. White, so the fog's
 * colour is the colour mod and nothing else: interpolating a constant white
 * leaves the filtered edge free of the fringe a two-coloured texture would
 * bleed into it. Nothing else ever draws this texture, so — unlike the host's
 * tile sheet, which ImGui also submits — the sampler set here survives from
 * frame to frame. */
static bool overviewViewEnsureFog(OverviewView *v, SDL_Renderer *r) {
    if (v->fog && v->fogRenderer == r) return true;

    if (v->fog) {
        SDL_DestroyTexture(v->fog);
        v->fog = NULL;
    }
    v->fogRenderer = NULL;
    v->fogValid = false;

    v->fog = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                               SDL_TEXTUREACCESS_STREAMING,
                               MAP_ARRAY_SIZE, MAP_ARRAY_SIZE);
    if (!v->fog) return false;

    SDL_SetTextureBlendMode(v->fog, SDL_BLENDMODE_BLEND);
    /* The whole point: one texel per square blown up to whole tiles, with the
     * hardware shading between them. */
    SDL_SetTextureScaleMode(v->fog, SDL_SCALEMODE_LINEAR);
    /* Black fog — src is white, so this alone picks the colour a future tint
     * would change. */
    SDL_SetTextureColorMod(v->fog, 0, 0, 0);
    v->fogRenderer = r;
    return true;
}

/* Rebuild the mask from the regions and push it into the texture. RGBA8888 is
 * one Uint32 per texel with red in the top byte, so a white texel carrying the
 * mask as its alpha is 0xFFFFFF00 | mask. */
static void overviewViewUploadFog(OverviewView *v, const OverviewMap *om) {
    void *pixels = NULL;
    int   pitch  = 0;

    overviewFogBuildMask(om->live, om->liveCount, v->fogMask);
    if (!SDL_LockTexture(v->fog, NULL, &pixels, &pitch)) return;

    for (int y = 0; y < MAP_ARRAY_SIZE; y++) {
        Uint32     *row = (Uint32 *)((Uint8 *)pixels + (size_t)y * (size_t)pitch);
        const BYTE *src = v->fogMask + (size_t)y * MAP_ARRAY_SIZE;
        for (int x = 0; x < MAP_ARRAY_SIZE; x++) {
            row[x] = 0xFFFFFF00u | (Uint32)src[x];
        }
    }
    SDL_UnlockTexture(v->fog);
}

/* The fog over the terrain the pass above just drew.
 *
 * The texture covers the whole map, one texel to a square, so it goes down as
 * a single blit of the map's own rect: texel i then spans exactly square i and
 * its centre lands on the square's centre, which is what makes the filtering
 * shade between square centres instead of smearing the mask off by half a
 * tile. The rect's origin is rounded the way the terrain's is, and its size is
 * a whole number of tiles, so the two stay registered at every zoom.
 *
 * The mask comes from the live regions rather than the per-square LIVE flag —
 * the sim writes the flag from those same rects, so they say the same thing,
 * and the rects are at most OVERVIEW_MAX_REGIONS structs to compare where the
 * flags are 64K of bytes.
 *
 * One consequence of filtering: the fade starts at the last live square's
 * centre, not its outer edge, so the fully-clear area gives up half a square
 * at the boundary. It never gains any, which is the direction that matters —
 * nothing outside a live region is ever drawn at full brightness. */
static void overviewViewDrawFog(OverviewView *v, SDL_Renderer *r,
                                const OverviewCamera *cam, int viewW, int viewH,
                                const OverviewMap *om) {
    if (!overviewViewEnsureFog(v, r)) return;

    if (!v->fogValid || v->fogLiveCount != om->liveCount ||
        SDL_memcmp(v->fogLive, om->live,
                   sizeof(OverviewRect) * (size_t)om->liveCount) != 0) {
        overviewViewUploadFog(v, om);
        SDL_memcpy(v->fogLive, om->live, sizeof(v->fogLive));
        v->fogLiveCount = om->liveCount;
        v->fogValid = true;
    }

    float tilePx = (float)OVERVIEW_TILE_PX * overviewCameraZoomScale(cam);
    float sx = 0.0f, sy = 0.0f;
    overviewCameraWorldToScreen(cam, viewW, viewH, 0.0f, 0.0f, &sx, &sy);

    SDL_FRect dst = { SDL_roundf(sx), SDL_roundf(sy),
                      tilePx * (float)MAP_ARRAY_SIZE,
                      tilePx * (float)MAP_ARRAY_SIZE };
    SDL_RenderTexture(r, v->fog, NULL, &dst);
}

/* Black over the finished frame, for as long as the sim says the death is in
 * its last couple of seconds. It fades up over OVERVIEW_DEATH_BLACK_FADE_MS
 * and then holds, so the window darkens rather than cutting; the respawn ends
 * it outright, which is the cut back to the map the classic view makes too. */
static void overviewViewDrawDeathBlackout(OverviewView *v, SDL_Renderer *r,
                                          int viewW, int viewH, ClientSim *cs) {
    bool showing = (cs != NULL) && clientSimIsMyTankDeathBlackout(cs);
    Uint64 now = SDL_GetTicks();

    if (!showing) {
        v->blackoutTick = 0;
        return;
    }
    if (v->blackoutTick == 0) {
        v->blackoutTick = now;
    }

    Uint64 elapsed = now - v->blackoutTick;
    Uint8  alpha   = 255;
    if (elapsed < OVERVIEW_DEATH_BLACK_FADE_MS) {
        alpha = (Uint8)(255.0f * (float)elapsed /
                        (float)OVERVIEW_DEATH_BLACK_FADE_MS);
    }

    SDL_FRect dst = { 0.0f, 0.0f, (float)viewW, (float)viewH };
    SDL_BlendMode was = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(r, &was);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, alpha);
    SDL_RenderFillRect(r, &dst);
    SDL_SetRenderDrawBlendMode(r, was);
}

/* Copies the entries the player is allowed to see into a second set of lists.
 * The builders work over the whole map — wider than anything the server culls
 * to — so this is where sight is enforced: an entity is kept only when the
 * square it stands on is live, with the local player's own tank the one
 * exception. Copying into fresh lists rather than editing the built ones
 * leaves the sim's per-frame views untouched, as the renderer is meant to. */
static void overviewViewFilterEntities(const OverviewMap *om, BYTE me,
                                       bool selfAlive,
                                       const screenTanks *allTks,
                                       const screenLgm *allLgms,
                                       const screenBullets *allSb,
                                       screenTanks *outTks, screenLgm *outLgms,
                                       screenBullets *outSb) {
    BYTE mx, my, px, py, frame, playerNum;
    char name[PLAYER_NAME_LEN];
    BYTE count;
    BYTE total;
    int bulletTotal;
    int bullet;

    total = screenTanksGetNumEntries(allTks);
    for (count = 1; count <= total; count++) {
        screenTanksGetItem(allTks, count, &mx, &my, &px, &py, &frame,
                           &playerNum, name);
        bool isSelf = (playerNum == me);
        /* A tank waiting to respawn reads as sitting on the map origin, which
         * is nowhere it is — the exception is for a tank that is actually on
         * the field. */
        if (isSelf && !selfAlive) continue;
        if (!overviewEntityIsVisible(om, mx, my, isSelf)) continue;
        screenTanksAddItem(outTks, mx, my, px, py, frame, playerNum, name);
    }

    total = screenLgmGetNumEntries(allLgms);
    for (count = 1; count <= total; count++) {
        screenLgmGetItem(allLgms, count, &mx, &my, &px, &py, &frame);
        if (!overviewEntityIsVisible(om, mx, my, false)) continue;
        screenLgmAddItem(outLgms, mx, my, px, py, frame);
    }

    /* Shells, shell explosions and tank explosions share one list and one
     * position per entry, so the square each stands on is all the filter
     * needs; what the frame draws as never enters into it. */
    bulletTotal = screenBulletsGetNumEntries(allSb);
    for (bullet = 1; bullet <= bulletTotal; bullet++) {
        screenBulletsGetItem(allSb, bullet, &mx, &my, &px, &py, &frame);
        if (!overviewEntityIsVisible(om, mx, my, false)) continue;
        screenBulletsAddItem(outSb, mx, my, px, py, frame);
    }
}

/* Tank names beside the sprites, drawn by the same tank_label.c body the
 * classic view uses — same font, same colours, same flag / brain icon —
 * through this view's own cache, because the classic pass's textures live
 * on the main window's renderer and this view may be on the pop-out's. The
 * label string is passed through whole, so the Tank Labels menu setting
 * (none / short / long, and with it the icon) applies here too. */
static void overviewViewDrawLabels(OverviewView *v, SDL_Renderer *r,
                                   const OverviewCamera *cam,
                                   int viewW, int viewH,
                                   const screenTanks *tks) {
    BYTE total = screenTanksGetNumEntries(tks);
    BYTE count;

    TTF_Font *font = sdl3DrawGetMessageFont();

    /* The face is opened at 13 px times the main window's zoom; the blit is
     * scaled so the on-screen height is 13 px times the overview zoom. */
    int mainZoom = sdl3DrawGetZoomFactor();
    if (mainZoom < 1) mainZoom = 1;
    float ds = overviewCameraZoomScale(cam) / (float)mainZoom;

    for (count = 1; count <= total; count++) {
        BYTE mx, my, px, py, frame, playerNum;
        char name[PLAYER_NAME_LEN];
        float sx = 0.0f, sy = 0.0f;

        screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &playerNum,
                           name);
        if (name[0] == '\0') continue;

        /* One square to the right of the tank, as the main view places it. */
        overviewCameraWorldToScreen(cam, viewW, viewH,
                                    (float)mx + (float)px / (float)TILE_SIZE_X + 1.0f,
                                    (float)my + (float)py / (float)TILE_SIZE_Y,
                                    &sx, &sy);
        if (sx > (float)viewW || sy > (float)viewH || sy < 0.0f) continue;
        if (sx < 0.0f) sx = 0.0f;

        tankLabelDraw(&v->labelCache, r, font, name, playerNum, sx, sy, ds);
    }
}

/* The sprite overlay: everything that moves, on the squares the player can
 * see this instant. Runs on the offscreen the terrain passes just filled. */
static void overviewViewDrawEntities(OverviewView *v,
                                     SDL_Renderer *r, SDL_Texture *tiles, int ss,
                                     SDL_Texture *crosshair,
                                     const OverviewCamera *cam,
                                     int viewW, int viewH,
                                     const OverviewMap *om, ClientSim *cs) {
    screenTanks   allTks;
    screenTanks   tks;
    screenLgm     allLgms;
    screenLgm     lgms;
    screenBullets allSb;
    screenBullets sb;
    MapViewCtx    ctx;

    screenTanksCreate(&allTks);
    screenTanksCreate(&tks);
    screenLgmCreate(&allLgms);
    screenLgmCreate(&lgms);
    allSb = screenBulletsCreate();
    sb    = screenBulletsCreate();

    clientSimPrepareOverviewEntities(cs, &allTks, &allLgms, &allSb);
    overviewViewFilterEntities(om, clientSimGetMyPlayerNum(cs),
                               clientSimIsMyTankAlive(cs), &allTks, &allLgms,
                               &allSb, &tks, &lgms, &sb);

    /* mapview.c positions a sprite at originX - tileW + bbx * zoomFactor -
     * edgeX, where bbx is the entity's game-pixel offset from the rect's
     * origin. The lists were built from a rect starting at 0,0, so bbx is the
     * offset from map square 0,0 and the whole transform reduces to placing
     * that square: hand it the camera's answer for 0,0, converted into the
     * sub-pixel steps the factor works in, and every sprite lands where the
     * camera would have put it. */
    float zoomScale = overviewCameraZoomScale(cam);
    float perStep   = zoomScale / (float)OVERVIEW_ENTITY_SUBPX;
    float o0x = 0.0f, o0y = 0.0f;
    overviewCameraWorldToScreen(cam, viewW, viewH, 0.0f, 0.0f, &o0x, &o0y);

    int tileW = TILE_SIZE_X * OVERVIEW_ENTITY_SUBPX;
    int tileH = TILE_SIZE_Y * OVERVIEW_ENTITY_SUBPX;
    int originX = tileW + (int)SDL_lroundf(o0x / perStep);
    int originY = tileH + (int)SDL_lroundf(o0y / perStep);

    ctx.renderer   = r;
    ctx.tilesTex   = tiles;
    ctx.zoomFactor = OVERVIEW_ENTITY_SUBPX;
    ctx.sheetScale = ss;

    float wasScaleX = 1.0f, wasScaleY = 1.0f;
    SDL_GetRenderScale(r, &wasScaleX, &wasScaleY);
    SDL_SetRenderScale(r, perStep, perStep);

    /* Where a build will land, from the same mouse_square sprite the main view
     * draws. Solid while cursor mode is on or the pointer is over the map (the
     * main view draws its mouse square solid); a target that is only locked in
     * shows faint, which is the split the main view makes too. Ahead of the
     * sprite passes so tanks and men stand on top of it. */
    BYTE bcX = 0, bcY = 0;
    bool cursorMode  = buildCursorGetTile(&bcX, &bcY);
    bool cursorSolid = cursorMode || v->mouseOnMap;
    if (cursorMode || buildCursorGetTargetTile(&bcX, &bcY)) {
        int bbx = (int)bcX * TILE_SIZE_X;
        int bby = (int)bcY * TILE_SIZE_Y;
        SDL_FRect src = mapViewAtlasSrc(MOUSE_SQUARE_X, MOUSE_SQUARE_Y,
                                        TILE_SIZE_X, TILE_SIZE_Y, ss);
        SDL_FRect dst = {
            (float)(originX - tileW + bbx * OVERVIEW_ENTITY_SUBPX),
            (float)(originY - tileH + bby * OVERVIEW_ENTITY_SUBPX),
            (float)(TILE_SIZE_X * OVERVIEW_ENTITY_SUBPX),
            (float)(TILE_SIZE_Y * OVERVIEW_ENTITY_SUBPX)
        };
        if (!cursorSolid) SDL_SetTextureAlphaMod(tiles, 128);
        SDL_RenderTexture(r, tiles, &src, &dst);
        /* The sprite passes below draw from this texture, and the terrain
         * passes will next frame — neither wants a leftover alpha. */
        SDL_SetTextureAlphaMod(tiles, 255);
    }

    mapViewDrawShells(&ctx, &sb, originX, originY, tileW, tileH, 0, 0);
    mapViewDrawTanks(&ctx, &tks, originX, originY, tileW, tileH, 0, 0);
    mapViewDrawLGMs(&ctx, &lgms, originX, originY, tileW, tileH, 0, 0);

    /* The local player's own reticle, last so it sits on top of the sprites
     * the way the main view's does. Its top-left goes where a 16x16 tile
     * sprite would — mapViewDrawTanks' formula with bbx built from the
     * gunsight's square and pixel offset — which is what puts the sprite's
     * centre pixel on the aim point. Inside the render scale, so its 17 game
     * pixels track the map at every zoom exactly as a tank's 16 do. */
    BYTE gsMX, gsMY, gsPX, gsPY;
    if (crosshair != NULL &&
        clientSimGetGunsightPos(cs, &gsMX, &gsMY, &gsPX, &gsPY)) {
        /* The ImGui SDL3 backend sets the sampler per draw, so the mode the
         * host set at load time does not survive to here. */
        SDL_SetTextureScaleMode(crosshair, SDL_SCALEMODE_NEAREST);
        int bbx = (int)gsMX * TILE_SIZE_X + (int)gsPX;
        int bby = (int)gsMY * TILE_SIZE_Y + (int)gsPY;
        SDL_FRect dst = {
            (float)(originX - tileW + bbx * OVERVIEW_ENTITY_SUBPX),
            (float)(originY - tileH + bby * OVERVIEW_ENTITY_SUBPX),
            (float)(OVERVIEW_CROSSHAIR_PX * OVERVIEW_ENTITY_SUBPX),
            (float)(OVERVIEW_CROSSHAIR_PX * OVERVIEW_ENTITY_SUBPX)
        };
        SDL_RenderTexture(r, crosshair, NULL, &dst);
    }
    SDL_SetRenderScale(r, wasScaleX, wasScaleY);

    if (zoomScale >= OVERVIEW_LABEL_MIN_ZOOM) {
        overviewViewDrawLabels(v, r, cam, viewW, viewH, &tks);
    }

    screenBulletsDestroy(&sb);
    screenBulletsDestroy(&allSb);
    screenLgmDestroy(&lgms);
    screenLgmDestroy(&allLgms);
    screenTanksDestroy(&tks);
    screenTanksDestroy(&allTks);
}

extern "C" OverviewView *overviewViewCreate(void) {
    OverviewView *v = (OverviewView *)SDL_calloc(1, sizeof(*v));
    if (!v) return NULL;
    /* Idempotent, and the only thing that fills mapViewPosX/Y — calling it
     * here means the view does not depend on the main window having drawn
     * a frame first. */
    mapViewInit();
    overviewCameraInit(&v->cam);
    return v;
}

extern "C" void overviewViewDestroy(OverviewView *v) {
    if (!v) return;
    tankLabelCacheFlush(&v->labelCache);
    if (v->fog) {
        SDL_DestroyTexture(v->fog);
        v->fog = NULL;
    }
    if (v->target) {
        SDL_DestroyTexture(v->target);
        v->target = NULL;
    }
    SDL_free(v);
}

extern "C" SDL_Texture *overviewViewGetTexture(const OverviewView *v) {
    return v ? v->target : NULL;
}

extern "C" void overviewViewGetSize(const OverviewView *v, int *outW, int *outH) {
    if (outW) *outW = v ? v->targetW : 0;
    if (outH) *outH = v ? v->targetH : 0;
}

extern "C" OverviewCamera *overviewViewCamera(OverviewView *v) {
    return v ? &v->cam : NULL;
}

extern "C" void overviewViewRenderOffscreen(OverviewView *v, SDL_Renderer *r,
                                            SDL_Texture *tiles, int sheetScale,
                                            SDL_Texture *crosshair,
                                            int w, int h, ClientSim *cs,
                                            bool ownsWindow) {
    if (!v || !r || w <= 0 || h <= 0) return;
    if (!overviewViewEnsureTarget(v, r, w, h)) return;

    SDL_SetRenderTarget(r, v->target);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    const OverviewMap *om = clientSimGetOverviewMap(cs);
    if (om != NULL && tiles != NULL) {
        if (sheetScale < 1) sheetScale = 1;
        /* The ImGui SDL3 backend sets the sampler per draw, so the host's
         * one-off NEAREST at build time does not survive to here. */
        SDL_SetTextureScaleMode(tiles, SDL_SCALEMODE_NEAREST);

        /* A tank waiting to respawn has a position but is not anywhere the
         * player is, so follow mode holds the centre it already had. The
         * sub-square read is what lets follow glide with the tank rather
         * than stepping a whole square at a time; with Smooth Scrolling off
         * the position is snapped back to its square's centre, so follow
         * steps the way the classic view's scroll does. */
        float tankX = 0.0f, tankY = 0.0f;
        if (clientSimIsMyTankAlive(cs) &&
            clientSimGetMyTankMapPosF(cs, &tankX, &tankY)) {
            if (!smoothScrollingEnabled) {
                tankX = SDL_floorf(tankX) + 0.5f;
                tankY = SDL_floorf(tankY) + 0.5f;
            }
            overviewCameraFollowTick(&v->cam, w, h, tankX, tankY);
            /* With follow off the camera stays where it was put, and the tank
             * can drive out of the view — or respawn outside it. In the mode
             * where this is the only picture the player has, it gets nudged
             * back on rather than lost. A no-op under follow, which has just
             * centred on it. */
            if (ownsWindow) {
                overviewCameraKeepTankOnScreen(&v->cam, w, h, tankX, tankY);
            }
        }

        int left = 0, top = 0, right = 0, bottom = 0;
        if (overviewCameraVisibleRange(&v->cam, w, h,
                                       &left, &top, &right, &bottom)) {
            overviewViewDrawTerrain(r, tiles, sheetScale, &v->cam, w, h, om,
                                    left, top, right, bottom);
            overviewViewDrawFog(v, r, &v->cam, w, h, om);
        }

        /* Sprites on top of the fog: a tank only stands on a live square, and
         * the build cursor and the gunsight are the player's own marks, so
         * neither wants dimming. */
        overviewViewDrawEntities(v, r, tiles, sheetScale, crosshair, &v->cam,
                                 w, h, om, cs);
    }

    /* Over the lot, and outside the test above: a death is worth answering
     * even on a frame with no map to draw. The host's own chrome — the status
     * panels in full screen, the window's furniture in the pop-out — goes on
     * afterwards and stays clear of it, so the player can still read what
     * they died with. */
    overviewViewDrawDeathBlackout(v, r, w, h, cs);

    SDL_SetRenderTarget(r, NULL);
}

/* The overview's own keys as SDL scancodes — ImGui numbers its keys its own
 * way, and the bindings are scancodes. 0 for any key the overview does not
 * read, which keyIsClaimedByGame never claims. */
static int overviewScancodeForKey(ImGuiKey key) {
    switch (key) {
        case ImGuiKey_Equal:          return SDL_SCANCODE_EQUALS;
        case ImGuiKey_KeypadAdd:      return SDL_SCANCODE_KP_PLUS;
        case ImGuiKey_Minus:          return SDL_SCANCODE_MINUS;
        case ImGuiKey_KeypadSubtract: return SDL_SCANCODE_KP_MINUS;
        case ImGuiKey_LeftArrow:      return SDL_SCANCODE_LEFT;
        case ImGuiKey_RightArrow:     return SDL_SCANCODE_RIGHT;
        case ImGuiKey_UpArrow:        return SDL_SCANCODE_UP;
        case ImGuiKey_DownArrow:      return SDL_SCANCODE_DOWN;
        case ImGuiKey_Home:           return SDL_SCANCODE_HOME;
        case ImGuiKey_C:              return SDL_SCANCODE_C;
        default:                      return 0;
    }
}

/* A key the player has bound to an in-game action belongs to the game: the
 * overview reads it as unpressed and does without it, rather than panning or
 * zooming while the same press also drives the tank. */
static bool overviewKeyPressed(const keyItems *keys, ImGuiKey key) {
    if (keyIsClaimedByGame(keys, overviewScancodeForKey(key))) return false;
    return ImGui::IsKeyPressed(key);
}

static bool overviewKeyDown(const keyItems *keys, ImGuiKey key) {
    if (keyIsClaimedByGame(keys, overviewScancodeForKey(key))) return false;
    return ImGui::IsKeyDown(key);
}

/* The modifier mask a scancode is one half of, or 0 for an ordinary key. Both
 * halves map to the pair: nobody rebinds to reach for one particular Ctrl, so
 * a binding on the left one answers to the right one too. */
static SDL_Keymod overviewModMaskFor(int scancode) {
    switch (scancode) {
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:  return SDL_KMOD_CTRL;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT: return SDL_KMOD_SHIFT;
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:   return SDL_KMOD_ALT;
        case SDL_SCANCODE_LGUI:
        case SDL_SCANCODE_RGUI:   return SDL_KMOD_GUI;
        default:                  return SDL_KMOD_NONE;
    }
}

/* Does the wheel zoom this instant, or move the gunsight? The zoom key is the
 * view's own binding rather than one of the keys it borrows, so it is read
 * straight off the keyboard: keyIsClaimedByGame counts it — which is what
 * stops the overview reaching for the same key to pan or centre — and
 * overviewKeyDown would therefore always read it as up.
 *
 * A modifier is read through the mod state, which SDL keeps from key events,
 * rather than the keyboard array: input.c's mine key is on Shift for the same
 * reason — the array reports a held modifier on some platforms long after it
 * was let go, and here that would leave the wheel stuck on zoom with the
 * gunsight unreachable.
 *
 * Unbound means the wheel zooms the way it did before there was a key to hold,
 * so clearing the row in Key Setup is the way back to that. */
static bool overviewWheelZooms(const keyItems *keys) {
    int sc = keys ? keys->kiOverviewZoom : 0;
    if (sc <= 0) return true;
    if (sc >= SDL_SCANCODE_COUNT) return false;

    SDL_Keymod mask = overviewModMaskFor(sc);
    if (mask != SDL_KMOD_NONE) return (SDL_GetModState() & mask) != 0;

    const bool *state = SDL_GetKeyboardState(NULL);
    return state && state[sc];
}

/* An in-game binding held, read straight off the keyboard for the same reason
 * overviewWheelZooms is: keyIsClaimedByGame counts every keyItems field, so a
 * binding the view is deliberately taking over reads as up through
 * overviewKeyDown. Unbound is never held. */
static bool overviewBindingDown(int scancode) {
    if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return false;
    const bool *state = SDL_GetKeyboardState(NULL);
    return state && state[scancode];
}

/* The map square under the mouse pointer, taken against the item the caller
 * submitted immediately before this — the same rect the wheel zoom anchors
 * to. False when the pointer is off the map; the square is still written, so
 * callers have to test the return rather than the values. */
static bool overviewSquareUnderMouse(const OverviewCamera *cam,
                                     int viewW, int viewH,
                                     int *outMapX, int *outMapY) {
    ImVec2 rectMin = ImGui::GetItemRectMin();
    ImVec2 mouse   = ImGui::GetMousePos();
    return overviewCameraScreenToWorld(cam, viewW, viewH,
                                       mouse.x - rectMin.x, mouse.y - rectMin.y,
                                       outMapX, outMapY);
}

extern "C" void overviewViewHandleInput(OverviewView *v, bool hovered,
                                        int viewW, int viewH, ClientSim *cs,
                                        const keyItems *keys, bool ownsWindow) {
    if (!v || viewW <= 0 || viewH <= 0) return;

    ImGuiIO &io = ImGui::GetIO();
    OverviewCamera *cam = &v->cam;

    /* The pan InvisibleButton the caller submitted immediately before this
     * owns the press, so a drag that wanders off the image keeps panning. */
    if (ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        /* Only a press that actually moved pans — a plain click would
         * otherwise pan by nothing and switch follow off as a side effect.
         * Negated: dragging right pulls the map right, which is the centre
         * moving left. */
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
            overviewCameraPan(cam, viewW, viewH,
                              -io.MouseDelta.x, -io.MouseDelta.y);
        }
    }

    /* The game's crosshair while the pointer is over the map, the system
     * cursor back when it leaves. The ImGui SDL3 backend only calls
     * SDL_SetCursor when its own expected cursor changes, so one set here
     * holds frame after frame — but the drag above asks for ResizeAll, and
     * ImGui going back to Arrow once the drag ends is such a change, applied
     * at the start of the frame after. Setting the crosshair before that
     * would be wiped, so the drag suppresses it for one frame more and the
     * set lands the frame after, when the backend has already been through.
     * The restore is this view's own job: cursorMove only tracks the main
     * window's pointer and will not undo a crosshair set from here. */
    bool dragging = ImGui::IsItemActive();
    if (dragging || v->dragWasActive) {
        v->crosshairOn = false;
    } else if (hovered) {
        if (!v->crosshairOn) {
            cursorSetCursor(false);
            v->crosshairOn = true;
        }
    } else if (v->crosshairOn) {
        cursorSetCursor(true);
        v->crosshairOn = false;
    }
    v->dragWasActive = dragging;

    /* The wheel moves the gunsight over the map exactly as it does over the
     * main view, and zooms only while the zoom key is held. The bump is
     * dispatched from here rather than left to the wheel handler in
     * sdl3imgui.cpp, which neither host's wheel reaches: the in-window mode
     * sits under an ImGui window that captures the mouse, and the pop-out's
     * events are consumed as that window's own. Same running-game test the
     * handler there makes. */
    if (hovered && io.MouseWheel != 0.0f) {
        int steps = (io.MouseWheel > 0.0f) ? 1 : -1;
        if (overviewWheelZooms(keys)) {
            ImVec2 rectMin = ImGui::GetItemRectMin();
            ImVec2 mouse   = ImGui::GetMousePos();
            overviewCameraZoomAt(cam, viewW, viewH,
                                 mouse.x - rectMin.x, mouse.y - rectMin.y, steps);
        } else if (cs && clientSimGetNetStatus(cs) == netRunning) {
            inputBumpGunsight(steps);
        }
    }

    /* Moving the pointer over the map drives the one shared build cursor, the
     * way moving it over the main view does, so toggling build mode picks up
     * where the pointer is. Only on real movement, and only while the pointer
     * is over a square that exists — past the map edge it stays where it was
     * rather than jumping to a clamped square.
     *
     * While cursor mode is ON, buildCursorClampToView drags the cursor back
     * inside the main view's 15x15 each frame, so a square set from here
     * outside that area does not survive to the next one. Cursor mode is the
     * keyboard/gamepad affordance; with a mouse it is off and the clamp is a
     * no-op. The click below dispatches from the pointer's square rather than
     * from the cursor, so building is unaffected either way. */
    if (hovered && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        int mx = 0, my = 0;
        if (overviewSquareUnderMouse(cam, viewW, viewH, &mx, &my)) {
            buildCursorSetTile((BYTE)mx, (BYTE)my);
        }
    }

    /* Tested every frame, not just on movement: the flag has to drop the
     * instant the pointer leaves the map so the square fades back to the
     * locked-target rendering. */
    {
        int mx = 0, my = 0;
        v->mouseOnMap = hovered &&
                        overviewSquareUnderMouse(cam, viewW, viewH, &mx, &my);
    }

    /* Left-click builds at the square under the pointer. The pan item claims
     * the right button, so the left one arrives here unswallowed. Range,
     * terrain and cost are the server's to refuse, exactly as for a build
     * dispatched from the main view; clientSimManMoveToMap already drops the
     * request for a dead tank or a failed connection. */
    if (hovered && cs && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int mx = 0, my = 0;
        if (overviewSquareUnderMouse(cam, viewW, viewH, &mx, &my)) {
            clientMutexWaitFor();
            clientSimManMoveToMap(cs, (BYTE)mx, (BYTE)my,
                                  clientSimGetCurrentBuildSelect(cs));
            clientMutexRelease();
            /* Leave the shared cursor on the square just built, as pointer
             * motion over the main view would have. */
            buildCursorSetTile((BYTE)mx, (BYTE)my);
        }
    }

    /* Keyboard zoom has no cursor to hold onto, so it anchors the centre. */
    float midX = (float)viewW * 0.5f;
    float midY = (float)viewH * 0.5f;
    if (overviewKeyPressed(keys, ImGuiKey_Equal) ||
        overviewKeyPressed(keys, ImGuiKey_KeypadAdd)) {
        overviewCameraZoomAt(cam, viewW, viewH, midX, midY, 1);
    }
    if (overviewKeyPressed(keys, ImGuiKey_Minus) ||
        overviewKeyPressed(keys, ImGuiKey_KeypadSubtract)) {
        overviewCameraZoomAt(cam, viewW, viewH, midX, midY, -1);
    }

    /* The keyboard's pan. The arrows are the view's own and are given up to
     * whatever in-game binding has taken them; the scroll keys are added while
     * this view owns the window, because the classic view they would otherwise
     * scroll is not on screen — and on the default bindings the scroll keys
     * *are* the arrows, so without this there is no keyboard pan at all. The
     * two sets never both report the same key: a bound one is claimed and
     * reads as up through overviewKeyDown, and only a bound one is read by
     * overviewBindingDown.
     *
     * Not while an item view has them: there the scroll keys step between
     * pills, bases or allied tanks, which is still their job with the classic
     * view hidden. Not while a text box has the keyboard either, or typing a
     * message would pan the map behind it.
     *
     * Panning clears follow, the way a drag does, so a held key wins over the
     * tank exactly as manual scrolling wins over auto-scroll in the classic
     * view. Home / C hands the map back to the tank. */
    if (!io.WantTextInput) {
        bool scrollKeysArePan = ownsWindow && cs && !clientSimIsInItemView(cs);
        float dx = 0.0f, dy = 0.0f;
        if (overviewKeyDown(keys, ImGuiKey_LeftArrow))  dx -= OVERVIEW_ARROW_STEP_PX;
        if (overviewKeyDown(keys, ImGuiKey_RightArrow)) dx += OVERVIEW_ARROW_STEP_PX;
        if (overviewKeyDown(keys, ImGuiKey_UpArrow))    dy -= OVERVIEW_ARROW_STEP_PX;
        if (overviewKeyDown(keys, ImGuiKey_DownArrow))  dy += OVERVIEW_ARROW_STEP_PX;
        if (scrollKeysArePan && keys) {
            if (overviewBindingDown(keys->kiScrollLeft))  dx -= OVERVIEW_ARROW_STEP_PX;
            if (overviewBindingDown(keys->kiScrollRight)) dx += OVERVIEW_ARROW_STEP_PX;
            if (overviewBindingDown(keys->kiScrollUp))    dy -= OVERVIEW_ARROW_STEP_PX;
            if (overviewBindingDown(keys->kiScrollDown))  dy += OVERVIEW_ARROW_STEP_PX;
        }
        if (dx != 0.0f || dy != 0.0f) {
            overviewCameraPan(cam, viewW, viewH, dx, dy);
        }
    }

    /* Nothing to centre on while the tank is dead, so the key does nothing
     * rather than throwing the view at wherever the corpse reads. */
    if (overviewKeyPressed(keys, ImGuiKey_Home) ||
        overviewKeyPressed(keys, ImGuiKey_C)) {
        float tankX = 0.0f, tankY = 0.0f;
        if (clientSimIsMyTankAlive(cs) &&
            clientSimGetMyTankMapPosF(cs, &tankX, &tankY)) {
            overviewCameraCenterOnTank(cam, viewW, viewH, tankX, tankY);
        }
    }
}

/* The hover test in overviewViewHandleInput restores the pointer when it
 * leaves the map, but hiding the window with the pointer still over it stops
 * that function running at all, and the crosshair would be left set over the
 * rest of the UI until the main view's next in/out transition. */
extern "C" void overviewViewReleaseCursor(OverviewView *v) {
    if (v && v->crosshairOn) {
        cursorSetCursor(true);
        v->crosshairOn = false;
    }
}
